#include "app/screens/messages/table_view.h"

#include "app/screens/messages/message_dialogs.h"
#include "app/screens/messages/rich.h"
#include "base/i18n.h"
#include "gfx/icons_generated.h"

#include <algorithm>
#include <cmath>

namespace screens {

using ui::C;

namespace {

constexpr float kMarginV = 8;          // the table's 8-px margins
constexpr float kPadX = 12, kPadY = 5; // cell padding
constexpr float kPillH = 28, kPillIconPad = 40;

// The rows an inline table shows, rich (mentions, links, emoji) as the body
// draws them: the header bold, the last row dimmed when rows were cut ("more
// below"). Custom emoji boxes are numbered across the table.
std::vector<std::vector<text::AttributedText>> inlineCells(
    Context                                     &ctx,
    const std::vector<std::vector<std::string>> &cells,
    std::vector<std::string>                    *images
) {
    const size_t total = cells.size();
    const size_t shown = std::min(total, size_t(kMaxInlineTableRows));
    std::vector<std::vector<text::AttributedText>> texts(shown);
    for (size_t r = 0; r < shown; ++r)
        for (size_t c = 0; c < cells[r].size(); ++c) {
            RichOptions o;
            o.font  = r == 0 ? ui::Font::BodyBold : ui::Font::Body;
            o.color = shown < total && r + 1 == shown ? C::TextFaint : C::Text;
            texts[r].push_back(richText(ctx, cells[r][c], o, images));
        }
    return texts;
}

} // namespace

std::string tableText(Context &ctx, const std::vector<std::vector<std::string>> &cells) {
    std::vector<std::string> images;
    std::string              out;
    const auto               texts = inlineCells(ctx, cells, &images);
    for (size_t r = 0; r < texts.size(); ++r) {
        if (r)
            out += '\n';
        for (size_t c = 0; c < texts[r].size(); ++c) {
            if (c)
                out += '\t';
            out += texts[r][c].text;
        }
    }
    return out;
}

// ── TableGrid ───────────────────────────────────────────────────────────────

void TableGrid::setCells(std::vector<std::vector<text::AttributedText>> cells, float scale) {
    _texts      = std::move(cells);
    _scale      = scale;
    size_t cols = 0;
    for (const auto &r : _texts)
        cols = std::max(cols, r.size());
    _natW.assign(cols, 2 * kPadX);
    _natural.clear();
    _natural.resize(_texts.size());
    for (size_t r = 0; r < _texts.size(); ++r)
        for (size_t c = 0; c < _texts[r].size(); ++c) {
            _natural[r].push_back(text::Layout::build(_texts[r][c], {}, scale));
            _natW[c] = std::max(_natW[c], std::ceil(_natural[r][c]->width()) + 2 * kPadX);
        }
    _naturalW = 0;
    for (float v : _natW)
        _naturalW += v;
    // Offsets as tableText joins the cells; a row's last entry is its end.
    uint32_t off = 0;
    _base.assign(_texts.size(), {});
    for (size_t r = 0; r < _texts.size(); ++r) {
        if (r)
            ++off;
        for (size_t c = 0; c < _texts[r].size(); ++c) {
            if (c)
                ++off;
            _base[r].push_back(off);
            off += uint32_t(_texts[r][c].text.size());
        }
        _base[r].push_back(off);
    }
    _textSize = off;
    _selFrom = _selTo = 0;
}

void TableGrid::setSelection(uint32_t from, uint32_t to) {
    _selFrom = std::min(from, _textSize);
    _selTo   = std::min(to, _textSize);
}

size_t TableGrid::rowIndexAt(float y) const {
    float top = 0;
    for (size_t r = 0; r + 1 < _rowH.size(); ++r) {
        top += _rowH[r];
        if (y < top)
            return r;
    }
    return _rowH.size() - 1;
}

uint32_t TableGrid::offsetAt(ui::PointF p) const {
    if (_rowH.empty())
        return 0;
    const size_t r   = rowIndexAt(p.y);
    float        top = 0;
    for (size_t i = 0; i < r; ++i)
        top += _rowH[i];
    if (_texts[r].empty())
        return _base[r].back();
    // The column under x (past a short row's end: its last cell).
    size_t c = 0;
    float  x = 0;
    while (c + 1 < _texts[r].size() && p.x >= x + _colW[c])
        x += _colW[c++];
    return _base[r][c] + cell(r, c).hitTest({p.x - x - kPadX, p.y - top - kPadY}).offset;
}

void TableGrid::wordAt(uint32_t offset, uint32_t *from, uint32_t *to) const {
    *from = *to = offset;
    for (size_t r = 0; r < _texts.size() && r < _wrapped.size(); ++r)
        for (size_t c = 0; c < _texts[r].size(); ++c) {
            const uint32_t b = _base[r][c], n = uint32_t(_texts[r][c].text.size());
            if (offset < b || offset > b + n)
                continue;
            *from = b + cell(r, c).wordStart(offset - b);
            *to   = b + cell(r, c).wordEnd(offset - b);
            return;
        }
}

void TableGrid::rowAt(float y, uint32_t *from, uint32_t *to) const {
    *from = *to = 0;
    if (_rowH.empty())
        return;
    const size_t r = rowIndexAt(y);
    *from          = _base[r].front();
    *to            = _base[r].back();
}

void TableGrid::fit(float width, float minColW, float emptyRowH) {
    _colW     = _natW;
    _squeezed = _naturalW > width + 0.5f;
    if (_squeezed)
        for (float &v : _colW)
            v = std::max(minColW, std::floor(v * width / _naturalW));
    _tableW = 0;
    for (float v : _colW)
        _tableW += v;
    _wrapped.clear();
    _wrapped.resize(_texts.size());
    _rowH.assign(_texts.size(), 0);
    _tableH = 0;
    for (size_t r = 0; r < _texts.size(); ++r) {
        float h = 0;
        _wrapped[r].resize(_texts[r].size());
        for (size_t c = 0; c < _texts[r].size(); ++c) {
            // Wider than its column now: wrapped to it.
            const float         maxW = std::max(1.f, _colW[c] - 2 * kPadX);
            const text::Layout *l    = _natural[r][c].get();
            if (l->width() > maxW) {
                text::LayoutOptions o;
                o.maxWidth     = maxW;
                _wrapped[r][c] = text::Layout::build(_texts[r][c], o, _scale);
                l              = _wrapped[r][c].get();
            }
            h = std::max(h, l->height());
        }
        if (h == 0)
            h = emptyRowH;
        _rowH[r] = std::ceil(h) + 2 * kPadY;
        _tableH += _rowH[r];
    }
}

void TableGrid::paint(
    gfx::Painter                   &p,
    ui::View                       &v,
    float                           y0,
    float                           top,
    float                           bottom,
    Context                        *ctx,
    const std::vector<std::string> &images,
    EmojiFrameTimer                *anim
) const {
    const float tw = _tableW;
    // The table's chrome: header tint, a hairline under it, row
    // rules, a 1-px frame of radius 6; no vertical lines.
    float       y  = y0;
    for (size_t r = 0; r < _rowH.size() && y < bottom; y += _rowH[r], ++r) {
        const float rh = _rowH[r];
        if (y + rh <= top)
            continue;
        if (r == 0) {
            p.save();
            p.clipRoundRect({0, y0, tw, _tableH}, 6);
            p.fillRect({0, y, tw, rh}, ui::color(C::TableHeaderBg));
            p.restore();
        } else {
            p.fillRect({1, y, tw - 2, 1}, ui::color(r == 1 ? C::TableBorder : C::TableRowRule));
        }
        float x = 0;
        for (size_t c = 0; c < _natural[r].size(); ++c) {
            const text::Layout &l = cell(r, c);
            const ui::PointF    o = v.snapPx({x + kPadX, y + kPadY});
            l.paint(p, o);
            // A message selection: the system highlight, the text on it
            // white (as Label draws one).
            const uint32_t b = _base[r][c], n = uint32_t(_texts[r][c].text.size());
            const uint32_t lo = std::max(_selFrom, b), hi = std::min(_selTo, b + n);
            if (hi > lo)
                for (const gfx::RectF &sr : l.selectionRects(lo - b, hi - b)) {
                    const ui::RectF rr{sr.x + o.x, sr.y + o.y, sr.w, sr.h};
                    p.save();
                    p.clipRect(rr);
                    p.fillRect(rr, ui::systemHighlight());
                    l.paintAs(p, o, 0xffffffff);
                    p.restore();
                }
            if (ctx && !images.empty())
                anim->schedule(paintEmojiBoxes(*ctx, p, l, o, images, &v));
            x += _colW[c];
        }
    }
    p.strokeRoundRect({0, y0, tw, _tableH}, 6, 1, ui::color(C::TableBorder));
}

// ── TableView ───────────────────────────────────────────────────────────────

TableView::TableView(Context &ctx, std::vector<std::vector<std::string>> cells)
    : _ctx(ctx), _cells(std::move(cells)) {
    setHoverRepaint(true);
    setRole(ui::Role::Group);
    _anim = std::make_unique<EmojiFrameTimer>(ctx, *this);
    // Made once: their text's length (tableText's) now, shaped on the first build().
    _made = inlineCells(ctx, _cells, &_images);
    for (size_t r = 0; r < _made.size(); ++r) {
        _textSize += r > 0; // '\n'
        for (size_t c = 0; c < _made[r].size(); ++c)
            _textSize += uint32_t(_made[r][c].text.size()) + (c > 0); // '\t'
    }
    onOpenFull = [this] {
        if (window())
            showTableViewer(*window(), _ctx, _cells);
    };
}

TableView::~TableView() = default;

std::string TableView::accessibleName() const {
    std::string out;
    if (!_cells.empty())
        for (size_t c = 0; c < _cells[0].size(); ++c)
            out += (c ? " | " : "") + _cells[0][c];
    return out;
}

void TableView::styleChanged() {
    _grid   = TableGrid(); // the cells' colours and sizes are the style's
    _builtW = -1;
    _pillText.reset();
    View::styleChanged();
}

void TableView::build(float width) {
    const float scale = windowScale();
    if (_grid.scale() != scale) {
        auto texts = std::move(_made);
        _made.clear();
        if (texts.empty()) {
            _images.clear();
            texts = inlineCells(_ctx, _cells, &_images);
        }
        for (auto &row : texts)
            for (text::AttributedText &t : row)
                ui::resolveSpans(t);
        _grid.setCells(std::move(texts), scale);
        _grid.setSelection(_selFrom, _selTo);
        _builtW = -1;
    }
    if (_builtW == width)
        return;
    _builtW               = width;
    const text::Metrics m = text::metrics(ui::font(ui::Font::Body), scale);
    _grid.fit(width, 2 * kPadX + 8, m.ascent + m.descent);
}

bool TableView::clipped() {
    return _cells.size() > size_t(kMaxInlineTableRows) || _grid.squeezed();
}

ui::SizeF TableView::measureContent(float aw, float) {
    build(aw >= ui::kInf ? 600 : aw);
    return {_grid.width(), _grid.height() + 2 * kMarginV};
}

void TableView::layout() {
    build(width());
}

void TableView::paint(gfx::Painter &p) {
    build(width());
    _grid.paint(p, *this, kMarginV, -ui::kInf, ui::kInf, &_ctx, _images, _anim.get());
}

void TableView::selectText(uint32_t from, uint32_t to) {
    from = std::min(from, _textSize);
    to   = std::min(to, _textSize);
    if (to < from)
        std::swap(from, to);
    if (from == _selFrom && to == _selTo)
        return;
    _selFrom = from;
    _selTo   = to;
    _grid.setSelection(from, to);
    update();
}

uint32_t TableView::textOffsetAt(ui::PointF local) const {
    return _grid.offsetAt({local.x, local.y - kMarginV});
}

void TableView::wordAt(uint32_t offset, uint32_t *from, uint32_t *to) const {
    _grid.wordAt(offset, from, to);
}

// The table row under the point.
void TableView::lineAt(ui::PointF local, uint32_t *from, uint32_t *to) const {
    _grid.rowAt(local.y - kMarginV, from, to);
}

ui::RectF TableView::pillRect() const {
    if (!hovered() || !const_cast<TableView *>(this)->clipped() || !_pillText)
        return {};
    // Centred on the visible part of the table (it can be taller or wider
    // than the list).
    const ui::RectF  win   = windowRect();
    const ui::PointF o     = mapToWindow({0, 0});
    const float      top   = std::max(kMarginV, win.y - o.y);
    const float      bot   = std::min(kMarginV + _grid.height(), win.y + win.h - o.y);
    const float      left  = std::max(0.f, win.x - o.x);
    const float      right = std::min(_grid.width(), win.x + win.w - o.x);
    if (bot <= top || right <= left)
        return {};
    const float w = std::ceil(_pillText->width()) + kPillIconPad;
    const float x = std::max(left, std::floor((left + right) / 2 - w / 2));
    return {x, std::floor((top + bot) / 2 - kPillH / 2), w, kPillH};
}

void TableView::paintOver(gfx::Painter &p) {
    if (!hovered() || !clipped())
        return;
    if (!_pillText) {
        _pillText = text::layoutPlain(
            i18n::tr("Open full table"), ui::font(ui::Font::Body, C::TooltipText), windowScale()
        );
    }
    const ui::RectF r = pillRect();
    if (r.w <= 0)
        return;
    p.fillRoundRect(r, r.h / 2, ui::color(C::TooltipBg));
    gfx::drawIcon(
        p, gfx::Icon::Maximize2, {r.x + 10, r.y + (r.h - 14) / 2, 14, 14}, ui::color(C::TooltipText)
    );
    _pillText->paint(
        p, snapPx({r.x + 10 + 14 + 6, r.y + std::floor((r.h - _pillText->height()) / 2)})
    );
}

bool TableView::onEvent(ui::Event &e) {
    switch (e.type) {
    case ui::EventType::PointerMove: {
        const bool over = pillRect().contains(e.pos);
        if (over != _overPill) {
            _overPill = over;
            update();
        }
        return false;
    }
    case ui::EventType::PointerLeave:
        _overPill = false;
        update();
        return false;
    case ui::EventType::PointerDown:
        return e.button == plat::Button::Left && pillRect().contains(e.pos);
    case ui::EventType::PointerUp:
        if (pillRect().contains(e.pos) && onOpenFull) {
            auto fn = onOpenFull;
            fn();
        }
        return true;
    default:
        return false;
    }
}

uint8_t TableView::cursorAt(ui::PointF local) const {
    if (pillRect().contains(local))
        return uint8_t(plat::Cursor::Hand);
    if (_selectable)
        return uint8_t(plat::Cursor::IBeam);
    return View::cursorAt(local);
}

} // namespace screens
