#include "harness.h"

using namespace uitest;
using K = plat::Key;

namespace {

struct EditWin : Win {
    ui::TextEdit *edit;
    EditWin() : Win(400, 300) {
        auto *col = w->root().add<ui::View>();
        col->style().items(ui::Align::Stretch).justifyContent(ui::Justify::End);
        edit = col->add<ui::TextEdit>();
        frame();
        const ui::RectF r = edit->windowRect();
        click(r.x + r.w - 5, r.y + r.h / 2);
    }
    const uint32_t primary = plat::primaryMod();
#ifdef __APPLE__
    const uint32_t word = plat::ModAlt;
#else
    const uint32_t word = plat::ModCtrl;
#endif
};

std::string runsDesc(const ui::TextEdit &e) {
    std::string s;
    for (const auto &r : e.runs()) {
        s += std::to_string(r.start) + "-" + std::to_string(r.end) + ":" +
             std::to_string(int(r.format));
        if (!r.link.empty())
            s += "@" + std::string(r.link);
        s += " ";
    }
    return s;
}

} // namespace

TEST("textedit: typing, caret movement, backspace, delete") {
    EditWin t;
    REQUIRE(t.edit->focused());
    t.type("hello");
    CHECK_STR(t.edit->text(), "hello");
    CHECK(t.edit->caret() == 5);
    t.key(K::Left);
    t.key(K::Left);
    t.key(K::Backspace);
    CHECK_STR(t.edit->text(), "helo");
    CHECK(t.edit->caret() == 2);
    t.key(K::Delete);
    CHECK_STR(t.edit->text(), "heo");
    t.key(K::End);
    t.text("é ü");
    CHECK_STR(t.edit->text(), "heoé ü");
    t.key(K::Backspace); // one code point, two bytes
    CHECK_STR(t.edit->text(), "heoé ");
    t.key(K::Home);
    CHECK(t.edit->caret() == 0);
}

TEST("textedit: selection with shift, word jumps, select all replaces") {
    EditWin t;
    t.type("hello big world");
    t.chord(t.word, K::Left);
    CHECK(t.edit->caret() == 10);
    t.chord(t.word | plat::ModShift, K::Left);
    CHECK(t.edit->anchor() == 10 && t.edit->caret() == 6);
    CHECK_STR(t.edit->selectedText(), "big ");
    t.key(K::Right); // collapses to the selection end
    CHECK(t.edit->caret() == 10 && !t.edit->hasSelection());
    t.chord(t.word | plat::ModShift, K::Right);
    CHECK_STR(t.edit->selectedText(), "world");
    t.chord(t.word, K::Backspace);
    CHECK_STR(t.edit->text(), "hello big ");
    t.chord(t.primary, K::A);
    CHECK(t.edit->selectedText() == "hello big ");
    t.type("x");
    CHECK_STR(t.edit->text(), "x");
}

TEST("textedit: undo coalesces a typing run, redo restores") {
    EditWin t;
    t.type("hello world");
    t.key(K::Enter); // no onSubmit: newline (its own undo step)
    t.type("again");
    CHECK_STR(t.edit->text(), "hello world\nagain");
    t.chord(t.primary, K::Z);
    CHECK_STR(t.edit->text(), "hello world\n");
    t.chord(t.primary, K::Z);
    CHECK_STR(t.edit->text(), "hello world");
    t.chord(t.primary, K::Z);
    CHECK_STR(t.edit->text(), "");
    CHECK(!t.edit->canUndo());
    t.chord(t.primary | plat::ModShift, K::Z);
    CHECK_STR(t.edit->text(), "hello world");
    t.key(K::Backspace);
    t.key(K::Backspace);
    t.chord(t.primary, K::Z); // the two backspaces are one step
    CHECK_STR(t.edit->text(), "hello world");
    CHECK(t.edit->caret() == 11);
}

