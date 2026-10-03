#include "ui/textedit.h"
#include "ui/widgets.h"

#include "base/i18n.h"

#include <algorithm>
#include <cmath>

namespace ui {

namespace {

constexpr size_t   kUndoLimit  = 200;
constexpr double   kCoalesceMs = 1500;
constexpr float    kWheelStep  = 50;
constexpr uint16_t kLinkMask   = 0xff00;

inline bool isCont(char c) {
    return (uint8_t(c) & 0xc0) == 0x80;
}
inline bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n';
}

// Menu ids for the built-in context menu.
enum : int { kCut = 1, kCopy, kPaste, kSelectAll };

} // namespace

TextEdit::TextEdit() : _alive(std::make_shared<char>(0)) {
    setRole(Role::TextInput);
    setFocusable(true);
    setCursor(plat::Cursor::IBeam);
    style().padding(8, 6);
}

TextEdit::~TextEdit() {
    if (_blinkTimer)
        app()->cancelTimer(_blinkTimer);
}

// ── Offsets ─────────────────────────────────────────────────────────────────

uint32_t TextEdit::prevChar(uint32_t o) const {
    if (o == 0)
        return 0;
    --o;
    while (o > 0 && isCont(_text[o]))
        --o;
    return o;
}

uint32_t TextEdit::nextChar(uint32_t o) const {
    const uint32_t n = uint32_t(_text.size());
    if (o >= n)
        return n;
    ++o;
    while (o < n && isCont(_text[o]))
        ++o;
    return o;
}

// Masked text shows one 3-byte bullet per byte (keys are ASCII), so offsets
// scale by 3 and there is never a preedit.
constexpr uint32_t kBulletBytes = 3;

uint32_t TextEdit::toDisplay(uint32_t m) const {
    if (_masked)
        return m * kBulletBytes;
    return !_preedit.empty() && m >= _preeditPos ? m + uint32_t(_preedit.size()) : m;
}

uint32_t TextEdit::toModel(uint32_t d) const {
    if (_masked)
        return std::min(d / kBulletBytes, uint32_t(_text.size()));
    if (_preedit.empty() || d < _preeditPos)
        return std::min(d, uint32_t(_text.size()));
    if (d < _preeditPos + _preedit.size())
        return _preeditPos;
    return std::min(d - uint32_t(_preedit.size()), uint32_t(_text.size()));
}

// ── Layout ──────────────────────────────────────────────────────────────────

float TextEdit::contentWidth() const {
    const Style &s = currentStyle();
    return std::max(1.f, width() - s.pad.l - s.pad.r);
}

text::Style TextEdit::baseStyle(C c) const {
    return font(_font, c);
}

float TextEdit::lineHeight() const {
    return std::ceil(baseStyle(C::Text).size * 1.4f);
}

const text::Layout *TextEdit::layoutFor(float w) {
    const float scale = windowScale();
    if (_layout && w == _layoutW)
        return _layout.get();
    const text::Style    base = baseStyle(C::Text);
    text::AttributedText t;
    auto                 styleFor = [&](uint16_t f) {
        text::Style st = base;
        if (f & Bold)
            st.weight = text::Weight::Bold;
        if (f & Italic)
            st.italic = true;
        if (f & Strike)
            st.strike = true;
        if (f & Code) {
            st.mono       = true;
            st.size       = std::max(10.f, base.size - 2);
            st.color      = color(C::CodeText);
            st.background = color(C::CodeBg);
        }
        if (f & kLinkMask) {
            st.color = color(C::Link);
            if (_linkBg != C::None) // pills: mentions in the composer
                st.background = color(_linkBg);
        }
        return st;
    };
    // Runs of equal format; the preedit goes in underlined at its position.
    auto appendRange = [&](uint32_t a, uint32_t b) {
        uint32_t i = a;
        while (i < b) {
            uint32_t j = i + 1;
            while (j < b && _fmt[j] == _fmt[i])
                ++j;
            t.append(std::string_view(_text).substr(i, j - i), styleFor(_fmt[i]));
            i = j;
        }
    };
    if (_masked) {
        std::string dots;
        for (size_t i = 0; i < _text.size(); ++i)
            dots += "\xE2\x80\xA2";
        t.append(dots, base);
    } else if (_preedit.empty()) {
        appendRange(0, uint32_t(_text.size()));
    } else {
        appendRange(0, _preeditPos);
        text::Style pe = _preeditPos > 0 ? styleFor(_fmt[_preeditPos - 1]) : base;
        pe.underline   = true;
        t.append(_preedit, pe);
        appendRange(_preeditPos, uint32_t(_text.size()));
    }
    text::LayoutOptions o;
    o.maxWidth = w;
    _layout    = text::Layout::build(std::move(t), o, scale);
    _layoutW   = w;
    return _layout.get();
}

const text::Layout *TextEdit::currentLayout() {
    return layoutFor(contentWidth());
}

