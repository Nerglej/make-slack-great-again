#include "screens/shell/standin.h"

#include "app/mrkdwn/emoji.h"
#include "app/mrkdwn/mrkdwn.h"
#include "base/i18n.h"
#include "base/str.h"
#include "base/time.h"

using namespace ui;
using i18n::tr;

namespace shell {

std::string plainText(const model::Store &store, std::string_view text) {
    const mrkdwn::Rich r   = mrkdwn::parse(text);
    // Entities keep their raw form in the text (":name:", "@U0…", "#C0…"):
    // swap in glyphs and names (msga's notificationText), back to front.
    std::string        out = r.text;
    for (size_t i = r.entities.size(); i-- > 0;) {
        const auto &e = r.entities[i];
        std::string to;
        switch (e.kind) {
        case mrkdwn::Kind::Emoji:
            to = store.emojiFor(e.data).unicode;
            break;
        case mrkdwn::Kind::User:
            if (const model::UserRef u = store.findUser(e.data);
                u != model::kNoUser && !store.user(u).label().empty())
                to = "@" + std::string(store.user(u).label());
            break;
        case mrkdwn::Kind::Channel:
            if (const model::ConvRef c = store.findConversation(e.data);
                c != model::kNoConv && !store.conversation(c).name.empty())
                to = "#" + store.conversation(c).name;
            else if (const std::string *n = store.channelName(e.data); n && !n->empty())
                to = "#" + *n;
            break;
        case mrkdwn::Kind::Usergroup:
            if (const model::Store::Usergroup *g = store.findUsergroup(e.data))
                to = "@" + (g->handle.empty() ? g->name : g->handle);
            break;
        default:
            break;
        }
        if (!to.empty())
            out.replace(e.start, e.length, to);
    }
    return out;
}

MessageStandIn::MessageStandIn(screens::Context &ctx, Avatars &avatars)
    : _ctx(ctx), _avatars(avatars) {
    setBackground(C::Surface);
    content()->style().padding(0, 12, 0, 12).spacing(2);
    _observer = ctx.store.observe(model::Store::kAnyConv, [this](const model::Change &ch) {
        if (ch.conv == _conv && ch.kind != model::ChangeKind::Typing &&
            ch.kind != model::ChangeKind::Meta)
            rebuild();
    });
}

MessageStandIn::~MessageStandIn() {
    _ctx.store.unobserve(_observer);
}

void MessageStandIn::setTarget(model::ConvRef conv, model::Ts thread) {
    _conv   = conv;
    _thread = thread;
    rebuild();
}

void MessageStandIn::rebuild() {
    View *list = content();
    list->clearChildren();
    if (_conv == model::kNoConv)
        return;
    const auto                         &store = _ctx.store();
    std::vector<const model::Message *> msgs;
    if (_thread) {
        if (const auto *root = store.findMessage(_conv, _thread))
            msgs.push_back(root);
        if (const auto *replies = store.replies(_conv, _thread))
            for (const auto &m : *replies)
                msgs.push_back(&m);
    } else {
        for (const auto &m : store.conversation(_conv).messages)
            msgs.push_back(&m);
    }
    const int64_t now = base::nowSecs();
    for (const model::Message *m : msgs) {
        auto *row = list->add<View>();
        row->style().row().padding(20, 6).spacing(10).items(Align::Start);
        auto *img = row->add<Avatar>();
        img->style().size(36, 36);
        img->setRadius(6);
        const auto &u = store.user(m->user);
        img->setBitmap(_avatars.get(
            m->extra && !m->extra->botAvatar.empty() ? m->extra->botAvatar : u.avatar, 72
        ));
        auto *col = row->add<View>();
        col->style().flex(1).spacing(2);
        auto *head = col->add<View>();
        head->style().row().spacing(8).items(Align::Center);
        head->add<Label>(
            m->extra && !m->extra->botName.empty() ? m->extra->botName : std::string(u.label()),
            Font::BodyBold
        );
        head->add<Label>(base::dateTimeLabel(model::tsSecs(m->ts), now), Font::Small, C::TextFaint);
        std::string body = plainText(store, m->text);
        for (const auto &a : m->attachments())
            body += str::concat(
                {body.empty() ? "" : "\n", "\xE2\x96\x8E ", a.title.empty() ? a.link : a.title}
            );
        for (const auto &f : m->files())
            body += str::concat({body.empty() ? "" : "\n", "\xF0\x9F\x93\x8E ", f.name});
        if (m->pending)
            col->setEnabled(false);
        col->add<Label>(std::move(body), Font::Body, m->pending ? C::TextMuted : C::Text);
        if (!_thread && m->replyCount) {
            auto *replies = col->add<Button>(
                m->replyCount == 1 ? std::string(tr("1 reply"))
                                   : i18n::arg(tr("%1 replies"), str::number(m->replyCount)),
                Button::Kind::Ghost
            );
            replies->setTextColor(C::Link);
            replies->style().alignSelf(Align::Start);
            const model::Ts root = m->ts;
            replies->onClick     = [this, root] {
                if (_ctx.openThread)
                    _ctx.openThread(_conv, root);
            };
        }
    }
    // New messages: follow the end like a chat.
    scrollTo(1e9f);
}

} // namespace shell
