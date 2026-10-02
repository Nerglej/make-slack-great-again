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
    _builtW = -1;
    _pillText.reset();
    View::styleChanged();
}

void TableView::build(float width) {
    const float scale = window() ? window()->scale() : 1.f;
    if (_builtW == width && _builtScale == scale)
        return;
    _builtW            = width;
    _builtScale        = scale;
    const size_t total = _cells.size();
    const size_t shown = std::min(total, size_t(kMaxInlineTableRows));
    size_t       cols  = 0;
    for (size_t r = 0; r < shown; ++r)
        cols = std::max(cols, _cells[r].size());
    // Rich cells (mentions, links, emoji) as the body draws them; the
    // header bold, the last row dimmed when rows were cut ("more below").
    // Custom emoji boxes are numbered across the table (the measuring pass
    // keeps none).
    _images.clear();
    auto cellText = [&](size_t r, size_t c, std::vector<std::string> *images) {
        RichOptions o;
        o.font  = r == 0 ? ui::Font::BodyBold : ui::Font::Body;
        o.color = shown < total && r + 1 == shown ? C::TextFaint : C::Text;
        return richText(_ctx, _cells[r][c], o, images);
    };
    std::vector<std::string> measured;
    std::vector<float>       nat(cols, 2 * kPadX);
    _layouts.clear();
    _layouts.resize(shown);
    for (size_t r = 0; r < shown; ++r)
        for (size_t c = 0; c < _cells[r].size(); ++c) {
            text::AttributedText t = cellText(r, c, &measured);
            ui::resolveSpans(t);
            nat[c] =
                std::max(nat[c], std::ceil(text::Layout::build(t, {}, scale)->width()) + 2 * kPadX);
        }
    float sum = 0;
    for (float v : nat)
        sum += v;
    _colW     = nat;
    _squeezed = sum > width + 0.5f;
    if (_squeezed)
        for (float &v : _colW)
            v = std::max(2 * kPadX + 8, std::floor(v * width / sum));
    _tableW = 0;
    for (float v : _colW)
        _tableW += v;
    _rowH.assign(shown, 0);
    _tableH = 0;
    for (size_t r = 0; r < shown; ++r) {
        float h = 0;
        for (size_t c = 0; c < cols; ++c) {
            if (c >= _cells[r].size()) {
                _layouts[r].push_back(nullptr);
                continue;
            }
            text::AttributedText t = cellText(r, c, &_images);
            ui::resolveSpans(t);
            text::LayoutOptions o;
            o.maxWidth = std::max(1.f, _colW[c] - 2 * kPadX);
            _layouts[r].push_back(text::Layout::build(t, o, scale));
            h = std::max(h, _layouts[r].back()->height());
        }
        if (h == 0)
            h = text::metrics(ui::font(ui::Font::Body), scale).ascent +
                text::metrics(ui::font(ui::Font::Body), scale).descent;
        _rowH[r] = std::ceil(h) + 2 * kPadY;
        _tableH += _rowH[r];
    }
}

bool TableView::clipped() {
    return _cells.size() > size_t(kMaxInlineTableRows) || _squeezed;
}

ui::SizeF TableView::measureContent(float aw, float) {
    build(aw >= ui::kInf ? 600 : aw);
    return {_tableW, _tableH + 2 * kMarginV};
}

void TableView::layout() {
    build(width());
}

void TableView::paint(gfx::Painter &p) {
    build(width());
    const float y0 = kMarginV, tw = _tableW;
    // msga's paintDataTableChrome: header tint, a hairline under it, row
    // rules, a 1-px frame of radius 6; no vertical lines.
    float       y = y0;
    for (size_t r = 0; r < _layouts.size(); ++r) {
        const float rh = _rowH[r];
        if (r == 0) {
            p.save();
            p.clipRoundRect({0, y0, tw, _tableH}, 6);
            p.fillRect({0, y, tw, rh}, ui::color(C::TableHeaderBg));
            p.restore();
        } else {
            p.fillRect({1, y, tw - 2, 1}, ui::color(r == 1 ? C::TableBorder : C::TableRowRule));
        }
        float x = 0;
        for (size_t c = 0; c < _layouts[r].size(); ++c) {
            if (const text::Layout *l = _layouts[r][c].get()) {
                const ui::PointF o = snapPx({x + kPadX, y + kPadY});
                l->paint(p, o);
                if (!_images.empty())
                    _anim->schedule(paintEmojiBoxes(_ctx, p, *l, o, _images, this));
            }
            x += _colW[c];
        }
        y += rh;
    }
    p.strokeRoundRect({0.5f, y0 + 0.5f, tw - 1, _tableH - 1}, 6, 1, ui::color(C::TableBorder));
}

ui::RectF TableView::pillRect() const {
    if (!hovered() || !const_cast<TableView *>(this)->clipped() || !_pillText)
        return {};
    // Centred on the visible part of the table (it can be taller than the
    // list), inset from its right edge.
    const ui::RectF  win = windowRect();
    const ui::PointF o   = mapToWindow({0, 0});
    const float      top = std::max(kMarginV, win.y - o.y);
    const float      bot = std::min(kMarginV + _tableH, win.y + win.h - o.y);
    if (bot <= top)
        return {};
    const float w = std::ceil(_pillText->width()) + kPillIconPad;
    const float x = std::max(0.f, _tableW - w - ui::metric(ui::M::SpaceS));
    return {x, std::floor((top + bot) / 2 - kPillH / 2), w, kPillH};
}

void TableView::paintOver(gfx::Painter &p) {
    if (!hovered() || !clipped())
        return;
    if (!_pillText) {
        text::AttributedText t;
        t.append(i18n::tr("Open full table"), ui::font(ui::Font::Body, C::TooltipText));
        _pillText = text::Layout::build(t, {}, window() ? window()->scale() : 1.f);
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
