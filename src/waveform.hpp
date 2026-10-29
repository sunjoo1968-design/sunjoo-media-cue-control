// Copyright (C) 2026 SunjooAn. SPDX-License-Identifier: GPL-2.0-or-later.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
namespace cue_editor {
struct Waveform {
 int64_t startMs=0,endMs=0;
 bool hasAudio=false;
 std::vector<float> peaks;
};
// File analysis only. Never connects to an OBS source or audio output.
// Bounded peak storage; zoom requests reanalyse the visible interval.
Waveform analyseWaveform(const std::string &path,int64_t startMs,int64_t endMs,
                         const std::function<bool()> &cancel);
}
