// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "media/audio_engine.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>
#include <functional>

#include "miniaudio.h"

using namespace Qt::StringLiterals;

// Linux has no dependency-free in-process audio output (every sound server
// needs a shared lib that won't link into the static release binary — same
// constraint as sound_player_linux.cpp), so playback is a two-stage pipe:
//
//   source (PCM frames)  ──►  sink (helper process reading raw PCM on stdin)
//
// Sources: miniaudio's decoders in-process (MP3/WAV/FLAC/Vorbis), else
// `ffmpeg` decoding to s16le on stdout when it is on PATH (AAC/M4A, WebM/Opus
// voice clips, …). Sink: the first of pw-cat / paplay / aplay that starts —
// the sound server's own CLI, present on essentially every desktop.
//
// Pause and seek kill the sink (its buffered audio must not play out) and
// restart it at the new offset; the position is the wall clock since the sink
// started plus that offset, capped by what has been written.

namespace Media {
namespace {

struct PcmFormat {
    int rate     = 48000;
    int channels = 2;
};
inline qint64 bytesPerFrame(const PcmFormat &f) {
    return 2 * f.channels;
} // s16le

// Interleaved s16le PCM frames pulled by the engine. read() may return an
// empty array before atEnd(): the source has no data yet and will call
// onReadable() once it does (asynchronous decoders).
class PcmSource {
public:
    virtual ~PcmSource()                                       = default;
    virtual bool       open(const QString &path, QString *err) = 0;
    virtual PcmFormat  format() const                          = 0;
    virtual qint64     totalFrames() const                     = 0; // 0 = unknown
    virtual void       seekFrame(qint64 frame)                 = 0;
    virtual QByteArray read(qint64 maxBytes)                   = 0;
    virtual bool       atEnd() const                           = 0;

    std::function<void()>                onReadable;
    std::function<void(const QString &)> onFailed;
};

// In-process decode via miniaudio. Output stays in the file's native rate and
// channel count; the sink is told the format instead of resampling.
class MiniaudioSource : public PcmSource {
public:
    ~MiniaudioSource() override {
        if (_open)
            ma_decoder_uninit(&_dec);
    }

    bool open(const QString &path, QString *err) override {
        ma_decoder_config cfg = ma_decoder_config_init(ma_format_s16, 0, 0);
        if (ma_decoder_init_file(path.toUtf8().constData(), &cfg, &_dec) != MA_SUCCESS) {
            if (err)
                *err = QCoreApplication::translate(
                    "Media::AudioPlayer", "This audio format can't be played here"
                );
            return false;
        }
        _open = true;
        _fmt  = {(int)_dec.outputSampleRate, (int)_dec.outputChannels};
        if (_fmt.rate <= 0 || _fmt.channels <= 0) {
            if (err)
                *err = QCoreApplication::translate(
                    "Media::AudioPlayer", "This audio format can't be played here"
                );
            return false;
        }
        ma_uint64 len = 0;
        if (ma_decoder_get_length_in_pcm_frames(&_dec, &len) == MA_SUCCESS)
            _total = (qint64)len;
        return true;
    }
    PcmFormat format() const override { return _fmt; }
    qint64    totalFrames() const override { return _total; }
    void      seekFrame(qint64 frame) override {
        ma_decoder_seek_to_pcm_frame(&_dec, (ma_uint64)std::max<qint64>(0, frame));
        _eof = false;
    }
    QByteArray read(qint64 maxBytes) override {
        const qint64    bpf    = bytesPerFrame(_fmt);
        const qint64    frames = std::max<qint64>(1, maxBytes / bpf);
        QByteArray      buf(frames * bpf, Qt::Uninitialized);
        ma_uint64       got = 0;
        const ma_result r = ma_decoder_read_pcm_frames(&_dec, buf.data(), (ma_uint64)frames, &got);
        if (r == MA_AT_END || (r == MA_SUCCESS && got == 0))
            _eof = true;
        else if (r != MA_SUCCESS)
            _eof = true; // corrupt tail: stop rather than loop forever
        buf.resize((qsizetype)(got * bpf));
        return buf;
    }
    bool atEnd() const override { return _eof; }

private:
    ma_decoder _dec{};
    bool       _open = false;
    bool       _eof  = false;
    PcmFormat  _fmt;
    qint64     _total = 0;
};

// External decode: `ffmpeg … -f s16le pipe:1`. Seeking restarts the process
// with -ss. Duration is parsed from ffmpeg's own "Duration:" banner on stderr.
class FfmpegSource : public QObject, public PcmSource {
public:
    static QString executable() { return QStandardPaths::findExecutable(u"ffmpeg"_s); }

