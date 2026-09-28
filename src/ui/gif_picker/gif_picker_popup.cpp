// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "gif_picker_popup.h"
#include "ui/image_cache.h"
#include "ui/popup_placement.h"
#include "ui/styled_button/styled_button.h"
#include "ui/styled_line_edit/styled_line_edit.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"

#include <QDesktopServices>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QMovie>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollBar>
#include <QStackedLayout>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace {

// Popup chrome — same footprint as the emoji picker so the two feel related.
constexpr int kFullWidth  = 354;
constexpr int kFullHeight = 460;

} // namespace

// ── GifGrid ──────────────────────────────────────────────────────────────────

GifGrid::GifGrid(QWidget *parent) : VirtualListWidget(parent) {
    verticalScrollBar()->setSingleStep(48);
}

GifGrid::~GifGrid() {
    releaseAllMovies();
}

void GifGrid::setImageCache(ImageCache *cache) {
    if (_imgCache == cache)
        return;
    releaseAllMovies();
    if (_imgCache)
        disconnect(_imgCache, nullptr, this, nullptr);
    _imgCache = cache;
    if (_imgCache) {
        // The panel holds at most a few dozen cells, so repainting the whole
        // viewport when any preview lands is cheaper than mapping url → rect.
        connect(_imgCache, &ImageCache::loaded, this, [this](const QString &) {
            viewport()->update();
        });
    }
}

void GifGrid::clear() {
    releaseAllMovies();
    _items.clear();
    _contentH = 0;
    _sel      = -1;
    _hover    = -1;
    updateScrollRange();
    viewport()->update();
}

void GifGrid::setResults(const QList<net::GifResult> &results) {
    // The outgoing set's movies are handed back before the new one is laid out;
    // otherwise every swap would leak an acquisition and pin the cache entry.
    releaseAllMovies();
    _items.clear();
    _items.reserve(results.size());
    for (const net::GifResult &g : results)
        _items.append({g, QRect(), 0});
    _sel   = -1;
    _hover = -1;
    relayout();
    verticalScrollBar()->setValue(0);
    viewport()->update();
}

void GifGrid::relayout() {
    const int avail = viewport()->width() - kMargin * 2 - kGap * (kCols - 1);
    const int colW  = std::max(40, avail / kCols);

    int colY[kCols];
    for (int &y : colY)
        y = kMargin;

    for (Item &item : _items) {
        // Shortest column wins, which is what keeps the two sides level.
        int col = 0;
        for (int c = 1; c < kCols; ++c)
            if (colY[c] < colY[col])
                col = c;

        const QSize src = item.gif.previewSize;
        int         h   = colW; // square fallback until dimensions are known
        if (src.isValid() && src.width() > 0)
            h = qRound(static_cast<qreal>(colW) * src.height() / src.width());
        h = std::clamp(h, kMinH, kMaxH);

        item.col  = col;
        item.rect = QRect(kMargin + col * (colW + kGap), colY[col], colW, h);
        colY[col] += h + kGap;
    }

    _contentH = *std::max_element(colY, colY + kCols) - kGap + kMargin;
    if (_items.isEmpty())
        _contentH = 0;
    updateScrollRange();
}

void GifGrid::updateScrollRange() {
    auto     *sb = verticalScrollBar();
    const int vh = viewport()->height();
    sb->setRange(0, std::max(0, _contentH - vh));
    sb->setPageStep(vh);
}

int GifGrid::itemAt(const QPoint &vp) const {
    const QPoint content(vp.x(), vp.y() + verticalScrollBar()->value());
    for (int i = 0; i < _items.size(); ++i)
        if (_items[i].rect.contains(content))
            return i;
    return -1;
}

void GifGrid::ensureVisible(int idx) {
    if (idx < 0 || idx >= _items.size())
        return;
    auto        *sb = verticalScrollBar();
    const int    vh = viewport()->height();
    const QRect &r  = _items[idx].rect;
    if (r.top() < sb->value())
        sb->setValue(std::max(0, r.top() - kMargin));
    else if (r.bottom() > sb->value() + vh)
        sb->setValue(std::min(sb->maximum(), r.bottom() - vh + kMargin));
}

