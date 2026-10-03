#include "screens/shell/session_dialogs.h"

#include "app/claude/avatar_glyphs.h"
#include "base/i18n.h"
#include "base/str.h"
#include "base/time.h"
#include "base/utf8.h"
#include "gfx/icons_generated.h"
#include "ui/controls.h"

#include <algorithm>
#include <cmath>
#include <memory>

using namespace ui;
using gfx::Icon;
using i18n::tr;
using AgentRole = model::Backend::AgentRole;
using V         = FormButton::Kind;

namespace shell {

namespace {

// The browse list's metrics.
constexpr float kRowH = BrowseList::kRowH, kAvatar = 36, kRowPadH = 24;

std::string lowered(std::string_view s) {
    return utf8::foldCase(s);
}

// One row: the picture (or a channel's icon), the title over its subtitle,
// the badge. Recycled: bind() refills it for another item.
class BrowseRow final : public Clickable {
public:
    BrowseRow() {
        // Keyboard selection reads stronger than hover.
        setLook({C::None, C::FormHighlight, C::FormHighlight, C::FormHighlightStrong, 0});
        setRole(Role::ListItem);
        setFocusable(false);
        style().row().height(kRowH).padding(kRowPadH, 0).items(Align::Center).noShrink();
    }
    void bind(Avatars &avatars, const BrowseList::Item &it, bool onContent, float radius) {
        clearChildren();
        const bool channel = it.titleIcon != 0xffff;
        if (!channel) {
            auto *av = add<Avatar>();
            av->style().size(kAvatar, kAvatar).margins(0, 0, 12, 0).noShrink();
            if (radius < 0)
                av->setCircle(true);
            else
                av->setRadius(radius);
            av->setPlaceholder(C::PresenceAway);
            av->setInitial(it.title);
            av->setBitmap(avatars.get(it.avatar, int(kAvatar * 2)));
        }
        const C primary = onContent ? C::Text : C::FormText;
        const C muted   = onContent ? C::TextMuted : C::FormTextMuted;
        auto   *text    = add<View>();
        text->style().flex(1).spacing(1);
        text->style().shrink = 1;
        View *titleLine      = text;
        if (channel) { // the hash / lock 6 px before the title
            titleLine = text->add<View>();
            titleLine->style().row().items(Align::Center).spacing(6);
            titleLine->add<IconView>(Icon(it.titleIcon), 14, muted)->style().size(14, 14);
        }
        auto *title = titleLine->add<Label>(it.title, Font::BodyBold, primary);
        title->setMaxLines(1);
        title->style().shrink = 1;
        if (!it.subtitle.empty())
            styledLabel(
                text, it.subtitle, pxFont(15 * 0.88f, text::Weight::Regular, themed(muted)), 1
            );
        if (!it.badge.empty()) {
            auto *badge = add<View>();
            badge->style().row().items(Align::Center).spacing(4).margins(12, 0, 0, 0).noShrink();
            if (it.badgeCheck)
                badge->add<IconView>(Icon::Check, 13, muted)->style().size(13, 13);
            styledLabel(
                badge,
                it.badge,
                pxFont(
                    15 * 0.88f,
                    it.badgeStrong ? text::Weight::Bold : text::Weight::Regular,
                    themed(it.badgeStrong ? primary : muted)
                ),
                1
            );
        }
    }
    bool onEvent(Event &e) override {
        // The list activates on press.
        if (e.type == EventType::PointerDown && e.button == plat::Button::Left) {
            activate();
            return true;
        }
        return Clickable::onEvent(e);
    }
};

// A 32 px close button with a 14 px cross: flat, a round hover wash.
class CloseX final : public Clickable {
public:
    CloseX() {
        style().size(32, 32).noShrink();
        setFocusable(false);
        setLook({C::None, C::None, C::None, C::None, 0});
    }
    void paint(gfx::Painter &p) override {
        if (hovered())
            p.fillCircle({16, 16}, 16, color(C::FormHighlight));
        gfx::drawIcon(p, Icon::X, {9, 9, 14, 14}, color(C::FormIcon));
    }
};

// ── Session finder ──────────────────────────────────────────────────────────

class SessionFinder final : public Dialog {
public:
    SessionFinder(
        screens::Context                        &ctx,
        Avatars                                 &avatars,
        std::function<void(const std::string &)> pick,
        std::function<void()>                    create
    )
        : Dialog(std::string(), 720), _ctx(ctx), _pick(std::move(pick)),
          _create(std::move(create)) {
        // A bare card: its own chrome, the list edge to edge.
        content()->style().spacing(0).minH = 520;

        auto *top = content()->add<View>();
        top->style()
            .row()
            .items(Align::Center)
            .spacing(8)
            .padding(24, 20, 24 - Dialog::kCloseInset, 12);
        _search = top->add<TextField>(
            tr("Search for sessions"), TextField::Size::Normal, uint16_t(Icon::Search)
        );
        _search->style().flex(1).minW = 200;
        auto *createBtn = top->add<FormButton>(tr("Create a session"), V::Primary, false);
        createBtn->setFocusable(false);
        createBtn->onClick = [this] {
            auto cb = _create;
            reject();
            if (cb)
                cb();
        };
        top->add<CloseX>()->onClick = [this] { reject(); };

        content()->add<Separator>(false, C::FormDivider);

        auto *stack                         = content()->add<View>();
        stack->style().stack().flex(1).minH = 400;
        _status = stack->add<Label>(tr("Looking for sessions…"), Font::Body, C::FormTextMuted);
        _status->setAlign(text::LayoutOptions::Align::Center);
        _status->style().alignSelf(Align::Center).padding(24, 0);
        _list = stack->add<BrowseList>(avatars);
        _list->setVisible(false);
        _list->onActivated = [this](const std::string &id) {
            auto cb = _pick;
            accept();
            if (cb)
                cb(id);
        };

        _search->edit().onChange = [this] { applyFilter(); };
        // The search field drives the list: arrows move, Enter opens.
        _search->edit().onKey    = [this](const Event &e) {
            if (e.type != EventType::KeyDown || !_list->visible())
                return false;
            switch (e.key) {
            case plat::Key::Down:
                _list->moveSelection(1);
                return true;
            case plat::Key::Up:
                _list->moveSelection(-1);
                return true;
            case plat::Key::Enter:
            case plat::Key::KpEnter:
                _list->activateSelected();
                return true;
            default:
                return false;
            }
        };
    }

