#include "screens/shell/saved_page.h"

#include "base/i18n.h"
#include "base/time.h"
#include "gfx/icons_generated.h"
#include "screens/shell/message_search.h"
#include "screens/shell/threads_page.h"

#include <algorithm>

using namespace ui;
using i18n::arg;
using i18n::tr;
using model::ConvRef;
using model::Ts;
using SavedItem = model::Store::SavedItem;

namespace shell {

namespace {

// The card geometry the Threads page's cards share.
constexpr float kAvatar = 36, kAvatarRadius = 4, kAvatarGap = 10;
constexpr float kRowPadV = 4, kHdrGap = 2;

std::string dueLabel(const SavedItem &s) {
    if (s.due <= 0)
        return tr("Saved for later");
    return arg(tr("Reminder set for %1"), base::formatDateTime(s.due));
}

} // namespace

// One saved message: the conversation header outside the bordered body,
// the message row, the due footer.
class SavedPage::Card final : public View {
public:
    Card(SavedPage &page, const SavedItem &item) : _page(page), _item(item) {
        screens::Context          &ctx = page._ctx;
        const model::Store        &st  = ctx.store;
        const model::Conversation &c   = st.conversation(item.conv);
        style().spacing(12);
        auto *nameRow = add<View>();
        nameRow->style().row().spacing(4).items(Align::Center);
        nameRow->add<IconView>(
            c.isDirect()                         ? gfx::Icon::MessageSquare
            : c.kind == model::ConvKind::Private ? gfx::Icon::Lock
                                                 : gfx::Icon::Hash,
            15,
            C::Text
        );
        auto *name =
            nameRow->add<TextLink>(st.displayName(item.conv), Font::BodyBold, C::Text, false);
        name->onClick = [this] {
            if (_page.onOpenChannel)
                _page.onOpenChannel(_item.conv);
        };

        auto *body = add<View>();
        body->setBackground(C::Surface, 8);
        body->setBorder(C::Border);
        body->style().padding(24, 16, 24, 16).spacing(4);
        // The message, chat-style; a click anywhere jumps to it.
        auto *row = body->add<Clickable>();
        row->setLook({C::None, C::None, C::None, C::None, 0});
        row->setCursor(plat::Cursor::Hand);
        row->style()
            .row()
            .spacing(kAvatarGap)
            .padding(0, kRowPadV, 0, kRowPadV)
            .items(Align::Start);
        row->onClick           = [this] { open(); };
        const model::User &u   = st.user(item.author);
        // A reminder set from another client carries no author.
        const std::string  who = !item.botName.empty()           ? item.botName
                                 : item.author != model::kNoUser ? std::string(u.label())
                                                                 : std::string(tr("Message"));
        auto              *av  = row->add<Avatar>();
        av->style().size(kAvatar, kAvatar).noShrink();
        av->setRadius(kAvatarRadius);
        av->setPlaceholder(C::PresenceAway);
        av->setInitial(who);
        const std::string &pic = !item.botAvatar.empty() ? item.botAvatar : u.avatar;
        if (!pic.empty())
            av->setBitmap(page._avatars.get(pic, int(kAvatar * 2)));
        auto *col = row->add<View>();
        col->style().flex(1).spacing(kHdrGap);
        col->style().shrink = 1;
        auto *hdr           = col->add<View>();
        hdr->style().row().spacing(8).items(Align::Center);
        auto *n = hdr->add<Label>(who, Font::BodySemibold);
        n->setMaxLines(1);
        n->style().shrink = 1;
        hdr->add<Label>(
               base::dateTimeLabel(model::tsSecs(item.ts), ctx.backend.nowSecs()),
               Font::Small,
               C::TextMuted
        )
            ->style()
            .noShrink();
        const std::string text = searchPreview(st, item.text);
        col->add<Label>(text.empty() ? std::string(tr("No preview available")) : text, Font::Body)
            ->setMaxLines(1);

        // Overdue reminders already alarmed: the clock line in the mention
        // badge's colour. A plain bookmark shows a bookmark, not the clock.
        const bool reminder = item.due > 0;
        const bool overdue  = reminder && item.due <= base::nowSecs();
        const C    dueCol   = overdue ? C::Badge : C::TextMuted;
        auto      *footer   = body->add<View>();
        footer->style().row().spacing(8).items(Align::Center);
        footer->add<IconView>(reminder ? gfx::Icon::AlarmClock : gfx::Icon::Bookmark, 13, dueCol);
        footer->add<Label>(dueLabel(item), Font::Caption, dueCol)->style().flex(1);
        auto *removeBtn    = footer->add<TextLink>(tr("Remove"), Font::Control, C::Link, false);
        removeBtn->onClick = [this] { remove(); };
    }

