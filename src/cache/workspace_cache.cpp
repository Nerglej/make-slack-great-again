// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "workspace_cache.h"
#include "cache_evictor.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QUrl>
#include <algorithm>

using namespace Qt::StringLiterals;

// ── JSON serialization helpers ────────────────────────────────────────────────

static QJsonObject toJson(const TextEntity &e) {
    QJsonObject o;
    o[u"t"_s] = static_cast<int>(e.type);
    o[u"o"_s] = e.offset;
    o[u"l"_s] = e.length;
    if (!e.data.isEmpty())
        o[u"d"_s] = e.data;
    return o;
}
static TextEntity entityFromJson(const QJsonObject &o) {
    TextEntity e;
    e.type   = static_cast<EntityType>(o[u"t"_s].toInt());
    e.offset = o[u"o"_s].toInt();
    e.length = o[u"l"_s].toInt();
    e.data   = o[u"d"_s].toString();
    return e;
}

static QJsonObject toJson(const TextWithEntities &t) {
    QJsonObject o;
    o[u"x"_s] = t.text;
    if (!t.entities.empty()) {
        QJsonArray arr;
        for (const auto &e : t.entities)
            arr.append(toJson(e));
        o[u"e"_s] = arr;
    }
    return o;
}
static TextWithEntities tweFromJson(const QJsonObject &o) {
    TextWithEntities t;
    t.text = o[u"x"_s].toString();
    for (const auto &v : o[u"e"_s].toArray())
        t.entities.push_back(entityFromJson(v.toObject()));
    return t;
}

static QJsonObject toJson(const Reaction &r) {
    QJsonObject o;
    o[u"n"_s] = r.name;
    o[u"c"_s] = r.count;
    QJsonArray users;
    for (const auto &u : r.users)
        users.append(u.value);
    o[u"u"_s] = users;
    return o;
}
static Reaction reactionFromJson(const QJsonObject &o) {
    Reaction r;
    r.name  = o[u"n"_s].toString();
    r.count = o[u"c"_s].toInt();
    for (const auto &v : o[u"u"_s].toArray())
        r.users.push_back(UserId{v.toString()});
    return r;
}

static QJsonObject toJson(const File &f) {
    QJsonObject o;
    o[u"id"_s] = f.id;
    o[u"na"_s] = f.name;
    o[u"mi"_s] = f.mimeType;
    o[u"pt"_s] = f.prettyType;
    o[u"up"_s] = f.urlPrivate;
    o[u"pl"_s] = f.permalink;
    o[u"th"_s] = f.thumbUrl;
    o[u"iw"_s] = f.imageWidth;
    o[u"ih"_s] = f.imageHeight;
    o[u"sz"_s] = static_cast<double>(f.size);
    if (!f.urlPrivateDownload.isEmpty())
        o[u"ud"_s] = f.urlPrivateDownload;
    if (f.durationMs > 0)
        o[u"dm"_s] = static_cast<double>(f.durationMs);
    if (!f.aacUrl.isEmpty())
        o[u"aac"_s] = f.aacUrl;
    if (!f.subtype.isEmpty())
        o[u"st"_s] = f.subtype;
    if (!f.fileType.isEmpty())
        o[u"ft"_s] = f.fileType;
    if (!f.transcriptStatus.isEmpty())
        o[u"tst"_s] = f.transcriptStatus;
    if (!f.transcriptPreview.isEmpty())
        o[u"tpv"_s] = f.transcriptPreview;
    if (!f.transcriptVttUrl.isEmpty())
        o[u"tvt"_s] = f.transcriptVttUrl;
    if (!f.title.isEmpty())
        o[u"ti"_s] = f.title;
    if (!f.thumbs.empty()) {
        QJsonArray arr;
        for (const auto &t : f.thumbs)
            arr.append(QJsonObject{{u"w"_s, t.width}, {u"h"_s, t.height}, {u"u"_s, t.url}});
        o[u"tb"_s] = arr;
    }
    if (!f.animThumbs.empty()) {
        QJsonArray arr;
        for (const auto &t : f.animThumbs)
            arr.append(QJsonObject{{u"w"_s, t.width}, {u"h"_s, t.height}, {u"u"_s, t.url}});
        o[u"ta"_s] = arr;
    }
    return o;
}
static File fileFromJson(const QJsonObject &o) {
    File f;
    f.id                 = o[u"id"_s].toString();
    f.name               = o[u"na"_s].toString();
    f.mimeType           = o[u"mi"_s].toString();
    f.prettyType         = o[u"pt"_s].toString();
    f.urlPrivate         = o[u"up"_s].toString();
    f.permalink          = o[u"pl"_s].toString();
    f.thumbUrl           = o[u"th"_s].toString();
    f.imageWidth         = o[u"iw"_s].toInt();
    f.imageHeight        = o[u"ih"_s].toInt();
    f.size               = static_cast<qint64>(o[u"sz"_s].toDouble());
    f.urlPrivateDownload = o[u"ud"_s].toString();
    f.durationMs         = static_cast<qint64>(o[u"dm"_s].toDouble());
    f.aacUrl             = o[u"aac"_s].toString();
    f.subtype            = o[u"st"_s].toString();
    f.fileType           = o[u"ft"_s].toString();
    f.transcriptStatus   = o[u"tst"_s].toString();
    f.transcriptPreview  = o[u"tpv"_s].toString();
    f.transcriptVttUrl   = o[u"tvt"_s].toString();
    f.title              = o[u"ti"_s].toString();
    for (const auto &v : o[u"tb"_s].toArray()) {
        const auto t = v.toObject();
        f.thumbs.push_back(FileThumb{t[u"w"_s].toInt(), t[u"h"_s].toInt(), t[u"u"_s].toString()});
    }
    for (const auto &v : o[u"ta"_s].toArray()) {
        const auto t = v.toObject();
        f.animThumbs.push_back(
            FileThumb{t[u"w"_s].toInt(), t[u"h"_s].toInt(), t[u"u"_s].toString()}
        );
    }
    return f;
}