float TextEdit::clampedHeight(const text::Layout *l) const {
    const float lh = lineHeight();
    float       h  = std::max(l->height(), lh);
    h              = std::max(h, lh * _minLines);
    if (_maxLines > 0)
        h = std::min(h, lh * _maxLines);
    return std::ceil(h);
}

SizeF TextEdit::measureContent(float aw, float ah) {
    const float         w = aw < kInf / 2 ? aw : 400;
    const text::Layout *l = layoutFor(std::max(1.f, w));
    return {w, clampedHeight(l)};
}

void TextEdit::layout() {
    ensureCaretVisible(); // the layout itself is cached per content width
    updateIme();
}

void TextEdit::styleChanged() {
    _layout.reset();
    _placeholderLayout.reset();
    _layoutW = -1;
    update();
}

void TextEdit::windowChanged() {
    if (!window())
        stopBlink();
}

bool TextEdit::caretOnEdgeLine(bool top) const {
    // Edge: Up / Down lands on the same line (the layout may still move the
    // caret along it, to the line's end).
    auto               *self = const_cast<TextEdit *>(this);
    const text::Layout *l    = self->currentLayout();
    const uint32_t      d    = toDisplay(_caret);
    const uint32_t      to   = l->moveCaret(d, 0, top ? -1 : 1);
    return to == d || l->caretRect(to).y == l->caretRect(d).y;
}

RectF TextEdit::caretRect() const {
    auto               *self = const_cast<TextEdit *>(this);
    const text::Layout *l    = self->currentLayout();
    uint32_t            d    = toDisplay(_caret);
    if (!_preedit.empty())
        d = _preeditPos + uint32_t(
                              std::clamp(
                                  _preeditCursor < 0 ? int(_preedit.size()) : _preeditCursor,
                                  0,
                                  int(_preedit.size())
                              )
                          );
    RectF        r = l->caretRect(d);
    const Style &s = currentStyle();
    r.x += s.pad.l;
    r.y += s.pad.t - _scrollY;
    r.w = std::max(r.w, 1.f);
    if (r.h <= 0)
        r.h = lineHeight();
    return r;
}

void TextEdit::ensureCaretVisible() {
    if (width() <= 0)
        return;
    const text::Layout *l    = currentLayout();
    const Style        &s    = currentStyle();
    const float         view = std::max(1.f, height() - s.pad.t - s.pad.b);
    RectF               r    = l->caretRect(toDisplay(_caret));
    const float         old  = _scrollY;
    if (r.y < _scrollY)
        _scrollY = r.y;
    if (r.y + r.h > _scrollY + view)
        _scrollY = r.y + r.h - view;
    _scrollY = std::clamp(_scrollY, 0.f, std::max(0.f, l->height() - view));
    if (_scrollY != old)
        update();
}

void TextEdit::updateIme() {
    if (!window() || !focused())
        return;
    const RectF  r = caretRect();
    const PointF o = mapToWindow({r.x, r.y});
    window()->setTextInput(true, {o.x, o.y, r.w, r.h});
}

// ── Caret blink ─────────────────────────────────────────────────────────────

void TextEdit::startBlink() {
    stopBlink();
    _caretOn     = true;
    const int ms = app()->settings().caretBlinkMs;
    if (ms > 0 && focused())
        _blinkTimer = app()->addTimer(ms, true, [this] {
            _caretOn = !_caretOn;
            RectF r  = caretRect();
            update({r.x - 1, r.y - 1, r.w + 2, r.h + 2});
        });
}

void TextEdit::stopBlink() {
    if (_blinkTimer)
        app()->cancelTimer(_blinkTimer);
    _blinkTimer = 0;
}

// ── Editing core ────────────────────────────────────────────────────────────

uint16_t TextEdit::typingFormat() const {
    if (_typingSet)
        return _typing;
    const uint32_t a = std::min(_caret, _anchor);
    // Continue the format of the text before the caret, but never a link.
    return a > 0 && a <= _fmt.size() ? uint16_t(_fmt[a - 1] & ~kLinkMask) : 0;
}

void TextEdit::apply(const Edit &e, bool reverse) {
    const std::string           &out = reverse ? e.inserted : e.removed;
    const std::string           &in  = reverse ? e.removed : e.inserted;
    const std::vector<uint16_t> &inF = reverse ? e.removedFmt : e.insertedFmt;
    shiftSquiggles(e.pos, out.size(), in.size());
    _text.replace(e.pos, out.size(), in);
    _fmt.erase(_fmt.begin() + e.pos, _fmt.begin() + e.pos + out.size());
    _fmt.insert(_fmt.begin() + e.pos, inF.begin(), inF.end());
}

