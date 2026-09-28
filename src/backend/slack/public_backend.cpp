// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "public_backend.h"
#include "json_mappers.h"
#include "slack_auth.h"
#include "socket_mode_realtime.h"
#include "session_realtime.h"
#include "rtm_presence.h"
#include "auth/token_store.h"
#include "backend/common_commands.h"
#include "network/form_urlencode.h"

#include <QUrlQuery>
#include <QFile>
#include <QSet>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QDateTime>
#include <QTimer>
#include <QDebug>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace slack {

namespace {

// Slack error codes that mean "this method is not available to this caller" — as
// opposed to a passing failure. Used to decide whether an undocumented endpoint
// (client.counts) should be given up on permanently for a workspace. Deliberately
// an allowlist: onError also delivers transport failures (kConnectionLost, or a
// raw Qt error string), and treating one of those as definitive would cost a
// workspace the endpoint for the whole run over a brief network outage.
bool isMethodUnavailable(const QString &err) {
    static const QSet<QString> codes = {
        QStringLiteral("unknown_method"),
        QStringLiteral("method_deprecated"),
        QStringLiteral("method_not_supported_for_channel_type"),
        QStringLiteral("not_allowed_token_type"),
        QStringLiteral("missing_scope"),
        QStringLiteral("no_permission"),
        QStringLiteral("invalid_arguments"),
        QStringLiteral("org_login_required"),
        QStringLiteral("enterprise_is_restricted"),
        QStringLiteral("user_is_restricted"),
        QStringLiteral("ekm_access_denied"),
    };
    return codes.contains(err);
}

// Slack resolves a session (xoxc) token in the context of the HOST the call is
// made on. On Enterprise Grid, bare slack.com/api has no workspace to resolve
// to, so the call lands in the ORG context and every workspace-scoped method is
// refused with `enterprise_is_restricted` — which is exactly why a Grid sign-in
// came up with an empty conversation list (issue #49). The web client never
// calls slack.com/api either: it talks to its own workspace host. Do the same
// whenever we know the host — for a single-workspace team the two hosts are
// interchangeable, so this costs non-Grid workspaces nothing.
QString apiBaseFor(const QString &workspaceUrl) {
    const QUrl url(workspaceUrl.trimmed());
    if (!url.isValid() || url.scheme() != QLatin1String("https") || url.host().isEmpty())
        return WebApiClient::kBaseUrl;
    return QStringLiteral("https://") + url.host() + QStringLiteral("/api/");
}

// Translate a non-idempotent write's failure for the Session: a connection that
// died mid-flight says nothing about whether the write landed, so it travels up
// as kAmbiguousWriteFailure (do not roll back) rather than as a rejection.
QString ambiguousOrReal(const QString &err) {
    return err == WebApiClient::kConnectionLost ? kAmbiguousWriteFailure : err;
}

} // namespace

PublicBackend::PublicBackend(
    const Credentials &creds, const AppConfig &appCfg, const QString &refreshUrl
)
    : _teamId(creds.teamId), _refreshToken(creds.refreshToken), _refreshUrl(refreshUrl),
      _api(new WebApiClient(nullptr)), _historyApi(new WebApiClient(nullptr)),
      _infoApi(new WebApiClient(nullptr)) {
    // Socket Mode is app-key (xapp) based and only serves OAuth workspaces. A
    // session-auth workspace has no xapp and polls instead, so it must NOT open
    // the shared socket — doing so connects with the build's app key and causes
    // exactly the shared-key contention (bare-1000 closes, "same app keys on
    // another device" banner) that session auth exists to avoid. Not acquiring the
    // refcounted handle means: all-session-auth ⇒ socket never opens at all.
    if (!creds.isSessionAuth()) {
        _realtimeHandle = std::make_unique<SharedRealtime>();
        _sharedRealtime = _realtimeHandle->socket();
    }

    _api->setToken(creds.xoxp);
    _historyApi->setToken(creds.xoxp);
    _infoApi->setToken(creds.xoxp);
    // Session-authed workspaces (xoxc token) also need the `d` cookie on every
    // request; no-op for OAuth workspaces where the cookie is empty. These
    // workspaces have no Socket Mode socket (needs an xapp token), so realtime
    // falls back to Session's polling — see setupTokenRefresh's proactive guard,
    // which is naturally skipped since session creds carry no refresh token.
    if (creds.isSessionAuth()) {
        _sessionAuth = true;
        _api->setCookie(creds.cookie);
        _historyApi->setCookie(creds.cookie);
        _infoApi->setCookie(creds.cookie);
        // Realtime for session workspaces is delivered by Session's fast poll
        // (foregroundPollGapMs() == 5 s for the open chat, plus the client.counts
        // activity snapshot in loadUnreadCounts() that tells it which OTHER
        // conversations to poll), NOT the classic RTM WebSocket. RTM itself DOES
        // work for a session token — the "~5 s LEGACY_BOT close" that shelved it
        // was an unauthenticated handshake (the wss URL carries no auth; the `d`
        // cookie must ride the handshake, see RtmPresence) — but switching
        // delivery over to it is a separate effort with its own dedup/backfill
        // implications. SessionRealtime is kept for that future work and is
        // intentionally NOT started here.
        // _sessionRealtime = std::make_unique<SessionRealtime>(creds.xoxp, creds.cookie, &_events);
        // Pin every call to this workspace's own host (see apiBaseFor). Only
        // session auth: an OAuth token is workspace-scoped by construction, so
        // the shared slack.com base already resolves it unambiguously.
        applyApiBase(apiBaseFor(creds.workspaceUrl));
        // The presence link (PresenceMode): an RTM socket held only so Slack counts
        // us as a connected client. Idle until Session applies the preference via
        // setPresenceMode(). Its rtm.connect rides _api (token, cookie, host).
        _rtmPresence = std::make_unique<RtmPresence>(_api, creds.cookie);
        QObject::connect(
            _rtmPresence.get(), &RtmPresence::stateChanged, [this](PresenceLinkState st) {
                _events.fire_copy(Event{EvPresenceLinkChanged{st}});
            }
        );
    }
    // Pace the background lane on the info client: a reconnect enqueues one
    // conversations.info per DM/MPDM (100+ on a busy workspace), and that method
    // is Slack Tier 3 (~50/min). ~1.2 s spacing keeps the whole sweep under the
    // tier so it never 429s, while interactive (Normal) info calls still preempt
    // and run unpaced.
    _infoApi->setBackgroundPaceMs(1200);
    // Pre-warm TLS so the first API calls skip the handshake latency. The info
    // client is not pre-warmed: its background sweep starts well after launch.
    const QString apiHost = QUrl(_apiBase).host();
    _api->preWarm(apiHost);
    _historyApi->preWarm(apiHost);

    // Surface HTTP 429s to the UI (transient notice). All three clients can be
    // throttled; the background-sweep _infoApi is the usual culprit. The sending
    // client is the QObject context, so the connection dies with it.
    for (WebApiClient *client : {_api, _historyApi, _infoApi})
        QObject::connect(
            client, &WebApiClient::rateLimited, client, [this](const QString &method, int secs) {
                _events.fire(EvRateLimited{method, secs});
            }
        );

    // Always install the handler so token_expired triggers logout/refresh even
    // when the stored token has no companion refresh token yet.
    setupTokenRefresh(creds, appCfg);
}

void PublicBackend::setupTokenRefresh(const Credentials &creds, const AppConfig &appCfg) {
    _appCfg         = appCfg;
    _tokenExpiresAt = creds.expiresAt;

    // Reactive: WebApiClient calls this on token_expired API error
    auto handler = [this](std::function<void(bool)> done) {
        qDebug() << "[TokenRefresh] token_expired received";
        triggerRefresh(std::move(done));
    };
    _api->setOnTokenExpired(handler);
    _historyApi->setOnTokenExpired(handler);
    _infoApi->setOnTokenExpired(handler);

    // Proactive: refresh before expiry so users never see a token_expired error.
    // A periodic wall-clock check rather than one long single-shot timer: Qt
    // timers run on the monotonic clock, which pauses during system suspend, so
    // a multi-hour timer fires hours late after a night of sleep. The periodic
    // check also retries transient refresh failures automatically.
    if (!_refreshToken.isEmpty()) {
        _proactiveRefreshTimer = new QTimer(_api);
        _proactiveRefreshTimer->setInterval(60 * 1000);
        QObject::connect(_proactiveRefreshTimer, &QTimer::timeout, _api, [this]() {
            maybeProactiveRefresh();
        });
        _proactiveRefreshTimer->start();
        if (_tokenExpiresAt > 0) {
            const qint64 secsLeft = _tokenExpiresAt - QDateTime::currentSecsSinceEpoch();
            qDebug() << "[TokenRefresh] token healthy, valid for" << secsLeft
                     << "s more; will auto-refresh in" << std::max<qint64>(secsLeft - 3600, 0)
                     << "s";
        }
        // First check is deferred: doRefresh is virtual and must not be
        // dispatched from within the constructor.
        QTimer::singleShot(0, _api, [this]() { maybeProactiveRefresh(); });
    }
}

void PublicBackend::triggerRefresh(std::function<void(bool)> done) {
    qDebug() << "[TokenRefresh] triggerRefresh; inProgress=" << _refreshInProgress
             << "refreshToken present=" << !_refreshToken.isEmpty();
    _refreshWaiters.push_back(std::move(done));
    if (_refreshInProgress) {
        qDebug() << "[TokenRefresh] refresh already in flight, queuing";
        return;
    }
    _refreshInProgress = true;
    doRefresh([this](RefreshResult result) {
        qDebug() << "[TokenRefresh] doRefresh completed, result="
                 << (result == RefreshResult::Success          ? "success"
                     : result == RefreshResult::TransientError ? "transient error"
                                                               : "auth error");
        _refreshInProgress = false;
        auto waiters       = std::move(_refreshWaiters);
        for (auto &w : waiters)
            w(result == RefreshResult::Success);
        if (result == RefreshResult::AuthError) {
            // Credentials definitively rejected — stop retrying, show login.
            if (_proactiveRefreshTimer)
                _proactiveRefreshTimer->stop();
            // Drain every client's pending queue before tearing anything down.
            // The token_expired waiters above only drain the client that
            // actually got the expiry; a refresh kicked off proactively (no-op
            // waiter) leaves queued calls — and their self-referential paginate
            // Ctx — orphaned forever. Done before force_assign so the queues are
            // emptied while the backend is still alive (force_assign may notify
            // subscribers that tear the session down synchronously).
            _api->failAllPending(u"token_expired"_s);
            _historyApi->failAllPending(u"token_expired"_s);
            _infoApi->failAllPending(u"token_expired"_s);
            _authState.force_assign(AuthState::NotLoggedIn);
        }
        // TransientError: stay logged in — the periodic check retries within 60 s.
    });
}

void PublicBackend::maybeProactiveRefresh() {
    if (_refreshToken.isEmpty() || _tokenExpiresAt == 0 || _refreshInProgress)
        return;

    const qint64 secsLeft = _tokenExpiresAt - QDateTime::currentSecsSinceEpoch();
    if (secsLeft > 3600)
        return;

    qDebug() << "[TokenRefresh] proactive: token expires in" << secsLeft << "s, refreshing now";
    triggerRefresh([](bool) {});
}