static QJsonArray buttonsToJson(const std::vector<BotButton> &buttons) {
    QJsonArray arr;
    for (const auto &btn : buttons) {
        QJsonObject o{{u"t"_s, btn.text}, {u"u"_s, btn.url}, {u"s"_s, btn.style}};
        if (!btn.actionId.isEmpty()) {
            o[u"a"_s] = btn.actionId;
            o[u"b"_s] = btn.blockId;
            o[u"v"_s] = btn.value;
        }
        arr.append(o);
    }
    return arr;
}
static std::vector<BotButton> buttonsFromJson(const QJsonArray &arr) {
    std::vector<BotButton> buttons;
    for (const auto &v : arr) {
        const auto o = v.toObject();
        buttons.push_back(
            BotButton{
                .text     = o[u"t"_s].toString(),
                .url      = o[u"u"_s].toString(),
                .style    = o[u"s"_s].toString(),
                .actionId = o[u"a"_s].toString(),
                .blockId  = o[u"b"_s].toString(),
                .value    = o[u"v"_s].toString(),
            }
        );
    }
    return buttons;
}

static QJsonObject toJson(const Block &b) {
    QJsonObject o;
    o[u"ty"_s] = b.typeStr;
    o[u"tx"_s] = toJson(b.text);
    if (!b.imageUrl.isEmpty())
        o[u"iu"_s] = b.imageUrl;
    if (!b.altText.isEmpty())
        o[u"at"_s] = b.altText;
    if (!b.buttons.empty())
        o[u"bt"_s] = buttonsToJson(b.buttons);
    if (!b.tableRows.empty()) {
        QJsonArray rows;
        for (const auto &row : b.tableRows) {
            QJsonArray cells;
            for (const auto &cell : row)
                cells.append(toJson(cell));
            rows.append(cells);
        }
        o[u"tr"_s] = rows;
    }
    return o;
}
static Block blockFromJson(const QJsonObject &o) {
    Block b;
    b.typeStr  = o[u"ty"_s].toString();
    b.text     = tweFromJson(o[u"tx"_s].toObject());
    b.imageUrl = o[u"iu"_s].toString();
    b.altText  = o[u"at"_s].toString();
    b.buttons  = buttonsFromJson(o[u"bt"_s].toArray());
    for (const auto &rv : o[u"tr"_s].toArray()) {
        std::vector<TextWithEntities> row;
        for (const auto &cv : rv.toArray())
            row.push_back(tweFromJson(cv.toObject()));
        b.tableRows.push_back(std::move(row));
    }
    return b;
}