void TextEdit::replace(
    uint32_t         from,
    uint32_t         to,
    std::string_view ins,
    const uint16_t  *fmt,
    uint16_t         fill,
    EditKind         kind
) {
    from = std::min(from, uint32_t(_text.size()));
    to   = std::clamp(to, from, uint32_t(_text.size()));
    Edit e;
    e.pos     = from;
    e.removed = _text.substr(from, to - from);
    e.removedFmt.assign(_fmt.begin() + from, _fmt.begin() + to);
    e.inserted = std::string(ins);
    if (fmt)
        e.insertedFmt.assign(fmt, fmt + ins.size());
    else
        e.insertedFmt.assign(ins.size(), fill);
    e.anchorBefore = _anchor;
    e.caretBefore  = _caret;
    e.kind         = kind;
    e.time         = app()->nowMs();
    apply(e, false);
    if (kind == EditKind::Format) {
        e.anchorAfter = _anchor; // formatting keeps the selection
        e.caretAfter  = _caret;
    } else {
        _caret = _anchor = from + uint32_t(ins.size());
        e.anchorAfter = e.caretAfter = _caret;
        _caretTyped                  = true;
    }
    // Coalesce runs of typing / deleting into one undo step.
    Edit *last   = _undo.empty() ? nullptr : &_undo.back();
    bool  merged = false;
    if (last && last->kind == kind && e.time - last->time < kCoalesceMs) {
        if (kind == EditKind::Typing && e.removed.empty() &&
            last->pos + last->inserted.size() == from && !(ins.size() == 1 && ins[0] == '\n') &&
            !(!last->inserted.empty() && last->inserted.back() == '\n')) {
            last->inserted += e.inserted;
            last->insertedFmt.insert(
                last->insertedFmt.end(), e.insertedFmt.begin(), e.insertedFmt.end()
            );
            merged = true;
        } else if (kind == EditKind::Backspace && e.inserted.empty() && last->pos == to) {
            last->removed.insert(0, e.removed);
            last->removedFmt.insert(
                last->removedFmt.begin(), e.removedFmt.begin(), e.removedFmt.end()
            );
            last->pos = from;
            merged    = true;
        } else if (kind == EditKind::DeleteForward && e.inserted.empty() && last->pos == from) {
            last->removed += e.removed;
            last->removedFmt.insert(
                last->removedFmt.end(), e.removedFmt.begin(), e.removedFmt.end()
            );
            merged = true;
        }
        if (merged) {
            last->anchorAfter = e.anchorAfter;
            last->caretAfter  = e.caretAfter;
            last->time        = e.time;
        }
    }
    if (!merged) {
        _undo.push_back(std::move(e));
        if (_undo.size() > kUndoLimit)
            _undo.erase(_undo.begin());
    }
    _redo.clear();
    if (kind != EditKind::Format)
        _typingSet = false;
    contentChanged();
}

void TextEdit::contentChanged() {
    _layout.reset();
    _layoutW = -1;
    if (width() > 0) {
        const text::Layout *l    = currentLayout();
        const Style        &s    = currentStyle();
        const float         want = clampedHeight(l) + s.pad.t + s.pad.b;
        if (std::abs(want - height()) > 0.5f)
            invalidateLayout(); // grow/shrink the composer
    } else {
        invalidateLayout();
    }
    update();
    ensureCaretVisible();
    startBlink();
    updateIme();
    if (onChange) {
        auto cb = onChange;
        cb();
    }
    selectionChanged();
}

void TextEdit::selectionChanged() {
    if (onSelectionChange) {
        auto cb = onSelectionChange;
        cb();
    }
}

void TextEdit::deleteSelection(EditKind kind) {
    if (!hasSelection())
        return;
    replace(std::min(_caret, _anchor), std::max(_caret, _anchor), {}, nullptr, 0, kind);
}

void TextEdit::moveTo(uint32_t c, bool extend) {
    c = std::min(c, uint32_t(_text.size()));
    if (_caretTyped) {
        _caretTyped = false; // put there, not typed: its squiggle shows
        update();
    }
    if (c == _caret && (extend || c == _anchor))
        return;
    _caret = c;
    if (!extend)
        _anchor = c;
    _typingSet = false;
    update();
    ensureCaretVisible();
    startBlink();
    updateIme();
    selectionChanged();
}

// ── Public editing API ──────────────────────────────────────────────────────

void TextEdit::setText(std::string_view plain) {
    _squiggles.clear();
    _text.assign(plain);
    _fmt.assign(_text.size(), 0);
    _links.clear();
    _undo.clear();
    _redo.clear();
    _preedit.clear();
    _caret = _anchor = uint32_t(_text.size());
    _typingSet       = false;
    _caretTyped      = false;
    _scrollY         = 0;
    contentChanged();
}

void TextEdit::insertText(std::string_view s) {
    const uint16_t f = typingFormat();
    replace(std::min(_caret, _anchor), std::max(_caret, _anchor), s, nullptr, f, EditKind::Other);
}

