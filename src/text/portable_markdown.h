// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Parsed text → composer markdown that means the same thing in ANY workspace.
//
// A message's rawText is backend-native and workspace-local: Slack mrkdwn holds
// <@U…>/<#C…>/<!subteam^S…> ids that point at nothing (or, in an Enterprise
// Grid sibling, at real people who get pinged) once posted elsewhere, and a
// Teams body is HTML. Forwarding into another workspace sends this instead:
//   • mentions, channel links and message links become the words they read as
//     in the source workspace ("@Alice", "#general") — plain text, not tokens;
//   • @here/@channel/@everyone stay plain words, never a broadcast;
//   • emphasis, code and quotes become CommonMark (**x**, _x_, ~~x~~, `x`,
//     ``` fences, "> "), links [label](url) or the bare URL, so the target's
//     own composer pipeline formats them like anything its user typed.
// A literal "<@…", "<#…" or "<!…" in the text gets a word joiner after the
// '<', so it can't turn into a Slack token on the way out either.
#pragma once

#include "backend/domain.h"

#include <functional>

namespace PortableMarkdown {

// The words a mention, channel, usergroup, emoji or message-link entity reads
// as (MsgRender::entityDisplayText resolves them against a session). A null
// string keeps the parsed text of the span.
using LeafText = std::function<QString(const TextEntity &)>;

QString fromText(const TextWithEntities &twe, const LeafText &leafText = {});

} // namespace PortableMarkdown