static QJsonObject toJson(const Attachment &a) {
    QJsonObject o;
    if (a.id > 0)
        o[u"id"_s] = a.id; // positional id, what chat.deleteAttachment addresses
    o[u"fb"_s] = a.fallback;
    o[u"co"_s] = a.color;
    o[u"pt"_s] = a.pretext;
    o[u"an"_s] = a.authorName;
    o[u"ti"_s] = a.title;
    o[u"tl"_s] = a.titleLink;
    o[u"tx"_s] = toJson(a.text);
    o[u"iu"_s] = a.imageUrl;
    o[u"tu"_s] = a.thumbUrl;
    o[u"fo"_s] = a.footer;
    if (!a.footerIcon.isEmpty())
        o[u"fc"_s] = a.footerIcon;
    if (a.msgDate > 0)
        o[u"md"_s] = QString::number(a.msgDate); // epoch micros; string-encoded like Message::date
    o[u"lp"_s]  = a.isLinkPreview;
    o[u"lpv"_s] = 2; // app unfurls now count as previews too
    if (a.imageWidth > 0)
        o[u"iw"_s] = a.imageWidth;
    if (a.imageHeight > 0)
        o[u"ih"_s] = a.imageHeight;
    if (a.thumbWidth > 0)
        o[u"tw"_s] = a.thumbWidth;
    if (a.thumbHeight > 0)
        o[u"tg"_s] = a.thumbHeight;
    if (!a.blocks.empty()) {
        QJsonArray arr;
        for (const auto &b : a.blocks)
            arr.append(toJson(b));
        o[u"bl"_s] = arr;
    }
    if (!a.buttons.empty())
        o[u"bt"_s] = buttonsToJson(a.buttons);
    // Classic bot "fields" rows. Jenkins-style bots put their whole body here
    // (text empty, fallback = the same string): dropping them demoted every
    // cached copy to the fallback rendering.
    if (!a.fields.empty()) {
        QJsonArray arr;
        for (const auto &f : a.fields) {
            QJsonObject fo;
            fo[u"t"_s] = f.title;
            fo[u"v"_s] = toJson(f.value);
            arr.append(fo);
        }
        o[u"fd"_s] = arr;
    }
    if (a.isMsgUnfurl) {
        o[u"mu"_s] = true;
        o[u"ai"_s] = a.authorIcon;
        o[u"as"_s] = a.authorSubname;
        o[u"ci"_s] = a.channelId;
        if (!a.files.empty()) {
            QJsonArray arr;
            for (const auto &f : a.files)
                arr.append(toJson(f));
            o[u"fi"_s] = arr;
        }
    }
    return o;
}
static Attachment attachmentFromJson(const QJsonObject &o) {
    Attachment a;
    a.id            = o[u"id"_s].toInt();
    a.fallback      = o[u"fb"_s].toString();
    a.color         = o[u"co"_s].toString();
    a.pretext       = o[u"pt"_s].toString();
    a.authorName    = o[u"an"_s].toString();
    a.title         = o[u"ti"_s].toString();
    a.titleLink     = o[u"tl"_s].toString();
    a.text          = tweFromJson(o[u"tx"_s].toObject());
    a.imageUrl      = o[u"iu"_s].toString();
    a.thumbUrl      = o[u"tu"_s].toString();
    a.footer        = o[u"fo"_s].toString();
    a.footerIcon    = o[u"fc"_s].toString();
    a.msgDate       = o[u"md"_s].toString().toLongLong();
    a.imageWidth    = o[u"iw"_s].toInt();
    a.imageHeight   = o[u"ih"_s].toInt();
    a.thumbWidth    = o[u"tw"_s].toInt();
    a.thumbHeight   = o[u"tg"_s].toInt();
    a.isLinkPreview = o[u"lp"_s].toBool();
    for (const auto &v : o[u"bl"_s].toArray())
        a.blocks.push_back(blockFromJson(v.toObject()));
    a.buttons = buttonsFromJson(o[u"bt"_s].toArray());
    for (const auto &v : o[u"fd"_s].toArray()) {
        const auto fo = v.toObject();
        a.fields.push_back(
            AttachmentField{
                .title = fo[u"t"_s].toString(),
                .value = tweFromJson(fo[u"v"_s].toObject()),
            }
        );
    }
    a.isMsgUnfurl = o[u"mu"_s].toBool();
    if (a.isMsgUnfurl) {
        a.authorIcon    = o[u"ai"_s].toString();
        a.authorSubname = o[u"as"_s].toString();
        a.channelId     = o[u"ci"_s].toString();
        for (const auto &v : o[u"fi"_s].toArray())
            a.files.push_back(fileFromJson(v.toObject()));
    }
    return a;
}

