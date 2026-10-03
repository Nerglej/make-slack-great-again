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

    Context                                                &_ctx;
    std::vector<std::vector<std::string>>                   _cells;
    std::vector<std::vector<std::unique_ptr<text::Layout>>> _layouts;
    std::vector<std::string>                                _images; // emoji box id i: [i - 1]
    std::unique_ptr<EmojiFrameTimer>                        _anim;
    std::vector<float>                                      _colW, _rowH;
    std::unique_ptr<text::Layout>                           _pillText;
    float _builtW = -1, _builtScale = 0, _tableW = 0, _tableH = 0;
    bool  _squeezed = false, _overPill = false;
};

} // namespace screens