    ~FfmpegSource() override { killProc(); }

    bool open(const QString &path, QString *err) override {
        _exe = executable();
        if (_exe.isEmpty()) {
            if (err)
                *err = QCoreApplication::translate(
                    "Media::AudioPlayer", "This audio format needs ffmpeg installed to play"
                );
            return false;
        }
        _path = path;
        start(0);
        return true;
    }
    PcmFormat  format() const override { return _fmt; }
    qint64     totalFrames() const override { return _total; }
    void       seekFrame(qint64 frame) override { start(frame); }
    QByteArray read(qint64 maxBytes) override {
        if (_buf.isEmpty())
            return {};
        const qint64 n   = std::min<qint64>(maxBytes, _buf.size());
        QByteArray   out = _buf.left(n);
        _buf.remove(0, n);
        return out;
    }
    bool atEnd() const override { return _finished && _buf.isEmpty(); }

private:
    void killProc() {
        if (!_proc)
            return;
        QProcess *p = _proc;
        _proc       = nullptr;
        p->disconnect(this);
        if (p->state() == QProcess::NotRunning) {
            p->deleteLater();
        } else {
            connect(p, &QProcess::finished, p, &QObject::deleteLater);
            p->kill();
        }
    }

    void start(qint64 frame) {
        killProc();
        _buf.clear();
        _finished        = false;
        _gotData         = false;
        _proc            = new QProcess(this);
        const double sec = (double)frame / _fmt.rate;
        QStringList  args{
            u"-nostdin"_s, u"-hide_banner"_s, u"-nostats"_s, u"-loglevel"_s, u"info"_s
        };
        if (frame > 0)
            args << u"-ss"_s << QString::number(sec, 'f', 3);
        args << u"-i"_s << _path << u"-vn"_s << u"-f"_s << u"s16le"_s << u"-ac"_s
             << QString::number(_fmt.channels) << u"-ar"_s << QString::number(_fmt.rate)
             << u"pipe:1"_s;
        connect(_proc, &QProcess::readyReadStandardOutput, this, [this] {
            _buf += _proc->readAllStandardOutput();
            _gotData = true;
            if (onReadable)
                onReadable();
        });
        connect(_proc, &QProcess::readyReadStandardError, this, [this] {
            _errOut += _proc->readAllStandardError();
            if (_total == 0) {
                static const QRegularExpression re(uR"(Duration:\s*(\d+):(\d\d):(\d\d)\.(\d\d))"_s);
                const auto                      m = re.match(QString::fromUtf8(_errOut));
                if (m.hasMatch()) {
                    const qint64 ms =
                        ((m.captured(1).toLongLong() * 60 + m.captured(2).toLongLong()) * 60 +
                         m.captured(3).toLongLong()) *
                            1000 +
                        m.captured(4).toLongLong() * 10;
                    _total = ms * _fmt.rate / 1000;
                }
            }
        });
        connect(_proc, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) {
            _buf += _proc->readAllStandardOutput();
            _finished = true;
            if (!_gotData && _buf.isEmpty() && (code != 0 || st != QProcess::NormalExit)) {
                if (onFailed)
                    onFailed(
                        QCoreApplication::translate(
                            "Media::AudioPlayer", "This audio format can't be played here"
                        )
                    );
                return;
            }
            if (onReadable)
                onReadable();
        });
        connect(_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
            if (e == QProcess::FailedToStart && onFailed)
                onFailed(
                    QCoreApplication::translate(
                        "Media::AudioPlayer", "This audio format needs ffmpeg installed to play"
                    )
                );
        });
        _proc->start(_exe, args);
    }

    QString    _exe, _path;
    QProcess  *_proc = nullptr;
    QByteArray _buf, _errOut;
    PcmFormat  _fmt{48000, 2};
    qint64     _total    = 0;
    bool       _finished = false;
    bool       _gotData  = false;
};

// Helper process consuming raw s16le PCM on stdin.
class ProcessSink : public QObject {
public:
    using QObject::QObject;
    ~ProcessSink() override { kill(); }

