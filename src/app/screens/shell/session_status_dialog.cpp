#include "screens/shell/session_status_dialog.h"

#include "base/i18n.h"
#include "ui/controls.h"

using namespace ui;
using i18n::tr;

namespace shell {

namespace {

// A value: one line (cut off when too long), its whole text as the tooltip.
class ValueLabel final : public Label {
public:
    using Label::Label;
    std::string tooltip() const override { return text(); }
};

} // namespace

Dialog *
SessionStatusDialog::show(Window &w, const std::vector<std::pair<std::string, std::string>> &rows) {
    // msga's cardWidth: as wide as the window allows, 480…760.
    auto  d    = std::make_unique<Dialog>(tr("Session status"), 760.f);
    // msga's QGridLayout: spacing.xl between the columns, spacing.sm between
    // the rows, the values' column stretching.
    auto *grid = d->content()->add<View>();
    grid->style().row().spacing(16);
    auto *labels = grid->add<View>();
    labels->style().spacing(4).noShrink();
    auto *values = grid->add<View>();
    values->style().spacing(4).flex(1);
    values->style().shrink = 1;
    for (const auto &[label, value] : rows) {
        styledLabel(labels, label, pxFont(15, text::Weight::Regular, color(C::FormTextMuted)), 1);
        auto *v = values->add<ValueLabel>(value);
        v->setRichText([&] {
            text::AttributedText t;
            t.append(value, pxFont(15, text::Weight::Regular, themed(C::FormText)));
            return t;
        }());
        v->setMaxLines(1);
    }
    auto   *close = d->makeButton(tr("Close"), FormButton::Kind::Primary);
    Dialog *raw   = d.get();
    d->addButtonRow(close, nullptr);
    close->onClick = [raw] { raw->accept(); };
    w.showPopup(std::move(d));
    return raw;
}

} // namespace shell
