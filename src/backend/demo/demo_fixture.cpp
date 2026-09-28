// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "demo_fixture.h"

#include "text/mrkdwn_parser.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <algorithm>

using namespace Qt::StringLiterals;

namespace demo {

namespace {

QString prettyTypeFor(const QString &mime, const QString &name) {
    static const QHash<QString, QString> known = {
        {u"image/png"_s, u"PNG"_s},
        {u"image/jpeg"_s, u"JPEG"_s},
        {u"image/gif"_s, u"GIF"_s},
        {u"image/webp"_s, u"WebP"_s},
        {u"image/svg+xml"_s, u"SVG"_s},
        {u"application/pdf"_s, u"PDF"_s},
        {u"text/csv"_s, u"CSV"_s},
        {u"text/plain"_s, u"Plain text"_s},
        {u"application/zip"_s, "Zip"},
    };
    if (const auto it = known.constFind(mime); it != known.constEnd())
        return *it;
    const QString suffix = QFileInfo(name).suffix();
    return suffix.isEmpty() ? QStringLiteral("File") : suffix.toUpper();
}

File fileFrom(const QJsonObject &o, const QString &dir, int index) {
    const QString rel = o.value(u"path"_s).toString();
    File          f   = fileFromLocalPath(QDir(dir).absoluteFilePath(rel), index);
    if (o.contains(u"name"_s))
        f.name = o.value(u"name"_s).toString();
    if (o.contains(u"mime"_s))
        f.mimeType = o.value(u"mime"_s).toString();
    if (o.contains(u"type"_s))
        f.prettyType = o.value(u"type"_s).toString();
    if (o.value(u"width"_s).toInt() > 0 && o.value(u"height"_s).toInt() > 0) {
        f.imageWidth  = o.value(u"width"_s).toInt();
        f.imageHeight = o.value(u"height"_s).toInt();
        f.thumbs      = {FileThumb{f.imageWidth, f.imageHeight, f.urlPrivate}};
    }
    // Audio: a voice clip ("subtype": "slack_audio") with its transcript, or a
    // plain upload with just a duration.
    f.subtype    = o.value(u"subtype"_s).toString();
    f.durationMs = qint64(o.value(u"durationMs"_s).toDouble());
    if (const QString t = o.value(u"transcript"_s).toString(); !t.isEmpty()) {
        f.transcriptStatus  = QStringLiteral("complete");
        f.transcriptPreview = t;
    }
    return f;
}

Attachment attachmentFrom(const QJsonObject &o, const QString &dir) {
    Attachment a;
    a.fallback   = o.value(u"title"_s).toString();
    a.color      = o.value(u"color"_s).toString();
    a.pretext    = o.value(u"pretext"_s).toString();
    a.authorName = o.value(u"author"_s).toString();
    a.title      = o.value(u"title"_s).toString();
    a.titleLink  = o.value(u"link"_s).toString();
    a.text       = MrkdwnParser::parse(o.value(u"text"_s).toString());
    a.footer     = o.value(u"footer"_s).toString(o.value(u"service"_s).toString());
    if (const QString fav = o.value(u"favicon"_s).toString(); !fav.isEmpty()) {
        a.faviconUrl = assetUrl(dir, fav);
        a.footerIcon = a.faviconUrl;
    }
    if (const QString img = o.value(u"image"_s).toString(); !img.isEmpty()) {
        a.imageUrl     = assetUrl(dir, img);
        const QSize sz = QImageReader(QDir(dir).absoluteFilePath(img)).size();
        a.imageWidth   = o.value(u"width"_s).toInt(sz.width());
        a.imageHeight  = o.value(u"height"_s).toInt(sz.height());
    }
    for (const auto &fv : o.value(u"fields"_s).toArray()) {
        const auto fo = fv.toObject();
        a.fields.push_back(
            AttachmentField{
                fo.value(u"title"_s).toString(),
                MrkdwnParser::parse(fo.value(u"value"_s).toString())
            }
        );
    }
    a.isLinkPreview = o.value(u"linkPreview"_s).toBool(!a.titleLink.isEmpty() && a.fields.empty());
    return a;
}

std::vector<Reaction> reactionsFrom(const QJsonArray &arr) {
    std::vector<Reaction> out;
    for (const auto &rv : arr) {
        const auto ro = rv.toObject();
        Reaction   r;
        r.name = ro.value(u"name"_s).toString();
        for (const auto &u : ro.value(u"users"_s).toArray())
            r.users.push_back(UserId{u.toString()});
        r.count = int(r.users.size());
        if (!r.name.isEmpty() && r.count > 0)
            out.push_back(std::move(r));
    }
    return out;
}

int tzOffsetSeconds(const QString &spec, bool *ok) {
    // "+02:00" / "-05:30" / "Z"
    *ok = true;
    if (spec.isEmpty() || spec == QLatin1String("Z"))
        return 0;
    static const QRegularExpression re(uR"(^([+-])(\d{1,2}):?(\d{2})?$)"_s);
    const auto                      m = re.match(spec);
    if (!m.hasMatch()) {
        *ok = false;
        return 0;
    }
    const int sign = m.captured(1) == QLatin1String("-") ? -1 : 1;
    return sign * (m.captured(2).toInt() * 3600 + m.captured(3).toInt() * 60);
}

struct TsAllocator {
    // Guarantees strictly unique, order-preserving ts strings per conversation.
    QSet<QString> used;
    Ts            allocate(const QDateTime &when) {
        for (int off = 0;; ++off) {
            const Ts ts = tsFor(when, off);
            if (!used.contains(ts)) {
                used.insert(ts);
                return ts;
            }
        }
    }
};

} // namespace

QString assetUrl(const QString &dir, const QString &rel) {
    if (rel.isEmpty())
        return {};
    if (rel.contains(QLatin1String("://")))
        return rel; // already a URL (remote asset — discouraged, but allowed)
    return QUrl::fromLocalFile(QDir(dir).absoluteFilePath(rel)).toString();
}

File fileFromLocalPath(const QString &absPath, int index) {
    const QString url = QUrl::fromLocalFile(absPath).toString();
    File          f;
    f.id                 = QStringLiteral("FDEMO%1").arg(index, 4, 10, QLatin1Char('0'));
    f.name               = QFileInfo(absPath).fileName();
    f.mimeType           = QMimeDatabase().mimeTypeForFile(absPath).name();
    f.prettyType         = prettyTypeFor(f.mimeType, f.name);
    f.urlPrivate         = url;
    f.urlPrivateDownload = url;
    f.thumbUrl           = url;
    f.size               = QFileInfo(absPath).size();
    if (f.mimeType.startsWith(u"image/"_s)) {
        const QSize sz = QImageReader(absPath).size();
        f.imageWidth   = sz.width();
        f.imageHeight  = sz.height();
        if (f.imageWidth > 0)
            f.thumbs.push_back(FileThumb{f.imageWidth, f.imageHeight, url});
    }
    return f;
}

Ts tsFor(const QDateTime &when, int microOffset) {
    const qint64 micros = when.toMSecsSinceEpoch() * 1000 + microOffset;
    return QStringLiteral("%1.%2")
        .arg(micros / 1000000)
        .arg(micros % 1000000, 6, 10, QLatin1Char('0'));
}

QDateTime parseTimeSpec(const QString &spec, const QDateTime &now, const QDateTime &prev) {
    const QString                   s = spec.trimmed();
    // Relative: "-45m" (before now) / "+7m" (after prev).
    static const QRegularExpression rel(uR"(^([+-])(\d+)([smhd])$)"_s);
    if (const auto m = rel.match(s); m.hasMatch()) {
        qint64 secs = m.captured(2).toLongLong();
        switch (m.captured(3).at(0).toLatin1()) {
        case 'm':
            secs *= 60;
            break;
        case 'h':
            secs *= 3600;
            break;
        case 'd':
            secs *= 86400;
            break;
        default:
            break;
        }
        if (m.captured(1) == QLatin1String("-"))
            return now.addSecs(-secs);
        return (prev.isValid() ? prev : now).addSecs(secs);
    }
    // Wall clock: "[-Nd] HH:MM"
    static const QRegularExpression wall(uR"(^(?:(-?\d+)d\s+)?(\d{1,2}):(\d{2})$)"_s);
    if (const auto m = wall.match(s); m.hasMatch()) {
        const int   days = m.captured(1).isEmpty() ? 0 : m.captured(1).toInt();
        const QTime t(m.captured(2).toInt(), m.captured(3).toInt());
        if (!t.isValid())
            return {};
        return QDateTime(now.date().addDays(days), t, now.timeZone());
    }
    return {};
}

std::optional<Fixture> loadFixture(const QString &path, QString *error, const QDateTime &now) {
    auto fail = [error](const QString &why) -> std::optional<Fixture> {
        if (error)
            *error = why;
        return std::nullopt;
    };

    QFileInfo info(path);
    if (info.isDir())
        info = QFileInfo(QDir(path).filePath(QStringLiteral("fixture.json")));
    QFile f(info.absoluteFilePath());
    if (!f.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("cannot open %1").arg(info.absoluteFilePath()));
    QJsonParseError perr;
    const auto      doc = QJsonDocument::fromJson(f.readAll(), &perr);
    if (doc.isNull() || !doc.isObject())
        return fail(QStringLiteral("%1: %2").arg(info.fileName(), perr.errorString()));
    const QJsonObject root = doc.object();

    Fixture fx;
    fx.dir = info.absolutePath();

    const auto ws    = root.value(u"workspace"_s).toObject();
    fx.workspaceId   = ws.value(u"id"_s).toString(QStringLiteral("DEMO"));
    fx.workspaceName = ws.value(u"name"_s).toString(QStringLiteral("Demo workspace"));
    fx.workspaceIcon = assetUrl(fx.dir, ws.value(u"icon"_s).toString());
    fx.me            = UserId{root.value(u"me"_s).toString()};
    if (fx.me.value.isEmpty())
        return fail(QStringLiteral("\"me\" is required"));

    // ── users ──
    QSet<QString> userIds;
    for (const auto &uv : root.value(u"users"_s).toArray()) {
        const auto o = uv.toObject();
        User       u;
        u.id          = UserId{o.value(u"id"_s).toString()};
        u.name        = o.value(u"name"_s).toString();
        u.displayName = o.value(u"displayName"_s).toString();
        u.avatarUrl   = assetUrl(fx.dir, o.value(u"avatar"_s).toString());
        u.isBot       = o.value(u"bot"_s).toBool();
        u.isActive    = o.value(u"active"_s).toBool();
        u.isAdmin     = o.value(u"admin"_s).toBool();
        u.isOwner     = o.value(u"owner"_s).toBool();
        u.title       = o.value(u"title"_s).toString();
        u.email       = o.value(u"email"_s).toString();
        u.dndEnabled  = o.value(u"dnd"_s).toBool();
        u.teamId      = fx.workspaceId;
        const auto st = o.value(u"status"_s).toObject();
        u.statusEmoji = st.value(u"emoji"_s).toString();
        u.statusText  = st.value(u"text"_s).toString();
        if (o.contains(u"tz"_s)) {
            bool ok    = false;
            u.tzOffset = tzOffsetSeconds(o.value(u"tz"_s).toString(), &ok);
            if (!ok)
                return fail(QStringLiteral("user %1: bad tz %2")
                                .arg(u.id.value, o.value(u"tz"_s).toString()));
            u.hasTz = true;
        }
        if (u.id.value.isEmpty() || u.name.isEmpty())
            return fail(QStringLiteral("every user needs an id and a name"));
        userIds.insert(u.id.value);
        fx.users.push_back(std::move(u));
    }
    if (!userIds.contains(fx.me.value))
        return fail(QStringLiteral("\"me\" (%1) is not in users").arg(fx.me.value));

    // ── conversations ──
    QHash<QString, int> convIndex;
    for (const auto &cv : root.value(u"conversations"_s).toArray()) {
        const auto    o    = cv.toObject();
        const QString kind = o.value(u"kind"_s).toString(QStringLiteral("channel"));
        Conversation  c;
        c.id = ConversationId{o.value(u"id"_s).toString()};
        if (c.id.value.isEmpty())
            return fail(QStringLiteral("every conversation needs an id"));
        if (kind == QLatin1String("channel"))
            c.kind = ConvKind::PublicChannel;
        else if (kind == QLatin1String("private"))
            c.kind = ConvKind::PrivateChannel;
        else if (kind == QLatin1String("dm"))
            c.kind = ConvKind::Im;
        else if (kind == QLatin1String("group"))
            c.kind = ConvKind::Mpim;
        else
            return fail(QStringLiteral("conversation %1: unknown kind %2").arg(c.id.value, kind));
        c.name         = o.value(u"name"_s).toString();
        c.description  = o.value(u"topic"_s).toString();
        c.isMember     = o.value(u"member"_s).toBool(true);
        c.memberCount  = o.value(u"memberCount"_s).toInt();
        c.unread       = o.value(u"unread"_s).toInt();
        c.mentionCount = o.value(u"mentions"_s).toInt();
        c.isStarred    = o.value(u"starred"_s).toBool();
        c.isMuted      = o.value(u"muted"_s).toBool();
        for (const auto &m : o.value(u"members"_s).toArray())
            c.members.push_back(UserId{m.toString()});
        if (c.kind == ConvKind::Im) {
            const QString peer = o.value(u"user"_s).toString();
            if (!userIds.contains(peer))
                return fail(QStringLiteral("dm %1: unknown user %2").arg(c.id.value, peer));
            c.dmUser = UserId{peer};
            if (c.name.isEmpty())
                for (const auto &u : fx.users)
                    if (u.id.value == peer)
                        c.name = u.name;
        }
        if (c.kind == ConvKind::Mpim) {
            if (std::find(c.members.begin(), c.members.end(), fx.me) == c.members.end())
                c.members.push_back(fx.me);
            if (c.name.isEmpty()) {
                QStringList names;
                for (const auto &m : c.members)
                    for (const auto &u : fx.users)
                        if (u.id == m)
                            names << u.name;
                c.name = QStringLiteral("mpdm-%1-1").arg(names.join(QStringLiteral("--")));
            }
        }
        if (c.memberCount == 0 && !c.members.empty())
            c.memberCount = int(c.members.size());
        if (o.contains(u"canvas"_s)) {
            const auto cv = o.value(u"canvas"_s).toObject();
            Canvas     canvas;
            canvas.conv   = c.id.value;
            canvas.fileId = QStringLiteral("F0CANVAS-%1").arg(c.id.value);
            canvas.title  = cv.value(u"title"_s).toString();
            QFile html(QDir(fx.dir).filePath(cv.value(u"html"_s).toString()));
            if (cv.value(u"html"_s).toString().isEmpty() || !html.open(QIODevice::ReadOnly))
                return fail(QStringLiteral("conversation %1: canvas needs a readable \"html\" file")
                                .arg(c.id.value));
            canvas.html    = QString::fromUtf8(html.readAll());
            c.canvasFileId = canvas.fileId;
            fx.canvases.push_back(std::move(canvas));
        }
        convIndex.insert(c.id.value, int(fx.conversations.size()));
        fx.conversations.push_back(std::move(c));
    }

    // ── messages ──
    std::unordered_map<QString, TsAllocator> alloc;
    std::unordered_map<QString, QDateTime>   prevRoot; // conv → time of the previous root
    int                                      fileIndex = 0;

    auto parseMessage = [&](const QJsonObject &o,
                            const QString     &conv,
                            const QDateTime   &prev,
                            int                index,
                            Message           &out,
                            QDateTime         &when) -> QString {
        when = parseTimeSpec(o.value(u"time"_s).toString(), now, prev);
        if (!when.isValid())
            return QStringLiteral("message %1 in %2: bad time %3")
                .arg(index)
                .arg(conv, o.value(u"time"_s).toString());
        out.ts         = alloc[conv].allocate(when);
        out.date       = decimalTsToMicros(out.ts);
        const auto bot = o.value(u"bot"_s).toObject();
        if (!bot.isEmpty()) {
            out.subtype      = QStringLiteral("bot_message");
            out.botName      = bot.value(u"name"_s).toString();
            out.botAvatarUrl = assetUrl(fx.dir, bot.value(u"avatar"_s).toString());
        }
        const QString user = o.value(u"user"_s).toString();
        if (!user.isEmpty()) {
            if (!userIds.contains(user))
                return QStringLiteral("message %1 in %2: unknown user %3")
                    .arg(index)
                    .arg(conv, user);
            out.author = UserId{user};
        } else if (bot.isEmpty()) {
            return QStringLiteral("message %1 in %2: needs a user or a bot").arg(index).arg(conv);
        }
        if (o.contains(u"subtype"_s))
            out.subtype = o.value(u"subtype"_s).toString();
        out.rawText   = o.value(u"text"_s).toString();
        out.text      = MrkdwnParser::parse(out.rawText);
        out.reactions = reactionsFrom(o.value(u"reactions"_s).toArray());
        out.edited    = o.value(u"edited"_s).toBool();
        out.pinned    = o.value(u"pinned"_s).toBool();
        if (out.pinned)
            out.pinnedBy = fx.me;
        for (const auto &fv : o.value(u"files"_s).toArray())
            out.files.push_back(fileFrom(fv.toObject(), fx.dir, ++fileIndex));
        for (const auto &av : o.value(u"attachments"_s).toArray())
            out.attachments.push_back(attachmentFrom(av.toObject(), fx.dir));
        return {};
    };

    int index = 0;
    for (const auto &mv : root.value(u"messages"_s).toArray()) {
        ++index;
        const auto    o    = mv.toObject();
        const QString conv = o.value(u"conv"_s).toString();
        if (!convIndex.contains(conv))
            return fail(QStringLiteral("message %1: unknown conversation %2").arg(index).arg(conv));

        Message   msg;
        QDateTime when;
        if (const QString err = parseMessage(o, conv, prevRoot[conv], index, msg, when);
            !err.isEmpty())
            return fail(err);
        prevRoot[conv] = when;

        const auto replies = o.value(u"replies"_s).toArray();
        if (!replies.isEmpty()) {
            QDateTime            prev = when;
            std::vector<Message> thread;
            int                  ri = 0;
            for (const auto &rv : replies) {
                ++ri;
                Message   reply;
                QDateTime rwhen;
                if (const QString err = parseMessage(rv.toObject(), conv, prev, ri, reply, rwhen);
                    !err.isEmpty())
                    return fail(QStringLiteral("message %1, reply %2").arg(index).arg(err));
                if (rwhen < when)
                    return fail(QStringLiteral("message %1: reply %2 is dated before its root")
                                    .arg(index)
                                    .arg(ri));
                prev               = rwhen;
                reply.threadRoot   = msg.ts;
                reply.parentUserId = msg.author;
                if (!reply.author.value.isEmpty() &&
                    std::find(msg.replyUsers.begin(), msg.replyUsers.end(), reply.author) ==
                        msg.replyUsers.end() &&
                    msg.replyUsers.size() < 5)
                    msg.replyUsers.push_back(reply.author);
                thread.push_back(std::move(reply));
            }
            msg.replyCount                                               = int(thread.size());
            msg.latestReply                                              = thread.back().ts;
            fx.threads[Fixture::threadKey(ConversationId{conv}, msg.ts)] = std::move(thread);
        }
        fx.history[conv].push_back(std::move(msg));
    }

    // Oldest first, then derive per-conversation cursors from the result.
    for (auto &c : fx.conversations) {
        auto &msgs = fx.history[c.id.value];
        std::stable_sort(msgs.begin(), msgs.end(), MessageDateLess{});
        const int n = int(msgs.size());
        c.latestTs  = n ? msgs.back().ts : Ts();
        if (c.unread > 0 && c.unread < n)
            c.lastRead = msgs[n - c.unread - 1].ts;
        else if (c.unread > 0)
            c.lastRead = QStringLiteral("0");
        else
            c.lastRead = n ? msgs.back().ts : QStringLiteral("0");
    }

    // ── auto replies ──
    for (const auto &av : root.value(u"autoReplies"_s).toArray()) {
        const auto o = av.toObject();
        AutoReply  r;
        r.conv     = o.value(u"conv"_s).toString();
        r.user     = UserId{o.value(u"user"_s).toString()};
        r.text     = o.value(u"text"_s).toString();
        r.afterMs  = o.value(u"afterMs"_s).toInt(r.afterMs);
        r.typingMs = o.value(u"typingMs"_s).toInt(r.typingMs);
        r.inThread = o.value(u"inThread"_s).toBool();
        if (!convIndex.contains(r.conv) || !userIds.contains(r.user.value) || r.text.isEmpty())
            return fail(QStringLiteral("autoReplies: each needs a known conv, user and a text"));
        fx.autoReplies.push_back(std::move(r));
    }

    // ── unfurls / ai ──
    for (const auto &uv : root.value(u"unfurls"_s).toArray()) {
        const auto o = uv.toObject();
        Unfurl     u;
        u.url = o.value(u"url"_s).toString();
        if (u.url.isEmpty())
            return fail(QStringLiteral("unfurls: each needs a url"));
        u.attachment               = attachmentFrom(o, fx.dir);
        u.attachment.isLinkPreview = true;
        if (u.attachment.titleLink.isEmpty())
            u.attachment.titleLink = u.url;
        fx.unfurls.push_back(std::move(u));
    }
    const auto ai = root.value(u"ai"_s).toObject();
    fx.aiDefault  = ai.value(u"default"_s).toString();
    for (const auto &rv : ai.value(u"replies"_s).toArray()) {
        const auto o = rv.toObject();
        AiReply    r{o.value(u"match"_s).toString(), o.value(u"text"_s).toString()};
        if (r.match.isEmpty() || r.text.isEmpty())
            return fail(QStringLiteral("ai.replies: each needs match and text"));
        fx.aiReplies.push_back(std::move(r));
    }

    const QString start = root.value(u"startConversation"_s).toString();
    if (!start.isEmpty() && !convIndex.contains(start))
        return fail(QStringLiteral("startConversation %1 is not a conversation").arg(start));
    fx.startConversation = ConversationId{
        start.isEmpty() ? (fx.conversations.empty() ? QString() : fx.conversations.front().id.value)
                        : start
    };
    return fx;
}

} // namespace demo
