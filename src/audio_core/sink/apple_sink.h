// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "audio_core/sink/sink.h"
namespace AudioCore::Sink {
// iOS output only. Capture is deliberately unsupported (no microphone permission).
std::unique_ptr<Sink> CreateAppleSink();
}
