// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"
#include "llm/voice_input.h"

#include <vector>

class Session;

// Builds the Voice::Context a composer host hands to
// ComposerWidget::setVoiceContextSource from what it already has loaded: the
// conversation from Session's roster, names from the user cache and the
// messages on screen. Never a network call — an unknown user is left out
// rather than fetched (Session::userDisplayName would kick off users.info).
namespace VoiceContext {

// How many of the newest messages go into Context::recentMessages.
inline constexpr int kMaxMessages = 30;

// `messages` oldest → newest (a thread: root first, then its replies). System
// rows and empty messages are skipped; each line reads "Name: text".
Voice::Context build(
    const Session              *session,
    const ConversationId       &conv,
    const std::vector<Message> &messages,
    int                         maxMessages = kMaxMessages
);

} // namespace VoiceContext
