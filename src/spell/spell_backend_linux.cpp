// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "spell/spell_backend.h"
#include "spell/spell_codepages.h"

#include <hunspell.hxx>

#include <QChar>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLocale>
#include <QMap>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTextStream>

#include <cstring>
#include <vector>

using namespace Qt::StringLiterals;

// Unicode case and letter lookups for the trimmed Hunspell (HUNSPELL_QT_UNICODE
// in third_party/hunspell/csutil.cxx): Qt's tables are in the binary already,
// Hunspell's own utf_tbl would add 384 KB. Hunspell asks about UTF-16 units.
unsigned short hunspell_qt_toupper(unsigned short c) {
    return static_cast<unsigned short>(QChar::toUpper(char32_t(c)));
}
unsigned short hunspell_qt_tolower(unsigned short c) {
    return static_cast<unsigned short>(QChar::toLower(char32_t(c)));
}
int hunspell_qt_isalpha(unsigned short c) {
    return QChar::isLetter(char32_t(c)) ? 1 : 0;
}

namespace Spell {
namespace {

// Linux: Hunspell over the dictionaries the system's packages installed. The
// words someone adds live in msga's own list (userWordsPath), one per line,
// and are added to every dictionary on load.

struct DictionaryFiles {
    QString aff;
    QString dic;
};

// Hunspell's own search order, less the office-suite folders: $DICPATH, the
// user's data dir, then the system's.
QStringList dictionaryDirs() {
    QStringList dirs = qEnvironmentVariable("DICPATH").split(u':', Qt::SkipEmptyParts);
    dirs << QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + u"/hunspell"_s
         << u"/usr/local/share/hunspell"_s << u"/usr/share/hunspell"_s << u"/usr/share/myspell"_s
         << u"/usr/share/myspell/dicts"_s;
    return dirs;
}

// Language code ("en_US", the files' base name) → files; the first directory
// that has a language wins. A .dic without its .aff (hyphenation patterns,
// thesauri) is no spelling dictionary.
QMap<QString, DictionaryFiles> findDictionaries() {
    QMap<QString, DictionaryFiles> found;
    for (const QString &dir : dictionaryDirs()) {
        const QFileInfoList dics =
            QDir(dir).entryInfoList({u"*.dic"_s}, QDir::Files | QDir::Readable, QDir::Name);
        for (const QFileInfo &dic : dics) {
            const QString code = dic.completeBaseName();
            if (found.contains(code) || code.startsWith(u"hyph_"_s))
                continue;
            const QString aff = dic.absolutePath() + u'/' + code + u".aff"_s;
            if (QFileInfo(aff).isReadable())
                found.insert(code, {aff, dic.absoluteFilePath()});
        }
    }
    return found;
}

QString userWordsPath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
           u"/spelling/words.txt"_s;
}

// The character set a dictionary declares (its .aff SET line, which Hunspell
// reports) — words go in and suggestions come out in it. Hunspell treats a set
// it doesn't know as ISO-8859-1; so does this.
class Charset {
public:
    explicit Charset(const std::string &set) {
        QByteArray norm;
        for (const char c : set)
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
                norm += c;
            else if (c >= 'A' && c <= 'Z')
                norm += char(c - 'A' + 'a');
        if (norm == "utf8") {
            _kind = Kind::Utf8;
            return;
        }
        for (const auto &a : kCodepageAliases)
            if (norm == a.alias)
                norm = a.name;
        for (const auto &cp : kCodepages) {
            if (norm == cp.name) {
                _kind = Kind::Table;
                _high = cp.high;
                for (int i = 0; i < 128; ++i)
                    if (cp.high[i] != 0xFFFD)
                        _reverse.insert(cp.high[i], char(0x80 + i));
                return;
            }
        }
    }

    // False when the word has a character this set can't hold — then the
    // dictionary can't have the word either.
    bool encode(const QString &word, std::string &out) const {
        if (_kind == Kind::Utf8) {
            out = word.toStdString();
            return true;
        }
        out.clear();
        out.reserve(word.size());
        for (const QChar c : word) {
            const char16_t u = c.unicode();
            if (u < 0x80 || (_kind == Kind::Latin1 && u < 0x100)) {
                out += char(u);
                continue;
            }
            const auto it = _reverse.constFind(u);
            if (_kind == Kind::Latin1 || it == _reverse.cend())
                return false;
            out += *it;
        }
        return true;
    }

