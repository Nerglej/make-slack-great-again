#include "app/model/backend.h"

namespace model {

// The defaults of the calls a backend may lack (see the header).

void Backend::edit(ConvRef, Ts, std::string) {}
void Backend::remove(ConvRef, Ts) {}
void Backend::react(ConvRef, Ts, std::string_view, bool) {}
void Backend::markUnread(ConvRef, Ts) {}
void Backend::setStarred(ConvRef, bool) {}
void Backend::setMuted(ConvRef, bool) {}
void Backend::setNotifyLevel(ConvRef, NotifyLevel) {}
void Backend::leave(ConvRef) {}

void Backend::openDm(UserRef, std::function<void(ConvRef)> done) {
    if (done)
        done(kNoConv);
}

void Backend::setPinned(ConvRef, Ts, bool) {}
void Backend::setSaved(ConvRef, Ts, bool) {}
void Backend::deleteFile(ConvRef, Ts, const std::string &) {}
void Backend::setReminder(ConvRef, Ts, int64_t) {}
void Backend::userTyping(ConvRef, Ts) {}

// A reply sent to the channel too sits in the channel's list without its
// thread (mapPage drops threadTs there): only the service knows its root.
void Backend::resolveThreadRoot(ConvRef conv, Ts ts, std::function<void(Ts)> done) {
    if (const Message *m = _store.findMessage(conv, ts); m && m->subtype() != "thread_broadcast") {
        if (done)
            done(m->threadTs ? m->threadTs : m->ts);
        return;
    }
    loadMessage(conv, ts, [done = std::move(done)](bool ok, Message m) {
        if (done)
            done(!ok ? 0 : m.threadTs ? m.threadTs : m.ts);
    });
}

std::string Backend::selfName() const {
    return std::string(_store.user(_store.me).label());
}

void Backend::search(std::string, std::function<void(std::vector<SearchHit>)> done) {
    if (done)
        done({});
}

} // namespace model
