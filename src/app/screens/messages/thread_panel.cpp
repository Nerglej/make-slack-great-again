#include "app/screens/messages/thread_panel.h"

#include "app/screens/messages/thread_export.h"
#include "base/str.h"
#include "base/time.h"
#include "gfx/icons_generated.h"
#include "screens/common/file_dialogs.h"

#include "base/i18n.h"

#include <algorithm>

using namespace ui;
using i18n::arg;
using i18n::tr;

namespace screens {

namespace {

// msga's IconButton: 32 px round, an 18 px icon.def glyph, surface.highlight
// on hover.
class RoundButton final : public Clickable {
public:
    RoundButton(gfx::Icon icon, std::string tip) : icon(icon) {
        style().size(32, 32).noShrink();
        setLook({C::None, C::FormHighlight, C::FormHighlight, C::None, 16});
        setTooltip(std::move(tip));
        setRole(Role::Button);
    }
    void paint(gfx::Painter &p) override {
        Clickable::paint(p);
        gfx::drawIcon(
            p,
            icon,
            {snapPx((width() - 18) / 2), snapPx((height() - 18) / 2), 18, 18},
            color(C::FormIcon)
        );
    }
    gfx::Icon icon;
};

} // namespace

ThreadPanel::ThreadPanel(Context &ctx) : _ctx(ctx) {
    setBackground(C::Surface);
    setRole(Role::Group);
    auto *header = add<View>();
    header->style().row().height(48).padding(16, 0, 8, 0).spacing(8).items(Align::Center);
    header->style().noShrink();
    header->add<Label>(tr("Thread"), Font::BodyBold)->style().flex(1);
    _mute          = header->add<RoundButton>(gfx::Icon::Bell, tr("Mute thread"));
    _mute->onClick = [this] { toggleMuted(); };
    _download      = header->add<RoundButton>(gfx::Icon::Download, tr("Download thread as text"));
    _download->onClick = [this] { downloadThread(); };
    // A branched agent conversation can move to the list as a session.
    _openSession       = header->add<RoundButton>(gfx::Icon::ExternalLink, tr("Open as session"));
    _openSession->setVisible(false);
    _openSession->onClick = [this] {
        if (onOpenAsSession && root())
            onOpenAsSession(conversation(), root());
    };
    auto *close    = header->add<RoundButton>(gfx::Icon::X, tr("Close thread"));
    close->onClick = [this] {
        if (_ctx.closeThread)
            _ctx.closeThread();
    };
    _list = add<MessageList>(ctx);
    _list->style().flex(1);
    _list->setTypingRow(false); // the shell's indicator sits above the composer
    _typingSlot = add<View>();
    _slot       = add<View>();
    // The composer brings its own margins, as in the channel view (msga adds none).
    _slot->style().padding(0);
    // In line with the composer's contents (its margin plus the box's inner
    // padding), clear of the bottom edge; the composer's margin is the gap above.
    _broadcastRow = add<View>();
    _broadcastRow->style().row().padding(12 + 8, 0, 12, 12);
    _broadcast = _broadcastRow->add<CheckBox>(tr("Also send to channel"));
    _broadcast->setLabelFont(Font::Tiny, C::FormTextMuted);
    _broadcast->onChange = [this](bool on) { _wanted = on; };
    _broadcastRow->setVisible(false);
}

void ThreadPanel::show(ConvRef conv, Ts root) {
    // Another thread: the tick was for a reply there.
    if (conv != conversation() || root != this->root())
        _wanted = false;
    _list->showThread(conv, root);
    // An agent session's thread is a subagent's run, which a reply goes on to
    // (relayed by the session), or a side conversation branched off it
    // (/btw), which is also a session of its own. A subagent that hasn't
    // started yet takes nothing: no composer.
    const bool agent    = conv != model::kNoConv && _ctx.backend.capabilities().agentSessions;
    const bool readOnly = agent && !_ctx.backend.threadAcceptsReplies(conv, root);
    _openSession->setVisible(agent && _ctx.backend.threadOpensAsSession(conv, root));
    if (_composer) {
        _composer->setVisible(!readOnly);
        _composer->setEnabled(!readOnly);
    }
    refreshMute();
    refreshBroadcast();
}

ui::View *ThreadPanel::setComposer(std::unique_ptr<ui::View> composer) {
    _slot->clearChildren();
    _composer = composer ? _slot->adopt(std::move(composer)) : nullptr;
    return _composer;
}

ui::View *ThreadPanel::setTyping(std::unique_ptr<ui::View> typing) {
    _typingSlot->clearChildren();
    return typing ? _typingSlot->adopt(std::move(typing)) : nullptr;
}

void ThreadPanel::toggleMuted() {
    const ConvRef c = conversation();
    if (c == model::kNoConv || !root())
        return;
    _ctx.store().setThreadMuted(c, root(), !_ctx.store().threadMuted(c, root()));
    refreshMute();
}

void ThreadPanel::refreshMute() {
    const bool muted = conversation() != model::kNoConv && root() &&
                       _ctx.store().threadMuted(conversation(), root());
    auto      *b     = static_cast<RoundButton *>(_mute);
    b->icon          = muted ? gfx::Icon::BellOff : gfx::Icon::Bell;
    b->setTooltip(muted ? tr("Unmute thread") : tr("Mute thread"));
    b->update();
}

bool ThreadPanel::broadcastShown() const {
    return _broadcastRow->visible();
}

bool ThreadPanel::broadcastWanted() const {
    return _wanted && !_blocked && broadcastShown();
}

void ThreadPanel::setBroadcastWanted(bool on) {
    _wanted = on;
    refreshBroadcast();
}

void ThreadPanel::setBroadcastBlocked(bool blocked) {
    _blocked = blocked;
    refreshBroadcast();
}

void ThreadPanel::refreshBroadcast() {
    // Only a channel thread has a channel to also send the reply to.
    const ConvRef c    = conversation();
    const bool    show = c != model::kNoConv && _ctx.backend.capabilities().replyBroadcast &&
                         !_ctx.store().conversation(c).isDirect();
    _broadcastRow->setVisible(show);
    _broadcast->setEnabled(show && !_blocked);
    _broadcast->setChecked(show && !_blocked && _wanted);
}

// msga's downloadThread: where to, then ThreadExportJob (exportThread) pages
// through the whole thread and writes its transcript in the background.
void ThreadPanel::downloadThread() {
    const ConvRef conv = conversation();
    const Ts      rt   = root();
    if (conv == model::kNoConv || !rt)
        return;
    // Taken now: the export may outlive this panel and its thread.
    std::string          title = threadExportTitle(_ctx.store, conv);
    plat::FileDialogDesc d;
    d.mode          = plat::FileDialogDesc::Mode::Save;
    d.title         = tr("Save thread");
    d.initialDir    = _ctx.app.platform().standardDir(plat::StandardDir::Home);
    d.suggestedName = str::concat({"thread-", base::isoDate(model::tsSecs(rt)), ".txt"});
    Context &ctx    = _ctx; // owned by main: outlives the panel
    fileDialog(
        ctx,
        std::move(d),
        [&ctx, conv, rt, title = std::move(title)](std::vector<std::string> paths) mutable {
            if (!paths.empty() && !paths[0].empty())
                exportThread(ctx, conv, rt, std::move(title), std::move(paths[0]));
        }
    );
}

} // namespace screens
