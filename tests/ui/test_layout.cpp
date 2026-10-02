#include "harness.h"

#include <cmath>

using namespace uitest;
using ui::Align;
using ui::Justify;

TEST("layout: row with fixed, grow and gap") {
    Win   w(400, 300);
    auto *row = w.root().add<ui::View>();
    row->style().row().padding(10).spacing(5);
    auto *a = row->add<Box>();
    a->style().width(50);
    auto *b = row->add<Box>();
    b->style().flex(1);
    auto *c = row->add<Box>();
    c->style().width(30);
    w.frame();
    CHECK(row->frame().w == 400 && row->frame().h == 300);
    CHECK(a->frame().x == 10 && a->frame().w == 50);
    CHECK(b->frame().x == 65 && b->frame().w == 290);
    CHECK(c->frame().x == 360 && c->frame().w == 30);
    CHECK(a->frame().y == 10 && a->frame().h == 280); // stretched across
}

TEST("layout: grow shares by weight, min/max clamp") {
    Win   w(400, 100);
    auto *row = w.root().add<ui::View>();
    row->style().row();
    auto *a = row->add<Box>();
    a->style().flex(1);
    auto *b = row->add<Box>();
    b->style().flex(3);
    w.frame();
    CHECK(a->frame().w == 100 && b->frame().w == 300);
    b->style().maxW = 200;
    w.frame();
    CHECK(b->frame().w == 200);
}

TEST("layout: column align center, justify end") {
    Win   w(200, 200);
    auto *col = w.root().add<ui::View>();
    col->style().column().items(Align::Center).justifyContent(Justify::End).spacing(10);
    auto *a = col->add<Box>(40, 20);
    auto *b = col->add<Box>(60, 30);
    w.frame();
    CHECK(a->frame().w == 40 && a->frame().x == 80);
    CHECK(b->frame().w == 60 && b->frame().x == 70);
    CHECK(b->frame().y + b->frame().h == 200);
    CHECK(a->frame().y == 200 - 30 - 10 - 20);
}

TEST("layout: space-between and self alignment") {
    Win   w(300, 100);
    auto *row = w.root().add<ui::View>();
    row->style().row().items(Align::Start).justifyContent(Justify::SpaceBetween);
    auto *a = row->add<Box>(50, 10);
    auto *b = row->add<Box>(50, 20);
    b->style().alignSelf(Align::End);
    auto *c = row->add<Box>(50, 30);
    c->style().alignSelf(Align::Center);
    w.frame();
    CHECK(a->frame().x == 0 && b->frame().x == 125 && c->frame().x == 250);
    CHECK(a->frame().y == 0 && b->frame().y == 80 && c->frame().y == 35);
}

TEST("layout: overflow shrinks by content, never below min") {
    Win   w(100, 50);
    auto *row = w.root().add<ui::View>();
    row->style().row();
    auto *a = row->add<Box>(80, 10);
    auto *b = row->add<Box>(80, 10);
    w.frame();
    CHECK(a->frame().w == 50 && b->frame().w == 50);
    a->style().minW = 70;
    b->style().noShrink();
    w.frame();
    CHECK(b->frame().w == 80); // shrink 0 keeps its size
    CHECK(a->frame().w == 70); // min wins over the overflow
}

TEST("layout: margins, stack alignment, padding") {
    Win   w(200, 100);
    auto *st = w.root().add<ui::View>();
    st->style().stack().padding(10).items(Align::End);
    auto *a = st->add<Box>(30, 20);
    a->style().margins(0, 0, 5, 5);
    auto *b = st->add<Box>(30, 20);
    b->style().alignSelf(Align::Stretch);
    w.frame();
    CHECK(a->frame().x == 200 - 10 - 5 - 30 && a->frame().y == 100 - 10 - 5 - 20);
    CHECK(b->frame().x == 10 && b->frame().w == 180 && b->frame().h == 80);
}

TEST("layout: hidden children take no space") {
    Win   w(300, 100);
    auto *row = w.root().add<ui::View>();
    row->style().row().spacing(10);
    auto *a = row->add<Box>(50, 10);
    auto *b = row->add<Box>(50, 10);
    auto *c = row->add<Box>(50, 10);
    w.frame();
    CHECK(c->frame().x == 120);
    b->setVisible(false);
    w.frame();
    CHECK(c->frame().x == 60);
    CHECK(a->frame().x == 0);
}

TEST("layout: nested measure — a row sizes to wrapped content") {
    Win   w(400, 300);
    auto *col = w.root().add<ui::View>();
    col->style().column().items(Align::Start);
    auto *row = col->add<ui::View>();
    row->style().row().padding(4).spacing(6);
    row->add<Box>(20, 20);
    row->add<Box>(30, 40);
    w.frame();
    CHECK(row->frame().w == 4 + 20 + 6 + 30 + 4);
    CHECK(row->frame().h == 4 + 40 + 4);
}

