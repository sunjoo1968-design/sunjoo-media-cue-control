// Copyright (C) 2026 SunjooAn. SPDX-License-Identifier: GPL-2.0-or-later.
#include "range-cache.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace cue_range {
namespace {
constexpr size_t memoryLimit=96ull*1024*1024;
std::atomic<size_t> reserved{0};
struct CacheStorage : Cache {
 size_t bytes=0;
 ~CacheStorage(){reserved.fetch_sub(bytes);}
 bool reserve(size_t size){
  auto used=reserved.load();
  do {if(size>memoryLimit-used)return false;} while(!reserved.compare_exchange_weak(used,used+size));
  bytes+=size;
  return true;
 }
};
struct Input {
 AVFormatContext *format=nullptr;
 AVCodecContext *video=nullptr,*audio=nullptr;
 AVFrame *frame=av_frame_alloc();
 AVPacket *packet=av_packet_alloc();
 ~Input(){av_packet_free(&packet);av_frame_free(&frame);avcodec_free_context(&video);avcodec_free_context(&audio);avformat_close_input(&format);}
};
void require(int code){if(code<0)throw std::runtime_error("Cannot decode range");}
void openDecoder(AVCodecContext *&context,const AVStream *stream){
 const auto *codec=avcodec_find_decoder(stream->codecpar->codec_id);
 if(!codec)throw std::runtime_error("No range decoder");
 context=avcodec_alloc_context3(codec);
 if(!context)throw std::bad_alloc();
 require(avcodec_parameters_to_context(context,stream->codecpar));
 context->thread_count=1;
 require(avcodec_open2(context,codec,nullptr));
}
}