void PublicBackend::doRefresh(std::function<void(RefreshResult)> done) {
    if (_refreshToken.isEmpty()) {
        qDebug() << "[TokenRefresh] no refresh token stored — forcing logout";
        done(RefreshResult::AuthError);
        return;
    }
    qDebug() << "[TokenRefresh] posting oauth.v2.access for team" << _teamId;

    // oauth.v2.access with grant_type=refresh_token is the rotation refresh
    // call; oauth.v2.exchange only migrates a legacy token and rejects this
    // grant.
    const QUrl endpoint{
        _refreshUrl.isEmpty() ? QStringLiteral("https://slack.com/api/oauth.v2.access")
                              : _refreshUrl
    };
    auto           *nam = new QNetworkAccessManager(_api); // _api owns it → cleaned up with backend
    QNetworkRequest req(endpoint);
    req.setHeader(QNetworkRequest::ContentTypeHeader, u"application/x-www-form-urlencoded"_s);
    QUrlQuery body;
    body.addQueryItem(u"grant_type"_s, u"refresh_token"_s);
    body.addQueryItem(u"client_id"_s, _appCfg.clientId);
    body.addQueryItem(u"client_secret"_s, _appCfg.clientSecret);
    body.addQueryItem(u"refresh_token"_s, _refreshToken);
    auto *reply = nam->post(req, net::formUrlEncode(body));

    QObject::connect(reply, &QNetworkReply::finished, _api, [this, reply, nam, done]() mutable {
        reply->deleteLater();
        nam->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "[TokenRefresh] network error:" << reply->errorString();
            done(RefreshResult::TransientError);
            return;
        }
        const auto raw = reply->readAll();
        auto       obj = QJsonDocument::fromJson(raw).object();
        if (!obj.value(u"ok"_s).toBool()) {
            const QString err = obj.value(u"error"_s).toString();
            qWarning() << "[TokenRefresh] Slack error:" << err;
            // Server-side hiccups are retried later; anything else (e.g.
            // invalid_refresh_token) means the credentials are dead.
            const bool transient = err == QLatin1String("internal_error") ||
                                   err == QLatin1String("service_unavailable") ||
                                   err == QLatin1String("fatal_error") ||
                                   err == QLatin1String("ratelimited");
            done(transient ? RefreshResult::TransientError : RefreshResult::AuthError);
            return;
        }
        const QString newToken   = obj.value(u"access_token"_s).toString();
        const QString newRefresh = obj.value(u"refresh_token"_s).toString();
        if (newToken.isEmpty()) {
            qWarning() << "[TokenRefresh] empty access_token in successful response";
            done(RefreshResult::TransientError);
            return;
        }

        // Update in-memory state
        _api->setToken(newToken);
        _historyApi->setToken(newToken);
        _infoApi->setToken(newToken);
        _refreshToken          = newRefresh.isEmpty() ? _refreshToken : newRefresh;
        const qint64 expiresIn = obj.value(u"expires_in"_s).toInteger(0);
        if (expiresIn > 0)
            _tokenExpiresAt = QDateTime::currentSecsSinceEpoch() + expiresIn;

        // Persist atomically. Decode the existing record's Slack auth blob,
        // update only the rotated token fields, re-encode — displayName/iconUrl
        // are preserved.
        const WorkspaceKey key{kService, _teamId};
        Credentials        saved = fromRecord(
            TokenStore::loadWorkspace(key).value_or(TokenStore::WorkspaceRecord{key, {}, {}, {}})
        );
        saved.xoxp         = newToken;
        saved.refreshToken = _refreshToken;
        saved.expiresAt    = _tokenExpiresAt;
        TokenStore::saveWorkspace(toRecord(saved));

        qDebug() << "[TokenRefresh] token refreshed successfully for team" << _teamId
                 << "next expiry in" << expiresIn << "s";
        done(RefreshResult::Success);
    });
}

PublicBackend::~PublicBackend() {
    // Drop our sink before _realtimeHandle releases (and possibly destroys) the
    // shared socket.
    if (_sharedRealtime)
        _sharedRealtime->removeSink(&_events);
    // Before _api goes: its in-flight rtm.connect callbacks guard on a QPointer
    // to this, and the socket must not outlive the backend that counts as online.
    _rtmPresence.reset();
    delete _infoApi;
    delete _historyApi;
    delete _api;
}

void PublicBackend::setApiBaseUrlForTests(const QString &url) {
    applyApiBase(url);
    _apiBasePinned = true; // auth.test's `url` must not steer the fake server away
}

void PublicBackend::applyApiBase(const QString &base) {
    if (base.isEmpty() || base == _apiBase || _apiBasePinned)
        return;
    _apiBase = base;
    _api->setBaseUrl(base);
    _historyApi->setBaseUrl(base);
    _infoApi->setBaseUrl(base);
}

rpl::producer<AuthState> PublicBackend::authState() const {
    return _authState.value();
}

Capabilities PublicBackend::capabilities() const {
    // What the public API path supports today. typing + livePresence stay false:
    // live "is typing" and realtime presence_change need the internal path
    // (Phase 5) — the public path only polls presence. Everything else listed
    // here is already live in the UI, so reporting it true keeps behavior
    // unchanged now that the UI gates on these flags.
    Capabilities c;
    c.presence         = true; // polled presence (users.getPresence) → online/away dots
    c.huddles          = true;
    c.canvases         = true;
    c.slashCommands    = true;
    c.reactions        = true;
    c.editMessage      = true;
    c.deleteMessage    = true;
    c.threads          = true;
    c.newThreads       = true;
    c.pins             = true;
    c.deleteFiles      = true;
    c.replyBroadcast   = true;
    c.memberList       = true; // conversations.members
    c.gifAttachments   = true;
    c.moveToThread     = true; // sendMessage confirms from the chat.postMessage response
    c.fileUpload       = true;
    c.scheduledSend    = true; // chat.scheduleMessage
    // A message permalink is teamUrl (auth.test's `url`) + /archives/<conv>/p<ts>,
    // the exact string chat.getPermalink would return — no API call needed.
    c.permalinks       = true;
    // The Threads overview rides subscriptions.thread.getView, which Slack only
    // serves to a session (xoxc) token — an OAuth workspace would get every call
    // rejected, so don't claim the capability there (no dead roster entry).
    c.threadsView      = _sessionAuth;
    // Message reminders ride the saved.* family ("Later"), likewise served only
    // to a session token. The public reminders.* API is retired and never could
    // attach a reminder to a message.
    c.messageReminders = _sessionAuth;
    // Bot-button presses ride the internal blocks.actions (what the official
    // client calls), likewise session-token only.
    c.botButtons       = _sessionAuth;
    // users.prefs.get (the stored sidebar theme) is session-token only as well.
    c.sidebarTheme     = _sessionAuth;
    // The presence link is an RTM socket, and rtm.connect refuses a granular
    // OAuth token (not_allowed_token_type) — session tokens only.
    c.presenceLink     = _sessionAuth;
    // users.list is a plain paged snapshot with no per-member side requests, so
    // the Session may re-fetch it daily to pick up renames/avatars on a session
    // that never restarts (the only other refresh path, user_change, needs push).
    c.rosterRefresh    = true;
    // "Remove preview" rides the internal chat.deleteAttachment, session-token
    // only like the rest of that family; OAuth workspaces keep the local hide.
    c.removePreview    = _sessionAuth;
    return c;
}

bool PublicBackend::isSyntheticUser(UserId id) const {
    // Slack's two built-in pseudo-accounts: USLACKBOT (Slackbot) and USLACK (the
    // "Slack" workspace/billing notifier). Both are absent from users.list and
    // both report is_bot=false, so only the fixed ids identify them.
    return id.value == QLatin1String("USLACKBOT") || id.value == QLatin1String("USLACK");
}

bool PublicBackend::isBotId(UserId id) const {
    return id.value.startsWith('B');
}

bool PublicBackend::isUserId(UserId id) const {
    // Human users are "U…" on a normal workspace and "W…" on Enterprise Grid;
    // external Slack Connect collaborators surface with either prefix. Accept
    // both so they're resolved via the user-info path (mirrors the U/W test in
    // isUnresolvedUserId) — a W-only omission left Connect partners stuck as a
    // raw id with no name or avatar.
    return id.value.startsWith('U') || id.value.startsWith('W');
}

bool PublicBackend::isUnresolvedUserId(const QString &s) const {
    // Looks like a raw Slack user id ("U0A1B2C3D" / "W…" for enterprise) rather
    // than a human name — long enough, leading U/W, and otherwise [A-Z0-9].
    if (s.length() < 9)
        return false;
    if (s[0] != 'U' && s[0] != 'W')
        return false;
    for (int i = 1; i < s.length(); ++i) {
        const QChar c = s[i];
        if (!c.isDigit() && !(c >= 'A' && c <= 'Z'))
            return false;
    }
    return true;
}

void PublicBackend::connectRealtime() {
    // Session auth: per-workspace RTM WebSocket (user-level). Fires straight into
    // this backend's own event stream (no shared multi-workspace fan-out).
    if (_sessionRealtime) {
        _sessionRealtime->start();
        return;
    }
    if (!_sharedRealtime)
        return; // no xapp token configured → no realtime, same as before
    _sharedRealtime->addSink(&_events);
    _sharedRealtime->start();
}

void PublicBackend::disconnectRealtime() {
    // Going offline on purpose (logout, workspace drop): stop counting as a
    // connected client too.
    if (_rtmPresence)
        _rtmPresence->setMode(PresenceMode::Native);
    if (_sessionRealtime) {
        _sessionRealtime->stop();
        return;
    }
    if (_sharedRealtime)
        _sharedRealtime->removeSink(&_events);
}

void PublicBackend::verifyRealtime() {
    if (_sessionRealtime) {
        _sessionRealtime->ensureConnected();
        return;
    }
    if (_sharedRealtime)
        _sharedRealtime->ensureConnected();
}

void PublicBackend::reestablishRealtime() {
    if (_sessionRealtime) {
        _sessionRealtime->reconnectNow();
        return;
    }
    if (_sharedRealtime)
        _sharedRealtime->reconnectNow();
}

// ── Snapshot loads ────────────────────────────────────────────────

rpl::producer<UserId> PublicBackend::loadMe() {
    return [this](auto consumer) mutable {
        _api->call(
            u"auth.test"_s,
            QUrlQuery{},
            [this, consumer](QJsonObject resp) mutable {
                _meUserId = UserId{resp.value(u"user_id"_s).toString()};
                _teamUrl  = resp.value(u"url"_s).toString();
                // auth.test's `url` is the authoritative workspace host. Adopt it
                // for a session workspace whose stored credentials predate
                // workspaceUrl (or carry a stale one) — see apiBaseFor.
                if (_sessionAuth)
                    applyApiBase(apiBaseFor(_teamUrl));
                if (!resp.value(u"enterprise_id"_s).toString().isEmpty())
                    qInfo().noquote()
                        << "Slack workspace" << _teamId << "is part of Enterprise Grid org"
                        << resp.value(u"enterprise_id"_s).toString();
                consumer.put_next(UserId{resp.value(u"user_id"_s).toString()});
                consumer.put_done();
            },
            [consumer](QString err) mutable {
                qWarning() << "loadMe error:" << err;
                consumer.put_done();
            }
        );
        return rpl::lifetime();
    };
}

rpl::producer<std::vector<Conversation>> PublicBackend::loadConversations() {
    return [this](auto consumer) mutable {
        auto deliver = [consumer](std::vector<Conversation> convs) mutable {
            consumer.put_next(std::move(convs));
            consumer.put_done();
        };
        // Session::reloadConversations REPLACES the roster with whatever arrives,
        // so a failed load must complete without a value rather than hand over an
        // empty list (which would wipe the sidebar and the cache with it).
        auto fail = [consumer]() mutable { consumer.put_done(); };
        // A Grid workspace never gets a usable answer out of conversations.list,
        // so once it has told us so we go straight to the web-client route.
        if (_convListRestricted) {
            loadConversationsViaWebClient(deliver, fail);
            return rpl::lifetime();
        }

        auto      accum = std::make_shared<std::vector<Conversation>>();
        QUrlQuery params;
        params.addQueryItem(u"types"_s, u"public_channel,private_channel,im,mpim"_s);
        params.addQueryItem(u"exclude_archived"_s, u"true"_s);
        // Slack's default page is 100; conversations.list is heavily rate-limited
        // (Tier 2), so pull the max 1000 per page to minimise the number of calls
        // a full reload costs (fewer pages = fewer chances to trip a 429).
        params.addQueryItem(u"limit"_s, u"1000"_s);
        // Required when the token resolves as org-level (Enterprise Grid), and
        // documented as ignored for a workspace-level one — so it is always safe.
        if (!_teamId.isEmpty())
            params.addQueryItem(u"team_id"_s, _teamId);

        _api->paginate(
            u"conversations.list"_s,
            u"channels"_s,
            params,
            [accum](QJsonArray page) {
                auto batch = JsonMappers::toConversations(page);
                accum->insert(accum->end(), batch.begin(), batch.end());
            },
            [deliver, accum]() mutable { deliver(std::move(*accum)); },
            [this, deliver, fail](QString err) mutable {
                // "The method cannot be called from an Enterprise": conversations.list
                // is simply not served to a Grid session token. Latch it and take the
                // route the web client itself takes (see loadConversationsViaWebClient)
                // — this is the whole of issue #49.
                if (err == QLatin1String("enterprise_is_restricted")) {
                    if (!_convListRestricted) {
                        _convListRestricted = true;
                        qInfo() << "conversations.list is restricted on this Enterprise Grid "
                                   "workspace — falling back to client.userBoot + im.list";
                    }
                    loadConversationsViaWebClient(deliver, fail);
                    return;
                }
                qWarning() << "loadConversations error:" << err;
                fail();
            }
        );
        return rpl::lifetime();
    };
}