TEST("layout: text wraps narrower, gets taller") {
    Win   w(600, 400);
    auto *col = w.root().add<ui::View>();
    col->style().column().items(Align::Start);
    auto *l = col->add<ui::Label>(
        "The quick brown fox jumps over the lazy dog, again and again, until it is tired."
    );
    l->style().width(500);
    w.frame();
    const float wide = l->frame().h;
    l->style().width(120);
    w.frame();
    CHECK(wide > 0);
    CHECK(l->frame().h > wide * 1.9f);
}

TEST("layout: only the dirty subtree re-lays out") {
    Win   w(400, 300);
    auto *row = w.root().add<ui::View>();
    row->style().row();
    // Two fixed-size panes are layout boundaries: a change inside one must
    // not re-run the other's layout.
    auto *left = row->add<Box>();
    left->style().size(200, 300);
    left->setLayoutBoundary(true);
    auto *right = row->add<Box>();
    right->style().size(200, 300);
    right->setLayoutBoundary(true);
    auto *leftChild  = left->add<Box>(10, 10);
    auto *rightChild = right->add<Box>(10, 10);
    w.frame();
    const int l0 = left->layouts, r0 = right->layouts, rc0 = rightChild->layouts;
    leftChild->content = {20, 20};
    leftChild->invalidateLayout();
    w.frame();
    CHECK(left->layouts == l0 + 1);
    CHECK(right->layouts == r0);
    CHECK(rightChild->layouts == rc0);
    CHECK(leftChild->frame().h == 20);
}

TEST("layout: measure cache — unchanged views are not re-measured") {
    struct Counting : Box {
        int measures = 0;
        using Box::Box;
        ui::SizeF measureContent(float a, float b) override {
            ++measures;
            return Box::measureContent(a, b);
        }
    };
    Win   w(400, 300);
    auto *col = w.root().add<ui::View>();
    auto *a   = col->add<Counting>(10, 10);
    auto *b   = col->add<Counting>(10, 10);
    w.frame();
    const int b0 = b->measures;
    a->content   = {10, 30};
    a->invalidateLayout();
    w.frame();
    CHECK(b->measures == b0);
    CHECK(a->frame().h == 30);
    CHECK(b->frame().y == 30);
}

TEST("layout: per-view footprint stays small") {
    // Every live row of a list is a handful of these; keep them lean.
    std::printf(
        "    sizeof View %zu, Label %zu, Clickable %zu, Button %zu, Image %zu, "
        "TextEdit %zu, VirtualList %zu\n",
        sizeof(ui::View),
        sizeof(ui::Label),
        sizeof(ui::Clickable),
        sizeof(ui::Button),
        sizeof(ui::Image),
        sizeof(ui::TextEdit),
        sizeof(ui::VirtualList)
    );
    CHECK(sizeof(ui::View) <= 192);
    CHECK(sizeof(ui::Label) <= sizeof(ui::View) + 112);
}

namespace plat::testing_internal {
void setHeadlessScale(Window &w, double s);
}

TEST("layout: a label never re-wraps because its frame was snapped to the pixel grid") {
    // Regression (msga-next at 1.25x): a link preview's "Figment" label got a
    // frame up to one physical pixel narrower than its measured width after
    // the flex edges were snapped to the grid, re-laid out at that width and
    // wrapped its last letter onto a second line, over the title below.
    for (double scale : {1.25, 1.5, 1.75}) {
        Win w(400, 200);
        plat::testing_internal::setHeadlessScale(w.native(), scale);
        auto                    *col = w.root().add<ui::View>();
        std::vector<ui::Label *> labels;
        for (int i = 0; i < 24; ++i) {
            auto *row = col->add<ui::View>();
            row->style().row().spacing(6).items(ui::Align::Center);
            row->style().pad.l = 0.37f * float(i); // every sub-pixel phase
            row->add<Box>(16, 16);
            labels.push_back(
                row->add<ui::Label>(i % 2 ? "Figment" : "GitForge", ui::Font::SmallBold)
            );
        }
        w.frame();
        for (ui::Label *l : labels) {
            REQUIRE(l->textLayout() != nullptr);
            CHECK(l->textLayout()->lineCount() == 1);
        }
    }
}

namespace {
struct HideWatcher : Box {
    int hidden = 0;
    HideWatcher() { watchAncestorHide(); }
    void hiddenByAncestor() override { ++hidden; }
};
} // namespace

