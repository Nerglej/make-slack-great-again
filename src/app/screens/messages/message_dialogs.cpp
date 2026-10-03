#include "app/screens/messages/message_dialogs.h"

#include "app/screens/messages/image_cache.h"
#include "app/screens/messages/rich.h"
#include "app/screens/messages/rows.h"
#include "base/file.h"
#include "base/i18n.h"
#include "base/str.h"
#include "base/time.h"
#include "gfx/icons_generated.h"
#include "ui/controls.h"
#include "ui/datetime.h"

#include <algorithm>
#include <cmath>

namespace screens {

using i18n::arg;
using i18n::tr;
using ui::Align;
using ui::C;
using ui::FormButton;
using V = ui::FormButton::Kind;

namespace {

// A rounded frame: 1-px border, fill.
class Frame final : public ui::View {
public:
    Frame(C border, C fill, float radius) : _b(border), _f(fill), _r(radius) {}
    void paint(gfx::Painter &p) override {
        const ui::RectF b = bounds();
        p.fillRoundRect(b, _r, ui::color(_f));
        p.strokeRoundRect(b, _r, 1, ui::color(_b));
        View::paint(p);
    }

private:
    C     _b, _f;
    float _r;
};

std::string authorName(const Context &ctx, const model::Message &m) {
    if (m.extra && !m.extra->botName.empty())
        return m.extra->botName;
    return std::string(ctx.store().user(m.user).label());
}

} // namespace

ui::View *addMessagePreview(
    Context &ctx, ui::View *parent, const model::Message &m, float bodyH, bool files
) {
    auto *card = parent->add<Frame>(C::FormHighlightStrong, C::FileChipBg, 6);
    card->style().padding(0, 8, 0, 8).spacing(4).noShrink();
    // "<b>name</b>  <time>" (the time in 11 px: tertiary in the forward
    // dialog, secondary in the delete dialog; both read the same here).
    auto *head = card->add<ui::View>();
    head->style().row().padding(12, 0, 12, 0).spacing(8).items(Align::End);
    ui::styledLabel(
        head, authorName(ctx, m), ui::pxFont(15, text::Weight::Bold, ui::color(C::FormText)), 1
    );
    ui::styledLabel(
        head,
        base::formatTime(model::tsSecs(m.ts)),
        ui::pxFont(
            11, text::Weight::Regular, ui::color(files ? C::FormTextFaint : C::FormTextMuted)
        ),
        1
    );
    head->add<ui::View>()->style().flex(1);
    if (!m.text.empty()) {
        auto *scroll = card->add<ui::ScrollView>();
        scroll->style().height(bodyH);
        scroll->content()->style().padding(12, 0, 0, 0).spacing(2);
        buildBody(ctx, scroll->content(), m.text, {}, scroll);
    }
    if (files)
        for (const model::File &f : m.files()) {
            auto *row = card->add<ui::View>();
            row->style().padding(12, 0, 12, 0).spacing(2);
            if (f.isImage()) { // name, then the image within 300×150
                ui::styledLabel(
                    row,
                    f.name,
                    ui::pxFont(15 * 0.82f, text::Weight::Regular, ui::color(C::FileNameDim)),
                    1
                );
                float       w = float(f.width > 0 ? f.width : 300),
                            h = float(f.height > 0 ? f.height : 150);
                const float s = std::min({1.f, 300 / w, 150 / h});
                auto       *img =
                    row->add<CachedImage>(ctx.images, f.path, ImageCache::Shape::Rounded, 4);
                img->style().size(std::floor(w * s), std::floor(h * s)).alignSelf(Align::Start);
            } else {
                addFileChip(row, f, nullptr, 0);
            }
        }
    return card;
}

// ── Delete message ──────────────────────────────────────────────────────────

ui::Popup *showDeleteMessageDialog(Context &ctx, ui::Window &w, ConvRef conv, Ts ts) {
    const model::Message *m = ctx.store().findMessage(conv, ts);
    if (!m)
        return nullptr;
    auto d = ui::Dialog::confirm(
        tr("Delete message"),
        tr("This action cannot be undone."),
        tr("Delete"),
        V::Danger,
        ui::color(C::FormTextMuted),
        [&](ui::View *content) { addMessagePreview(ctx, content, *m, 160, false); }
    );
    ui::Dialog *raw = d.get();
    d->onAccepted   = [&ctx, conv, ts] { ctx.backend.remove(conv, ts); };
    w.showPopup(std::move(d));
    return raw;
}

// ── Move to thread ──────────────────────────────────────────────────────────

std::string movedMessageText(const Context &ctx, const model::Message &m, bool withNote) {
    std::string out;
    if (withNote) {
        const int64_t     secs = model::tsSecs(m.ts);
        const std::string note =
            arg(tr("Moved from the channel \xC2\xB7 originally posted by %1 on %2 at %3"),
                authorName(ctx, m),
                base::formatDate(secs, base::nowSecs()),
                base::formatTime(secs));
        out = str::concat({"_", note, "_"});
    }
    if (!str::trim(m.text).empty())
        out = out.empty() ? m.text : str::concat({out, "\n\n", m.text});
    // The files go along as links (the original, and with it the upload,
    // is deleted once the copy is in).
    for (const model::File &f : m.files()) {
        if (f.permalink.empty())
            continue;
        const std::string link =
            f.name.empty() ? f.permalink : str::concat({"<", f.permalink, "|", f.name, ">"});
        out = out.empty() ? link : str::concat({out, "\n", link});
    }
    return out;
}

namespace {

class ThreadRow final : public ui::Clickable {
public:
    ThreadRow(Context &ctx, const model::Message &root, int index) : index(index) {
        setLook({C::None, C::None, C::None, C::None, 0});
        style().row().height(60).padding(24, 0, 24, 0).spacing(12).items(Align::Center);
        const std::string author = authorName(ctx, root);
        auto             *av     = add<CachedImage>(
            ctx.images,
            root.extra && !root.extra->botAvatar.empty() ? root.extra->botAvatar
                                                         : ctx.store().user(root.user).avatar,
            ImageCache::Shape::Circle
        );
        av->style().size(36, 36).noShrink();
        auto *col = add<ui::View>();
        col->style().flex(1).spacing(1);
        std::string title = plainText(ctx, root.text);
        for (char &c : title)
            if (c == '\n')
                c = ' ';
        if (str::trim(title).empty())
            title = root.files().empty() ? std::string(tr("(no text)")) : root.files().front().name;
        ui::styledLabel(col, title, ui::pxFont(15, text::Weight::Bold, ui::color(C::FormText)), 1);
        const std::string sub = str::concat(
            {author,
             " \xC2\xB7 ",
             base::formatDateTime(model::tsSecs(root.ts)),
             " \xC2\xB7 ",
             root.replyCount == 1 ? std::string(tr("1 reply"))
                                  : arg(tr("%1 replies"), std::to_string(root.replyCount))}
        );
        ui::styledLabel(
            col, sub, ui::pxFont(15 * 0.88f, text::Weight::Regular, ui::color(C::FormTextMuted)), 1
        );
        key = str::concat({title, " ", author});
        for (char &c : key)
            if (c >= 'A' && c <= 'Z')
                c = char(c + 32);
    }
    void paint(gfx::Painter &p) override {
        p.fillRect(
            bounds(),
            ui::color(
                selected    ? C::FormHighlightStrong
                : hovered() ? C::FormHighlight
                            : C::FormBg
            )
        );
        View::paint(p);
    }
    int         index;
    std::string key;
    bool        selected = false;
};

class MoveDialog final : public ui::Dialog {
public:
    MoveDialog(Context &ctx, ConvRef conv, Ts ts, std::vector<Ts> roots)
        : ui::Dialog(tr("Move to thread"), 0, Scroll::Disabled), _ctx(ctx), _conv(conv), _ts(ts) {
        content()->style().spacing(8);
        ui::styledLabel(
            content(),
            tr("Pick a thread in this channel. The message is posted there again by you, and the "
               "original is deleted."),
            ui::pxFont(12, text::Weight::Regular, ui::color(C::FormTextMuted))
        );
        _filter = content()->add<ui::TextField>(
            tr("Filter threads\xE2\x80\xA6"),
            ui::TextField::Size::Normal,
            uint16_t(gfx::Icon::Search)
        );
        auto *stack = content()->add<ui::View>();
        stack->style().stack().height(280);
        _list = stack->add<ui::ScrollView>();
        for (Ts r : roots)
            if (const model::Message *m = ctx.store().findMessage(conv, r)) {
                auto *row    = _list->content()->add<ThreadRow>(ctx, *m, int(_roots.size()));
                row->onClick = [this, row] { select(row->index); };
                _rows.push_back(row);
                _roots.push_back(r);
            }
        _empty = ui::styledLabel(
            stack,
            _rows.empty() ? tr("No threads in this channel's loaded history yet.")
                          : tr("No threads match."),
            ui::pxFont(14, text::Weight::Regular, ui::color(C::FormTextFaint))
        );
        _empty->setAlign(text::LayoutOptions::Align::Center);
        _empty->style().alignSelf(Align::Center);
        _note = content()->add<ui::CheckBox>(tr("Add a note with the original author and time"));
        _move = makeButton(tr("Move"), V::Primary);
        addButtonRow(_move, makeButton(tr("Cancel"), V::Secondary));
        _move->onClick           = [this] { go(); };
        _filter->edit().onChange = [this] { refilter(); };
        _filter->edit().onSubmit = [this] { return go(), true; };
        _filter->edit().onKey    = [this](const ui::Event &e) {
            if (e.type != ui::EventType::KeyDown ||
                (e.key != plat::Key::Up && e.key != plat::Key::Down))
                return false;
            moveSelection(e.key == plat::Key::Down ? 1 : -1);
            return true;
        };
        refilter();
    }
    void focusFilter() { _filter->edit().focus(); }

private:
    void refilter() {
        std::string q(str::trim(_filter->edit().text()));
        for (char &c : q)
            if (c >= 'A' && c <= 'Z')
                c = char(c + 32);
        int first = -1;
        for (ThreadRow *r : _rows) {
            const bool on = q.empty() || r->key.find(q) != std::string::npos;
            r->setVisible(on);
            if (on && first < 0)
                first = r->index;
        }
        _empty->setVisible(first < 0);
        _list->setVisible(first >= 0);
        select(first); // row 0 preselected after every change
    }
    void select(int i) {
        _sel = i;
        for (ThreadRow *r : _rows) {
            r->selected = r->index == i;
            r->update();
        }
        _move->setEnabled(i >= 0);
        if (i >= 0)
            _list->ensureVisible(_rows[size_t(i)]);
    }
    void moveSelection(int dir) { // wraps at both ends, over the visible rows
        std::vector<int> vis;
        for (ThreadRow *r : _rows)
            if (r->visible())
                vis.push_back(r->index);
        if (vis.empty())
            return;
        auto it = std::find(vis.begin(), vis.end(), _sel);
        int  k  = it == vis.end() ? 0 : int(it - vis.begin()) + dir;
        k       = (k % int(vis.size()) + int(vis.size())) % int(vis.size());
        select(vis[size_t(k)]);
    }
    void go() {
        if (_sel < 0)
            return;
        const model::Message *m = _ctx.store().findMessage(_conv, _ts);
        if (!m || m->pending)
            return;
        const Ts          root = _roots[size_t(_sel)];
        const std::string text = movedMessageText(_ctx, *m, _note->checked());
        Context          &ctx  = _ctx;
        const ConvRef     conv = _conv;
        const Ts          ts   = _ts;
        // The original goes only once the copy is in the thread.
        ctx.backend.send(conv, text, root, [&ctx, conv, ts](bool ok, const std::string &) {
            if (ok) {
                ctx.backend.remove(conv, ts);
                return;
            }
            // The send path has said why; this says what it means for the move.
            if (ctx.backend.onError)
                ctx.backend.onError(
                    tr("Couldn't move the message \xE2\x80\x94 the original is still in place.")
                );
        });
        if (ctx.openThread)
            ctx.openThread(conv, root);
        accept();
    }

