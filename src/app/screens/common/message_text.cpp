#include "screens/common/message_text.h"

#include "app/mrkdwn/emoji.h"
#include "base/file.h"

namespace screens {

std::string entityText(const model::Store &store, const mrkdwn::Entity &e, uint8_t skinTone) {
    switch (e.kind) {
    case mrkdwn::Kind::User:
        if (const model::UserRef u = store.findUser(e.data);
            u != model::kNoUser && !store.user(u).label().empty())
            return "@" + std::string(store.user(u).label());
        break;
    case mrkdwn::Kind::Channel:
        if (const model::ConvRef c = store.findConversation(e.data);
            c != model::kNoConv && !store.conversation(c).name.empty())
            return "#" + store.conversation(c).name;
        else if (const std::string *n = store.channelName(e.data); n && !n->empty())
            return "#" + *n;
        break;
    case mrkdwn::Kind::Usergroup:
        // A user group mention: the live handle (else name).
        if (const model::Store::Usergroup *g = store.findUsergroup(e.data))
            return "@" + (g->handle.empty() ? g->name : g->handle);
        break;
    case mrkdwn::Kind::Emoji:
        if (std::string u = store.emojiFor(e.data).unicode; !u.empty())
            return skinTone ? emoji::applySkinTone(u, skinTone) : u;
        break;
    default:
        break;
    }
    return {};
}

std::string plainText(const model::Store &store, std::string_view text) {
    const mrkdwn::Rich r   = mrkdwn::parse(text);
    // Entities keep their raw form in the text (":name:", "@U0…", "#C0…").
    // Only leaf entities resolve to anything, so swapping back to front keeps
    // every earlier offset valid.
    std::string        out = r.text;
    for (size_t i = r.entities.size(); i-- > 0;) {
        const mrkdwn::Entity &e = r.entities[i];
        if (std::string to = entityText(store, e); !to.empty())
            out.replace(e.start, e.length, to);
    }
    return out;
}

std::string firstLink(std::string_view text) {
    const mrkdwn::Rich r = mrkdwn::parse(text);
    for (const mrkdwn::Entity &e : r.entities) {
        if (e.kind == mrkdwn::Kind::Link && !e.data.empty())
            return e.data;
        // A link to another message is a link too: its entity holds the ref,
        // so rebuild the permalink the user sees.
        if (e.kind == mrkdwn::Kind::MessageLink)
            if (std::string url = mrkdwn::messagePermalink(mrkdwn::refFromToken(e.data));
                !url.empty())
                return url;
    }
    return {};
}

std::string fileUrl(const std::string &path) {
    return path.find("://") != std::string::npos ? path : file::toFileUrl(path);
}

} // namespace screens
