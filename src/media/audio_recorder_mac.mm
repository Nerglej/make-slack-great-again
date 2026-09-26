// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "media/audio_recorder.h"

#import <AVFoundation/AVFoundation.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QMetaObject>
#include <QPointer>
#include <QTimer>
#include <QtEndian>
#include <algorithm>
#include <cmath>

// macOS: AVAudioRecorder writing 16 kHz mono s16le LinearPCM to a temp .wav,
// with metering polled on a main-thread timer for level(). Microphone access
// goes through TCC: the first start() asks (NSMicrophoneUsageDescription in
// Info.plist; the hardened-runtime signature also needs the
// com.apple.security.device.audio-input entitlement — resources/
// msga.entitlements), and a denial fails with a pointer to System Settings.
// Manual retain/release (no ARC), like the other .mm files in this tree.

namespace Media {

// Outside the anonymous namespace: the Objective-C delegate below refers to it.
class MacRecorder;

namespace {

// Core Audio's WAV writer may add chunks (FLLR padding) before "data"; hand
// out the same plain 44-byte-header file as the other platforms.
QByteArray normalizedWav(const QByteArray &file) {
    if (file.size() < 12 || !file.startsWith("RIFF") || file.mid(8, 4) != "WAVE")
        return file;
    qsizetype pos = 12;
    while (pos + 8 <= file.size()) {
        const QByteArray id  = file.mid(pos, 4);
        const quint32    len = qFromLittleEndian<quint32>(file.constData() + pos + 4);
        if (id == "data") {
            const qsizetype n = std::min<qsizetype>(len, file.size() - pos - 8);
            return wavFromPcm16(
                file.mid(pos + 8, n & ~qsizetype(1)), Recorder::kSampleRate, Recorder::kChannels
            );
        }
        pos += 8 + qsizetype(len) + (len & 1);
    }
    return file;
}

} // namespace
} // namespace Media

@interface MsgaAudioRecorderDelegate : NSObject <AVAudioRecorderDelegate>
@property(nonatomic, assign) Media::MacRecorder *recorder;
@end

namespace Media {

class MacRecorder : public Recorder {
public:
    explicit MacRecorder(QObject *parent) : Recorder(parent) {
        _meter.setInterval(50);
        QObject::connect(&_meter, &QTimer::timeout, this, [this] {
            if (!_rec)
                return;
            [_rec updateMeters];
            const float db = [_rec peakPowerForChannel:0]; // -160 (silence) .. 0 dBFS
            emit level(std::clamp(float(std::pow(10.0, db / 20.0)), 0.0f, 1.0f));
        });
    }
    ~MacRecorder() override { cancel(); }

    void start() override {
        if (_recording)
            return;
        _recording    = true;
        const int gen = ++_generation;
        switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio]) {
        case AVAuthorizationStatusAuthorized:
            // Queued, so started() never fires inside the caller's start().
            QTimer::singleShot(0, this, [this, gen] {
                if (gen == _generation)
                    begin();
            });
            return;
        case AVAuthorizationStatusNotDetermined: {
            // Shows the system prompt; the answer arrives on an arbitrary queue.
            QPointer<MacRecorder> guard(this);
            [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                                     completionHandler:^(BOOL granted) {
                                       QMetaObject::invokeMethod(
                                           QCoreApplication::instance(),
                                           [guard, gen, granted] {
                                               if (!guard || gen != guard->_generation)
                                                   return;
                                               if (granted)
                                                   guard->begin();
                                               else
                                                   guard->deny();
                                           },
                                           Qt::QueuedConnection
                                       );
                                     }];
            return;
        }
        default: // denied, restricted
            deny();
            return;
        }
    }

    void stop() override {
        if (!_recording)
            return;
        if (!_rec) { // still waiting for the permission answer
            ++_generation;
            _recording = false;
            emit finished(wavFromPcm16({}, kSampleRate, kChannels));
            return;
        }
        [_rec stop]; // finalizes the file synchronously
        QFile      f(_path);
        const bool ok  = f.open(QIODevice::ReadOnly);
        QByteArray wav = ok ? normalizedWav(f.readAll()) : QByteArray();
        f.close();
        teardown();
        if (ok)
            emit finished(wav);
        else
            emit failed(
                QCoreApplication::translate("Media::Recorder", "Couldn't read the recording")
            );
    }

    void cancel() override {
        ++_generation;
        if (_rec)
            [_rec stop];
        teardown();
    }

    bool isRecording() const override { return _recording; }

    // From the delegate, possibly off the main thread: `rec` identifies the
    // recording, so a late error can't end a newer one.
    void encodeFailed(void *rec, const QString &msg) {
        QPointer<MacRecorder> guard(this);
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [guard, rec, msg] {
                if (!guard || (void *)guard->_rec != rec)
                    return;
                guard->cancel();
                emit guard->failed(msg);
            },
            Qt::QueuedConnection
        );
    }

