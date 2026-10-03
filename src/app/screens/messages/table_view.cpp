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

} // namespace

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
            const text::Layout &l = _wrapped[r][c] ? *_wrapped[r][c] : *_natural[r][c];
            const ui::PointF    o = v.snapPx({x + kPadX, y + kPadY});
            l.paint(p, o);
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
    _anim      = std::make_unique<EmojiFrameTimer>(ctx, *this);
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
        // Rich cells (mentions, links, emoji) as the body draws them; the
        // header bold, the last row dimmed when rows were cut ("more
        // below"). Custom emoji boxes are numbered across the table.
        const size_t total = _cells.size();
        const size_t shown = std::min(total, size_t(kMaxInlineTableRows));
        std::vector<std::vector<text::AttributedText>> texts(shown);
        _images.clear();
        for (size_t r = 0; r < shown; ++r)
            for (size_t c = 0; c < _cells[r].size(); ++c) {
                RichOptions o;
                o.font  = r == 0 ? ui::Font::BodyBold : ui::Font::Body;
                o.color = shown < total && r + 1 == shown ? C::TextFaint : C::Text;
                texts[r].push_back(richText(_ctx, _cells[r][c], o, &_images));
                ui::resolveSpans(texts[r].back());
            }
        _grid.setCells(std::move(texts), scale);
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
        text::AttributedText t;
        t.append(i18n::tr("Open full table"), ui::font(ui::Font::Body, C::TooltipText));
        _pillText = text::Layout::build(t, {}, windowScale());
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
    return View::cursorAt(local);
}

} // namespace screens
