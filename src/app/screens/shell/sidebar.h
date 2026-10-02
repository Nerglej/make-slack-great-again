// The conversation sidebar, laid out as msga's ConvListWidget paints it:
// uniform 30 px rows under a 6 px top inset — the "Threads" and "Saved
// messages" entries, then the Starred / Channels / Direct messages / Agents &
// apps sections (a click on a header collapses it, hiding every row; hovering
// shows the chevron that says what a click does; the Direct messages header
// has a "+" on hover), "N more channels" for the ones outside the
// relevant-days window, "Add channels", and the footer (avatar + menu,
// presence toggle). Over msga's nav gradient.
//
// An agent workspace (Capabilities::agentSessions, Claude Code) calls the
// direct messages "Sessions" and ends them with "Add sessions" (its "+" and
// that row open the find-or-create menu), and lists the team
// (Backend::agentRoles) in a "Team" section under them: a teammate's row
// opens its page, its dot is its user's presence, its "+" adds a teammate.
//
// Rows are live: a Store observer restyles a row when its counts change —
// semibold for unread, a red count for DM unreads and channel mentions, a
// blue dot for other unread in an "All new posts" channel; muted
// conversations and ones quiet for over 30 days show no badge.
#pragma once

#include "screens/common/context.h"
#include "screens/shell/avatars.h"
#include "ui/ui.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace shell {

// A workspace's share of the tray dot, the launcher badge and its rail
// tile's dot (msga's updateUnreadBadges): `important` = unread direct
// messages + mentions (an agent workspace: only what needs you, its
// answers), `unread` = other unread activity in channels on "All new posts"
// (`fallback`: the global level). Muted, "Nothing", left and closed
// conversations, and ones idle past the notification window, count for
// neither.
struct Attention {
    int  important = 0;
    bool unread    = false;
    int  dot() const { return important > 0 ? 2 : unread ? 1 : 0; } // 0, 1 blue, 2 red
    bool operator==(const Attention &) const = default;
};
Attention
workspaceAttention(const model::Store &store, model::NotifyLevel fallback, int64_t nowSecs);

class ConvRow;
class Menus;
class SectionHeader;
class SidebarFooter;
class TeammateRow;

class Sidebar : public ui::View {
public:
    Sidebar(screens::Context &ctx, Avatars &avatars);
    ~Sidebar() override;

    // The Settings → Appearance / Notifications choices the list follows.
    struct Filters {
        int                relevantDays   = 14;
        bool               showAgentsApps = true, unreadsOnly = false;
        bool               highlightMentionsOnly = true; // "Highlight mentions-only channels…"
        model::NotifyLevel defaultLevel          = model::NotifyLevel::All;
        bool               operator==(const Filters &) const = default;
    };
    void setFilters(const Filters &f);
    // msga's conv/visitedAt: when each conversation was last opened here (or
    // seen unread), which keeps it listed for the relevant days. The shell
    // persists it (Settings::visitedAt) whenever onVisitedChanged fires.
    using VisitStamps = std::unordered_map<std::string, int64_t>; // conv id → epoch secs

    void                  setVisited(VisitStamps stamps);
    const VisitStamps    &visited() const { return _visited; }
    void                  clearVisited(); // Settings → "Clear state"
    std::function<void()> onVisitedChanged;

    // Now, in epoch seconds (the relevance window's end). Tests pin it.
    void setClock(std::function<int64_t()> now) { _now = std::move(now); }

    void                        rebuild();                   // from the Store (roster changed)
    void                        select(model::ConvRef conv); // highlight only (kNoConv clears)
    model::ConvRef              selected() const { return _selected; }
    // Conversations in on-screen order (the quick switcher's empty query).
    std::vector<model::ConvRef> order() const;

    // What a row shows (tests, and the tray/badge code).
    struct RowState {
        bool exists = false, visible = false, bold = false, dot = false, selected = false;
        int  badge  = 0;
        int  huddle = 0; // a live huddle's pill: its participant count (1 when none listed)
    };
    RowState                 rowState(model::ConvRef conv) const;
    // Sum the app badge shows: mentions + unread direct messages.
    int                      attentionCount() const;
    // The "N more channels" row's N (0: no such row).
    int                      hiddenChannels() const { return _hiddenChannels; }
    // Titles top to bottom: the visible nav entries, then the sections.
    std::vector<std::string> sectionTitles() const;
    // Collapses or expands a section by title (tests; a click does the same).
    bool                     toggleSection(std::string_view title);
    void                     showAllChannels(); // the "N more channels" row

