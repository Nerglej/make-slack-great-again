#include "screens/shell/shell_dialogs.h"

#include "app/identity.h"
#include "app/mrkdwn/mrkdwn.h"
#include "app/model/jobs.h"
#include "app/screens/common/downloads.h"
#include "app/screens/common/file_dialogs.h"
#include "app/screens/common/message_text.h"
#include "app/screens/common/remote_images.h"
#include "base/file.h"
#include "base/i18n.h"
#include "base/log.h"
#include "base/str.h"
#include "base/time.h"
#include "gfx/icons_generated.h"
#include "screens/shell/composer.h"
#include "ui/controls.h"

#ifdef MSGA_HAVE_MESSAGES
#include "app/screens/messages/message_dialogs.h"
#endif

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

using namespace ui;
using model::ConvKind;
using model::ConvRef;
using model::kNoConv;
using model::kNoUser;
using model::UserRef;
using V = FormButton::Kind;
using i18n::tr;

namespace shell {

namespace {

std::string lower(std::string s) {
    for (char &c : s)
        if (c >= 'A' && c <= 'Z')
            c = char(c + 32);
    return s;
}

// ── The conversation picker (msga's ConvSelectorWidget) ─────────────────────

struct Target {
    ConvRef     conv   = kNoConv;
    UserRef     person = kNoUser; // someone without a DM yet
    std::string label;
    std::string role; // an agent workspace's teammate: a new session with it
};

class ConvSelector;

// The drop-down under the field: literal white, a divider.strong border,
// radius 4; rows padded 6/12, hover surface.highlight, the current row
// accent.subtleBg with replyLink text; at most 6 rows (200 px) tall.
class PickerList final : public Popup {
public:
    PickerList(ConvSelector &owner, std::vector<Target> items)
        : _owner(owner), _items(std::move(items)) {
        setModal(false);
        setCard(false);
        setPaintOutset(0);
    }
    SizeF measureContent(float, float) override {
        const float n = float(std::min<size_t>(_items.size(), 6));
        return {_w, std::min(200.f, rowH() * n + 4)};
    }
    void  setWidth(float w) { _w = w; }
    float rowH() const {
        return std::ceil(ui::pxFont(15, text::Weight::Regular, 0).size * 1.4f) + 12;
    }
    void paint(gfx::Painter &p) override {
        const RectF b = bounds();
        p.fillRoundRect(b, 4, 0xffffffffU);
        p.save();
        p.clipRoundRect(b, 4);
        const float k = windowScale();
        for (size_t i = 0; i < _items.size(); ++i) {
            const float y = 2 + rowH() * float(i) - _scroll;
            if (y + rowH() < 0 || y > b.h)
                continue;
            const bool cur = int(i) == _current;
            if (cur)
                p.fillRect({1, y, b.w - 2, rowH()}, ui::color(C::AccentSubtle));
            else if (int(i) == _hover)
                p.fillRect({1, y, b.w - 2, rowH()}, ui::color(C::FormHighlight));
            text::AttributedText t;
            t.append(
                _items[i].label,
                ui::pxFont(15, text::Weight::Regular, cur ? ui::color(C::ReplyLink) : 0xff1d1c1dU)
            );
            text::LayoutOptions o;
            o.maxLines = 1;
            o.ellipsis = true;
            o.maxWidth = b.w - 24;
            auto l     = text::Layout::build(t, o, k);
            l->paint(p, snapPx({12, y + std::floor((rowH() - l->height()) / 2)}));
        }
        p.restore();
        p.strokeRoundRect({0.5f, 0.5f, b.w - 1, b.h - 1}, 4, 1, ui::color(C::FormDividerStrong));
    }
    bool    onEvent(Event &e) override;
    uint8_t cursorAt(PointF p) const override {
        return rowAt(p.y) >= 0 ? uint8_t(plat::Cursor::Hand) : View::cursorAt(p);
    }
    const std::vector<Target> &items() const { return _items; }
    int                        current() const { return _current; }

private:
    int rowAt(float y) const {
        const int i = int((y - 2 + _scroll) / rowH());
        return i >= 0 && i < int(_items.size()) ? i : -1;
    }
    ConvSelector       &_owner;
    std::vector<Target> _items;
    float               _w = 300, _scroll = 0;
    int                 _hover = -1, _current = -1;
};

class ConvSelector final : public View {
public:
    explicit ConvSelector(screens::Context &ctx) : _ctx(ctx) {
        style().row().height(36).padding(8, 0, 8, 0).spacing(4).items(Align::Center).noShrink();
        _edit = add<TextEdit>();
        _edit->style().flex(1);
        _edit->setMaxLines(1);
        _edit->setPlaceholder(tr("Search channels and people\xE2\x80\xA6"));
        _edit->onChange = [this] { refresh(); };
        _edit->onSubmit = [this] {
            if (_list && !_list->items().empty())
                pick(_list->items()[size_t(std::max(0, _list->current()))]);
            return true;
        };
        _chip = add<Clickable>();
        _chip->setVisible(false);
        _chip->setLook({C::None, C::None, C::None, C::None, 0});
        _chip->style().row().spacing(4).items(Align::Center);
        _chip->setCursor(plat::Cursor::IBeam); // only its × is clickable
        _chipLabel = _chip->add<Label>();
        auto *x    = _chip->add<Clickable>();
        x->style().size(18, 18);
        x->setLook({C::None, C::None, C::None, C::None, 0});
        _x         = x;
        x->onClick = [this] { clear(); };
        setCursor(plat::Cursor::IBeam);
    }
    ~ConvSelector() override { closeList(); }

    std::function<void(const Target *)> onPick;
    const Target                       *target() const { return _picked ? &_target : nullptr; }
    TextEdit                           &edit() { return *_edit; }