void GifGrid::setSelected(int idx) {
    if (idx < 0 || idx >= _items.size()) {
        if (_sel != -1) {
            _sel = -1;
            viewport()->update();
        }
        return;
    }
    _sel = idx;
    ensureVisible(_sel);
    viewport()->update();
}

void GifGrid::moveSelection(int dCol, int dRow) {
    if (_items.isEmpty())
        return;
    if (_sel < 0) {
        setSelected(0);
        return;
    }
    if (dCol != 0) {
        setSelected(std::clamp(_sel + dCol, 0, static_cast<int>(_items.size()) - 1));
        return;
    }
    // Up/down: nearest item in the same column, by vertical position. A masonry
    // has no rows to index, so "the next one down in this column" is the only
    // meaning that matches what the user sees.
    const Item &cur  = _items[_sel];
    int         best = -1;
    for (int i = 0; i < _items.size(); ++i) {
        if (i == _sel || _items[i].col != cur.col)
            continue;
        const bool below = _items[i].rect.top() > cur.rect.top();
        if (below != (dRow > 0))
            continue;
        if (best < 0 || std::abs(_items[i].rect.top() - cur.rect.top()) <
                            std::abs(_items[best].rect.top() - cur.rect.top()))
            best = i;
    }
    if (best >= 0)
        setSelected(best);
}

void GifGrid::activateSelected() {
    if (_sel >= 0 && _sel < _items.size())
        emit gifActivated(_items[_sel].gif);
}

void GifGrid::doPaint(QPaintEvent *) {
    QPainter p(viewport());
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.fillRect(viewport()->rect(), Th::c().surface.raised);
    if (_items.isEmpty())
        return;

    const int     scrollY = verticalScrollBar()->value();
    const int     vh      = viewport()->height();
    QSet<QString> visible;

    for (int i = 0; i < _items.size(); ++i) {
        const Item &item = _items[i];
        QRect       r    = item.rect.translated(0, -scrollY);
        // One cell of slack either way, so a preview has started decoding by the
        // time it scrolls in rather than popping in as a grey box.
        if (r.bottom() < -kMaxH || r.top() > vh + kMaxH)
            continue;
        const bool onScreen = r.bottom() >= 0 && r.top() <= vh;
        if (onScreen)
            visible.insert(item.gif.previewUrl);

        QPainterPath clip;
        clip.addRoundedRect(r, kRadius, kRadius);

        // Placeholder under every cell: it shows through while the preview is
        // still downloading and behind anything with transparency.
        p.fillPath(clip, Th::c().surface.sunken);

        QPixmap frame;
        if (const auto it = _movies.constFind(item.gif.previewUrl); it != _movies.constEnd())
            frame = it.value()->currentPixmap();
        if (frame.isNull() && _imgCache)
            frame = _imgCache->get(item.gif.previewUrl);

        if (!frame.isNull()) {
            p.save();
            p.setClipPath(clip);
            // Cover-fit: GIF aspect ratios rarely match the clamped cell height
            // exactly, and cropping the overflow looks better than letterboxing.
            const QSize scaled = frame.size().scaled(r.size(), Qt::KeepAspectRatioByExpanding);
            QRect       target(QPoint(), scaled);
            target.moveCenter(r.center());
            p.drawPixmap(target, frame);
            p.restore();
        }

        if (i == _sel || i == _hover) {
            p.setPen(QPen(Th::c().accent.def, 2));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(r.adjusted(1, 1, -1, -1), kRadius, kRadius);
        }
    }

    paintScrollThumb(p, _contentH, Th::c().divider.strong);
    syncPlayback(visible);
}

