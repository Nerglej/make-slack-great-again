// The sidebar's "Scheduled messages" page: every message waiting for the
// service to post it (Slack's "Scheduled"), soonest first, as cards like the
// Saved messages page's — the conversation name (a click opens it) on the
// page's grey, then in a bordered card the message as a chat row under my
// name and a footer with the send time, "Send now" and "Cancel". Data is the
// Store's scheduled list (Store::scheduled); opening the page asks the
// backend to list them again, and the cards follow the list while it is up.
#pragma once

#include "screens/common/context.h"
#include "screens/shell/avatars.h"
#include "ui/ui.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace shell {

class ScheduledPage : public ui::View {
public:
    ScheduledPage(screens::Context &ctx, Avatars &avatars);
    ~ScheduledPage() override;

    // (Re)builds the cards and re-lists. Every time the page is brought to front.
    void open();
    // Drops the cards (workspace switch).
    void clear();

    // The conversation name on a card: the conversation.
    std::function<void(model::ConvRef)> onOpenChannel;

    // Tests: what the page shows.
    size_t             cardCount() const { return _cards.size(); }
    const std::string &statusText() const { return _statusText; }
    std::string        whenText(size_t i) const; // a card's footer ("Sends Mar 15, 2:34 PM")
    bool               canSendNow(size_t i) const;
    void               sendNow(size_t i); // the card's "Send now"
    void               cancel(size_t i);  // the card's "Cancel"

    void paint(gfx::Painter &p) override;

private:
    class Card;
    void rebuild();
    void reset(); // rebuilt from scratch (a failed action's card clickable again)
    void setStatus(std::string text);
    void onChange(const model::Change &ch);

    screens::Context                        &_ctx;
    Avatars                                 &_avatars;
    ui::ScrollView                          *_scroll = nullptr;
    ui::Label                               *_status = nullptr;
    ui::View                                *_list   = nullptr;
    std::vector<Card *>                      _cards;
    std::vector<model::Store::ScheduledItem> _items; // what the cards show, in order
    std::string                              _statusText;
    bool                                     _rebuildQueued = false;
    model::Store::ObserverId                 _observer      = 0;
    std::shared_ptr<int>                     _alive         = std::make_shared<int>(0);
};

} // namespace shell