    void paint(gfx::Painter &p) override {
        const RectF b = bounds();
        p.fillRoundRect(b, 4, 0xffffffffU); // msga: literal white, also in dark mode
        p.strokeRoundRect(
            {0.5f, 0.5f, b.w - 1, b.h - 1},
            4,
            1,
            ui::color(_edit->focused() ? C::Accent : C::FormDividerStrong)
        );
        if (_chip->visible()) { // the chip: accent.subtleBg, radius 12
            const RectF c = _chip->frame(), l = _chipLabel->frame();
            p.fillRoundRect({c.x + l.x, c.y + l.y, l.w, l.h}, 12, ui::color(C::AccentSubtle));
        }
        View::paint(p);
    }
    void paintOver(gfx::Painter &p) override {
        if (!_chip->visible())
            return;
        // The chip: accent.subtleBg, replyLink bold text, radius 12, padding 2/8,
        // then the flat "×" (#666, #333 on hover).
        const RectF c  = _chip->frame();
        const RectF x  = _x->frame();
        const Color xc = _x->hovered() ? 0xff333333U : 0xff666666U;
        const float cx = c.x + x.x + 9, cy = c.y + x.y + 9;
        p.drawLine({cx - 4, cy - 4}, {cx + 4, cy + 4}, 1.3f, xc);
        p.drawLine({cx + 4, cy - 4}, {cx - 4, cy + 4}, 1.3f, xc);
    }
    bool onEvent(Event &e) override {
        if (e.type == EventType::PointerDown) {
            _edit->focus();
            return true;
        }
        if (e.type == EventType::FocusOut)
            _ctx.app.platform().post([this] {
                if (!_edit->focused())
                    closeList();
            });
        return false;
    }
    void pick(const Target &t) {
        _target = t;
        _picked = true;
        closeList();
        text::AttributedText a;
        a.append(t.label, ui::pxFont(15, text::Weight::Bold, ui::color(C::ReplyLink)));
        _chipLabel->setRichText(std::move(a));
        _chipLabel->style().padding(8, 2);
        _chip->setVisible(true);
        _edit->setVisible(false);
        _edit->clear();
        if (onPick)
            onPick(&_target);
    }
    void clear() {
        _picked = false;
        _chip->setVisible(false);
        _edit->setVisible(true);
        _edit->focus();
        if (onPick)
            onPick(nullptr);
    }
    void closeList() {
        if (PickerList *l = _list) {
            _list = nullptr;
            l->close();
        }
    }

private:
    void refresh() {
        closeList();
        std::string q(str::trim(_edit->text()));
        if (q.empty() || !window())
            return;
        const bool chans = q[0] == '#', people = q[0] == '@';
        if (chans || people)
            q.erase(0, 1);
        q                      = lower(q);
        const model::Store &st = _ctx.store;
        std::vector<Target> out;
        auto                match = [&](const std::string &s) {
            return q.empty() || lower(s).find(q) != std::string::npos;
        };
        for (ConvRef c = 0; c < st.conversationCount(); ++c) {
            const auto &cv = st.conversation(c);
            if (!cv.member)
                continue;
            const bool direct = cv.isDirect();
            if ((chans && direct) || (people && !direct))
                continue;
            const std::string name = st.displayName(c);
            if (match(name))
                out.push_back({c, kNoUser, direct ? name : "#" + name});
        }
        // An agent workspace's teammates (msga's ChatTarget::teammate):
        // writing to one starts a session with it.
        if (!chans && _ctx.backend.capabilities().agentSessions)
            for (const model::Backend::AgentRole &r : _ctx.backend.agentRoles())
                if (!r.id.empty() && match(r.name))
                    out.push_back({kNoConv, kNoUser, r.name, r.id});
        if (!chans) { // people without a DM (up to 50): prefix matches first
            std::vector<std::pair<int, Target>> ppl;
            for (UserRef u = 0; u < st.userCount(); ++u) {
                const model::User &us = st.user(u);
                if (us.bot || us.placeholder || u == st.me)
                    continue;
                bool hasDm = false;
                for (ConvRef c = 0; c < st.conversationCount() && !hasDm; ++c)
                    hasDm =
                        st.conversation(c).kind == ConvKind::Dm && st.conversation(c).dmUser == u;
                const std::string label(us.label());
                if (hasDm || !match(label))
                    continue;
                ppl.push_back({lower(label).rfind(q, 0) == 0 ? 0 : 1, {kNoConv, u, label}});
            }
            std::sort(ppl.begin(), ppl.end(), [](const auto &a, const auto &b) {
                return a.first != b.first ? a.first < b.first
                                          : lower(a.second.label) < lower(b.second.label);
            });
            for (size_t i = 0; i < ppl.size() && i < 50; ++i)
                out.push_back(ppl[i].second);
        }
        if (out.empty())
            return;
        auto l = std::make_unique<PickerList>(*this, std::move(out));
        l->setWidth(width());
        l->setAnchor(windowRect(), Popup::Place::Below);
        _list = l.get();
        window()->showPopup(std::move(l));
    }