void GifGrid::syncPlayback(const QSet<QString> &visibleUrls) {
    if (!_imgCache)
        return;

    // Start (or acquire) what is on screen.
    for (const QString &url : visibleUrls) {
        auto it = _movies.find(url);
        if (it == _movies.end()) {
            QMovie *m = _imgCache->movie(url);
            if (!m)
                continue; // not downloaded yet, or a single-frame image
            it = _movies.insert(url, m);
            connect(m, &QMovie::frameChanged, this, [this](int) { viewport()->update(); });
        }
        if (it.value()->state() != QMovie::Running)
            it.value()->setPaused(false);
        if (it.value()->state() == QMovie::NotRunning)
            it.value()->start();
    }

    // Pause and release what scrolled away. The acquisition has to go back to
    // the cache or the entry stays pinned against eviction for good.
    for (auto it = _movies.begin(); it != _movies.end();) {
        if (visibleUrls.contains(it.key())) {
            ++it;
            continue;
        }
        QMovie *m = it.value();
        disconnect(m, nullptr, this, nullptr);
        const QString url = it.key();
        it                = _movies.erase(it);
        _imgCache->releaseMovie(url);
    }
}

void GifGrid::releaseAllMovies() {
    if (_movies.isEmpty())
        return;
    const QStringList urls = _movies.keys();
    for (const QString &url : urls) {
        if (QMovie *m = _movies.value(url))
            disconnect(m, nullptr, this, nullptr);
        if (_imgCache)
            _imgCache->releaseMovie(url);
    }
    _movies.clear();
}

void GifGrid::doMousePress(QMouseEvent *event) {
    const QPoint pos = event->pos();

    const int sbHitX = scrollThumbHitX();
    if (pos.x() >= sbHitX && isOnScrollThumb(pos.y(), _contentH)) {
        _sbDragging        = true;
        _sbDragStartY      = pos.y();
        _sbDragStartScroll = verticalScrollBar()->value();
        return;
    }

    const int idx = itemAt(pos);
    if (idx >= 0) {
        _sel = idx;
        activateSelected();
    }
}

