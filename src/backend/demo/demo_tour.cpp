// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "demo_tour.h"

#include "backend/backend.h"
#include "backend/demo/demo_backend.h"
#include "session/session.h"
#include "ui/app_dialog/app_dialog.h"
#include "ui/canvas_page/canvas_page.h"
#include "ui/composer/composer_widget.h"
#include "ui/context_menu/context_menu.h"
#include "ui/conv_list/conv_list_widget.h"
#include "ui/conv_tabs/conv_tabs_widget.h"
#include "ui/gif_picker/gif_picker_popup.h"
#include "ui/image_viewer/image_viewer.h"
#include "ui/main_window.h"
#include "ui/message_list/message_list.h"
#include "ui/message_list/message_render.h"
#include "ui/saved_page/saved_messages_page.h"
#include "ui/search/search_widget.h"
#include "ui/settings/settings_dialog.h"
#include "ui/styled_button/styled_button.h"
#include "ui/theme_manager.h"
#include "ui/thread_panel/thread_panel.h"
#include "ui/threads_page/threads_page.h"

#include <QAbstractScrollArea>
#include <QApplication>
#include <QCursor>
#include <QDateTime>
#include <QEnterEvent>
#include <QKeyEvent>
#include <QListWidget>
#include <QMouseEvent>
#include <QPushButton>
#include <QScreen>
#include <QStackedWidget>
#include <QToolButton>
#include <QScrollBar>
#include <QTimer>
#include <QVariantAnimation>
#include <QWidget>
#include <cstdio>

using namespace Qt::StringLiterals;