    screens::Context &_ctx;
    TextEdit         *_edit;
    Clickable        *_chip, *_x;
    Label            *_chipLabel;
    PickerList       *_list = nullptr;
    Target            _target;
    bool              _picked = false;
};

bool PickerList::onEvent(Event &e) {
    switch (e.type) {
    case EventType::PointerMove: {
        const int i = rowAt(e.pos.y);
        if (i != _hover) {
            _hover = i;
            update();
        }
        return true;
    }
    case EventType::PointerDown:
        if (const int i = rowAt(e.pos.y); i >= 0) {
            const Target t = _items[size_t(i)];
            _owner.pick(t); // closes this list (deferred)
        }
        return true;
    case EventType::Scroll: {
        const float max = std::max(0.f, rowH() * float(_items.size()) + 4 - height());
        _scroll         = std::clamp(_scroll - e.dy * (e.precise ? 1.f : 30.f), 0.f, max);
        update();
        return true;
    }
    default:
        return false;
    }
}

// ── Forward ─────────────────────────────────────────────────────────────────

// What a forward's files become (msga's fetchForwardedFiles): `paths` to
// upload again, in file order; `links` that go along as text instead (a
// canvas, a file with nothing to fetch, or every file when the target can't
// take uploads); `temps` the downloaded copies among the paths, removed once
// the send is over.
struct ForwardFiles {
    std::vector<std::string> paths, links, temps;
};

// Local files go as they are; the service's are downloaded through the
// backend first (off the UI thread). done(files, error) once, later; a
// non-empty error = a download failed, nothing should be sent (and the
// copies are gone).
void fetchForwardedFiles(
    screens::Context                                    &ctx,
    const std::vector<model::File>                      &files,
    bool                                                 upload,
    std::function<void(ForwardFiles, std::string error)> done
) {
    struct Batch {
        std::vector<std::string>                             paths; // by file index
        ForwardFiles                                         out;
        int                                                  pending = 1;
        bool                                                 failed  = false;
        std::function<void(ForwardFiles, std::string error)> done;
    };
    auto batch  = std::make_shared<Batch>();
    batch->done = std::move(done);
    batch->paths.resize(files.size());
    auto finish = [batch] {
        if (batch->failed || --batch->pending > 0)
            return;
        for (std::string &p : batch->paths)
            if (!p.empty())
                batch->out.paths.push_back(std::move(p));
        batch->done(std::move(batch->out), {});
    };
    auto fail = [batch](const std::string &err) {
        if (std::exchange(batch->failed, true))
            return;
        for (const std::string &t : batch->out.temps) // those still coming go as they land
            screens::removeTempDownload(t);
        batch->done({}, err.empty() ? std::string("download failed") : err);
    };
    // Held at 1 until every download has started: one that answers at once
    // can't finish the batch early.
    for (size_t i = 0; i < files.size() && !batch->failed; ++i) {
        const model::File &f      = files[i];
        const std::string &src    = f.source();
        const bool         remote = screens::RemoteImages::isRemote(src);
        if (src.empty() || !upload || f.mime == "application/vnd.slack-docs") { // a canvas
            if (!src.empty())
                batch->out.links.push_back(remote ? src : file::toFileUrl(src));
            continue;
        }
        if (!remote && file::exists(src) && !file::isDir(src)) { // already on disk
            batch->paths[i] = src;
            continue;
        }
        const std::string to = screens::tempDownloadPath(ctx.app.platform(), f.name);
        if (to.empty()) {
            fail("no temporary folder");
            break;
        }
        batch->out.temps.push_back(to);
        ++batch->pending;
        screens::fetchFile(
            ctx.app.platform(),
            ctx.backend,
            src,
            to,
            [batch, finish, fail, i, to](bool ok, const std::string &err) {
                if (batch->failed) {
                    screens::removeTempDownload(to);
                    return;
                }
                if (!ok)
                    return fail(err);
                batch->paths[i] = to;
                finish();
            }
        );
    }
    finish();
}

// A Context over another workspace's Store and Backend (the forward
// dialog's picker and composer work in the workspace the message goes to):
// the rest is the window's.
std::unique_ptr<screens::Context>
contextFor(const screens::Context &base, model::Store &store, model::Backend &backend) {
    auto c = std::unique_ptr<screens::Context>(
        new screens::Context{base.app, store, backend, base.images, {}, {}, {}, {}, {}}
    );
    c->openUrl        = base.openUrl;
    c->profileHover   = base.profileHover;
    c->window         = base.window;
    c->linkPreviews   = base.linkPreviews;
    c->remote         = base.remote;
    c->ai             = base.ai;
    c->openAiSettings = base.openAiSettings;
    return c;
}

class ForwardDialog final : public Dialog {
public:
    ForwardDialog(
        screens::Context                        &ctx,
        ConvRef                                  conv,
        model::Ts                                ts,
        std::string                              onlyFile,
        std::function<void(const std::string &)> onError,
        std::vector<ForwardWorkspace>            workspaces,
        PrefillTeammate                          prefillTeammate
    )
        : Dialog(onlyFile.empty() ? tr("Forward this message") : tr("Forward this file")),
          _conv(conv), _ts(ts), _onlyFile(std::move(onlyFile)), _onError(std::move(onError)),
          _workspaces(std::move(workspaces)), _prefillTeammate(std::move(prefillTeammate)) {
        // The message's workspace: the open one. Its own backend reads its
        // files (not the screens' proxy, which follows the next switch).
        model::Backend *srcBackend = &ctx.backend;
        int             at         = -1;
        for (size_t i = 0; i < _workspaces.size(); ++i)
            if (_workspaces[i].store == &ctx.store()) {
                srcBackend = _workspaces[i].backend;
                at         = int(i);
            }
        if (at < 0) { // the demo, tests: the open workspace alone
            _workspaces = {ForwardWorkspace{{}, {}, &ctx.store(), &ctx.backend, {}}};
            at          = 0;
        }
        _src = contextFor(ctx, ctx.store(), *srcBackend);
        // msga: a picker above the selector when there is a choice of
        // workspace, starting on the message's.
        if (_workspaces.size() > 1) {
            std::vector<std::string> names;
            for (const ForwardWorkspace &w : _workspaces)
                names.push_back(w.name);
            _wsPicker           = content()->add<Dropdown>(std::move(names), at);
            _wsPicker->onChange = [this](int i) { setTarget(i); };
        }
        setTarget(at);
        if (const model::Message *m = _src->store().findMessage(conv, ts)) {
#ifdef MSGA_HAVE_MESSAGES
            if (_onlyFile.empty()) {
                screens::addMessagePreview(*_src, content(), *m, 140, true);
            } else {
                model::Message only = m->clone();
                only.text.clear();
                auto &files = only.extras().files;
                std::erase_if(files, [this](const model::File &f) { return f.path != _onlyFile; });
                screens::addMessagePreview(*_src, content(), only, 140, true);
            }
#endif
        }
        auto *copy = makeButton(tr("Copy link"), V::Secondary);
        _fwd       = makeButton(tr("Forward"), V::Primary);
        _fwd->setEnabled(false);
        addButtonRow(_fwd, makeButton(tr("Cancel"), V::Secondary), copy);
        copy->onClick = [this] { copyLink(); };
        _fwd->onClick = [this] { go(); };
    }
    // The views go first: they point into the Contexts below.
    ~ForwardDialog() override { clearChildren(); }
    void focusPicker() { _picker->edit().focus(); }

private:
    // msga's setTargetSession: the picker and the composer work in the
    // workspace `i` (a pick from the other one is cleared; the comment stays).
    void setTarget(int i) {
        if (i < 0 || size_t(i) >= _workspaces.size() || (i == _target && _picker))
            return;
        _target              = i;
        const std::string cm = _composer ? _composer->mrkdwn() : std::string();
        const int         at = _wsPicker ? 1 : 0;
        const bool        re = _picker != nullptr; // a workspace picked: its chats next
        if (_picker) {
            content()->remove(_picker);
            content()->remove(_composer);
            _picker   = nullptr;
            _composer = nullptr;
        }
        const ForwardWorkspace &w = _workspaces[size_t(i)];
        _tctx = w.store == &_src->store() ? nullptr : contextFor(*_src, *w.store, *w.backend);
        screens::Context &tc = _tctx ? *_tctx : *_src;
        _picker =
            static_cast<ConvSelector *>(content()->adopt(std::make_unique<ConvSelector>(tc), at));
        _composer = static_cast<Composer *>(
            content()->adopt(std::make_unique<Composer>(tc, _drafts), at + 1)
        );
        _composer->style().padding(0);
        _composer->style().maxH = 120;
        _composer->setPlaceholder(tr("Add a message, if you'd like."));
        _composer->setVoiceInput(false); // msga's forward composer had no voice source
        if (!cm.empty())
            loadMrkdwn(_composer->edit(), tc.store(), cm);
        _composer->onSendRequest = [this] {
            if (_fwd->enabled())
                go();
            return true;
        };
        _picker->onPick = [this](const Target *t) { _fwd->setEnabled(t != nullptr); };
        if (_fwd)
            _fwd->setEnabled(false);
        if (re)
            _picker->edit().focus();
    }
    void copyLink() {
        const model::Store   &st = _src->store();
        const model::Message *m  = st.findMessage(_conv, _ts);
        if (!m)
            return;
        std::string url =
            !_onlyFile.empty() ? screens::fileUrl(_onlyFile) : screens::firstLink(m->text);
        if (!url.empty())
            _src->app.platform().setClipboardText(std::move(url));
    }
    void go() {
        const Target         *t = _picker->target();
        const model::Message *m = _src->store().findMessage(_conv, _ts);
        if (!t || !m)
            return;
        const ForwardWorkspace &to   = _workspaces[size_t(_target)];
        // No attribution header: the text is re-posted as mine — verbatim in
        // its own workspace, else as portable mrkdwn (msga's forwardedText:
        // mentions and channels become the words they read as here) — after
        // the comment on its own line.
        const bool              same = to.store == &_src->store();
        const std::string       comment(str::trim(_composer->mrkdwn()));
        const std::string       fwd     = !_onlyFile.empty() ? std::string()
                                          : same ? m->text
                                                 : portableMrkdwn(_src->store(), m->text);
        const std::string       full    = comment.empty() ? fwd
                                          : fwd.empty()   ? comment
                                                          : str::concat({comment, "\n", fwd});
        // Into the pick — a DM may be opened first — once the files are
        // here, unless that workspace went meanwhile; downloaded copies go
        // when the send is over.
        auto                    deliver = [to,
                                           conv    = t->conv,
                                           person  = t->person,
                                           role    = t->role,
                                           ws      = to.store->workspaceId,
                                           prefill = _prefillTeammate,
                                           onError = _onError](std::string text, ForwardFiles ff) {
            auto cleanup = [temps = ff.temps] {
                for (const std::string &t : temps)
                    screens::removeTempDownload(t);
            };
            const auto there = [to, ws] {
                return (!to.alive || to.alive()) && to.store->workspaceId == ws;
            };
            if (!there() || (text.empty() && ff.paths.empty()))
                return cleanup();
            if (conv == kNoConv && !role.empty()) {
                // msga's prefillTeammate: the teammate's page with it all in
                // the composer, left to send (the copies stay for that send).
                if (prefill)
                    return prefill(to.key, role, std::move(text), std::move(ff.paths));
                cleanup();
                if (onError)
                    onError(tr("Couldn't open the chat."));
                return;
            }
            auto send = [to, there, text = std::move(text), paths = std::move(ff.paths), cleanup](
                            ConvRef c
                        ) {
                if (c == kNoConv || !there())
                    return cleanup();
                if (paths.empty()) {
                    to.backend->send(c, text, 0, nullptr);
                    return cleanup();
                }
                to.backend->sendWithFiles(c, text, 0, paths, [cleanup](bool, const std::string &) {
                    cleanup();
                });
            };
            if (conv != kNoConv) {
                send(conv);
            } else {
                to.backend->openDm(person, send);
            }
        };
        std::vector<model::File> files;
        for (const model::File &f : m->files())
            if (_onlyFile.empty() || f.path == _onlyFile)
                files.push_back(f);
        if (files.empty()) {
            deliver(full, {});
            accept();
            return;
        }
        // The files go along as uploads of their own bytes, fetched through
        // the workspace they live in; a target that can't take uploads gets
        // links.
        const int job = model::jobs().begin(
            files.size() == 1
                ? i18n::arg(tr("Forwarding %1"), files.front().name)
                : i18n::trn("Forwarding %n files", "Forwarding %n files", int64_t(files.size()))
        );
        fetchForwardedFiles(
            *_src,
            files,
            to.backend->capabilities().fileUpload,
            [job, full, deliver, onError = _onError](ForwardFiles ff, std::string err) {
                model::jobs().end(job);
                if (!err.empty()) {
                    LOG_WARN("shell", "forward: file download failed: %s", err.c_str());
                    if (onError)
                        onError(tr("Couldn't forward the file."));
                    return;
                }
                std::string text = full;
                for (const std::string &link : ff.links)
                    text += (text.empty() ? "" : "\n") + link;
                deliver(std::move(text), std::move(ff));
            }
        );
        accept();
    }