void GifGrid::doMouseMove(QMouseEvent *event) {
    const QPoint pos = event->pos();

    if (_sbDragging) {
        const int vh     = viewport()->height();
        const int thumbH = std::max(20, _contentH > 0 ? vh * vh / _contentH : vh);
        const int denom  = vh - thumbH;
        if (denom > 0) {
            const int newScroll =
                _sbDragStartScroll + (pos.y() - _sbDragStartY) * (_contentH - vh) / denom;
            verticalScrollBar()->setValue(std::clamp(newScroll, 0, verticalScrollBar()->maximum()));
        }
        return;
    }

    const int idx = itemAt(pos);
    if (idx != _hover) {
        _hover = idx;
        setCursor(idx >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        viewport()->update();
    }
}

void GifGrid::doMouseRelease(QMouseEvent *) {
    _sbDragging = false;
}

void GifGrid::doMouseLeave() {
    if (_hover != -1) {
        _hover = -1;
        unsetCursor();
        viewport()->update();
    }
}

void GifGrid::scrollContentsBy(int, int) {
    _hover = -1;
    viewport()->update();
}

void GifGrid::resizeEvent(QResizeEvent *event) {
    VirtualListWidget::resizeEvent(event);
    relayout(); // column width changed, so every cell's geometry did too
    viewport()->update();
}

void GifGrid::hideEvent(QHideEvent *event) {
    // Nothing is visible once the panel closes; keeping decoders running behind
    // it would animate GIFs nobody can see until the popup is reopened.
    releaseAllMovies();
    VirtualListWidget::hideEvent(event);
}

// ── GifPickerPopup ───────────────────────────────────────────────────────────

GifPickerPopup::GifPickerPopup(QWidget *parent)
    : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint) {
    setObjectName("gifPicker");
    setFixedSize(kFullWidth, kFullHeight);

    auto       *lay = new QVBoxLayout(this);
    const auto &sp  = Th::c().spacing;
    lay->setContentsMargins(sp.md, sp.md, sp.md, sp.md);
    lay->setSpacing(sp.md);

    _search = new StyledLineEdit(this);
    _search->setPlaceholderText(tr("Search GIFs"));
    _search->setLeadingIcon(QStringLiteral(":/ui/search.svg"));
    lay->addWidget(_search);

    // The grid and the message share the body area: exactly one is ever shown,
    // and stacking them keeps the panel from resizing as states change.
    _body = new QStackedLayout;
    _body->setStackingMode(QStackedLayout::StackOne);
    _grid = new GifGrid(this);
    _body->addWidget(_grid);
    _message = new QLabel(this);
    _message->setAlignment(Qt::AlignCenter);
    _message->setWordWrap(true);
    _body->addWidget(_message);
    _setup = buildSetupPage();
    _body->addWidget(_setup);
    lay->addLayout(_body, 1);

    // GIPHY's API terms require the attribution wherever the API is used, so it
    // is part of the chrome rather than something a state can hide.
    _attribution = new QLabel(net::GifSearch::attributionText(), this);
    _attribution->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    lay->addWidget(_attribution);

    _api = new net::GifSearch(this);
    _debounce.setSingleShot(true);
    _debounce.setInterval(kDebounceMs);

    connect(&_debounce, &QTimer::timeout, this, &GifPickerPopup::requery);
    connect(_search, &StyledLineEdit::textChanged, this, [this](const QString &) {
        scheduleQuery();
    });
    connect(_search, &StyledLineEdit::returnPressed, this, [this] {
        // Enter with a selection sends it; otherwise commit the query now
        // instead of waiting out the debounce.
        if (_state == State::Results && _grid->selected() >= 0)
            _grid->activateSelected();
        else if (_debounce.isActive()) {
            _debounce.stop();
            requery();
        }
    });

    connect(_grid, &GifGrid::gifActivated, this, [this](const net::GifResult &gif) {
        hide();
        emit gifSelected(gif.postUrl, gif.description);
    });

    connect(
        _api,
        &net::GifSearch::results,
        this,
        [this](const QString &query, const QList<net::GifResult> &gifs) {
            if (query != _pending) // a newer query is already in flight
                return;
            if (gifs.isEmpty()) {
                _grid->clear();
                setState(State::Empty, tr("No GIFs found."));
                return;
            }
            _grid->setResults(gifs);
            setState(State::Results);
        }
    );
    connect(
        _api,
        &net::GifSearch::failed,
        this,
        [this](const QString &query, const QString &error, bool keyRejected) {
            if (query != _pending)
                return;
            _grid->clear();
            if (keyRejected) {
                // Back to the setup form with the reason attached — a key GIPHY
                // refused is fixed by pasting another one, not by retrying. The
                // error goes up first so the compact height accounts for it.
                _setupError->setText(error);
                _setupError->show();
                setState(State::NeedsKey);
                return;
            }
            setState(State::Error, error);
        }
    );

    _search->lineEdit()->installEventFilter(this);

    applyTheme();
    connect(
        &ThemeManager::instance(), &ThemeManager::themeChanged, this, &GifPickerPopup::applyTheme
    );
}

