#include "waveform.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <future>
#include <thread>
static int passes=0,fails=0;
static void check(bool ok,const char *text){printf("%s: %s\n",ok?"PASS":"FAIL",text);ok?++passes:++fails;}
static float peak(const cue_editor::Waveform &w){return w.peaks.empty()?0:*std::max_element(w.peaks.begin(),w.peaks.end());}
int main(int argc,char **argv){
 if(argc!=2)return 2;auto root=std::filesystem::absolute(argv[1]);auto never=[]{return false;};
 try{
  auto file=(root/"waveform-test.wav").u8string();
  auto wave=cue_editor::analyseWaveform(file,0,3000,never);
  check(wave.hasAudio&&wave.peaks.size()==3000,"Waveform bins reflect actual decoded audio at 1ms resolution");
  check(*std::max_element(wave.peaks.begin(),wave.peaks.begin()+990)==0,"Leading silence remains exactly zero");
  check(*std::max_element(wave.peaks.begin()+1010,wave.peaks.begin()+1990)>0.45f,"Opposite-phase stereo sound is preserved without downmix cancellation");
  check(*std::max_element(wave.peaks.begin()+2010,wave.peaks.end())==0,"Trailing silence remains exactly zero");
  auto silence=cue_editor::analyseWaveform(file,100,900,never);check(peak(silence)==0&&silence.startMs==100&&silence.endMs==900,"Zoomed silent interval has aligned timestamps");
  auto sound=cue_editor::analyseWaveform(file,1100,1150,never);check(peak(sound)>0.45f&&sound.peaks.size()==50,"Zoom reanalyses actual samples at fine resolution");
  auto bounded=cue_editor::analyseWaveform(file,0,604800000,never);check(bounded.peaks.size()==32768,"Even week-long views use at most 128KiB of peaks");
  for(auto name:{"cue-test.mp4","cue-test.mov","cue-test.webm","cue-test-vp8.webm"}){auto w=cue_editor::analyseWaveform((root/name).u8string(),0,3000,never);check(w.hasAudio&&peak(w)>0.01f,name);}
  auto noAudio=cue_editor::analyseWaveform((root/"no-audio.mp4").u8string(),0,3000,never);check(!noAudio.hasAudio,"Video with no audio is a supported empty waveform");
  check(cue_editor::analyseWaveform(file,0,3000,[]{return true;}).peaks.empty(),"Already cancelled request does not open a decoder");
  std::atomic<int> polls{0};auto cancelled=cue_editor::analyseWaveform(file,0,3000,[&]{return ++polls>10;});check(cancelled.peaks.empty(),"Cancellation during packet decoding discards partial result");
  check(peak(cue_editor::analyseWaveform(file,1000,2000,never))>0.45f,"Fresh request recovers after cancellation");
  bool missing=false;try{cue_editor::analyseWaveform((root/"missing.wav").u8string(),0,1000,never);}catch(...){missing=true;}check(missing,"Missing file returns a handled failure");
  bool repeat=true;for(int i=0;i<24;++i)repeat&=peak(cue_editor::analyseWaveform(file,1000,2000,never))>0.45f;check(repeat,"24 repeated open/decode/close cycles preserve audio peaks");
  auto first=std::async(std::launch::async,[&]{return cue_editor::analyseWaveform(file,0,3000,never);});auto second=std::async(std::launch::async,[&]{return cue_editor::analyseWaveform(file,0,3000,never);});check(first.get().peaks==second.get().peaks,"Concurrent editor analyses serialize without corrupting results");
 }catch(const std::exception &e){printf("ERROR %s\n",e.what());++fails;}
 printf("COMPLETE passes=%d failures=%d\n",passes,fails);return fails?1:0;
}