static QJsonObject toJson(const Message &m) {
    QJsonObject o;
    o[u"ts"_s] = m.ts;
    o[u"da"_s] = QString::number(m.date); // epoch micros; string-encoded to avoid JSON double loss
    if (m.threadRoot)
        o[u"tr"_s] = *m.threadRoot;
    o[u"au"_s] = m.author.value;
    if (!m.botName.isEmpty())
        o[u"bn"_s] = m.botName;
    if (!m.botAvatarUrl.isEmpty())
        o[u"ba"_s] = m.botAvatarUrl;
    if (!m.botId.isEmpty())
        o[u"bd"_s] = m.botId;
    o[u"tx"_s] = toJson(m.text);
    o[u"ed"_s] = m.edited;
    if (m.subtype)
        o[u"st"_s] = *m.subtype;
    if (!m.reactions.empty()) {
        QJsonArray arr;
        for (const auto &r : m.reactions)
            arr.append(toJson(r));
        o[u"re"_s] = arr;
    }
    if (!m.files.empty()) {
        QJsonArray arr;
        for (const auto &f : m.files)
            arr.append(toJson(f));
        o[u"fi"_s] = arr;
    }
    if (!m.blocks.empty()) {
        QJsonArray arr;
        for (const auto &b : m.blocks)
            arr.append(toJson(b));
        o[u"bl"_s] = arr;
    }
    if (!m.attachments.empty()) {
        QJsonArray arr;
        for (const auto &a : m.attachments)
            arr.append(toJson(a));
        o[u"at"_s] = arr;
    }
    if (m.huddle) {
        QJsonArray who;
        for (const auto &u : m.huddle->attendees)
            who.append(u.value);
        o[u"hu"_s] = QJsonObject{
            {u"a"_s, who},
            {u"s"_s, QString::number(m.huddle->startSec)},
            {u"e"_s, QString::number(m.huddle->endSec)},
            {u"x"_s, m.huddle->ended},
        };
    }
    return o;
}
// Host (minus "www.") plus path (minus trailing '/'), lower-cased host; the
// part of a URL that survives Slack's unfurl canonicalisation.
static QString urlResourceKey(const QString &url) {
    const QUrl u(url);
    if (!u.isValid() || u.host().isEmpty())
        return url;
    QString host = u.host().toLower();
    if (host.startsWith(QLatin1String("www.")))
        host.remove(0, 4);
    QString path = u.path();
    while (path.size() > 1 && path.endsWith(QLatin1Char('/')))
        path.chop(1);
    return host + path;
}
static Message messageFromJson(const QJsonObject &o) {
    Message m;
    m.ts   = o[u"ts"_s].toString();
    // Legacy caches predate the field — backfill from the stored ts so old and
    // new entries agree to the microsecond (no cache-version bump needed).
    m.date = o.contains(u"da"_s) ? o[u"da"_s].toString().toLongLong() : decimalTsToMicros(m.ts);
    if (o.contains(u"tr"_s))
        m.threadRoot = o[u"tr"_s].toString();
    m.author       = UserId{o[u"au"_s].toString()};
    m.botName      = o[u"bn"_s].toString();
    m.botAvatarUrl = o[u"ba"_s].toString();
    m.botId        = o[u"bd"_s].toString();
    m.text         = tweFromJson(o[u"tx"_s].toObject());
    m.edited       = o[u"ed"_s].toBool();
    if (o.contains(u"st"_s))
        m.subtype = o[u"st"_s].toString();
    for (const auto &v : o[u"re"_s].toArray())
        m.reactions.push_back(reactionFromJson(v.toObject()));
    for (const auto &v : o[u"fi"_s].toArray())
        m.files.push_back(fileFromJson(v.toObject()));
    for (const auto &v : o[u"bl"_s].toArray())
        m.blocks.push_back(blockFromJson(v.toObject()));
    for (const auto &v : o[u"at"_s].toArray()) {
        const auto obj = v.toObject();
        auto       att = attachmentFromJson(obj);
        // Older caches discarded Slack's unfurl metadata. Only infer a preview
        // when its target is also a link in the message body; a bot attachment
        // with a linked title alone is not enough.
        if (obj.value(u"lpv"_s).toInt() < 2 && !att.isLinkPreview && !att.isMsgUnfurl &&
            m.botName.isEmpty() && (!m.subtype || *m.subtype != QLatin1String("bot_message"))) {
            // Slack reports the unfurl's canonical URL (redirects resolved,
            // tracking params dropped), so compare host+path, not the string.
            const auto linksTo = [&](const QString &url) {
                if (url.isEmpty())
                    return false;
                const QString key = urlResourceKey(url);
                return std::any_of(
                    m.text.entities.begin(), m.text.entities.end(), [&](const TextEntity &e) {
                        return e.type == EntityType::Link && urlResourceKey(e.data) == key;
                    }
                );
            };
            att.isLinkPreview = linksTo(att.titleLink) || linksTo(att.imageUrl);
        }
        m.attachments.push_back(std::move(att));
    }
    if (const auto h = o[u"hu"_s].toObject(); !h.isEmpty()) {
        HuddleInfo info;
        for (const auto &v : h[u"a"_s].toArray())
            info.attendees.push_back(UserId{v.toString()});
        info.startSec = h[u"s"_s].toString().toLongLong();
        info.endSec   = h[u"e"_s].toString().toLongLong();
        info.ended    = h[u"x"_s].toBool();
        m.huddle      = std::move(info);
    }
    // Re-derive the synthesized huddle label on every load — it must follow
    // the current locale, and rows cached before the transform existed have
    // empty text.
    presentHuddleThread(m);
    return m;
}