namespace demo {

// ── lifecycle ─────────────────────────────────────────────────────────────────

Tour::Tour(MainWindow *window, TourScript script, QObject *parent)
    : QObject(parent), _win(window), _script(std::move(script)) {}

void Tour::start() {
    // Pin the frame the recording is sized for. fitToScreen() only shrinks, and
    // the recording script sizes the virtual screen to exactly this.
    _win->resize(_script.window);
    _win->move(0, 0);

    auto *poll = new QTimer(this);
    connect(poll, &QTimer::timeout, this, [this, poll] {
        if (!_win->_session || _win->_currentConvId.value.isEmpty() || !_win->_composer ||
            !_win->_composer->isVisible())
            return;
        poll->stop();
        poll->deleteLater();
        _pos = _win->mapToGlobal(QPoint(_win->width() * 3 / 5, _win->height() * 11 / 20));
        QCursor::setPos(_win->screen(), _pos.toPoint());
        // Let the freshly mapped window settle (geometry restore, mask, first
        // layout) before the recording's first frame — the capture is trimmed
        // to this stamp, and a window still churning shows up as black frames.
        after(1500, [this] {
            printf("tour: start %lld\n", QDateTime::currentMSecsSinceEpoch());
            fflush(stdout);
            after(400, [this] { runNext(); });
        });
    });
    poll->start(100);
}

void Tour::runNext() {
    if (_done)
        return;
    if (_index >= _script.steps.size()) {
        finish();
        return;
    }
    const TourStep &step = _script.steps[_index++];
    fprintf(stderr, "tour: step %zu/%zu %s\n", _index, _script.steps.size(), qPrintable(step.arg));
    run(step, [this, kind = step.kind] {
        if (kind == TourStep::Kind::Quit || kind == TourStep::Kind::Wait)
            runNext();
        else
            after(_script.pauseMs, [this] { runNext(); });
    });
}

void Tour::finish() {
    if (_done)
        return;
    _done = true;
    printf("tour: end %lld\n", QDateTime::currentMSecsSinceEpoch());
    fflush(stdout);
    // Let the last frame sit for a moment before the window vanishes.
    QTimer::singleShot(600, qApp, [] { QCoreApplication::quit(); });
}

// ── steps ─────────────────────────────────────────────────────────────────────

void Tour::run(const TourStep &step, Done done) {
    using K = TourStep::Kind;
    switch (step.kind) {
    case K::Wait:
        after(step.ms, done);
        return;

    case K::Open: {
        const QPoint pt = conversationPoint(step.arg);
        if (pt.isNull()) {
            fprintf(stderr, "tour: conversation %s is not in the sidebar\n", qPrintable(step.arg));
            _win->_convList->selectConversation(ConversationId{step.arg});
            done();
            return;
        }
        click(pt, done);
        return;
    }

    case K::Scroll: {
        // Whatever page the content stack shows: the canvas body or an
        // overview page's scroll area instead of the message list.
        QAbstractScrollArea *area = _win->_messageList;
        if (QWidget *cur = _win->_contentStack ? _win->_contentStack->currentWidget() : nullptr;
            cur &&
            (cur == _win->_canvasPage || cur == _win->_threadsPage || cur == _win->_savedPage))
            if (auto *inner = cur->findChild<QAbstractScrollArea *>())
                area = inner;
        auto *bar = area->verticalScrollBar();
        parkCursor([this, bar, delta = int(step.num), done] {
            const int from = bar->value();
            const int to   = std::clamp(from + delta, bar->minimum(), bar->maximum());
            auto     *anim = new QVariantAnimation(this);
            anim->setStartValue(from);
            anim->setEndValue(to);
            anim->setDuration(std::clamp(std::abs(to - from) * 2, 400, 1600));
            anim->setEasingCurve(QEasingCurve::InOutCubic);
            connect(anim, &QVariantAnimation::valueChanged, this, [bar](const QVariant &v) {
                bar->setValue(v.toInt());
            });
            connect(anim, &QVariantAnimation::finished, this, [anim, done] {
                anim->deleteLater();
                done();
            });
            anim->start();
        });
        return;
    }

    case K::Hover:
    case K::MessageMenu:
    case K::Play:
    case K::OpenImage:
    case K::Thread:
    case K::React: {
        const auto ts = findMessageTs(step.arg);
        if (!ts) {
            fprintf(stderr, "tour: no message contains \"%s\"\n", qPrintable(step.arg));
            done();
            return;
        }
        if (!scrollIntoView(*ts, [this, step, done] { run(step, done); }))
            return;
        const ConversationId conv = _win->_currentConvId;
        if (step.kind == K::Hover) {
            moveCursor(messagePoint(*ts, 220, 34), done);
        } else if (step.kind == K::MessageMenu) {
            // The message menu lives behind the hover toolbar's "…" button.
            moveCursor(messagePoint(*ts, 220, 34), [this, ts = *ts, done] {
                after(250, [this, ts, done] { click(toolbarButtonPoint(ts, moreButton()), done); });
            });
        } else if (step.kind == K::Thread) {
            // The "N replies" footer sits at the bottom of the row.
            const QRect r = _win->_messageList->rowViewportRect(*ts);
            moveCursor(messagePoint(*ts, 150, r.height() - 14), [this, conv, ts = *ts, done] {
                _win->openThreadPanel(conv, ts);
                done();
            });
        } else if (step.kind == K::React) {
            moveCursor(messagePoint(*ts, 220, 34), [this, conv, ts = *ts, emoji = step.arg2, done] {
                after(350, [this, conv, ts, emoji, done] {
                    _win->_session->backend()->addReaction(conv, ts, emoji);
                    done();
                });
            });
        } else if (step.kind == K::OpenImage) {
            // The preview fills the lower part of the row (reactions under it).
            const QRect r = _win->_messageList->rowViewportRect(*ts);
            click(messagePoint(*ts, 160, r.height() - 90), done);
        } else { // Play: find the audio chip by probing down the row, hit its button
            MessageListWidget *list = _win->_messageList;
            const QRect        r    = list->rowViewportRect(*ts);
            QRect              chip;
            for (int y = r.top() + 20; y < r.bottom(); y += 6)
                if (list->fileChipAt(QPoint(r.left() + 80, y), &chip))
                    break;
            if (chip.isEmpty()) {
                fprintf(stderr, "tour: no audio chip on \"%s\"\n", qPrintable(step.arg));
                done();
                return;
            }
            const QRect btn = MsgRender::audioChipButtonRect(chip);
            click(list->viewport()->mapToGlobal(btn.center()), done);
        }
        return;
    }

    case K::CloseThread:
        if (_win->_threadPanel && _win->_threadPanel->isVisible()) {
            _win->_threadPanel->close();
            _win->_threadPanel->setVisible(false);
            _win->_messageList->setOpenThreadRoot({});
        }
        done();
        return;

    case K::Type: {
        // Into a dialog or popup field when one has focus; else click into the
        // thread composer (panel open) or the main one so the caret is real.
        ComposerWidget *composer = _win->_composer;
        if (_win->_threadPanel && _win->_threadPanel->isVisible())
            if (auto *c = _win->_threadPanel->findChild<ComposerWidget *>())
                composer = c;
        const bool fieldFocused = QApplication::activePopupWidget() ||
                                  AppDialog::topmostVisible(_win) ||
                                  (_win->_searchWidget && _win->_searchWidget->isVisible());
        auto       typeIt       = [this, text = step.arg, cps = step.num, done] {
            typeText(text, cps > 0 ? cps : 16, done);
        };
        if (fieldFocused || !composer) {
            typeIt();
            return;
        }
        click(centerOf(composer), [composer, typeIt, this] {
            composer->focusInput();
            after(150, typeIt);
        });
        return;
    }

    case K::Key: {
        static const QHash<QString, int> keys = {
            {u"Return"_s, Qt::Key_Return},
            {u"Tab"_s, Qt::Key_Tab},
            {u"Escape"_s, Qt::Key_Escape},
            {u"Down"_s, Qt::Key_Down},
            {u"Up"_s, Qt::Key_Up},
            {u"Backspace"_s, Qt::Key_Backspace},
        };
        const int key = keys.value(step.arg, Qt::Key_unknown);
        pressKey(key, key == Qt::Key_Return ? QStringLiteral("\r") : QString(), done);
        return;
    }

    case K::Send:
        pressKey(Qt::Key_Return, QStringLiteral("\r"), done);
        return;

    case K::Search:
        click(centerOf(_win->_searchBtn), [this, query = step.arg, done] {
            after(300, [this, query, done] {
                if (_win->_searchWidget)
                    _win->_searchWidget->focusInput();
                // Off the button (its tooltip would otherwise stay up) and onto
                // the results, where the eye goes next.
                const QPoint over = _win->_searchWidget ? centerOf(_win->_searchWidget)
                                                        : centerOf(_win->_messageList);
                moveCursor(over, [this, query, done] {
                    typeText(query, 18, [this, done] {
                        after(250, [this, done] {
                            pressKey(Qt::Key_Return, QStringLiteral("\r"), done);
                        });
                    });
                });
            });
        });
        return;

    case K::CloseSearch:
        if (_win->_searchWidget)
            _win->_searchWidget->closeSearch();
        done();
        return;

    case K::QuickSwitch:
        parkCursor([this, text = step.arg, done] {
            // The switcher runs a nested event loop (exec()) until a pick closes
            // it, so arm the typing first — its timer fires inside that loop —
            // and open the dialog from the event loop so this call returns.
            after(450, [this, text, done] {
                typeText(text, 14, [this, done] {
                    after(600, [this, done] {
                        pressKey(Qt::Key_Return, QStringLiteral("\r"), done);
                    });
                });
            });
            QTimer::singleShot(0, this, [this] { _win->openQuickSwitcher(); });
        });
        return;

    case K::Theme:
        parkCursor([this, dark = step.arg == u"dark"_s, done] {
            ThemeManager::instance().setMode(
                dark ? ThemeManager::ColorMode::Dark : ThemeManager::ColorMode::Light
            );
            done();
        });
        return;

    case K::Settings: {
        static const QStringList pages = {
            u"appearance"_s, u"notifications"_s, u"ai"_s, u"storage"_s, u"system"_s, u"about"_s
        };
        parkCursor([this, list = step.list, each = step.ms, done] {
            _win->_settingsDialog->openAt(SettingsDialog::Page::Appearance);
            auto *tabs = _win->_settingsDialog->findChild<QListWidget *>();
            auto  idx  = std::make_shared<int>(0);
            auto  step = std::make_shared<std::function<void()>>();
            *step      = [this, list, each, tabs, idx, step, done] {
                if (*idx >= list.size()) {
                    _win->_settingsDialog->hide();
                    done();
                    return;
                }
                const int page = pages.indexOf(list[(*idx)++]);
                auto      hold = [this, each, step] { after(each, *step); };
                if (!tabs || page < 0 || page >= tabs->count()) {
                    hold();
                    return;
                }
                const QRect r = tabs->visualItemRect(tabs->item(page));
                click(tabs->viewport()->mapToGlobal(r.center()), hold);
            };
            after(500, *step);
        });
        return;
    }

    case K::ChannelMenu: {
        const QPoint pt = conversationPoint(step.arg);
        if (pt.isNull()) {
            done();
            return;
        }
        click(pt, done, Qt::RightButton);
        return;
    }

    case K::MenuHover:
    case K::MenuPick: {
        const QPoint pt = menuItemPoint(step.arg);
        if (pt.isNull()) {
            fprintf(stderr, "tour: no open menu item \"%s\"\n", qPrintable(step.arg));
            done();
            return;
        }
        if (step.kind == K::MenuHover)
            moveCursor(pt, done);
        else
            click(pt, done);
        return;
    }

    case K::CloseMenu:
        if (auto *menu = visibleMenu())
            menu->close();
        done();
        return;

    case K::MoveToThread: {
        // Genuine path: hover → "…" → "Move to thread…" → filter → Move.
        const auto ts = findMessageTs(step.arg);
        if (!ts) {
            fprintf(stderr, "tour: no message contains \"%s\"\n", qPrintable(step.arg));
            done();
            return;
        }
        if (!scrollIntoView(*ts, [this, step, done] { run(step, done); }))
            return;
        auto pressMove = [this, done] {
            auto *dlg = AppDialog::topmostVisible(_win);
            if (dlg)
                for (auto *b : dlg->findChildren<StyledButton *>())
                    if (b->text() == QLatin1String("Move")) {
                        click(centerOf(b), done);
                        return;
                    }
            done();
        };
        auto pickItem = [this, target = step.arg2, done, pressMove] {
            const QPoint item = menuItemPoint(QStringLiteral("Move to thread"));
            if (item.isNull()) {
                done();
                return;
            }
            click(item, [this, target, pressMove] {
                after(700, [this, target, pressMove] {
                    typeText(target, 16, [this, pressMove] { after(700, pressMove); });
                });
            });
        };
        moveCursor(messagePoint(*ts, 220, 34), [this, ts = *ts, pickItem] {
            after(250, [this, ts, pickItem] {
                click(toolbarButtonPoint(ts, moreButton()), [this, pickItem] {
                    after(700, pickItem);
                });
            });
        });
        return;
    }

    case K::DialogButton: {
        auto *dlg = AppDialog::topmostVisible(_win);
        if (dlg)
            for (auto *b : dlg->findChildren<StyledButton *>())
                if (b->text().compare(step.arg, Qt::CaseInsensitive) == 0) {
                    click(centerOf(b), done);
                    return;
                }
        fprintf(stderr, "tour: no dialog button \"%s\"\n", qPrintable(step.arg));
        done();
        return;
    }

    case K::CloseDialog:
        if (auto *dlg = AppDialog::topmostVisible(_win))
            dlg->reject();
        done();
        return;

    case K::Gif: {
        ComposerWidget *composer = _win->_composer;
        click(centerOf(composer->_gifBtn), [this, composer, query = step.arg, done] {
            after(500, [this, composer, query, done] {
                typeText(query, 14, [this, composer, done] {
                    // Debounced query → stand-in server → previews decode.
                    after(1600, [this, composer, done] {
                        auto *picker = composer->_gifPicker;
                        auto *grid   = picker ? picker->findChild<GifGrid *>() : nullptr;
                        if (!grid) {
                            done();
                            return;
                        }
                        const QPoint cell = grid->viewport()->mapToGlobal(
                            QPoint(grid->viewport()->width() / 4, 70)
                        );
                        moveCursor(cell, [grid, this, done] {
                            after(300, [grid, done] {
                                grid->setSelected(0);
                                grid->activateSelected();
                                done();
                            });
                        });
                    });
                });
            });
        });
        return;
    }

    case K::CloseImage:
        if (auto *viewer = _win->_messageList->_imageViewer; viewer && viewer->isVisible())
            viewer->hide();
        done();
        return;

    case K::OpenThreads:
    case K::OpenSaved: {
        const bool   threads = step.kind == K::OpenThreads;
        const QPoint pt      = fixedRowPoint(int(threads ? RowKind::Threads : RowKind::SavedMsgs));
        if (pt.isNull()) {
            fprintf(stderr, "tour: no %s entry in the sidebar\n", threads ? "Threads" : "Saved");
            done();
            return;
        }
        click(pt, done);
        return;
    }

    case K::Canvas:
    case K::MessagesTab: {
        ConvTabsWidget *tabs = _win->_convTabs;
        const QRect     r    = tabs ? tabs->_tabs[step.kind == K::Canvas ? 1 : 0].rect : QRect();
        if (r.isEmpty() || !tabs->isVisible()) {
            fprintf(stderr, "tour: no %s tab\n", step.kind == K::Canvas ? "canvas" : "Messages");
            done();
            return;
        }
        click(tabs->mapToGlobal(r.center()), done);
        return;
    }

    case K::Post: {
        // Someone else posts now — the fixture backend fires it like an incoming
        // event, so badges, the Threads entry and notifications all react.
        auto *backend = dynamic_cast<DemoBackend *>(_win->_session->backend());
        if (!backend) {
            done();
            return;
        }
        const ConversationId conv{step.conv};
        std::optional<Ts>    root;
        if (!step.arg2.isEmpty()) {
            root = backend->findTs(conv, step.arg2);
            if (!root) {
                fprintf(
                    stderr,
                    "tour: no message in %s contains \"%s\"\n",
                    qPrintable(step.conv),
                    qPrintable(step.arg2)
                );
                done();
                return;
            }
        }
        backend->postAs(conv, UserId{step.user}, step.arg, root);
        done();
        return;
    }

    case K::Quit:
        finish();
        return;
    }
    done();
}

// ── primitives ────────────────────────────────────────────────────────────────

void Tour::after(int ms, Done done) {
    QTimer::singleShot(std::max(0, ms), this, [done] { done(); });
}

void Tour::parkCursor(Done done) {
    const QWidget *vp = _win->_messageList->viewport();
    moveCursor(vp->mapToGlobal(QPoint(vp->width() * 2 / 3, vp->height() * 3 / 5)), std::move(done));
}

void Tour::moveCursor(QPoint global, Done done) {
    const QPointF from = _pos;
    const QPointF to(global);
    const qreal   dist = QLineF(from, to).length();
    auto         *anim = new QVariantAnimation(this);
    anim->setStartValue(from);
    anim->setEndValue(to);
    anim->setDuration(int(std::clamp(180.0 + dist * 0.9, 220.0, 900.0)));
    anim->setEasingCurve(QEasingCurve::InOutQuad);
    connect(anim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
        _pos = v.toPointF();
        QCursor::setPos(_win->screen(), _pos.toPoint());
        dispatchHover(_pos.toPoint());
    });
    connect(anim, &QVariantAnimation::finished, this, [this, anim, global, done] {
        anim->deleteLater();
        _pos = global;
        QCursor::setPos(_win->screen(), global);
        dispatchHover(global);
        done();
    });
    anim->start();
}

