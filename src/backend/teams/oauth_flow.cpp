// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "oauth_flow.h"

#include "network/oauth_pkce.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrlQuery>

using namespace Qt::StringLiterals;

// Same custom-scheme redirect the OS routes back to the app (registered verbatim
// in the Entra app's "Mobile and desktop applications" platform).
static constexpr const char *kOAuthRedirectUri = "msga://oauth/callback";

namespace teams {

OAuthFlow::OAuthFlow(AppConfig app, QObject *parent)
    : auth::AuthStrategy(parent), _app(std::move(app)) {}

QString OAuthFlow::authorityBase() {
    // "organizations" = any work/school (Entra) tenant; Teams has no consumer API,
    // so personal Microsoft accounts are excluded. The signed-in user picks their
    // account, so home-realm discovery routes to the right tenant.
    return QStringLiteral("https://login.microsoftonline.com/organizations");
}

QString OAuthFlow::scopes() {
    // Delegated Graph scopes (space-separated, v2 endpoint). Several require
    // tenant-admin consent under Microsoft's managed default policy — an
    // un-consented tenant surfaces an admin-consent error the UI explains.
    return QStringLiteral(
        "openid profile offline_access "
        "User.Read User.ReadBasic.All User.ReadWrite "
        "Team.ReadBasic.All Channel.ReadBasic.All "
        "Chat.Read Chat.ReadWrite "
        "ChannelMessage.Read.All ChannelMessage.Send "
        "Presence.Read.All Presence.ReadWrite Files.ReadWrite.All"
    );
}

void OAuthFlow::start() {
    if (_app.clientId.isEmpty()) {
        // The client ID can be pasted at runtime (Settings → System → "Microsoft
        // Teams"), so lead with that — editing credentials.cmake only applies to
        // your own build.
        emit failed(
            QCoreApplication::translate(
                "teams::OAuthFlow",
                "No Microsoft Teams app is set up yet.\n\n"
                "Open Settings → System, find “Microsoft Teams”, and paste your Entra app's "
                "client ID. Building msga yourself? Put it in credentials.cmake as "
                "MSGA_TEAMS_CLIENT_ID instead and rebuild."
            )
        );
        return;
    }

    const auto pkce = net::oauth::makePkce();
    _codeVerifier   = pkce.verifier;
    _state          = pkce.state;

    QUrl      url(authorityBase() + QStringLiteral("/oauth2/v2.0/authorize"));
    QUrlQuery q;
    q.addQueryItem(u"client_id"_s, _app.clientId);
    q.addQueryItem(u"response_type"_s, u"code"_s);
    q.addQueryItem(u"redirect_uri"_s, kOAuthRedirectUri);
    q.addQueryItem(u"response_mode"_s, u"query"_s);
    q.addQueryItem(u"scope"_s, scopes());
    q.addQueryItem(u"state"_s, _state);
    q.addQueryItem(u"code_challenge"_s, QString::fromLatin1(pkce.challenge));
    q.addQueryItem(u"code_challenge_method"_s, u"S256"_s);
    q.addQueryItem(u"prompt"_s, u"select_account"_s);
    url.setQuery(q);

    QDesktopServices::openUrl(url);
}

void OAuthFlow::handleCallbackUri(const QUrl &uri) {
    // Expected: msga://oauth/callback?code=…&state=…
    if (uri.scheme() != u"msga"_s || uri.host() != u"oauth"_s || uri.path() != u"/callback"_s)
        return;

    const auto cb = net::oauth::parseCallback(QUrlQuery(uri.query()), _state);
    if (!cb.error.isEmpty()) {
        // Surface the admin-consent case clearly; Entra returns error=
        // access_denied / consent_required with a description.
        emit failed(cb.errorDescription.isEmpty() ? cb.error : cb.errorDescription);
        return;
    }
    exchangeCode(cb.code);
}

void OAuthFlow::exchangeCode(const QString &code) {
    QUrlQuery params;
    params.addQueryItem(u"client_id"_s, _app.clientId);
    params.addQueryItem(u"grant_type"_s, u"authorization_code"_s);
    params.addQueryItem(u"code"_s, code);
    params.addQueryItem(u"redirect_uri"_s, kOAuthRedirectUri);
    params.addQueryItem(
        u"code_verifier"_s, _codeVerifier
    ); // PKCE (no client_secret — public client)
    params.addQueryItem(u"scope"_s, scopes());

    const QUrl tokenUrl(authorityBase() + QStringLiteral("/oauth2/v2.0/token"));
    net::oauth::postForm(this, tokenUrl, params, [this](QNetworkReply *reply) {
        const auto obj = QJsonDocument::fromJson(reply->readAll()).object();
        if (obj.contains(u"error"_s)) {
            const QString desc = obj.value(u"error_description"_s).toString();
            emit failed(desc.isEmpty() ? obj.value(u"error"_s).toString(u"unknown"_s) : desc);
            return;
        }
        const qint64 expiresIn = obj.value(u"expires_in"_s).toInteger(0);
        const qint64 expiresAt = expiresIn > 0 ? QDateTime::currentSecsSinceEpoch() + expiresIn : 0;
        finish(
            obj.value(u"access_token"_s).toString(),
            obj.value(u"refresh_token"_s).toString(),
            expiresAt,
            obj.value(u"id_token"_s).toString()
        );
    });
}

namespace {
// Decode a JWT payload (the middle segment) without verifying the signature —
// we only read non-sensitive claims (tid, oid) from a token Microsoft just
// issued to us over TLS.
QJsonObject jwtPayload(const QString &jwt) {
    const auto parts = jwt.split(QLatin1Char('.'));
    if (parts.size() < 2)
        return {};
    const auto payload = QByteArray::fromBase64(parts[1].toLatin1(), QByteArray::Base64UrlEncoding);
    return QJsonDocument::fromJson(payload).object();
}
} // namespace

void OAuthFlow::finish(
    const QString &accessToken,
    const QString &refreshToken,
    qint64         expiresAt,
    const QString &idToken
) {
    const auto    claims   = jwtPayload(idToken);
    const QString tenantId = claims.value(u"tid"_s).toString();
    const QString userId   = claims.value(u"oid"_s).toString();
    // Fallback workspace name from the UPN domain; replaced by the org's real
    // displayName once /organization returns.
    const QString upn      = claims.value(u"preferred_username"_s).toString();
    const QString domain   = upn.contains('@') ? upn.section('@', 1) : upn;

    // Look up the organization's display name (and skip icon for now — the org
    // photo needs a separate, optional call). Falls back to the UPN domain.
    auto           *nam = new QNetworkAccessManager(this);
    QNetworkRequest req(QUrl(QStringLiteral("https://graph.microsoft.com/v1.0/organization")));
    req.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(accessToken).toUtf8());
    auto *reply = nam->get(req);
    connect(
        reply,
        &QNetworkReply::finished,
        this,
        [this, reply, nam, accessToken, refreshToken, expiresAt, tenantId, userId, domain] {
            reply->deleteLater();
            nam->deleteLater();
            QString orgName = domain;
            if (reply->error() == QNetworkReply::NoError) {
                const auto arr =
                    QJsonDocument::fromJson(reply->readAll()).object().value(u"value"_s).toArray();
                if (!arr.isEmpty()) {
                    const auto name = arr.first().toObject().value(u"displayName"_s).toString();
                    if (!name.isEmpty())
                        orgName = name;
                }
            }
            emit succeeded(
                teams::toRecord(
                    teams::Credentials{
                        accessToken,
                        tenantId,
                        orgName,
                        /*iconUrl*/ {},
                        refreshToken,
                        expiresAt,
                        userId
                    }
                )
            );
        }
    );
}

} // namespace teams
