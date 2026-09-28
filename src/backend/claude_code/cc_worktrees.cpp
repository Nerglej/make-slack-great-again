// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "cc_worktrees.h"

#include <QByteArrayView>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <memory>
#include <utility>

#if defined(Q_OS_WIN)
#include <windows.h>
#endif

namespace claude_code {
namespace {

QJsonObject parseObject(QByteArrayView json) {
    QJsonParseError err;
    const auto      doc = QJsonDocument::fromJson(json.toByteArray(), &err);
    return err.error == QJsonParseError::NoError ? doc.object() : QJsonObject{};
}

QString cleaned(const QString &path) {
    return path.isEmpty() ? QString() : QDir::cleanPath(QDir::fromNativeSeparators(path));
}

// Paths compared as the file system does: resolved where they exist.
QString comparable(const QString &path) {
    const QString canonical = QFileInfo(path).canonicalFilePath();
    return canonical.isEmpty() ? cleaned(path) : canonical;
}

Qt::CaseSensitivity pathCase() {
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

bool samePath(const QString &a, const QString &b) {
    return !a.isEmpty() && !b.isEmpty() &&
           (cleaned(a).compare(cleaned(b), pathCase()) == 0 ||
            comparable(a).compare(comparable(b), pathCase()) == 0);
}

QString shortBranch(QString branch) {
    if (branch.startsWith(QLatin1String("refs/heads/")))
        branch.remove(0, 11);
    return branch;
}

WorktreeRef refFrom(const QJsonObject &o, const char *originKey) {
    return {
        cleaned(o.value(QLatin1String("worktreePath")).toString()),
        shortBranch(o.value(QLatin1String("worktreeBranch")).toString()),
        cleaned(o.value(QLatin1String(originKey)).toString()),
    };
}

void runGit(
    QObject *ctx, const QStringList &args, std::function<void(int code, QByteArray out)> done
) {
    auto *p = new QProcess(ctx);
#if defined(Q_OS_WIN)
    p->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *a) {
        a->flags |= CREATE_NO_WINDOW;
    });
#endif
    p->setStandardInputFile(QProcess::nullDevice());
    p->setStandardErrorFile(QProcess::nullDevice()); // unread, it could fill up
    auto settled = std::make_shared<bool>(false);    // finished and errorOccurred can both fire
    QObject::connect(
        p, &QProcess::finished, ctx, [p, done, settled](int code, QProcess::ExitStatus st) {
            if (std::exchange(*settled, true))
                return;
            done(st == QProcess::NormalExit ? code : -1, p->readAllStandardOutput());
            p->deleteLater();
        }
    );
    QObject::connect(
        p, &QProcess::errorOccurred, ctx, [p, done, settled](QProcess::ProcessError e) {
            if (e != QProcess::FailedToStart || std::exchange(*settled, true))
                return;
            done(-1, {});
            p->deleteLater();
        }
    );
    QTimer::singleShot(60'000, p, [p] {
        if (p->state() != QProcess::NotRunning)
            p->kill();
    });
    p->start(QStringLiteral("git"), args);
}

struct Reap {
    QObject                             *ctx = nullptr;
    std::vector<WorktreeRef>             refs;
    std::size_t                          next = 0;
    std::function<bool(const QString &)> inUse;
    std::function<void(QStringList)>     done;
    QStringList                          notDeleted;
};

void reapNext(std::shared_ptr<Reap> r);

// The branch goes last, unless the main worktree or another has it out.
void deleteBranch(
    std::shared_ptr<Reap>             r,
    const QString                    &mainDir,
    const std::vector<WorktreeEntry> &list,
    const WorktreeRef                &ref
) {
    const bool checkedOut = std::any_of(list.begin(), list.end(), [&](const WorktreeEntry &e) {
        return e.branch == ref.branch && !samePath(e.path, ref.path);
    });
    if (ref.branch.isEmpty() || checkedOut) {
        reapNext(r);
        return;
    }
    runGit(
        r->ctx,
        {QStringLiteral("-C"), mainDir, QStringLiteral("branch"), QStringLiteral("-D"), ref.branch},
        [r, ref](int code, QByteArray) {
            if (code != 0)
                qInfo("claude code: branch %s not deleted", qPrintable(ref.branch));
            reapNext(r);
        }
    );
}