    ConvRef                                  _conv;
    model::Ts                                _ts;
    std::string                              _onlyFile;
    std::function<void(const std::string &)> _onError;
    std::vector<ForwardWorkspace>            _workspaces;
    int                                      _target = -1;
    std::unique_ptr<screens::Context>        _src, _tctx; // the message's; the pick's (if other)
    PrefillTeammate                          _prefillTeammate;
    DraftStash                               _drafts;
    Dropdown                                *_wsPicker = nullptr;
    ConvSelector                            *_picker   = nullptr;
    Composer                                *_composer = nullptr;
    FormButton                              *_fwd      = nullptr;
};

// ── Workspace icon ──────────────────────────────────────────────────────────

// msga's paintWorkspaceBubble at 96 px, radius 24: the picture, else the
// first letter on the workspace colour (HSL(hash·37 mod 360, 65 %, 42 %)).
class IconPreview final : public View {
public:
    IconPreview(screens::Context &ctx) : _ctx(ctx) {
        style().size(96, 96).alignSelf(Align::Center);
    }
    void set(std::shared_ptr<const gfx::Bitmap> b) {
        _bmp = std::move(b);
        update();
    }
    void paint(gfx::Painter &p) override {
        const RectF r{0, 0, 96, 96};
        if (_bmp) {
            p.save();
            p.clipRoundRect(r, 24);
            p.drawBitmap(_bmp->view(), r, gfx::Sampling::Smooth);
            p.restore();
            return;
        }
        uint32_t h = 0;
        for (char c : _ctx.store().workspaceId)
            h = h * 31 + uint8_t(c);
        p.fillRoundRect(r, 24, hsl(float((h * 37) % 360), 0.65f, 0.42f));
        const std::string &n = _ctx.store().workspaceName;
        std::string        letter =
            n.empty() ? std::string("?")
                      : std::string(1, char(n[0] >= 'a' && n[0] <= 'z' ? n[0] - 32 : n[0]));
        text::AttributedText t;
        t.append(letter, ui::pxFont(std::round(96.f * 17 / 40), text::Weight::Bold, 0xffffffffU));
        auto l = text::Layout::build(t, {}, windowScale());
        l->paint(
            p, snapPx({std::floor((96 - l->width()) / 2), std::floor((96 - l->height()) / 2)})
        );
    }

private:
    static gfx::Color hsl(float h, float s, float l) {
        const float c = (1 - std::fabs(2 * l - 1)) * s,
                    x = c * (1 - std::fabs(std::fmod(h / 60, 2.f) - 1));
        const float m = l - c / 2;
        float       r = 0, g = 0, b = 0;
        if (h < 60)
            r = c, g = x;
        else if (h < 120)
            r = x, g = c;
        else if (h < 180)
            g = c, b = x;
        else if (h < 240)
            g = x, b = c;
        else if (h < 300)
            r = x, b = c;
        else
            r = c, b = x;
        auto ch = [m](float v) { return uint32_t(std::lround((v + m) * 255)); };
        return 0xff000000U | ch(r) << 16 | ch(g) << 8 | ch(b);
    }
    screens::Context                  &_ctx;
    std::shared_ptr<const gfx::Bitmap> _bmp;
};

std::string iconDir(plat::App &app) {
    // The old app's folder: its pictures are named slack_<id>-…, ours <id>-….
    return file::join(identity::dataDir(app), "workspace_icons");
}
// msga's <stem>-<msecs>.png naming: a new file each time, so nothing caches
// the old picture under the same path.
std::vector<std::string> iconFiles(plat::App &app, const std::string &workspaceId) {
    std::vector<std::string>    out;
    std::vector<file::DirEntry> es;
    const std::string           dir = iconDir(app), stem = lower(workspaceId) + "-";
    if (file::listDir(dir, &es))
        for (const file::DirEntry &e : es)
            if (!e.isDir && e.name.rfind(stem, 0) == 0)
                out.push_back(file::join(dir, e.name));
    std::sort(out.begin(), out.end());
    return out;
}

constexpr const char *kIconHint =
    N_("Only you see this icon. The picture is cropped to a square. You can also drop an image "
       "file onto this window.");

class WorkspaceIconDialog final : public Dialog {
public:
    WorkspaceIconDialog(
        screens::Context                        &ctx,
        Avatars                                 &avatars,
        std::string                              workspaceId,
        std::string                              defaultIcon,
        std::string                              current,
        std::function<void(const std::string &)> done
    )
        : Dialog(tr("Workspace icon"), 440), _ctx(ctx), _avatars(avatars),
          _id(std::move(workspaceId)), _defaultIcon(std::move(defaultIcon)),
          _hasCustom(!iconFiles(ctx.app.platform(), _id).empty()), _done(std::move(done)) {
        content()->style().spacing(8);
        _preview = content()->add<IconPreview>(ctx);
        _preview->set(avatars.get(current, 192));
        auto *row = content()->add<View>();
        row->style().row().spacing(8).items(Align::Center);
        row->add<View>()->style().flex(1);
        auto *choose = row->add<FormButton>(tr("Choose image\xE2\x80\xA6"), V::Secondary, false);
        _reset       = row->add<FormButton>(tr("Use default"), V::Ghost, false);
        row->add<View>()->style().flex(1);
        _hint = styledLabel(
            content(),
            tr(kIconHint),
            ui::pxFont(11, text::Weight::Regular, ui::color(C::FormTextMuted))
        );
        _hint->setAlign(text::LayoutOptions::Align::Center);
        _save = makeButton(tr("Save"), V::Primary);
        addButtonRow(_save, makeButton(tr("Cancel"), V::Secondary), new View());
        choose->onClick = [this] { choose_(); };
        _reset->onClick = [this] {
            _chosen.clear();
            _resetOn = true;
            _dirty   = _hasCustom;
            _preview->set(_avatars.get(_defaultIcon, 192));
            refresh();
        };
        // Saved first: a picture that can't be stored keeps the dialog open
        // and says so (msga's "The icon could not be saved.").
        _save->onClick = [this] {
            if (!_dirty)
                return;
            if (apply())
                accept();
            else
                saveFailed();
        };
        refresh();
    }
    bool onEvent(Event &e) override {
        // Drops of an image file onto the window.
        if (e.type == EventType::DropEnter || e.type == EventType::DropMove) {
            if (!screens::dragOffersFiles(e.raw))
                return Dialog::onEvent(e);
            e.dropAction = plat::DropAction::Copy;
            return true;
        }
        if (e.type == EventType::Drop) {
            const auto p = screens::droppedFiles(e.raw);
            if (!p.empty())
                load(p[0]);
            return true;
        }
        return Dialog::onEvent(e);
    }

private:
    void refresh() {
        _reset->setVisible(_hasCustom || !_chosen.empty());
        _reset->setEnabled(!_resetOn);
        _save->setEnabled(_dirty);
    }
    void choose_() {
        plat::FileDialogDesc d;
        d.mode    = plat::FileDialogDesc::Mode::Open;
        d.title   = tr("Choose workspace icon");
        d.filters = {
            {tr("Images"), {"*.png", "*.jpg", "*.jpeg", "*.webp", "*.gif", "*.bmp", "*.svg"}}
        };
        screens::fileDialog(_ctx, std::move(d), [this](std::vector<std::string> p) {
            if (!p.empty())
                load(p[0]);
        });
    }
    void load(const std::string &path) {
        auto bmp = _avatars.get(path, 192);
        if (!bmp) {
            text::AttributedText t;
            t.append(
                tr("That file could not be read as an image."),
                ui::pxFont(11, text::Weight::Regular, ui::color(C::FormTextMuted))
            );
            _hint->setRichText(std::move(t));
            return;
        }
        _chosen  = path;
        _resetOn = false;
        _dirty   = true;
        _preview->set(std::move(bmp));
        refresh();
    }
    void saveFailed() {
        text::AttributedText t;
        t.append(
            tr("The icon could not be saved."),
            ui::pxFont(11, text::Weight::Regular, ui::color(C::FormError))
        );
        _hint->setRichText(std::move(t));
    }
    bool apply() {
        plat::App        &pa  = _ctx.app.platform();
        const std::string dir = iconDir(pa);
        const auto        old = iconFiles(pa, _id);
        std::string       now;
        if (!_chosen.empty() && !_resetOn) {
            const std::string dest = file::join(
                dir,
                str::concat({lower(_id), "-", std::to_string(base::nowMicros() / 1000), ".img"})
            );
            std::string bytes;
            if (!file::makeDirs(dir) || !file::readAll(_chosen, &bytes) ||
                !file::writeAtomic(dest, bytes)) {
                LOG_WARN("shell", "The icon could not be saved.");
                return false;
            }
            now = dest;
        }
        for (const std::string &f : old) // the previous picture goes
            file::remove(f);
        if (_done)
            _done(now);
        return true;
    }

