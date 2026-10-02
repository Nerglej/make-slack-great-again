// The rows of a MessageList (internal to screens/messages). A row is rebuilt
// from the Store on every bind; only ~20 exist at a time (VirtualList
// recycles them per kind), so building children per bind is cheap and keeps
// no second copy of any message.
#pragma once

#include "app/screens/messages/image_cache.h"
#include "app/screens/messages/message_list.h"

#include <string>
#include <vector>

namespace screens {

enum RowKind : int { kRowFull, kRowGrouped, kRowDay, kRowSystem, kRowDivider };

// Children laid out left to right, wrapping onto new lines (reaction pills).
class FlowRow : public ui::View {
public:
    explicit FlowRow(float gap) : _gap(gap) {}
    ui::SizeF measureContent(float availW, float availH) override;
    void      layout() override;

private:
    ui::SizeF place(float width, bool apply);
    float     _gap;
};

// msga's plain file chip (the message list's, the forward preview's).
ui::Clickable *addFileChip(ui::View *parent, const model::File &f, MessageList *list, Ts ts);

class RichLabel;

// msga's huddleSummaryText: a huddle row's sentence ("Mira and Jonas were in
// the huddle for 1h 5m.").
std::string huddleSummaryText(const Store &st, const model::Huddle &h);

// The texts a message's selection runs over (MessageRow::selectionLabels'),
// for rows not on screen: its body, or its text blocks.
std::vector<std::string> selectableTexts(Context &ctx, const model::Message &m);

// A canvas card (msga's paintCanvasCard): its title as shown (CanvasDisplay::
// title), and the start of the document as text — headings larger, list
// markers, inline formats, member mentions as chips with names, emoji (custom
// ones as inline boxes, box id i is (*images)[i - 1]); no pictures.
std::string          canvasTitle(const Context &ctx, const model::File &f);
text::AttributedText canvasPreviewText(
    Context &ctx, const std::string &html, const model::File &f, std::vector<std::string> *images
);

class MessageRow : public ui::View {
public:
    MessageRow(MessageList &list, int kind);
    ~MessageRow() override;

    void                            bind(const MessageList::Item &item);
    Ts                              ts() const { return _ts; }
    // The message's text labels a selection runs over, in order.
    const std::vector<RichLabel *> &selectionLabels() const { return _sel; }
    // An attachment card under the pointer (index), for the "×" in the
    // gutter beside it (msga's dismiss button on link previews).
    void                            attachHovered(int index, ui::View *card, bool on);

    void        paint(gfx::Painter &p) override;
    void        paintOver(gfx::Painter &p) override;
    bool        onEvent(ui::Event &e) override;
    std::string tooltip() const override;
    ui::RectF   tooltipAnchor() const override;
    bool        tooltipImmediate() const override { return true; }

private:
    void buildMessage(const model::Message &m, bool grouped);
    void buildSystem(const model::Message &m);
    void buildHeader(ui::View *col, const model::Message &m, bool tight);
    // The message's content under the header: body or blocks, buttons,
    // files, attachments (index ≥ 0 cards get the dismiss "×"), reactions.
    void buildContent(ui::View *col, const model::Message &m, bool root);
    void buildBlocks(
        ui::View                        *col,
        const std::vector<model::Block> &blocks,
        Ts                               ts,
        int                              attachment,
        bool                             edited,
        std::vector<RichLabel *>        *labels
    );
    void      buildReactions(ui::View *col, const model::Message &m);
    void      buildThreadSummary(ui::View *col, const model::Message &m);
    void      buildInlineThread(ui::View *col, const model::Message &root);
    void      buildFile(ui::View *col, const model::File &f, Ts ts);
    void      buildGallery(ui::View *col, const std::vector<const model::File *> &files, Ts ts);
    void      buildAttachment(ui::View *col, const model::Message &m, size_t index, bool root);
    void      buildUnfurl(ui::View *col, const model::Message &m, size_t index);
    ui::RectF dismissRect() const; // local; empty while no card is hovered
    bool      dismissable(int index) const;
    // `file`: an uploaded image (msga's placeholder box and "Loading image…"
    // while it loads); else a link preview's picture (nothing until then).
    ui::View *addThumb(
        ui::View          *col,
        const std::string &path,
        int                w,
        int                h,
        int                maxW,
        int                maxH,
        float              gapAbove = 6,
        bool               file     = false
    );

    MessageList                  &_list;
    std::string                   _hoverTime; // grouped rows: the time shown in the gutter on hover
    std::unique_ptr<text::Layout> _hoverLayout;
    // msga's mini-banners over the message: "Pinned by …" and the saved /
    // reminder strip (text; laid out when painted).
    std::string                   _pinText, _savedText;
    std::unique_ptr<text::Layout> _pinLayout, _savedLayout;
    std::vector<RichLabel *>      _sel;
    ui::View                     *_attachCard = nullptr; // the hovered attachment card
    int                           _attach     = -1;      // … its index
    Ts                            _ts         = 0;
    bool                          _reminded   = false;
    int                           _kind;
    bool                          _pending = false, _overDismiss = false;
};

// "Today" pill on a hairline.
class DayRow : public ui::View {
public:
    DayRow();
    void setText(std::string s) { _label->setText(std::move(s)); }
    void paint(gfx::Painter &p) override;

private:
    ui::Label *_label;
};

// Thread mode: "3 replies ———".
class DividerRow : public ui::View {
public:
    DividerRow();
    void setText(std::string s) { _label->setText(std::move(s)); }

private:
    ui::Label *_label;
};

} // namespace screens
