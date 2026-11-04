#include <obs-module.h>
#include <mutex>
#include <string>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <memory>
#include <future>
#include <unordered_set>
#include <unordered_map>
#include <util/platform.h>
#include "editor.hpp"
#include "preview-decoder.hpp"
#include "range-cache.hpp"

OBS_DECLARE_MODULE()
MODULE_EXPORT const char *obs_module_name(void) { return "Sunjoo Media Cue Control " FILTER_VERSION; }
MODULE_EXPORT const char *obs_module_description(void) { return "Visible media preview cue / play on transition. SunjooAn " FILTER_VERSION; }
MODULE_EXPORT const char *obs_module_author(void) { return "SunjooAn"; }

namespace {
thread_local const void *rangeAudioSender=nullptr;
bool programUsesCut(){
 auto *transition=obs_get_output_source(0);
 const bool cut=transition&&strcmp(obs_source_get_unversioned_id(transition),"cut_transition")==0;
 obs_source_release(transition);
 return cut;
}
constexpr const char *options[] = {"restart_on_activate", "close_when_inactive", "clear_on_media_end"};
// Video-thread-only scheduler: spread native decoder destruction across frames.
uint64_t nextIdleRelease=0;
// Built once per video frame (or topology change), shared by every controller.
// UUIDs and weak scene watches never keep an unused decoder alive.
struct VisibilityCache {
 std::atomic<uint64_t> revision{1},queries{0},scans{0};
 uint64_t frame=0,builtRevision=0;
 std::unordered_set<std::string> visible;
 struct Watch {obs_weak_source_t *weak;signal_handler_t *signals;};
 std::unordered_map<std::string,Watch> watches;
 static void changed(void *data,calldata_t*){++static_cast<VisibilityCache*>(data)->revision;}
 void unwatch(const Watch &w){for(auto key:{"item_visible","item_add","item_remove"})signal_handler_disconnect(w.signals,key,changed,this);obs_weak_source_release(w.weak);}
 void watch(obs_source_t *source){auto id=std::string(obs_source_get_uuid(source));auto old=watches.find(id);if(old!=watches.end()){if(!obs_weak_source_expired(old->second.weak))return;unwatch(old->second);watches.erase(old);}Watch w{obs_source_get_weak_source(source),obs_source_get_signal_handler(source)};for(auto key:{"item_visible","item_add","item_remove"})signal_handler_connect_ref(w.signals,key,changed,this);watches.emplace(id,w);}
 // Keep only the handler alive for disconnection; never acquire scene/source
 // strong references while pruning or shutting down.
 void prune(bool all=false){for(auto i=watches.begin();i!=watches.end();){if(all||obs_weak_source_expired(i->second.weak)){unwatch(i->second);i=watches.erase(i);}else ++i;}}
 void connect(bool enable){for(auto key:{"source_activate","source_deactivate","source_show","source_hide","source_transition_start","source_transition_stop","channel_change","source_remove","source_destroy"}){if(enable)signal_handler_connect(obs_get_signal_handler(),key,changed,this);else signal_handler_disconnect(obs_get_signal_handler(),key,changed,this);}if(!enable){prune(true);visible.clear();}}
} visibility;
struct VisibleSearch {std::unordered_set<obs_source_t*> visited;unsigned depth=0;};
void visibleSource(obs_source_t *source,VisibleSearch &search);
bool visibleItem(obs_scene_t*,obs_sceneitem_t *item,void *param) {
 auto &search=*static_cast<VisibleSearch*>(param);
 if(!obs_sceneitem_visible(item))return true;
 if(search.depth<64) {
  ++search.depth;visibleSource(obs_sceneitem_get_source(item),search);--search.depth;
 }
 return true;
}
void visibleSource(obs_source_t *source,VisibleSearch &search) {
 if(!source||search.depth>=64||!search.visited.insert(source).second)return;
 visibility.visible.insert(obs_source_get_uuid(source));
 auto *scene=obs_scene_from_source(source);
 if(!scene)scene=obs_group_from_source(source);
 if(scene){visibility.watch(source);obs_scene_enum_items(scene,visibleItem,&search);}
 else if(obs_source_get_type(source)==OBS_SOURCE_TYPE_TRANSITION) {
  ++search.depth;
  for(auto side:{OBS_TRANSITION_SOURCE_A,OBS_TRANSITION_SOURCE_B}) {
   auto *child=obs_transition_get_source(source,side);visibleSource(child,search);obs_source_release(child);
  }
  --search.depth;
 }
}
bool visibleScene(void *param,obs_source_t *source) {
 auto &search=*static_cast<VisibleSearch*>(param);
 // enum_scenes includes groups. A group's retained showing count must not
 // bypass the eye state of its parent item by treating it as a root view.
 if(obs_source_showing(source)&&obs_scene_from_source(source))visibleSource(source,search);
 return true;
}
bool visibleInScene(obs_source_t *source) {
 ++visibility.queries;const auto frame=obs_get_video_frame_time(),revision=visibility.revision.load();
 if(visibility.frame!=frame||visibility.builtRevision!=revision){
  visibility.frame=frame;visibility.builtRevision=revision;visibility.visible.clear();VisibleSearch search;
  // Studio Mode program copies are private and absent from enum_scenes.
  auto *output=obs_get_output_source(0);visibleSource(output,search);obs_source_release(output);
  obs_enum_scenes(visibleScene,&search);++visibility.scans;
  if(visibility.scans%256==0)visibility.prune();
 }
 return visibility.visible.count(obs_source_get_uuid(source))!=0;
}
struct Control {
 struct StillFrame {obs_source_frame *frame;int64_t target;int64_t cachedOut=0;std::shared_ptr<cue_range::Cache> range;StillFrame(const obs_source_frame *src,int64_t at):frame(obs_source_frame_create(src->format,src->width,src->height)),target(at){obs_source_frame_copy(frame,src);}~StillFrame(){obs_source_frame_destroy(frame);}};
 obs_source_t *self;
 obs_weak_source_t *parent = nullptr;
 std::recursive_mutex mutex;
 bool managed = false, wasActive = false, wasShowing = false, armed = false, enabled = false;
 bool attachedOnProgram = false;
 bool localFile = false;
 bool nativeUnbuffered = false;
 bool canCacheRange = false;
 uint64_t resumeBufferAfterFrame = 0;
 uint64_t inspectedParentRevision = UINT64_MAX;
 bool inspectedTrimmed = false;
 std::atomic<bool> mutePreview{false};
 std::atomic<int64_t> audioOut{0};
 std::atomic<bool> preparing{false};
 bool priming=false, rangeEnded=false, offStopped=false, loopManaged=false, invalidRange=false, waitingRestart=false;
 int64_t inMs=0,outMs=0,cueIn=0,runIn=0,runOut=0;
 uint64_t revision=1,appliedRevision=0,prepareAt=0;
 std::string path;
 uint64_t idleAt=0,refreshRevision=0;
 std::atomic<uint64_t> parentRevision{0};
 int idlePhase=0;bool unloaded=false;
 std::string idleOptions;
 std::atomic<bool> programHold{false},stopRequest{false},playRequest{false},restartRequest{false};
 std::atomic<unsigned> ownStops{0},ownPlays{0},ownRestarts{0},ownPauses{0};
 bool playAfterCue=false;std::atomic<bool> releasingZero{false};
 std::shared_ptr<StillFrame> still;
 std::atomic<bool> cancelPoster{false};
 std::future<std::shared_ptr<StillFrame>> posterJob;
 uint64_t posterGeneration=0,failedPosterGeneration=0;
 std::atomic<uint64_t> captureGeneration{0},prepareRequests{0},prepares{0},coalesced{0};
 std::atomic<bool> ended{false};
 std::atomic<int64_t> captureIn{0};
 std::atomic<int64_t> preloadIn{-1};
 uint64_t settleAt=0;int64_t stillIn=-1;bool reuseStill=false,seekPending=false;int zeroFallback=0;
 std::shared_ptr<cue_range::Cache> playingRange;
 uint64_t rangeStart=0;
 size_t rangePicture=0,rangeSample=0;
 std::atomic<bool> freezeVideo{false};std::atomic<uint64_t> videoSerial{0},posterTs{UINT64_MAX};uint64_t warmSerial=0;
 void showStill(obs_source_t *source){auto target=captureIn.load();auto image=std::atomic_load(&still);if(image&&image->target!=target)image.reset();if(image){posterTs=image->frame->timestamp;obs_source_output_video(source,image->frame);obs_source_set_video_frame(source,image->frame);preloadIn=target;}else if(preloadIn==target)obs_source_show_preloaded_video(source);}
 obs_source_frame *video(obs_source_frame *frame){
  if(frame&&frame->timestamp!=posterTs)++videoSerial;
  if((programHold||freezeVideo)&&frame){obs_source_release_frame(obs_filter_get_parent(self),frame);return nullptr;}
  return frame;
 }
 static bool consume(std::atomic<unsigned> &count){auto n=count.load();while(n){if(count.compare_exchange_weak(n,n-1))return true;}return false;}
 void playNative(obs_source_t *source,bool pause){
  if(pause){
   if(ownPauses)return;
   ++ownPauses;
  }else{
   ++ownPlays;
   // Keep draining stale cue timestamps until new decoder frames arrive.
   resumeBufferAfterFrame=videoSerial.load()+2;
   obs_source_set_async_unbuffered(source,true);
  }
  obs_source_media_play_pause(source,pause);
 }
 void restartNative(obs_source_t *source){++ownRestarts;obs_source_media_restart(source);}
 void stopNative(obs_source_t *source){++ownStops;obs_source_media_stop(source);}
 void releaseHold(obs_source_t *source){
  // Restart zero-cues on Play so tick-sized silent preroll is never skipped.
  // Keep the poster gate until the native restart clears async_active.
  ended=false;freezeVideo=false;playAfterCue=false;if(cueIn==0){releasingZero=true;restartNative(source);}else programHold=false;
  playNative(source,false);
 }
 void outputPreparedRange(obs_source_t *source){
  const auto tick=obs_get_video_frame_time();
  const auto interval=obs_get_frame_interval_ns();
  const auto elapsed=tick-rangeStart;
  const auto duration=uint64_t(runOut-runIn)*1000000;
  const auto &cache=*playingRange;
  if(elapsed>=duration){
   rangeEnded=true;
   playingRange.reset();
   return;
  }
  if(elapsed){
   const auto previous=rangePicture;
   while(rangePicture+1<cache.video.size()&&cache.video[rangePicture+1].offset<=elapsed+interval/2)++rangePicture;
   if(rangePicture!=previous){
    auto *frame=cache.video[rangePicture].frame.get();
    frame->timestamp=tick;
    obs_source_set_video_frame(source,frame);
   }
  }
  if(!cache.rate)return;
  const size_t until=std::min(cache.audio[0].size(),size_t((elapsed+interval)*cache.rate/1000000000));
  if(until<=rangeSample)return;
  obs_source_audio audio={};
  audio.format=AUDIO_FORMAT_FLOAT_PLANAR;
  audio.samples_per_sec=cache.rate;
  audio.speakers=cache.channels==1?SPEAKERS_MONO:SPEAKERS_STEREO;
  audio.frames=uint32_t(until-rangeSample);
  audio.timestamp=rangeStart+rangeSample*1000000000ull/cache.rate;
  for(size_t ch=0;ch<cache.channels;++ch)audio.data[ch]=reinterpret_cast<const uint8_t*>(cache.audio[ch].data()+rangeSample);
  rangeAudioSender=this;
  obs_source_output_audio(source,&audio);
  rangeAudioSender=nullptr;
  rangeSample=until;
 }
 static void stopped(void *data,calldata_t *event){
  auto *c=static_cast<Control*>(data);
  auto *source=static_cast<obs_source_t*>(calldata_ptr(event,"source"));
  if(consume(c->ownStops)){if(source&&c->preparing&&c->captureIn==0&&c->preloadIn==0)c->showStill(source);return;}
  if(source&&obs_source_enabled(c->self)&&obs_source_active(source)){
   c->programHold=true;c->mutePreview=true;c->releasingZero=false;c->playRequest=false;c->restartRequest=false;c->stopRequest=true;
   // STOP clears async_active before emitting this signal. Expose the existing
   // seek/preload still immediately; the next tick silently re-cues the target.
   c->showStill(source);
  }
 }
 static void played(void *data,calldata_t*){auto *c=static_cast<Control*>(data);if(!consume(c->ownPlays))c->playRequest=true;}
 static void paused(void *data,calldata_t*){consume(static_cast<Control*>(data)->ownPauses);}
 static void finished(void *data,calldata_t *event){auto *c=static_cast<Control*>(data);auto *s=static_cast<obs_source_t*>(calldata_ptr(event,"source"));if(s&&!c->preparing&&!c->programHold&&obs_source_enabled(c->self)&&obs_source_active(s)){c->ended=true;c->mutePreview=true;}}
 static void restarted(void *data,calldata_t *event){auto *c=static_cast<Control*>(data);const bool internal=consume(c->ownRestarts);if(c->programHold){auto *source=static_cast<obs_source_t*>(calldata_ptr(event,"source"));if(source)c->showStill(source);if(internal&&c->releasingZero.exchange(false))c->programHold=false;}if(!internal){c->restartRequest=true;c->playRequest=true;}}
 static void parentUpdated(void *data,calldata_t*){++static_cast<Control*>(data)->parentRevision;}
 explicit Control(obs_source_t *source) : self(source) {proc_handler_add(obs_source_get_proc_handler(self),"void cue_metrics(out int visibility_queries, out int visibility_scans, out int prepare_requests, out int prepares, out int coalesced)",[](void *d,calldata_t *v){auto *c=static_cast<Control*>(d);calldata_set_int(v,"visibility_queries",visibility.queries);calldata_set_int(v,"visibility_scans",visibility.scans);calldata_set_int(v,"prepare_requests",c->prepareRequests);calldata_set_int(v,"prepares",c->prepares);calldata_set_int(v,"coalesced",c->coalesced);},this);}
 ~Control() { cancelPoster=true;if(posterJob.valid())posterJob.wait();detach(); }
 void attach(obs_source_t *source) {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  parent = obs_source_get_weak_source(source);
  inspectedParentRevision=UINT64_MAX;
  attachedOnProgram = obs_source_active(source);
  // Persist the original options with this filter so removal after a saved/reloaded
  // scene collection can restore the user's original settings.
  if (strcmp(obs_source_get_unversioned_id(source), "ffmpeg_source")) return;
  signal_handler_connect(obs_source_get_signal_handler(source),"update",parentUpdated,this);
  signal_handler_connect(obs_source_get_signal_handler(source),"media_stopped",stopped,this);
  signal_handler_connect(obs_source_get_signal_handler(source),"media_play",played,this);
  signal_handler_connect(obs_source_get_signal_handler(source),"media_restart",restarted,this);
  signal_handler_connect(obs_source_get_signal_handler(source),"media_ended",finished,this);
  signal_handler_connect(obs_source_get_signal_handler(source),"media_pause",paused,this);
  auto *saved = obs_source_get_settings(self);
  if (!obs_data_get_bool(saved, "saved_options")) {
   auto *settings = obs_source_get_settings(source);
   for (const char *key : options) obs_data_set_bool(saved, (std::string("original_")+key).c_str(), obs_data_get_bool(settings,key));
   obs_data_set_bool(saved,"saved_options",true);
   obs_data_set_bool(saved,"original_unbuffered",obs_source_async_unbuffered(source));
   obs_data_set_bool(saved,"original_looping",obs_data_get_bool(settings,"looping"));
   obs_data_release(settings);
  }
  // v0.1.0 scene collections predate range-loop management.
  if(!obs_data_has_user_value(saved,"original_looping")){auto *settings=obs_source_get_settings(source);obs_data_set_bool(saved,"original_looping",obs_data_get_bool(settings,"looping"));obs_data_release(settings);}
  nativeUnbuffered=obs_data_get_bool(saved,"original_unbuffered");
  obs_data_release(saved);
  auto *pending=obs_source_get_settings(self);
  if(obs_data_get_bool(pending,"idle_restore_pending")){
   idleOptions=obs_data_get_string(pending,"idle_original_options");
   refreshRevision=parentRevision.load();idlePhase=2;
   auto *settings=obs_source_get_settings(source);obs_data_set_string(settings,"ffmpeg_options",idleOptions.c_str());obs_data_set_bool(settings,"close_when_inactive",true);obs_data_set_bool(settings,"restart_on_activate",true);obs_source_update(source,settings);obs_data_release(settings);
  }
  obs_data_release(pending);
 }
 void restore(obs_source_t *source) {
  if (!managed&&!idlePhase&&!unloaded) return;
  const bool restoringIdle=unloaded||idlePhase!=0;
  auto *settings = obs_source_get_settings(source);
  auto *saved = obs_source_get_settings(self);
  if(idlePhase){if(std::string(obs_data_get_string(settings,"ffmpeg_options"))==idleOptions+" ")obs_data_set_string(settings,"ffmpeg_options",idleOptions.c_str());obs_data_set_bool(saved,"idle_restore_pending",false);idlePhase=0;}
  for (const char *key : options) {
   // Preserve a user change made while this controller was attached.
   if (restoringIdle||!obs_data_get_bool(settings,key))
    obs_data_set_bool(settings,key,obs_data_get_bool(saved,(std::string("original_")+key).c_str()));
  }
  if(loopManaged){obs_data_set_bool(settings,"looping",obs_data_get_bool(saved,"original_looping"));loopManaged=false;}
  obs_source_update(source,settings);
  obs_source_set_async_unbuffered(source,obs_data_get_bool(saved,"original_unbuffered"));
  obs_data_release(settings);obs_data_release(saved);managed=false;unloaded=false;idleAt=0;
 }
 void detach() {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (!parent) return;
  auto *source = obs_weak_source_get_source(parent);
  if (source) {auto *signals=obs_source_get_signal_handler(source);signal_handler_disconnect(signals,"update",parentUpdated,this);signal_handler_disconnect(signals,"media_stopped",stopped,this);signal_handler_disconnect(signals,"media_play",played,this);signal_handler_disconnect(signals,"media_restart",restarted,this);signal_handler_disconnect(signals,"media_ended",finished,this);signal_handler_disconnect(signals,"media_pause",paused,this);restore(source);obs_source_release(source);}
  obs_weak_source_release(parent);parent=nullptr;
 }
 void update(obs_data_t *settings) {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  auto value=[](double v)->int64_t{return std::isfinite(v)?int64_t(std::clamp(v,0.0,604800.0)*1000.0+0.5):0;};
  auto in=value(obs_data_get_double(settings,"in_seconds")),out=value(obs_data_get_double(settings,"out_seconds"));
  if(in!=inMs||out!=outMs){inMs=in;outMs=out;++revision;}
 }
 void prepare(obs_source_t *source,obs_media_state state,int64_t target,bool force=false) {
  ++prepareRequests;if(!force&&preparing&&!unloaded&&cueIn==target&&appliedRevision==revision){++coalesced;return;}++prepares;preparing=true;ended=false;mutePreview=true;freezeVideo=true;
  const auto previousStill=std::atomic_load(&still);
  reuseStill=stillIn==target&&previousStill&&previousStill->cachedOut==outMs;
  ++captureGeneration;captureIn=target;if(!reuseStill)std::atomic_store(&still,std::shared_ptr<StillFrame>{});
  cueIn=target;settleAt=0;zeroFallback=0;seekPending=true;warmSerial=videoSerial;
  waitingRestart=unloaded||state==OBS_MEDIA_STATE_ENDED||state==OBS_MEDIA_STATE_STOPPED||state==OBS_MEDIA_STATE_NONE;unloaded=false;
  if(target==0&&preloadIn==0){waitingRestart=false;seekPending=false;zeroFallback=3;stopNative(source);}
  else if(waitingRestart)restartNative(source);else if(state!=OBS_MEDIA_STATE_PLAYING)playNative(source,false);
  priming=true;armed=false;rangeEnded=false;prepareAt=os_gettime_ns();appliedRevision=revision;
 }
 void prepareTick(obs_source_t *source,bool active) {
  auto now=os_gettime_ns();auto state=obs_source_media_get_state(source);
  if(waitingRestart){if(now-prepareAt<80000000ull)return;waitingRestart=false;warmSerial=videoSerial;if(state!=OBS_MEDIA_STATE_PLAYING)playNative(source,false);prepareAt=now;return;}
  if(seekPending){
   if(state==OBS_MEDIA_STATE_ENDED||state==OBS_MEDIA_STATE_STOPPED||state==OBS_MEDIA_STATE_NONE){if(!ownRestarts)restartNative(source);waitingRestart=true;prepareAt=now;return;}
   if(state!=OBS_MEDIA_STATE_PLAYING){if(!ownPlays)playNative(source,false);return;}
   // Metadata PLAYING is not a decoder acknowledgement. Observe live frames
   // before seeking so FFmpeg has consumed its resume/reset-timestamp action.
   if(videoSerial<warmSerial+2&&now-prepareAt<500000000ull)return;
   ++captureGeneration;
   // A cached In poster must remain visible while the decoder seeks behind it.
   // Clearing async video here disables rendering even though the texture exists.
   if(reuseStill)showStill(source);
   else obs_source_output_video(source,nullptr);
   preloadIn=-1;obs_source_media_set_time(source,cueIn);seekPending=false;prepareAt=now;return;
  }
  // Frame zero uses the exact native paused preload, never a later frame
  // guessed from FFmpeg's prefetched clock. Stop then re-arm without seeking
  // again so decoder pacing is reset before the next real Play. The queued
  // Pause is acknowledged before publishing the cue as ready.
  if(zeroFallback){
   if(zeroFallback==1){if(state!=OBS_MEDIA_STATE_PAUSED){playNative(source,true);return;}obs_source_media_set_time(source,0);zeroFallback=2;prepareAt=now;return;}
   if(zeroFallback==2){if(now-prepareAt<80000000ull)return;obs_source_output_video(source,nullptr);obs_source_show_preloaded_video(source);preloadIn=0;stopNative(source);zeroFallback=3;prepareAt=now;return;}
   if(zeroFallback==3){if(ownStops||now-prepareAt<50000000ull||obs_source_media_get_time(source)>150)return;restartNative(source);playNative(source,true);zeroFallback=4;prepareAt=now;return;}
   if(ownPauses)return;
   // A late native Stop completion or user Play can overwrite PAUSED metadata.
   // Reassert the silent hold; zero playback always restarts both streams.
   if(state!=OBS_MEDIA_STATE_PAUSED){playNative(source,true);return;}
   obs_source_output_video(source,nullptr);obs_source_show_preloaded_video(source);preparing=false;armed=!active;if(active){programHold=true;releaseHold(source);}return;
  }
  if(cueIn==0){if(now-prepareAt<200000000ull)return;zeroFallback=1;playNative(source,true);return;}
  if(now-prepareAt<20000000ull)return;
  auto pos=obs_source_media_get_time(source);
  if(pos<cueIn-20){if(state==OBS_MEDIA_STATE_PAUSED&&!ownPlays)playNative(source,false);return;}
  auto image=reuseStill?std::atomic_load(&still):std::shared_ptr<StillFrame>{};
  if(cueIn>0&&pos>cueIn+80&&now-prepareAt<500000000ull)return;
  if(!active&&!settleAt){playNative(source,true);settleAt=now+50000000ull;return;}
  if(!active&&(now<settleAt||state!=OBS_MEDIA_STATE_PAUSED)){if(state!=OBS_MEDIA_STATE_PAUSED)playNative(source,true);return;}
  if(!image){
   const auto generation=captureGeneration.load();
   if(posterJob.valid()){
    if(posterGeneration!=generation)cancelPoster=true;
    if(posterJob.wait_for(std::chrono::seconds(0))!=std::future_status::ready)return;
    auto decoded=posterJob.get();
    if(posterGeneration==generation){
     image=std::move(decoded);
     if(!image)failedPosterGeneration=generation;
    }
   }
   if(!image){
    if(failedPosterGeneration==generation)return;
    cancelPoster=false;
    posterGeneration=generation;
    try{
     posterJob=std::async(std::launch::async,[this,file=path,target=cueIn,out=outMs,prepareRange=canCacheRange&&!active&&!programHold]()->std::shared_ptr<StillFrame>{
      try{
       cue_editor::PreviewDecoder decoder(cancelPoster,true);
       decoder.open(file);
       auto picture=decoder.frameAt(target,[this]{return cancelPoster.load();});
       if(picture.rgba.empty()||target<picture.timeMs||target>=picture.nextTimeMs)return {};
       obs_source_frame frame={};
       frame.format=VIDEO_FORMAT_RGBA;frame.width=picture.width;frame.height=picture.height;
       frame.data[0]=picture.rgba.data();frame.linesize[0]=picture.width*4;frame.full_range=true;
       frame.timestamp=os_gettime_ns();
       auto stillFrame=std::make_shared<StillFrame>(&frame,target);
       stillFrame->cachedOut=out;
       if(prepareRange){
        try{stillFrame->range=cue_range::prepare(file,target,out,cancelPoster);}
        catch(const std::exception &){}
       }
       return stillFrame;
      }catch(const std::exception &){return {};}
     });
    }catch(const std::exception &){failedPosterGeneration=generation;}
    return;
   }
  }

  if(image){std::atomic_store(&still,image);stillIn=cueIn;showStill(source);}
  preparing=false;armed=!active;
  if(active){freezeVideo=false;playNative(source,false);}
 }
 void tick() {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  auto *source=parent?obs_weak_source_get_source(parent):nullptr;
  if(!source)return;
  const bool run=obs_source_enabled(self);
  if(!run) {
   playingRange.reset();
   mutePreview=false;
   restore(source);enabled=false;armed=false;wasActive=false;wasShowing=false;preparing=false;offStopped=false;idleAt=0;
   inspectedParentRevision=UINT64_MAX;
   programHold=false;stopRequest=false;playRequest=false;restartRequest=false;playAfterCue=false;releasingZero=false;ended=false;freezeVideo=false;
   obs_source_release(source);return;
  }
  if(strcmp(obs_source_get_unversioned_id(source),"ffmpeg_source")) {mutePreview=false;obs_source_release(source);return;}
  const bool activeCount=obs_source_active(source),showingCount=obs_source_showing(source);
  // Native source updates are deferred to video_tick. Wait for their actual
  // completion, not a guessed delay, including a return during destruction.
  if(idlePhase){
   mutePreview=true;
   if(parentRevision.load()!=refreshRevision){
    if(idlePhase==1){
     auto *settings=obs_source_get_settings(source);
     // Use the original options again while keeping an unused decoder closed.
     // If it returned, reopen first, then the normal silent cue path takes over.
     // Do not overwrite an explicit option edit made during the deferred close.
     if(std::string(obs_data_get_string(settings,"ffmpeg_options"))==idleOptions+" ")obs_data_set_string(settings,"ffmpeg_options",idleOptions.c_str());
     obs_data_set_bool(settings,"close_when_inactive",!(activeCount||showingCount));obs_data_set_bool(settings,"restart_on_activate",!(activeCount||showingCount));
     refreshRevision=parentRevision.load();idlePhase=2;obs_source_update(source,settings);obs_data_release(settings);
    }else{
     idlePhase=0;unloaded=true;auto *saved=obs_source_get_settings(self);obs_data_set_bool(saved,"idle_restore_pending",false);obs_data_release(saved);
     // Once closed, let the controller perform the only restart. Native
     // activate plus controller restart must not reset a new decoder twice.
     auto *settings=obs_source_get_settings(source);obs_data_set_bool(settings,"restart_on_activate",false);obs_source_update(source,settings);obs_data_release(settings);
    }
   }
   if(idlePhase){obs_source_release(source);return;}
  }
  // Dormant media cannot be visible. Avoid enumerating all scenes for every
  // hidden filter at every video tick (common in large broadcast collections).
  bool visible=(activeCount||showingCount)&&visibleInScene(source);
  bool active=activeCount&&visible,showing=showingCount&&visible;
  // Inspect scene-item visibility too: OBS can retain show/active counts briefly
  // while the eye-OFF audio transition is draining.
  if(!visible||(!showing&&!active)) {
   playingRange.reset();
   programHold=false;stopRequest=false;playRequest=false;restartRequest=false;playAfterCue=false;releasingZero=false;ended=false;freezeVideo=false;
   // A managed clip that leaves both buses must not continue decoding/audio.
   // Issue once on departure; do not repeatedly control unrelated hidden items.
   if(managed&&(wasShowing||wasActive||preparing)) {mutePreview=true;stopNative(source);offStopped=true;}
   // A very short activation can begin and end between controller ticks.
   // Stop any local decoder that escaped that observed-entry path as well.
   if(!activeCount&&!showingCount&&(!unloaded||obs_source_media_get_duration(source)>0)&&obs_source_media_get_state(source)==OBS_MEDIA_STATE_PLAYING){
    auto *hiddenSettings=obs_source_get_settings(source);if(obs_data_get_bool(hiddenSettings,"is_local_file")){mutePreview=true;stopNative(source);offStopped=true;}obs_data_release(hiddenSettings);
   }
   mutePreview=offStopped;preparing=false;priming=false;
   wasShowing=false;wasActive=false;armed=false;
   // Count five continuously unused seconds. Never release while any OBS view
   // or transition still holds showing/program references, even if an eye is off.
   if(activeCount||showingCount){idleAt=0;}
   else if(!unloaded){
    const auto now=os_gettime_ns();if(!idleAt)idleAt=now;
    // A never-opened native source needs no cleanup and is not a suspended
    // source. Preserve its normal first activation instead of restarting twice.
    if(obs_source_media_get_state(source)==OBS_MEDIA_STATE_NONE&&obs_source_media_get_duration(source)==0){obs_source_release(source);return;}
    if(now-idleAt>=5000000000ull&&now>=nextIdleRelease){
     auto *settings=obs_source_get_settings(source);
     if(obs_data_get_bool(settings,"is_local_file")){
      idleOptions=obs_data_get_string(settings,"ffmpeg_options");auto *saved=obs_source_get_settings(self);obs_data_set_string(saved,"idle_original_options",idleOptions.c_str());obs_data_set_bool(saved,"idle_restore_pending",true);obs_data_release(saved);
      // FFmpeg treats trailing spaces as option separators. A temporary extra
      // separator requests OBS's native reinitialization without changing file,
      // codec, rate, color range or source identity. Unlike STOP, reinitializing
      // also destroys an already stopped decoder. Restore exact options next.
      obs_data_set_string(settings,"ffmpeg_options",(idleOptions+" ").c_str());obs_data_set_bool(settings,"close_when_inactive",true);obs_data_set_bool(settings,"restart_on_activate",true);
      std::atomic_store(&still,std::shared_ptr<StillFrame>{});preloadIn=-1;++captureGeneration;
      stopNative(source);
      mutePreview=true;refreshRevision=parentRevision.load();idlePhase=1;managed=true;obs_source_update(source,settings);nextIdleRelease=now+100000000ull;
     }
     obs_data_release(settings);
    }
   }
   obs_source_release(source);return;
  }
  idleAt=0;
  bool changed=false;
  const auto parentVersion=parentRevision.load();
  const bool trimmed=active&&wasActive?(runIn>0||runOut>0):(inMs>0||outMs>0);
  if(inspectedParentRevision!=parentVersion||inspectedTrimmed!=trimmed||unloaded){
   inspectedParentRevision=parentVersion;
   inspectedTrimmed=trimmed;
   auto *settings=obs_source_get_settings(source);
   localFile=obs_data_get_bool(settings,"is_local_file");
   canCacheRange=localFile&&obs_data_get_int(settings,"speed_percent")==100&&
                 obs_data_get_int(settings,"color_range")==VIDEO_RANGE_DEFAULT&&
                 !*obs_data_get_string(settings,"ffmpeg_options")&&
                 !*obs_data_get_string(settings,"input_format");
   if(localFile){
    std::string current=obs_data_get_string(settings,"local_file");
    changed=current!=path;
    if(changed){ended=false;std::atomic_store(&still,std::shared_ptr<StillFrame>{});preloadIn=-1;++captureGeneration;stillIn=-1;captureIn=inMs;if(programHold)stopRequest=true;}
    path=std::move(current);
    bool settingsChanged=false;
    for(const char *key:options) if(obs_data_get_bool(settings,key)) {obs_data_set_bool(settings,key,false);settingsChanged=true;}
    if(trimmed) {
     if(obs_data_get_bool(settings,"looping")){obs_data_set_bool(settings,"looping",false);settingsChanged=true;}
     loopManaged=true;
    } else if(loopManaged) {
     auto *saved=obs_source_get_settings(self);obs_data_set_bool(settings,"looping",obs_data_get_bool(saved,"original_looping"));obs_data_release(saved);settingsChanged=true;loopManaged=false;
    }
    if(settingsChanged)obs_source_update(source,settings);
    managed=true;
   }else path.clear();
   obs_data_release(settings);
  }
  if(!localFile||path.empty()){mutePreview=false;armed=false;obs_source_release(source);return;}
  // Bypass the queue while cueing, then let OBS pace normal program playback.
  const bool unbuffered=!active||preparing||programHold||nativeUnbuffered||
                        videoSerial.load()<resumeBufferAfterFrame;
  if(obs_source_async_unbuffered(source)!=unbuffered)
   obs_source_set_async_unbuffered(source,unbuffered);
  auto state=obs_source_media_get_state(source);
  auto duration=obs_source_media_get_duration(source);
  // If a return raced the deferred close, OBS already opened this source at
  // frame zero. Preserve that fresh live playback instead of restarting again.
  if(unloaded&&active&&state==OBS_MEDIA_STATE_PLAYING&&duration>0&&inMs==0)unloaded=false;
  // Snapshot even a rejected range on entry. Otherwise the early invalid-range
  // return marks wasActive without updating runIn, and the next tick resurrects
  // the previous valid range and releases the audio gate.
  if(active&&!wasActive){runIn=inMs;runOut=outMs>0?(duration>0?std::min(outMs,duration):outMs):0;audioOut=runOut;rangeEnded=false;}
  auto checkedIn=active&&wasActive?runIn:inMs,checkedOut=active&&wasActive?runOut:outMs;
  invalidRange=(checkedOut>0&&checkedOut<=checkedIn)||(duration>0&&checkedIn>=duration);
  if(invalidRange){playNative(source,true);mutePreview=true;armed=false;preparing=false;enabled=true;wasShowing=showing;wasActive=active;obs_source_release(source);return;}
  offStopped=false;
  if(!active)playingRange.reset();
  if(active) {
   if(playingRange&&(stopRequest||playRequest||restartRequest))playingRange.reset();
   if(playingRange){
    outputPreparedRange(source);
    obs_source_release(source);
    return;
   }
   if(attachedOnProgram&&(runIn>0||runOut>0)&&!enabled&&!preparing&&!programHold){
    attachedOnProgram=false;
    programHold=true;
    prepare(source,state,inMs);
   }
   if(!programHold&&playRequest.exchange(false)){
    const bool eof=ended.load(),restart=restartRequest.exchange(false);const bool replay=rangeEnded||eof||restart;
    if(replay){programHold=true;playAfterCue=true;rangeEnded=false;runIn=inMs;runOut=outMs>0?(duration>0?std::min(outMs,duration):outMs):0;audioOut=runOut;preparing=false;prepare(source,eof?OBS_MEDIA_STATE_ENDED:state,inMs);}
   }
   if(stopRequest.exchange(false)){
    programHold=true;playAfterCue=false;preparing=false;rangeEnded=false;
    runIn=inMs;runOut=outMs>0?(duration>0?std::min(outMs,duration):outMs):0;audioOut=runOut;
    prepare(source,state,inMs);
   }
   if(programHold&&playRequest.exchange(false)){
    rangeEnded=false;runIn=inMs;runOut=outMs>0?(duration>0?std::min(outMs,duration):outMs):0;audioOut=runOut;
    if(restartRequest.exchange(false)&&inMs>0&&!preparing)prepare(source,state,inMs);
    if(appliedRevision!=revision&&!preparing)prepare(source,state,inMs);
    // A cue's queued Pause may run after the user's Play in the same native
    // action batch. Re-assert Play when releasing the hold so it cannot be lost.
    if(preparing)playAfterCue=true;else releaseHold(source);
   }
   if(programHold&&!preparing&&appliedRevision!=revision){runIn=inMs;runOut=outMs>0?(duration>0?std::min(outMs,duration):outMs):0;audioOut=runOut;prepare(source,state,inMs);}
   if(programHold){
    if(preparing)prepareTick(source,false);
    if(!preparing&&playAfterCue)releaseHold(source);
    mutePreview=programHold||preparing;armed=false;enabled=true;wasActive=true;wasShowing=showing;obs_source_release(source);return;
   }
   // Attaching/enabling on an already-live source must not rewind it.
   if(!wasActive) {
    if(armed&&!preparing){
     auto prepared=std::atomic_load(&still);
     if(canCacheRange&&cueIn>0&&prepared&&prepared->cachedOut==runOut&&prepared->range&&state==OBS_MEDIA_STATE_PAUSED&&programUsesCut()){
      playingRange=prepared->range;
      rangeStart=obs_get_video_frame_time();
      rangePicture=rangeSample=0;
      freezeVideo=true;mutePreview=true;armed=false;wasActive=true;wasShowing=showing;
      outputPreparedRange(source);
      obs_source_release(source);
      return;
     }
     if(cueIn==0){programHold=true;releaseHold(source);}else {freezeVideo=false;playNative(source,false);}
    }
    else if(!preparing&&(unloaded||(enabled&&state!=OBS_MEDIA_STATE_PLAYING)||inMs>0))prepare(source,state,inMs);
   }
   if(preparing)prepareTick(source,true);
   if(!preparing&&ended&&state==OBS_MEDIA_STATE_ENDED)playNative(source,true);
   if(!preparing&&!rangeEnded&&runOut>0&&obs_source_media_get_time(source)>=runOut){
    freezeVideo=true;
    playNative(source,true);
    rangeEnded=true;
   }
   if(rangeEnded&&state==OBS_MEDIA_STATE_PLAYING)playNative(source,true);
   mutePreview=preparing||rangeEnded||ended;
   armed=false;
  } else {
   attachedOnProgram=false;
   programHold=false;stopRequest=false;playRequest=false;restartRequest=false;playAfterCue=false;releasingZero=false;ended=false;
   if(!enabled||!wasShowing||wasActive||changed||appliedRevision!=revision||(!armed&&!preparing)||state==OBS_MEDIA_STATE_ENDED||state==OBS_MEDIA_STATE_STOPPED) {
    prepare(source,state,inMs,changed);
   } else if(!preparing&&state==OBS_MEDIA_STATE_PLAYING) {
    prepare(source,state,inMs);
   }
   if(preparing)prepareTick(source,false);
   mutePreview=true;
   // Native paused seeking updates the preload texture but does not set its
   // async_active flag. Explicitly expose that texture on initial preparation.
   if(!obs_source_get_width(source))obs_source_show_preloaded_video(source);
  }
  enabled=true;wasActive=active;wasShowing=showing;
  obs_source_release(source);
 }
 bool mark(bool out) {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  auto *source=parent?obs_weak_source_get_source(parent):nullptr;if(!source)return false;
  auto pos=obs_source_media_get_time(source);obs_source_release(source);
  auto *settings=obs_source_get_settings(self);obs_data_set_double(settings,out?"out_seconds":"in_seconds",std::max<int64_t>(0,pos)/1000.0);obs_source_update(self,settings);obs_data_release(settings);return true;
 }
 bool edit() {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  auto *source=parent?obs_weak_source_get_source(parent):nullptr;if(!source)return false;
  auto *settings=obs_source_get_settings(source);
  if(!strcmp(obs_source_get_unversioned_id(source),"ffmpeg_source")&&obs_data_get_bool(settings,"is_local_file"))cue_editor_open(self,obs_source_get_uuid(source),obs_data_get_string(settings,"local_file"),inMs/1000.0,outMs/1000.0);
  obs_data_release(settings);obs_source_release(source);return false;
 }
 std::string status() {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if(invalidRange)return "구간 오류: Out은 In보다 커야 하며 In은 파일 길이보다 짧아야 합니다.";
  if(programHold&&!preparing)return "프로그램 스틸 대기 — 첫 프레임 / In 지점 · 재생 명령 대기 · 무음";
  if(preparing&&failedPosterGeneration&&failedPosterGeneration==captureGeneration)return "In 프레임을 읽지 못했습니다 — 무음 대기";
  if(preparing)return "In 지점 준비 중 — 키프레임부터 무음으로 준비합니다.";
  if(idlePhase)return "화면 밖 미디어 디코더 정리 중 — 무음";
  if(unloaded)return "5초 미사용 — 디코더 메모리 해제 / 프리뷰 진입 시 다시 준비";
  if(rangeEnded)return "Out 도달 — 마지막 프레임 / 무음 · Play: In부터 다시 재생";
  if(ended)return "재생 완료 — 마지막 프레임 / 무음 · Play: 처음 또는 In부터 다시 재생";
  if(offStopped)return "프로그램·프리뷰 이탈 — 정지 / 무음";
  return wasActive?"프로그램 재생 중":"프리뷰 대기 — 선택한 In 지점 정지";
 }
};
const char *name(void*) {return "Sunjoo Media Cue Control";}
obs_properties_t *properties(void *data) {
 auto *p=obs_properties_create();
 obs_properties_add_text(p,"creator","SunjooAn · Sunjoo Media Cue Control " FILTER_VERSION " · OBS " FILTER_OBS_MAJOR,OBS_TEXT_INFO);
 obs_properties_add_button2(p,"edit_range","영상 보면서 구간 편집…",[](obs_properties_t*,obs_property_t*,void *d){return d&&static_cast<Control*>(d)->edit();},data);
 auto *in=obs_properties_add_float(p,"in_seconds","In 시작 지점 (초)",0,604800,0.001);obs_property_float_set_suffix(in," 초");
 auto *out=obs_properties_add_float(p,"out_seconds","Out 종료 지점 (초, 0 = 파일 끝)",0,604800,0.001);obs_property_float_set_suffix(out," 초");
 obs_properties_add_button2(p,"mark_in","현재 재생 위치를 In으로",[](obs_properties_t*,obs_property_t*,void *d){return d&&static_cast<Control*>(d)->mark(false);},data);
 obs_properties_add_button2(p,"mark_out","현재 재생 위치를 Out으로",[](obs_properties_t*,obs_property_t*,void *d){return d&&static_cast<Control*>(d)->mark(true);},data);
 obs_properties_add_button2(p,"reset_range","구간 초기화 (전체 영상)",[](obs_properties_t*,obs_property_t*,void *d){if(!d)return false;auto *c=static_cast<Control*>(d);auto *s=obs_source_get_settings(c->self);obs_data_set_double(s,"in_seconds",0);obs_data_set_double(s,"out_seconds",0);obs_source_update(c->self,s);obs_data_release(s);return true;},data);
 auto status=data?static_cast<Control*>(data)->status():std::string("In 0 / Out 0: 전체 영상");obs_properties_add_text(p,"status",status.c_str(),OBS_TEXT_INFO);
 obs_properties_add_button2(p,"refresh","상태 새로고침",[](obs_properties_t*,obs_property_t*,void*){return true;},data);
 obs_properties_add_text(p,"usage","기본 미디어 소스 전용 · MP4 / MOV / WebM\n프리뷰: In 지점 정지 / 전환 시작: 재생 / Out: 정지\n프로그램 Stop: 첫 프레임 / In 스틸·무음 대기\nPlay: 대기 지점부터 재생 / 완료 후 Play: 처음·In부터 재생 / Pause: 현재 위치 유지\n두 화면 이탈: 즉시 정지·무음 / 5초 뒤 디코더 해제\n프리뷰 재진입: 첫 프레임 또는 In 지점 무음 준비\n프로그램 공유·전환 중인 미디어는 유지합니다.\n구간 변경은 다음 준비·프로그램 진입에 적용합니다.\n필터를 끄거나 제거하면 원래 미디어 설정을 복구합니다.",OBS_TEXT_INFO);
 return p;
}
}
bool obs_module_load() {
 visibility.connect(true);
 obs_source_info info={};info.id="sunjoo_media_cue_control";info.type=OBS_SOURCE_TYPE_FILTER;
 info.output_flags=OBS_SOURCE_ASYNC_VIDEO|OBS_SOURCE_AUDIO;
 info.get_name=name;
 info.create=[](obs_data_t *settings,obs_source_t *s)->void*{auto *c=new Control(s);c->update(settings);return c;};
 info.update=[](void *p,obs_data_t *s){static_cast<Control*>(p)->update(s);};
 info.destroy=[](void *p){delete static_cast<Control*>(p);};
 info.filter_add=[](void *p,obs_source_t *s){static_cast<Control*>(p)->attach(s);};
 info.filter_remove=[](void *p,obs_source_t*){static_cast<Control*>(p)->detach();};
 info.video_tick=[](void *p,float){static_cast<Control*>(p)->tick();};
 info.filter_video=[](void *p,obs_source_frame *frame)->obs_source_frame*{return static_cast<Control*>(p)->video(frame);};
 info.filter_audio=[](void *p,obs_audio_data *audio)->obs_audio_data* {
  auto *c=static_cast<Control*>(p);
  if(rangeAudioSender==c)return audio;
  // OBS keeps the parent/filter alive throughout this callback. Avoid waiting
  // on scene enumeration or video preparation while holding its audio lock.
  auto *s=obs_filter_get_parent(c->self);
  if(s) {
   const auto end=c->audioOut.load();
   // Decoder audio can arrive before video_tick sees a preview reactivation.
   // Gate each packet against the live program activation count as well as the
   // cached preparation state. Preview is silent even during that first packet.
   if(obs_source_enabled(c->self)&&(!obs_source_active(s)||c->mutePreview.load()||(end>0&&obs_source_media_get_time(s)>=end))) {
    const auto channels=audio_output_get_channels(obs_get_audio());
    for(size_t i=0;i<channels;++i)if(audio->data[i])memset(audio->data[i],0,audio->frames*sizeof(float));
   }
  }
  return audio;
 };
 info.get_properties=properties;obs_register_source(&info);
 blog(LOG_INFO,"[Sunjoo Media Cue Control] SunjooAn " FILTER_VERSION " / OBS " FILTER_OBS_MAJOR);return true;
}
void obs_module_unload(){visibility.connect(false);cue_editor_shutdown();}