void Tour::click(QPoint global, Done done, Qt::MouseButton button) {
    moveCursor(global, [this, global, button, done] {
        after(90, [this, global, button, done] {
            dispatchMouse(global, true, button);
            after(70, [this, global, button, done] {
                dispatchMouse(global, false, button);
                done();
            });
        });
    });
}

void Tour::typeText(const QString &text, double cps, Done done) {
    auto chars = std::make_shared<QString>(text);
    auto idx   = std::make_shared<int>(0);
    auto step  = std::make_shared<std::function<void()>>();
    *step      = [this, chars, idx, step, cps, done] {
        if (*idx >= chars->size()) {
            done();
            return;
        }
        const QChar ch = chars->at((*idx)++);
        QWidget    *w  = QApplication::focusWidget();
        if (!w)
            fprintf(stderr, "tour: typing with no focus widget\n");
        if (w) {
            int key = Qt::Key_unknown;
            if (ch == QLatin1Char('\n'))
                key = Qt::Key_Return;
            else if (ch.unicode() < 128)
                key = QKeySequence(QString(ch.toUpper()))[0].key();
            const Qt::KeyboardModifiers mods = ch.isUpper() ? Qt::ShiftModifier : Qt::NoModifier;
            QKeyEvent                   press(QEvent::KeyPress, key, mods, QString(ch));
            QKeyEvent                   release(QEvent::KeyRelease, key, mods, QString(ch));
            QApplication::sendEvent(w, &press);
            QApplication::sendEvent(w, &release);
        }
        // Human-ish rhythm: a little jitter, a longer beat after punctuation.
        int       delay  = int(1000.0 / cps);
        const int jitter = int(std::hash<int>{}(*idx) % 7) - 3; // -3…+3 tenths
        delay += jitter * delay / 10;
        if (ch == QLatin1Char(',') || ch == QLatin1Char('.'))
            delay += delay * 2;
        if (ch == QLatin1Char(' '))
            delay += delay / 3;
        QTimer::singleShot(delay, this, *step);
    };
    (*step)();
}