    screens::Context                        &_ctx;
    Avatars                                 &_avatars;
    std::string                              _id, _defaultIcon; // the workspace's
    IconPreview                             *_preview = nullptr;
    FormButton                              *_reset = nullptr, *_save = nullptr;
    Label                                   *_hint = nullptr;
    std::string                              _chosen;
    bool                                     _hasCustom, _resetOn = false, _dirty = false;
    std::function<void(const std::string &)> _done;
};

// ── Tray icon ───────────────────────────────────────────────────────────────

// msga's TrayIconDialog preview: a dark panel stand-in (most trays are dark,
// and the built-in icon is white), the icon at 56 px inside it, the unread
// dot over its corner at the tray's proportions (36 of 128).
class TrayPreview final : public View {
public:
    TrayPreview() { style().size(96, 96).alignSelf(Align::Center); }
    void set(std::shared_ptr<const gfx::Bitmap> b) {
        _bmp = std::move(b);
        update();
    }
    void paint(gfx::Painter &p) override {
        p.fillRoundRect({0, 0, 96, 96}, 16, ui::color(C::Sidebar));
        const RectF icon{20, 20, 56, 56};
        if (_bmp)
            p.drawBitmap(_bmp->view(), icon, gfx::Sampling::Smooth);
        else
            gfx::drawIcon(p, gfx::Icon::Tray, icon, 0xffffffffU);
        const float d = 56.f * 36 / kTrayIconSize;
        p.fillCircle({icon.x + icon.w - d / 2, icon.y + icon.h - d / 2}, d / 2, 0xffcd2553U);
    }

private:
    std::shared_ptr<const gfx::Bitmap> _bmp;
};

// The old app's data folder (app/identity.h), where its own tray_icon.png
// is too; ours have names of their own, so a rollback finds its picture.
std::string trayDir(plat::App &app) {
    return identity::dataDir(app);
}

std::vector<std::string> trayFiles(plat::App &app) {
    std::vector<std::string>    out;
    std::vector<file::DirEntry> es;
    const std::string           dir = trayDir(app);
    if (file::listDir(dir, &es))
        for (const file::DirEntry &e : es)
            if (!e.isDir && e.name.rfind("tray_icon-", 0) == 0)
                out.push_back(file::join(dir, e.name));
    std::sort(out.begin(), out.end());
    return out;
}

class TrayIconDialog final : public Dialog {
public:
    TrayIconDialog(screens::Context &ctx, std::string current, bool monochrome, TrayIconDone done)
        : Dialog(tr("Tray icon"), 440), _ctx(ctx), _current(std::move(current)),
          _hasCustom(!_current.empty()), _done(std::move(done)) {
        content()->style().spacing(8);
        _preview  = content()->add<TrayPreview>();
        auto *row = content()->add<View>();
        row->style().row().spacing(8).items(Align::Center);
        row->add<View>()->style().flex(1);
        auto *choose = row->add<FormButton>(tr("Choose image\xE2\x80\xA6"), V::Secondary, false);
        _reset       = row->add<FormButton>(tr("Use default"), V::Ghost, false);
        row->add<View>()->style().flex(1);
        _hint = styledLabel(
            content(),
            tr("The picture is fitted into a square, and the unread dot is drawn over its corner. "
               "You can also drop an image file onto this window."),
            ui::pxFont(11, text::Weight::Regular, ui::color(C::FormTextMuted))
        );
        _hint->setAlign(text::LayoutOptions::Align::Center);
        _mono = new CheckBox(tr("Convert to monochrome"), monochrome);
        _save = makeButton(tr("Save"), V::Primary);
        addButtonRow(_save, makeButton(tr("Cancel"), V::Secondary), _mono);
        _mono->onChange = [this](bool) {
            _dirty = true; // an option alone changed (msga's markDirty)
            refreshPreview();
            refresh();
        };
        choose->onClick = [this] { choose_(); };
        _reset->onClick = [this] {
            _chosen.clear();
            _picture.reset();
            _resetOn = true;
            _dirty   = true;
            refreshPreview();
            refresh();
        };
        // Saved first: a picture that can't be stored keeps the dialog open
        // and says so (msga's "The icon could not be saved.").
        _save->onClick = [this] {
            if (!_dirty)
                return;
            if (apply())
                accept();
            else
                saveFailed();
        };
        if (_hasCustom)
            _picture = decodeFile(_current);
        refreshPreview();
        refresh();
    }
    bool onEvent(Event &e) override {
        if (e.type == EventType::DropEnter || e.type == EventType::DropMove) {
            if (!screens::dragOffersFiles(e.raw))
                return Dialog::onEvent(e);
            e.dropAction = plat::DropAction::Copy;
            return true;
        }
        if (e.type == EventType::Drop) {
            const auto p = screens::droppedFiles(e.raw);
            if (!p.empty())
                load(p[0]);
            return true;
        }
        return Dialog::onEvent(e);
    }

private:
    static std::shared_ptr<gfx::Bitmap> decodeFile(const std::string &path) {
        std::string bytes;
        auto        bmp = std::make_shared<gfx::Bitmap>();
        if (!file::readAll(path, &bytes) || !decodeTrayPicture(bytes, bmp.get()))
            return nullptr;
        return bmp;
    }
    void refreshPreview() {
        _preview->set(
            _picture ? std::make_shared<gfx::Bitmap>(trayPicture(*_picture, _mono->checked()))
                     : nullptr
        );
    }
    void refresh() {
        _reset->setVisible(_hasCustom || !_chosen.empty());
        _reset->setEnabled(!_resetOn);
        _save->setEnabled(_dirty);
    }
    void choose_() {
        plat::FileDialogDesc d;
        d.mode    = plat::FileDialogDesc::Mode::Open;
        d.title   = tr("Choose tray icon");
        d.filters = {
            {tr("Images"), {"*.png", "*.jpg", "*.jpeg", "*.webp", "*.gif", "*.bmp", "*.svg"}}
        };
        screens::fileDialog(_ctx, std::move(d), [this](std::vector<std::string> p) {
            if (!p.empty())
                load(p[0]);
        });
    }
    void load(const std::string &path) {
        auto bmp = decodeFile(path);
        if (!bmp) {
            text::AttributedText t;
            t.append(
                tr("That file could not be read as an image."),
                ui::pxFont(11, text::Weight::Regular, ui::color(C::FormTextMuted))
            );
            _hint->setRichText(std::move(t));
            return;
        }
        _chosen  = path;
        _picture = std::move(bmp);
        _resetOn = false;
        _dirty   = true;
        refreshPreview();
        refresh();
    }
    void saveFailed() {
        text::AttributedText t;
        t.append(
            tr("The icon could not be saved."),
            ui::pxFont(11, text::Weight::Regular, ui::color(C::FormError))
        );
        _hint->setRichText(std::move(t));
    }
    // The picture is copied into the data folder (the original may move or
    // vanish), under a new name each time so nothing caches the old one.
    bool apply() {
        plat::App  &pa  = _ctx.app.platform();
        const auto  old = trayFiles(pa);
        std::string now = _resetOn ? std::string() : _current;
        if (!_chosen.empty() && !_resetOn) {
            const std::string dest = file::join(
                trayDir(pa),
                str::concat({"tray_icon-", std::to_string(base::nowMicros() / 1000), ".img"})
            );
            std::string bytes;
            if (!file::makeDirs(trayDir(pa)) || !file::readAll(_chosen, &bytes) ||
                !file::writeAtomic(dest, bytes)) {
                LOG_WARN("shell", "The tray icon could not be saved.");
                return false;
            }
            now = dest;
        }
        for (const std::string &f : old)
            if (f != now)
                file::remove(f);
        if (_done)
            _done(now, _mono->checked());
        return true;
    }

