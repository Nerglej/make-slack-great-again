// Between audio_common.cpp and the per-OS audio backends.
#pragma once

#include "plat/audio.h"

namespace plat::audio {

// Defined by exactly one of audio_{linux,win32,cocoa}.
std::unique_ptr<Recorder> createNativeRecorder(App &app);

} // namespace plat::audio