void PublicBackend::loadConversationsViaWebClient(
    std::function<void(std::vector<Conversation>)> done, std::function<void()> fail
) {
    // Two calls, exactly as Slack's own client boots: client.userBoot for every
    // channel/private channel/MPDM the user belongs to, im.list for every DM
    // (userBoot only carries the handful of DMs that are currently open). Both
    // are undocumented web-client endpoints — served to a session token, and the
    // only conversation listing a Grid workspace hands one out.
    //
    // Two known gaps vs. conversations.list, both benign here: no unread state
    // (client.counts already supplies that for session auth) and no public
    // channels the user has NOT joined, which only thins the browse dialog. The
    // endpoint that would restore those (search.modules.channels) pages the whole
    // org directory — tens of thousands of channels on a Grid — so it is
    // deliberately not part of a startup load.
    // Both halves must land: handing the Session channels-without-DMs (or the
    // reverse) would replace the roster with a half of itself. One failure ⇒ the
    // whole load fails, exactly as a mid-pagination conversations.list error did.
    struct Accum {
        std::vector<Conversation>                      convs;
        int                                            pending  = 2;
        int                                            failures = 0;
        std::function<void(std::vector<Conversation>)> done;
        std::function<void()>                          fail;
        void                                           finish() {
            if (--pending > 0)
                return;
            if (failures > 0)
                fail();
            else
                done(std::move(convs));
        }
    };
    auto acc  = std::make_shared<Accum>();
    acc->done = std::move(done);
    acc->fail = std::move(fail);

    QUrlQuery bootParams;
    bootParams.addQueryItem(u"min_channel_updated"_s, u"0"_s);
    _api->call(
        u"client.userBoot"_s,
        bootParams,
        [acc](QJsonObject resp) {
            // conversations.list was asked for exclude_archived; userBoot has no
            // such switch, so drop archived channels here to keep parity.
            QJsonArray live;
            for (const auto v : resp.value(u"channels"_s).toArray())
                if (!v.toObject().value(u"is_archived"_s).toBool())
                    live.append(v);
            auto batch = JsonMappers::toConversations(live);
            acc->convs.insert(acc->convs.end(), batch.begin(), batch.end());
            acc->finish();
        },
        [acc](QString err) mutable {
            qWarning() << "loadConversations (client.userBoot) error:" << err;
            ++acc->failures;
            acc->finish();
        }
    );

    QUrlQuery imParams;
    imParams.addQueryItem(u"get_latest"_s, u"true"_s);
    imParams.addQueryItem(u"get_read_state"_s, u"true"_s);
    imParams.addQueryItem(u"limit"_s, u"1000"_s);
    _api->paginate(
        u"im.list"_s,
        u"ims"_s,
        imParams,
        [acc](QJsonArray page) {
            QJsonArray live;
            for (const auto v : page)
                if (!v.toObject().value(u"is_archived"_s).toBool())
                    live.append(v);
            auto batch = JsonMappers::toConversations(live);
            acc->convs.insert(acc->convs.end(), batch.begin(), batch.end());
        },
        [acc]() { acc->finish(); },
        [acc](QString err) mutable {
            qWarning() << "loadConversations (im.list) error:" << err;
            ++acc->failures;
            acc->finish();
        }
    );
}

rpl::producer<Conversation>
PublicBackend::loadConversationInfo(ConversationId id, bool background) {
    return [this, id, background](auto consumer) mutable {
        QUrlQuery params;
        params.addQueryItem(u"channel"_s, id.value);
        auto onOk = [consumer](QJsonObject resp) mutable {
            consumer.put_next(JsonMappers::toConversation(resp.value(u"channel"_s).toObject()));
            consumer.put_done();
        };
        auto onErr = [consumer, id](QString err) mutable {
            qWarning() << "loadConversationInfo error:" << id.value << err;
            // channel_not_found is definitive: this conversation does not
            // exist for this workspace. Emit a not-found sentinel so Session
            // can remember it and stop re-fetching (a busy foreign conv would
            // otherwise re-fire on every message). Any other error (a
            // sustained transient that exhausted retries, missing_scope, …)
            // stays a bare completion so the caller retries as before.
            if (err == QLatin1String("channel_not_found"))
                consumer.put_next(Conversation{.id = id, .notFound = true});
            consumer.put_done();
        };
        // Bulk sweeps ride the paced low-priority lane so they never crowd out an
        // interactive fetch (a freshly-joined channel, an unknown-conv message)
        // or burst past conversations.info's rate-limit tier.
        if (background)
            _infoApi->callBackground(
                u"conversations.info"_s, params, std::move(onOk), std::move(onErr)
            );
        else
            _infoApi->call(u"conversations.info"_s, params, std::move(onOk), std::move(onErr));
        return rpl::lifetime();
    };
}

rpl::producer<std::vector<ConvCounts>> PublicBackend::loadUnreadCounts() {
    return [this](auto consumer) mutable {
        // Unsupported: complete WITHOUT a value (the Backend contract's "no
        // snapshot" signal, which makes the Session fall back to its roster diff).
        // OAuth workspaces have Socket Mode, so they neither need this nor are
        // allowed to call it.
        if (!_sessionAuth || _countsUnavailable) {
            consumer.put_done();
            return rpl::lifetime();
        }
        _api->call(
            u"client.counts"_s,
            QUrlQuery{},
            [consumer](QJsonObject resp) mutable {
                consumer.put_next(JsonMappers::toConvCounts(resp));
                consumer.put_done();
            },
            [this, consumer](QString err) mutable {
                // Latch only on a Slack error code that says the METHOD is not ours
                // to call. onError also carries transport failures (kConnectionLost,
                // or a raw Qt errorString for a DNS/TLS failure), and a workspace
                // must not lose the mechanism for the rest of the run because the
                // laptop was briefly offline — those stay unlatched and the Session
                // degrades on its own after a few empty attempts.
                if (isMethodUnavailable(err)) {
                    _countsUnavailable = true;
                    qWarning() << "client.counts rejected:" << err
                               << "— unread-counts polling disabled for this workspace";
                }
                consumer.put_done();
            },
            /*quietErrors=*/true
        );
        return rpl::lifetime();
    };
}

rpl::producer<std::vector<User>> PublicBackend::loadUsers() {
    return [this](auto consumer) mutable {
        auto      accum = std::make_shared<std::vector<User>>();
        QUrlQuery params;
        // Same org-token rule as conversations.list: required for an org-level
        // token, ignored for a workspace-level one.
        if (!_teamId.isEmpty())
            params.addQueryItem(u"team_id"_s, _teamId);
        _historyApi->paginate(
            u"users.list"_s,
            u"members"_s,
            params,
            [accum, myTeam = _teamId](QJsonArray page) {
                auto batch = JsonMappers::toUsers(page);
                for (auto &u : batch)
                    u.isExternal = u.isExternal ||
                                   (!u.teamId.isEmpty() && !myTeam.isEmpty() && u.teamId != myTeam);
                accum->insert(accum->end(), batch.begin(), batch.end());
            },
            [consumer, accum]() mutable {
                consumer.put_next(std::move(*accum));
                consumer.put_done();
            },
            [consumer](QString err) mutable {
                qWarning() << "loadUsers error:" << err;
                consumer.put_done();
            }
        );
        return rpl::lifetime();
    };
}

rpl::producer<std::vector<Usergroup>> PublicBackend::loadUsergroups() {
    return [this](auto consumer) mutable {
        QUrlQuery params;
        // On Enterprise Grid a user group belongs to the org, and usergroups.list
        // answers for the workspace named by team_id (required for an org-level
        // token, ignored for a workspace-level one) — without it a member
        // workspace sees an empty list and every <!subteam^S…> stays raw.
        if (!_teamId.isEmpty())
            params.addQueryItem(u"team_id"_s, _teamId);
        // Member ids, so a mention of a group I belong to counts as a mention.
        params.addQueryItem(u"include_users"_s, u"1"_s);
        _api->call(
            u"usergroups.list"_s,
            params,
            [consumer](QJsonObject resp) mutable {
                consumer.put_next(JsonMappers::toUsergroups(resp.value(u"usergroups"_s).toArray()));
                consumer.put_done();
            },
            [consumer](QString err) mutable {
                // missing_scope on an OAuth token issued before usergroups:read
                // was requested: mentions keep their label fallback.
                qWarning() << "loadUsergroups error:" << err;
                consumer.put_done();
            },
            /*quietErrors=*/true
        );
        return rpl::lifetime();
    };
}

rpl::producer<bool> PublicBackend::loadPresence(UserId userId) {
    return loadPresenceImpl(std::move(userId), /*background=*/false);
}

rpl::producer<bool> PublicBackend::loadPresenceBackground(UserId userId) {
    return loadPresenceImpl(std::move(userId), /*background=*/true);
}

rpl::producer<bool> PublicBackend::loadPresenceImpl(UserId userId, bool background) {
    return [this, userId, background](auto consumer) mutable {
        QUrlQuery params;
        params.addQueryItem(u"user"_s, userId.value);
        auto onOk = [consumer](QJsonObject resp) mutable {
            bool active = resp.value(u"presence"_s).toString() == u"active"_s;
            consumer.put_next(std::move(active));
            consumer.put_done();
        };
        auto onErr = [consumer](QString err) mutable {
            qWarning() << "loadPresence error:" << err;
            consumer.put_done();
        };
        // The periodic DM-partner sweep (Session::pollDmPresence) rides the paced
        // low-priority lane like the conversations.info sweeps do; a single
        // interactive probe (opening a DM, the hover card) goes straight out.
        if (background)
            _infoApi->callBackground(
                u"users.getPresence"_s, params, std::move(onOk), std::move(onErr)
            );
        else
            _api->call(u"users.getPresence"_s, params, std::move(onOk), std::move(onErr));
        return rpl::lifetime();
    };
}

rpl::producer<SelfPresence> PublicBackend::loadSelfPresence() {
    return [this](auto consumer) mutable {
        // No "user" param → Slack returns the rich self snapshot
        // (online / auto_away / manual_away / connection_count).
        _api->call(
            u"users.getPresence"_s,
            QUrlQuery{},
            [consumer](QJsonObject resp) mutable {
                consumer.put_next(JsonMappers::toSelfPresence(resp));
                consumer.put_done();
            },
            [consumer](QString err) mutable {
                qWarning() << "loadSelfPresence error:" << err;
                consumer.put_done();
            }
        );
        return rpl::lifetime();
    };
}

rpl::producer<User> PublicBackend::loadBotInfo(UserId botId) {
    return [this, botId](auto consumer) mutable {
        QUrlQuery params;
        params.addQueryItem(u"bot"_s, botId.value);
        _api->call(
            u"bots.info"_s,
            params,
            [consumer](QJsonObject resp) mutable {
                const auto bot   = resp.value(u"bot"_s).toObject();
                const auto icons = bot.value(u"icons"_s).toObject();
                User       u;
                u.id          = UserId{bot.value(u"id"_s).toString()};
                u.name        = bot.value(u"name"_s).toString();
                u.displayName = bot.value(u"name"_s).toString();
                u.avatarUrl   = icons.value(u"image_72"_s)
                                    .toString(icons.value(u"image_48"_s)
                                                  .toString(icons.value(u"image_36"_s).toString()));
                u.isBot       = true;
                consumer.put_next(std::move(u));
                consumer.put_done();
            },
            [consumer](QString err) mutable {
                qWarning() << "loadBotInfo error:" << err;
                consumer.put_done();
            }
        );
        return rpl::lifetime();
    };
}