void TextEdit::insertHtml(std::string_view html) {
    std::string              t;
    std::vector<uint16_t>    f;
    std::vector<std::string> links;
    rich::fromHtml(html, &t, &f, &links);
    // Remap the fragment's link indices into ours.
    const size_t base = _links.size();
    for (auto &l : links)
        _links.push_back(std::move(l));
    for (uint16_t &x : f)
        if (x & kLinkMask) {
            const size_t idx = ((x >> 8) - 1) + base + 1;
            x                = uint16_t((x & 0xff) | (idx <= 255 ? idx << 8 : 0));
        }
    replace(std::min(_caret, _anchor), std::max(_caret, _anchor), t, f.data(), 0, EditKind::Other);
}

std::vector<TextEdit::Run> TextEdit::runs() const {
    std::vector<Run> out;
    uint32_t         i = 0, n = uint32_t(_text.size());
    while (i < n) {
        uint32_t j = i + 1;
        while (j < n && _fmt[j] == _fmt[i])
            ++j;
        Run r;
        r.start           = i;
        r.end             = j;
        r.format          = uint8_t(_fmt[i] & 0xff);
        const size_t link = _fmt[i] >> 8;
        if (link && link <= _links.size())
            r.link = _links[link - 1];
        out.push_back(r);
        i = j;
    }
    return out;
}

std::string TextEdit::html() const {
    uint32_t a = std::min(_caret, _anchor), b = std::max(_caret, _anchor);
    if (a == b) {
        a = 0;
        b = uint32_t(_text.size());
    }
    return rich::toHtml(std::string_view(_text).substr(a, b - a), _fmt.data() + a, _links);
}

void TextEdit::setSelection(uint32_t a, uint32_t c) {
    a       = std::min(a, uint32_t(_text.size()));
    _anchor = a;
    _caret  = uint32_t(-1); // force moveTo to act
    moveTo(c, true);
}

void TextEdit::selectAll() {
    setSelection(0, uint32_t(_text.size()));
}

std::string TextEdit::selectedText() const {
    const uint32_t a = std::min(_caret, _anchor), b = std::max(_caret, _anchor);
    return _text.substr(a, b - a);
}

void TextEdit::toggleFormat(Format f) {
    if (!hasSelection()) {
        _typing    = uint16_t(typingFormat() ^ f);
        _typingSet = true;
        selectionChanged(); // toolbar state
        return;
    }
    const uint32_t a = std::min(_caret, _anchor), b = std::max(_caret, _anchor);
    bool           all = true;
    for (uint32_t i = a; i < b; ++i)
        all &= (_fmt[i] & f) != 0;
    std::vector<uint16_t> nf(_fmt.begin() + a, _fmt.begin() + b);
    for (uint16_t &x : nf)
        x = all ? uint16_t(x & ~f) : uint16_t(x | f);
    const std::string same = _text.substr(a, b - a);
    replace(a, b, same, nf.data(), 0, EditKind::Format);
}

bool TextEdit::formatActive(Format f) const {
    if (!hasSelection())
        return (typingFormat() & f) != 0;
    const uint32_t a = std::min(_caret, _anchor), b = std::max(_caret, _anchor);
    for (uint32_t i = a; i < b; ++i)
        if (!(_fmt[i] & f))
            return false;
    return true;
}

void TextEdit::setLink(std::string url) {
    if (!hasSelection())
        return;
    uint16_t idx = 0;
    if (!url.empty() && _links.size() < 255) {
        _links.push_back(std::move(url));
        idx = uint16_t(_links.size());
    }
    const uint32_t        a = std::min(_caret, _anchor), b = std::max(_caret, _anchor);
    std::vector<uint16_t> nf(_fmt.begin() + a, _fmt.begin() + b);
    for (uint16_t &x : nf)
        x = uint16_t((x & 0xff) | (idx << 8));
    const std::string same = _text.substr(a, b - a);
    replace(a, b, same, nf.data(), 0, EditKind::Format);
}

void TextEdit::setPlaceholder(std::string s) {
    _placeholder = std::move(s);
    _placeholderLayout.reset();
    update();
}

void TextEdit::setMasked(bool on) {
    _masked = on;
    _preedit.clear();
    _layout.reset();
    invalidateLayout();
    update();
}

void TextEdit::setMinLines(int n) {
    _minLines = uint8_t(std::clamp(n, 1, 255));
    invalidateLayout();
}

void TextEdit::setMaxLines(int n) {
    _maxLines = uint8_t(std::clamp(n, 0, 255));
    invalidateLayout();
}

void TextEdit::setFont(Font f) {
    _font = f;
    styleChanged();
    invalidateLayout();
}

bool TextEdit::undo() {
    if (_undo.empty())
        return false;
    Edit e = std::move(_undo.back());
    _undo.pop_back();
    apply(e, true);
    _anchor     = e.anchorBefore;
    _caret      = e.caretBefore;
    _caretTyped = true;
    _redo.push_back(std::move(e));
    contentChanged();
    return true;
}