static QJsonObject toJson(const User &u) {
    QJsonObject o;
    o[u"id"_s] = u.id.value;
    o[u"na"_s] = u.name;
    o[u"dn"_s] = u.displayName;
    o[u"av"_s] = u.avatarUrl;
    o[u"bo"_s] = u.isBot;
    o[u"ex"_s] = u.isExternal;
    o[u"ac"_s] = u.isActive;
    o[u"de"_s] = u.isDeactivated;
    o[u"ad"_s] = u.isAdmin;
    o[u"ow"_s] = u.isOwner;
    o[u"se"_s] = u.statusEmoji;
    o[u"st"_s] = u.statusText;
    o[u"ti"_s] = u.title;
    if (!u.email.isEmpty())
        o[u"em"_s] = u.email;
    if (u.hasTz)
        o[u"tz"_s] = u.tzOffset;
    return o;
}
static User userFromJson(const QJsonObject &o) {
    User u;
    u.id            = UserId{o[u"id"_s].toString()};
    u.name          = o[u"na"_s].toString();
    u.displayName   = o[u"dn"_s].toString();
    u.avatarUrl     = o[u"av"_s].toString();
    u.isBot         = o[u"bo"_s].toBool();
    u.isExternal    = o[u"ex"_s].toBool();
    u.isActive      = o[u"ac"_s].toBool();
    u.isDeactivated = o[u"de"_s].toBool();
    u.isAdmin       = o[u"ad"_s].toBool();
    u.isOwner       = o[u"ow"_s].toBool();
    u.statusEmoji   = o[u"se"_s].toString();
    u.statusText    = o[u"st"_s].toString();
    u.title         = o[u"ti"_s].toString();
    u.email         = o[u"em"_s].toString();
    u.hasTz         = o.contains(u"tz"_s);
    u.tzOffset      = o[u"tz"_s].toInt();
    return u;
}

static QJsonObject toJson(const Conversation &c) {
    QJsonObject o;
    o[u"id"_s] = c.id.value;
    o[u"ki"_s] = static_cast<int>(c.kind);
    o[u"na"_s] = c.name;
    o[u"mb"_s] = c.isMember;
    o[u"lr"_s] = c.lastRead;
    if (!c.latestTs.isEmpty())
        o[u"lt"_s] = c.latestTs;
    o[u"un"_s] = c.unread;
    if (c.mentionCount > 0)
        o[u"mc"_s] = c.mentionCount;
    if (c.dmUser)
        o[u"dm"_s] = c.dmUser->value;
    if (c.isMuted)
        o[u"mu"_s] = true;
    if (c.isStarred)
        o[u"st"_s] = true;
    if (c.locallyMuted)
        o[u"lm"_s] = true;
    if (!c.localName.isEmpty())
        o[u"ln"_s] = c.localName;
    if (c.notifLevel != NotificationLevel::Default)
        o[u"nl"_s] = static_cast<int>(c.notifLevel);
    if (!c.agentRole.isEmpty())
        o[u"ar"_s] = c.agentRole;
    return o;
}
static Conversation convFromJson(const QJsonObject &o) {
    Conversation c;
    c.id           = ConversationId{o[u"id"_s].toString()};
    c.kind         = static_cast<ConvKind>(o[u"ki"_s].toInt());
    c.name         = o[u"na"_s].toString();
    c.isMember     = o[u"mb"_s].toBool();
    c.lastRead     = o[u"lr"_s].toString();
    c.latestTs     = o[u"lt"_s].toString();
    c.unread       = o[u"un"_s].toInt();
    c.mentionCount = o[u"mc"_s].toInt();
    if (o.contains(u"dm"_s))
        c.dmUser = UserId{o[u"dm"_s].toString()};
    if (o.contains(u"mu"_s))
        c.isMuted = o[u"mu"_s].toBool();
    if (o.contains(u"st"_s))
        c.isStarred = o[u"st"_s].toBool();
    if (o.contains(u"lm"_s))
        c.locallyMuted = o[u"lm"_s].toBool();
    c.localName = o[u"ln"_s].toString();
    if (o.contains(u"nl"_s))
        c.notifLevel = static_cast<NotificationLevel>(o[u"nl"_s].toInt());
    c.agentRole = o[u"ar"_s].toString();
    return c;
}

// ── WorkspaceCache ────────────────────────────────────────────────────────────

WorkspaceCache::WorkspaceCache(const QString &handle) {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    // `handle` is the WorkspaceKey form ("slack:T0123"). The ':' is illegal in a
    // path component on Windows, so sanitize it into a safe directory name.
    QString       safe = handle;
    safe.replace(QLatin1Char(':'), QLatin1Char('_'));
    _dir            = base + u"/cache/"_s + safe;
    // One-time migration: pre-multi-service caches were keyed by the bare id
    // (the part after the service prefix). Rename it forward so an existing
    // offline cache survives the upgrade instead of being silently rebuilt.
    const int colon = handle.indexOf(QLatin1Char(':'));
    if (colon > 0 && !QDir(_dir).exists()) {
        const QString legacy = base + u"/cache/"_s + handle.mid(colon + 1);
        if (QDir(legacy).exists())
            QDir().rename(legacy, _dir);
    }
    QDir().mkpath(_dir + u"/messages"_s);
    QDir().mkpath(_dir + u"/images"_s);
}

QString WorkspaceCache::convPath() const {
    return _dir + u"/conversations.json"_s;
}
QString WorkspaceCache::usersPath() const {
    return _dir + u"/users.json"_s;
}
QString WorkspaceCache::botsPath() const {
    return _dir + u"/bots.json"_s;
}
QString WorkspaceCache::usergroupsPath() const {
    return _dir + u"/usergroups.json"_s;
}
QString WorkspaceCache::emojiPath() const {
    return _dir + u"/emoji.json"_s;
}
QString WorkspaceCache::msgsPath(const ConversationId &conv) const {
    return _dir + u"/messages/"_s + conv.value + u".json"_s;
}
QString WorkspaceCache::metaPath() const {
    return _dir + u"/meta.json"_s;
}
QString WorkspaceCache::imgPath(const QString &url) const {
    const auto hash = QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Md5).toHex();
    return _dir + u"/images/"_s + hash;
}

