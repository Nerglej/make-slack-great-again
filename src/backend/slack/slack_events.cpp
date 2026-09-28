// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "backend/slack/slack_events.h"

#include "backend/slack/json_mappers.h"

using namespace Qt::StringLiterals;

namespace slack {

std::optional<Event> normalizeSlackEvent(const QJsonObject &ev) {
    const auto type    = ev.value(u"type"_s).toString();
    const auto subtype = ev.value(u"subtype"_s).toString();

    if (type == u"message"_s) {
        if (subtype == u"message_deleted"_s) {
            // previous_message carries the deleted message; toMessage derives
            // threadRoot (set only when it was a reply) so the channel list can
            // drop the root's reply count.
            const auto prev = JsonMappers::toMessage(ev.value(u"previous_message"_s).toObject());
            return EvMessageDeleted{
                ConversationId{ev.value(u"channel"_s).toString()},
                ev.value(u"deleted_ts"_s).toString(),
                prev.threadRoot
            };
        }
        if (subtype == u"message_changed"_s || subtype == u"message_replied"_s) {
            return EvMessageChanged{
                ConversationId{ev.value(u"channel"_s).toString()},
                JsonMappers::toMessage(ev.value(u"message"_s).toObject())
            };
        }
        // Plain message or bot_message
        return EvMessageNew{
            ConversationId{ev.value(u"channel"_s).toString()}, JsonMappers::toMessage(ev)
        };
    }

    if (type == u"reaction_added"_s) {
        const auto item = ev.value(u"item"_s).toObject();
        return EvReactionAdded{
            ConversationId{item.value(u"channel"_s).toString()},
            item.value(u"ts"_s).toString(),
            ev.value(u"reaction"_s).toString(),
            UserId{ev.value(u"user"_s).toString()}
        };
    }

    if (type == u"reaction_removed"_s) {
        const auto item = ev.value(u"item"_s).toObject();
        return EvReactionRemoved{
            ConversationId{item.value(u"channel"_s).toString()},
            item.value(u"ts"_s).toString(),
            ev.value(u"reaction"_s).toString(),
            UserId{ev.value(u"user"_s).toString()}
        };
    }

    // channel_marked, group_marked, im_marked, mpim_marked all have the same shape
    if (type == u"channel_marked"_s || type == u"group_marked"_s || type == u"im_marked"_s ||
        type == u"mpim_marked"_s) {
        return EvConvMarked{
            ConversationId{ev.value(u"channel"_s).toString()},
            ev.value(u"ts"_s).toString(),
            ev.value(u"unread_count_display"_s).toInt(),
            ev.value(u"mention_count_display"_s).toInt()
        };
    }

    // user_typing is an RTM-only event (no Events API equivalent), so over Socket
    // Mode this branch is dead — but SessionRealtime rides RTM, where Slack DOES
    // send user_typing, so here it lights up the typing UI for real.
    if (type == u"user_typing"_s) {
        return EvTyping{
            ConversationId{ev.value(u"channel"_s).toString()},
            UserId{ev.value(u"user"_s).toString()}
        };
    }

    if (type == u"presence_change"_s) {
        return EvPresenceChanged{
            UserId{ev.value(u"user"_s).toString()},
            ev.value(u"presence"_s).toString() == u"active"_s
        };
    }

    if (type == u"dnd_updated_user"_s) {
        return EvDndChanged{
            UserId{ev.value(u"user"_s).toString()},
            ev.value(u"dnd_status"_s).toObject().value(u"dnd_enabled"_s).toBool()
        };
    }

    if (type == u"channel_created"_s) {
        return EvChannelCreated{JsonMappers::toConversation(ev.value(u"channel"_s).toObject())};
    }

    if (type == u"member_joined_channel"_s) {
        return EvMemberJoined{
            ConversationId{ev.value(u"channel"_s).toString()},
            UserId{ev.value(u"user"_s).toString()}
        };
    }

    // A member updated their profile (incl. avatar). user_change carries the
    // full user object — same shape as users.list — so toUser parses it
    // directly. (user_profile_changed carries only id+profile and would zero
    // out is_admin/is_bot/etc., so we don't map it.)
    if (type == u"user_change"_s) {
        return EvUserChanged{JsonMappers::toUser(ev.value(u"user"_s).toObject())};
    }

    // User groups: any change re-fetches the (small) list rather than patching
    // from the payload — subteam_members_changed carries only deltas, and the
    // self_* pair carries just an id.
    if (type == u"subteam_created"_s || type == u"subteam_updated"_s ||
        type == u"subteam_members_changed"_s || type == u"subteam_self_added"_s ||
        type == u"subteam_self_removed"_s) {
        return EvUsergroupsChanged{};
    }

    return std::nullopt;
}

std::optional<Event> huddleEventFor(const QJsonObject &ev) {
    if (ev.value(u"type"_s).toString() != u"message"_s)
        return std::nullopt;

    const auto subtype = ev.value(u"subtype"_s).toString();

    // A huddle starting: USLACKBOT posts a "huddle_thread" message carrying the
    // live `room`. The subtype itself proves it's a huddle, so "ongoing" is just
    // "no end timestamp" (participants may not be populated at the announce
    // moment); a roomless announce still counts as a start.
    QJsonObject room;
    QString     channel  = ev.value(u"channel"_s).toString();
    bool        isHuddle = false;
    if (subtype == u"huddle_thread"_s) {
        room     = ev.value(u"room"_s).toObject();
        isHuddle = true;
    } else if (subtype == u"message_changed"_s) {
        // A huddle ending/changing arrives as an edit of the huddle_thread
        // message (its room gains a date_end).
        const auto inner = ev.value(u"message"_s).toObject();
        if (inner.value(u"subtype"_s).toString() == u"huddle_thread"_s) {
            room     = inner.value(u"room"_s).toObject();
            isHuddle = true;
        }
    }
    if (!isHuddle)
        return std::nullopt;

    const auto h = JsonMappers::readHuddleRoom(room);
    return EvHuddleChanged{ConversationId{channel}, h.active, h.link, h.participants};
}

} // namespace slack