    Context                 &_ctx;
    ConvRef                  _conv;
    Ts                       _ts;
    ui::TextField           *_filter = nullptr;
    ui::ScrollView          *_list   = nullptr;
    ui::Label               *_empty  = nullptr;
    ui::CheckBox            *_note   = nullptr;
    FormButton              *_move   = nullptr;
    std::vector<ThreadRow *> _rows;
    std::vector<Ts>          _roots;
    int                      _sel = -1;
};

} // namespace

ui::Popup *
showMoveToThreadDialog(Context &ctx, ui::Window &w, ConvRef conv, Ts ts, std::vector<Ts> roots) {
    auto  d   = std::make_unique<MoveDialog>(ctx, conv, ts, std::move(roots));
    auto *raw = d.get();
    w.showPopup(std::move(d));
    raw->focusFilter();
    return raw;
}

// ── Reminder ────────────────────────────────────────────────────────────────

ui::Popup *showReminderDialog(Context &ctx, ui::Window &w, ConvRef conv, Ts ts) {
    auto              d  = std::make_unique<ui::Dialog>(tr("Reminder"));
    ui::View         *c  = d->content();
    const text::Style lb = ui::pxFont(15, text::Weight::Semibold, ui::color(C::FormTextMuted));
    c->style().spacing(4);
    ui::styledLabel(c, tr("When"), lb);
    auto *date = c->add<ui::DateTimeField>(ui::DateTimeField::Kind::Date);
    c->add<ui::View>()->style().height(0); // + the 4 px spacing
    ui::styledLabel(c, tr("Time"), lb);
    auto *time = c->add<ui::DateTimeField>(ui::DateTimeField::Kind::Time);
    c->add<ui::View>()->style().height(4);
    // Default: now + 1 h, rounded up to the next 5 minutes.
    const int64_t   now         = base::nowSecs();
    base::CivilTime t           = base::localTime(now + 3600);
    const int       add         = (5 - t.minute % 5) % 5;
    t                           = base::localTime(now + 3600 + add * 60 - t.second);
    const base::CivilTime today = base::localTime(now);
    date->setMinimumDate(today.year, today.month, today.day);
    date->setDate(t.year, t.month, t.day);
    time->setTime(t.hour, t.minute);
    auto *save = d->makeButton(tr("Save"), V::Primary);
    d->addButtonRow(save, d->makeButton(tr("Cancel"), V::Secondary));
    ui::Dialog *raw = d.get();
    save->onClick   = [raw] { raw->accept(); };
    d->onAccepted   = [&ctx, conv, ts, date, time] {
        // Due never earlier than a minute from now.
        const int64_t due =
            base::fromLocal(date->year(), date->month(), date->day(), time->hour(), time->minute());
        ctx.backend.setReminder(conv, ts, std::max(due, base::nowSecs() + 60));
    };
    w.showPopup(std::move(d));
    date->focus();
    return raw;
}

// ── Table viewer ────────────────────────────────────────────────────────────

std::vector<std::vector<std::string>> parseCsv(std::string_view s) {
    if (s.substr(0, 3) == "\xEF\xBB\xBF")
        s.remove_prefix(3);
    // The delimiter, from the first line (outside quotes).
    int  tabs = 0, commas = 0, semis = 0;
    bool q = false;
    for (char c : s) {
        if (c == '"')
            q = !q;
        else if (!q && (c == '\n' || c == '\r'))
            break;
        else if (!q)
            tabs += c == '\t', commas += c == ',', semis += c == ';';
    }
    const char delim = tabs > commas && tabs > semis ? '\t' : semis > commas ? ';' : ',';
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string>              row;
    std::string                           cell;
    bool                                  quoted = false, any = false;
    auto                                  endRow = [&] {
        if (any || !cell.empty() || !row.empty()) {
            row.push_back(std::move(cell));
            rows.push_back(std::move(row));
        }
        row.clear();
        cell.clear();
        any = false;
    };
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (quoted) {
            if (c == '"' && i + 1 < s.size() && s[i + 1] == '"')
                cell += '"', ++i;
            else if (c == '"')
                quoted = false;
            else
                cell += c;
        } else if (c == '"' && cell.empty()) {
            quoted = any = true;
        } else if (c == delim) {
            row.push_back(std::move(cell));
            cell.clear();
            any = true;
        } else if (c == '\r' || c == '\n') {
            if (c == '\r' && i + 1 < s.size() && s[i + 1] == '\n')
                ++i;
            endRow();
        } else {
            cell += c;
            any = true;
        }
    }
    endRow();
    return rows;
}