QByteArray WorkspaceCache::readFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}

bool WorkspaceCache::writeFile(const QString &path, const QByteArray &data) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    f.write(data);
    return true;
}

bool WorkspaceCache::writeJson(const QString &path, const QJsonDocument &doc) {
    return writeFile(path, doc.toJson(QJsonDocument::Compact));
}

void WorkspaceCache::saveConversations(const std::vector<Conversation> &convs) {
    QJsonArray arr;
    for (const auto &c : convs)
        arr.append(toJson(c));
    writeJson(convPath(), QJsonDocument(arr));
}

std::vector<Conversation> WorkspaceCache::loadConversations() const {
    const auto data = readFile(convPath());
    if (data.isEmpty())
        return {};
    const auto doc = QJsonDocument::fromJson(data);
    if (!doc.isArray())
        return {};
    std::vector<Conversation> result;
    for (const auto &v : doc.array())
        result.push_back(convFromJson(v.toObject()));
    return result;
}

void WorkspaceCache::saveUsers(const std::vector<User> &users) {
    QJsonArray arr;
    for (const auto &u : users)
        arr.append(toJson(u));
    writeJson(usersPath(), QJsonDocument(arr));
}

std::vector<User> WorkspaceCache::loadUsers() const {
    const auto data = readFile(usersPath());
    if (data.isEmpty())
        return {};
    const auto doc = QJsonDocument::fromJson(data);
    if (!doc.isArray())
        return {};
    std::vector<User> result;
    for (const auto &v : doc.array())
        result.push_back(userFromJson(v.toObject()));
    return result;
}

void WorkspaceCache::saveBots(const QHash<QString, User> &bots) {
    QJsonArray arr;
    for (const auto &u : bots)
        arr.append(toJson(u));
    writeJson(botsPath(), QJsonDocument(arr));
}

void WorkspaceCache::saveUsergroups(const std::vector<Usergroup> &groups) {
    QJsonArray arr;
    for (const auto &g : groups) {
        QJsonArray users;
        for (const auto &u : g.users)
            users.append(u.value);
        arr.append(
            QJsonObject{{u"id"_s, g.id}, {u"ha"_s, g.handle}, {u"na"_s, g.name}, {u"us"_s, users}}
        );
    }
    writeJson(usergroupsPath(), QJsonDocument(arr));
}

std::vector<Usergroup> WorkspaceCache::loadUsergroups() const {
    const auto data = readFile(usergroupsPath());
    if (data.isEmpty())
        return {};
    const auto doc = QJsonDocument::fromJson(data);
    if (!doc.isArray())
        return {};
    std::vector<Usergroup> result;
    for (const auto &v : doc.array()) {
        const auto o = v.toObject();
        Usergroup  g;
        g.id     = o[u"id"_s].toString();
        g.handle = o[u"ha"_s].toString();
        g.name   = o[u"na"_s].toString();
        for (const auto &u : o[u"us"_s].toArray())
            g.users.push_back(UserId{u.toString()});
        if (!g.id.isEmpty())
            result.push_back(std::move(g));
    }
    return result;
}

QHash<QString, User> WorkspaceCache::loadBots() const {
    const auto data = readFile(botsPath());
    if (data.isEmpty())
        return {};
    const auto doc = QJsonDocument::fromJson(data);
    if (!doc.isArray())
        return {};
    QHash<QString, User> result;
    for (const auto &v : doc.array()) {
        auto u = userFromJson(v.toObject());
        if (!u.id.value.isEmpty())
            result[u.id.value] = std::move(u);
    }
    return result;
}

void WorkspaceCache::saveMessages(const ConversationId &conv, const std::vector<Message> &msgs) {
    const int  total = static_cast<int>(msgs.size());
    const int  start = std::max(0, total - kMaxMessages);
    QJsonArray arr;
    for (int i = start; i < total; ++i)
        arr.append(toJson(msgs[i]));
    writeJson(msgsPath(conv), QJsonDocument(arr));
}

std::vector<Message> WorkspaceCache::loadMessages(const ConversationId &conv) const {
    const auto data = readFile(msgsPath(conv));
    if (data.isEmpty())
        return {};
    const auto doc = QJsonDocument::fromJson(data);
    if (!doc.isArray())
        return {};
    std::vector<Message> result;
    for (const auto &v : doc.array())
        result.push_back(messageFromJson(v.toObject()));
    return result;
}

QJsonObject &WorkspaceCache::metaObject() const {
    if (!_meta)
        _meta = QJsonDocument::fromJson(readFile(metaPath())).object();
    return *_meta;
}

