// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "json_mappers.h"
#include "text/mrkdwn_parser.h"
#include "util/slack_links.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>

using namespace Qt::StringLiterals;

namespace slack {
namespace JsonMappers {

User toUser(const QJsonObject &o) {
    auto       profile     = o.value(u"profile"_s).toObject();
    // display_name / real_name are often "" (empty string, not null) → must check after toString()
    // Prefer real_name: enterprise workspaces often auto-provision display_name from
    // AD/LDAP as a username slug (e.g. "john.doe.dept") while real_name holds the
    // human-readable full name ("John Doe").
    const auto rn          = profile.value(u"real_name"_s).toString().trimmed();
    const auto dn          = profile.value(u"display_name"_s).toString().trimmed();
    const auto displayName = !rn.isEmpty()   ? rn
                             : !dn.isEmpty() ? dn
                                             : o.value(u"name"_s).toString();
    // Strip enclosing colons: ":palm_tree:" → "palm_tree"
    // Also strip skin-tone modifier suffix: ":baby::skin-tone-3:" → "baby"
    auto       rawEmoji    = profile.value(u"status_emoji"_s).toString();
    if (rawEmoji.startsWith(':'))
        rawEmoji = rawEmoji.mid(1);
    if (rawEmoji.endsWith(':'))
        rawEmoji.chop(1);
    const int skinToneSep = rawEmoji.indexOf(u"::"_s);
    if (skinToneSep != -1)
        rawEmoji = rawEmoji.left(skinToneSep);
    return User{
        .id            = UserId{o.value(u"id"_s).toString()},
        .name          = o.value(u"name"_s).toString(),
        .displayName   = displayName,
        .avatarUrl     = profile.value(u"image_72"_s).toString(),
        .isBot         = o.value(u"is_bot"_s).toBool(),
        .isExternal    = o.value(u"is_stranger"_s).toBool(),
        .isStranger    = o.value(u"is_stranger"_s).toBool(),
        .teamId        = o.value(u"team_id"_s).toString(),
        .isActive      = false, // filled by presence poll
        .isDeactivated = o.value(u"deleted"_s).toBool(),
        .isAdmin       = o.value(u"is_admin"_s).toBool() || o.value(u"is_owner"_s).toBool(),
        .isOwner       = o.value(u"is_owner"_s).toBool() || o.value(u"is_primary_owner"_s).toBool(),
        .statusEmoji   = rawEmoji,
        .statusText    = profile.value(u"status_text"_s).toString(),
        .title         = profile.value(u"title"_s).toString(),
        .email         = profile.value(u"email"_s).toString(),
        .hasTz         = o.contains(u"tz_offset"_s),
        .tzOffset      = o.value(u"tz_offset"_s).toInt(),
    };
}

Conversation toConversation(const QJsonObject &o) {
    const auto typeStr = o.value(u"is_im"_s).toBool()        ? "im"
                         : o.value(u"is_mpim"_s).toBool()    ? "mpim"
                         : o.value(u"is_private"_s).toBool() ? "private_channel"
                                                             : "public_channel";

    ConvKind kind = ConvKind::PublicChannel;
    if (typeStr == "im")
        kind = ConvKind::Im;
    else if (typeStr == "mpim")
        kind = ConvKind::Mpim;
    else if (typeStr == "private_channel")
        kind = ConvKind::PrivateChannel;

    std::optional<UserId> dmUser;
    if (kind == ConvKind::Im)
        dmUser = UserId{o.value(u"user"_s).toString()};

    std::vector<UserId> members;
    if (kind == ConvKind::Mpim) {
        for (const auto &v : o.value(u"members"_s).toArray())
            members.push_back(UserId{v.toString()});
    }

    const QString     notifPref  = o.value(u"notification_preference"_s).toString();
    NotificationLevel notifLevel = NotificationLevel::Default;
    if (notifPref == u"everything"_s)
        notifLevel = NotificationLevel::All;
    else if (notifPref == u"nothing"_s)
        notifLevel = NotificationLevel::Mute;
    else if (notifPref == u"mentions"_s)
        notifLevel = NotificationLevel::Mentions;

    const auto topic   = o.value(u"topic"_s).toObject().value(u"value"_s).toString().trimmed();
    const auto purpose = o.value(u"purpose"_s).toObject().value(u"value"_s).toString().trimmed();

    // Compute timestamps locally so we can use both in the unread fallback below.
    const QString lastRead = o.value(u"last_read"_s).toString();
    const QString latestTs = [&]() -> QString {
        const auto v = o.value(u"latest"_s);
        if (v.isObject())
            return v.toObject().value(u"ts"_s).toString();
        if (v.isString())
            return v.toString(); // DMs return ts directly
        return {};
    }();

    // unread_count is not always populated by the API (often 0 for public channels).
    // latestTs > lastRead is a reliable fallback: Slack timestamps are zero-padded
    // fixed-width strings so lexicographic comparison is identical to numeric.
    const int rawUnread = o.value(u"unread_count"_s).toInt();
    const int unread    = rawUnread > 0 ? rawUnread
                          : (!latestTs.isEmpty() && !lastRead.isEmpty() && latestTs > lastRead) ? 1
                                                                                                : 0;

    // Channel canvas (conversations.info; conversations.list may omit "properties").
    const auto [canvasFileId, canvasIsEmpty] = channelCanvas(o);

    // Huddle state: a live huddle attaches a `room` object to the channel.
    const auto huddle = readHuddleRoom(o.value(u"room"_s).toObject());

    return Conversation{
        .id   = ConversationId{o.value(u"id"_s).toString()},
        .kind = kind,
        .name = o.value(u"name"_s).toString(
            o.value(u"user"_s).toString()
        ), // Im: use user id as fallback
        .description        = !topic.isEmpty() ? topic : purpose,
        .isMember           = o.value(u"is_member"_s).toBool(true),
        .memberCount        = o.value(u"num_members"_s).toInt(),
        .lastRead           = lastRead,
        .latestTs           = latestTs,
        .unread             = unread,
        .mentionCount       = o.value(u"mention_count"_s).toInt(),
        .dmUser             = dmUser,
        .members            = std::move(members),
        .isMuted            = o.value(u"is_muted"_s).toBool(),
        .isStarred          = o.value(u"is_starred"_s).toBool(),
        .notifLevel         = notifLevel,
        .canvasFileId       = canvasFileId,
        .canvasIsEmpty      = canvasIsEmpty,
        .huddleActive       = huddle.active,
        .huddleLink         = huddle.link,
        .huddleParticipants = huddle.participants,
    };
}

// A huddle's `room` is ended once Slack sets either signal. has_ended is the
// explicit flag; date_end is the end timestamp (0/absent = still live). Slack
// sends date_end as a JSON number in some payloads and a string in others —
// QJsonValue::toDouble() silently yields 0 for a string, so a string date_end
// would otherwise read as "still live" and strand the huddle banner forever
// (the exact failure where an end-edit arrives but the green bar never clears).
static bool roomHasEnded(const QJsonObject &room) {
    if (room.value(u"has_ended"_s).toBool(false))
        return true;
    const auto   dateEnd = room.value(u"date_end"_s);
    // String form (e.g. "1718...") parses via QString; number form via toDouble
    // (a unix-second ts overflows a 32-bit int, so read as double either way).
    const double end     = dateEnd.isString() ? dateEnd.toString().toDouble() : dateEnd.toDouble();
    return end != 0.0;
}

HuddleRoom readHuddleRoom(const QJsonObject &room) {
    HuddleRoom h;
    // A "huddle" (not a third-party Call). Ignore everything else.
    if (room.isEmpty() || room.value(u"call_family"_s).toString() != u"huddle"_s)
        return h;
    // Ongoing while not ended. A freshly-announced "prewarmed" huddle has no
    // participants yet but is live, so we do NOT require a non-empty list.
    h.active = !roomHasEnded(room);
    h.link   = room.value(u"huddle_link"_s).toString();
    for (const auto &v : room.value(u"participants"_s).toArray())
        h.participants.push_back(UserId{v.toString()});
    // Nobody connected yet → show the host so the indicator still has a face.
    if (h.participants.empty()) {
        const QString host = room.value(u"created_by"_s).toString();
        if (!host.isEmpty())
            h.participants.push_back(UserId{host});
    }
    return h;
}

static qint64 roomSeconds(const QJsonValue &v) {
    // Same number-or-string ambiguity as date_end in roomHasEnded.
    return static_cast<qint64>(v.isString() ? v.toString().toDouble() : v.toDouble());
}

std::optional<HuddleInfo> readHuddleSummary(const QJsonObject &room) {
    if (room.isEmpty())
        return std::nullopt;
    HuddleInfo h;
    h.ended         = roomHasEnded(room);
    h.startSec      = roomSeconds(room.value(u"date_start"_s));
    h.endSec        = h.ended ? roomSeconds(room.value(u"date_end"_s)) : 0;
    // Ended: the history is everyone who was in it (the live list is empty by
    // then). Live: who is in it right now.
    const auto list = room.value(h.ended ? u"participant_history"_s : u"participants"_s).toArray();
    for (const auto &v : list)
        if (const QString id = v.toString(); !id.isEmpty())
            h.attendees.push_back(UserId{id});
    return h;
}

std::pair<QString, bool> channelCanvas(const QJsonObject &channel) {
    const auto props  = channel.value(u"properties"_s).toObject();
    const auto canvas = props.value(u"canvas"_s).toObject();
    if (const auto fileId = canvas.value(u"file_id"_s).toString(); !fileId.isEmpty())
        return {fileId, canvas.value(u"is_empty"_s).toBool()};
    // Free-team shape: the channel canvas is a "canvas" tab.
    for (const auto tab : props.value(u"tabs"_s).toArray()) {
        const auto o = tab.toObject();
        if (o.value(u"type"_s).toString() == QLatin1String("canvas")) {
            const auto fileId = o.value(u"data"_s).toObject().value(u"file_id"_s).toString();
            if (!fileId.isEmpty())
                return {fileId, false};
        }
    }
    return {{}, false};
}

QJsonArray toCanvasChanges(const std::vector<CanvasChange> &changes) {
    QJsonArray out;
    for (const auto &c : changes) {
        QJsonObject op;
        switch (c.op) {
        case CanvasChange::Op::InsertAtStart:
            op.insert(u"operation"_s, "insert_at_start");
            break;
        case CanvasChange::Op::InsertAtEnd:
            op.insert(u"operation"_s, "insert_at_end");
            break;
        case CanvasChange::Op::InsertAfter:
            op.insert(u"operation"_s, "insert_after");
            break;
        case CanvasChange::Op::InsertBefore:
            op.insert(u"operation"_s, "insert_before");
            break;
        case CanvasChange::Op::ReplaceSection:
        case CanvasChange::Op::ReplaceAll:
            op.insert(u"operation"_s, "replace");
            break;
        case CanvasChange::Op::DeleteSection:
            op.insert(u"operation"_s, "delete");
            break;
        case CanvasChange::Op::Rename:
            op.insert(u"operation"_s, "rename");
            break;
        }
        if (!c.sectionId.isEmpty())
            op.insert(u"section_id"_s, c.sectionId);
        if (c.op == CanvasChange::Op::Rename)
            op.insert(
                u"title_content"_s,
                QJsonObject{{u"type"_s, u"markdown"_s}, {u"markdown"_s, c.markdown}}
            );
        else if (c.op != CanvasChange::Op::DeleteSection)
            op.insert(
                u"document_content"_s,
                QJsonObject{{u"type"_s, u"markdown"_s}, {u"markdown"_s, c.markdown}}
            );
        out.append(op);
    }
    return out;
}

static std::vector<Reaction> parseReactions(const QJsonArray &arr) {
    std::vector<Reaction> out;
    for (auto v : arr) {
        auto                r = v.toObject();
        std::vector<UserId> users;
        for (auto u : r.value(u"users"_s).toArray())
            users.push_back(UserId{u.toString()});
        out.push_back(
            Reaction{r.value(u"name"_s).toString(), r.value(u"count"_s).toInt(), std::move(users)}
        );
    }
    return out;
}

// Local text builder for rich_text block conversion.
struct Builder {
    QString                 text;
    std::vector<TextEntity> entities;
};

// Convert a rich_text inline element to a fragment of TextWithEntities.
static void richInlineToTWE(const QJsonObject &el, Builder &b) {
    // Build plain text + entity spans from a structured Slack rich_text element.
    const auto type = el.value(u"type"_s).toString();
    if (type == u"text"_s) {
        const auto             style = el.value(u"style"_s).toObject();
        // A text element's emphasis comes from its style object, but Slack's
        // text→rich_text conversion (and some bots, e.g. Outlook Calendar) leave
        // <!date^…>, <url|label> and <@user> tokens unexpanded inside the raw
        // text. resolveTokens() expands those (and :emoji:) without touching *_~`.
        const TextWithEntities run   = MrkdwnParser::resolveTokens(el.value(u"text"_s).toString());
        const int              start = b.text.size();
        b.text += run.text;
        // Style span wraps the whole run; token spans nest inside it (the
        // renderer builds containment from offset/length ordering).
        if (style.value(u"bold"_s).toBool())
            b.entities.push_back({EntityType::Bold, start, (int)run.text.size(), {}});
        else if (style.value(u"italic"_s).toBool())
            b.entities.push_back({EntityType::Italic, start, (int)run.text.size(), {}});
        else if (style.value(u"strike"_s).toBool())
            b.entities.push_back({EntityType::Strike, start, (int)run.text.size(), {}});
        else if (style.value(u"code"_s).toBool())
            b.entities.push_back({EntityType::Code, start, (int)run.text.size(), {}});
        for (auto e : run.entities) {
            e.offset += start;
            b.entities.push_back(e);
        }
    } else if (type == u"user"_s) {
        const auto uid   = el.value(u"user_id"_s).toString();
        const int  start = b.text.size();
        b.text += u"@"_s + uid;
        b.entities.push_back({EntityType::UserMention, start, (int)b.text.size() - start, uid});
    } else if (type == u"channel"_s) {
        const auto cid   = el.value(u"channel_id"_s).toString();
        const int  start = b.text.size();
        b.text += u"#"_s + cid;
        b.entities.push_back({EntityType::ChannelMention, start, (int)b.text.size() - start, cid});
    } else if (type == u"usergroup"_s) {
        // A user-group mention carries only the S… id — no handle, ever. The
        // renderer resolves it through the Session's usergroups; the id is the
        // fallback text when it can't.
        const auto gid   = el.value(u"usergroup_id"_s).toString();
        const int  start = b.text.size();
        b.text += u"@"_s + gid;
        b.entities.push_back(
            {EntityType::UsergroupMention, start, (int)b.text.size() - start, gid}
        );
    } else if (type == u"emoji"_s) {
        const auto name  = el.value(u"name"_s).toString();
        const int  start = b.text.size();
        b.text += u":"_s + name + u":"_s;
        b.entities.push_back({EntityType::Emoji, start, (int)b.text.size() - start, name});
    } else if (type == u"link"_s) {
        const auto url   = el.value(u"url"_s).toString();
        const auto label = el.value(u"text"_s).toString(url);
        const int  start = b.text.size();
        b.text += label;
        b.entities.push_back({EntityType::Link, start, (int)b.text.size() - start, url});
    } else if (type == u"message_mention"_s) {
        // A link to another message, pasted into a rich_text block. Slack sends
        // the permalink AND the parts it points at — including the author, which
        // the URL alone can't give us, so the chip can read "alice in #general"
        // like the official client. `text`/`url` are the permalink; the host only
        // exists there, so parse it out and let the element's own fields win.
        auto ref          = SlackLinks::parseMessageLink(el.value(u"url"_s).toString());
        ref.conv          = el.value(u"channel_id"_s).toString(ref.conv);
        ref.ts            = el.value(u"message_ts"_s).toString(ref.ts);
        ref.author        = el.value(u"author_id"_s).toString();
        // Slack repeats the message's own ts as thread_ts on a thread ROOT; only
        // a genuine reply gets a thread target (same rule as parseMessageLink).
        const auto thread = el.value(u"thread_ts"_s).toString();
        ref.threadTs      = (thread.isEmpty() || thread == ref.ts) ? QString() : thread;
        const int start   = b.text.size();
        b.text += el.value(u"text"_s).toString(el.value(u"url"_s).toString());
        if (ref.isValid()) {
            b.entities.push_back(
                {EntityType::MessageLink,
                 start,
                 (int)b.text.size() - start,
                 SlackLinks::refToToken(ref)}
            );
        }
    } else if (type == u"broadcast"_s) {
        const auto range = el.value(u"range"_s).toString();
        const int  start = b.text.size();
        if (range == u"here"_s) {
            b.text += u"@here"_s;
            b.entities.push_back({EntityType::HereCommand, start, (int)b.text.size() - start, {}});
        } else {
            b.text += u"@"_s + range;
            b.entities.push_back(
                {EntityType::ChannelCommand, start, (int)b.text.size() - start, {}}
            );
        }
    } else {
        // Unknown inline type — emit its text (token-resolved) if present.
        const auto text = el.value(u"text"_s).toString();
        if (!text.isEmpty()) {
            const TextWithEntities run   = MrkdwnParser::resolveTokens(text);
            const int              start = b.text.size();
            b.text += run.text;
            for (auto e : run.entities) {
                e.offset += start;
                b.entities.push_back(e);
            }
        }
    }
}

// Parse a rich_text block's elements array to TextWithEntities.
static TextWithEntities richTextToTWE(const QJsonObject &block) {
    Builder b;
    for (const auto &sectionVal : block.value(u"elements"_s).toArray()) {
        const auto section = sectionVal.toObject();
        const auto stype   = section.value(u"type"_s).toString();
        const auto elems   = section.value(u"elements"_s).toArray();

        if (stype == u"rich_text_preformatted"_s || stype == u"rich_text_quote"_s) {
            const int start = b.text.size();
            // The wrapping entity must land BEFORE the ones its content emits:
            // the renderer derives containment from (offset, length) with ties
            // broken by insertion order, so a quote spanning exactly one child
            // (e.g. a lone pasted message link) would otherwise become the CHILD
            // and be dropped — the quote bar silently disappeared.
            const int at    = (int)b.entities.size();
            for (const auto &ev : elems)
                richInlineToTWE(ev.toObject(), b);
            b.entities.insert(
                b.entities.begin() + at,
                {stype == u"rich_text_quote"_s ? EntityType::Blockquote : EntityType::Pre,
                 start,
                 (int)b.text.size() - start,
                 {}}
            );
        } else if (stype == u"rich_text_list"_s) {
            const auto style   = section.value(u"style"_s).toString(); // "bullet" or "ordered"
            int        itemIdx = section.value(u"offset"_s).toInt();   // "3." starts at offset 2
            for (const auto &ev : elems) {
                // Each list item is a rich_text_section
                const auto item = ev.toObject();
                if (style == u"ordered"_s)
                    b.text += QString::number(++itemIdx) + u". "_s;
                else
                    b.text += u"• "_s;
                for (const auto &ie : item.value(u"elements"_s).toArray())
                    richInlineToTWE(ie.toObject(), b);
                b.text += u"\n"_s;
            }
        } else {
            // rich_text_section (most common) — just parse inline elements
            for (const auto &ev : elems)
                richInlineToTWE(ev.toObject(), b);
            if (!b.text.endsWith('\n'))
                b.text += u"\n"_s;
        }
    }
    // Trim trailing newline
    while (b.text.endsWith('\n'))
        b.text.chop(1);
    return TextWithEntities{b.text, b.entities};
}

File toFile(const QJsonObject &o) {
    File f{
        .id          = o.value(u"id"_s).toString(),
        .name        = o.value(u"name"_s).toString(),
        .mimeType    = o.value(u"mimetype"_s).toString(),
        .prettyType  = o.value(u"pretty_type"_s).toString(),
        .urlPrivate  = o.value(u"url_private"_s).toString(),
        .permalink   = o.value(u"permalink"_s).toString(),
        .thumbUrl    = o.value(u"thumb_360"_s).toString(o.value(u"thumb_480"_s).toString()),
        .imageWidth  = o.value(u"original_w"_s).toInt(o.value(u"thumb_360_w"_s).toInt()),
        .imageHeight = o.value(u"original_h"_s).toInt(o.value(u"thumb_360_h"_s).toInt()),
        .size        = (qint64)o.value(u"size"_s).toDouble(),
    };
    f.urlPrivateDownload = o.value(u"url_private_download"_s).toString();
    f.durationMs         = (qint64)o.value(u"duration_ms"_s).toDouble();
    f.aacUrl             = o.value(u"aac"_s).toString();
    f.subtype            = o.value(u"subtype"_s).toString();
    f.fileType           = o.value(u"filetype"_s).toString();
    if (const QJsonObject tr = o.value(u"transcription"_s).toObject(); !tr.isEmpty()) {
        f.transcriptStatus  = tr.value(u"status"_s).toString();
        f.transcriptPreview = tr.value(u"preview"_s).toObject().value(u"content"_s).toString();
    }
    f.transcriptVttUrl = o.value(u"vtt"_s).toString();
    if (f.isCanvas())
        f.title = MrkdwnParser::decodeEntities(o.value(u"title"_s).toString());
    // Full thumbnail ladder — the UI picks the variant matching the physical
    // (DPR-scaled) preview size, so previews stay crisp on any screen density.
    static constexpr int kThumbSides[] = {64, 80, 160, 360, 480, 720, 800, 960, 1024};
    for (int side : kThumbSides) {
        const QString key = QStringLiteral("thumb_%1").arg(side);
        const QString url = o.value(key).toString();
        if (url.isEmpty())
            continue;
        f.thumbs.push_back(
            FileThumb{
                o.value(key + u"_w"_s).toInt(side),
                o.value(key + u"_h"_s).toInt(),
                url,
            }
        );
    }
    // Animated GIF uploads: thumb_N is a static first frame; the thumb_N_gif
    // variants carry the animation (and share thumb_N's dimensions).
    static constexpr int kAnimThumbSides[] = {360, 480};
    for (int side : kAnimThumbSides) {
        const QString url = o.value(QStringLiteral("thumb_%1_gif").arg(side)).toString();
        if (url.isEmpty())
            continue;
        f.animThumbs.push_back(
            FileThumb{
                o.value(QStringLiteral("thumb_%1_w").arg(side)).toInt(side),
                o.value(QStringLiteral("thumb_%1_h").arg(side)).toInt(),
                url,
            }
        );
    }
    // PDFs: Slack prerenders the first page server-side (thumb_pdf + thumb_pdf_w/h).
    if (f.thumbUrl.isEmpty() && o.contains(u"thumb_pdf"_s)) {
        f.thumbUrl    = o.value(u"thumb_pdf"_s).toString();
        f.imageWidth  = o.value(u"thumb_pdf_w"_s).toInt(f.imageWidth);
        f.imageHeight = o.value(u"thumb_pdf_h"_s).toInt(f.imageHeight);
    }
    return f;
}

// Parse a text-object ({"type":"mrkdwn"|"plain_text","text":"..."}).
static TextWithEntities parseTextObj(const QJsonObject &o) {
    const auto type = o.value(u"type"_s).toString();
    const auto text = o.value(u"text"_s).toString();
    if (type == u"mrkdwn"_s)
        return MrkdwnParser::parse(text);
    // plain_text: no mrkdwn emphasis (*_~` stay literal), but Slack still expands
    // :emoji: shortcodes — e.g. a bot header ":mega: Notification" — unless the
    // object opts out with "emoji": false (default true). resolveTokens does the
    // emoji pass and leaves marks alone; decodeEntities matches the title path.
    if (o.value(u"emoji"_s).toBool(true))
        return MrkdwnParser::resolveTokens(MrkdwnParser::decodeEntities(text));
    return TextWithEntities{MrkdwnParser::decodeEntities(text), {}};
}

// A button element — Block Kit shape ("text" is a plain_text object) or the
// legacy attachment-actions shape ("text" is a plain string).
static BotButton toButton(const QJsonObject &el) {
    const auto textVal = el.value(u"text"_s);
    return BotButton{
        .text     = textVal.isObject() ? textVal.toObject().value(u"text"_s).toString()
                                       : textVal.toString(),
        .url      = el.value(u"url"_s).toString(),
        .style    = el.value(u"style"_s).toString(),
        .actionId = el.value(u"action_id"_s).toString(),
        .value    = el.value(u"value"_s).toString(),
    };
}

Block toBlock(const QJsonObject &o) {
    Block b;
    b.typeStr = o.value(u"type"_s).toString();

    const QString blockId = o.value(u"block_id"_s).toString();

    if (b.typeStr == u"rich_text"_s) {
        b.text = richTextToTWE(o);
    } else if (b.typeStr == u"image"_s) {
        b.imageUrl    = o.value(u"image_url"_s).toString();
        b.altText     = o.value(u"alt_text"_s).toString();
        b.imageWidth  = o.value(u"image_width"_s).toInt();
        b.imageHeight = o.value(u"image_height"_s).toInt();
        if (o.contains(u"title"_s))
            b.text = parseTextObj(o.value(u"title"_s).toObject());
    } else if (b.typeStr == u"header"_s || b.typeStr == u"section"_s) {
        if (o.contains(u"text"_s))
            b.text = parseTextObj(o.value(u"text"_s).toObject());
        // Section blocks may carry a "fields" array instead of (or alongside) "text";
        // append each field as its own line, shifting entity offsets accordingly.
        for (const auto &fv : o.value(u"fields"_s).toArray()) {
            const TextWithEntities ft = parseTextObj(fv.toObject());
            if (ft.text.isEmpty())
                continue;
            if (!b.text.text.isEmpty())
                b.text.text += u"\n"_s;
            const int base = b.text.text.size();
            b.text.text += ft.text;
            for (auto e : ft.entities) {
                e.offset += base;
                b.text.entities.push_back(e);
            }
        }
        // Accessory button (e.g. "View details" next to a section's text).
        const auto acc = o.value(u"accessory"_s).toObject();
        if (acc.value(u"type"_s).toString() == u"button"_s) {
            b.buttons.push_back(toButton(acc));
            b.buttons.back().blockId = blockId;
        }
    } else if (b.typeStr == u"context"_s) {
        // Context blocks have an "elements" array; concatenate text elements
        // (mrkdwn ones parsed, with entity offsets shifted to the joined text).
        TextWithEntities ct;
        for (const auto &ev : o.value(u"elements"_s).toArray()) {
            const auto el    = ev.toObject();
            const auto etype = el.value(u"type"_s).toString();
            if (etype != u"mrkdwn"_s && etype != u"plain_text"_s)
                continue;
            const TextWithEntities part = parseTextObj(el);
            if (part.text.isEmpty())
                continue;
            if (!ct.text.isEmpty())
                ct.text += u"  "_s;
            const int base = ct.text.size();
            ct.text += part.text;
            for (auto e : part.entities) {
                e.offset += base;
                ct.entities.push_back(e);
            }
        }
        b.text = ct;
    } else if (b.typeStr == u"table"_s) {
        // Table messages (Slack's newer editor). Each row is an array of cells;
        // a cell is a nested rich_text block, {"type":"raw_text","text":…}, or
        // null (the header row pads short rows with nulls) → empty cell.
        for (const auto &rv : o.value(u"rows"_s).toArray()) {
            std::vector<TextWithEntities> row;
            for (const auto &cv : rv.toArray()) {
                const auto cell = cv.toObject();
                const auto ct   = cell.value(u"type"_s).toString();
                if (ct == u"rich_text"_s)
                    row.push_back(richTextToTWE(cell));
                else if (ct == u"raw_text"_s)
                    row.push_back(TextWithEntities{cell.value(u"text"_s).toString(), {}});
                else
                    row.push_back({});
            }
            b.tableRows.push_back(std::move(row));
        }
    } else if (b.typeStr == u"actions"_s) {
        // Buttons render as inert chips (URL buttons are clickable); other
        // interactive elements (selects, datepickers) aren't representable.
        for (const auto &ev : o.value(u"elements"_s).toArray()) {
            const auto el = ev.toObject();
            if (el.value(u"type"_s).toString() == u"button"_s) {
                b.buttons.push_back(toButton(el));
                b.buttons.back().blockId = blockId;
            }
        }
    }
    return b;
}

// An attachment's `ts` is the footer timestamp bots set (documented as an
// integer epoch, so it usually arrives as a JSON number) or, on a message
// unfurl, the QUOTED message's "1234.567890" ts string (the unfurling message
// has its own). Either way it becomes epoch micros for the card/footer.
static qint64 attachmentTs(const QJsonValue &v) {
    if (v.isString())
        return decimalTsToMicros(v.toString());
    if (v.isDouble())
        return qRound64(v.toDouble() * 1000000.0);
    return 0;
}

Attachment toAttachment(const QJsonObject &o) {
    std::vector<Block> blocks;
    for (const auto &bv : o.value(u"blocks"_s).toArray())
        blocks.push_back(toBlock(bv.toObject()));
    std::vector<BotButton> buttons;
    for (const auto &av : o.value(u"actions"_s).toArray()) {
        const auto ao = av.toObject();
        if (ao.value(u"type"_s).toString() == u"button"_s)
            buttons.push_back(toButton(ao));
    }
    std::vector<AttachmentField> fields;
    for (const auto &fv : o.value(u"fields"_s).toArray()) {
        const auto fo = fv.toObject();
        fields.push_back(
            AttachmentField{
                .title = fo.value(u"title"_s).toString(),
                .value = MrkdwnParser::parse(fo.value(u"value"_s).toString()),
            }
        );
    }
    // Shared-message unfurl: the attachment describes a quoted Slack message, so
    // its own `files` array (the quoted message's uploads) matters too.
    const bool        isMsgUnfurl = o.value(u"is_msg_unfurl"_s).toBool();
    std::vector<File> files;
    if (isMsgUnfurl)
        for (const auto &fv : o.value(u"files"_s).toArray())
            files.push_back(toFile(fv.toObject()));

    return Attachment{
        .id            = o.value(u"id"_s).toInt(),
        .fallback      = o.value(u"fallback"_s).toString(),
        .color         = o.value(u"color"_s).toString(),
        .pretext       = o.value(u"pretext"_s).toString(),
        .authorName    = o.value(u"author_name"_s).toString(),
        .title         = o.value(u"title"_s).toString(),
        .titleLink     = o.value(u"title_link"_s).toString(),
        .text          = MrkdwnParser::parse(o.value(u"text"_s).toString()),
        .imageUrl      = o.value(u"image_url"_s).toString(),
        .thumbUrl      = o.value(u"thumb_url"_s).toString(),
        .faviconUrl    = o.value(u"service_icon"_s).toString(),
        .footer        = o.value(u"footer"_s).toString(),
        .footerIcon    = o.value(u"footer_icon"_s).toString(),
        .imageWidth    = o.value(u"image_width"_s).toInt(),
        .imageHeight   = o.value(u"image_height"_s).toInt(),
        .thumbWidth    = o.value(u"thumb_width"_s).toInt(),
        .thumbHeight   = o.value(u"thumb_height"_s).toInt(),
        .fields        = std::move(fields),
        .blocks        = std::move(blocks),
        .buttons       = std::move(buttons),
        // Generated previews include app cards and shared-message links. Bot
        // attachments without unfurl metadata remain standalone content.
        .isLinkPreview = isMsgUnfurl || o.value(u"is_app_unfurl"_s).toBool() ||
                         o.value(u"is_unfurl"_s).toBool() ||
                         !o.value(u"original_url"_s).toString().isEmpty() ||
                         !o.value(u"from_url"_s).toString().isEmpty(),
        .isMsgUnfurl   = isMsgUnfurl,
        .authorIcon    = o.value(u"author_icon"_s).toString(),
        .authorSubname = o.value(u"author_subname"_s).toString(),
        .channelId     = o.value(u"channel_id"_s).toString(),
        .msgDate       = attachmentTs(o.value(u"ts"_s)),
        .files         = std::move(files),
    };
}

SelfPresence toSelfPresence(const QJsonObject &o) {
    return SelfPresence{
        .loaded          = true,
        .active          = o.value(u"presence"_s).toString() == u"active"_s,
        .online          = o.value(u"online"_s).toBool(false),
        .autoAway        = o.value(u"auto_away"_s).toBool(false),
        .manualAway      = o.value(u"manual_away"_s).toBool(false),
        .connectionCount = o.value(u"connection_count"_s).toInt(0),
    };
}

SearchResult toSearchResult(const QJsonObject &o) {
    return SearchResult{
        .conv     = ConversationId{o.value(u"channel"_s).toObject().value(u"id"_s).toString()},
        .convName = o.value(u"channel"_s).toObject().value(u"name"_s).toString(),
        .msg      = toMessage(o),
    };
}

Message toMessage(const QJsonObject &o) {
    // Handle message_changed subtype — actual message is nested under "message"
    auto msg = o;
    if (o.value(u"subtype"_s).toString() == u"message_changed"_s)
        msg = o.value(u"message"_s).toObject();

    std::vector<File> files;
    for (const auto &fv : msg.value(u"files"_s).toArray())
        files.push_back(toFile(fv.toObject()));

    std::vector<Block> blocks;
    for (const auto &bv : msg.value(u"blocks"_s).toArray())
        blocks.push_back(toBlock(bv.toObject()));

    std::vector<Attachment> attachments;
    for (const auto &av : msg.value(u"attachments"_s).toArray())
        attachments.push_back(toAttachment(av.toObject()));

    // Extract bot display name and avatar from username / bot_profile / icon_url.
    QString botName;
    QString botAvatarUrl;
    if (msg.contains(u"bot_id"_s)) {
        botName               = msg.value(u"username"_s).toString();
        const auto botProfile = msg.value(u"bot_profile"_s).toObject();
        if (botName.isEmpty())
            botName = botProfile.value(u"name"_s).toString();
        const auto icons = botProfile.value(u"icons"_s).toObject();
        botAvatarUrl =
            icons.value(u"image_72"_s)
                .toString(
                    icons.value(u"image_48"_s).toString(icons.value(u"image_36"_s).toString())
                );
        if (botAvatarUrl.isEmpty())
            botAvatarUrl = msg.value(u"icon_url"_s).toString();
    }

    const QString ts = msg.value(u"ts"_s).toString();
    Message       m{
        .ts   = ts,
        .date = decimalTsToMicros(ts), // epoch micros for sort + display
        .threadRoot =
            msg.contains(u"thread_ts"_s) && msg.value(u"thread_ts"_s) != msg.value(u"ts"_s)
                ? std::optional<Ts>(msg.value(u"thread_ts"_s).toString())
                : std::nullopt,
        .replyCount = msg.value(u"reply_count"_s).toInt(),
        .replyUsers =
            [&] {
                std::vector<UserId> v;
                for (const auto &u : msg.value(u"reply_users"_s).toArray())
                    v.push_back(UserId{u.toString()});
                return v;
            }(),
        .latestReply  = msg.contains(u"latest_reply"_s)
                            ? std::optional<Ts>(msg.value(u"latest_reply"_s).toString())
                            : std::nullopt,
        // Author of the thread root, present on reply events; drives the
        // "reply to a thread I started" notification (isFollowedThreadReply).
        .parentUserId = UserId{msg.value(u"parent_user_id"_s).toString()},
        .author       = UserId{msg.value(u"user"_s).toString(msg.value(u"bot_id"_s).toString())},
        .botName      = botName,
        .botAvatarUrl = botAvatarUrl,
        .botId        = msg.value(u"bot_id"_s).toString(),
        .text         = MrkdwnParser::parse(msg.value(u"text"_s).toString()),
        .rawText      = msg.value(u"text"_s).toString(),
        .reactions    = parseReactions(msg.value(u"reactions"_s).toArray()),
        .edited       = msg.contains(u"edited"_s),
        .subtype      = msg.contains(u"subtype"_s)
                            ? std::optional<QString>(msg.value(u"subtype"_s).toString())
                            : std::nullopt,
        .files        = std::move(files),
        .blocks       = std::move(blocks),
        .attachments  = std::move(attachments),
    };
    if (isHuddleMessage(m))
        m.huddle = readHuddleSummary(msg.value(u"room"_s).toObject());
    presentHuddleThread(m);
    return m;
}

std::vector<User> toUsers(const QJsonArray &a) {
    std::vector<User> out;
    out.reserve(a.size());
    for (auto v : a) {
        auto u = toUser(v.toObject());
        if (!u.id.value.isEmpty())
            out.push_back(std::move(u));
    }
    return out;
}

Usergroup toUsergroup(const QJsonObject &o) {
    Usergroup g;
    g.id     = o.value(u"id"_s).toString();
    g.handle = o.value(u"handle"_s).toString();
    g.name   = o.value(u"name"_s).toString();
    // include_users=1 puts the member ids in `users`; without it there is only
    // `user_count`, so a missing array simply means "membership unknown".
    for (const auto &v : o.value(u"users"_s).toArray())
        if (const auto id = v.toString(); !id.isEmpty())
            g.users.push_back(UserId{id});
    return g;
}

std::vector<Usergroup> toUsergroups(const QJsonArray &a) {
    std::vector<Usergroup> out;
    out.reserve(a.size());
    for (auto v : a) {
        auto g = toUsergroup(v.toObject());
        if (!g.id.isEmpty())
            out.push_back(std::move(g));
    }
    return out;
}

std::vector<Conversation> toConversations(const QJsonArray &a) {
    std::vector<Conversation> out;
    out.reserve(a.size());
    for (auto v : a) {
        auto c = toConversation(v.toObject());
        if (!c.id.value.isEmpty())
            out.push_back(std::move(c));
    }
    return out;
}

std::vector<ConvCounts> toConvCounts(const QJsonObject &resp) {
    // client.counts groups conversations by kind into three arrays of identically
    // shaped entries. Unlike conversations.list it reports `latest` for CHANNELS
    // too, which is the whole reason we call it.
    std::vector<ConvCounts> out;
    for (const auto *key : {"channels", "mpims", "ims"}) {
        const auto arr = resp.value(QLatin1String(key)).toArray();
        out.reserve(out.size() + arr.size());
        for (const auto v : arr) {
            const auto o  = v.toObject();
            const auto id = o.value(u"id"_s).toString();
            if (id.isEmpty())
                continue;
            // The endpoint reports mentions exactly; for everything else it gives
            // only the boolean has_unreads (a channel's plain-traffic count is not
            // exposed). Both DM shapes appear in the wild: `dm_count` on some
            // responses, has_unreads on all of them. A 1 stands in for "some" —
            // Session only ever compares counts for movement, never displays them.
            const int mentions = o.value(u"mention_count"_s).toInt();
            const int dmCount  = o.value(u"dm_count"_s).toInt();
            const int unread =
                std::max({mentions, dmCount, o.value(u"has_unreads"_s).toBool() ? 1 : 0});
            out.push_back(
                ConvCounts{
                    .id           = ConversationId{id},
                    .latestTs     = o.value(u"latest"_s).toString(),
                    .lastRead     = o.value(u"last_read"_s).toString(),
                    .unread       = unread,
                    .mentionCount = mentions,
                }
            );
        }
    }
    return out;
}

ThreadsViewPage toThreadsViewPage(const QJsonObject &resp) {
    ThreadsViewPage page;
    page.totalUnreadReplies = resp.value(u"total_unread_replies"_s).toInt();
    page.hasMore            = resp.value(u"has_more"_s).toBool();
    // Continuation cursor: the top-level max_ts, passed back as current_ts.
    // There is no response_metadata.next_cursor on this endpoint.
    page.nextCursor         = resp.value(u"max_ts"_s).toString();
    const auto threads      = resp.value(u"threads"_s).toArray();
    page.threads.reserve(threads.size());
    for (const auto v : threads) {
        const auto t       = v.toObject();
        const auto rootObj = t.value(u"root_msg"_s).toObject();
        // Only currently-subscribed threads have been observed, but filter
        // defensively so a future inactive entry can't pollute the view.
        if (rootObj.contains(u"subscribed"_s) && !rootObj.value(u"subscribed"_s).toBool())
            continue;
        ThreadOverview item;
        // root_msg is a complete parent message and — unlike history messages —
        // carries the channel it lives in.
        item.conv     = ConversationId{rootObj.value(u"channel"_s).toString()};
        item.root     = toMessage(rootObj);
        item.lastRead = rootObj.value(u"last_read"_s).toString();
        if (item.conv.value.isEmpty() || item.root.ts.isEmpty())
            continue;
        // The replies key varies by read state: `latest_replies` on a fully-read
        // thread, `unread_replies` (with NO latest_replies at all) when there
        // are replies past last_read. Merge both — a partially-read thread
        // plausibly carries both — and dedup by ts.
        for (const auto key : {"latest_replies", "unread_replies"}) {
            const auto replies = t.value(QLatin1String(key)).toArray();
            for (const auto rv : replies) {
                auto m = toMessage(rv.toObject());
                if (!m.ts.isEmpty())
                    item.latestReplies.push_back(std::move(m));
            }
        }
        // Oldest-first for display, whatever order the server sent.
        std::sort(item.latestReplies.begin(), item.latestReplies.end(), MessageDateLess{});
        item.latestReplies.erase(
            std::unique(
                item.latestReplies.begin(),
                item.latestReplies.end(),
                [](const Message &a, const Message &b) { return a.ts == b.ts; }
            ),
            item.latestReplies.end()
        );
        page.threads.push_back(std::move(item));
    }
    return page;
}

std::vector<MessageReminder> toMessageReminders(const QJsonObject &resp) {
    std::vector<MessageReminder> out;
    const auto                   items = resp.value(u"saved_items"_s).toArray();
    out.reserve(items.size());
    for (const auto v : items) {
        const auto it = v.toObject();
        // Saved items also cover files; we keep the pending message items —
        // reminders (date_due set) and plain "save for later" ones (date_due 0).
        if (it.value(u"item_type"_s).toString() != QLatin1String("message"))
            continue;
        if (it.value(u"state"_s).toString() == QLatin1String("completed") ||
            it.value(u"is_archived"_s).toBool())
            continue;
        MessageReminder r;
        r.conv    = ConversationId{it.value(u"item_id"_s).toString()};
        r.ts      = it.value(u"ts"_s).toString();
        // date_due can exceed int range (Unix seconds) — read as double.
        r.dueAt   = static_cast<qint64>(it.value(u"date_due"_s).toDouble());
        r.savedAt = static_cast<qint64>(it.value(u"date_created"_s).toDouble());
        if (r.conv.value.isEmpty() || r.ts.isEmpty())
            continue;
        out.push_back(std::move(r));
    }
    return out;
}

std::vector<ConversationId> toStarredConversationIds(const QJsonArray &items) {
    static const QSet<QString> convTypes = {
        QStringLiteral("channel"),
        QStringLiteral("im"),
        QStringLiteral("group"),
        QStringLiteral("mpim"),
    };
    std::vector<ConversationId> out;
    out.reserve(items.size());
    for (const auto v : items) {
        const auto it = v.toObject();
        if (!convTypes.contains(it.value(u"type"_s).toString()))
            continue;
        // Defensive: a message/file star also carries `channel`, so never let one
        // through on the strength of that field alone.
        if (it.contains(u"message"_s) || it.contains(u"file"_s) || it.contains(u"comment"_s))
            continue;
        const QString id = it.value(u"channel"_s).toString();
        if (id.isEmpty())
            continue;
        out.push_back(ConversationId{id});
    }
    return out;
}

std::vector<Message> toMessages(const QJsonArray &a, bool reverseOrder) {
    std::vector<Message> out;
    out.reserve(a.size());
    for (auto v : a)
        out.push_back(toMessage(v.toObject()));
    if (reverseOrder)
        std::reverse(out.begin(), out.end());
    return out;
}

std::vector<SearchResult> toSearchResults(const QJsonArray &a) {
    std::vector<SearchResult> out;
    out.reserve(a.size());
    for (auto v : a) {
        auto r = toSearchResult(v.toObject());
        if (!r.conv.value.isEmpty())
            out.push_back(std::move(r));
    }
    return out;
}

static SlashCommand toSlashCommand(const QJsonObject &o) {
    SlashCommand c;
    c.name = o.value(u"name"_s).toString();
    if (c.name.startsWith('/'))
        c.name = c.name.mid(1);
    c.desc  = o.value(u"desc"_s).toString();
    c.usage = o.value(u"usage"_s).toString();
    if (o.value(u"type"_s).toString() == QLatin1String("app"))
        c.appId = o.value(u"app"_s).toString();
    // App identity for the command palette. The endpoint is undocumented and
    // these keys are best-effort; the palette falls back gracefully when absent.
    c.appName = o.value(u"app_name"_s).toString();
    c.iconUrl = o.value(u"icon_url"_s).toString();
    if (c.iconUrl.isEmpty()) {
        const QJsonObject icons = o.value(u"icons"_s).toObject();
        c.iconUrl               = icons.value(u"image_48"_s).toString();
        if (c.iconUrl.isEmpty())
            c.iconUrl = icons.value(u"image_36"_s).toString();
    }
    return c;
}

std::vector<SlashCommand> toSlashCommands(const QJsonValue &v) {
    std::vector<SlashCommand> out;
    const auto                add = [&out](const QJsonObject &o) {
        auto c = toSlashCommand(o);
        if (!c.name.isEmpty())
            out.push_back(std::move(c));
    };
    if (v.isArray()) {
        const auto a = v.toArray();
        out.reserve(a.size());
        for (auto e : a)
            add(e.toObject());
    } else if (v.isObject()) {
        const auto o = v.toObject();
        out.reserve(o.size());
        for (auto it = o.begin(); it != o.end(); ++it)
            add(it.value().toObject());
    }
    return out;
}

} // namespace JsonMappers
} // namespace slack
