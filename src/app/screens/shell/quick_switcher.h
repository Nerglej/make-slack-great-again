// Ctrl/Cmd+K: a popup with a filter field over every conversation.
#pragma once

#include "screens/common/context.h"
#include "screens/shell/avatars.h"
#include "ui/ui.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace shell {

// Every conversation you are a member of except
// DMs whose peer is deactivated or still unresolved (a raw user id), most
// recent first by max(visited here, latest message, last read), then by
// name. `visited`: the sidebar's visit stamps (conv id → epoch secs).
std::vector<model::ConvRef> quickSwitchOrder(
    const model::Store &store, const std::unordered_map<std::string, int64_t> &visited
);

// Conversations whose name fuzzy-matches `query` (fuzzy_match.h), best
// first, group DMs a little behind; ties keep `order`. Empty query = order
// (minus the ones without a name yet).
std::vector<model::ConvRef> quickSwitchFilter(
    const model::Store &store, std::string_view query, const std::vector<model::ConvRef> &order
);

// One workspace's tab.
struct QuickSwitchTab {
    std::string                 key, name, icon; // icon: a local picture ("" = the letter)
    const model::Store         *store = nullptr;
    std::vector<model::ConvRef> order; // quickSwitchOrder
};

// With several workspaces a strip of their bubbles sits above the field,
// opening on the open one; ←/→ (or Tab) move between them and a click does
// too. Typing re-aims the tab at the workspace holding the best match (a tab
// picked by hand while a query is up stays put until it runs dry), and the
// workspaces with nothing for the query are dimmed.
class QuickSwitcher : public ui::Popup {
public:
    QuickSwitcher(
        screens::Context           &ctx,
        Avatars                    &avatars,
        std::vector<model::ConvRef> order,
        std::vector<QuickSwitchTab> tabs = {}
    );
    ~QuickSwitcher() override;
    ui::TextEdit                      &field() { return *_field; }
    const std::vector<model::ConvRef> &results() const { return _results; }
    void                               choose(int index); // opens results[index] and closes
    int                                tab() const { return _tab; }
    void                               setTab(int index); // a click on its bubble
    void                               stepTab(int delta);
    // The empty state shown instead of the list ("" while there are results).
    std::string                        emptyText() const;
    // A pick in another workspace's tab: switch there, then open it.
    std::function<void(const std::string &key, model::ConvRef conv)> onChooseIn;

private:
    class Rows;
    friend class Rows;
    void                  applyFilter(); // re-aims the tab, then refilter()
    void                  refilter();
    void                  highlight(int index);
    void                  refreshStrip();
    std::optional<double> bestScore(const QuickSwitchTab &t, std::string_view query) const;

    screens::Context           &_ctx;
    Avatars                    &_avatars;
    std::vector<QuickSwitchTab> _tabs;
    int                         _tab       = 0;
    bool                        _manualTab = false; // picked by hand since typing began
    std::vector<model::ConvRef> _results;
    std::vector<bool>           _dimmed;
    ui::TextEdit               *_field   = nullptr;
    ui::View                   *_strip   = nullptr;
    ui::Label                  *_tabName = nullptr, *_empty = nullptr;
    std::unique_ptr<Rows>       _rows; // the list's adapter
    ui::VirtualList            *_list    = nullptr;
    int                         _current = 0;
};

} // namespace shell