private:
    void deny() {
        _recording = false;
        emit failed(QCoreApplication::translate(
            "Media::Recorder",
            "Microphone access is blocked. Allow msga in System Settings → Privacy & Security → "
            "Microphone."
        ));
    }

    void begin() {
        static int counter = 0;
        _path              = QDir::temp().filePath(QStringLiteral("msga-voice-%1-%2.wav")
                                                       .arg(QCoreApplication::applicationPid())
                                                       .arg(++counter));
        QFile::remove(_path);
        NSDictionary *settings = @{
            AVFormatIDKey : @(kAudioFormatLinearPCM),
            AVSampleRateKey : @(double(kSampleRate)),
            AVNumberOfChannelsKey : @(kChannels),
            AVLinearPCMBitDepthKey : @16,
            AVLinearPCMIsFloatKey : @NO,
            AVLinearPCMIsBigEndianKey : @NO,
            AVLinearPCMIsNonInterleaved : @NO,
        };
        NSURL   *url = [NSURL fileURLWithPath:_path.toNSString()];
        NSError *err = nil;
        _rec         = [[AVAudioRecorder alloc] initWithURL:url settings:settings error:&err];
        if (!_rec) {
            const QString msg = err ? QString::fromNSString(err.localizedDescription)
                                    : QCoreApplication::translate(
                                          "Media::Recorder", "Couldn't open the microphone"
                                      );
            teardown();
            emit failed(msg);
            return;
        }
        _delegate            = [[MsgaAudioRecorderDelegate alloc] init];
        _delegate.recorder   = this;
        _rec.delegate        = _delegate;
        _rec.meteringEnabled = YES;
        if (![_rec prepareToRecord] || ![_rec record]) {
            teardown();
            emit failed(QCoreApplication::translate(
                "Media::Recorder", "Couldn't start recording from the microphone"
            ));
            return;
        }
        _meter.start();
        emit started();
    }

    // Releases the recorder and deletes the temp file; emits nothing.
    void teardown() {
        _meter.stop();
        _recording = false;
        if (_rec) {
            _rec.delegate = nil;
            [_rec release];
            _rec = nil;
        }
        if (_delegate) {
            _delegate.recorder = nullptr;
            [_delegate release];
            _delegate = nil;
        }
        if (!_path.isEmpty()) {
            QFile::remove(_path);
            _path.clear();
        }
    }

    AVAudioRecorder           *_rec      = nil;
    MsgaAudioRecorderDelegate *_delegate = nil;
    QString                    _path;
    QTimer                     _meter;
    int                        _generation = 0;
    bool                       _recording  = false;
};

} // namespace Media

@implementation MsgaAudioRecorderDelegate
- (void)audioRecorderEncodeErrorDidOccur:(AVAudioRecorder *)rec error:(NSError *)error {
    if (!self.recorder)
        return;
    self.recorder->encodeFailed(
        rec, error ? QString::fromNSString(error.localizedDescription)
                   : QCoreApplication::translate(
                         "Media::Recorder", "Recording from the microphone failed"
                     )
    );
}
@end

namespace Media {

std::unique_ptr<Recorder> createNativeRecorder(QObject *parent) {
    return std::make_unique<MacRecorder>(parent);
}

} // namespace Media
