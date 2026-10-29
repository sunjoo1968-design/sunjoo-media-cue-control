#include "preview-decoder.hpp"
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cmath>
#include <chrono>
static int passes=0,fails=0;
static void check(bool value,const char *label){printf("%s: %s\n",value?"PASS":"FAIL",label);value?++passes:++fails;}
int main(int argc,char **argv){
 if(argc!=2)return 2;auto root=std::filesystem::absolute(argv[1]);std::atomic<bool> stop{false};
 try{
  cue_editor::PreviewDecoder decoder(stop);decoder.open((root/"editor-test.mp4").u8string());check(decoder.durationMs>=11900&&decoder.durationMs<12200,"Video-only editor reads file duration");
  auto first=decoder.frameAt(0,[]{return false;});check(first.width==320&&first.height==180&&first.timeMs==0&&!first.rgba.empty(),"Editor decodes first video frame without OBS source");
  auto middle=decoder.frameAt(6000,[]{return false;});check(std::abs(middle.timeMs-6000)<35&&middle.rgba!=first.rgba,"Precise seek decodes past long-GOP keyframe to requested frame");
  std::ifstream reference(root/"editor-6s.rgba",std::ios::binary);std::vector<uint8_t> raw((std::istreambuf_iterator<char>(reference)),{});double error=1000;if(raw.size()==middle.rgba.size()&&!raw.empty()){error=0;for(size_t i=0;i<raw.size();++i)error+=std::abs(int(raw[i])-middle.rgba[i]);error/=raw.size();}
  printf("Independent FFmpeg reference error=%.4f\n",error);check(error<1.0,"Editor preview matches independent decoded frame reference");
  auto reverse=decoder.frameAt(1000,[]{return false;});check(std::abs(reverse.timeMs-1000)<35,"Reverse timeline scrubbing seeks correctly");
  auto cancel=decoder.frameAt(11000,[]{return true;});check(cancel.rgba.empty(),"Superseded scrub request can cancel decoding");
  auto after=decoder.frameAt(2000,[]{return false;});check(std::abs(after.timeMs-2000)<35,"Decoder recovers after cancelled request");
  auto end=decoder.frameAt(decoder.durationMs-1,[]{return false;});check(end.timeMs>decoder.durationMs-100&&end.timeMs<decoder.durationMs,"Out at file end shows final available frame");
  bool seeks=true;for(int i=0;i<20;++i){int target=(i*617)%11000;auto frame=decoder.frameAt(target,[]{return false;});if(frame.rgba.empty()||std::abs(frame.timeMs-target)>35)seeks=false;}check(seeks,"20 rapid forward/reverse seeks return corresponding frames");
  {cue_editor::PreviewDecoder fractional(stop);fractional.open((root/"fractional-test.mp4").u8string());auto shown=fractional.frameAt(8175,[]{return false;});auto previous=fractional.frameBefore(shown.timeMs,[]{return false;});printf("Previous regression shown=%lld previous=%lld\n",(long long)shown.timeMs,(long long)previous.timeMs);check(previous.timeMs<shown.timeMs&&previous.rgba!=shown.rgba,"Previous-frame click really moves backward at fractional frame rate");bool reverse=true;auto current=shown;for(int i=0;i<20;++i){auto prior=fractional.frameBefore(current.timeMs,[]{return false;});if(prior.timeMs>=current.timeMs||prior.rgba==current.rgba)reverse=false;current=std::move(prior);}check(reverse,"20 previous-frame steps move to distinct earlier 29.97fps frames");auto next=fractional.frameAt(previous.nextTimeMs,[]{return false;});check(next.timeMs==shown.timeMs&&next.rgba==shown.rgba,"Backward then forward returns to the exact original fractional-rate frame");}
  for(const char *name:{"cue-test.mov","cue-test.webm","cue-test-vp8.webm"}){cue_editor::PreviewDecoder other(stop);other.open((root/name).u8string());auto frame=other.frameAt(1500,[]{return false;});check(!frame.rgba.empty()&&std::abs(frame.timeMs-1500)<70,name);}
  bool missing=false;try{cue_editor::PreviewDecoder invalid(stop);invalid.open((root/"missing.mp4").u8string());}catch(...){missing=true;}check(missing,"Missing file reports error instead of changing OBS source");
  stop=true;auto stopped=decoder.frameAt(1000,[]{return false;});check(stopped.rgba.empty(),"Closing editor interrupts outstanding frame request");
 }catch(const std::exception &e){printf("Exception: %s\n",e.what());check(false,"Preview decoding completes");}
 printf("PREVIEW COMPLETE passes=%d failures=%d\n",passes,fails);return fails?1:0;
}
