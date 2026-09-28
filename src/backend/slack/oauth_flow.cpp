// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "oauth_flow.h"

#include "network/oauth_pkce.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>

using namespace Qt::StringLiterals;

namespace slack {

OAuthFlow::OAuthFlow(AppConfig app, QObject *parent)
    : auth::AuthStrategy(parent), _app(std::move(app)) {}

QStringList OAuthFlow::userScopes() {
    // NOTE: tokens issued before a scope was added here lack it until the user
    // signs in to the workspace again; the affected calls fail with
    // missing_scope and the UI shows a re-auth hint.
    return {
        u"channels:history"_s, u"groups:history"_s,     u"im:history"_s,  u"mpim:history"_s,
        u"channels:read"_s,    u"groups:read"_s,        u"im:read"_s,     u"mpim:read"_s,
        u"users:read"_s,       u"team:read"_s,          u"emoji:read"_s,  u"reactions:read"_s,
        u"files:read"_s,       u"users.profile:read"_s, u"search:read"_s, "chat:write",
        "reactions:write",     "files:write",           "stars:read",     "stars:write",
        "channels:write",      "groups:write",          "mpim:write",     "im:write",
        "users:write",         "users.profile:write",   "dnd:write",      "dnd:read",
        "pins:write",          "canvases:read",         "canvases:write", "usergroups:read",
    };
}

void OAuthFlow::start() {
    if (_app.clientId.isEmpty()) {
        // Keys can be pasted at runtime (Settings → System → "Slack app keys"), so
        // lead with that — editing credentials.cmake only applies to your own build.
        emit failed(
            QCoreApplication::translate(
                "slack::OAuthFlow",
                "No Slack app keys are set up yet.\n\n"
                "Open Settings → System, choose “Slack app keys”, and paste your client ID, "
                "client secret and app token. Building msga yourself? Put them in "
                "credentials.cmake instead and rebuild."
            )
        );
        return;
    }

    const auto pkce = net::oauth::makePkce();
    _codeVerifier   = pkce.verifier;
    _state          = pkce.state;

    QUrl      url(u"https://slack.com/oauth/v2/authorize"_s);
    QUrlQuery q;
    q.addQueryItem(u"client_id"_s, _app.clientId);
    q.addQueryItem("user_scope", userScopes().join(','));
    q.addQueryItem(u"redirect_uri"_s, kOAuthRedirectUri);
    q.addQueryItem(u"state"_s, _state);
    q.addQueryItem(u"code_challenge"_s, QString::fromLatin1(pkce.challenge));
    q.addQueryItem(u"code_challenge_method"_s, u"S256"_s);
    url.setQuery(q);

    QDesktopServices::openUrl(url);
}

void OAuthFlow::handleCallbackUri(const QUrl &uri) {
    // Expected: msga://oauth/callback?code=…&state=…
    if (uri.scheme() != u"msga"_s || uri.host() != u"oauth"_s || uri.path() != u"/callback"_s)
        return;

    const auto cb = net::oauth::parseCallback(QUrlQuery(uri.query()), _state);
    if (!cb.error.isEmpty()) {
        emit failed(cb.error);
        return;
    }
    exchangeCode(cb.code);
}

void OAuthFlow::exchangeCode(const QString &code) {
    QUrlQuery params;
    params.addQueryItem(u"client_id"_s, _app.clientId);
    params.addQueryItem(u"client_secret"_s, _app.clientSecret);
    params.addQueryItem(u"code"_s, code);
    params.addQueryItem(u"redirect_uri"_s, kOAuthRedirectUri);
    params.addQueryItem(u"code_verifier"_s, _codeVerifier); // PKCE

    const QUrl tokenUrl(u"https://slack.com/api/oauth.v2.access"_s);
    net::oauth::postForm(this, tokenUrl, params, [this](QNetworkReply *reply) {
        if (reply->error() != QNetworkReply::NoError) {
            emit failed(reply->errorString());
            return;
        }
        auto obj = QJsonDocument::fromJson(reply->readAll()).object();
        if (!obj.value(u"ok"_s).toBool()) {
            emit failed(obj.value(u"error"_s).toString(u"unknown"_s));
            return;
        }
        auto         user      = obj.value(u"authed_user"_s).toObject();
        auto         team      = obj.value(u"team"_s).toObject();
        const qint64 expiresIn = user.value(u"expires_in"_s).toInteger(0);
        const qint64 expiresAt = expiresIn > 0 ? QDateTime::currentSecsSinceEpoch() + expiresIn : 0;
        fetchTeamInfo(
            user.value(u"access_token"_s).toString(),
            user.value(u"refresh_token"_s).toString(),
            expiresAt,
            team.value(u"id"_s).toString(),
            team.value(u"name"_s).toString()
        );
    });
}

void OAuthFlow::fetchTeamInfo(
    const QString &xoxp,
    const QString &refreshToken,
    qint64         expiresAt,
    const QString &teamId,
    const QString &teamName
) {
    auto           *nam = new QNetworkAccessManager(this);
    QNetworkRequest req(QUrl(u"https://slack.com/api/team.info"_s));
    req.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(xoxp).toUtf8());
    auto *reply = nam->get(req);
    connect(
        reply,
        &QNetworkReply::finished,
        this,
        [this, reply, nam, xoxp, refreshToken, expiresAt, teamId, teamName] {
            reply->deleteLater();
            nam->deleteLater();
            QString iconUrl;
            if (reply->error() == QNetworkReply::NoError) {
                auto root = QJsonDocument::fromJson(reply->readAll()).object();
                auto icon = root.value(u"team"_s).toObject().value(u"icon"_s).toObject();
                iconUrl   = icon.value(u"image_88"_s).toString();
            }
            // Encode the Slack token into the neutral record's opaque blob — the
            // UI above the seam never touches slack::Credentials.
            emit succeeded(
                slack::toRecord(
                    slack::Credentials{xoxp, teamId, teamName, iconUrl, refreshToken, expiresAt}
                )
            );
        }
    );
}

} // namespace slack
