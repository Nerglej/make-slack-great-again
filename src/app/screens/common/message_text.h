// A message's mrkdwn as plain text, resolved through a Store: the one place
// that turns mentions, channels, user groups and emoji into what the user
// reads. The rich renderer (messages/rich) styles the same results; the
// notification text, the stand-in, search and saved previews use plainText.
#pragma once

#include "app/model/store.h"
#include "app/mrkdwn/mrkdwn.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace screens {

// What entity `e` reads as: "@name" for a known user, "#name" for a channel
// the Store can name, "@handle" (else "@name") for a user group, the emoji's
// glyph (with skinTone 1-5 applied). "" when there is nothing better than the
// parser's own text, or for any other kind.
std::string entityText(const model::Store &store, const mrkdwn::Entity &e, uint8_t skinTone = 0);

// A notification's text: the parsed text with every entity entityText can
// resolve swapped in. For a Store that need not be the one on screen (a
// background workspace's notification).
std::string plainText(const model::Store &store, std::string_view mrkdwn);

// The first URL the message links to: a Link's URL, or a MessageLink's
// permalink (the app's own thread links have none and are skipped). "" when
// there is none. The forward dialog's "Copy link" uses it.
std::string firstLink(std::string_view mrkdwn);

// A local path as a file:// URL; a URL (it has "://") is returned as is.
std::string fileUrl(const std::string &pathOrUrl);

} // namespace screens