std::shared_ptr<Cache> prepare(const std::string &path,int64_t in,int64_t out,std::atomic<bool> &cancel){
 if(in<=0||out<=in||out-in>2000)return {};
 Input reader;
 reader.format=avformat_alloc_context();
 if(!reader.format||!reader.frame||!reader.packet)return {};
 reader.format->interrupt_callback={[](void *data){return int(static_cast<std::atomic<bool>*>(data)->load());},&cancel};
 AVDictionary *options=nullptr;
 av_dict_set(&options,"protocol_whitelist","file",0);
 const int opened=avformat_open_input(&reader.format,path.c_str(),nullptr,&options);
 av_dict_free(&options);
 require(opened);
 require(avformat_find_stream_info(reader.format,nullptr));
 const int v=av_find_best_stream(reader.format,AVMEDIA_TYPE_VIDEO,-1,-1,nullptr,0);
 const int a=av_find_best_stream(reader.format,AVMEDIA_TYPE_AUDIO,-1,-1,nullptr,0);
 if(v<0)return {};
 const auto *video=reader.format->streams[v];
 openDecoder(reader.video,video);
 if(a>=0)openDecoder(reader.audio,reader.format->streams[a]);
 const AVRational ns={1,1000000000};
 const int64_t origin=video->start_time==AV_NOPTS_VALUE?0:av_rescale_q(video->start_time,video->time_base,ns);
 const int64_t first=origin+in*1000000,last=origin+out*1000000;
 require(av_seek_frame(reader.format,v,av_rescale_q_rnd(first,ns,video->time_base,AV_ROUND_DOWN),AVSEEK_FLAG_BACKWARD));
 auto cache=std::make_shared<CacheStorage>();
 bool videoDone=false,audioDone=a<0;
 auto read=[&](AVCodecContext *decoder,int index,AVPacket *packet){
  require(avcodec_send_packet(decoder,packet));
  for(;;){
   const int result=avcodec_receive_frame(decoder,reader.frame);
   if(result==AVERROR(EAGAIN)||result==AVERROR_EOF)return;
   require(result);
   const auto &f=*reader.frame;
   if(f.best_effort_timestamp==AV_NOPTS_VALUE)throw std::runtime_error("Missing range timestamp");
   const auto pts=av_rescale_q(f.best_effort_timestamp,reader.format->streams[index]->time_base,ns);
   if(index==v){
    if(pts>=last){videoDone=true;return;}
    const auto length=av_rescale_q(f.duration,video->time_base,ns);
    if(pts<first&&length<=first-pts){av_frame_unref(reader.frame);continue;}
    if(f.format!=AV_PIX_FMT_YUV420P||f.width<=0||f.height<=0||f.width>1920||f.height>1080||
       f.color_trc==AVCOL_TRC_SMPTE2084||f.color_trc==AVCOL_TRC_ARIB_STD_B67)
     throw std::runtime_error("Unsupported range video");
    if(cache->video.empty()&&pts>first)throw std::runtime_error("Range starts after In");
    if(!cache->reserve(size_t(f.width+31)/32*32*f.height+2*(size_t((f.width+1)/2+31)/32*32)*((f.height+1)/2)))
     throw std::runtime_error("Range cache limit");
    Video item{uint64_t(std::max<int64_t>(0,pts-first)),std::unique_ptr<obs_source_frame,DestroyFrame>(obs_source_frame_create(VIDEO_FORMAT_I420,f.width,f.height))};
    auto *image=item.frame.get();
    image->full_range=f.color_range==AVCOL_RANGE_JPEG;
    image->trc=VIDEO_TRC_SRGB;
    video_format_get_parameters_for_format(f.colorspace==AVCOL_SPC_BT709?VIDEO_CS_709:VIDEO_CS_601,image->full_range?VIDEO_RANGE_FULL:VIDEO_RANGE_PARTIAL,VIDEO_FORMAT_I420,image->color_matrix,image->color_range_min,image->color_range_max);
    for(int p=0;p<3;++p){
     const int width=p?(f.width+1)/2:f.width,height=p?(f.height+1)/2:f.height;
     for(int y=0;y<height;++y)std::memcpy(image->data[p]+size_t(y)*image->linesize[p],f.data[p]+ptrdiff_t(y)*f.linesize[p],width);
    }
    cache->video.push_back(std::move(item));
   }else{
    if(pts>=last){audioDone=true;return;}
    const int channels=f.ch_layout.nb_channels,rate=f.sample_rate;
    if(f.format!=AV_SAMPLE_FMT_FLTP||channels<1||channels>2||rate<8000||rate>192000)
     throw std::runtime_error("Unsupported range audio");
    if(!cache->rate){
     const auto size=size_t((out-in)*rate/1000);
     if(!cache->reserve(size*channels*sizeof(float)))throw std::runtime_error("Range cache limit");
     cache->rate=rate;cache->channels=channels;
     for(int ch=0;ch<channels;++ch)cache->audio[ch].resize(size);
    }
    if(cache->rate!=uint32_t(rate)||cache->channels!=uint32_t(channels))throw std::runtime_error("Range audio changed");
    const auto indexAt=av_rescale_rnd(pts-first,rate,1000000000,AV_ROUND_NEAR_INF);
    const auto skip=std::max<int64_t>(0,-indexAt);
    const auto start=std::max<int64_t>(0,indexAt);
    const auto count=std::min<int64_t>(f.nb_samples-skip,int64_t(cache->audio[0].size())-start);
    if(count>0)for(int ch=0;ch<channels;++ch)
     std::copy_n(reinterpret_cast<const float*>(f.extended_data[ch])+skip,count,cache->audio[ch].begin()+start);
   }
   av_frame_unref(reader.frame);
  }
 };
 while(!cancel&&!(videoDone&&audioDone)){
  const int result=av_read_frame(reader.format,reader.packet);
  if(result==AVERROR_EOF){
   if(!videoDone)read(reader.video,v,nullptr);
   if(!audioDone)read(reader.audio,a,nullptr);
   break;
  }
  require(result);
  if(reader.packet->stream_index==v&&!videoDone)read(reader.video,v,reader.packet);
  if(reader.packet->stream_index==a&&!audioDone)read(reader.audio,a,reader.packet);
  av_packet_unref(reader.packet);
 }
 if(cancel||cache->video.size()<2||(a>=0&&!cache->rate))return {};
 return cache;
}
}
