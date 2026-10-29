// Copyright (C) 2026 SunjooAn. SPDX-License-Identifier: GPL-2.0-or-later.
#include "waveform.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/samplefmt.h>
}
namespace cue_editor {
namespace {
// Across all open editors, only one background audio analysis runs at a time.
std::timed_mutex analysisSlot;
void require(int code,const char *what){if(code>=0)return;char text[AV_ERROR_MAX_STRING_SIZE]={};av_strerror(code,text,sizeof(text));throw std::runtime_error(std::string(what)+": "+text);}
struct Reader {
 AVFormatContext *format=nullptr;AVCodecContext *codec=nullptr;AVPacket *packet=nullptr;AVFrame *frame=nullptr;
 ~Reader(){av_frame_free(&frame);av_packet_free(&packet);avcodec_free_context(&codec);avformat_close_input(&format);}
};
template<class T> double sample(const uint8_t *data){T value;memcpy(&value,data,sizeof(value));return double(value);}
float amplitude(const uint8_t *data,AVSampleFormat format){
 double value=0;
 switch(format){
 case AV_SAMPLE_FMT_U8:value=(sample<uint8_t>(data)-128)/128;break;
 case AV_SAMPLE_FMT_S16:value=sample<int16_t>(data)/32768;break;
 case AV_SAMPLE_FMT_S32:value=sample<int32_t>(data)/2147483648.0;break;
 case AV_SAMPLE_FMT_S64:value=sample<int64_t>(data)/9223372036854775808.0;break;
 case AV_SAMPLE_FMT_FLT:value=sample<float>(data);break;
 case AV_SAMPLE_FMT_DBL:value=sample<double>(data);break;
 default:throw std::runtime_error("Unsupported audio sample format");
 }
 return std::isfinite(value)?float(std::min(1.0,std::abs(value))):0.0f;
}
}
Waveform analyseWaveform(const std::string &path,int64_t startMs,int64_t endMs,const std::function<bool()> &cancel){
 if(startMs<0||endMs<=startMs)throw std::runtime_error("Invalid waveform interval");
 std::unique_lock<std::timed_mutex> slot(analysisSlot,std::defer_lock);
 while(!slot.try_lock_for(std::chrono::milliseconds(10)))if(cancel())return {};
 if(cancel())return {};
 Waveform result;result.startMs=startMs;result.endMs=endMs;
 // ~1ms resolution for short views, at most 128KiB regardless of file duration.
 result.peaks.resize(size_t(std::clamp<int64_t>(endMs-startMs,1,32768)));
 Reader r;r.format=avformat_alloc_context();if(!r.format)throw std::bad_alloc();
 r.format->interrupt_callback={[](void *p){return (*static_cast<const std::function<bool()>*>(p))()?1:0;},const_cast<std::function<bool()>*>(&cancel)};
 AVDictionary *options=nullptr;av_dict_set(&options,"protocol_whitelist","file",0);
 int code=avformat_open_input(&r.format,path.c_str(),nullptr,&options);av_dict_free(&options);require(code,"Open waveform");
 require(avformat_find_stream_info(r.format,nullptr),"Read waveform information");
 int index=av_find_best_stream(r.format,AVMEDIA_TYPE_AUDIO,-1,-1,nullptr,0);
 if(index==AVERROR_STREAM_NOT_FOUND)return result;require(index,"Find audio stream");result.hasAudio=true;
 auto *stream=r.format->streams[index];auto *decoder=avcodec_find_decoder(stream->codecpar->codec_id);
 if(!decoder)throw std::runtime_error("Unsupported audio codec");
 r.codec=avcodec_alloc_context3(decoder);if(!r.codec)throw std::bad_alloc();
 require(avcodec_parameters_to_context(r.codec,stream->codecpar),"Audio parameters");r.codec->thread_count=1;r.codec->pkt_timebase=stream->time_base;
 require(avcodec_open2(r.codec,decoder,nullptr),"Open audio decoder");
 r.packet=av_packet_alloc();r.frame=av_frame_alloc();if(!r.packet||!r.frame)throw std::bad_alloc();
 // Match the video editor's origin, retaining audio delay instead of moving
 // its first sound to zero. All channels use max(abs(sample)), never a downmix.
 int video=av_find_best_stream(r.format,AVMEDIA_TYPE_VIDEO,-1,-1,nullptr,0);
 auto *origin=video>=0?r.format->streams[video]:stream;
 const double originMs=origin->start_time==AV_NOPTS_VALUE?0:origin->start_time*av_q2d(origin->time_base)*1000;
 int64_t seek=av_rescale_q(int64_t(std::max(0.0,originMs+startMs-1000)),{1,1000},stream->time_base);
 require(av_seek_frame(r.format,index,seek,AVSEEK_FLAG_BACKWARD),"Seek waveform");
 bool eof=false,done=false;double nextMs=stream->start_time==AV_NOPTS_VALUE?0:stream->start_time*av_q2d(stream->time_base)*1000-originMs;
 auto slice=std::chrono::steady_clock::now();
 while(!cancel()&&!done){
  code=avcodec_receive_frame(r.codec,r.frame);
  if(code>=0){
   auto *f=r.frame;int channels=f->ch_layout.nb_channels,rate=f->sample_rate;
   auto format=static_cast<AVSampleFormat>(f->format);auto packed=av_get_packed_sample_fmt(format);int bytes=av_get_bytes_per_sample(format);
   if(channels<=0||channels>64||rate<=0||bytes<=0)throw std::runtime_error("Invalid audio frame");
   double time=f->best_effort_timestamp==AV_NOPTS_VALUE?nextMs:f->best_effort_timestamp*av_q2d(stream->time_base)*1000-originMs;
   double step=1000.0/rate;bool planar=av_sample_fmt_is_planar(format)!=0;
   for(int i=0;i<f->nb_samples;++i){double position=time+i*step;if(position>=endMs){done=true;break;}if(position<startMs)continue;
    size_t bin=std::min(result.peaks.size()-1,size_t((position-startMs)*result.peaks.size()/(endMs-startMs)));
    for(int ch=0;ch<channels;++ch){auto *data=f->extended_data[planar?ch:0]+size_t(planar?i:i*channels+ch)*bytes;result.peaks[bin]=std::max(result.peaks[bin],amplitude(data,packed));}
   }
   nextMs=time+f->nb_samples*step;av_frame_unref(f);
   // Yield regularly: analysis has no realtime deadline and must not consume
   // a core continuously while OBS is encoding a broadcast.
   if(std::chrono::steady_clock::now()-slice>std::chrono::milliseconds(8)){std::this_thread::sleep_for(std::chrono::milliseconds(4));slice=std::chrono::steady_clock::now();}
   continue;
  }
  if(code==AVERROR_EOF)break;if(code!=AVERROR(EAGAIN))require(code,"Decode waveform");if(eof)break;
  do{code=av_read_frame(r.format,r.packet);if(code<0||r.packet->stream_index==index)break;av_packet_unref(r.packet);}while(!cancel());
  if(cancel())return {};
  if(code==AVERROR_EOF){eof=true;require(avcodec_send_packet(r.codec,nullptr),"Finish waveform");}
  else{require(code,"Read waveform packet");require(avcodec_send_packet(r.codec,r.packet),"Submit waveform packet");av_packet_unref(r.packet);}
 }
 return cancel()?Waveform{}:result;
}
}