TEST("visibility: a watcher learns an ancestor was hidden, others don't pay") {
    Win   w(200, 100);
    auto *outer = w.root().add<ui::View>();
    auto *inner = outer->add<ui::View>();
    auto *watch = inner->add<HideWatcher>();
    auto *side  = w.root().add<ui::View>();
    w.frame();
    side->setVisible(false); // not an ancestor
    CHECK(watch->hidden == 0);
    outer->setVisible(false);
    CHECK(watch->hidden == 1);
    outer->setVisible(true);
    inner->setVisible(false);
    CHECK(watch->hidden == 2);
    watch->setVisible(false); // its own: visibilityChanged, not this
    CHECK(watch->hidden == 2);
    // Detached and back: still watched; destroyed: dropped from the list.
    std::unique_ptr<ui::View> out = outer->remove(inner);
    outer->adopt(std::move(out));
    outer->setVisible(false);
    CHECK(watch->hidden == 3);
    outer->clearChildren();
    outer->setVisible(true);
    outer->setVisible(false); // must not touch the destroyed watcher
}

TEST("label: a selection is the system highlight with the text white on it") {
    Win   w(300, 60);
    auto *l = w.root().add<ui::Label>("MMMMMMMMMM", ui::Font::Body, ui::C::Text);
    l->style().alignSelf(Align::Start);
    w.frame();
    l->setSelection(0, 10);
    w.frame();
    REQUIRE(l->textLayout() != nullptr);
    const std::vector<ui::RectF> sel = l->textLayout()->selectionRects(0, 10);
    REQUIRE(!sel.empty());
    const ui::PointF o  = l->textOrigin();
    const ui::RectF  wr = l->windowRect();
    const ui::RectF  r{wr.x + o.x + sel[0].x, wr.y + o.y + sel[0].y, sel[0].w, sel[0].h};
    const float      sc = w.w->scale();
    int              hi = 0, white = 0, dark = 0;
    for (int y = int(std::ceil(r.y * sc)); y < int((r.y + r.h) * sc); ++y)
        for (int x = int(std::ceil(r.x * sc)); x < int((r.x + r.w) * sc); ++x) {
            uint32_t px = 0;
            if (!hooks().readPixel(w.native(), x, y, &px))
                continue;
            px &= 0xffffff;
            hi += px == (ui::systemHighlight() & 0xffffff);
            white += px == 0xffffff;
            dark += ((px >> 16) & 0xff) < 0x40 && ((px >> 8) & 0xff) < 0x40 && (px & 0xff) < 0x40;
        }
    CHECK(hi > 0);
    CHECK(white > 0); // the glyphs on the highlight
    CHECK(dark == 0); // none of the normal (near-black) text left
}

// Inline code pads its pill by 2 px a side; the white copy a selection paints
// must keep that padding, or everything after the pill shows shifted under
// the highlight ("buil|ild").
TEST("label: selected text after inline code stays where it was drawn") {
    Win                  w(400, 60);
    auto                *l = w.root().add<ui::Label>();
    text::AttributedText a;
    text::Style          plain, code;
    plain.color     = ui::themed(ui::C::Text);
    code.color      = plain.color;
    code.mono       = true;
    code.background = ui::themed(ui::C::CodeBg);
    a.append("x ", plain);
    a.append("main.cpp", code);
    a.append(" and ", plain);
    a.append("--demo", code);
    a.append(" in every build", plain);
    const uint32_t from = uint32_t(a.text.size() - 5); // "build"
    const uint32_t to   = uint32_t(a.text.size());
    l->setRichText(std::move(a));
    l->style().alignSelf(Align::Start);
    w.frame();
    REQUIRE(l->textLayout() != nullptr);
    const std::vector<ui::RectF> sel = l->textLayout()->selectionRects(from, to);
    REQUIRE(sel.size() == 1);
    const ui::PointF o  = l->textOrigin();
    const ui::RectF  wr = l->windowRect();
    const ui::RectF  r{wr.x + o.x + sel[0].x, wr.y + o.y + sel[0].y, sel[0].w, sel[0].h};
    const float      sc         = w.w->scale();
    // Ink columns inside the selection box: pixels unlike the box's corner.
    auto             inkColumns = [&] {
        std::vector<int> cols;
        const int        x0 = int(std::ceil(r.x * sc)), x1 = int((r.x + r.w) * sc);
        const int        y0 = int(std::ceil(r.y * sc)), y1 = int((r.y + r.h) * sc);
        uint32_t         bg = 0;
        hooks().readPixel(w.native(), x0, y0, &bg);
        for (int x = x0; x < x1; ++x)
            for (int y = y0; y < y1; ++y) {
                uint32_t px = 0;
                if (hooks().readPixel(w.native(), x, y, &px) &&
                    (px & 0xffffff) != (bg & 0xffffff)) {
                    cols.push_back(x);
                    break;
                }
            }
        return cols;
    };
    const std::vector<int> before = inkColumns();
    l->setSelection(from, to);
    w.frame();
    const std::vector<int> after = inkColumns();
    REQUIRE(!before.empty() && !after.empty());
    CHECK(std::abs(before.front() - after.front()) <= 1);
    CHECK(std::abs(before.back() - after.back()) <= 1);
}