void Tour::pressKey(int key, const QString &text, Done done) {
    if (QWidget *w = QApplication::focusWidget()) {
        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, text);
        QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier, text);
        QApplication::sendEvent(w, &press);
        QApplication::sendEvent(w, &release);
    }
    after(60, done);
}

QWidget *Tour::widgetAt(QPoint global) const {
    // Across windows: context menus and the GIF picker are Qt::Popup top-levels,
    // dialogs and the image viewer are in-window overlays.
    if (QWidget *w = QApplication::widgetAt(global))
        return w;
    return _win;
}

void Tour::dispatchHover(QPoint global) {
    QWidget *w = widgetAt(global);
    if (w != _hovered) {
        if (_hovered) {
            QEvent leave(QEvent::Leave);
            QApplication::sendEvent(_hovered, &leave);
        }
        const QPointF local = w->mapFromGlobal(global);
        QEnterEvent   enter(local, local, global);
        QApplication::sendEvent(w, &enter);
        _hovered = w;
    }
    const QPointF local = w->mapFromGlobal(global);
    QMouseEvent   move(
        QEvent::MouseMove, local, local, global, Qt::NoButton, Qt::NoButton, Qt::NoModifier
    );
    QApplication::sendEvent(w, &move);
}

void Tour::dispatchMouse(QPoint global, bool press, Qt::MouseButton button) {
    QWidget      *w     = widgetAt(global);
    const QPointF local = w->mapFromGlobal(global);
    QMouseEvent   ev(
        press ? QEvent::MouseButtonPress : QEvent::MouseButtonRelease,
        local,
        local,
        global,
        button,
        press ? button : Qt::NoButton,
        Qt::NoModifier
    );
    QApplication::sendEvent(w, &ev);
}