    QString decode(const std::string &s) const {
        if (_kind == Kind::Utf8)
            return QString::fromStdString(s);
        if (_kind == Kind::Latin1)
            return QString::fromLatin1(s.data(), qsizetype(s.size()));
        QString out;
        out.reserve(qsizetype(s.size()));
        for (const char c : s) {
            const auto b = static_cast<unsigned char>(c);
            out += b < 0x80 ? QChar(b) : QChar(_high[b - 0x80]);
        }
        return out;
    }

private:
    enum class Kind { Utf8, Latin1, Table };
    Kind                  _kind = Kind::Latin1;
    const char16_t       *_high = nullptr;
    QHash<char16_t, char> _reverse;
};

class HunspellBackend : public Backend {
public:
    bool loadsOffThread() const override { return true; }

    bool load(const QStringList &codes) override {
        const auto files = findDictionaries();
        for (const QString &code : codes) {
            const auto it = files.constFind(code);
            if (it == files.cend())
                continue;
            auto hs = std::make_unique<Hunspell>(
                QFile::encodeName(it->aff).constData(), QFile::encodeName(it->dic).constData()
            );
            Charset charset(hs->get_dict_encoding());
            _dicts.push_back({std::move(hs), std::move(charset)});
        }
        if (_dicts.empty())
            return false;
        QFile words(userWordsPath());
        if (words.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&words);
            while (!in.atEnd()) {
                const QString w = in.readLine().trimmed();
                if (!w.isEmpty())
                    addToAll(w);
            }
        }
        return true;
    }

    bool check(const QString &word) override {
        std::string enc;
        for (const auto &d : _dicts)
            if (d.charset.encode(word, enc) && d.hs->spell(enc))
                return true;
        return false;
    }

    QStringList suggest(const QString &word, int max) override {
        QStringList out;
        std::string enc;
        for (const auto &d : _dicts) {
            if (!d.charset.encode(word, enc))
                continue;
            for (const std::string &s : d.hs->suggest(enc)) {
                const QString q = d.charset.decode(s);
                if (!out.contains(q))
                    out << q;
                if (out.size() >= max)
                    return out;
            }
        }
        return out;
    }

    void addToDictionary(const QString &word) override {
        addToAll(word);
        const QString path = userWordsPath();
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        if (f.open(QIODevice::Append | QIODevice::Text))
            f.write(word.toUtf8() + '\n');
    }

private:
    void addToAll(const QString &word) {
        std::string enc;
        for (const auto &d : _dicts)
            if (d.charset.encode(word, enc))
                d.hs->add(enc);
    }

    struct Dictionary {
        std::unique_ptr<Hunspell> hs;
        Charset                   charset;
    };
    std::vector<Dictionary> _dicts;
};

// ID and ID_LIKE of /etc/os-release, lower case: "ubuntu debian".
QString osFamily() {
    QFile f(u"/etc/os-release"_s);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    QString ids;
    for (const QByteArray &line : f.readAll().split('\n')) {
        if (line.startsWith("ID=") || line.startsWith("ID_LIKE="))
            ids += u' ' + QString::fromUtf8(line.mid(line.indexOf('=') + 1)).remove(u'"').toLower();
    }
    return ids;
}

} // namespace

std::unique_ptr<Backend> createPlatformBackend() {
    return std::make_unique<HunspellBackend>();
}

QList<Language> availableLanguages() {
    QList<Language> out;
    const auto      files = findDictionaries();
    for (auto it = files.cbegin(); it != files.cend(); ++it)
        out.append({it.key(), languageName(it.key())});
    return out;
}

QString dictionaryPackageHint() {
    // The system language's package, named the way the distribution does.
    const QLocale sys  = QLocale::system();
    const QString lang = QLocale::languageToCode(sys.language());
    const QString terr = QLocale::territoryToCode(sys.territory());
    if (lang.isEmpty() || lang == u"C"_s)
        return u"hunspell-en-us"_s;
    const QString os = osFamily();
    if (os.contains(u"debian"_s) || os.contains(u"ubuntu"_s)) {
        // Debian splits English and German by country: hunspell-en-us, hunspell-de-de.
        if ((lang == u"en"_s || lang == u"de"_s) && !terr.isEmpty())
            return u"hunspell-%1-%2"_s.arg(lang, terr.toLower());
        return u"hunspell-"_s + lang;
    }
    if (os.contains(u"fedora"_s) || os.contains(u"rhel"_s))
        return lang == u"en"_s ? u"hunspell-en-US"_s : u"hunspell-"_s + lang;
    if (os.contains(u"arch"_s))
        return lang == u"en"_s ? u"hunspell-en_us"_s : u"hunspell-"_s + lang;
    if (os.contains(u"suse"_s))
        return u"myspell-%1_%2"_s.arg(lang, terr.isEmpty() ? lang.toUpper() : terr);
    return u"hunspell-"_s + lang;
}

} // namespace Spell
