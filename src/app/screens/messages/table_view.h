// The inline data table (with the list's "Open full table" pill): a Block Kit table in a message,
// at most kMaxInlineTableRows rows, row 0 the header in bold. No vertical lines: a rounded 1-px
// frame, a tinted header with a hairline under it, rules between rows. Columns take their natural
// width and shrink in proportion (wrapping) when the message column is narrower. When rows were cut
// (the last one shown dimmed) or columns squeezed, hovering the table shows the "Open full table"
// pill, which opens the table viewer.
#pragma once

#include "app/screens/messages/context_fwd.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace screens {

class EmojiFrameTimer;

constexpr int kMaxInlineTableRows = 10;

// The table itself, for TableView and the table viewer (message_dialogs.h):
// cells shaped once at their natural width, columns shrunk in proportion
// (wrapping; only the cells that no longer fit are shaped again) when the
// width is short of them, rows as tall as their tallest cell; drawn with the
// header tint, the rules and the frame, only the rows in view.
class TableGrid {
public:
    // The cells' text, row-major (rows may be short), shaped at `scale`.
    void setCells(std::vector<std::vector<text::AttributedText>> cells, float scale);
    // Columns for `width` (logical px): a squeezed one keeps at least
    // minColW; a row without text is emptyRowH high (plus the padding).
    void fit(float width, float minColW, float emptyRowH);
    // From y0 down in view `v`'s coordinates, the rows that meet [top,
    // bottom) only. With a Context, the cells' custom emoji boxes too (box id
    // i is images[i - 1]; `anim` repaints v for their next frame).
    void paint(
        gfx::Painter                   &p,
        ui::View                       &v,
        float                           y0,
        float                           top,
        float                           bottom,
        Context                        *ctx,
        const std::vector<std::string> &images,
        EmojiFrameTimer                *anim
    ) const;
    bool  empty() const { return _texts.empty(); }
    float scale() const { return _scale; }
    float naturalWidth() const { return _naturalW; } // every column at its natural width
    float width() const { return _tableW; }
    float height() const { return _tableH; }
    bool  squeezed() const { return _squeezed; }

private:
    std::vector<std::vector<text::AttributedText>>          _texts;
    std::vector<std::vector<std::unique_ptr<text::Layout>>> _natural, _wrapped;
    std::vector<float>                                      _colW, _natW, _rowH;
    float _scale = 0, _naturalW = 0, _tableW = 0, _tableH = 0;
    bool  _squeezed = false;
};

class TableView : public ui::View {
public:
    // cells: mrkdwn, row-major.
    TableView(Context &ctx, std::vector<std::vector<std::string>> cells);
    ~TableView() override;

    // The pill was pressed (default: the table viewer with every row, the
    // cells rich as here).
    std::function<void()> onOpenFull;

    bool      clipped();        // rows cut or columns squeezed (the pill's condition)
    ui::RectF pillRect() const; // local; empty unless clipped and hovered

    ui::SizeF   measureContent(float availW, float availH) override;
    void        layout() override;
    void        paint(gfx::Painter &p) override;
    void        paintOver(gfx::Painter &p) override;
    bool        onEvent(ui::Event &e) override;
    uint8_t     cursorAt(ui::PointF local) const override;
    void        styleChanged() override;
    // The header cells, " | " between them.
    std::string accessibleName() const override;

private:
    void build(float width);

    Context                              &_ctx;
    std::vector<std::vector<std::string>> _cells;
    TableGrid                             _grid;
    std::vector<std::string>              _images; // emoji box id i: [i - 1]
    std::unique_ptr<EmojiFrameTimer>      _anim;
    std::unique_ptr<text::Layout>         _pillText;
    float                                 _builtW   = -1;
    bool                                  _overPill = false;
};

} // namespace screens