ContextMenu *Tour::visibleMenu() const {
    for (QWidget *w : QApplication::topLevelWidgets())
        if (auto *m = qobject_cast<ContextMenu *>(w); m && m->isVisible())
            return m;
    return nullptr;
}

QPoint Tour::menuItemPoint(const QString &label) const {
    ContextMenu *menu = visibleMenu();
    if (!menu)
        return {};
    auto norm = [](QString s) { return s.remove(QChar(0x2026)).remove('.').trimmed().toLower(); };
    const QString want = norm(label);
    for (int i = 0; i < int(menu->_items.size()); ++i) {
        const auto &it = menu->_items[i];
        if (it.separator || it.header)
            continue;
        if (norm(it.text).startsWith(want))
            return menu->mapToGlobal(menu->itemRect(i).center());
    }
    return {};
}

std::optional<QString> Tour::findMessageTs(const QString &fragment) const {
    if (!_win->_session)
        return std::nullopt;
    // The live list first (it has what the tour just sent), then the cache.
    // Text, then file names and attachment titles — a voice clip or an image
    // post has no text of its own.
    std::vector<Message> pool;
    for (const auto &item : _win->_messageList->_items)
        pool.push_back(item.msg);
    for (auto &m : _win->_session->cachedMessages(_win->_currentConvId))
        pool.push_back(std::move(m));
    for (const auto &m : pool) {
        if (m.text.text.contains(fragment, Qt::CaseInsensitive))
            return m.ts;
        for (const auto &f : m.files)
            if (f.name.contains(fragment, Qt::CaseInsensitive))
                return m.ts;
        for (const auto &a : m.attachments)
            if (a.title.contains(fragment, Qt::CaseInsensitive))
                return m.ts;
    }
    return std::nullopt;
}

