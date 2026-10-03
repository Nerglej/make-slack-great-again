// mrkdwn → views: message bodies as a column of blocks (paragraphs, list
// items with hanging markers, code blocks, quoted runs with a bar), with
// mentions/channels/@here as pills, links, Unicode emoji as text and custom
// emoji as inline image boxes. Used by the message rows and anything else
// that shows message text (unfurl text, attachment fields).
#pragma once

#include "app/mrkdwn/mrkdwn.h"
#include "app/screens/messages/context_fwd.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace screens {

// A piece of message text the message list selects across (RichOptions::
// labels, MessageRow::selectionLabels): a body label, or a data table's cells
// (TableView). Offsets are bytes of its text (selectableTexts' part).
class SelectableText {
public:
    virtual ui::View &textView()                             = 0; // its geometry
    virtual uint32_t  textSize() const                       = 0;
    // The highlighted range (from == to: none).
    virtual void      selectText(uint32_t from, uint32_t to) = 0;
    // The offset nearest to a point in textView()'s coordinates.
    virtual uint32_t  textOffsetAt(ui::PointF local) const   = 0;
    // Double click: the word around an offset; triple click: the line
    // (a table's row) under a point. [*from, *to).
    virtual void      wordAt(uint32_t offset, uint32_t *from, uint32_t *to) const  = 0;
    virtual void      lineAt(ui::PointF local, uint32_t *from, uint32_t *to) const = 0;

protected:
    ~SelectableText() = default;
};

// A Label that knows what its links point at and paints custom emoji boxes.
// Clicks go through the Context (openUrl / openConversation / openProfile).
class RichLabel : public ui::Label, public SelectableText {
public:
    struct Target {
        mrkdwn::Kind kind;
        std::string  data; // URL, user id, conversation id, …
    };
    RichLabel(Context &ctx, ui::View *imageWaiter);
    ~RichLabel() override;

    // Replaces the text; targets[i] is linkId i + 1, images[i] is box id i + 1.
    void setContent(
        text::AttributedText t, std::vector<Target> targets, std::vector<std::string> images
    );
    void          activate(uint32_t linkId);
    // The target under a local point (null: plain text).
    const Target *targetAt(ui::PointF local) const;

    // Message text the list selects across: the I-beam over plain text.
    void setSelectable(bool on) { _selectable = on; }
    bool selectable() const { return _selectable; }

    ui::View &textView() override { return *this; }
    uint32_t  textSize() const override { return uint32_t(text().size()); }
    void      selectText(uint32_t from, uint32_t to) override { setSelection(from, to); }
    uint32_t  textOffsetAt(ui::PointF local) const override { return offsetAt(local); }
    void      wordAt(uint32_t offset, uint32_t *from, uint32_t *to) const override;
    void      lineAt(ui::PointF local, uint32_t *from, uint32_t *to) const override;

    void        paint(gfx::Painter &p) override;
    // Hover on a mention or name: the profile card; right click on a link:
    // the link menu.
    bool        onEvent(ui::Event &e) override;
    uint8_t     cursorAt(ui::PointF local) const override;
    void        windowChanged() override;
    // Link hover: the URL above the pointer, at once (none when the
    // link's text is the URL itself).
    std::string tooltip() const override { return _tip; }
    ui::RectF   tooltipAnchor() const override;
    bool        tooltipImmediate() const override { return true; }

private:
    void hoverLink(uint32_t linkId, ui::PointF windowPos);

    Context                 &_ctx;
    ui::View                *_waiter; // repainted when an emoji image lands
    std::vector<Target>      _targets;
    std::vector<std::string> _images;
    std::string              _tip;
    ui::PointF               _tipAt;
    uint32_t                 _hoverLink  = 0;
    model::UserRef           _hoverUser  = model::kNoUser;
    plat::TimerId            _animTimer  = 0; // the next frame of an animated emoji
    bool                     _selectable = false;
};

struct RichOptions {
    ui::Font                       font     = ui::Font::Body;
    ui::C                          color    = ui::C::Text;
    bool                           edited   = false; // append " (edited)" to the last text block
    int                            maxLines = 0;     // > 0: one paragraph, ellipsized (previews)
    float                          scale  = 1; // the font size times this (Block Kit headers: 1.1)
    // Stop at this byte offset of the parsed text with "…" (previewCut);
    // UINT32_MAX = the whole text.
    uint32_t                       cut    = UINT32_MAX;
    // Collects the text labels made, in order (the message list's selection).
    std::vector<SelectableText *> *labels = nullptr;
};

// Appends the blocks of `mrkdwnText` to `column` (a Column view).
// `imageWaiter` is the view to repaint when custom emoji images arrive (the row).
void buildBody(
    Context           &ctx,
    ui::View          *column,
    std::string_view   mrkdwnText,
    const RichOptions &o,
    ui::View          *imageWaiter
);

// The whole of a mrkdwn string as one styled run of text (no paragraphs or
// targets): table cells. With `images`, custom emoji are inline boxes whose
// id i is (*images)[i - 1] (appended to what it holds; paintEmojiBoxes).
text::AttributedText richText(
    Context                  &ctx,
    std::string_view          mrkdwnText,
    const RichOptions        &o,
    std::vector<std::string> *images = nullptr
);

// Draws the custom emoji boxes of a laid-out text at `origin` (box id i is
// images[i - 1]), animated on the shared clock; `waiter` is repainted when
// an image lands. Returns the ms until the next frame changes (-1: none) —
// when to repaint for the animation.
double paintEmojiBoxes(
    Context                        &ctx,
    gfx::Painter                   &p,
    const text::Layout             &l,
    ui::PointF                      origin,
    const std::vector<std::string> &images,
    ui::View                       *waiter
);

// Repaints a view that paints emoji boxes itself (paintEmojiBoxes) when
// its animated emoji change frame — RichLabel's timer.
class EmojiFrameTimer {
public:
    EmojiFrameTimer(Context &ctx, ui::View &view) : _ctx(ctx), _view(view) {}
    ~EmojiFrameTimer();
    void schedule(double ms); // paintEmojiBoxes' answer; < 0: nothing to do

private:
    Context      &_ctx;
    ui::View     &_view;
    plat::TimerId _id = 0;
};

// The preview cut over the text the user reads, on a budget shared by
// several texts: the byte offset where the preview ends (both budgets drop
// to 0), or UINT32_MAX when all of it fits (what it used is taken off).
uint32_t previewCut(std::string_view mrkdwnText, int *maxChars, int *maxLines);

// The texts of the labels buildBody makes for `mrkdwnText` (o.labels gets
// the same ones, same order): what a selection spanning them copies, also
// when their rows are gone.
std::vector<std::string> bodyTexts(Context &ctx, std::string_view mrkdwnText, const RichOptions &o);

// plainText (common/message_text.h) of the Context's Store.
std::string plainText(const Context &ctx, std::string_view mrkdwnText, bool fullUrls = false);

// A link from a message: mailto: opens the mail app — none registered, the
// address is copied and a toast says so at `at` — else Context::openUrl.
void openLink(Context &ctx, const std::string &url, ui::Window *w, ui::PointF at);

// A tooltip chip above a click for `ms`.
void showClickToast(Context &ctx, ui::Window &w, const std::string &text, int ms, ui::PointF at);

// A conversation's place label: "#name", a DM peer's name, a group DM's name or
// "group message"; "" for a conversation this workspace can't see.
std::string placeLabel(const Store &st, std::string_view convId);
// What a message permalink's chip reads.
std::string messageLinkLabel(const Store &st, const mrkdwn::MessageRef &ref);

} // namespace screens