rpl::producer<User> PublicBackend::loadUser(UserId userId) {
    return loadUserImpl(std::move(userId), /*background=*/false);
}

rpl::producer<User> PublicBackend::loadUserBackground(UserId userId) {
    return loadUserImpl(std::move(userId), /*background=*/true);
}

rpl::producer<User> PublicBackend::loadUserImpl(UserId userId, bool background) {
    return [this, userId, background](auto consumer) mutable {
        QUrlQuery params;
        params.addQueryItem(u"user"_s, userId.value);
        auto onOk = [consumer, myTeam = _teamId](QJsonObject resp) mutable {
            auto u = JsonMappers::toUser(resp.value(u"user"_s).toObject());
            u.isExternal =
                u.isExternal || (!u.teamId.isEmpty() && !myTeam.isEmpty() && u.teamId != myTeam);
            consumer.put_next(std::move(u));
            consumer.put_done();
        };
        auto onErr = [consumer](QString err) mutable {
            qWarning() << "loadUser error:" << err;
            consumer.put_done();
        };
        // The Session's periodic off-roster re-probe rides the paced low-priority
        // lane like the conversations.info sweeps; an on-demand resolve (a DM peer
        // users.list omitted, an unknown author) goes straight out.
        if (background)
            _infoApi->callBackground(u"users.info"_s, params, std::move(onOk), std::move(onErr));
        else
            _api->call(u"users.info"_s, params, std::move(onOk), std::move(onErr));
        return rpl::lifetime();
    };
}

void PublicBackend::reconcileHuddleFromHistory(
    const ConversationId &conv, const QJsonArray &messages
) {
    // Find the newest huddle_thread message in this page (a busy channel can
    // hold several over time; only the latest reflects "now").
    QJsonObject newest;
    QString     newestTs;
    for (const auto &v : messages) {
        const auto m = v.toObject();
        if (m.value(u"subtype"_s).toString() != QLatin1String("huddle_thread"))
            continue;
        const auto ts = m.value(u"ts"_s).toString();
        if (newestTs.isEmpty() || ts.toDouble() > newestTs.toDouble()) {
            newest   = m;
            newestTs = ts;
        }
    }
    // Only act when the message carries a usable huddle `room`. If it's absent
    // (no huddle_thread in the window, or the token can't read `room`), stay
    // silent rather than emitting active=false and wiping a live huddle.
    const auto room = newest.value(u"room"_s).toObject();
    if (room.isEmpty() || room.value(u"call_family"_s).toString() != QLatin1String("huddle"))
        return;
    const auto h = JsonMappers::readHuddleRoom(room);
    _events.fire(EvHuddleChanged{conv, h.active, h.link, h.participants});
}

rpl::producer<MessagePage>
PublicBackend::loadHistory(ConversationId conv, std::optional<QString> cursor) {
    return [this, conv, cursor](auto consumer) mutable {
        QUrlQuery params;
        params.addQueryItem(u"channel"_s, conv.value);
        params.addQueryItem(u"limit"_s, u"50"_s);
        if (cursor)
            params.addQueryItem(u"cursor"_s, *cursor);

        const bool firstPage = !cursor;
        _historyApi->call(
            u"conversations.history"_s,
            params,
            [this, conv, firstPage, consumer](QJsonObject resp) mutable {
                const auto messages = resp.value(u"messages"_s).toArray();
                // Self-heal huddle state from the authoritative huddle_thread
                // message. Only on the newest page — older (scroll-up) pages may
                // hold a long-ended huddle that must not clobber a live one.
                if (firstPage)
                    reconcileHuddleFromHistory(conv, messages);
                MessagePage page;
                page.messages = JsonMappers::toMessages(messages);
                auto meta     = resp.value(u"response_metadata"_s).toObject();
                auto next     = meta.value(u"next_cursor"_s).toString();
                if (!next.isEmpty())
                    page.olderCursor = next;
                consumer.put_next(std::move(page));
                consumer.put_done();
            },
            [consumer](QString err) mutable {
                qWarning() << "loadHistory error:" << err;
                consumer.put_done();
            }
        );
        return rpl::lifetime();
    };
}

rpl::producer<MessagePage>
PublicBackend::loadThread(ConversationId conv, Ts root, std::optional<QString> cursor) {
    return [this, conv, root, cursor](auto consumer) mutable {
        QUrlQuery params;
        params.addQueryItem(u"channel"_s, conv.value);
        params.addQueryItem(u"ts"_s, root);
        params.addQueryItem(u"limit"_s, u"50"_s);
        if (cursor)
            params.addQueryItem(u"cursor"_s, *cursor);

        _historyApi->call(
            u"conversations.replies"_s,
            params,
            [consumer](QJsonObject resp) mutable {
                MessagePage page;
                // conversations.replies returns oldest-first; no reversal needed.
                page.messages = JsonMappers::toMessages(resp.value(u"messages"_s).toArray(), false);
                auto meta     = resp.value(u"response_metadata"_s).toObject();
                auto next     = meta.value(u"next_cursor"_s).toString();
                if (!next.isEmpty())
                    page.olderCursor = next;
                consumer.put_next(std::move(page));
                consumer.put_done();
            },
            [consumer](QString err) mutable {
                qWarning() << "loadThread error:" << err;
                consumer.put_done();
            }
        );
        return rpl::lifetime();
    };
}

rpl::producer<Message> PublicBackend::loadMessageAt(ConversationId conv, Ts ts) {
    return [this, conv, ts](auto consumer) mutable {
        if (conv.value.isEmpty() || ts.isEmpty()) {
            consumer.put_done();
            return rpl::lifetime();
        }
        // conversations.replies answers for any ts in the conversation: a plain
        // message and a thread root come back as messages[0] (limit 1 truncates
        // the rest of the thread), and a reply's own ts returns just that reply
        // — which conversations.history cannot do at all, it never lists
        // replies. Verified live against both shapes.
        QUrlQuery params;
        params.addQueryItem(u"channel"_s, conv.value);
        params.addQueryItem(u"ts"_s, ts);
        params.addQueryItem(u"limit"_s, u"1"_s);
        _infoApi->callBackground(
            u"conversations.replies"_s,
            params,
            [consumer, ts](QJsonObject resp) mutable {
                for (const auto v : resp.value(u"messages"_s).toArray()) {
                    auto m = JsonMappers::toMessage(v.toObject());
                    if (m.ts != ts)
                        continue; // a thread root came back instead of the reply
                    consumer.put_next(std::move(m));
                    break;
                }
                consumer.put_done();
            },
            // Gone, or not ours to read: complete without a value (see backend.h).
            [consumer](QString) mutable { consumer.put_done(); }
        );
        return rpl::lifetime();
    };
}

rpl::producer<ThreadsViewPage> PublicBackend::loadThreadsView(const QString &cursor) {
    return [this, cursor](auto consumer) mutable {
        // Unsupported: complete WITHOUT a value (same contract as
        // loadUnreadCounts — see backend.h). Session-token only.
        if (!_sessionAuth || _threadsViewUnavailable) {
            consumer.put_done();
            return rpl::lifetime();
        }
        QUrlQuery params;
        params.addQueryItem(u"limit"_s, u"10"_s);
        // Every subscribed thread; other modes ("important") would filter.
        params.addQueryItem(u"priority_mode"_s, u"all"_s);
        // Continuation is the previous response's max_ts (no next_cursor here).
        if (!cursor.isEmpty())
            params.addQueryItem(u"current_ts"_s, cursor);
        _historyApi->call(
            u"subscriptions.thread.getView"_s,
            params,
            [consumer](QJsonObject resp) mutable {
                consumer.put_next(JsonMappers::toThreadsViewPage(resp));
                consumer.put_done();
            },
            [this, consumer](QString err) mutable {
                // Latch only on a method-level rejection, never on a transport
                // failure — same reasoning as client.counts above.
                if (isMethodUnavailable(err)) {
                    _threadsViewUnavailable = true;
                    qWarning() << "subscriptions.thread.getView rejected:" << err
                               << "— Threads overview disabled for this workspace";
                }
                consumer.put_done();
            },
            /*quietErrors=*/true
        );
        return rpl::lifetime();
    };
}

void PublicBackend::markThreadRead(ConversationId conv, Ts root, Ts ts) {
    if (!_sessionAuth || _threadsViewUnavailable)
        return;
    QUrlQuery params;
    params.addQueryItem(u"channel"_s, conv.value);
    params.addQueryItem(u"thread_ts"_s, root);
    params.addQueryItem(u"ts"_s, ts);
    // Best-effort write (a stale read cursor is harmless); non-idempotent lane
    // like every other write so a dying connection can't double-apply it.
    _api->callNonIdempotent(
        u"subscriptions.thread.mark"_s,
        params,
        [](QJsonObject) {},
        [](QString err) { qWarning() << "subscriptions.thread.mark error:" << err; }
    );
}

// ── Message reminders (internal saved.* API — "Later") ───────────────────────

rpl::producer<std::vector<MessageReminder>> PublicBackend::loadMessageReminders() {
    return [this](auto consumer) mutable {
        // Unsupported: complete WITHOUT a value (same contract as
        // loadUnreadCounts — see backend.h). Session-token only.
        if (!_sessionAuth || _savedUnavailable) {
            consumer.put_done();
            return rpl::lifetime();
        }
        QUrlQuery params;
        // One page is plenty: the list is the user's pending reminders, a
        // handful of items in practice. 50 is what Slack's own Later panel
        // requests — and the endpoint's ceiling is close by (200 answers
        // invalid_arguments; verified live).
        params.addQueryItem(u"limit"_s, u"50"_s);
        params.addQueryItem(u"filter"_s, u"saved"_s);
        _api->call(
            u"saved.list"_s,
            params,
            [consumer](QJsonObject resp) mutable {
                consumer.put_next(JsonMappers::toMessageReminders(resp));
                consumer.put_done();
            },
            [this, consumer](QString err) mutable {
                // Latch only on a method-level rejection, never on a transport
                // failure — same reasoning as client.counts above.
                if (isMethodUnavailable(err)) {
                    _savedUnavailable = true;
                    qWarning() << "saved.list rejected:" << err
                               << "— message reminders disabled for this workspace";
                }
                consumer.put_done();
            },
            /*quietErrors=*/true
        );
        return rpl::lifetime();
    };
}

void PublicBackend::setMessageReminder(
    ConversationId conv, Ts ts, qint64 dueAt, std::function<void(bool, QString)> done
) {
    if (!_sessionAuth || _savedUnavailable) {
        if (done)
            done(false, QStringLiteral("not_supported"));
        return;
    }
    QUrlQuery params;
    params.addQueryItem(u"item_type"_s, u"message"_s);
    params.addQueryItem(u"item_id"_s, conv.value);
    params.addQueryItem(u"ts"_s, ts);
    // No date_due = a plain "Save for later" bookmark (the server answers
    // date_due 0, todo_state "saved"; verified live).
    if (dueAt > 0)
        params.addQueryItem(u"date_due"_s, QString::number(dueAt));
    _api->callNonIdempotent(
        u"saved.add"_s,
        params,
        [done](QJsonObject) {
            if (done)
                done(true, {});
        },
        [this, done](QString err) {
            if (isMethodUnavailable(err))
                _savedUnavailable = true;
            qWarning() << "saved.add error:" << err;
            if (done)
                done(false, ambiguousOrReal(err));
        }
    );
}

void PublicBackend::removeMessageReminder(
    ConversationId conv, Ts ts, std::function<void(bool, QString)> done
) {
    if (!_sessionAuth || _savedUnavailable) {
        if (done)
            done(false, QStringLiteral("not_supported"));
        return;
    }
    QUrlQuery params;
    params.addQueryItem(u"item_type"_s, u"message"_s);
    params.addQueryItem(u"item_id"_s, conv.value);
    params.addQueryItem(u"ts"_s, ts);
    _api->callNonIdempotent(
        u"saved.delete"_s,
        params,
        [done](QJsonObject) {
            if (done)
                done(true, {});
        },
        [this, done](QString err) {
            if (isMethodUnavailable(err))
                _savedUnavailable = true;
            qWarning() << "saved.delete error:" << err;
            if (done)
                done(false, ambiguousOrReal(err));
        }
    );
}