namespace {

// rows: plain text (a CSV file), or mrkdwn with a Context (a Block Kit
// table: cells, mentions, links and emoji as in the
// message).
class TableViewer final : public ui::Popup {
public:
    TableViewer(std::vector<std::vector<std::string>> rows, Context *ctx)
        : _rows(std::move(rows)), _ctx(ctx) {
        if (_ctx)
            _anim = std::make_unique<EmojiFrameTimer>(*_ctx, *this);
        setCard(false);
        setAnchor({}, Place::Fill);
        setPaintOutset(0);
        setFocusable(true);
        style().dir = ui::Dir::None;
    }
    void layout() override { _built = false; }
    void paint(gfx::Painter &p) override {
        p.fillRect(bounds(), ui::color(C::ViewerBackdrop));
        build();
        const ui::RectF c = card();
        p.fillRoundRect(c, 8, ui::color(C::FormBg));
        p.save();
        p.clipRect({c.x + 12, c.y + 12, c.w - 24, c.h - 24});
        p.translate(c.x + 12, c.y + 12 - _scroll);
        // The table's chrome: header tint, a hairline under it,
        // row rules, a 1-px outer border radius 6; no vertical lines.
        const float tw = _tableW, y0 = 8;
        float       y = y0;
        for (size_t r = 0; r < _cells.size(); ++r) {
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
            for (size_t k = 0; k < _cells[r].size(); ++k) {
                if (const text::Layout *l = _cells[r][k].get()) {
                    const ui::PointF o = snapPx({x + 12, y + 5});
                    l->paint(p, o);
                    if (_ctx && !_images.empty())
                        _anim->schedule(paintEmojiBoxes(*_ctx, p, *l, o, _images, this));
                }
                x += _colW[k];
            }
            y += rh;
        }
        p.strokeRoundRect({0, y0, tw, _tableH}, 6, 1, ui::color(C::TableBorder));
        p.restore();
    }
    bool onEvent(ui::Event &e) override {
        const ui::RectF c   = card();
        const float     max = std::max(0.f, _docH - (c.h - 24));
        auto            to  = [&](float v) {
            _scroll = std::clamp(v, 0.f, max);
            update();
        };
        switch (e.type) {
        case ui::EventType::PointerDown:
            if (!c.contains(e.pos))
                close();
            return true;
        case ui::EventType::Scroll:
            to(_scroll - e.dy * (e.precise ? 1.f : 40.f) / 2);
            return true;
        case ui::EventType::KeyDown:
            switch (e.key) {
            case plat::Key::Escape:
                close();
                return true;
            case plat::Key::Up:
                to(_scroll - 40);
                return true;
            case plat::Key::Down:
                to(_scroll + 40);
                return true;
            case plat::Key::PageUp:
                to(_scroll - std::max(40.f, c.h - 48));
                return true;
            case plat::Key::PageDown:
                to(_scroll + std::max(40.f, c.h - 48));
                return true;
            case plat::Key::Home:
                to(0);
                return true;
            case plat::Key::End:
                to(max);
                return true;
            default:
                return true;
            }
        default:
            return Popup::onEvent(e);
        }
    }

private:
    ui::RectF card() const {
        const float w = std::min(_idealW + 24, width() - 96),
                    h = std::min(_docH + 24, height() - 96);
        return {std::round((width() - w) / 2), std::round((height() - h) / 2), w, h};
    }
    void build() {
        if (_built)
            return;
        _built            = true;
        const float textW = std::max(50.f, width() - 96 - 24);
        const float k     = windowScale();
        size_t      cols  = 0;
        for (auto &r : _rows)
            cols = std::max(cols, r.size());
        // Automatic table layout, roughly: natural widths, shrunk in
        // proportion (wrapping) when they don't fit.
        std::vector<float> nat(cols, 24);
        // Row 0 is the header, in bold. Custom emoji boxes are numbered
        // across the whole table (the measuring pass keeps none).
        _images.clear();
        auto cellText = [&](size_t r, size_t c, std::vector<std::string> *images) {
            text::AttributedText t;
            if (_ctx) {
                RichOptions o;
                o.font  = r == 0 ? ui::Font::BodyBold : ui::Font::Body;
                o.color = C::FormText;
                t       = richText(*_ctx, _rows[r][c], o, images);
                ui::resolveSpans(t);
            } else {
                t.append(
                    _rows[r][c],
                    ui::pxFont(
                        15,
                        r == 0 ? text::Weight::Bold : text::Weight::Regular,
                        ui::color(C::FormText)
                    )
                );
            }
            return t;
        };
        std::vector<std::string> measured;
        for (size_t r = 0; r < _rows.size(); ++r)
            for (size_t c = 0; c < _rows[r].size(); ++c) {
                const text::AttributedText t = cellText(r, c, &measured);
                nat[c] = std::max(nat[c], std::ceil(text::Layout::build(t, {}, k)->width()) + 24);
            }
        float sum = 0;
        for (float v : nat)
            sum += v;
        _idealW = sum;
        _colW   = nat;
        if (sum > textW)
            for (float &v : _colW)
                v = std::floor(v * textW / sum);
        _tableW = 0;
        for (float v : _colW)
            _tableW += v;
        _cells.clear();
        _rowH.clear();
        _tableH = 0;
        for (size_t r = 0; r < _rows.size(); ++r) {
            _cells.emplace_back();
            float h = 0;
            for (size_t c = 0; c < cols; ++c) {
                if (c >= _rows[r].size()) {
                    _cells.back().push_back(nullptr);
                    continue;
                }
                const text::AttributedText t = cellText(r, c, &_images);
                text::LayoutOptions        o;
                o.maxWidth = std::max(1.f, _colW[c] - 24);
                _cells.back().push_back(text::Layout::build(t, o, k));
                h = std::max(h, _cells.back().back()->height());
            }
            _rowH.push_back(std::ceil(h) + 10);
            _tableH += _rowH.back();
        }
        _docH = _tableH + 16; // the table's 8-px margins
    }