void WorkspaceCache::writeMeta() {
    writeJson(metaPath(), QJsonDocument(metaObject()));
}

void WorkspaceCache::saveLastConv(const ConversationId &conv, const QString &displayName) {
    // meta.json also carries other keys (activity sweep stamp etc.) — they
    // survive because the cached object is mutated in place, no re-read needed.
    auto &o      = metaObject();
    o[u"conv"_s] = conv.value;
    o[u"name"_s] = displayName;
    writeMeta();
}

std::pair<ConversationId, QString> WorkspaceCache::loadLastConv() const {
    const auto &o = metaObject();
    if (o.isEmpty())
        return {};
    return {ConversationId{o.value(u"conv"_s).toString()}, o.value(u"name"_s).toString()};
}

void WorkspaceCache::saveMeUserId(const UserId &id) {
    metaObject()[u"meId"_s] = id.value;
    writeMeta();
}

UserId WorkspaceCache::loadMeUserId() const {
    return UserId{metaObject().value(u"meId"_s).toString()};
}

void WorkspaceCache::saveActivitySweepAt(qint64 unixSecs) {
    metaObject()[u"sweepAt"_s] = unixSecs;
    writeMeta();
}

qint64 WorkspaceCache::loadActivitySweepAt() const {
    return metaObject().value(u"sweepAt"_s).toVariant().toLongLong();
}

void WorkspaceCache::saveMutedThreads(const QStringList &keys) {
    metaObject()[u"mutedThreads"_s] = QJsonArray::fromStringList(keys);
    writeMeta();
}

QStringList WorkspaceCache::loadMutedThreads() const {
    QStringList out;
    for (const auto &v : metaObject().value(u"mutedThreads"_s).toArray())
        out.append(v.toString());
    return out;
}

void WorkspaceCache::saveFollowedThreads(const QStringList &keys) {
    metaObject()[u"followedThreads"_s] = QJsonArray::fromStringList(keys);
    writeMeta();
}

QStringList WorkspaceCache::loadFollowedThreads() const {
    QStringList out;
    for (const auto &v : metaObject().value(u"followedThreads"_s).toArray())
        out.append(v.toString());
    return out;
}

void WorkspaceCache::saveAiTranscripts(const QHash<QString, AiTranscript> &byFileId) {
    QJsonObject o;
    for (auto it = byFileId.constBegin(); it != byFileId.constEnd(); ++it)
        o[it.key()] = QJsonObject{{u"text"_s, it.value().text}, {u"by"_s, it.value().provider}};
    metaObject()[u"aiTranscripts"_s] = o;
    writeMeta();
}

QHash<QString, AiTranscript> WorkspaceCache::loadAiTranscripts() const {
    QHash<QString, AiTranscript> out;
    const QJsonObject            o = metaObject().value(u"aiTranscripts"_s).toObject();
    for (auto it = o.constBegin(); it != o.constEnd(); ++it) {
        const QJsonObject e = it.value().toObject();
        AiTranscript      t{e.value(u"text"_s).toString(), e.value(u"by"_s).toString()};
        if (!t.text.isEmpty())
            out.insert(it.key(), t);
    }
    return out;
}

void WorkspaceCache::saveReminders(const std::vector<MessageReminder> &reminders) {
    QJsonArray arr;
    for (const auto &r : reminders) {
        QJsonObject o;
        o[u"conv"_s] = r.conv.value;
        o[u"ts"_s]   = r.ts;
        o[u"due"_s]  = r.dueAt;
        if (r.savedAt > 0)
            o[u"saved"_s] = r.savedAt;
        if (!r.threadRoot.isEmpty())
            o[u"root"_s] = r.threadRoot;
        if (!r.snippet.isEmpty())
            o[u"snippet"_s] = r.snippet;
        if (!r.author.value.isEmpty())
            o[u"author"_s] = r.author.value;
        if (!r.botName.isEmpty())
            o[u"botName"_s] = r.botName;
        if (!r.botAvatarUrl.isEmpty())
            o[u"botAvatar"_s] = r.botAvatarUrl;
        if (r.fired)
            o[u"fired"_s] = true;
        arr.append(o);
    }
    metaObject()[u"reminders"_s] = arr;
    writeMeta();
}

std::vector<MessageReminder> WorkspaceCache::loadReminders() const {
    std::vector<MessageReminder> out;
    const auto                   arr = metaObject().value(u"reminders"_s).toArray();
    out.reserve(arr.size());
    for (const auto &v : arr) {
        const auto      o = v.toObject();
        MessageReminder r;
        r.conv         = ConversationId{o.value(u"conv"_s).toString()};
        r.ts           = o.value(u"ts"_s).toString();
        r.dueAt        = o.value(u"due"_s).toVariant().toLongLong();
        r.savedAt      = o.value(u"saved"_s).toVariant().toLongLong();
        r.threadRoot   = o.value(u"root"_s).toString();
        r.snippet      = o.value(u"snippet"_s).toString();
        r.author       = UserId{o.value(u"author"_s).toString()};
        r.botName      = o.value(u"botName"_s).toString();
        r.botAvatarUrl = o.value(u"botAvatar"_s).toString();
        r.fired        = o.value(u"fired"_s).toBool();
        if (!r.conv.value.isEmpty() && !r.ts.isEmpty())
            out.push_back(std::move(r));
    }
    return out;
}

