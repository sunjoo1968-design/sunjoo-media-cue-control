// Copyright (C) 2026 SunjooAn. SPDX-License-Identifier: GPL-2.0-or-later.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}
namespace cue_editor {
struct Picture {int width=0,height=0;int64_t timeMs=0,nextTimeMs=0;std::vector<uint8_t> rgba;};
// Video-only reader: never creates an OBS source, mixer, or audio decoder.
class PreviewDecoder {
 AVFormatContext *format_=nullptr;AVCodecContext *codec_=nullptr;
 AVPacket *packet_=nullptr;AVFrame *frame_=nullptr,*last_=nullptr;SwsContext *scale_=nullptr;
 std::atomic<bool> &stop_;int stream_=-1;int64_t start_=0;
 Picture convert(AVFrame *frame);
 Picture seek(int64_t ms,bool before,const std::function<bool()> &cancel);
public:
 explicit PreviewDecoder(std::atomic<bool> &stop):stop_(stop){}
 ~PreviewDecoder();
 void open(const std::string &path);
 Picture frameAt(int64_t ms,const std::function<bool()> &cancel);
 Picture frameBefore(int64_t shownMs,const std::function<bool()> &cancel);
 int64_t durationMs=0;double frameMs=1000.0/30;
};
}