bool Tour::scrollIntoView(const QString &ts, Done retry) {
    // One jump per step: a row taller than the viewport never fits entirely,
    // and the re-run must not jump again (the tour would spin forever).
    if (std::exchange(_jumped, false))
        return true;
    MessageListWidget *list = _win->_messageList;
    const QRect        r    = list->rowViewportRect(ts);
    const QRect        vp   = list->viewport()->rect();
    if (r.isEmpty() || (r.top() >= 0 && r.bottom() <= vp.bottom()))
        return true;
    // jumpToTs scrolls smoothly: row rectangles are only right once the
    // animation has settled, so the step re-runs after it (visible by then).
    list->jumpToTs(ts);
    _jumped = true;
    after(700, std::move(retry));
    return false;
}

QPoint Tour::messagePoint(const QString &ts, int dxFromLeft, int dyFromTop) const {
    MessageListWidget *list = _win->_messageList;
    QRect              r    = list->rowViewportRect(ts);
    if (r.isEmpty())
        return centerOf(list);
    const QRect vp = list->viewport()->rect();
    if (r.top() < 0 || r.bottom() > vp.bottom()) {
        list->jumpToTs(ts);
        r = list->rowViewportRect(ts);
    }
    return list->viewport()->mapToGlobal(
        QPoint(r.left() + dxFromLeft, r.top() + std::clamp(dyFromTop, 4, r.height() - 4))
    );
}