void WorkspaceCache::saveReminderPreviews(const QHash<QString, ReminderPreview> &previews) {
    QJsonArray arr;
    for (auto it = previews.constBegin(); it != previews.constEnd(); ++it) {
        const auto &p = it.value();
        if (p.isEmpty())
            continue;
        QJsonObject o;
        o[u"key"_s] = it.key();
        if (!p.threadRoot.isEmpty())
            o[u"root"_s] = p.threadRoot;
        if (!p.snippet.isEmpty())
            o[u"snippet"_s] = p.snippet;
        if (!p.author.value.isEmpty())
            o[u"author"_s] = p.author.value;
        if (!p.botName.isEmpty())
            o[u"botName"_s] = p.botName;
        if (!p.botAvatarUrl.isEmpty())
            o[u"botAvatar"_s] = p.botAvatarUrl;
        arr.append(o);
    }
    metaObject()[u"reminderPreviews"_s] = arr;
    writeMeta();
}

QHash<QString, ReminderPreview> WorkspaceCache::loadReminderPreviews() const {
    QHash<QString, ReminderPreview> out;
    for (const auto &v : metaObject().value(u"reminderPreviews"_s).toArray()) {
        const auto    o   = v.toObject();
        const QString key = o.value(u"key"_s).toString();
        if (key.isEmpty())
            continue;
        ReminderPreview p;
        p.threadRoot   = o.value(u"root"_s).toString();
        p.snippet      = o.value(u"snippet"_s).toString();
        p.author       = UserId{o.value(u"author"_s).toString()};
        p.botName      = o.value(u"botName"_s).toString();
        p.botAvatarUrl = o.value(u"botAvatar"_s).toString();
        if (!p.isEmpty())
            out.insert(key, std::move(p));
    }
    return out;
}

void WorkspaceCache::saveDeadConvIds(const QStringList &ids) {
    metaObject()[u"deadConvIds"_s] = QJsonArray::fromStringList(ids);
    writeMeta();
}

QStringList WorkspaceCache::loadDeadConvIds() const {
    QStringList out;
    for (const auto &v : metaObject().value(u"deadConvIds"_s).toArray())
        out.append(v.toString());
    return out;
}

void WorkspaceCache::saveUserProbeTimes(const QHash<QString, qint64> &byUserId) {
    QJsonObject o;
    for (auto it = byUserId.constBegin(); it != byUserId.constEnd(); ++it)
        o[it.key()] = QJsonValue(qint64(it.value()));
    metaObject()[u"userProbeTimes"_s] = o;
    writeMeta();
}

QHash<QString, qint64> WorkspaceCache::loadUserProbeTimes() const {
    QHash<QString, qint64> out;
    const QJsonObject      o = metaObject().value(u"userProbeTimes"_s).toObject();
    for (auto it = o.constBegin(); it != o.constEnd(); ++it)
        out.insert(it.key(), it.value().toVariant().toLongLong());
    return out;
}

void WorkspaceCache::saveEmojiMap(const QHash<QString, QString> &map) {
    QJsonObject o;
    for (auto it = map.constBegin(); it != map.constEnd(); ++it)
        o[it.key()] = it.value();
    writeJson(emojiPath(), QJsonDocument(o));
}

QHash<QString, QString> WorkspaceCache::loadEmojiMap() const {
    const auto data = readFile(emojiPath());
    if (data.isEmpty())
        return {};
    const auto doc = QJsonDocument::fromJson(data);
    if (!doc.isObject())
        return {};
    QHash<QString, QString> result;
    const auto              obj = doc.object();
    for (auto it = obj.constBegin(); it != obj.constEnd(); ++it)
        result[it.key()] = it.value().toString();
    return result;
}

void WorkspaceCache::saveImage(const QString &url, const QByteArray &data) {
    if (data.isEmpty())
        return;
    if (writeFile(imgPath(url), data))
        CacheEvictor::noteBytesWritten(data.size());
}

QByteArray WorkspaceCache::loadImage(const QString &url) const {
    const QString path = imgPath(url);
    const auto    data = readFile(path);
    if (data.isEmpty())
        return data;
    // LRU bookkeeping for CacheEvictor: a blob's mtime is its last-used time.
    // Bumped at most hourly — finer grain isn't worth a write per read.
    const auto now = QDateTime::currentDateTimeUtc();
    if (QFileInfo(path).lastModified().secsTo(now) > 3600) {
        QFile f(path);
        if (f.open(QIODevice::ReadWrite))
            f.setFileTime(now, QFileDevice::FileModificationTime);
    }
    return data;
}