TEST("textedit: formats on selection and while typing, runs and undo") {
    EditWin t;
    t.type("make it bold");
    t.edit->setSelection(8, 12);
    t.edit->toggleFormat(ui::TextEdit::Bold); // the composer's Ctrl+B
    CHECK_STR(runsDesc(*t.edit), "0-8:0 8-12:1 ");
    CHECK(t.edit->formatActive(ui::TextEdit::Bold));
    CHECK_STR(t.edit->html(), "<b>bold</b>");
    t.chord(t.primary, K::Z);
    CHECK_STR(runsDesc(*t.edit), "0-12:0 ");
    // Typing format: toggle italic with no selection, type, toggle off.
    t.key(K::End);
    t.edit->toggleFormat(ui::TextEdit::Italic);
    t.type(" now");
    t.edit->toggleFormat(ui::TextEdit::Italic);
    t.type(" plain");
    CHECK_STR(runsDesc(*t.edit), "0-12:0 12-16:2 16-22:0 ");
    // Links.
    t.edit->setSelection(0, 4);
    t.edit->setLink("https://example.com");
    CHECK_STR(runsDesc(*t.edit), "0-4:0@https://example.com 4-12:0 12-16:2 16-22:0 ");
    t.edit->setSelection(4, 4);
    t.type("X"); // typing right after a link does not extend it
    CHECK_STR(runsDesc(*t.edit), "0-4:0@https://example.com 4-13:0 13-17:2 17-23:0 ");
}

TEST("textedit: copy puts text and html on the clipboard; paste keeps formats") {
    EditWin t;
    t.type("one two");
    t.edit->setSelection(4, 7);
    t.edit->toggleFormat(ui::TextEdit::Bold);
    t.edit->setSelection(0, 7);
    t.chord(t.primary, K::C);
    std::optional<std::string> html, plain;
    app().platform().requestClipboard("text/html", [&](auto v) { html = v; });
    app().platform().requestClipboard("text/plain;charset=utf-8", [&](auto v) { plain = v; });
    t.frame();
    REQUIRE(html && plain);
    CHECK_STR(*plain, "one two");
    CHECK_STR(*html, "one <b>two</b>");
    t.key(K::End);
    t.type(" ");
    t.chord(t.primary, K::V);
    t.frame();
    CHECK_STR(t.edit->text(), "one two one two");
    // The typed space continued the bold before it; the pasted fragment keeps its own.
    CHECK_STR(runsDesc(*t.edit), "0-4:0 4-8:1 8-12:0 12-15:1 ");
    // Plain paste (Shift) drops the formats.
    t.chord(t.primary | plat::ModShift, K::V);
    t.frame();
    CHECK_STR(t.edit->text(), "one two one twoone two");
    CHECK_STR(runsDesc(*t.edit), "0-4:0 4-8:1 8-12:0 12-22:1 ");
    // …the typing format carried on from the bold text before the caret,
    // which is what every editor does for pasted plain text.
    t.chord(t.primary, K::A);
    t.chord(t.primary, K::X);
    CHECK_STR(t.edit->text(), "");
}

TEST("textedit: IME preedit shows inline, commit replaces it") {
    EditWin t;
    t.type("ab");
    t.key(K::Left);
    t.preedit("ni", 2);
    CHECK_STR(t.edit->text(), "ab");
    CHECK_STR(t.edit->preedit(), "ni");
    CHECK(t.edit->caret() == 1);
    t.preedit("nih", 3);
    CHECK_STR(t.edit->preedit(), "nih");
    t.key(K::Left); // the IME owns navigation while composing
    CHECK(t.edit->caret() == 1);
    t.preedit("");
    t.text("你好");
    CHECK_STR(t.edit->text(), "a你好b");
    CHECK(t.edit->preedit().empty());
    CHECK(t.edit->caret() == 1 + 6);
    // The candidate window follows the caret.
    const ui::RectF c = t.edit->caretRect();
    CHECK(c.h > 0);
}

TEST("textedit: Enter submits, Shift+Enter inserts a newline") {
    EditWin t;
    int     sent     = 0;
    t.edit->onSubmit = [&] {
        ++sent;
        t.edit->clear();
        return true;
    };
    t.type("hi");
    t.chord(plat::ModShift, K::Enter);
    t.type("there");
    CHECK_STR(t.edit->text(), "hi\nthere");
    t.key(K::Enter);
    CHECK(sent == 1 && t.edit->text().empty());
}

TEST("textedit: grows line by line up to maxLines, then scrolls inside") {
    EditWin t;
    t.edit->setMaxLines(3);
    t.frame();
    const float h1 = t.edit->frame().h;
    t.type("a");
    t.chord(plat::ModShift, K::Enter);
    t.type("b");
    t.frame();
    const float h2 = t.edit->frame().h;
    CHECK(h2 > h1);
    for (int i = 0; i < 6; ++i) {
        t.chord(plat::ModShift, K::Enter);
        t.type("c");
    }
    t.frame();
    const float h8 = t.edit->frame().h;
    CHECK(h8 < h1 * 3.5f);
    CHECK(near(h8 - h1, 2 * (h2 - h1), 1.5f));
    // The caret (last line) is visible inside the box.
    const ui::RectF c = t.edit->caretRect();
    CHECK(c.y >= 0 && c.y + c.h <= t.edit->frame().h + 0.5f);
}