// ── Self presence / status ────────────────────────────────────────

void PublicBackend::setPresence(bool away, std::function<void(bool, QString)> done) {
    QUrlQuery params;
    params.addQueryItem(u"presence"_s, away ? u"away"_s : u"auto"_s);
    _api->call(
        u"users.setPresence"_s,
        params,
        [done](QJsonObject) {
            if (done)
                done(true, {});
        },
        [done](QString e) {
            qWarning() << "setPresence error:" << e;
            if (done)
                done(false, e);
        }
    );
}

void PublicBackend::setPresenceMode(PresenceMode mode) {
    if (_rtmPresence)
        _rtmPresence->setMode(mode);
}

void PublicBackend::noteUserActivity() {
    if (_rtmPresence)
        _rtmPresence->noteActivity();
}

void PublicBackend::setStatus(
    const QString                     &emoji,
    const QString                     &text,
    qint64                             expirationTs,
    std::function<void(bool, QString)> done
) {
    const QJsonObject profile{
        {u"status_text"_s, text},
        {u"status_emoji"_s, emoji},
        {u"status_expiration"_s, expirationTs},
    };
    QUrlQuery params;
    params.addQueryItem(
        u"profile"_s, QString::fromUtf8(QJsonDocument(profile).toJson(QJsonDocument::Compact))
    );
    _api->call(
        u"users.profile.set"_s,
        params,
        [done](QJsonObject) {
            if (done)
                done(true, {});
        },
        [done](QString e) {
            qWarning() << "setStatus error:" << e;
            if (done)
                done(false, e);
        }
    );
}

void PublicBackend::setDndSnooze(int minutes, std::function<void(bool, QString)> done) {
    QUrlQuery params;
    if (minutes > 0)
        params.addQueryItem(u"num_minutes"_s, QString::number(minutes));
    _api->call(
        minutes > 0 ? u"dnd.setSnooze"_s : u"dnd.endSnooze"_s,
        params,
        [done](QJsonObject) {
            if (done)
                done(true, {});
        },
        [done](QString e) {
            qWarning() << "setDndSnooze error:" << e;
            if (done)
                done(false, e);
        }
    );
}

// ── Own profile ───────────────────────────────────────────────────

void PublicBackend::loadSidebarTheme(std::function<void(SidebarThemePrefs, QString)> done) {
    if (!_sessionAuth) { // undocumented client pref call: OAuth tokens get not_authed
        if (done)
            done({}, QStringLiteral("not_supported"));
        return;
    }
    _api->call(
        u"users.prefs.get"_s,
        QUrlQuery{},
        [done](QJsonObject resp) {
            const QJsonObject prefs = resp.value(u"prefs"_s).toObject();
            SidebarThemePrefs out;
            out.iaTheme       = prefs.value(u"ia_theme"_s).toString();
            // The legacy custom values arrive as a JSON object — usually
            // serialised into a string — keyed by slot; re-emit them as the
            // share string every importer understands, in Slack's slot order.
            QJsonValue legacy = prefs.value(u"sidebar_theme_custom_values"_s);
            if (legacy.isString())
                legacy = QJsonDocument::fromJson(legacy.toString().toUtf8()).object();
            if (legacy.isObject()) {
                const QJsonObject o = legacy.toObject();
                QStringList       parts;
                for (const char *key :
                     {"column_bg",
                      "menu_bg",
                      "active_item",
                      "active_item_text",
                      "hover_item",
                      "text_color",
                      "active_presence",
                      "badge"})
                    parts << o.value(QLatin1String(key)).toString();
                if (o.contains(u"top_nav_bg"_s) && o.contains(u"top_nav_text"_s))
                    parts << o.value(u"top_nav_bg"_s).toString()
                          << o.value(u"top_nav_text"_s).toString();
                if (!parts.contains(QString()))
                    out.legacyValues = parts.join(QLatin1Char(','));
            }
            if (done)
                done(out, {});
        },
        [done](QString e) {
            qWarning() << "users.prefs.get error:" << e;
            if (done)
                done({}, e);
        },
        /*quietErrors=*/true
    );
}

void PublicBackend::loadMembers(
    ConversationId conv, std::function<void(std::vector<UserId>, QString)> done
) {
    QUrlQuery params;
    params.addQueryItem(u"channel"_s, conv.value);
    params.addQueryItem(u"limit"_s, u"1000"_s);
    auto members = std::make_shared<std::vector<UserId>>();
    _api->paginate(
        u"conversations.members"_s,
        u"members"_s,
        params,
        [members](QJsonArray page) {
            for (const auto v : page)
                members->push_back(UserId{v.toString()});
        },
        [members, done] {
            if (done)
                done(std::move(*members), {});
        },
        [done](QString e) {
            qWarning() << "conversations.members error:" << e;
            if (done)
                done({}, e);
        }
    );
}

void PublicBackend::loadMyProfile(std::function<void(MyProfile)> done) {
    _api->call(
        u"users.profile.get"_s,
        QUrlQuery{},
        [done](QJsonObject resp) {
            const auto p = resp.value(u"profile"_s).toObject();
            MyProfile  mp;
            mp.realName    = p.value(u"real_name"_s).toString();
            mp.displayName = p.value(u"display_name"_s).toString();
            mp.email       = p.value(u"email"_s).toString();
            mp.phone       = p.value(u"phone"_s).toString();
            mp.avatarUrl   = p.value(u"image_512"_s).toString();
            if (mp.avatarUrl.isEmpty())
                mp.avatarUrl = p.value(u"image_192"_s).toString();
            if (done)
                done(mp);
        },
        [done](QString e) {
            qWarning() << "loadMyProfile error:" << e;
            if (done)
                done({});
        }
    );
}

void PublicBackend::updateProfile(
    const QHash<QString, QString> &fields, std::function<void(bool, QString)> done
) {
    QJsonObject profile;
    for (auto it = fields.constBegin(); it != fields.constEnd(); ++it)
        profile.insert(it.key(), it.value());
    QUrlQuery params;
    params.addQueryItem(
        u"profile"_s, QString::fromUtf8(QJsonDocument(profile).toJson(QJsonDocument::Compact))
    );
    _api->call(
        u"users.profile.set"_s,
        params,
        [done](QJsonObject) {
            if (done)
                done(true, {});
        },
        [done](QString e) {
            qWarning() << "updateProfile error:" << e;
            if (done)
                done(false, e);
        }
    );
}

void PublicBackend::setPhoto(
    const QString &filePath, std::function<void(bool, QString, QString)> done
) {
    auto *file = new QFile(filePath);
    if (!file->open(QIODevice::ReadOnly)) {
        qWarning() << "setPhoto: cannot open" << filePath;
        delete file;
        if (done)
            done(false, QStringLiteral("cannot_open_file"), {});
        return;
    }

    auto         *mp = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    QHttpPart     imagePart;
    const QString fileName = QFileInfo(filePath).fileName();
    imagePart.setHeader(
        QNetworkRequest::ContentDispositionHeader,
        QVariant(QStringLiteral("form-data; name=\"image\"; filename=\"%1\"").arg(fileName))
    );
    file->setParent(mp); // closed/destroyed with the multipart
    imagePart.setBodyDevice(file);
    mp->append(imagePart);

    _api->postMultipart(
        u"users.setPhoto"_s,
        mp,
        [done](QJsonObject resp) {
            const auto profile = resp.value(u"profile"_s).toObject();
            QString    url     = profile.value(u"image_512"_s).toString();
            if (url.isEmpty())
                url = profile.value(u"image_192"_s).toString();
            if (done)
                done(true, {}, url);
        },
        [done](QString e) {
            qWarning() << "setPhoto error:" << e;
            if (done)
                done(false, e, {});
        }
    );
}

// ── Slash commands ────────────────────────────────────────────────
// Both endpoints are undocumented official-client API (commands.list /
// chat.command) and may be rejected for OAuth tokens (missing legacy `post`
// scope). listCommands degrades to "produce nothing" — the Session merges in
// its built-in command set; runCommand reports the server error to the caller.

std::vector<SlashCommand> PublicBackend::nativeCommands() const {
    // The Slack-flavoured commands Session::runCommand executes natively. These are
    // Slack conventions (incl. /shrug, /mute), so they live here — not app-level —
    // and never appear in another service's composer.
    return CommonCommands::select(
        {"shrug", "mute", "active", "away", "dnd", "status", "msg", "dm", "leave"}
    );
}

rpl::producer<std::vector<SlashCommand>> PublicBackend::listCommands() {
    return [this](auto consumer) mutable {
        _api->call(
            u"commands.list"_s,
            {},
            [consumer](QJsonObject resp) mutable {
                consumer.put_next(JsonMappers::toSlashCommands(resp.value(u"commands"_s)));
                consumer.put_done();
            },
            [consumer](QString err) mutable {
                // not_allowed_token_type is the norm for OAuth tokens (see the
                // comment above) — built-ins take over, nothing to report.
                if (err != QLatin1String("not_allowed_token_type"))
                    qWarning() << "listCommands error:" << err;
                consumer.put_done();
            },
            /*quietErrors=*/true
        );
        return rpl::lifetime();
    };
}

void PublicBackend::runCommand(
    ConversationId                                conv,
    const QString                                &command,
    const QString                                &text,
    std::function<void(bool ok, QString message)> done
) {
    QUrlQuery params;
    params.addQueryItem(u"channel"_s, conv.value);
    params.addQueryItem(u"command"_s, command);
    if (!text.isEmpty())
        params.addQueryItem(u"text"_s, text);
    _api->callNonIdempotent(
        u"chat.command"_s,
        params,
        [done](QJsonObject resp) {
            if (done)
                done(true, resp.value(u"response"_s).toString());
        },
        [done](QString err) {
            qWarning() << "runCommand error:" << err;
            if (done)
                done(false, err);
        }
    );
}

// ── Commands ──────────────────────────────────────────────────────

// A chat.postMessage whose connection died mid-flight may or may not have
// reached Slack, and resending blindly is exactly what duplicates messages.
// Slack offers no idempotency key for API callers (client_msg_id is internal
// to first-party clients and ignored here), so the loop is:
//   post → ambiguous failure → wait with backoff → scan recent history for
//   the message (own author + same text, window anchored on the last server
//   ts seen before the send) → found: emit its echo / absent: post again.
// It repeats until delivered or Slack returns a definitive error, which
// surfaces as EvSendFailed.
struct PublicBackend::SendState {
    ConversationId  conv;
    OutgoingMessage msg;
    QString         wireText; // exactly what chat.postMessage was given
    QString         oldestTs; // exclusive lower bound of the reconcile scan
    int             attempts = 0;
    // Outcome callback (Backend::sendMessage); fired once, after the event.
    std::function<void(bool ok, QString err)> done;

    void finish(bool ok, const QString &err) {
        if (auto cb = std::exchange(done, nullptr))
            cb(ok, err);
    }
};

