// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "backend/imap/imap_providers.h"

#include <QCryptographicHash>
#include <QSet>

using namespace Qt::StringLiterals;

namespace imap {

ProviderInfo providerForMxHost(const QString &mxHost) {
    const QString h = mxHost.trimmed().toLower();
    ProviderInfo  p;
    auto          set = [&](const QString &name,
                            const QString &imap,
                            const QString &smtp,
                            AuthMethod     auth,
                            const QString &help) {
        p.name               = name;
        p.imapHost           = imap;
        p.smtpHost           = smtp;
        p.auth               = auth;
        p.appPasswordHelpUrl = help;
        p.known              = true;
    };
    if (h.contains(u"google"_s) || h.contains(u"googlemail"_s))
        set(u"Gmail"_s,
            u"imap.gmail.com"_s,
            u"smtp.gmail.com"_s,
            AuthMethod::OAuthGoogle,
            u"https://myaccount.google.com/apppasswords"_s);
    else if (h.contains(u"outlook"_s) || h.contains(u"office365"_s) || h.contains(u"microsoft"_s))
        set(u"Outlook"_s,
            u"outlook.office365.com"_s,
            u"smtp.office365.com"_s,
            AuthMethod::OAuthMicrosoft,
            u"https://account.live.com/proofs/AppPassword"_s);
    else if (h.contains(u"messagingengine"_s) || h.contains(u"fastmail"_s))
        set(u"Fastmail"_s,
            u"imap.fastmail.com"_s,
            u"smtp.fastmail.com"_s,
            AuthMethod::Password,
            u"https://www.fastmail.help/hc/en-us/articles/360058752854"_s);
    else if (h.contains(u"yahoodns"_s) || h.contains(u"yahoo"_s))
        set(u"Yahoo"_s,
            u"imap.mail.yahoo.com"_s,
            u"smtp.mail.yahoo.com"_s,
            AuthMethod::Password,
            u"https://help.yahoo.com/kb/SLN15241.html"_s);
    else if (h.contains(u"zoho"_s))
        set(u"Zoho"_s, u"imap.zoho.com"_s, u"smtp.zoho.com"_s, AuthMethod::Password, {});
    else if (h.contains(u"icloud"_s) || h.contains(u"me.com"_s) || h.contains(u"apple"_s))
        set(u"iCloud"_s,
            u"imap.mail.me.com"_s,
            u"smtp.mail.me.com"_s,
            AuthMethod::Password,
            u"https://support.apple.com/en-us/102654"_s);
    return p; // known=false if no branch matched
}

bool isFreemailDomain(const QString &domain) {
    // Exact consumer-provider domains. Not exhaustive — a miss only means a
    // freemail user *might* get the provider's icon; the prefix families below
    // catch the big multi-TLD ones.
    static const QSet<QString> kExact = {
        QStringLiteral("gmail.com"),       QStringLiteral("googlemail.com"),
        QStringLiteral("msn.com"),         QStringLiteral("aol.com"),
        QStringLiteral("icloud.com"),      QStringLiteral("me.com"),
        QStringLiteral("mac.com"),         QStringLiteral("proton.me"),
        QStringLiteral("protonmail.com"),  QStringLiteral("pm.me"),
        QStringLiteral("web.de"),          QStringLiteral("t-online.de"),
        QStringLiteral("mail.com"),        QStringLiteral("email.com"),
        QStringLiteral("mail.ru"),         QStringLiteral("inbox.ru"),
        QStringLiteral("list.ru"),         QStringLiteral("bk.ru"),
        QStringLiteral("yandex.ru"),       QStringLiteral("yandex.com"),
        QStringLiteral("ya.ru"),           QStringLiteral("zoho.com"),
        QStringLiteral("zohomail.com"),    QStringLiteral("fastmail.com"),
        QStringLiteral("fastmail.fm"),     QStringLiteral("hey.com"),
        QStringLiteral("tutanota.com"),    QStringLiteral("tutanota.de"),
        QStringLiteral("tuta.com"),        QStringLiteral("tuta.io"),
        QStringLiteral("qq.com"),          QStringLiteral("163.com"),
        QStringLiteral("126.com"),         QStringLiteral("sina.com"),
        QStringLiteral("naver.com"),       QStringLiteral("daum.net"),
        QStringLiteral("hanmail.net"),     QStringLiteral("seznam.cz"),
        QStringLiteral("wp.pl"),           QStringLiteral("o2.pl"),
        QStringLiteral("interia.pl"),      QStringLiteral("onet.pl"),
        QStringLiteral("libero.it"),       QStringLiteral("virgilio.it"),
        QStringLiteral("orange.fr"),       QStringLiteral("wanadoo.fr"),
        QStringLiteral("free.fr"),         QStringLiteral("laposte.net"),
        QStringLiteral("sfr.fr"),          QStringLiteral("comcast.net"),
        QStringLiteral("verizon.net"),     QStringLiteral("att.net"),
        QStringLiteral("sbcglobal.net"),   QStringLiteral("cox.net"),
        QStringLiteral("earthlink.net"),   QStringLiteral("btinternet.com"),
        QStringLiteral("sky.com"),         QStringLiteral("talktalk.net"),
        QStringLiteral("telia.com"),       QStringLiteral("comhem.se"),
        QStringLiteral("bredband2.com"),   QStringLiteral("spray.se"),
        QStringLiteral("rediffmail.com"),  QStringLiteral("rocketmail.com"),
        QStringLiteral("ymail.com"),       QStringLiteral("ziggo.nl"),
        QStringLiteral("xs4all.nl"),       QStringLiteral("bluewin.ch"),
        QStringLiteral("shaw.ca"),         QStringLiteral("rogers.com"),
        QStringLiteral("sympatico.ca"),    QStringLiteral("bigpond.com"),
        QStringLiteral("optusnet.com.au"),
    };
    // Provider families spanning many country TLDs (yahoo.co.jp, hotmail.co.uk,
    // outlook.de, live.se, gmx.net/.de/.at, …).
    static const char *kFamilies[] = {"yahoo.", "hotmail.", "outlook.", "live.", "gmx."};

    QString d = domain.trimmed().toLower();
    while (d.contains(QLatin1Char('.'))) { // hop: mail.yahoo.com → yahoo.com → stop
        if (kExact.contains(d))
            return true;
        for (const char *fam : kFamilies)
            if (d.startsWith(QLatin1String(fam)))
                return true;
        d = d.mid(d.indexOf(QLatin1Char('.')) + 1);
    }
    return false;
}

QString gravatarUrl(const QString &email, int size) {
    const QByteArray hash =
        QCryptographicHash::hash(email.trimmed().toLower().toUtf8(), QCryptographicHash::Md5)
            .toHex();
    return QStringLiteral("https://www.gravatar.com/avatar/%1?s=%2&d=404")
        .arg(QString::fromLatin1(hash))
        .arg(size);
}

ProviderInfo detectProvider(const QString &email) {
    const int     at     = email.indexOf('@');
    const QString domain = (at >= 0 ? email.mid(at + 1) : email).trimmed().toLower();

    ProviderInfo p;
    auto         set = [&](const QString &name,
                           const QString &imap,
                           const QString &smtp,
                           AuthMethod     auth,
                           const QString &help) {
        p.name               = name;
        p.imapHost           = imap;
        p.smtpHost           = smtp;
        p.auth               = auth;
        p.appPasswordHelpUrl = help;
        p.known              = true;
    };

    if (domain == u"gmail.com"_s || domain == u"googlemail.com"_s) {
        set(u"Gmail"_s,
            u"imap.gmail.com"_s,
            u"smtp.gmail.com"_s,
            AuthMethod::OAuthGoogle,
            u"https://myaccount.google.com/apppasswords"_s);
    } else if (
        domain == u"outlook.com"_s || domain == u"hotmail.com"_s || domain == u"live.com"_s ||
        domain == u"msn.com"_s
    ) {
        set(u"Outlook"_s,
            u"outlook.office365.com"_s,
            u"smtp.office365.com"_s,
            AuthMethod::OAuthMicrosoft,
            u"https://account.live.com/proofs/AppPassword"_s);
    } else if (domain == u"icloud.com"_s || domain == u"me.com"_s || domain == u"mac.com"_s) {
        set(u"iCloud"_s,
            u"imap.mail.me.com"_s,
            u"smtp.mail.me.com"_s,
            AuthMethod::Password,
            u"https://support.apple.com/en-us/102654"_s);
    } else if (domain == u"fastmail.com"_s || domain == u"fastmail.fm"_s) {
        set(u"Fastmail"_s,
            u"imap.fastmail.com"_s,
            u"smtp.fastmail.com"_s,
            AuthMethod::Password,
            u"https://www.fastmail.help/hc/en-us/articles/360058752854"_s);
    } else if (domain == u"yahoo.com"_s || domain == u"ymail.com"_s) {
        set(u"Yahoo"_s,
            u"imap.mail.yahoo.com"_s,
            u"smtp.mail.yahoo.com"_s,
            AuthMethod::Password,
            u"https://help.yahoo.com/kb/SLN15241.html"_s);
    } else if (domain == u"gmx.com"_s || domain == u"gmx.net"_s) {
        set(u"GMX"_s, u"imap.gmx.com"_s, u"mail.gmx.com"_s, AuthMethod::Password, {});
    } else {
        // Unknown domain — guess the conventional hosts; the user can edit them.
        p.name     = domain.isEmpty() ? QStringLiteral("Email") : domain;
        p.imapHost = QStringLiteral("imap.") + domain;
        p.smtpHost = QStringLiteral("smtp.") + domain;
        p.auth     = AuthMethod::Password;
        p.known    = false;
    }
    return p;
}

} // namespace imap
