// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "time_format.h"
#include <QSettings>

using namespace Qt::StringLiterals;

namespace TimeFmt {
namespace {

struct Prefs {
    bool    loaded = false;
    bool    use24h = false;
    QString language; // "system" | "en" | "ja"
};

Prefs g_prefs;

QLocale resolveLocale(const QString &lang) {
    if (lang == u"en"_s)
        return QLocale(QLocale::English, QLocale::UnitedStates);
    if (lang == u"ja"_s)
        return QLocale(QLocale::Japanese, QLocale::Japan);
    return QLocale::system();
}

void ensureLoaded() {
    if (g_prefs.loaded)
        return;
    QSettings s(u"msga"_s, u"msga"_s);
    g_prefs.language          = s.value("appearance/language", u"system"_s).toString();
    // Japanese convention is 24-hour time; English-speaking locales 12-hour.
    const bool    ja24Default = resolveLocale(g_prefs.language).language() == QLocale::Japanese;
    const QString fmt =
        s.value("appearance/timeFormat", ja24Default ? u"24h"_s : u"12h"_s).toString();
    g_prefs.use24h = (fmt == u"24h"_s);
    g_prefs.loaded = true;
}

bool isJa() {
    return locale().language() == QLocale::Japanese;
}

// Per-locale display patterns. Japanese puts the day-period marker before the
// time (午後2:34) and orders dates year→month→day with counter suffixes.
QString timePattern() {
    // 24-hour convention uses a two-digit hour (09:05); 12-hour omits the
    // leading zero (9:05 AM).
    if (use24h())
        return u"HH:mm"_s;
    return isJa() ? u"APh:mm"_s : u"h:mm AP"_s;
}

QString datePattern(bool withYear) {
    if (isJa())
        return withYear ? u"yyyy年M月d日"_s : u"M月d日"_s;
    return withYear ? u"MMMM d, yyyy"_s : u"MMMM d"_s;
}

QString shortDatePattern() {
    return isJa() ? u"M月d日"_s : u"MMM d"_s;
}

} // namespace

bool use24h() {
    ensureLoaded();
    return g_prefs.use24h;
}

void setUse24h(bool on) {
    ensureLoaded();
    g_prefs.use24h = on;
    QSettings(u"msga"_s, u"msga"_s).setValue("appearance/timeFormat", on ? u"24h"_s : u"12h"_s);
}

QString language() {
    ensureLoaded();
    return g_prefs.language;
}

void setLanguage(const QString &lang) {
    ensureLoaded();
    g_prefs.language = lang;
    QSettings(u"msga"_s, u"msga"_s).setValue("appearance/language", lang);
}

QLocale locale() {
    ensureLoaded();
    return resolveLocale(g_prefs.language);
}

void reload() {
    g_prefs.loaded = false;
    ensureLoaded();
}

QString formatTime(const QDateTime &dt) {
    return locale().toString(dt, timePattern());
}

QString formatTime(qint64 unixSecs) {
    return formatTime(QDateTime::fromSecsSinceEpoch(unixSecs));
}

QString formatDate(const QDate &date) {
    const bool withYear = date.year() != QDate::currentDate().year();
    return locale().toString(date, datePattern(withYear));
}

QString formatDateTime(qint64 unixSecs) {
    const QDateTime dt = QDateTime::fromSecsSinceEpoch(unixSecs);
    return locale().toString(dt, shortDatePattern() + (isJa() ? u" "_s : u", "_s) + timePattern());
}

QString editFormat() {
    if (isJa())
        return u"yyyy年M月d日 "_s + timePattern();
    return u"MMM d, yyyy "_s + timePattern();
}

} // namespace TimeFmt
