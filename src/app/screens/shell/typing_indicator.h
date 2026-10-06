// The typing indicator, between the message list and the composer:
// "<b>Mira</b> is typing…", "<b>Mira</b>, <b>Jonas</b> are typing…", or
// "<b>You</b> are typing on another device…" — hidden (no height) while
// nobody types. Follows Store typing for one conversation (and thread).
// An agent at work (Typing::sinceMs) is "<b>Claude</b> is thinking (1m 5s)…"
// with the clock ticking every second ("… are thinking…" for several).
#pragma once

#include "screens/common/context.h"
#include "ui/ui.h"

namespace shell {

class TypingIndicator : public ui::View {
public:
    explicit TypingIndicator(screens::Context &ctx);
    ~TypingIndicator() override;

    void               setTarget(model::ConvRef conv, model::Ts thread);
    const std::string &text() const { return _label->text(); }

private:
    void refresh();
    // Elapsed time: "42s", "1m 5s", "2h 3m".
public:
    static std::string formatElapsed(int64_t ms);

private:
    screens::Context        &_ctx;
    ui::Label               *_label;
    model::ConvRef           _conv     = model::kNoConv;
    model::Ts                _thread   = 0;
    model::Store::ObserverId _observer = 0;
    plat::TimerId            _tick     = 0; // while someone is thinking
    text::Style              _shownStyle;   // the plain style of the text shown
};

} // namespace shell
