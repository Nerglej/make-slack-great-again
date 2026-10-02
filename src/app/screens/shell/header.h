// The conversation header and its tab strip, as msga's MainWindow builds
// them (buildRightPanel, ConvTabsWidget, HeaderAvatarWidget, MembersPopup):
//
//   [avatar] #name ··········· [people 9] [headphones] [star] [search]
//   Messages | Design crit — week 38                    (or "Add canvas")
//
// 48 px tall, 16 px in on the left, 8 on the right. A DM shows the peer's
// avatar with its presence dot, a group DM its stacked member avatars and
// count (a click lists them); a channel shows the members button (a click
// opens the searchable member list). The huddle button shows only where the
// service has huddles. On macOS the header is the unified title bar's
// content instead (msga's TitleBar::setContent): the name centred, the
// actions on the right, as tall as the title bar.
#pragma once

#include "screens/common/context.h"
#include "screens/shell/avatars.h"
#include "ui/ui.h"

#include <functional>
#include <string>
#include <unordered_set>

namespace shell {

class HeaderAvatar;
class StarButton;

class ConvHeader : public ui::View {
public:
    ConvHeader(screens::Context &ctx, Avatars &avatars);

    void           show(model::ConvRef conv); // kNoConv: nothing
    void           refresh();                 // re-read the Store
    model::ConvRef conv() const { return _conv; }

    ui::Label     *title() const { return _title; }
    ui::Clickable *avatar() const;
    ui::Clickable *members() const { return _members; }
    ui::Label     *memberCount() const { return _count; }
    ui::Clickable *huddle() const { return _huddle; }
    ui::Clickable *star() const;
    ui::Clickable *search() const { return _search; }
    bool           starred() const;
    int            avatarPresence() const; // tests: a DM's Avatar::Presence, else -1

    // SEARCH HOOK: the search button (msga's _searchBtn, toggling its
    // SearchWidget). Shell routes it to Shell::openSearch, where the message
    // search lands; the header itself does nothing more.
    std::function<void()> onSearch;
    // The member list: shown under `anchor` (window rect).
    void                  openMembers(ui::RectF anchor);

private:
    screens::Context               &_ctx;
    Avatars                        &_avatars;
    HeaderAvatar                   *_avatar = nullptr;
    ui::Label                      *_title = nullptr, *_count = nullptr;
    ui::Clickable                  *_members = nullptr, *_huddle = nullptr, *_search = nullptr;
    StarButton                     *_star = nullptr;
    model::ConvRef                  _conv = model::kNoConv;
    std::unordered_set<std::string> _membersAsked; // group DMs whose members were asked for
};

// Where the huddle button sends a conversation (msga's huddleJoinUrl).
std::string huddleJoinUrl(const model::Store &st, model::ConvRef conv);

// msga's ConvTabsWidget: 38 px (its bottom divider included), tabs 16 px in,
// the active one bold with a 2 px underline over the divider.
class ConvTabs : public ui::View {
public:
    ConvTabs();
    // hasCanvas: the second tab names the canvas (title, "Untitled"), else
    // offers "Add canvas"; visible = false leaves only Messages.
    void           setCanvas(bool visible, bool hasCanvas, std::string title);
    void           setActive(int tab); // 0 Messages, 1 canvas
    int            active() const { return _active; }
    ui::Clickable *tab(int i) const { return _tabs[i]; }
    std::string    tabText(int i) const;

    std::function<void(int tab)> onSelect;

    void paint(gfx::Painter &p) override;
    void paintOver(gfx::Painter &p) override;

private:
    void           restyle();
    ui::Clickable *_tabs[2] = {};
    int            _active  = 0;
};

} // namespace shell