QWidget *GifPickerPopup::buildSetupPage() {
    auto       *page = new QWidget(this);
    auto       *lay  = new QVBoxLayout(page);
    const auto &sp   = Th::c().spacing;
    // Insets on top of the popup's own margins, tuned so the VISIBLE gap is the
    // same on all four sides. The first line's font leading already adds a few
    // pixels above its glyphs, which is why the top inset is one step smaller
    // than the bottom one under the Save row.
    lay->setContentsMargins(sp.lg, sp.md, sp.lg, sp.lg);
    lay->setSpacing(sp.md);

    _setupText = new QLabel(
        tr("Searching GIFs needs a GIPHY API key.\n\n"
           "Create a free one — it takes a minute — then paste it below. "
           "You can change it later in Settings → System."),
        page
    );
    _setupText->setWordWrap(true);
    lay->addWidget(_setupText);

    auto *link = new StyledButton(tr("Get a free GIPHY key…"), StyledButton::Variant::Link, page);
    connect(link, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl(net::GifSearch::apiKeyUrl()));
    });
    auto *linkRow = new QHBoxLayout;
    linkRow->addWidget(link);
    linkRow->addStretch();
    lay->addLayout(linkRow);

    _keyEdit = new StyledLineEdit(page);
    _keyEdit->setSize(StyledLineEdit::Size::Small);
    _keyEdit->setPlaceholderText(tr("Paste your GIPHY API key"));
    _keyEdit->enablePasswordReveal();
    connect(_keyEdit, &StyledLineEdit::returnPressed, this, &GifPickerPopup::saveKeyFromSetup);
    lay->addWidget(_keyEdit);

    _setupError = new QLabel(page);
    _setupError->setWordWrap(true);
    _setupError->hide();
    lay->addWidget(_setupError);

    auto *saveRow = new QHBoxLayout;
    auto *saveBtn = new StyledButton(tr("Save"), StyledButton::Variant::Primary, page);
    saveBtn->setSize(StyledButton::Size::Small);
    connect(saveBtn, &QPushButton::clicked, this, &GifPickerPopup::saveKeyFromSetup);
    saveRow->addWidget(saveBtn);
    saveRow->addStretch();
    // The attribution shares the Save row here: the panel is sized to this
    // page, and a footer line of its own left a gap under the button twice the
    // one above the text. GIPHY's terms want the mark visible, not on its own.
    _setupAttribution = new QLabel(net::GifSearch::attributionText(), page);
    _setupAttribution->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    // Centred, the small mark sits on the Save label's baseline and reads as
    // part of the button row; it is set 8px lower (a centred label moves by
    // half its top margin) so it reads as the footer note it is.
    _setupAttribution->setContentsMargins(0, 2 * sp.md, 0, 0);
    saveRow->addWidget(_setupAttribution);
    lay->addLayout(saveRow);
    return page;
}

void GifPickerPopup::saveKeyFromSetup() {
    const QString key = _keyEdit->text().trimmed();
    if (key.isEmpty()) {
        _setupError->setText(tr("Paste a key first."));
        _setupError->show();
        fitHeight(); // the form just gained a line
        return;
    }
    _setupError->hide();
    net::GifSearch::setUserApiKey(key);
    // Anything cached was fetched under the old key (or is the no-key failure);
    // drop it so the very next search actually exercises the new one.
    _api->clearCache();
    _keyEdit->clear();
    requery(); // straight into trending, so the key visibly works
    _search->lineEdit()->setFocus();
}

void GifPickerPopup::applyTheme() {
    setStyleSheet(
        u"QFrame#gifPicker {"
        "  background: %1;"
        "  border: 1px solid %2;"
        "  border-radius: 8px;"
        "}"_s.arg(Th::qss(Th::c().surface.raised), Th::qss(Th::c().divider.strong))
    );
    const QString bodyText = u"QLabel { color: %1; font-size: %2px; background: transparent; }"_s
                                 .arg(Th::qss(Th::c().text.secondary))
                                 .arg(Th::c().fonts.md);
    _message->setStyleSheet(bodyText);
    _setupText->setStyleSheet(bodyText);
    _setupError->setStyleSheet(u"QLabel { color: %1; font-size: %2px; background: transparent; }"_s
                                   .arg(Th::qss(Th::c().text.danger))
                                   .arg(Th::c().fonts.sm));
    const QString attribution = u"QLabel { color: %1; font-size: %2px; background: transparent; }"_s
                                    .arg(Th::qss(Th::c().text.tertiary))
                                    .arg(Th::c().fonts.xs);
    _attribution->setStyleSheet(attribution);
    _setupAttribution->setStyleSheet(attribution);
}

void GifPickerPopup::setImageCache(ImageCache *cache) {
    _grid->setImageCache(cache);
}

void GifPickerPopup::setState(State state, const QString &message) {
    _state = state;
    _message->setText(message);

    // Exactly one of the three body pages is ever up. QStackedLayout owns page
    // visibility in StackOne mode, so this must go through it — calling
    // setVisible() on the pages directly just fights the layout.
    QWidget *const grid = _grid;
    QWidget *const msg  = _message;
    _body->setCurrentWidget(
        state == State::Results    ? grid
        : state == State::NeedsKey ? _setup
                                   : msg
    );

    // During setup the form is the whole panel: a search box that cannot search
    // is noise, and it would compete with the key field for focus.
    _search->setVisible(state != State::NeedsKey);
    // The setup page carries the attribution in its Save row instead.
    _attribution->setVisible(state != State::NeedsKey);
    if (state == State::NeedsKey)
        _keyEdit->lineEdit()->setFocus();
    fitHeight();
}