TEST("textedit: click places the caret, double click selects a word, triple a line") {
    EditWin t;
    t.type("alpha beta gamma");
    const ui::RectF r = t.edit->windowRect();
    t.edit->setSelection(8, 8);
    const ui::RectF c8 = t.edit->caretRect(); // inside "beta"
    t.edit->setSelection(0, 0);
    t.clickN(r.x + c8.x + 1, r.y + c8.y + c8.h / 2, 1);
    CHECK(t.edit->caret() == 8);
    t.clickN(r.x + c8.x + 1, r.y + c8.y + c8.h / 2, 2);
    CHECK_STR(t.edit->selectedText(), "beta");
    t.clickN(r.x + c8.x + 1, r.y + c8.y + c8.h / 2, 3);
    CHECK_STR(t.edit->selectedText(), "alpha beta gamma");
    t.clickN(r.x + 2, r.y + c8.y + c8.h / 2, 1);
    t.clickN(r.x + c8.x + 1, r.y + c8.y + c8.h / 2, 1, plat::ModShift); // shift extends
    CHECK(t.edit->anchor() == 0 && t.edit->caret() == 8);
}

TEST("textedit: caret blinks on the system period while focused") {
    EditWin   t;
    const int ms = app().settings().caretBlinkMs;
    if (ms <= 0)
        return;
    t.frame();
    const int f0 = t.w->stats().frames;
    // Within ~2 periods the caret toggles at least once, repainting its rect only.
    CHECK(t.until([&] { return t.w->stats().frames > f0; }, 3000));
    CHECK(t.damageArea() < 20 * 40);
}

TEST("textedit: a popup that closes when its field loses focus survives deactivation") {
    Win   w(400, 300);
    auto  pop  = std::make_unique<ui::Popup>();
    auto *edit = pop->add<ui::TextEdit>();
    edit->style().size(200, 30);
    pop->setAnchor({20, 20, 10, 10}, ui::Popup::Place::Below);
    ui::Popup *p        = w.w->showPopup(std::move(pop));
    edit->onFocusChange = [p](bool on) {
        if (!on)
            p->close();
    };
    w.frame();
    edit->focus();
    REQUIRE(w.w->focusView() == edit);
    w.w->handle({.type = plat::EventType::FocusOut}); // the window loses the keyboard
    w.frame();
    CHECK(w.w->topPopup() == nullptr);
    CHECK(w.w->focusView() == nullptr);
}

TEST("html: reader handles blocks, entities, links, whitespace") {
    std::string              text;
    std::vector<uint16_t>    fmt;
    std::vector<std::string> links;
    ui::rich::fromHtml(
        "<html><head><style>p{}</style></head><body>"
        "<p>Hi   <B>there</B></p>\n<p><a href=\"https://x.io/?a=1&amp;b=2\">link</a>"
        " &amp; <i>more</i>&nbsp;&#x263A;</p><!-- c --><pre>a  b\nc</pre></body>",
        &text,
        &fmt,
        &links
    );
    CHECK_STR(text, "Hi there\nlink & more \xe2\x98\xba\na  b\nc");
    REQUIRE(fmt.size() == text.size());
    CHECK(fmt[3] == ui::TextEdit::Bold);
    CHECK(fmt[9] == (1 << 8)); // "link"
    REQUIRE(links.size() == 1);
    CHECK_STR(links[0], "https://x.io/?a=1&b=2");
    CHECK(fmt[text.find("more")] == ui::TextEdit::Italic);
    CHECK(fmt[text.find("a  b")] == ui::TextEdit::Code);
}

TEST("html: Google Docs' normal-weight <b> wrapper is not bold; writer escapes") {
    std::string              text;
    std::vector<uint16_t>    fmt;
    std::vector<std::string> links;
    ui::rich::fromHtml(
        "<meta charset=\"utf-8\"><b style=\"font-weight:normal;\" id=\"docs\">"
        "<span style=\"font-weight:700\">Bold</span><span> plain</span></b>",
        &text,
        &fmt,
        &links
    );
    CHECK_STR(text, "Bold plain");
    CHECK(fmt[0] == ui::TextEdit::Bold && fmt[5] == 0);
    const std::vector<uint16_t> f(5, 0);
    CHECK_STR(ui::rich::toHtml("a<b&\n", f.data(), {}), "a&lt;b&amp;<br>");
}