bool TextEdit::redo() {
    if (_redo.empty())
        return false;
    Edit e = std::move(_redo.back());
    _redo.pop_back();
    apply(e, false);
    _anchor     = e.anchorAfter;
    _caret      = e.caretAfter;
    _caretTyped = true;
    e.time      = 0; // never coalesce into a redone step
    _undo.push_back(std::move(e));
    contentChanged();
    return true;
}

// ── Clipboard ───────────────────────────────────────────────────────────────

void TextEdit::copy() {
    if (!hasSelection() || _masked)
        return;
    app()->platform().setClipboard(
        {{"text/plain;charset=utf-8", selectedText()}, {"text/html", html()}}
    );
}

void TextEdit::cut() {
    if (!hasSelection() || _masked)
        return;
    copy();
    deleteSelection();
}

void TextEdit::setLinkBackground(C c) {
    _linkBg = c;
    _layout.reset();
    update();
}

void TextEdit::paste(bool plainText, plat::Selection sel) {
    if (!onPasteMedia)
        return pasteText(plainText, sel);
    std::weak_ptr<char> alive = _alive;
    app()->platform().requestClipboardMimes(
        [this, alive, plainText, sel](std::vector<std::string> m) {
            if (alive.expired())
                return;
            if (!(onPasteMedia && onPasteMedia(m, sel)))
                pasteText(plainText, sel);
        },
        sel
    );
}

void TextEdit::pasteText(bool plainText, plat::Selection sel) {
    plainText                      = plainText || _plainPaste;
    std::weak_ptr<char> alive      = _alive;
    auto                pastePlain = [this, alive, sel] {
        app()->platform().requestClipboard(
            "text/plain;charset=utf-8",
            [this, alive](std::optional<std::string> t) {
                if (!alive.expired() && t && !t->empty())
                    insertText(*t);
            },
            sel
        );
    };
    if (plainText) {
        pastePlain();
        return;
    }
    app()->platform().requestClipboard(
        "text/html",
        [this, alive, pastePlain](std::optional<std::string> h) {
            if (alive.expired())
                return;
            if (h && !h->empty())
                insertHtml(*h);
            else
                pastePlain();
        },
        sel
    );
}

void TextEdit::setPrimarySelection() {
#if defined(__linux__) || defined(__FreeBSD__)
    if (hasSelection())
        app()->platform().setClipboardText(selectedText(), plat::Selection::Primary);
#endif
}

// ── Navigation helpers ──────────────────────────────────────────────────────

uint32_t TextEdit::hitOffset(PointF local) {
    const Style        &s = currentStyle();
    const text::Layout *l = currentLayout();
    return toModel(l->hitTest({local.x - s.pad.l, local.y - s.pad.t + _scrollY}).offset);
}

uint32_t TextEdit::wordLeft(uint32_t o) {
    while (o > 0 && isSpace(_text[o - 1]))
        --o;
    if (o == 0)
        return 0;
    const uint32_t p = prevChar(o);
    const uint32_t w = toModel(currentLayout()->wordStart(toDisplay(p)));
    return std::min(w, p);
}

uint32_t TextEdit::wordRight(uint32_t o) {
    const uint32_t n = uint32_t(_text.size());
    while (o < n && isSpace(_text[o]))
        ++o;
    if (o >= n)
        return n;
    const uint32_t w = toModel(currentLayout()->wordEnd(toDisplay(o)));
    return w > o ? w : nextChar(o);
}

uint32_t TextEdit::lineEdge(uint32_t o, bool end) {
    const text::Layout *l = currentLayout();
    const RectF         r = l->caretRect(toDisplay(o));
    return toModel(l->hitTest({end ? 1e6f : -1e6f, r.y + r.h / 2}).offset);
}

std::vector<MenuItem> TextEdit::standardMenuItems() const {
    std::vector<MenuItem> items;
    const bool            sel = hasSelection();
    const char           *mod = plat::primaryMod() == plat::ModSuper ? "Cmd+" : "Ctrl+";
    items.push_back({kCut, i18n::tr("Cut"), std::string(mod) + "X", Button::kNoIcon, sel});
    items.push_back({kCopy, i18n::tr("Copy"), std::string(mod) + "C", Button::kNoIcon, sel});
    items.push_back({kPaste, i18n::tr("Paste"), std::string(mod) + "V"});
    items.push_back({0, {}, {}, Button::kNoIcon, true, false, true});
    items.push_back(
        {kSelectAll,
         i18n::tr("Select all"),
         std::string(mod) + "A",
         Button::kNoIcon,
         !_text.empty()}
    );
    return items;
}

void TextEdit::runStandardItem(int id) {
    focus();
    switch (id) {
    case kCut:
        cut();
        break;
    case kCopy:
        copy();
        break;
    case kPaste:
        paste();
        break;
    case kSelectAll:
        selectAll();
        break;
    }
}

