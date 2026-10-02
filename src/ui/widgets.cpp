#include "ui/widgets.h"

#include <algorithm>
#include <cmath>

namespace ui {

namespace {

float windowScale(const View *v) {
    return v->window() ? v->window()->scale() : 1.f;
}

std::unique_ptr<text::Layout>
buildPlain(std::string_view s, const text::Style &st, float scale, float maxWidth = kInf) {
    text::AttributedText t;
    t.append(s, st);
    text::LayoutOptions o;
    o.maxWidth = maxWidth;
    return text::Layout::build(t, o, scale);
}

} // namespace

// ── Label ───────────────────────────────────────────────────────────────────

Label::Label(std::string text, Font f, C color) : _text(std::move(text)), _font(f), _color(color) {
    setRole(Role::Text);
    updateInk();
}

// Glyphs paint outside the layout box (a J's hook and italic overhang to the
// sides, accents and descenders past the line box): damage and culling
// extend by a margin that scales with the largest font in the label. The old
// extent is repainted first, so shrinking text leaves no ink behind.
void Label::updateInk() {
    float size = font(_font).size;
    if (_rich)
        for (const text::Span &s : _rich->spans)
            size = std::max(size, s.style.size);
    const uint8_t outset = uint8_t(std::clamp(std::ceil(size * 0.3f), 2.f, 255.f));
    if (outset != paintOutset()) {
        update();
        setPaintOutset(outset);
    }
}

// The selected range and its look (the text white, built on demand).
struct Label::Selection {
    uint32_t                      from = 0, to = 0;
    std::unique_ptr<text::Layout> layout;
};

Label::~Label() = default;

uint32_t Label::selectionFrom() const {
    return _sel ? _sel->from : 0;
}

uint32_t Label::selectionTo() const {
    return _sel ? _sel->to : 0;
}

void Label::dropLayout() {
    _layout.reset();
    if (_sel)
        _sel->layout.reset();
    _layoutW = -1;
}

void Label::setText(std::string text) {
    if (!_rich && text == _text)
        return;
    update(); // the old text's extent
    _text = std::move(text);
    _rich.reset();
    updateInk();
    dropLayout();
    invalidateLayout();
    update();
}

void Label::setRichText(text::AttributedText t) {
    update();
    _text = t.text;
    _rich = std::make_unique<text::AttributedText>(std::move(t));
    updateInk();
    dropLayout();
    invalidateLayout();
    update();
}

void Label::setFont(Font f) {
    if (f == _font)
        return;
    update();
    _font = f;
    updateInk();
    dropLayout();
    invalidateLayout();
    update();
}

void Label::setColor(C c) {
    if (c == _color)
        return;
    _color = c;
    dropLayout(); // the colour is baked into the glyph runs
    update();
}

void Label::setMaxLines(int n) {
    _maxLines = uint8_t(std::clamp(n, 0, 255));
    dropLayout();
    invalidateLayout();
}

void Label::setAlign(text::LayoutOptions::Align a) {
    _align = a;
    dropLayout();
    update();
}

void Label::setLineHeight(float m) {
    _lineHeight = m;
    dropLayout();
    invalidateLayout();
}

void Label::styleChanged() {
    dropLayout();
    update();
}

// selected: the text as a selection shows it, white over the highlight
// (msga's QPalette::HighlightedText; mention pills lose their fill).
std::unique_ptr<text::Layout> Label::buildLayout(float w, float scale, bool selected) const {
    text::AttributedText t;
    if (_rich) {
        t = *_rich;
        resolveSpans(t);
    } else {
        t.append(_text, font(_font, _color));
    }
    // A background pads its span (inline code, mentions), so the fill is only
    // made transparent: the white copy must keep the exact same geometry.
    if (selected)
        for (text::Span &sp : t.spans) {
            sp.style.color = 0xffffffff;
            if (sp.style.background)
                sp.style.background = 0x00ffffff;
        }
    text::LayoutOptions o;
    // One physical pixel of slack: snapping flex edges to the pixel grid can
    // hand a label a frame that much narrower than the width it measured,
    // which must not wrap its last word (the ink margin covers the overhang).
    o.maxWidth   = w + 1.f / (window() ? window()->scale() : 1.f);
    o.maxLines   = _maxLines;
    o.ellipsis   = _maxLines > 0;
    o.align      = _align;
    o.lineHeight = _lineHeight;
    return text::Layout::build(t, o, scale);
}

const text::Layout *Label::layoutFor(float w) {
    const float slack = 1.f / (window() ? window()->scale() : 1.f);
    const float scale = windowScale(this);
    if (_layout && _layoutScale == scale) {
        if (w == _layoutW)
            return _layout.get();
        // Left-aligned text that fitted without being cut lays out the same
        // at any width between its tight width and the width it was built
        // for (wider too, when it is a single line): skip the rebuild.
        const bool left = _align == text::LayoutOptions::Align::Left;
        if (left && !_layout->truncated() && _layout->width() <= w + slack + 0.01f &&
            (w <= _layoutW || _layout->lineCount() <= 1))
            return _layout.get();
    }
    _layout = buildLayout(w, scale, false);
    if (_sel)
        _sel->layout.reset();
    _layoutW     = w;
    _layoutScale = scale;
    return _layout.get();
}

SizeF Label::measureContent(float aw, float ah) {
    if (_text.empty() && !_rich)
        return {0, 0};
    const text::Layout *l = layoutFor(aw);
    return {std::ceil(l->width()), std::ceil(l->height())};
}

// Top-left of the text: inside the padding, centred vertically in the rest.
PointF Label::textOrigin() const {
    const Style &s  = currentStyle();
    const float  h  = _layout ? _layout->height() : 0;
    const float  ch = height() - s.pad.t - s.pad.b;
    return snapPx({s.pad.l, s.pad.t + std::max(0.f, std::floor((ch - h) / 2))});
}

void Label::paint(gfx::Painter &p) {
    View::paint(p);
    if (_text.empty() && !_rich)
        return;
    const Style &s = currentStyle();
    layoutFor(std::max(0.f, width() - s.pad.l - s.pad.r));
    const PointF o = textOrigin();
    _layout->paint(p, o);
    if (!_sel)
        return;
    // msga's message selection: the system highlight, the text on it white.
    if (!_sel->layout)
        _sel->layout = buildLayout(_layoutW, _layoutScale, true);
    for (RectF r : _layout->selectionRects(_sel->from, _sel->to)) {
        const RectF rr{r.x + o.x, r.y + o.y, r.w, r.h};
        p.save();
        p.clipRect(rr);
        p.fillRect(rr, systemHighlight());
        _sel->layout->paint(p, o);
        p.restore();
    }
}

void Label::setSelection(uint32_t from, uint32_t to) {
    const uint32_t n = uint32_t(_text.size());
    from             = std::min(from, n);
    to               = std::min(to, n);
    if (to < from)
        std::swap(from, to);
    if (from == to)
        from = to = 0;
    if (from == selectionFrom() && to == selectionTo())
        return;
    if (from == to) {
        _sel.reset();
    } else {
        if (!_sel)
            _sel = std::make_unique<Selection>();
        _sel->from = from;
        _sel->to   = to;
    }
    update();
}

uint32_t Label::offsetAt(PointF p) const {
    if (!_layout)
        return 0;
    const PointF o = textOrigin();
    return _layout->hitTest({p.x - o.x, p.y - o.y}).offset;
}

bool Label::onEvent(Event &e) {
    if (!onLink || !_layout)
        return false;
    const PointF o = textOrigin();
    if (e.type == EventType::PointerDown && e.button == plat::Button::Left) {
        const text::HitResult h = _layout->hitTest({e.pos.x - o.x, e.pos.y - o.y});
        _pressedLink            = h.inside ? h.linkId : 0;
        return _pressedLink != 0;
    }
    if (e.type == EventType::PointerUp && _pressedLink) {
        const text::HitResult h  = _layout->hitTest({e.pos.x - o.x, e.pos.y - o.y});
        const uint32_t        id = _pressedLink;
        _pressedLink             = 0;
        if (h.inside && h.linkId == id) {
            auto cb = onLink;
            cb(id);
        }
        return true;
    }
    return false;
}

uint32_t Label::linkAt(PointF p) const {
    if (!_layout)
        return 0;
    const PointF          o = textOrigin();
    const text::HitResult h = _layout->hitTest({p.x - o.x, p.y - o.y});
    return h.inside ? h.linkId : 0;
}

uint8_t Label::cursorAt(PointF p) const {
    if (onLink && _layout) {
        const PointF          o = textOrigin();
        const text::HitResult h = _layout->hitTest({p.x - o.x, p.y - o.y});
        if (h.inside && h.linkId)
            return uint8_t(plat::Cursor::Hand);
    }
    return View::cursorAt(p);
}

// ── Clickable ───────────────────────────────────────────────────────────────

// UserFlag0 = checked, UserFlag1 = the held pointer is inside.
Clickable::Clickable() {
    setHoverRepaint(true);
    setRole(Role::Button);
    // Focusable by Tab but not by click, so pressing a toolbar button keeps
    // the caret in the composer.
    setFocusable(true, false);
    setCursor(plat::Cursor::Hand);
}

void Clickable::setLook(const Look &l) {
    _look = l;
    update();
}

void Clickable::setChecked(bool on) {
    if (checked() == on)
        return;
    setFlag(UserFlag0, on);
    stateChanged();
    update();
}

C Clickable::stateColor() const {
    if (!enabled())
        return _look.bg;
    if (flag(Pressed) && flag(UserFlag1))
        return _look.pressed;
    if (checked() && _look.checked != C::None)
        return _look.checked;
    if (hovered() && _look.hover != C::None)
        return _look.hover;
    return _look.bg;
}

void Clickable::paint(gfx::Painter &p) {
    View::paint(p);
    const C c = stateColor();
    if (c == C::None)
        return;
    if (_look.radius > 0)
        p.fillRoundRect(bounds(), _look.radius, color(c));
    else
        p.fillRect(bounds(), color(c));
}

void Clickable::activate() {
    if (onClick) {
        auto cb = onClick; // the handler may destroy this view
        cb();
    }
}

bool Clickable::onEvent(Event &e) {
    switch (e.type) {
    case EventType::PointerDown:
        if (e.button != plat::Button::Left)
            return false;
        setFlag(Pressed, true);
        setFlag(UserFlag1, true);
        update();
        if (e.clicks == 2 && onDoubleClick) {
            auto cb = onDoubleClick;
            cb();
        }
        return true;
    case EventType::PointerMove:
        if (flag(Pressed)) {
            const bool in = bounds().contains(e.pos);
            if (in != flag(UserFlag1)) {
                setFlag(UserFlag1, in);
                update();
            }
            return true;
        }
        return false;
    case EventType::PointerCancel:
        setFlag(Pressed, false);
        setFlag(UserFlag1, false);
        update();
        return true;
    case EventType::PointerUp:
        if (flag(Pressed)) {
            const bool in = bounds().contains(e.pos);
            setFlag(Pressed, false);
            setFlag(UserFlag1, false);
            update();
            if (in)
                activate();
            return true;
        }
        return false;
    case EventType::KeyDown:
        if ((e.key == plat::Key::Enter || e.key == plat::Key::KpEnter ||
             e.key == plat::Key::Space) &&
            !(e.mods & (plat::ModCtrl | plat::ModAlt | plat::ModSuper))) {
            activate();
            return true;
        }
        return false;
    default:
        return false;
    }
}

// ── Button ──────────────────────────────────────────────────────────────────

Button::Button(std::string label, Kind k) : _label(std::move(label)), _kind(k) {
    applyKind();
}

Button::Button(gfx::Icon icon, std::string tooltip, Kind k) : _icon(uint16_t(icon)), _kind(k) {
    _tooltip = std::move(tooltip);
    applyKind();
}

Button::~Button() = default;

void Button::applyKind() {
    Look   l;
    Style &s = style();
    l.radius = metric(M::RadiusM);
    s.noShrink();
    switch (_kind) {
    case Kind::Primary:
        l.bg      = C::Accent;
        l.hover   = C::AccentHover;
        l.pressed = C::AccentHover;
        _text     = C::AccentText;
        s.padding(12, 0);
        s.h = metric(M::ControlH);
        break;
    case Kind::Secondary:
        setBorder(C::BorderStrong);
        setBackground(C::None, l.radius);
        _text = C::Text;
        s.padding(12, 0);
        s.h = metric(M::ControlH);
        break;
    case Kind::Ghost:
        _text = C::Text;
        s.padding(8, 0);
        s.h = metric(M::ControlH);
        break;
    case Kind::Tab:
        l.hover  = C::None;
        l.radius = 0;
        _text    = C::TextMuted;
        s.padding(10, 0);
        s.h = 36;
        setRole(Role::Tab);
        break;
    case Kind::Icon:
        _text     = C::TextMuted;
        _iconSize = 18;
        s.w = s.h = metric(M::ControlH);
        break;
    }
    _look = l;
}

void Button::setLabel(std::string s) {
    if (s == _label)
        return;
    _label = std::move(s);
    _layout.reset();
    invalidateLayout();
    update();
}

void Button::setIcon(gfx::Icon icon) {
    _icon = uint16_t(icon);
    update();
}

void Button::setIconSize(float px) {
    _iconSize = px;
    invalidateLayout();
    update();
}

void Button::setTextColor(C c) {
    _text = c;
    _layout.reset();
    update();
}

void Button::styleChanged() {
    _layout.reset();
    update();
}

void Button::stateChanged() {
    _layout.reset();
}

const text::Layout *Button::labelLayout() {
    if (_label.empty())
        return nullptr;
    if (!_layout) {
        Font f = Font::Body;
        C    c = _text;
        if (_kind == Kind::Tab) {
            f = checked() ? Font::SmallBold : Font::Small;
            c = checked() ? C::Text : C::TextMuted;
        } else if (_kind == Kind::Primary) {
            f = Font::BodyBold;
        }
        _layout = buildPlain(_label, font(f, c), windowScale(this));
    }
    return _layout.get();
}

SizeF Button::measureContent(float, float) {
    const text::Layout *l  = labelLayout();
    const float         iw = _icon != kNoIcon ? _iconSize : 0;
    const float         lw = l ? std::ceil(l->width()) : 0;
    const float         lh = l ? std::ceil(l->height()) : 0;
    return {iw + lw + (iw > 0 && lw > 0 ? 6 : 0), std::max(iw, lh)};
}

void Button::paint(gfx::Painter &p) {
    Clickable::paint(p);
    const text::Layout *l     = labelLayout();
    const float         iw    = _icon != kNoIcon ? _iconSize : 0;
    const float         lw    = l ? l->width() : 0;
    const float         total = iw + lw + (iw > 0 && lw > 0 ? 6 : 0);
    float               x     = snapPx(std::floor((width() - total) / 2));
    if (iw > 0) {
        const C tint = (_kind == Kind::Icon && (hovered() || checked())) ? C::Text : _text;
        gfx::drawIcon(
            p, gfx::Icon(_icon), {x, snapPx(std::floor((height() - iw) / 2)), iw, iw}, color(tint)
        );
        x = snapPx(x + iw + 6);
    }
    if (l)
        l->paint(p, {x, snapPx(std::floor((height() - l->height()) / 2))});
}

void Button::paintOver(gfx::Painter &p) {
    if (_kind == Kind::Tab && checked())
        p.fillRect({0, height() - 2, width(), 2}, color(C::Text));
    if (focused() && window() && window()->focusVisible())
        p.strokeRoundRect(bounds(), _look.radius, 2, color(C::FocusRing));
}

// ── Badge ───────────────────────────────────────────────────────────────────

Badge::Badge(int count, C bg, C fg) : _count(count), _bg(bg), _fg(fg) {
    setRole(Role::Badge);
    style().noShrink();
    setFlag(Visible, count > 0);
}

Badge::~Badge() = default;

void Badge::setCount(int n) {
    if (n == _count)
        return;
    _count = n;
    _layout.reset();
    setVisible(n > 0 || _dot);
    invalidateLayout();
    update();
}

void Badge::setDot(bool on) {
    _dot = on;
    setVisible(_count > 0 || _dot);
    invalidateLayout();
    update();
}

void Badge::setColors(C bg, C fg) {
    _bg = bg;
    _fg = fg;
    _layout.reset();
    update();
}

void Badge::styleChanged() {
    _layout.reset();
    update();
}

SizeF Badge::measureContent(float, float) {
    if (_dot && _count <= 0)
        return {8, 8};
    if (!_layout)
        _layout = buildPlain(
            _count > 99 ? std::string("99+") : std::to_string(_count),
            font(Font::SmallBold, _fg),
            windowScale(this)
        );
    return {std::max(18.f, std::ceil(_layout->width()) + 10), 18};
}

void Badge::paint(gfx::Painter &p) {
    if (_dot && _count <= 0) {
        p.fillCircle({width() / 2, height() / 2}, std::min(width(), height()) / 2, color(_bg));
        return;
    }
    p.fillRoundRect(bounds(), height() / 2, color(_bg));
    if (!_layout)
        measureContent(0, 0);
    _layout->paint(
        p,
        snapPx(
            {std::floor((width() - _layout->width()) / 2),
             std::floor((height() - _layout->height()) / 2)}
        )
    );
}

// ── IconView ────────────────────────────────────────────────────────────────

IconView::IconView(gfx::Icon icon, float size, C tint) : _icon(uint16_t(icon)), _tint(tint) {
    setRole(Role::Image);
    style().size(size, size).noShrink();
}

void IconView::setIcon(gfx::Icon icon) {
    _icon = uint16_t(icon);
    update();
}

void IconView::setTint(C c) {
    _tint = c;
    update();
}

void IconView::paint(gfx::Painter &p) {
    View::paint(p);
    gfx::drawIcon(p, gfx::Icon(_icon), bounds(), color(_tint));
}

// ── Separator ───────────────────────────────────────────────────────────────

Separator::Separator(bool vertical, C c) : _c(c) {
    setRole(Role::Separator);
    Style &s = style();
    s.noShrink();
    if (vertical)
        s.w = 1;
    else
        s.h = 1;
}

void Separator::paint(gfx::Painter &p) {
    p.fillRect(bounds(), color(_c));
}

// ── Image ───────────────────────────────────────────────────────────────────

Image::Image() {
    setRole(Role::Image);
}

Image::~Image() {
    app()->cancelTimer(_timer);
}

const gfx::Bitmap *Image::bitmap() const {
    if (_frames && !_frames->empty())
        return &(*_frames)[size_t(_frame) % _frames->size()].frame;
    return _bitmap.get();
}

void Image::setBitmap(std::shared_ptr<const gfx::Bitmap> b) {
    app()->cancelTimer(_timer);
    _timer = 0;
    _frames.reset();
    _bitmap = std::move(b);
    if (currentStyle().w < 0 || currentStyle().h < 0)
        invalidateLayout();
    update();
}

void Image::setFrames(std::shared_ptr<const Frames> f) {
    _bitmap.reset();
    _frames = std::move(f);
    _frame  = 0;
    scheduleFrame();
    if (currentStyle().w < 0)
        invalidateLayout();
    update();
}

void Image::setFit(Fit f) {
    _fit = f;
    update();
}
void Image::setRadius(float r) {
    _radius = r;
    update();
}
void Image::setCircle(bool on) {
    _circle = on;
    update();
}
void Image::setPlaceholder(C c) {
    _placeholder = c;
    update();
}

void Image::windowChanged() {
    scheduleFrame();
}

void Image::scheduleFrame() {
    app()->cancelTimer(_timer);
    _timer = 0;
    if (!_frames || _frames->size() < 2 || !window() || app()->reducedMotion())
        return;
    const int delay = std::max(20, (*_frames)[size_t(_frame)].delayMs);
    _timer          = app()->addTimer(delay, false, [this] {
        _timer = 0;
        _frame = int((size_t(_frame) + 1) % _frames->size());
        update();
        scheduleFrame();
    });
}

SizeF Image::measureContent(float aw, float ah) {
    const gfx::Bitmap *b = bitmap();
    if (!b || b->empty())
        return {0, 0};
    float w = float(b->width()), h = float(b->height());
    // Shrink (never enlarge) to the available width, keeping the aspect.
    if (w > aw && aw > 0) {
        h = h * aw / w;
        w = aw;
    }
    return {std::floor(w), std::floor(h)};
}

void Image::paint(gfx::Painter &p) {
    View::paint(p);
    const gfx::Bitmap *b      = bitmap();
    const RectF        r      = bounds();
    const float        radius = _circle ? std::min(r.w, r.h) / 2 : _radius;
    if (!b || b->empty()) {
        if (_placeholder != C::None) {
            if (radius > 0)
                p.fillRoundRect(r, radius, color(_placeholder));
            else
                p.fillRect(r, color(_placeholder));
        }
        return;
    }
    const float bw = float(b->width()), bh = float(b->height());
    RectF       dst = r;
    if (_fit != Fit::Fill && bw > 0 && bh > 0) {
        const float sx = r.w / bw, sy = r.h / bh;
        const float s = _fit == Fit::Contain ? std::min(sx, sy) : std::max(sx, sy);
        dst           = {(r.w - bw * s) / 2, (r.h - bh * s) / 2, bw * s, bh * s};
    }
    p.save();
    if (radius > 0)
        p.clipRoundRect(r, radius);
    else if (_fit == Fit::Cover)
        p.clipRect(r);
    p.drawBitmap(b->view(), dst, gfx::Sampling::Smooth);
    p.restore();
}

} // namespace ui
