// msga's tag pill after a name (paintTagBadge): "APP" after a bot's name,
// "EXT" after a Slack Connect user's — in message headers, on shared-message
// cards, and (sidebar = true, coloured for the rail) in the chats list.
#pragma once

#include "ui/ui.h"

namespace screens {

ui::View *addTagBadge(ui::View *parent, bool ext, bool sidebar = false);

} // namespace screens