int GifPickerPopup::compactHeight() const {
    const QMargins m      = layout()->contentsMargins();
    const int      innerW = kFullWidth - m.left() - m.right() - 2 * frameWidth();
    // The setup text wraps, so its height is a function of the width it gets.
    const int      setupH = _setup->layout()->heightForWidth(innerW);
    return m.top() + setupH + m.bottom() + 2 * frameWidth();
}

void GifPickerPopup::fitHeight() {
    // The results grid wants the whole panel; the setup form is a few lines of
    // text and a field, and at grid height it sat in a sea of empty space.
    const int h = _state == State::NeedsKey ? compactHeight() : kFullHeight;
    if (h == height())
        return;
    // A Qt::Popup does not honour move() once shown on Wayland, so a visible
    // panel (the key was just saved, and the grid needs the room back) is
    // re-shown at its new size rather than resized in place. Either way the
    // bottom edge stays where the caller anchored it, just above the button.
    const bool reshow = isVisible();
    if (reshow)
        hide();
    setFixedHeight(h);
    place();
    if (reshow) {
        show();
        raise();
    }
}

void GifPickerPopup::place() {
    QPoint pos(_anchorX, _anchorBottom - height());
    if (QScreen *scr = QGuiApplication::screenAt(QPoint(_anchorX, _anchorBottom - 1)))
        pos = Ui::clampInto(size(), pos, scr->availableGeometry());
    move(pos);
}

void GifPickerPopup::scheduleQuery() {
    // Claim the new query NOW, not when the debounce fires. The reply to the
    // PREVIOUS query can land inside the debounce window, and while _pending
    // still named it the guard let it through — results for a query the user
    // had already edited flashed up, then reverted to "Searching…".
    _pending = _search->text().trimmed();
    // Show progress straight away; the request itself waits out the debounce.
    if (_state != State::Loading)
        setState(State::Loading, tr("Searching…"));
    _debounce.start();
}

void GifPickerPopup::requery() {
    if (!net::GifSearch::configured()) {
        _setupError->hide(); // a previous key's rejection is no longer the reason
        setState(State::NeedsKey);
        return;
    }
    _pending = _search->text().trimmed();
    setState(State::Loading, tr("Searching…"));
    _api->search(_pending);
}

void GifPickerPopup::open(const QPoint &globalPos) {
    _search->clear();
    _debounce.stop();
    _grid->clear();

    // The caller placed the panel's top-left by its CURRENT height (whatever
    // state it was last in), so the point it actually chose is the bottom edge:
    // remember that and let the state decide how tall the panel is above it.
    _anchorX      = globalPos.x();
    _anchorBottom = globalPos.y() + height();

    requery(); // trending, or the setup form — sets the state and the height
    place();
    show();
    raise();
    // The setup form has already claimed focus for its key field.
    if (_state != State::NeedsKey)
        _search->lineEdit()->setFocus();
}

bool GifPickerPopup::eventFilter(QObject *obj, QEvent *event) {
    if (obj == _search->lineEdit() && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        switch (ke->key()) {
        case Qt::Key_Escape:
            hide();
            return true;
        case Qt::Key_Down:
            _grid->moveSelection(0, 1);
            return true;
        case Qt::Key_Up:
            _grid->moveSelection(0, -1);
            return true;
        // Left/Right only steer the grid once a GIF is selected; before that
        // they have to keep working as ordinary cursor keys in the search box.
        case Qt::Key_Left:
            if (_grid->selected() < 0)
                break;
            _grid->moveSelection(-1, 0);
            return true;
        case Qt::Key_Right:
            if (_grid->selected() < 0)
                break;
            _grid->moveSelection(1, 0);
            return true;
        default:
            break;
        }
    }
    return QFrame::eventFilter(obj, event);
}