    void open() {
        if (_page.onOpenMessage)
            _page.onOpenMessage(_item.conv, _item.ts, _item.thread);
    }
    // The saved item goes, reminder or not.
    void remove() {
        if (_item.due > 0)
            _page._ctx.backend.setReminder(_item.conv, _item.ts, 0);
        else
            _page._ctx.backend.setSaved(_item.conv, _item.ts, false);
    }

private:
    SavedPage &_page;
    SavedItem  _item;
};

SavedPage::SavedPage(screens::Context &ctx, Avatars &avatars) : _ctx(ctx), _avatars(avatars) {
    style().column().flex(1);
    // The Threads page's header: 48 px, the title bold; a hairline under it.
    auto *header = add<View>();
    header->style().row().height(48).padding(16, 0, 8, 0).items(Align::Center).noShrink();
    header->add<Label>(tr("Saved messages"), Font::Title)->style().flex(1);
    add<Separator>(false, C::FormDivider);
    _scroll = add<ScrollView>();
    _scroll->style().flex(1);
    View *content = _scroll->content();
    content->style().padding(24).spacing(24);
    _status = content->add<Label>("", Font::Body, C::TextMuted);
    _status->setAlign(text::LayoutOptions::Align::Center);
    _status->style().padding(24);
    _status->setVisible(false);
    _list = content->add<View>();
    _list->style().spacing(24);
    // Follow set / remove / server sync while the page is on screen; open()
    // rebuilds anyway when it comes back.
    _observer = ctx.store.observe(model::Store::kAnyConv, [this](const model::Change &ch) {
        onChange(ch);
    });
}

SavedPage::~SavedPage() {
    _ctx.store.unobserve(_observer);
}

void SavedPage::paint(gfx::Painter &p) {
    p.fillRect(bounds(), color(C::FormSunken));
    View::paint(p);
}

void SavedPage::onChange(const model::Change &ch) {
    if (!visible() || _rebuildQueued)
        return;
    bool mine = false;
    if (ch.kind == model::ChangeKind::Reset || ch.kind == model::ChangeKind::Roster)
        mine = true;
    else if (ch.kind == model::ChangeKind::Update || ch.kind == model::ChangeKind::Remove)
        mine = _ctx.store().findSaved(ch.conv, ch.ts) ||
               std::any_of(_items.begin(), _items.end(), [&](const SavedItem &s) {
                   return s.conv == ch.conv && s.ts == ch.ts;
               });
    if (!mine)
        return;
    // Coalesced: a server sync changes many items in one go.
    _rebuildQueued           = true;
    std::weak_ptr<int> alive = _alive;
    _ctx.app.platform().post([this, alive] {
        if (alive.expired())
            return;
        _rebuildQueued = false;
        if (visible())
            rebuild();
    });
}

void SavedPage::open() {
    _tried.clear();
    rebuild();
}

void SavedPage::clear() {
    _list->clearChildren();
    _cards.clear();
    _items.clear();
    _tried.clear();
    setStatus({});
}

void SavedPage::rebuild() {
    _list->clearChildren();
    _cards.clear();
    _items.clear();
    for (SavedItem &s : _ctx.store().savedItems()) {
        if (s.conv >= _ctx.store().conversationCount())
            continue;
        _cards.push_back(_list->add<Card>(*this, s));
        _items.push_back(std::move(s));
    }
    setStatus(
        _cards.empty()
            ? std::string(tr("Messages you save for later or set reminders on will appear here."))
            : std::string()
    );
    resolvePreviews();
}

// A card whose message was never seen (saved
// from another client, not in the loaded history) fetches it once; the
// answer lands in the Store, which rebuilds the page.
void SavedPage::resolvePreviews() {
    for (const SavedItem &s : _items) {
        if (s.previewed)
            continue;
        const auto key = std::make_pair(s.conv, s.ts);
        if (std::find(_tried.begin(), _tried.end(), key) != _tried.end())
            continue;
        _tried.push_back(key);
        std::weak_ptr<int> alive = _alive;
        const std::string  ws    = _ctx.store().workspaceId;
        _ctx.backend.loadMessage(s.conv, s.ts, [this, alive, ws, key](bool ok, model::Message m) {
            if (alive.expired() || _ctx.store().workspaceId != ws)
                return;
            _ctx.store().setSavedPreview(key.first, key.second, ok ? &m : nullptr);
        });
    }
}

ui::View *SavedPage::card(size_t i) const {
    return _cards[i];
}

std::string SavedPage::dueText(size_t i) const {
    return i < _items.size() ? dueLabel(_items[i]) : std::string();
}

void SavedPage::remove(size_t i) {
    if (i < _cards.size())
        _cards[i]->remove();
}

void SavedPage::activate(size_t i) {
    if (i < _cards.size())
        _cards[i]->open();
}

void SavedPage::setStatus(std::string text) {
    _statusText = std::move(text);
    _status->setText(_statusText);
    _status->setVisible(!_statusText.empty());
}

} // namespace shell
