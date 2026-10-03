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

void Backend::search(std::string, std::function<void(std::vector<SearchHit>)> done) {
    if (done)
        done({});
}

} // namespace model
