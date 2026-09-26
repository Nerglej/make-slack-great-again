// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "voice_context.h"

#include "session/session.h"

#include <QSet>

namespace {

// A single message beyond this is cut: the context is there for vocabulary and
// names, and one pasted log must not crowd out the rest of the conversation.
constexpr int kMaxMessageChars = 500;
// Members beyond this add little and cost prompt space on huge channels.
constexpr int kMaxMembers      = 60;

QString userLabel(const Session *session, const UserId &id) {
    if (!session || id.value.isEmpty())
        return {};
    const User *u = session->findUser(id);
    return u ? u->displayLabel() : QString();
}

QString authorLabel(const Session *session, const Message &m) {
    if (!m.botName.isEmpty())
        return m.botName;
    return userLabel(session, m.author);
}

// Rows that are people talking (a bot's post, a broadcast reply) as opposed
// to "joined the channel" style system rows.
bool isConversational(const Message &m) {
    if (!m.subtype)
        return true;
    static const QSet<QString> kTalking{
        QStringLiteral("bot_message"),
        QStringLiteral("thread_broadcast"),
        QStringLiteral("me_message"),
        QStringLiteral("file_share"),
    };
    return kTalking.contains(*m.subtype);
}

// The message's plain text with @mentions resolved to names (back to front so
// the entity offsets stay valid), the way the summary transcript does it.
QString plainText(const Session *session, const Message &m) {
    QString text = m.text.text;
    for (auto it = m.text.entities.rbegin(); it != m.text.entities.rend(); ++it) {
        if (it->type != EntityType::UserMention)
            continue;
        const QString name = userLabel(session, UserId{it->data});
        if (!name.isEmpty() && it->offset >= 0 && it->offset + it->length <= text.size())
            text.replace(it->offset, it->length, QStringLiteral("@") + name);
    }
    text = text.trimmed();
    if (text.size() > kMaxMessageChars)
        text = text.left(kMaxMessageChars) + QStringLiteral("…");
    return text;
}

} // namespace

Voice::Context VoiceContext::build(
    const Session              *session,
    const ConversationId       &conv,
    const std::vector<Message> &messages,
    int                         maxMessages
) {
    Voice::Context ctx;

    // ── Recent messages, newest maxMessages, kept oldest → newest ──────────
    QStringList lines;
    QStringList authors; // newest first: who is talking now matters most
    for (auto it = messages.rbegin(); it != messages.rend() && lines.size() < maxMessages; ++it) {
        const Message &m = *it;
        if (m.pending || !isConversational(m))
            continue;
        const QString text = plainText(session, m);
        if (text.isEmpty())
            continue;
        const QString who = authorLabel(session, m);
        lines.prepend(who.isEmpty() ? text : who + QStringLiteral(": ") + text);
        if (!who.isEmpty() && !authors.contains(who))
            authors << who;
    }
    ctx.recentMessages = lines;

    // ── Conversation name + members ────────────────────────────────────────
    QStringList members   = authors;
    const auto  addMember = [&members](const QString &name) {
        if (!name.isEmpty() && !members.contains(name) && members.size() < kMaxMembers)
            members << name;
    };
    const Conversation *c = session ? session->findConversation(conv) : nullptr;
    if (c) {
        switch (c->kind) {
        case ConvKind::Im:
            if (c->dmUser)
                ctx.conversationName = userLabel(session, *c->dmUser);
            if (ctx.conversationName.isEmpty())
                ctx.conversationName = c->name;
            addMember(ctx.conversationName);
            break;
        case ConvKind::Mpim: {
            QStringList names;
            for (const UserId &id : c->members) {
                if (session && id == session->meUserId())
                    continue;
                const QString n = userLabel(session, id);
                if (!n.isEmpty())
                    names << n;
                addMember(n);
            }
            ctx.conversationName = names.isEmpty() ? c->name : names.join(QStringLiteral(", "));
            break;
        }
        default:
            ctx.conversationName = c->name.isEmpty() ? QString() : QStringLiteral("#") + c->name;
            // Only a list some earlier view already loaded; no members fetch.
            if (const auto *ids = session->cachedMembers(conv))
                for (const UserId &id : *ids)
                    addMember(userLabel(session, id));
            break;
        }
    }
    ctx.memberNames = members;
    return ctx;
}
