// Copyright (C) 2026 SunjooAn. SPDX-License-Identifier: GPL-2.0-or-later.
#pragma once
#include <obs.h>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace cue_range {
struct DestroyFrame {void operator()(obs_source_frame *frame) const {obs_source_frame_destroy(frame);}};
struct Video {
 uint64_t offset;
 std::unique_ptr<obs_source_frame,DestroyFrame> frame;
};
struct Cache {
 std::vector<Video> video;
 std::vector<float> audio[2];
 uint32_t rate=0,channels=0;
};
std::shared_ptr<Cache> prepare(const std::string &path,int64_t in,int64_t out,std::atomic<bool> &cancel);
}