    void setSessions(const std::vector<model::Backend::FoundSession> &sessions) {
        const std::string home = _ctx.app.platform().standardDir(plat::StandardDir::Home);
        const int64_t     now  = base::nowSecs();
        std::vector<BrowseList::Item> items;
        items.reserve(sessions.size());
        for (const auto &s : sessions)
            items.push_back(foundSessionItem(s, home, now));
        _loaded = true;
        _list->setItems(std::move(items));
        applyFilter();
    }
    TextField &search() { return *_search; }

private:
    void applyFilter() {
        if (!_loaded)
            return;
        _list->applyFilter(_search->text());
        if (_list->visibleCount() == 0) {
            _status->setText(
                _list->count() == 0 ? tr("There are no sessions yet.") : tr("No sessions match.")
            );
            _status->setVisible(true);
            _list->setVisible(false);
            return;
        }
        _status->setVisible(false);
        _list->setVisible(true);
        _list->setSelectedRow(0);
    }

    screens::Context                        &_ctx;
    std::function<void(const std::string &)> _pick;
    std::function<void()>                    _create;
    TextField                               *_search = nullptr;
    Label                                   *_status = nullptr;
    BrowseList                              *_list   = nullptr;
    bool                                     _loaded = false;
};

// ── Teammate editor ─────────────────────────────────────────────────────────

constexpr float kPreview = 64, kSwatch = 26;
constexpr int   kGlyphCols = 14;

std::shared_ptr<const gfx::Bitmap> tile(const std::string &glyph, uint32_t color, int px) {
    auto b = std::make_shared<gfx::Bitmap>();
    if (!gfx::renderSvg(claude::avatar_glyphs::svg(glyph, color), px, px, b.get()))
        b.reset();
    return b;
}

// One pick: a glyph tile (in the chosen colour) or a colour dot; the chosen
// one ringed in the accent colour, a hovered one in the subtle divider.
class GlyphSwatch final : public Clickable {
public:
    GlyphSwatch(std::string glyph, uint32_t color, bool isColor)
        : glyph(std::move(glyph)), color(color), isColor(isColor) {
        style().size(kSwatch + 6, kSwatch + 6).noShrink();
        setLook({C::None, C::None, C::None, C::None, 0});
        setFocusable(false);
        setHoverRepaint(true);
        if (!isColor)
            setTooltip(this->glyph);
    }
    void setTileColor(uint32_t c) {
        color = c;
        _bmp.reset();
        update();
    }
    void paint(gfx::Painter &p) override {
        const RectF inner{3, 3, kSwatch, kSwatch};
        if (isColor) {
            p.fillCircle({3 + kSwatch / 2, 3 + kSwatch / 2}, kSwatch / 2 - 4, 0xff000000U | color);
        } else {
            const float k  = windowScale();
            const int   px = int(std::lround(kSwatch * k));
            if (!_bmp || _bmp->width() != px)
                _bmp = tile(glyph, color, px);
            if (_bmp)
                p.drawBitmap(_bmp->view(), inner);
        }
        if (checked() || hovered()) {
            const Color ring = ui::color(checked() ? C::Accent : C::FormDivider);
            const RectF r{1, 1, width() - 2, height() - 2};
            if (isColor)
                p.strokeCircle({width() / 2, height() / 2}, r.w / 2, 2, ring);
            else
                p.strokeRoundRect(r, 8, 2, ring);
        }
    }
    std::string glyph;
    uint32_t    color;
    bool        isColor;

private:
    std::shared_ptr<const gfx::Bitmap> _bmp;
};

class TeammateDialog final : public Dialog {
public:
    TeammateDialog(const AgentRole &role, std::function<void(const AgentRole &, bool)> done)
        : Dialog(role.id.empty() ? tr("Add a teammate") : tr("Edit teammate")), _role(role),
          _done(std::move(done)) {
        namespace ag = claude::avatar_glyphs;
        _glyph       = ag::hasGlyph(role.glyph) ? role.glyph : std::string(ag::glyphs().front().id);
        _color       = role.color ? role.color : ag::colors()[1];

        const text::Style bold = pxFont(15, text::Weight::Bold, themed(C::FormText));
        const text::Style hint = pxFont(11, text::Weight::Regular, themed(C::FormTextMuted));

        // ── Picture preview beside the name and description ──
        auto *top = content()->add<View>();
        top->style().row().spacing(16);
        _preview = top->add<Image>();
        _preview->style().size(kPreview, kPreview).noShrink().alignSelf(Align::Start);
        auto *fields = top->add<View>();
        fields->style().flex(1).spacing(4);
        styledLabel(fields, tr("Name"), bold);
        _name = fields->add<TextField>(tr("Copywriter"), TextField::Size::Normal);
        _name->setMaxLength(40);
        _name->setText(role.name);
        fields->add<View>()->style().height(4);
        styledLabel(fields, tr("Description"), bold);
        _desc = fields->add<TextField>(
            tr("Writes clear, friendly product copy."), TextField::Size::Normal
        );
        _desc->setMaxLength(120);
        _desc->setText(role.description);

        // ── Picture: a glyph and a colour ──
        styledLabel(content(), tr("Picture"), bold)->style().margins(0, 4, 0, 0);
        auto *grid = content()->add<View>();
        grid->style().spacing(0);
        View *line = nullptr;
        int   i    = 0;
        for (const auto &g : ag::glyphs()) {
            if (i++ % kGlyphCols == 0) {
                line = grid->add<View>();
                line->style().row();
            }
            auto *b    = line->add<GlyphSwatch>(g.id, _color, false);
            b->onClick = [this, id = std::string(g.id)] { pickGlyph(id); };
            _glyphs.push_back(b);
        }
        auto *colors = content()->add<View>();
        colors->style().row();
        for (uint32_t c : ag::colors()) {
            auto *b    = colors->add<GlyphSwatch>(std::string(), c, true);
            b->onClick = [this, c] { pickColor(c); };
            _colors.push_back(b);
        }

        // ── Instructions ──
        styledLabel(content(), tr("Instructions"), bold)->style().margins(0, 4, 0, 0);
        _prompt = content()->add<TextField>(
            tr("You are the team's copywriter. You write short, friendly copy in the product's "
               "voice…"),
            true
        );
        _prompt->style().height(170);
        _prompt->setText(role.prompt);
        _prompt->edit().setSelection(0, 0); // shown from the top
        std::string about =
            tr("Added to Claude Code's own instructions: what the teammate focuses on, how it "
               "works, what to avoid.");
        if (!role.id.empty())
            about = str::concat(
                {about,
                 " ",
                 tr("Sessions already started keep the instructions they began with; new sessions "
                    "get these.")}
            );
        styledLabel(content(), std::move(about), hint);

        // ── Buttons ──
        _save = makeButton(role.id.empty() ? tr("Add teammate") : tr("Save"), V::Primary);
        FormButton *restore = nullptr;
        if (role.builtIn && role.edited) {
            restore          = makeButton(tr("Restore default"), V::Ghost);
            restore->onClick = [this] {
                auto cb = std::move(_done);
                accept();
                if (cb)
                    cb({}, true);
            };
        }
        addButtonRow(_save, makeButton(tr("Cancel"), V::Secondary), restore);
        _save->onClick = [this] {
            const AgentRole r  = result();
            auto            cb = std::move(_done);
            accept();
            if (cb)
                cb(r, false);
        };
        auto prevName          = _name->edit().onChange; // the length counter
        _name->edit().onChange = [this, prevName] {
            if (prevName)
                prevName();
            _save->setEnabled(!str::trim(_name->text()).empty());
        };
        pickGlyph(_glyph);
        pickColor(_color);
        _save->setEnabled(!str::trim(_name->text()).empty());
    }