void TextEdit::showContextMenu(PointF local) {
    if (!window())
        return;
    const PointF w = mapToWindow(local);
    if (onContextMenu && !_masked && onContextMenu(hitOffset(local), w))
        return;
    std::weak_ptr<char> alive = _alive;
    Menu::show(
        *window(),
        {w.x, w.y, 0, 0},
        standardMenuItems(),
        [this, alive](int id) {
            if (!alive.expired())
                runStandardItem(id);
        },
        Popup::Place::Over
    );
}

// ── Squiggles ───────────────────────────────────────────────────────────────

void TextEdit::setSquiggles(std::vector<Range> ranges) {
    if (ranges == _squiggles)
        return;
    _squiggles = std::move(ranges);
    update();
}

const TextEdit::Range *TextEdit::squiggleAt(uint32_t offset) const {
    for (const Range &r : _squiggles)
        if (offset >= r.from && offset <= r.to)
            return &r;
    return nullptr;
}

bool TextEdit::squiggleShown(const Range &r) const {
    return !(_caretTyped && focused() && !hasSelection() && _caret >= r.from && _caret <= r.to);
}

void TextEdit::shiftSquiggles(uint32_t pos, size_t removed, size_t inserted) {
    if (_squiggles.empty())
        return;
    const uint32_t     end   = pos + uint32_t(removed);
    const int64_t      delta = int64_t(inserted) - int64_t(removed);
    std::vector<Range> kept;
    for (const Range &r : _squiggles) {
        if (r.to < pos)
            kept.push_back(r);
        else if (r.from > end)
            kept.push_back({uint32_t(r.from + delta), uint32_t(r.to + delta)});
        // else: the edit touches the word; the owner re-checks it
    }
    _squiggles = std::move(kept);
}

// A 1-px zigzag 2 px below each line's baseline (spell-check underline).
void TextEdit::paintSquiggles(gfx::Painter &p, const text::Layout *l) const {
    const Color c = color(C::Danger);
    for (const Range &r : _squiggles) {
        if (!squiggleShown(r) || r.to > _text.size())
            continue;
        for (const RectF &b : l->selectionRects(toDisplay(r.from), toDisplay(r.to))) {
            float y = b.y + b.h - 2;
            for (int i = 0; i < l->lineCount(); ++i) {
                const float base = l->baseline(i);
                if (base >= b.y && base <= b.y + b.h) {
                    y = base + 2;
                    break;
                }
            }
            gfx::Path       path;
            constexpr float kStep = 2, kAmp = 1;
            path.moveTo(b.x, y);
            int k = 0;
            for (float x = b.x + kStep; x <= b.x + b.w + 0.01f; x += kStep, ++k)
                path.lineTo(x, (k & 1) ? y : y + kAmp);
            p.strokePath(path, 1, c);
        }
    }
}

// ── Painting ────────────────────────────────────────────────────────────────

void TextEdit::paint(gfx::Painter &p) {
    View::paint(p);
    const Style        &s = currentStyle();
    const text::Layout *l = currentLayout();
    p.save();
    p.clipRect({s.pad.l, s.pad.t, contentWidth(), std::max(0.f, height() - s.pad.t - s.pad.b)});
    p.translate(snapPx(s.pad.l), snapPx(s.pad.t - _scrollY));
    if (_text.empty() && _preedit.empty() && !_placeholder.empty()) {
        if (!_placeholderLayout) {
            text::AttributedText t;
            t.append(_placeholder, baseStyle(C::Placeholder));
            text::LayoutOptions o;
            o.maxWidth         = contentWidth();
            o.maxLines         = _maxLines; // a multi-line field may show a list of examples
            o.ellipsis         = true;
            _placeholderLayout = text::Layout::build(t, o, windowScale());
        }
        _placeholderLayout->paint(p, {0, 0});
    }
    if (hasSelection()) {
        const uint32_t a = toDisplay(std::min(_caret, _anchor));
        const uint32_t b = toDisplay(std::max(_caret, _anchor));
        const Color c = focused() ? color(C::Selection) : gfx::withAlpha(color(C::Selection), 0.5f);
        for (const RectF &r : l->selectionRects(a, b))
            p.fillRect(r, c);
    }
    l->paint(p, {0, 0});
    if (!_squiggles.empty())
        paintSquiggles(p, l);
    p.restore();
    const bool active = window() && window()->isActive();
    if (focused() && active && _caretOn && !hasSelection()) {
        RectF r = caretRect();
        p.save();
        p.clipRect({s.pad.l - 1, s.pad.t, contentWidth() + 2, height() - s.pad.t - s.pad.b});
        p.fillRect({std::round(r.x), r.y, 1.5f, r.h}, color(C::Caret));
        p.restore();
    }
}

// ── Events ──────────────────────────────────────────────────────────────────

