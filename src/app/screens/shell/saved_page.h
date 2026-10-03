// The sidebar's "Saved messages" page: every saved
// message (Slack's "Later": reminders soonest due first, then the plain "Save
// for later" bookmarks newest first) as cards — the conversation name (a
// click opens it) on the page's grey, then in a bordered card the message as
// a chat row (avatar, name, time, its text on one line; a click jumps to
// it) and a footer with the due time or "Saved for later" and "Remove".
// Data is the Store's saved list (Store::savedItems), so the page opens at
// once and follows changes while it is up; a card whose message isn't loaded
// fetches it for the preview (Backend::loadMessage).
#pragma once

#include "screens/common/context.h"
#include "screens/shell/avatars.h"
#include "ui/ui.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace shell {

class SavedPage : public ui::View {
public:
    SavedPage(screens::Context &ctx, Avatars &avatars);
    ~SavedPage() override;

    // (Re)builds the cards. Every time the page is brought to front.
    void open();
    // Drops the cards (workspace switch).
    void clear();

    // A message row: jump to the message (in its thread, for a reply). The
    // conversation name on a card: the conversation.
    std::function<void(model::ConvRef, model::Ts ts, model::Ts thread)> onOpenMessage;
    std::function<void(model::ConvRef)>                                 onOpenChannel;

    // Tests: what the page shows.
    size_t             cardCount() const { return _cards.size(); }
    ui::View          *card(size_t i) const;
    const std::string &statusText() const { return _statusText; }
    // A card's footer line ("Saved for later", "Reminder set for …").
    std::string        dueText(size_t i) const;
    void               remove(size_t i);   // the card's "Remove"
    void               activate(size_t i); // a click on the card's message

    void paint(gfx::Painter &p) override;

private:
    class Card;
    void rebuild();
    void resolvePreviews();
    void setStatus(std::string text);
    void onChange(const model::Change &ch);

    screens::Context                                 &_ctx;
    Avatars                                          &_avatars;
    ui::ScrollView                                   *_scroll = nullptr;
    ui::Label                                        *_status = nullptr;
    ui::View                                         *_list   = nullptr;
    std::vector<Card *>                               _cards;
    std::vector<model::Store::SavedItem>              _items; // what the cards show, in order
    std::vector<std::pair<model::ConvRef, model::Ts>> _tried; // previews asked for
    std::string                                       _statusText;
    bool                                              _rebuildQueued = false;
    model::Store::ObserverId                          _observer      = 0;
    std::shared_ptr<int>                              _alive         = std::make_shared<int>(0);
};

} // namespace shell