    std::vector<std::vector<std::string>>                   _rows;
    Context                                                *_ctx = nullptr;
    std::unique_ptr<EmojiFrameTimer>                        _anim;
    std::vector<std::string>                                _images; // emoji box id i: [i - 1]
    std::vector<std::vector<std::unique_ptr<text::Layout>>> _cells;
    std::vector<float>                                      _colW, _rowH;
    float _idealW = 0, _tableW = 0, _tableH = 0, _docH = 0, _scroll = 0;
    bool  _built = false;
};

} // namespace

std::vector<std::vector<std::string>> readCsvFile(const std::string &path) {
    std::string text;
    if (!file::readAll(path, &text))
        return {};
    return parseCsv(text);
}

ui::Popup *
showTableViewer(ui::Window &w, Context &ctx, std::vector<std::vector<std::string>> cells) {
    auto  v   = std::make_unique<TableViewer>(std::move(cells), &ctx);
    auto *raw = v.get();
    w.showPopup(std::move(v));
    return raw;
}

ui::Popup *showTableViewer(ui::Window &w, std::vector<std::vector<std::string>> rows) {
    if (rows.empty())
        rows = {{tr("This file is empty")}};
    constexpr size_t kMaxRows = 400;
    if (rows.size() > kMaxRows) {
        const size_t n = rows.size();
        rows.resize(kMaxRows);
        rows.push_back({arg(tr("Showing the first %1 of %2 rows"), "400", std::to_string(n))});
    }
    auto  v   = std::make_unique<TableViewer>(std::move(rows), nullptr);
    auto *raw = v.get();
    w.showPopup(std::move(v));
    return raw;
}

} // namespace screens