    std::function<void()>                    onThreads;       // the Threads entry
    std::function<void()>                    onSavedMessages; // the Saved messages entry
    std::function<void()>                    onFindChannel;   // "Add channels" → Find a channel
    std::function<void()>                    onCreateChannel; // "Add channels" → Create a channel
    std::function<void()>                    onBrowsePeople;  // the Direct messages header's "+"
    // Agent workspace: the Sessions "+" and "Add sessions" (msga's
    // agentSessionMenuRequested), a teammate's row, the Team header's "+".
    std::function<void(ui::PointF at)>       onSessionMenu;
    std::function<void(const std::string &)> onTeammate;
    std::function<void()>                    onAddTeammate;
    // Highlights a teammate's row as the open page ("" clears it); no
    // conversation is highlighted meanwhile.
    void                                     selectTeammate(const std::string &role);
    const std::string                       &selectedTeammate() const { return _selectedTeammate; }
    // Highlights the Threads entry as the open page (msga's
    // _threadsSelected); opening a conversation or a teammate clears it.
    void                                     selectThreads(bool on);
    bool threadsUnread() const; // the Threads entry is bright (Store::unreadThreads)
    // A click on a row's huddle pill (tests): false when it shows none.
    bool joinHuddle(model::ConvRef conv);
    bool threadsSelected() const { return _threadsSelected; }
    // The same for the Saved messages entry (msga's selectSavedMsgsRow).
    void selectSaved(bool on);
    bool savedSelected() const { return _savedSelected; }
    // The Team section's rows, top to bottom (tests): role ids.
    std::vector<std::string> teammates() const;
    // A teammate row's look (tests): bold, selected, its avatar's presence.
    struct TeammateState {
        bool exists = false, bold = false, selected = false;
        int  presence = 0; // Avatar::Presence
    };
    TeammateState  teammateState(const std::string &role) const;
    int            conversationPresence(model::ConvRef conv) const; // Avatar::Presence of a DM row
    // Context menus (right click, long press, Menu key): a conversation's on
    // its row.
    void           setMenus(Menus *m) { _menus = m; }
    SidebarFooter &footer() { return *_footer; }

    void paint(gfx::Painter &p) override; // the nav gradient

private:
    friend class ConvRow;
    friend class Menus; // the notify section ticks level()
    friend class SectionHeader;
    friend class TeammateRow;
    void               refresh(model::ConvRef conv); // one row from the Store
    void               refreshAll();
    void               refreshSections();
    ConvRow           *rowFor(model::ConvRef conv) const;
    // msga's paintsUnread / effective level (Settings' default) / badge rules.
    bool               paintsUnread(const model::Conversation &c) const;
    model::NotifyLevel level(const model::Conversation &c) const;
    bool               muted(const model::Conversation &c) const;
    bool               isApp(const model::Conversation &c) const;
    bool               relevant(model::ConvRef c) const;
    void               applyCollapse(SectionHeader *h);
    void               rebuildSoon();
    void               addChannelsMenu(ui::View *row);
    void               refreshTeammates();

    screens::Context                      &_ctx;
    Avatars                               &_avatars;
    ui::ScrollView                        *_scroll = nullptr;
    ui::View                              *_items  = nullptr;
    SidebarFooter                         *_footer = nullptr;
    Menus                                 *_menus  = nullptr;
    std::vector<ConvRow *>                 _rows;
    std::vector<TeammateRow *>             _teamRows;
    std::vector<model::Backend::AgentRole> _team; // as last listed (rebuild)
    std::string                            _selectedTeammate;
    std::vector<SectionHeader *>           _sections;
    std::vector<ui::View *>                _nav; // Threads, Saved messages
    std::vector<const char *>              _navTitles;
    ui::View                              *_savedRow = nullptr, *_threadsRow = nullptr;
    bool                                   _threadsSelected = false, _savedSelected = false;
    VisitStamps                            _visited; // opened here (msga's visit stamps)
    std::function<int64_t()>               _now;
    Filters                                _filters;
    model::ConvRef                         _selected       = model::kNoConv;
    model::Store::ObserverId               _observer       = 0;
    std::shared_ptr<int>                   _alive          = std::make_shared<int>(0);
    plat::TimerId                          _rebuildTimer   = 0;
    int                                    _hiddenChannels = 0;
    bool                                   _collapsed[5] = {}; // starred, channels, DMs, apps, team
    bool                                   _showAllChannels = false;
};

} // namespace shell
