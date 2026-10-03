// Back/forward conversation history, with text
// editor undo/redo semantics: going back keeps the forward stack, opening a
// conversation by a direct action (sidebar, switcher, notification, menu)
// clears everything in front of the current one. Conversations only:
// threads are not entries. A location is a conversation in a
// (possibly background) workspace, so the history crosses workspaces.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace shell {

// A location: the workspace's key and the conversation's id (ids,
// not ConvRefs: a workspace's Store is rebuilt when it restarts).
struct NavLocation {
    std::string key, conv;
    bool        valid() const { return !conv.empty(); }
    bool        operator==(const NavLocation &) const = default;
};

class NavHistory {
public:
    static constexpr size_t kMaxDepth = 32;
    // Entries go stale (a workspace signed out, a channel left); goBack and
    // goForward drop the ones the validator rejects.
    using Validator                   = std::function<bool(const NavLocation &)>;

    // A direct open: the previous location goes on the back stack, the
    // forward stack is cleared.
    void               recordOpen(const NavLocation &loc);
    // Syncs the current location without touching the stacks (while a
    // back/forward jump is being applied).
    void               setCurrent(const NavLocation &loc) { _current = loc; }
    const NavLocation &current() const { return _current; }
    // The location to show, or an invalid one when there is none.
    NavLocation        goBack(const Validator &valid) { return go(_back, _forward, valid); }
    NavLocation        goForward(const Validator &valid) { return go(_forward, _back, valid); }
    bool               canGoBack() const { return !_back.empty(); }
    bool               canGoForward() const { return !_forward.empty(); }
    // Drops every trace of a signed-out workspace.
    void               purge(const std::string &key);

private:
    static void push(std::vector<NavLocation> &d, const NavLocation &loc);
    NavLocation
    go(std::vector<NavLocation> &from, std::vector<NavLocation> &to, const Validator &valid);

    NavLocation              _current;
    std::vector<NavLocation> _back, _forward; // newest last
};

} // namespace shell
