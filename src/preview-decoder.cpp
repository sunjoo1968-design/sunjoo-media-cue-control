// Copyright (C) 2026 SunjooAn. SPDX-License-Identifier: GPL-2.0-or-later.
#include "preview-decoder.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace cue_editor {
static void require(int code,const char *what){if(code>=0)return;char message[AV_ERROR_MAX_STRING_SIZE]={};av_strerror(code,message,sizeof(message));throw std::runtime_error(std::string(what)+": "+message);}
PreviewDecoder::~PreviewDecoder(){sws_freeContext(scale_);av_frame_free(&last_);av_frame_free(&frame_);av_packet_free(&packet_);avcodec_free_context(&codec_);avformat_close_input(&format_);}
void PreviewDecoder::open(const std::string &path){
 format_=avformat_alloc_context();if(!format_)throw std::bad_alloc();
 format_->interrupt_callback={[](void *p){return static_cast<std::atomic<bool>*>(p)->load()?1:0;},&stop_};
 AVDictionary *options=nullptr;av_dict_set(&options,"protocol_whitelist","file,crypto,data",0);
 int code=avformat_open_input(&format_,path.c_str(),nullptr,&options);av_dict_free(&options);require(code,"Open video");require(avformat_find_stream_info(format_,nullptr),"Read video information");
 stream_=av_find_best_stream(format_,AVMEDIA_TYPE_VIDEO,-1,-1,nullptr,0);require(stream_,"Find video stream");
 auto *stream=format_->streams[stream_];auto *decoder=avcodec_find_decoder(stream->codecpar->codec_id);if(!decoder)throw std::runtime_error("Unsupported video codec");
 codec_=avcodec_alloc_context3(decoder);if(!codec_)throw std::bad_alloc();require(avcodec_parameters_to_context(codec_,stream->codecpar),"Video parameters");codec_->thread_count=1;require(avcodec_open2(codec_,decoder,nullptr),"Open video decoder");
 if(codec_->width<=0||codec_->height<=0||codec_->width>8192||codec_->height>8192)throw std::runtime_error("Video dimensions must be 1..8192");
 start_=stream->start_time==AV_NOPTS_VALUE?0:stream->start_time;
 durationMs=stream->duration!=AV_NOPTS_VALUE?av_rescale_q(stream->duration,stream->time_base,{1,1000}):format_->duration!=AV_NOPTS_VALUE?format_->duration/1000:0;
 if(durationMs<=0)throw std::runtime_error("Video duration is unavailable");
 auto rate=av_guess_frame_rate(format_,stream,nullptr);if(rate.num>0&&rate.den>0)frameMs=1000.0*rate.den/rate.num;
 frameMs=std::clamp(frameMs,1.0,10000.0);
 packet_=av_packet_alloc();frame_=av_frame_alloc();last_=av_frame_alloc();if(!packet_||!frame_||!last_)throw std::bad_alloc();
}
Picture PreviewDecoder::convert(AVFrame *f){
 if(f->color_trc==AVCOL_TRC_SMPTE2084||f->color_trc==AVCOL_TRC_ARIB_STD_B67)throw std::runtime_error("HDR preview is not supported by this SDR editor");
 Picture picture;double factor=std::min({1.0,960.0/f->width,540.0/f->height});picture.width=std::max(1,int(f->width*factor));picture.height=std::max(1,int(f->height*factor));
 auto pts=f->best_effort_timestamp;picture.timeMs=pts==AV_NOPTS_VALUE?0:std::max<int64_t>(0,av_rescale_q(pts-start_,format_->streams[stream_]->time_base,{1,1000}));
 picture.nextTimeMs=pts!=AV_NOPTS_VALUE&&f->duration>0?av_rescale_q(pts-start_+f->duration,format_->streams[stream_]->time_base,{1,1000}):picture.timeMs+int64_t(std::ceil(frameMs));
 picture.nextTimeMs=std::max(picture.timeMs+1,picture.nextTimeMs);
 picture.rgba.resize(size_t(picture.width)*picture.height*4);
 scale_=sws_getCachedContext(scale_,f->width,f->height,static_cast<AVPixelFormat>(f->format),picture.width,picture.height,AV_PIX_FMT_RGBA,SWS_BILINEAR,nullptr,nullptr,nullptr);if(!scale_)throw std::runtime_error("Preview conversion failed");
 const auto *coeff=sws_getCoefficients(f->colorspace==AVCOL_SPC_BT709?SWS_CS_ITU709:SWS_CS_DEFAULT);sws_setColorspaceDetails(scale_,coeff,f->color_range==AVCOL_RANGE_JPEG,coeff,1,0,1<<16,1<<16);
 uint8_t *data[]={picture.rgba.data(),nullptr,nullptr,nullptr};int stride[]={picture.width*4,0,0,0};require(sws_scale(scale_,f->data,f->linesize,0,f->height,data,stride),"Convert preview");return picture;
}
Picture PreviewDecoder::frameAt(int64_t ms,const std::function<bool()> &cancel){return seek(ms,false,cancel);}
Picture PreviewDecoder::frameBefore(int64_t ms,const std::function<bool()> &cancel){return ms<=0?seek(0,false,cancel):seek(ms,true,cancel);}
Picture PreviewDecoder::seek(int64_t ms,bool before,const std::function<bool()> &cancel){
 ms=std::clamp<int64_t>(ms,0,durationMs-1);auto *stream=format_->streams[stream_];
 auto anchor=before?std::max<int64_t>(0,ms-int64_t(std::ceil(frameMs*2))):ms;
 auto target=start_+av_rescale_q_rnd(anchor,{1,1000},stream->time_base,AV_ROUND_DOWN);
 require(av_seek_frame(format_,stream_,target,AVSEEK_FLAG_BACKWARD),"Seek preview");avcodec_flush_buffers(codec_);av_frame_unref(last_);av_packet_unref(packet_);bool eof=false;
 while(!stop_&&!cancel()){
  int code=avcodec_receive_frame(codec_,frame_);
  if(code>=0){
   auto pts=frame_->best_effort_timestamp;int64_t time=pts==AV_NOPTS_VALUE?ms:av_rescale_q(pts-start_,stream->time_base,{1,1000});
   auto end=pts!=AV_NOPTS_VALUE&&frame_->duration>0?av_rescale_q(pts-start_+frame_->duration,stream->time_base,{1,1000}):time+int64_t(std::ceil(frameMs));
   if(before&&time>=ms){auto picture=convert(last_->data[0]?last_:frame_);av_frame_unref(frame_);return picture;}
   av_frame_unref(last_);require(av_frame_ref(last_,frame_),"Cache preview frame");
   // Select the decoded frame containing the requested time, not just a keyframe.
   if(!before&&std::max(time+1,end)>ms){auto picture=convert(frame_);av_frame_unref(frame_);return picture;}
   av_frame_unref(frame_);continue;
  }
  if(code==AVERROR_EOF){if(last_->data[0])return convert(last_);break;}
  if(code!=AVERROR(EAGAIN))require(code,"Decode preview");
  if(eof)break;
  do {code=av_read_frame(format_,packet_);if(code<0||packet_->stream_index==stream_)break;av_packet_unref(packet_);}while(!stop_&&!cancel());
  if(stop_||cancel()){av_packet_unref(packet_);return {};}
  if(code==AVERROR_EOF){eof=true;require(avcodec_send_packet(codec_,nullptr),"Finish preview");}
  else {require(code,"Read preview packet");require(avcodec_send_packet(codec_,packet_),"Submit preview packet");av_packet_unref(packet_);}
 }
 return {};
}
}