    struct Candidate {
        QString     program;
        QStringList args;
    };
    static std::vector<Candidate> candidates(const PcmFormat &f) {
        const QString rate = QString::number(f.rate), ch = QString::number(f.channels);
        return {
            {u"pw-cat"_s,
             {u"--playback"_s,
              u"--raw"_s,
              u"--format"_s,
              u"s16"_s,
              u"--rate"_s,
              rate,
              u"--channels"_s,
              ch,
              u"-"_s}},
            {u"paplay"_s,
             {u"--raw"_s, u"--format=s16le"_s, u"--rate="_s + rate, u"--channels="_s + ch}},
            {u"aplay"_s,
             {u"-q"_s, u"-t"_s, u"raw"_s, u"-f"_s, u"S16_LE"_s, "-r", rate, "-c", ch, "-"}},
        };
    }

    // Start the first candidate at or after `attempt` that launches; returns
    // its index, or -1 when none did.
    int start(const PcmFormat &f, int attempt) {
        const auto cands = candidates(f);
        for (int i = attempt; i < (int)cands.size(); ++i) {
            auto *p = new QProcess(this);
            p->setStandardOutputFile(QProcess::nullDevice());
            p->setStandardErrorFile(QProcess::nullDevice());
            p->start(cands[i].program, cands[i].args);
            if (!p->waitForStarted(500)) {
                delete p;
                continue;
            }
            _proc = p;
            connect(p, &QProcess::bytesWritten, this, [this] {
                if (onDrained)
                    onDrained();
            });
            connect(p, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) {
                if (onFinished)
                    onFinished(st == QProcess::NormalExit && code == 0);
            });
            return i;
        }
        return -1;
    }
    void   write(const QByteArray &b) { _proc->write(b); }
    qint64 bytesQueued() const { return _proc ? _proc->bytesToWrite() : 0; }
    void   finishInput() { _proc->closeWriteChannel(); }
    void   kill() {
        if (!_proc)
            return;
        QProcess *p = _proc;
        _proc       = nullptr;
        p->disconnect(this);
        if (p->state() == QProcess::NotRunning) {
            p->deleteLater();
        } else {
            connect(p, &QProcess::finished, p, &QObject::deleteLater);
            p->kill();
        }
    }

    std::function<void()>     onDrained;
    std::function<void(bool)> onFinished;

private:
    QProcess *_proc = nullptr;
};

class LinuxEngine : public Engine {
public:
    using Engine::Engine;
    ~LinuxEngine() override { stop(); }

    void load(const QString &path) override {
        stop();
        QString err;
        auto    ma = std::make_unique<MiniaudioSource>();
        if (ma->open(path, &err)) {
            _src = std::move(ma);
        } else if (!FfmpegSource::executable().isEmpty()) {
            auto ff = std::make_unique<FfmpegSource>();
            if (!ff->open(path, &err)) {
                emit failed(err);
                return;
            }
            _src     = std::move(ff);
            // ffmpeg can't tell us up front whether it can decode the file:
            // report loaded() only once the first PCM arrives (or fail).
            _probing = true;
        } else {
            // miniaudio's decoders are the whole in-process repertoire; anything
            // else (AAC/M4A, WebM/Opus, …) only plays with ffmpeg on PATH.
            emit failed(
                QCoreApplication::translate(
                    "Media::AudioPlayer", "This audio format needs ffmpeg installed to play"
                )
            );
            return;
        }
        _fmt             = _src->format();
        _src->onReadable = [this] {
            if (_probing) {
                _probing = false;
                emit loaded();
                return;
            }
            pump();
        };
        _src->onFailed = [this](const QString &e) {
            _probing = false;
            _playing = false;
            if (_sink)
                _sink->kill();
            emit failed(e);
        };
        if (!_probing)
            emit loaded();
    }

    void play() override {
        if (!_src || _playing)
            return;
        _playing = true;
        _src->seekFrame(_pausedFrame);
        _baseFrame   = _pausedFrame;
        _sinkAttempt = 0;
        startSink();
    }

    void pause() override {
        if (!_playing)
            return;
        _pausedFrame = currentFrame();
        _playing     = false;
        if (_sink)
            _sink->kill();
    }

    void seek(qint64 ms) override {
        if (!_src)
            return;
        qint64 frame = ms * _fmt.rate / 1000;
        if (const qint64 total = _src->totalFrames(); total > 0)
            frame = std::min(frame, total);
        frame        = std::max<qint64>(0, frame);
        _pausedFrame = frame;
        if (_playing) {
            if (_sink)
                _sink->kill();
            _src->seekFrame(frame);
            _baseFrame = frame;
            startSink();
        }
    }

