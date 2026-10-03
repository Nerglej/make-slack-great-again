// How the message screens read a message: system lines, bots, the grouping
// of a run by one author, the author's name and picture, GIF paths. The one
// copy of each rule (the list, inline threads, dialogs, exports, summaries).
#pragma once

#include "app/model/store.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace screens {

// Messages by one author this close (µs) group under one header.
constexpr int64_t kGroupMicros = 300LL * 1000000;

// A system line: joins, leaves, topic / purpose / name changes, pins.
bool isSystem(const model::Message &m);
// Posted by a bot or an app (its own name and picture, the APP tag).
bool isBot(const model::Store &st, const model::Message &m);
// `cur` continues `prev`'s group: the same author (bot name too) within
// kGroupMicros; system lines, huddles and thread roots always stand alone.
bool groupable(const model::Message &prev, const model::Message &cur);

// The author as the header names it: a bot's (or an author-less message's)
// own name, else the user's label.
std::string_view   authorName(const model::Store &st, const model::Message &m);
// The author's picture, by the same rule.
const std::string &authorAvatar(const model::Store &st, const model::Message &m);

// An image path or URL that is a GIF (any case, a URL's ?query ignored).
bool isGifPath(std::string_view path);

} // namespace screens