namespace {
// Slack entity-escapes bare & < > in stored message text; unescape both
// sides so a sent text compares equal to its stored form.
QString unescapedText(QString t) {
    t.replace(QLatin1String("&lt;"), QLatin1String("<"))
        .replace(QLatin1String("&gt;"), QLatin1String(">"))
        .replace(QLatin1String("&amp;"), QLatin1String("&"));
    return t.trimmed();
}

// Block Kit `blocks` argument — JSON in a form field. Omitted when empty so a
// plain mrkdwn message keeps its plain wire shape.
void addBlocks(QUrlQuery &params, const QJsonArray &blocks) {
    if (blocks.isEmpty())
        return;
    params.addQueryItem(
        u"blocks"_s, QString::fromUtf8(QJsonDocument(blocks).toJson(QJsonDocument::Compact))
    );
}

// A picked GIF, posted the way Slack's own GIF picker posts one: an attachment
// holding an image block titled "GIF". Nothing links it from the text, so there
// is no unfurl card to go with it (gifAttachment in domain.h is how it reads back).
void addGifAttachments(QUrlQuery &params, const std::vector<OutgoingGif> &gifs) {
    if (gifs.empty())
        return;
    QJsonArray attachments;
    for (const auto &gif : gifs) {
        const QJsonObject image{
            {u"type"_s, u"image"_s},
            {u"image_url"_s, gif.url},
            {u"alt_text"_s,
             gif.altText.isEmpty() ? QStringLiteral("GIF") : gif.altText}, // required
            {u"title"_s, QJsonObject{{u"type"_s, u"plain_text"_s}, {u"text"_s, u"GIF"_s}}},
        };
        attachments.append(
            QJsonObject{
                {u"fallback"_s, u"shared a GIF"_s},
                {u"blocks"_s, QJsonArray{image}},
            }
        );
    }
    params.addQueryItem(
        u"attachments"_s,
        QString::fromUtf8(QJsonDocument(attachments).toJson(QJsonDocument::Compact))
    );
}

// True when history message `o` carries every GIF of `gifs` — with the text
// check, how reconcileSend recognises a GIF post whose text is empty.
bool carriesGifs(const QJsonObject &o, const std::vector<OutgoingGif> &gifs) {
    QSet<QString> urls;
    for (const auto a : o.value(u"attachments"_s).toArray())
        for (const auto b : a.toObject().value(u"blocks"_s).toArray())
            urls.insert(b.toObject().value(u"image_url"_s).toString());
    return std::all_of(gifs.begin(), gifs.end(), [&](const OutgoingGif &g) {
        return urls.contains(g.url);
    });
}
} // namespace

void PublicBackend::sendMessage(
    ConversationId conv, OutgoingMessage msg, std::function<void(bool ok, QString err)> done
) {
    auto st      = std::make_shared<SendState>();
    st->conv     = conv;
    st->done     = std::move(done);
    st->wireText = msg.rawText.isEmpty() ? msg.text.text : msg.rawText;
    // Prefer the server-assigned anchor (immune to local clock skew); fall
    // back to the local clock minus a minute of slack for empty convs.
    st->oldestTs = msg.sinceTs.isEmpty() ? QString::number(QDateTime::currentSecsSinceEpoch() - 60)
                                         : msg.sinceTs;
    st->msg      = std::move(msg);
    postMessageAttempt(std::move(st));
}

