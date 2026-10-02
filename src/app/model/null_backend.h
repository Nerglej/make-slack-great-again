// NullBackend — no workspace signed in: the Store stays empty and every call
// is a no-op. The shell runs on it while it shows the "Log in to workspace"
// page (the old app's logged-out state).
#pragma once

#include "app/model/backend.h"

namespace model {

class NullBackend : public Backend {
public:
    using Backend::Backend;

    void connect(Done) override {}
    void loadHistory(ConvRef, Ts, Done) override {}
    void loadThread(ConvRef, Ts, Done) override {}
    void send(ConvRef, std::string, Ts, Done) override {}
    void edit(ConvRef, Ts, std::string) override {}
    void remove(ConvRef, Ts) override {}
    void react(ConvRef, Ts, std::string_view, bool) override {}
    void markRead(ConvRef, Ts) override {}
    void markUnread(ConvRef, Ts) override {}
    void setStarred(ConvRef, bool) override {}
    void setMuted(ConvRef, bool) override {}
    void setNotifyLevel(ConvRef, NotifyLevel) override {}
    void leave(ConvRef) override {}
    void openDm(UserRef, std::function<void(ConvRef)> done) override {
        if (done)
            done(kNoConv);
    }
    void setPinned(ConvRef, Ts, bool) override {}
    void setSaved(ConvRef, Ts, bool) override {}
    void deleteFile(ConvRef, Ts, const std::string &) override {}
    void setReminder(ConvRef, Ts, int64_t) override {}
    void userTyping(ConvRef, Ts) override {}
    void search(std::string, std::function<void(std::vector<SearchHit>)> done) override {
        if (done)
            done({});
    }
};

} // namespace model