    screens::Context            &_ctx;
    std::string                  _current, _chosen;
    std::shared_ptr<gfx::Bitmap> _picture; // decoded, before styling
    TrayPreview                 *_preview = nullptr;
    FormButton                  *_reset = nullptr, *_save = nullptr;
    CheckBox                    *_mono = nullptr;
    Label                       *_hint = nullptr;
    bool                         _hasCustom, _resetOn = false, _dirty = false;
    TrayIconDone                 _done;
};

} // namespace

Popup *showForwardDialog(
    screens::Context                        &ctx,
    Window                                  &w,
    ConvRef                                  conv,
    model::Ts                                ts,
    const std::string                       &onlyFile,
    std::function<void(const std::string &)> onError,
    std::vector<ForwardWorkspace>            workspaces,
    PrefillTeammate                          prefillTeammate
) {
    if (!ctx.store().findMessage(conv, ts))
        return nullptr;
    auto d = std::make_unique<ForwardDialog>(
        ctx,
        conv,
        ts,
        onlyFile,
        std::move(onError),
        std::move(workspaces),
        std::move(prefillTeammate)
    );
    auto *raw = d.get();
    w.showPopup(std::move(d));
    raw->focusPicker();
    return raw;
}

std::string portableMrkdwn(const model::Store &source, std::string_view in) {
    // The words go out as text: mrkdwn's escapes keep them from turning into
    // tokens again on the other side.
    const auto text = [](std::string &out, std::string_view s) {
        for (const char c : s)
            out += c == '&' ? "&amp;" : c == '<' ? "&lt;" : c == '>' ? "&gt;" : std::string(1, c);
    };
    std::string out;
    out.reserve(in.size());
    size_t pos = 0;
    while (pos < in.size()) {
        const size_t open  = in.find('<', pos);
        const size_t close = open == std::string_view::npos ? open : in.find('>', open);
        if (close == std::string_view::npos) {
            out.append(in.substr(pos));
            break;
        }
        out.append(in.substr(pos, open - pos));
        pos                        = close + 1;
        const std::string_view tok = in.substr(open + 1, close - open - 1);
        const size_t           bar = tok.find('|');
        const std::string_view id =
            tok.substr(1, bar == std::string_view::npos ? tok.npos : bar - 1);
        const std::string label(
            bar == std::string_view::npos ? std::string_view() : tok.substr(bar + 1)
        );
        if (tok.empty() || (tok[0] != '@' && tok[0] != '#' && tok[0] != '!')) {
            out.append(in.substr(open, close - open + 1)); // a link: the same everywhere
            continue;
        }
        std::string words;
        if (tok[0] == '@') {
            const model::UserRef u = source.findUser(id);
            words                  = "@" + (u != model::kNoUser && !source.user(u).placeholder
                                                ? std::string(source.user(u).label())
                                            : !label.empty() ? label
                                                             : std::string(id));
        } else if (tok[0] == '#') {
            const model::ConvRef c = source.findConversation(id);
            words                  = "#" + (c != model::kNoConv ? source.displayName(c)
                                            : !label.empty()    ? label
                                                                : std::string(id));
        } else if (str::startsWith(id, "subteam^")) {
            words = !label.empty() ? label : "@" + std::string(id.substr(8));
            if (words[0] != '@')
                words.insert(0, 1, '@');
        } else if (str::startsWith(id, "date^")) {
            words = label;
        } else { // here, channel, everyone: plain words, no broadcast
            words = "@" + std::string(id.substr(0, id.find('^')));
        }
        text(out, mrkdwn::decodeEntities(words));
    }
    return out;
}

bool decodeTrayPicture(std::string_view bytes, gfx::Bitmap *out) {
    // SVG first, rendered straight at the stored size (a 24 px icon would
    // otherwise be upscaled blurry).
    float w = 0, h = 0;
    if (gfx::svgSize(bytes, &w, &h) && w > 0 && h > 0) {
        const float k = float(kTrayIconSize) / std::max(w, h);
        return gfx::renderSvg(
            bytes, std::max(1, int(std::lround(w * k))), std::max(1, int(std::lround(h * k))), out
        );
    }
    return gfx::decodeImage(bytes, out) && !out->empty();
}

gfx::Bitmap trayPicture(const gfx::Bitmap &src, bool monochrome) {
    // Fitted into the square, aspect kept and centred: a logo must not lose
    // its edges to a crop.
    constexpr int n = kTrayIconSize;
    gfx::Bitmap   out(n, n);
    if (src.empty())
        return out;
    const float k  = std::min(float(n) / src.width(), float(n) / src.height());
    const int   w  = std::max(1, int(std::lround(src.width() * k)));
    const int   h  = std::max(1, int(std::lround(src.height() * k)));
    gfx::Bitmap sc = gfx::resize(src.view(), w, h);
    const int   x0 = (n - w) / 2, y0 = (n - h) / 2;
    for (int y = 0; y < h; ++y)
        std::copy_n(sc.pixels() + size_t(y) * w, w, out.pixels() + size_t(y + y0) * n + x0);
    if (monochrome)
        trayMonochrome(&out);
    return out;
}

void trayMonochrome(gfx::Bitmap *bmp) {
    // msga's toMonochrome: a white silhouette. The shape comes from the alpha
    // channel when the picture has transparency inside its visible area (a
    // logo cut out of its background); an opaque one (a logo on a solid
    // backdrop, a photo) keys on each pixel's colour distance from the
    // backdrop sampled at the corners, so dark-on-light and light-on-dark
    // logos both come out as their mark.
    const int  W = bmp->width(), H = bmp->height();
    uint32_t  *px    = bmp->pixels();
    const auto alpha = [](uint32_t c) { return int(c >> 24); };
    int        x0 = W, y0 = H, x1 = -1, y1 = -1;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (alpha(px[y * W + x]) > 0) {
                x0 = std::min(x0, x), y0 = std::min(y0, y);
                x1 = std::max(x1, x), y1 = std::max(y1, y);
            }
    if (x1 < 0)
        return; // fully transparent: nothing to shape
    bool translucent = false;
    for (int y = y0; y <= y1 && !translucent; ++y)
        for (int x = x0; x <= x1; ++x)
            if (alpha(px[y * W + x]) < 250) {
                translucent = true;
                break;
            }
    const auto ch = [](uint32_t c, int shift) { return int((c >> shift) & 0xff); };
    int        br = 0, bg = 0, bb = 0, maxDist = 1;
    const auto dist = [&](uint32_t c) {
        return std::max(
            std::max(std::abs(ch(c, 16) - br), std::abs(ch(c, 8) - bg)), std::abs(ch(c, 0) - bb)
        );
    };
    if (!translucent) {
        for (const uint32_t c :
             {px[y0 * W + x0], px[y0 * W + x1], px[y1 * W + x0], px[y1 * W + x1]})
            br += ch(c, 16), bg += ch(c, 8), bb += ch(c, 0);
        br /= 4, bg /= 4, bb /= 4;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                maxDist = std::max(maxDist, dist(px[y * W + x]));
    }
    // A flat picture (one colour, give or take compression noise) has no
    // mark to key out: the whole square is the silhouette.
    constexpr int kFlatContrast = 24;
    const bool    flat          = !translucent && maxDist < kFlatContrast;
    for (int i = 0; i < W * H; ++i) {
        const int a0 = alpha(px[i]);
        const int a =
            translucent || flat || a0 == 0 ? a0 : std::min(255, dist(px[i]) * 2 * 255 / maxDist);
        px[i] = uint32_t(a) << 24 | uint32_t(a) << 16 | uint32_t(a) << 8 |
                uint32_t(a); // premultiplied white
    }
}

Popup *showTrayIconDialog(
    screens::Context &ctx, Window &w, const std::string &current, bool monochrome, TrayIconDone done
) {
    auto  d   = std::make_unique<TrayIconDialog>(ctx, current, monochrome, std::move(done));
    auto *raw = d.get();
    w.showPopup(std::move(d));
    return raw;
}

std::string customWorkspaceIconPath(plat::App &app, const std::string &workspaceId) {
    const auto files = iconFiles(app, workspaceId);
    return files.empty() ? std::string() : files.back();
}

Popup *showRenameDialog(screens::Context &ctx, Window &w, ConvRef c) {
    if (c >= ctx.store().conversationCount())
        return nullptr;
    const model::Conversation &cv    = ctx.store().conversation(c);
    const bool                 agent = ctx.backend.isAgentSession(c);
    auto d = std::make_unique<Dialog>(agent ? tr("Rename session") : tr("Name conversation"));
    styledLabel(d->content(), tr("Name"), pxFont(15, text::Weight::Bold, color(C::FormText)));
    auto *field = d->content()->add<TextField>(
        // Empty: the placeholder is the name it falls back to (derivedName).
        cv.localName.empty() ? std::string(ctx.store().displayName(c)) : std::string(),
        TextField::Size::Normal
    );
    field->setMaxLength(80); // Slack's channel-name cap
    field->setText(cv.localName);
    field->edit().selectAll();
    styledLabel(
        d->content(),
        agent ? tr("Only msga shows this name; Claude Code keeps its own. Leave it empty to use "
                   "Claude Code's name again.")
              : tr("Only you see this name. Leave it empty to show the members' names again."),
        pxFont(11, text::Weight::Regular, color(C::FormTextMuted))
    );
    auto *save = Dialog::makeButton(tr("Save"), V::Primary);
    d->addButtonRow(save, Dialog::makeButton(tr("Cancel"), V::Secondary));
    Dialog *raw   = d.get();
    // Read the field before accept() closes the dialog.
    save->onClick = [raw, field, &ctx, c] {
        std::string name(str::trim(field->text()));
        raw->accept();
        ctx.backend.setLocalName(c, std::move(name));
    };
    field->onReturn = save->onClick;
    w.showPopup(std::move(d));
    field->edit().focus();
    return raw;
}

Popup *showWorkspaceIconDialog(
    screens::Context                        &ctx,
    Window                                  &w,
    Avatars                                 &avatars,
    std::function<void(const std::string &)> done
) {
    return showWorkspaceIconDialog(
        ctx, w, avatars, ctx.store().workspaceId, ctx.store().workspaceIcon, std::move(done)
    );
}

Popup *showWorkspaceIconDialog(
    screens::Context                        &ctx,
    Window                                  &w,
    Avatars                                 &avatars,
    const std::string                       &workspaceId,
    const std::string                       &defaultIcon,
    std::function<void(const std::string &)> done
) {
    std::string cur = customWorkspaceIconPath(ctx.app.platform(), workspaceId);
    if (cur.empty())
        cur = defaultIcon;
    auto d = std::make_unique<WorkspaceIconDialog>(
        ctx, avatars, workspaceId, defaultIcon, cur, std::move(done)
    );
    auto *raw = d.get();
    w.showPopup(std::move(d));
    return raw;
}

} // namespace shell