    void stop() override {
        _playing = false;
        _probing = false;
        if (_sink)
            _sink->kill();
        _sink.reset();
        _src.reset();
        _baseFrame = _pausedFrame = 0;
    }

    qint64 positionMs() const override { return _src ? currentFrame() * 1000 / _fmt.rate : 0; }
    qint64 durationMs() const override { return _src ? _src->totalFrames() * 1000 / _fmt.rate : 0; }

    bool supportsExtension(const QString &ext) const override {
        static const QStringList kInProcess{u"mp3"_s, u"wav"_s, u"flac"_s, u"ogg"_s, u"oga"_s};
        if (kInProcess.contains(ext))
            return true;
        return !FfmpegSource::executable().isEmpty();
    }

private:
    void startSink() {
        _sink         = std::make_unique<ProcessSink>();
        const int idx = _sink->start(_fmt, _sinkAttempt);
        if (idx < 0) {
            _playing = false;
            _sink.reset();
            emit failed(
                QCoreApplication::translate(
                    "Media::AudioPlayer", "No audio output found (needs pw-cat, paplay or aplay)"
                )
            );
            return;
        }
        _sinkAttempt      = idx;
        _sink->onDrained  = [this] { pump(); };
        _sink->onFinished = [this](bool ok) { sinkFinished(ok); };
        _clock.start();
        _writtenFrames = 0;
        _inputDone     = false;
        pump();
    }

    // Keep ~250 ms queued in the sink's pipe; more only delays pause/seek.
    void pump() {
        if (!_playing || !_sink || !_src)
            return;
        const qint64 bpf    = bytesPerFrame(_fmt);
        const qint64 target = bpf * _fmt.rate / 4;
        const qint64 chunk  = bpf * _fmt.rate / 20; // 50 ms
        for (;;) {
            if (_src->atEnd()) {
                if (!_inputDone) {
                    _inputDone = true;
                    _sink->finishInput();
                }
                return;
            }
            if (_sink->bytesQueued() >= target)
                return;
            const QByteArray data = _src->read(chunk);
            if (data.isEmpty()) {
                if (_src->atEnd())
                    continue;
                return; // asynchronous source: onReadable() resumes the pump
            }
            _sink->write(data);
            _writtenFrames += data.size() / bpf;
        }
    }

    void sinkFinished(bool ok) {
        if (!_playing)
            return;
        if (ok || _inputDone) {
            // Played out to the end.
            _playing           = false;
            const qint64 total = _src ? _src->totalFrames() : 0;
            _pausedFrame       = total > 0 ? total : _baseFrame + _writtenFrames;
            _sink->kill();
            emit ended();
            return;
        }
        // Died early (sound server not running, device busy): try the next helper.
        _sink->kill();
        if (_clock.elapsed() < 2000 &&
            _sinkAttempt + 1 < (int)ProcessSink::candidates(_fmt).size()) {
            ++_sinkAttempt;
            _src->seekFrame(_baseFrame);
            startSink();
            return;
        }
        _playing     = false;
        _pausedFrame = _baseFrame;
        emit failed(QCoreApplication::translate("Media::AudioPlayer", "Audio output failed"));
    }

    qint64 currentFrame() const {
        if (!_playing)
            return _pausedFrame;
        qint64 f = _baseFrame + _clock.elapsed() * _fmt.rate / 1000;
        f        = std::min(f, _baseFrame + _writtenFrames);
        if (const qint64 total = _src ? _src->totalFrames() : 0; total > 0)
            f = std::min(f, total);
        return f;
    }

    std::unique_ptr<PcmSource>   _src;
    std::unique_ptr<ProcessSink> _sink;
    PcmFormat                    _fmt;
    QElapsedTimer                _clock;
    bool                         _playing       = false;
    bool                         _probing       = false; // ffmpeg source: awaiting first PCM
    bool                         _inputDone     = false;
    int                          _sinkAttempt   = 0;
    qint64                       _baseFrame     = 0; // source frame the running sink started at
    qint64                       _pausedFrame   = 0; // position while not playing
    qint64                       _writtenFrames = 0; // frames handed to the running sink
};

} // namespace

std::unique_ptr<Engine> createPlatformEngine(QObject *parent) {
    return std::make_unique<LinuxEngine>(parent);
}

} // namespace Media