    TextField &name() { return *_name; }

private:
    AgentRole result() const {
        AgentRole r   = _role;
        r.name        = str::simplified(_name->text());
        r.description = str::simplified(_desc->text());
        r.glyph       = _glyph;
        r.color       = _color;
        r.prompt      = std::string(str::trim(_prompt->text()));
        return r;
    }
    void pickGlyph(const std::string &id) {
        _glyph = id;
        for (auto *b : _glyphs)
            b->setChecked(b->glyph == id);
        updatePreview();
    }
    void pickColor(uint32_t c) {
        _color = c;
        for (auto *b : _colors)
            b->setChecked(b->color == c);
        for (auto *b : _glyphs)
            b->setTileColor(c);
        updatePreview();
    }
    void updatePreview() {
        const float k = window() ? window()->scale() : 2.f;
        _preview->setBitmap(tile(_glyph, _color, int(std::lround(kPreview * std::max(k, 1.f)))));
    }

    AgentRole                                    _role;
    std::function<void(const AgentRole &, bool)> _done;
    std::string                                  _glyph;
    uint32_t                                     _color   = 0;
    Image                                       *_preview = nullptr;
    TextField                                   *_name = nullptr, *_desc = nullptr;
    TextField                                   *_prompt = nullptr;
    FormButton                                  *_save   = nullptr;
    std::vector<GlyphSwatch *>                   _glyphs, _colors;
};

} // namespace

Clickable *addDialogCloseButton(View *parent) {
    return parent->add<CloseX>();
}

// ── BrowseList ──────────────────────────────────────────────────────────────

std::unique_ptr<View> BrowseListData::Rows::create(int) {
    return std::make_unique<BrowseRow>();
}

void BrowseListData::Rows::bind(View &row, int index) {
    static_cast<BrowseList *>(data)->bindRow(row, index);
}

float BrowseListData::Rows::estimateHeight(int) const {
    return kRowH;
}

BrowseList::BrowseList(Avatars &avatars) : VirtualList(&rows), _avatars(avatars) {
    style().flex(1);
}

BrowseList::~BrowseList() = default;

void BrowseList::setItems(std::vector<Item> list) {
    items = std::move(list);
    applyFilter({});
}

void BrowseList::applyFilter(std::string_view query) {
    const std::string q = lowered(str::trim(query));
    shown.clear();
    for (size_t i = 0; i < items.size(); ++i)
        if (q.empty() || items[i].searchKey.find(q) != std::string::npos)
            shown.push_back(i);
    _selected = -1; // a re-filtered list has a different nth row
    reset();
    scrollTo(0);
}

void BrowseList::bindRow(View &v, int index) {
    auto &row = static_cast<BrowseRow &>(v);
    row.bind(_avatars, items[shown[size_t(index)]], _onContent, _radius);
    row.setChecked(index == _selected);
    row.onClick = [this, id = items[shown[size_t(index)]].id] {
        if (onActivated)
            onActivated(id);
    };
}

void BrowseList::setSelectedRow(int row) {
    const int n = int(shown.size());
    row         = row < 0 || n == 0 ? -1 : std::min(row, n - 1);
    if (row == _selected)
        return;
    if (View *old = _selected >= 0 ? viewFor(_selected) : nullptr)
        static_cast<Clickable *>(old)->setChecked(false);
    _selected = row;
    if (row < 0)
        return;
    if (View *now = viewFor(row))
        static_cast<Clickable *>(now)->setChecked(true);
    scrollToItem(row, ItemAlign::Nearest, false);
}

void BrowseList::moveSelection(int delta) {
    const int n = int(shown.size());
    if (n == 0 || delta == 0)
        return;
    // Wrap, so Up from the top lands on the last match.
    const int from = _selected < 0 ? (delta > 0 ? -1 : 0) : _selected;
    setSelectedRow(((from + delta) % n + n) % n);
}

void BrowseList::activateSelected() {
    if (_selected < 0 || _selected >= int(shown.size()) || !onActivated)
        return;
    onActivated(items[shown[size_t(_selected)]].id);
}

void BrowseList::paint(gfx::Painter &p) {
    p.fillRect(bounds(), color(_onContent ? C::Surface : C::FormBg));
    VirtualList::paint(p);
}

BrowseList::Item
foundSessionItem(const model::Backend::FoundSession &s, const std::string &home, int64_t nowSecs) {
    BrowseList::Item it;
    it.id     = s.id;
    it.avatar = s.avatar; // the agent's picture, no channel icon
    // Untitled: what was asked first stands in for a title.
    it.title  = !s.title.empty() ? s.title : !s.firstPrompt.empty() ? s.firstPrompt : s.lastPrompt;
    std::string folder = s.folder;
    if (!home.empty() && (folder == home || str::startsWith(folder, home + "/")))
        folder = "~" + folder.substr(home.size());
    std::vector<std::string> sub;
    if (!folder.empty())
        sub.push_back(folder);
    if (s.lastActiveMs > 0)
        sub.push_back(base::relativeTime(s.lastActiveMs / 1000, nowSecs));
    if (!s.lastPrompt.empty() && s.lastPrompt != it.title)
        sub.push_back(s.lastPrompt);
    for (size_t i = 0; i < sub.size(); ++i)
        it.subtitle += (i ? " \xC2\xB7 " : "") + sub[i];
    if (s.listed != model::kNoConv)
        it.badge = tr("In the list");
    it.searchKey = lowered(
        str::concat({s.title, "\n", s.folder, "\n", s.firstPrompt, "\n", s.lastPrompt, "\n", s.id})
    );
    return it;
}

// ── Entry points ────────────────────────────────────────────────────────────

Popup *showSessionFinder(
    screens::Context                        &ctx,
    Window                                  &w,
    Avatars                                 &avatars,
    std::function<void(const std::string &)> pick,
    std::function<void()>                    create
) {
    auto  d     = std::make_unique<SessionFinder>(ctx, avatars, std::move(pick), std::move(create));
    auto *raw   = d.get();
    // The list arrives later; the dialog may be closed by then.
    auto  alive = std::make_shared<bool>(true);
    raw->onClosed = [alive] { *alive = false; };
    ctx.backend.findAgentSessions([raw, alive](std::vector<model::Backend::FoundSession> s) {
        if (*alive)
            raw->setSessions(s);
    });
    w.showPopup(std::move(d));
    raw->search().edit().focus();
    return raw;
}

Popup *showTeammateDialog(
    Window &w, const AgentRole &role, std::function<void(const AgentRole &, bool)> done
) {
    auto  d   = std::make_unique<TeammateDialog>(role, std::move(done));
    auto *raw = d.get();
    w.showPopup(std::move(d));
    raw->name().edit().focus();
    return raw;
}

Popup *showRemoveTeammateDialog(Window &w, const std::string &name, std::function<void()> remove) {
    auto d = Dialog::confirm(
        tr("Remove teammate"),
        i18n::arg(
            tr("Remove the %1 from the team? Its sessions stay in the list, with its name and "
               "picture."),
            name
        ),
        tr("Remove"),
        V::Danger,
        themed(C::FormText)
    );
    Dialog *raw   = d.get();
    d->onAccepted = std::move(remove);
    w.showPopup(std::move(d));
    return raw;
}

} // namespace shell