// The folder is gone (or never was): the registration goes, then the branch.
void pruneThenBranch(
    std::shared_ptr<Reap>             r,
    const QString                    &mainDir,
    const std::vector<WorktreeEntry> &list,
    const WorktreeRef                &ref
) {
    if (QFileInfo::exists(ref.path)) {
        r->notDeleted << ref.path;
        reapNext(r);
        return;
    }
    runGit(
        r->ctx,
        {QStringLiteral("-C"), mainDir, QStringLiteral("worktree"), QStringLiteral("prune")},
        [r, mainDir, list, ref](int, QByteArray) { deleteBranch(r, mainDir, list, ref); }
    );
}

// Deletes the folder itself, off the caller's thread.
void deleteFolder(
    std::shared_ptr<Reap>             r,
    const QString                    &mainDir,
    const std::vector<WorktreeEntry> &list,
    const WorktreeRef                &ref
) {
    if (!QFileInfo::exists(ref.path)) {
        pruneThenBranch(r, mainDir, list, ref);
        return;
    }
    auto *watcher = new QFutureWatcher<bool>(r->ctx);
    QObject::connect(
        watcher, &QFutureWatcherBase::finished, r->ctx, [r, watcher, mainDir, list, ref] {
            watcher->deleteLater();
            pruneThenBranch(r, mainDir, list, ref);
        }
    );
    watcher->setFuture(QtConcurrent::run([path = ref.path] {
        return QDir(path).removeRecursively();
    }));
}

void reapOne(
    std::shared_ptr<Reap> r, const WorktreeRef &ref, const std::vector<WorktreeEntry> &list
) {
    const QString mainDir = list.front().path;
    // Never the main worktree, nor the branch it has out.
    if (samePath(ref.path, mainDir) || pathWithin(mainDir, ref.path)) {
        reapNext(r);
        return;
    }
    const bool registered = std::any_of(list.begin() + 1, list.end(), [&](const WorktreeEntry &e) {
        return samePath(e.path, ref.path);
    });
    if (!registered) {
        // Unregistered: a folder is deleted only if it is a linked worktree's
        // (its .git a file pointing back to the repository), else left be.
        if (QFileInfo::exists(ref.path) &&
            !QFileInfo(ref.path + QStringLiteral("/.git")).isFile()) {
            reapNext(r);
            return;
        }
        deleteFolder(r, mainDir, list, ref);
        return;
    }
    runGit(
        r->ctx,
        {QStringLiteral("-C"),
         mainDir,
         QStringLiteral("worktree"),
         QStringLiteral("remove"),
         QStringLiteral("--force"),
         QStringLiteral("--force"),
         ref.path},
        [r, mainDir, list, ref](int, QByteArray) { deleteFolder(r, mainDir, list, ref); }
    );
}

// Finds the worktree's repository from the worktree itself or the checkout it
// was made from, whichever git still knows.
void findRepo(std::shared_ptr<Reap> r, const WorktreeRef &ref, QStringList from) {
    while (!from.isEmpty() && !QFileInfo(from.front()).isDir())
        from.removeFirst();
    if (from.isEmpty()) {
        qInfo("claude code: no repository found for worktree %s", qPrintable(ref.path));
        reapNext(r);
        return;
    }
    const QString dir = from.takeFirst();
    runGit(
        r->ctx,
        {QStringLiteral("-C"),
         dir,
         QStringLiteral("worktree"),
         QStringLiteral("list"),
         QStringLiteral("--porcelain")},
        [r, ref, from](int code, QByteArray out) {
            const auto list = code == 0 ? parseWorktreeList(out) : std::vector<WorktreeEntry>{};
            if (list.empty()) {
                findRepo(r, ref, from);
                return;
            }
            reapOne(r, ref, list);
        }
    );
}

void reapNext(std::shared_ptr<Reap> r) {
    if (r->next >= r->refs.size()) {
        if (r->done)
            r->done(r->notDeleted);
        return;
    }
    const WorktreeRef ref = r->refs[r->next++];
    if (ref.path.isEmpty() || (r->inUse && r->inUse(ref.path))) {
        if (!ref.path.isEmpty())
            qInfo(
                "claude code: worktree %s kept, a live session works in it", qPrintable(ref.path)
            );
        reapNext(r);
        return;
    }
    findRepo(r, ref, {ref.path, ref.origin});
}

} // namespace