void PublicBackend::postMessageAttempt(std::shared_ptr<SendState> st) {
    QUrlQuery params;
    params.addQueryItem(u"channel"_s, st->conv.value);
    params.addQueryItem(u"text"_s, st->wireText);
    addBlocks(params, st->msg.blocks);
    addGifAttachments(params, st->msg.gifs);
    if (st->msg.threadRoot)
        params.addQueryItem(u"thread_ts"_s, *st->msg.threadRoot);
    if (st->msg.threadRoot && st->msg.replyBroadcast)
        params.addQueryItem(u"reply_broadcast"_s, u"true"_s);
    _api->callNonIdempotent(
        u"chat.postMessage"_s,
        params,
        [this, st](QJsonObject resp) {
            // Confirm from the HTTP response instead of waiting for the
            // realtime echo — the websocket may be down while HTTP works.
            // Session drops the second copy when the echo arrives anyway.
            Message m = JsonMappers::toMessage(resp.value(u"message"_s).toObject());
            if (m.ts.isEmpty())
                m.ts = resp.value(u"ts"_s).toString();
            _events.fire(EvMessageNew{st->conv, std::move(m)});
            st->finish(true, {});
        },
        [this, st](QString err) {
            if (err == WebApiClient::kConnectionLost) {
                st->attempts++;
                const int delay = qMin(_sendRetryDelayMs << qMin(st->attempts - 1, 6), 60'000);
                qDebug() << "sendMessage: connection lost mid-flight, reconciling in" << delay
                         << "ms";
                QTimer::singleShot(delay, _api, [this, st] { reconcileSend(st); });
                return;
            }
            qWarning() << "sendMessage error:" << err;
            _events.fire(EvSendFailed{st->conv, err});
            st->finish(false, err);
        }
    );
}

void PublicBackend::reconcileSend(std::shared_ptr<SendState> st) {
    const bool inThread = st->msg.threadRoot.has_value();
    QUrlQuery  params;
    params.addQueryItem(u"channel"_s, st->conv.value);
    params.addQueryItem(u"oldest"_s, st->oldestTs);
    params.addQueryItem(u"limit"_s, u"100"_s);
    if (inThread)
        params.addQueryItem(u"ts"_s, *st->msg.threadRoot);
    _api->call(
        inThread ? u"conversations.replies"_s : u"conversations.history"_s,
        params,
        [this, st, inThread](QJsonObject resp) {
            const QString want = unescapedText(st->wireText);
            for (const auto v : resp.value(u"messages"_s).toArray()) {
                const auto o = v.toObject();
                if (inThread && o.value(u"ts"_s).toString() == *st->msg.threadRoot)
                    continue; // the thread root itself, not a reply
                if (!_meUserId.value.isEmpty() && o.value(u"user"_s).toString() != _meUserId.value)
                    continue;
                if (unescapedText(o.value(u"text"_s).toString()) != want ||
                    !carriesGifs(o, st->msg.gifs))
                    continue;
                qDebug() << "sendMessage: message" << o.value(u"ts"_s).toString()
                         << "was delivered after all — not resending";
                _events.fire(EvMessageNew{st->conv, JsonMappers::toMessage(o)});
                st->finish(true, {});
                return;
            }
            postMessageAttempt(st); // genuinely missing — safe to post again
        },
        [this, st](QString err) {
            // Transport failures retry inside WebApiClient and never reach
            // here; a Slack-level error means the conversation itself is
            // unusable (gone, kicked, …) — resending would fail the same way.
            qWarning() << "sendMessage reconcile error:" << err;
            _events.fire(EvSendFailed{st->conv, err});
            st->finish(false, err);
        }
    );
}

void PublicBackend::reconcileUpload(
    const ConversationId &conv,
    const QSet<QString>  &fileIds,
    std::optional<Ts>     threadRoot,
    int                   attempt
) {
    if (fileIds.isEmpty())
        return;
    const bool inThread = threadRoot.has_value();
    QUrlQuery  params;
    params.addQueryItem(u"channel"_s, conv.value);
    // The share lands at "now"; a small window absorbs clock skew and history
    // lag without trawling the whole channel.
    params.addQueryItem(u"oldest"_s, QString::number(QDateTime::currentSecsSinceEpoch() - 120));
    params.addQueryItem(u"limit"_s, u"30"_s);
    // A threaded share only surfaces in conversations.replies, never the channel
    // history — mirror reconcileSend and scan the thread when we have a root.
    if (inThread)
        params.addQueryItem(u"ts"_s, *threadRoot);
    _api->call(
        inThread ? u"conversations.replies"_s : u"conversations.history"_s,
        params,
        [this, conv, fileIds, threadRoot, attempt](QJsonObject resp) {
            for (const auto v : resp.value(u"messages"_s).toArray()) {
                const auto o = v.toObject();
                if (!_meUserId.value.isEmpty() && o.value(u"user"_s).toString() != _meUserId.value)
                    continue;
                bool match = false;
                for (const auto fv : o.value(u"files"_s).toArray()) {
                    if (fileIds.contains(fv.toObject().value(u"id"_s).toString())) {
                        match = true;
                        break;
                    }
                }
                if (!match)
                    continue;
                // Found the shared message — emit its echo. Session de-ghosts
                // the optimistic copy and dedups the later realtime echo by ts.
                _events.fire(EvMessageNew{conv, JsonMappers::toMessage(o)});
                return;
            }
            // Not yet visible — a heavy share routinely lags conversations.history
            // by a beat. Retry with backoff so de-ghosting doesn't hinge on the
            // realtime echo (which may be delayed or dropped). The later echo —
            // from a retry here or from the websocket — is deduped by ts in
            // Session, so an extra scan is harmless once one path succeeds.
            constexpr int kMaxUploadReconcileRetries = 6;
            if (attempt < kMaxUploadReconcileRetries) {
                const int delay = qMin(_sendRetryDelayMs << attempt, 60'000);
                QTimer::singleShot(delay, _api, [this, conv, fileIds, threadRoot, attempt] {
                    reconcileUpload(conv, fileIds, threadRoot, attempt + 1);
                });
            }
        },
        [conv](QString err) { qWarning() << "reconcileUpload error:" << conv.value << err; }
    );
}

void PublicBackend::editMessage(ConversationId conv, Ts ts, OutgoingMessage msg) {
    QUrlQuery params;
    params.addQueryItem(u"channel"_s, conv.value);
    params.addQueryItem(u"ts"_s, ts);
    params.addQueryItem(u"text"_s, msg.rawText.isEmpty() ? msg.text.text : msg.rawText);
    // Text without blocks makes chat.update DROP the message's blocks (documented),
    // which is right for a plain edit and why a list must travel again here.
    addBlocks(params, msg.blocks);
    // chat.update is idempotent, so a plain edit rides the GET path and gets
    // Qt's retransmit on a dropped connection for free. A blocks payload can
    // outgrow a query string (a long list, percent-encoded), so that one POSTs.
    const auto call = [&](auto &&onOk, auto &&onErr) {
        if (msg.blocks.isEmpty())
            _api->call(u"chat.update"_s, params, onOk, onErr);
        else
            _api->callNonIdempotent(u"chat.update"_s, params, onOk, onErr);
    };
    call(
        [this, conv, ts](QJsonObject resp) {
            // Confirm the edit from the response itself rather than waiting for
            // the realtime message_changed echo, which may never arrive on a
            // stalled/recycling socket (mirrors chat.delete above). The safety
            // poll can't recover edits either — an edited message keeps its ts,
            // so the "newer than last known" filter skips it. chat.update
            // returns only text/user in `message`, hence textOnly: the UI
            // merges the new text into the existing row; the realtime echo,
            // when it does arrive, carries the full message and replaces it.
            auto msg   = JsonMappers::toMessage(resp.value(u"message"_s).toObject());
            msg.ts     = resp.value(u"ts"_s).toString(ts);
            msg.edited = true;
            _events.fire(EvMessageChanged{conv, std::move(msg), /*textOnly=*/true});
        },
        [](QString e) { qWarning() << "editMessage error:" << e; }
    );
}

void PublicBackend::deleteMessage(ConversationId conv, Ts ts) {
    deleteMessageAttempt(conv, ts, 0);
}

void PublicBackend::deleteMessageAttempt(ConversationId conv, Ts ts, int attempts) {
    QUrlQuery params;
    params.addQueryItem(u"channel"_s, conv.value);
    params.addQueryItem(u"ts"_s, ts);
    // POST (non-idempotent): Qt silently retransmits a GET whose connection
    // died mid-flight, which would fire a second chat.delete that comes back
    // message_not_found. Deletion is naturally idempotent, but routing it as a
    // write method keeps it off the auto-retransmit path and consistent with
    // the other chat.* writes.
    _api->callNonIdempotent(
        u"chat.delete"_s,
        params,
        [this, conv, ts](QJsonObject) {
            // Confirm the deletion from the response itself rather than waiting
            // for the realtime message_deleted echo, which may never arrive if
            // the socket is recycling. Session dedups EvMessageDeleted, so the
            // later realtime frame collapses into this one harmlessly.
            _events.fire(EvMessageDeleted{conv, ts});
        },
        [this, conv, ts, attempts](QString e) {
            if (e == QLatin1String("message_not_found")) {
                // The message is already gone server-side (deleted elsewhere, or
                // a stale local copy such as an unreconciled optimistic send) —
                // the user's intent is satisfied either way, so drop it locally
                // as if the delete succeeded.
                _events.fire(EvMessageDeleted{conv, ts});
                return;
            }
            if (e == WebApiClient::kConnectionLost) {
                // Ambiguous mid-flight failure of a POST. The delete may or may
                // not have applied, but it is idempotent (a re-delete just yields
                // message_not_found, handled above), so resending is always safe.
                // Bounded backoff so a persistent outage doesn't loop forever.
                constexpr int kMaxDeleteRetries = 6;
                if (attempts < kMaxDeleteRetries) {
                    const int delay = qMin(_sendRetryDelayMs << attempts, 60'000);
                    QTimer::singleShot(delay, _api, [this, conv, ts, attempts] {
                        deleteMessageAttempt(conv, ts, attempts + 1);
                    });
                    return;
                }
            }
            qWarning() << "deleteMessage error:" << e;
        }
    );
}

void PublicBackend::deleteAttachment(
    ConversationId conv, Ts ts, int attachmentId, std::function<void(bool, QString)> done
) {
    // The official client's "Remove preview": the internal chat.deleteAttachment
    // (channel, ts, attachment = the 1-based positional id — Slack renumbers the
    // rest afterwards). Own messages only; anyone else's answers
    // cant_delete_message. A session (xoxc) token only, like the other internal
    // methods — capabilities() gates the UI accordingly. POST like every write:
    // an auto-retransmitted GET would address a renumbered attachment.
    QUrlQuery params;
    params.addQueryItem(u"channel"_s, conv.value);
    params.addQueryItem(u"ts"_s, ts);
    params.addQueryItem(u"attachment"_s, QString::number(attachmentId));
    _api->callNonIdempotent(
        u"chat.deleteAttachment"_s,
        params,
        [done](QJsonObject) {
            if (done)
                done(true, {});
        },
        [done](QString e) {
            // No transport retry: a mid-flight loss is ambiguous and a re-send
            // could hit a renumbered id. The user sees the failure and can click
            // again once the row has refreshed.
            qWarning() << "deleteAttachment error:" << e;
            if (done)
                done(false, e);
        }
    );
}

void PublicBackend::pressBotButton(
    ConversationId                     conv,
    Ts                                 ts,
    std::optional<Ts>                  threadTs,
    QString                            botId,
    BotButton                          button,
    std::function<void(bool, QString)> done
) {
    // The official client's button press: the internal blocks.actions, which
    // Slack relays to the app as a block_actions interaction (verified live —
    // service_id is the message's bot_id; an unknown one answers
    // invalid_service_id). Session (xoxc) token only; capabilities() gates the
    // UI. POST, never retried: a retransmitted press would run the bot's action
    // twice, and a lost one is simply clicked again.
    const QString nowTs = QString::number(QDateTime::currentMSecsSinceEpoch() / 1000.0, 'f', 6);
    QJsonObject   action{
        {u"action_id"_s, button.actionId},
        {u"block_id"_s, button.blockId},
        {u"type"_s, u"button"_s},
        {u"text"_s, QJsonObject{{u"type"_s, u"plain_text"_s}, {u"text"_s, button.text}}},
        {u"action_ts"_s, nowTs},
    };
    if (!button.value.isEmpty())
        action.insert(u"value"_s, button.value);
    if (!button.style.isEmpty())
        action.insert(u"style"_s, button.style);
    QJsonObject container{
        {u"type"_s, u"message"_s},
        {u"message_ts"_s, ts},
        {u"channel_id"_s, conv.value},
        {u"is_ephemeral"_s, false},
    };
    if (threadTs)
        container.insert(u"thread_ts"_s, *threadTs);

    QUrlQuery params;
    params.addQueryItem(u"service_id"_s, botId);
    params.addQueryItem(
        u"client_token"_s, u"msga-"_s + QString::number(QDateTime::currentMSecsSinceEpoch())
    );
    params.addQueryItem(
        u"actions"_s,
        QString::fromUtf8(QJsonDocument(QJsonArray{action}).toJson(QJsonDocument::Compact))
    );
    params.addQueryItem(
        u"container"_s, QString::fromUtf8(QJsonDocument(container).toJson(QJsonDocument::Compact))
    );
    _api->callNonIdempotent(
        u"blocks.actions"_s,
        params,
        [done](QJsonObject) {
            if (done)
                done(true, {});
        },
        [done](QString e) {
            qWarning() << "pressBotButton error:" << e;
            if (done)
                done(false, e);
        }
    );
}

void PublicBackend::deleteFile(const QString &fileId) {
    QUrlQuery params;
    params.addQueryItem(u"file"_s, fileId);
    _api->call(u"files.delete"_s, params, {}, [](QString e) {
        qWarning() << "deleteFile error:" << e;
    });
}

void PublicBackend::addReaction(ConversationId conv, Ts ts, QString emoji) {
    QUrlQuery params;
    params.addQueryItem(u"channel"_s, conv.value);
    params.addQueryItem(u"timestamp"_s, ts);
    params.addQueryItem(u"name"_s, emoji);
    _api->call(u"reactions.add"_s, params, {}, [](QString e) {
        qWarning() << "addReaction error:" << e;
    });
}

void PublicBackend::removeReaction(ConversationId conv, Ts ts, QString emoji) {
    QUrlQuery params;
    params.addQueryItem(u"channel"_s, conv.value);
    params.addQueryItem(u"timestamp"_s, ts);
    params.addQueryItem(u"name"_s, emoji);
    _api->call(u"reactions.remove"_s, params, {}, [](QString e) {
        qWarning() << "removeReaction error:" << e;
    });
}

void PublicBackend::markRead(ConversationId conv, Ts ts) {
    QUrlQuery params;
    params.addQueryItem(u"channel"_s, conv.value);
    params.addQueryItem(u"ts"_s, ts);
    _api->call(u"conversations.mark"_s, params, {}, [](QString e) {
        qWarning() << "markRead error:" << e;
    });
}

void PublicBackend::sendTyping(ConversationId) {
    // users.typing was removed from the Slack Web API; no-op.
}

void PublicBackend::scheduleMessage(ConversationId conv, OutgoingMessage msg, qint64 postAt) {
    QUrlQuery params;
    params.addQueryItem(u"channel"_s, conv.value);
    params.addQueryItem(u"text"_s, msg.rawText.isEmpty() ? msg.text.text : msg.rawText);
    addBlocks(params, msg.blocks);
    addGifAttachments(params, msg.gifs);
    params.addQueryItem(u"post_at"_s, QString::number(postAt));
    if (msg.threadRoot)
        params.addQueryItem(u"thread_ts"_s, *msg.threadRoot);
    _api->callNonIdempotent(u"chat.scheduleMessage"_s, params, {}, [](QString e) {
        qWarning() << "scheduleMessage error:" << e;
    });
}

void PublicBackend::pinMessage(ConversationId conv, Ts ts) {
    QUrlQuery params;
    params.addQueryItem(u"channel"_s, conv.value);
    params.addQueryItem(u"timestamp"_s, ts);
    _api->call(u"pins.add"_s, params, {}, [](QString e) {
        qWarning() << "pinMessage error:" << e;
    });
}

void PublicBackend::unpinMessage(ConversationId conv, Ts ts) {
    QUrlQuery params;
    params.addQueryItem(u"channel"_s, conv.value);
    params.addQueryItem(u"timestamp"_s, ts);
    _api->call(u"pins.remove"_s, params, {}, [](QString e) {
        qWarning() << "unpinMessage error:" << e;
    });
}

void PublicBackend::starConversation(ConversationId conv, bool star) {
    QUrlQuery params;
    params.addQueryItem(u"channel"_s, conv.value);
    _api->call(star ? u"stars.add"_s : u"stars.remove"_s, params, {}, [star](QString e) {
        qWarning() << (star ? "starConversation" : "unstarConversation") << "error:" << e;
    });
}

rpl::producer<std::vector<ConversationId>> PublicBackend::loadStarredConversations() {
    return [this](auto consumer) mutable {
        // Unsupported: complete WITHOUT a value (see backend.h) so the caller
        // keeps whatever star state it already has instead of wiping it.
        if (_starsUnavailable) {
            consumer.put_done();
            return rpl::lifetime();
        }
        auto acc = std::make_shared<std::vector<ConversationId>>();
        _api->paginate(
            u"stars.list"_s,
            u"items"_s,
            {},
            [acc](QJsonArray page) {
                auto ids = JsonMappers::toStarredConversationIds(page);
                acc->insert(acc->end(), ids.begin(), ids.end());
            },
            [acc, consumer]() mutable {
                consumer.put_next(std::move(*acc));
                consumer.put_done();
            },
            [this, consumer](QString err) mutable {
                // Latch only on a method-level rejection, never on a transport
                // failure — same reasoning as client.counts above. stars.list is
                // deprecated, so `method_deprecated` is a plausible future answer
                // and belongs in the same bucket as a missing `stars:read` scope.
                if (isMethodUnavailable(err)) {
                    _starsUnavailable = true;
                    qWarning() << "stars.list rejected:" << err
                               << "— starred conversations disabled for this workspace";
                }
                consumer.put_done();
            }
        );
        return rpl::lifetime();
    };
}

void PublicBackend::leaveConversation(ConversationId conv) {
    QUrlQuery params;
    params.addQueryItem(u"channel"_s, conv.value);
    _api->call(u"conversations.leave"_s, params, {}, [](QString e) {
        qWarning() << "leaveConversation error:" << e;
    });
}

void PublicBackend::createChannel(
    const QString                      &name,
    bool                                isPrivate,
    std::function<void(ConversationId)> onSuccess,
    std::function<void(QString)>        onError
) {
    QJsonObject body;
    body[u"name"_s]       = name;
    body[u"is_private"_s] = isPrivate;
    _api->postJson(
        u"conversations.create"_s,
        body,
        [onSuccess](QJsonObject resp) {
            const QString id = resp.value(u"channel"_s).toObject().value(u"id"_s).toString();
            if (!id.isEmpty() && onSuccess)
                onSuccess(ConversationId{id});
        },
        [onError](QString e) {
            qWarning() << "createChannel error:" << e;
            if (onError)
                onError(e);
        }
    );
}

void PublicBackend::joinChannel(
    ConversationId                      id,
    std::function<void(ConversationId)> onSuccess,
    std::function<void(QString)>        onError
) {
    QJsonObject body;
    body[u"channel"_s] = id.value;
    _api->postJson(
        u"conversations.join"_s,
        body,
        [id, onSuccess](QJsonObject) {
            if (onSuccess)
                onSuccess(id);
        },
        [onError](QString e) {
            qWarning() << "joinChannel error:" << e;
            if (onError)
                onError(e);
        }
    );
}

void PublicBackend::openDm(
    UserId user, std::function<void(ConversationId)> onSuccess, std::function<void(QString)> onError
) {
    QJsonObject body;
    body[u"users"_s] = user.value;
    _api->postJson(
        u"conversations.open"_s,
        body,
        [onSuccess](QJsonObject resp) {
            if (onSuccess)
                onSuccess(
                    ConversationId{resp.value(u"channel"_s).toObject().value(u"id"_s).toString()}
                );
        },
        [onError](QString e) {
            qWarning() << "openDm error:" << e;
            if (onError)
                onError(e);
        }
    );
}

void PublicBackend::subscribePresence(std::vector<UserId> userIds) {
    SocketModeRealtime *rt = _sharedRealtime;
    if (!rt)
        return;
    QStringList ids;
    ids.reserve(static_cast<qsizetype>(userIds.size()));
    for (const auto &u : userIds)
        ids.append(u.value);
    rt->subscribePresence(&_events, std::move(ids));
}

rpl::producer<Event> PublicBackend::events() const {
    return _events.events();
}

// ── Phase 3 ───────────────────────────────────────────────────────

rpl::producer<std::vector<SearchResult>> PublicBackend::searchMessages(const QString &query) {
    return [this, query](auto consumer) mutable {
        QUrlQuery params;
        params.addQueryItem(u"query"_s, query);
        params.addQueryItem(u"count"_s, u"20"_s);

        _api->call(
            u"search.messages"_s,
            params,
            [consumer](QJsonObject resp) mutable {
                auto msgs = resp.value(u"messages"_s).toObject().value(u"matches"_s).toArray();
                consumer.put_next(JsonMappers::toSearchResults(msgs));
                consumer.put_done();
            },
            [consumer](QString err) mutable {
                qWarning() << "searchMessages error:" << err;
                consumer.put_done();
            }
        );
        return rpl::lifetime();
    };
}

rpl::producer<QHash<QString, QString>> PublicBackend::loadEmojiList() {
    return [this](auto consumer) mutable {
        _api->call(
            u"emoji.list"_s,
            QUrlQuery{},
            [consumer](QJsonObject resp) mutable {
                QHash<QString, QString> map;
                const auto              emoji = resp.value(u"emoji"_s).toObject();
                for (auto it = emoji.begin(); it != emoji.end(); ++it)
                    map.insert(it.key(), it.value().toString());
                consumer.put_next(std::move(map));
                consumer.put_done();
            },
            [consumer](QString err) mutable {
                qWarning() << "loadEmojiList error:" << err;
                consumer.put_done();
            }
        );
        return rpl::lifetime();
    };
}

void PublicBackend::uploadFiles(
    ConversationId                              conv,
    const QStringList                          &filePaths,
    const QString                              &initialComment,
    std::optional<Ts>                           threadRoot,
    std::function<void(bool ok, QString error)> done
) {
    // Slack external upload flow: per file, files.getUploadURLExternal then a
    // raw POST of the bytes to the returned URL; once every file has settled,
    // ONE files.completeUploadExternal shares them all as a single message
    // with initial_comment as the message text.
    struct Batch {
        int        pending = 0;
        QJsonArray files; // {id, title} of successfully uploaded files
    };
    auto batch  = std::make_shared<Batch>();
    auto settle = std::make_shared<std::function<void(bool, QString)>>(std::move(done));

    auto finishOne = [this, conv, initialComment, threadRoot, batch, settle]() {
        if (--batch->pending > 0)
            return;
        if (batch->files.isEmpty()) {
            // Every upload failed; warnings already logged.
            if (*settle)
                (*settle)(false, QStringLiteral("file upload failed"));
            return;
        }
        QJsonObject body;
        body[u"channel_id"_s] = conv.value;
        body[u"files"_s]      = batch->files;
        if (!initialComment.isEmpty())
            body[u"initial_comment"_s] = initialComment;
        if (threadRoot)
            body[u"thread_ts"_s] = *threadRoot;

        // Collect the uploaded file ids so the post-upload reconcile can match
        // the shared message even when there's no initial_comment to compare.
        QSet<QString> fileIds;
        for (const auto v : std::as_const(batch->files))
            fileIds.insert(v.toObject().value(u"id"_s).toString());

        _api->postJson(
            u"files.completeUploadExternal"_s,
            body,
            [this, conv, fileIds, threadRoot, settle](QJsonObject) {
                // The upload landed but the response carries no message ts;
                // reconcile from history to emit the echo that de-ghosts the
                // optimistic copy without waiting on the realtime websocket.
                reconcileUpload(conv, fileIds, threadRoot);
                if (*settle)
                    (*settle)(true, {});
            },
            [settle](QString err) {
                qWarning() << "completeUploadExternal error:" << err;
                if (*settle)
                    (*settle)(false, err);
            }
        );
    };

    for (const QString &filePath : filePaths) {
        QFile f(filePath);
        if (!f.open(QIODevice::ReadOnly)) {
            qWarning() << "uploadFiles: cannot open" << filePath;
            continue;
        }
        const QByteArray data     = f.readAll();
        const QString    filename = QFileInfo(filePath).fileName();
        ++batch->pending;

        // Step 1: get upload URL (filename + length are the only request args)
        QUrlQuery params;
        params.addQueryItem(u"filename"_s, filename);
        params.addQueryItem(u"length"_s, QString::number(data.size()));

        _api->call(
            u"files.getUploadURLExternal"_s,
            params,
            [this, filename, data, finishOne, batch](QJsonObject resp) mutable {
                const QString uploadUrl = resp.value(u"upload_url"_s).toString();
                const QString fileId    = resp.value(u"file_id"_s).toString();

                // Step 2: POST bytes to the upload URL (no auth header)
                _api->rawPost(
                    QUrl(uploadUrl),
                    data,
                    [filename, fileId, finishOne, batch]() mutable {
                        QJsonObject entry;
                        entry[u"id"_s]    = fileId;
                        entry[u"title"_s] = filename;
                        batch->files.append(entry);
                        finishOne();
                    },
                    [finishOne](QString err) mutable {
                        qWarning() << "upload POST error:" << err;
                        finishOne();
                    }
                );
            },
            [finishOne](QString err) mutable {
                qWarning() << "getUploadURLExternal error:" << err;
                finishOne();
            }
        );
    }

    // No file could even be opened — finishOne will never run.
    if (batch->pending == 0 && *settle)
        (*settle)(false, QStringLiteral("could not read the selected files"));
}

void PublicBackend::downloadFile(
    const QString &url, std::function<void(QByteArray)> onData, std::function<void(QString)> onError
) {
    _api->downloadUrl(QUrl(url), std::move(onData), std::move(onError));
}

void PublicBackend::loadChannelCanvas(ConversationId id, std::function<void(QString, bool)> done) {
    QUrlQuery params;
    params.addQueryItem(u"channel"_s, id.value);
    _api->call(
        u"conversations.info"_s,
        params,
        [done](QJsonObject resp) {
            const auto [fileId, isEmpty] =
                JsonMappers::channelCanvas(resp.value(u"channel"_s).toObject());
            if (done)
                done(fileId, isEmpty);
        },
        [done, id](QString err) {
            qWarning() << "loadChannelCanvas error:" << id.value << err;
            if (done)
                done({}, true);
        }
    );
}

void PublicBackend::loadCanvasContent(
    const QString                    &fileId,
    std::function<void(QString html)> onHtml,
    std::function<void(QString)>      onError
) {
    // files.info → url_private → authed GET; canvases come back as HTML
    // (content-type text/html, <div class="quip-canvas-content">…).
    QUrlQuery params;
    params.addQueryItem(u"file"_s, fileId);
    _api->call(
        u"files.info"_s,
        params,
        [this, onHtml, onError](QJsonObject resp) {
            const QString url = resp.value(u"file"_s).toObject().value(u"url_private"_s).toString();
            if (url.isEmpty()) {
                if (onError)
                    onError(QStringLiteral("canvas has no url_private"));
                return;
            }
            _api->downloadUrl(
                QUrl(url),
                [onHtml](QByteArray data) {
                    if (onHtml)
                        onHtml(QString::fromUtf8(data));
                },
                onError
            );
        },
        [onError](QString err) {
            // not_visible is the routine "canvas owned elsewhere" case the caller
            // already turns into the read-only no-access notice; don't warn on it.
            if (err == QLatin1String("not_visible"))
                qDebug() << "loadCanvasContent:" << err << "(handled)";
            else
                qWarning() << "loadCanvasContent error:" << err;
            if (onError)
                onError(err);
        },
        /*quietErrors=*/true
    );
}

void PublicBackend::loadCanvasImage(
    const QString                  &fileId,
    std::function<void(QByteArray)> onData,
    std::function<void(QString)>    onError
) {
    // files.info → a sized thumbnail (preferred — the canvas renders inline
    // images downscaled) → authed GET. The relative blob URL in the HTML has
    // no host or token, so it can't be fetched directly.
    QUrlQuery params;
    params.addQueryItem(u"file"_s, fileId);
    _api->call(
        u"files.info"_s,
        params,
        [this, onData, onError](QJsonObject resp) {
            const QJsonObject f = resp.value(u"file"_s).toObject();
            QString           url;
            // Prefer the original (url_private): at HiDPI the column needs more
            // pixels than the 1024px thumbnail provides, so a thumb would look
            // upscaled. Thumbnails are only a fallback when there's no original.
            for (const char *key : {"url_private", "thumb_1024", "thumb_960", "thumb_800"}) {
                url = f.value(QLatin1String(key)).toString();
                if (!url.isEmpty())
                    break;
            }
            if (url.isEmpty()) {
                if (onError)
                    onError(QStringLiteral("canvas image has no url"));
                return;
            }
            _api->downloadUrl(
                QUrl(url),
                [onData](QByteArray data) {
                    if (onData)
                        onData(data);
                },
                onError
            );
        },
        [onError](QString err) {
            if (onError)
                onError(err);
        },
        /*quietErrors=*/true
    );
}

void PublicBackend::createChannelCanvas(
    ConversationId                      id,
    const QString                      &markdown,
    std::function<void(QString fileId)> onSuccess,
    std::function<void(QString)>        onError
) {
    QJsonObject body;
    body[u"channel_id"_s] = id.value;
    if (!markdown.isEmpty())
        body[u"document_content"_s] =
            QJsonObject{{u"type"_s, u"markdown"_s}, {u"markdown"_s, markdown}};
    _api->postJson(
        u"conversations.canvases.create"_s,
        body,
        [onSuccess](QJsonObject resp) {
            if (onSuccess)
                onSuccess(resp.value(u"canvas_id"_s).toString());
        },
        [onError](QString e) {
            qWarning() << "createChannelCanvas error:" << e;
            if (onError)
                onError(e);
        }
    );
}

void PublicBackend::loadCanvasMeta(
    const QString                                                               &fileId,
    std::function<void(QString title, QString permalink, CanvasMetaState state)> done
) {
    QUrlQuery params;
    params.addQueryItem(u"file"_s, fileId);
    _api->call(
        u"files.info"_s,
        params,
        [done](QJsonObject resp) {
            const auto file = resp.value(u"file"_s).toObject();
            if (done)
                done(
                    file.value(u"title"_s).toString(),
                    file.value(u"permalink"_s).toString(),
                    CanvasMetaState::Ok
                );
        },
        [done](QString err) {
            auto state = CanvasMetaState::Ok; // unknown error — assume still there
            if (err == QLatin1String("file_deleted") || err == QLatin1String("file_not_found"))
                state = CanvasMetaState::Gone;
            else if (err == QLatin1String("not_visible"))
                state = CanvasMetaState::NoAccess;
            // Gone/NoAccess are expected and handled by the caller (deleted-elsewhere
            // canvas, or a canvas owned by someone else — e.g. an app/bot DM canvas
            // or a channel the user hasn't joined). conversations.info advertises the
            // canvas file_id regardless of whether files.info will let the user view
            // it, so this is routine; only log genuinely unexpected errors.
            if (state == CanvasMetaState::Ok)
                qWarning() << "loadCanvasMeta error:" << err;
            else
                qDebug() << "loadCanvasMeta:" << err << "(handled)";
            if (done)
                done({}, {}, state);
        },
        /*quietErrors=*/true
    );
}

void PublicBackend::deleteCanvas(
    const QString &canvasId, std::function<void(bool ok, QString err)> done
) {
    QJsonObject body;
    body[u"canvas_id"_s] = canvasId;
    _api->postJson(
        u"canvases.delete"_s,
        body,
        [done](QJsonObject) {
            if (done)
                done(true, {});
        },
        [done](QString e) {
            qWarning() << "deleteCanvas error:" << e;
            if (done)
                done(false, e);
        }
    );
}

void PublicBackend::editCanvas(
    const QString                            &canvasId,
    const std::vector<CanvasChange>          &changes,
    std::function<void(bool ok, QString err)> done
) {
    if (changes.empty()) {
        if (done)
            done(true, {});
        return;
    }
    // canvases.edit rejects more than one item in "changes" (verified:
    // "[ERROR] no more than 1 items allowed") — send the ops one call each,
    // in order, aborting the sequence on the first failure.
    auto queue = std::make_shared<std::deque<CanvasChange>>(changes.begin(), changes.end());
    sendNextCanvasChange(canvasId, std::move(queue), std::move(done));
}

void PublicBackend::sendNextCanvasChange(
    const QString                            &canvasId,
    std::shared_ptr<std::deque<CanvasChange>> queue,
    std::function<void(bool ok, QString err)> done
) {
    const CanvasChange change = queue->front();
    queue->pop_front();

    QJsonObject body;
    body[u"canvas_id"_s] = canvasId;
    body[u"changes"_s]   = JsonMappers::toCanvasChanges({change});
    _api->postJson(
        u"canvases.edit"_s,
        body,
        [this, canvasId, queue = std::move(queue), done](QJsonObject) mutable {
            if (queue->empty()) {
                if (done)
                    done(true, {});
                return;
            }
            sendNextCanvasChange(canvasId, std::move(queue), std::move(done));
        },
        [done](QString e) {
            qWarning() << "editCanvas error:" << e;
            if (done)
                done(false, e);
        }
    );
}

} // namespace slack