bool TextEdit::onEvent(Event &e) {
    using K = plat::Key;
    switch (e.type) {
    case EventType::FocusIn:
        startBlink();
        updateIme();
        update();
        if (onFocusChange)
            onFocusChange(true);
        return true;
    case EventType::FocusOut:
        _caretTyped = false;
        if (onFocusChange)
            onFocusChange(false);
        stopBlink();
        if (!_preedit.empty()) {
            _preedit.clear();
            contentChanged();
        }
        if (window())
            window()->setTextInput(false, {});
        update();
        return true;
    case EventType::TextInput: {
        if (!e.raw)
            return false;
        std::string t;
        for (char c : e.raw->text) // drop control characters (except newline/tab)
            if (uint8_t(c) >= 0x20 || c == '\n' || (c == '\t' && flag(WantsTab)))
                if (c != 0x7f)
                    t.push_back(c);
        const bool hadPreedit = !_preedit.empty();
        _preedit.clear();
        if (t.empty()) {
            if (hadPreedit)
                contentChanged();
            return true;
        }
        const uint16_t f    = typingFormat();
        const bool     keep = _typingSet;
        replace(
            std::min(_caret, _anchor), std::max(_caret, _anchor), t, nullptr, f, EditKind::Typing
        );
        if (keep) { // an explicit toggle holds for the rest of the typing run
            _typing    = f;
            _typingSet = true;
        }
        return true;
    }
    case EventType::TextPreedit: {
        if (!e.raw || _masked)
            return false;
        if (e.raw->text.empty()) {
            if (_preedit.empty())
                return true;
            _preedit.clear();
        } else {
            if (_preedit.empty() && hasSelection())
                deleteSelection();
            if (_preedit.empty())
                _preeditPos = std::min(_caret, uint32_t(_text.size()));
            _preedit       = e.raw->text;
            _preeditCursor = e.raw->preeditCursorBegin;
        }
        _layout.reset();
        _layoutW = -1;
        invalidateLayout();
        update();
        updateIme();
        return true;
    }
    case EventType::PointerDown: {
        if (e.button == plat::Button::Middle) {
#if defined(__linux__) || defined(__FreeBSD__)
            // The primary selection, as plain text — or, to an owner that
            // takes media (the composer: the middle click attaches like a
            // paste), its files and pictures.
            moveTo(hitOffset(e.pos), false);
            paste(true, plat::Selection::Primary);
            return true;
#else
            return false;
#endif
        }
        if (e.button != plat::Button::Left)
            return false;
        const uint32_t o = hitOffset(e.pos);
        if (e.clicks == 2) {
            const text::Layout *l = currentLayout();
            _selOriginA           = toModel(l->wordStart(toDisplay(o)));
            _selOriginB           = toModel(l->wordEnd(toDisplay(o)));
            _selMode              = 1;
            setSelection(_selOriginA, _selOriginB);
        } else if (e.clicks >= 3) {
            uint32_t a = o, b = o;
            while (a > 0 && _text[a - 1] != '\n')
                --a;
            while (b < _text.size() && _text[b] != '\n')
                ++b;
            _selOriginA = a;
            _selOriginB = b;
            _selMode    = 2;
            setSelection(a, b);
        } else {
            _selMode = 0;
            moveTo(o, (e.mods & plat::ModShift) != 0);
        }
        _dragging = true;
        return true;
    }
    case EventType::PointerMove:
        if (!_dragging)
            return false;
        {
            const uint32_t o = hitOffset(e.pos);
            if (_selMode == 0) {
                moveTo(o, true);
            } else {
                const text::Layout *l = currentLayout();
                uint32_t            a = _selOriginA, b = _selOriginB;
                if (_selMode == 1) {
                    a = std::min(a, toModel(l->wordStart(toDisplay(o))));
                    b = std::max(b, toModel(l->wordEnd(toDisplay(o))));
                } else {
                    a = std::min(a, o);
                    b = std::max(b, o);
                }
                if (o < _selOriginA)
                    setSelection(b, a);
                else
                    setSelection(a, b);
            }
        }
        return true;
    case EventType::PointerUp:
        if (!_dragging)
            return false;
        _dragging = false;
        setPrimarySelection();
        return true;
    case EventType::Scroll: {
        const Style        &s    = currentStyle();
        const text::Layout *l    = currentLayout();
        const float         view = height() - s.pad.t - s.pad.b;
        const float         max  = std::max(0.f, l->height() - view);
        if (max <= 0)
            return false;
        const float old = _scrollY;
        _scrollY        = std::clamp(_scrollY + (e.precise ? e.dy : e.dy * kWheelStep), 0.f, max);
        if (_scrollY == old)
            return false;
        update();
        updateIme();
        return true;
    }
    case EventType::ContextMenu:
        showContextMenu(e.pos);
        return true;
    case EventType::KeyDown:
        break;
    default:
        return false;
    }

    // ── KeyDown ─────────────────────────────────────────────────────────────
    if (onKey) {
        auto cb = onKey;
        if (cb(e))
            return true;
    }
    if (!_preedit.empty())
        return true; // the IME owns the keyboard while composing
    const uint32_t m = e.mods & (plat::ModShift | plat::ModCtrl | plat::ModAlt | plat::ModSuper);
    const uint32_t primary = plat::primaryMod();
    const bool     shift   = m & plat::ModShift;
    const bool     cmd     = (m & primary) && !(m & ~(primary | plat::ModShift));
#ifdef __APPLE__
    const uint32_t wordMod = plat::ModAlt;
#else
    const uint32_t wordMod = plat::ModCtrl;
#endif
    const bool     word = (m & wordMod) != 0;
    const uint32_t a = std::min(_caret, _anchor), b = std::max(_caret, _anchor);
#ifndef __APPLE__
    // Alt+arrows edit nothing here: leave them to the
    // window's shortcuts (Alt+Left/Right = back/forward).
    if ((m & plat::ModAlt) &&
        (e.key == K::Left || e.key == K::Right || e.key == K::Up || e.key == K::Down))
        return false;
#endif
    switch (e.key) {
    case K::Left:
    case K::Right: {
        const bool left = e.key == K::Left;
        if (hasSelection() && !shift && !word && !(m & plat::ModSuper)) {
            moveTo(left ? a : b, false);
            return true;
        }
        uint32_t c = _caret;
#ifdef __APPLE__
        if (m & plat::ModSuper)
            c = lineEdge(c, !left);
        else
#endif
            if (word)
            c = left ? wordLeft(c) : wordRight(c);
        else
            c = toModel(currentLayout()->moveCaret(toDisplay(c), left ? -1 : 1, 0));
        moveTo(c, shift);
        return true;
    }
    case K::Up:
    case K::Down: {
        const bool up = e.key == K::Up;
        uint32_t   c;
#ifdef __APPLE__
        if (m & plat::ModSuper)
            c = up ? 0 : uint32_t(_text.size());
        else
#endif
        {
            c = toModel(currentLayout()->moveCaret(toDisplay(_caret), 0, up ? -1 : 1));
            if (c == _caret)
                c = up ? 0 : uint32_t(_text.size()); // first/last line: to the edge
        }
        moveTo(c, shift);
        return true;
    }
    case K::Home:
    case K::End: {
        const bool end = e.key == K::End;
        moveTo(
            (m & plat::ModCtrl) ? (end ? uint32_t(_text.size()) : 0) : lineEdge(_caret, end), shift
        );
        return true;
    }
    case K::Backspace:
        if (hasSelection())
            deleteSelection(EditKind::Backspace);
        else if (_caret > 0) {
            uint32_t from = word ? wordLeft(_caret)
                                 : toModel(currentLayout()->moveCaret(toDisplay(_caret), -1, 0));
            if (from >= _caret)
                from = prevChar(_caret);
            replace(from, _caret, {}, nullptr, 0, EditKind::Backspace);
        }
        return true;
    case K::Delete:
        if (hasSelection())
            deleteSelection(EditKind::DeleteForward);
        else if (_caret < _text.size()) {
            uint32_t to = word ? wordRight(_caret)
                               : toModel(currentLayout()->moveCaret(toDisplay(_caret), 1, 0));
            if (to <= _caret)
                to = nextChar(_caret);
            replace(_caret, to, {}, nullptr, 0, EditKind::DeleteForward);
        }
        return true;
    case K::Enter:
    case K::KpEnter:
        if (onSubmit && !shift && !(m & (plat::ModCtrl | plat::ModAlt | plat::ModSuper))) {
            auto cb = onSubmit;
            if (cb())
                return true;
        }
        replace(a, b, "\n", nullptr, typingFormat(), EditKind::Typing);
        return true;
    case K::Tab:
        if (!flag(WantsTab) || m)
            return false;
        replace(a, b, "\t", nullptr, typingFormat(), EditKind::Typing);
        return true;
    default:
        break;
    }
    if (cmd) {
        switch (e.key) {
        case K::A:
            selectAll();
            return true;
        // Format keys (Ctrl+B, Ctrl+Shift+X, …) belong to the composer's
        // shortcut table, not to every text field.
        case K::C:
            if (shift)
                return false;
            copy();
            return true;
        case K::X:
            if (shift)
                return false;
            cut();
            return true;
        case K::V:
            paste(shift);
            return true;
        case K::Z:
            if (shift)
                redo();
            else
                undo();
            return true;
        case K::Y:
            if (primary == plat::ModCtrl) {
                redo();
                return true;
            }
            return false;
        default:
            return false;
        }
    }
    // Printable keys arrive as TextInput; claim their KeyDown so plain-key
    // shortcuts never fire while typing.
    const bool printable = (e.key >= K::A && e.key <= K::Num9) || e.key == K::Space ||
                           (e.key >= K::Minus && e.key <= K::Slash) ||
                           (e.key >= K::Kp0 && e.key <= K::KpEqual && e.key != K::KpEnter);
    return printable && !(m & (plat::ModCtrl | plat::ModSuper));
}

} // namespace ui