void addWorktree(std::vector<WorktreeRef> &refs, WorktreeRef ref) {
    if (ref.path.isEmpty())
        return;
    for (WorktreeRef &have : refs)
        if (samePath(have.path, ref.path)) {
            if (have.branch.isEmpty())
                have.branch = ref.branch;
            if (have.origin.isEmpty())
                have.origin = ref.origin;
            return;
        }
    refs.push_back(std::move(ref));
}

std::vector<WorktreeRef> worktreesOfJob(const QByteArray &stateJson) {
    std::vector<WorktreeRef> out;
    addWorktree(out, refFrom(parseObject(stateJson), "originCwd"));
    return out;
}

std::vector<WorktreeRef> worktreesOfTranscript(const QString &transcriptPath) {
    std::vector<WorktreeRef> out;
    if (transcriptPath.isEmpty())
        return out;
    QFile f(transcriptPath);
    if (f.open(QIODevice::ReadOnly) && f.size() > 0) {
        const uchar         *data = f.map(0, f.size());
        const QByteArray     copy = data ? QByteArray() : f.readAll();
        const QByteArrayView all  = data ? QByteArrayView(data, f.size()) : QByteArrayView(copy);
        static constexpr QByteArrayView kNeedle("\"worktree-state\"");
        for (qsizetype at = all.indexOf(kNeedle); at >= 0; at = all.indexOf(kNeedle, at + 1)) {
            const qsizetype start = all.lastIndexOf('\n', at) + 1;
            qsizetype       end   = all.indexOf('\n', at);
            if (end < 0)
                end = all.size();
            const QJsonObject o = parseObject(all.sliced(start, end - start));
            if (o.value(QLatin1String("type")).toString() != QLatin1String("worktree-state"))
                continue;
            // null / {} = the session left its worktree, which stays on disk.
            addWorktree(
                out, refFrom(o.value(QLatin1String("worktreeSession")).toObject(), "originalCwd")
            );
        }
    }
    const QFileInfo fi(transcriptPath);
    const QDir      subagents(
        fi.absolutePath() + QLatin1Char('/') + fi.completeBaseName() + QStringLiteral("/subagents")
    );
    for (const QString &name : subagents.entryList({QStringLiteral("*.meta.json")}, QDir::Files)) {
        QFile meta(subagents.filePath(name));
        if (meta.open(QIODevice::ReadOnly))
            addWorktree(out, refFrom(parseObject(meta.read(256 * 1024)), "originalCwd"));
    }
    return out;
}

bool pathWithin(const QString &path, const QString &dir) {
    if (path.isEmpty() || dir.isEmpty())
        return false;
    if (samePath(path, dir))
        return true;
    for (const auto &[p, d] :
         {std::pair{cleaned(path), cleaned(dir)}, std::pair{comparable(path), comparable(dir)}}) {
        const QString prefix = d.endsWith(QLatin1Char('/')) ? d : d + QLatin1Char('/');
        if (p.startsWith(prefix, pathCase()))
            return true;
    }
    return false;
}

bool worktreeInUse(const Paths &paths, const QString &path) {
    const QDir sessions(paths.sessionsDir());
    for (const auto &f : sessions.entryList({QStringLiteral("*.json")}, QDir::Files)) {
        QFile file(sessions.filePath(f));
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const auto s = parseInteractiveSession(file.read(256 * 1024));
        if (s && isProcessAlive(s->pid) && pathWithin(s->cwd, path))
            return true;
    }
    for (const SessionInfo &s : scanSessions(paths))
        if (s.running && (pathWithin(s.cwd, path) || pathWithin(s.worktreePath, path)))
            return true;
    return false;
}

std::vector<WorktreeEntry> parseWorktreeList(const QByteArray &porcelain) {
    std::vector<WorktreeEntry> out;
    for (const QByteArray &line : porcelain.split('\n')) {
        if (line.startsWith("worktree "))
            out.push_back({cleaned(QString::fromUtf8(line.mid(9))), {}});
        else if (line.startsWith("branch ") && !out.empty())
            out.back().branch = shortBranch(QString::fromUtf8(line.mid(7)));
    }
    return out;
}

void reapWorktrees(
    QObject                                    *ctx,
    std::vector<WorktreeRef>                    refs,
    std::function<bool(const QString &path)>    inUse,
    std::function<void(QStringList notDeleted)> done
) {
    auto r   = std::make_shared<Reap>();
    r->ctx   = ctx;
    r->refs  = std::move(refs);
    r->inUse = std::move(inUse);
    r->done  = std::move(done);
    reapNext(r);
}

} // namespace claude_code
