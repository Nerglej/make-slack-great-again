// The canvas tab (msga's CanvasPage, src/ui/canvas_page): a big borderless
// title line over the body, autosaved a few seconds after typing stops, in a
// column as wide as Slack's (1040 px); a floating "⋮" with "Copy link" and
// "Delete canvas" once the canvas exists. A conversation without one opens a
// blank page: the first save creates the canvas, then the tab names it.
//
// The asymmetry is Slack's: a canvas reads as the HTML its file serves and
// saves as canvas markdown. msga edited it as rich text (QTextBrowser); here
// the body is a TextEdit holding the markdown's block syntax as text ("## ",
// "- ", "1. ", "> ", "| a | b |") with bold, italic, strike, code and links
// as formats. Saves are msga's section diff (screens/common/canvas_doc.h):
// only the sections that changed are written, the whole document when that
// can't be done safely. There is no co-editing.
#pragma once

#include "screens/common/context.h"
#include "ui/ui.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace shell {

class CanvasPage : public ui::View {
public:
    explicit CanvasPage(screens::Context &ctx);
    ~CanvasPage() override;

    // Shows conv's canvas (Conversation::canvasId; none: a blank page that
    // creates it on the first save). The same canvas again keeps the editor,
    // refetching only when nothing is unsaved.
    void open(model::ConvRef conv);
    // A canvas file shared in conv (the canvas viewer): the same page on
    // that file. title: the file's (the title line until the meta answers).
    void openFile(model::ConvRef conv, const std::string &fileId, const std::string &title);
    // After "Delete canvas" succeeded (the viewer closes itself).
    std::function<void()> onDeleted;
    // Saves pending edits now (tab or conversation switch, sign-out).
    void                  flushPendingSave();
    void                  clear();

    // The title as typed, for the tab (only while the canvas exists).
    std::function<void(const std::string &title)> onTitleChanged;
    std::function<void(const std::string &error)> onError;

    ui::TextEdit &title() const { return *_title; }
    ui::TextEdit &body() const { return *_body; }
    ui::View     *menuButton() const { return _menuBtn; }
    ui::Label    *notice() const { return _notice; }
    void          showMenu();
    void          confirmDelete(); // the menu's "Delete canvas"

private:
    enum class ReadOnly : uint8_t { None, NotAddressable, NoAccess };
    void loadContent();
    void applyRemoteHtml(const std::string &html);
    void setBody(const std::string &editorHtml);
    void setReadOnly(ReadOnly cause);
    void armSave();
    void dropTimer();

    screens::Context     &_ctx;
    ui::ScrollView       *_scroll  = nullptr;
    ui::Label            *_notice  = nullptr;
    ui::TextEdit         *_title   = nullptr;
    ui::TextEdit         *_body    = nullptr;
    ui::View             *_menuBtn = nullptr;
    model::ConvRef        _conv    = model::kNoConv;
    std::string           _fileId;        // "" = not created yet
    std::string           _permalink;     // "Copy link"; "" until the meta answered
    std::string           _lastHtml;      // the server's last answer (unchanged: no reset)
    std::string           _serverTitle;   // the file's title: names the title h1
    uint64_t              _seq       = 0; // invalidates answers for an earlier open
    uint32_t              _saveTimer = 0; // plat::TimerId
    ReadOnly              _ro        = ReadOnly::None;
    bool                  _loading = false, _refetching = false, _saving = false;
    bool                  _bodyDirty = false, _titleDirty = false;
    std::shared_ptr<bool> _alive;
};

// The conversion halves (canvas_page.cpp), exposed for tests.
namespace canvas {
// Canvas HTML → the body's text as HTML for TextEdit::insertHtml: a <p> per
// block with its markdown prefix, inline formats kept. A leading <h1> whose
// text is one of `titles` is taken out into *title.
std::string
editorHtml(std::string_view html, const std::vector<std::string> &titles, std::string *title);
// The body back to canvas markdown: block prefixes as typed, formats as
// **b** _i_ ~~s~~ `c` [t](u), a blank line between paragraphs.
std::string markdown(const ui::TextEdit &body);
} // namespace canvas

} // namespace shell