int Tour::moreButton() const {
    // The "…" button is always the toolbar's last one (Emoji, Forward, [Save], More).
    return _win->_messageList->toolbarButtonCount() - 1;
}

QPoint Tour::toolbarButtonPoint(const QString &ts, int btn) const {
    MessageListWidget *list = _win->_messageList;
    const int          row  = list->findByTs(ts);
    if (row < 0 || row >= int(list->_tops.size()))
        return centerOf(list);
    const int   rowTop = list->_tops[row] - list->verticalScrollBar()->value();
    const int   rh     = list->rowHeight(row);
    const int   sep    = list->needsDateSep(row) ? MessageListWidget::kSepH : 0;
    const QRect r      = list->toolbarButtonRect(btn, rowTop + sep, rh - sep);
    return list->viewport()->mapToGlobal(r.center());
}

QPoint Tour::conversationPoint(const QString &convId) const {
    ConvListWidget *list = _win->_convList;
    const int       row  = list->rowForId(ConversationId{convId});
    if (row < 0)
        return {};
    const QRect r = list->rowViewportRect(row);
    if (r.isEmpty())
        return {};
    return list->viewport()->mapToGlobal(QPoint(r.left() + r.width() / 3, r.center().y()));
}

QPoint Tour::fixedRowPoint(int rowKind) const {
    ConvListWidget *list = _win->_convList;
    for (int r = 0; r < int(list->_rows.size()); ++r) {
        if (int(list->_rows[r].kind) != rowKind)
            continue;
        const QRect rect = list->rowViewportRect(r);
        if (rect.isEmpty())
            return {};
        return list->viewport()->mapToGlobal(
            QPoint(rect.left() + rect.width() / 3, rect.center().y())
        );
    }
    return {};
}

QPoint Tour::centerOf(const QWidget *w) const {
    if (!w)
        return _win->mapToGlobal(_win->rect().center());
    return w->mapToGlobal(w->rect().center());
}

} // namespace demo
