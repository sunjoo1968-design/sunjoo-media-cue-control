#include <obs-module.h>
#include <mutex>
#include <string>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <util/platform.h>
#include "editor.hpp"

OBS_DECLARE_MODULE()
MODULE_EXPORT const char *obs_module_name(void) { return "Sunjoo Media Cue Control " FILTER_VERSION; }
MODULE_EXPORT const char *obs_module_description(void) { return "Visible media preview cue / play on transition. SunjooAn " FILTER_VERSION; }
MODULE_EXPORT const char *obs_module_author(void) { return "SunjooAn"; }

namespace {
constexpr const char *options[] = {"restart_on_activate", "close_when_inactive", "clear_on_media_end"};
struct VisibleSearch {obs_source_t *target;bool found=false;unsigned depth=0;};
void visibleSource(obs_source_t *source,VisibleSearch &search);
bool visibleItem(obs_scene_t*,obs_sceneitem_t *item,void *param) {
 auto &search=*static_cast<VisibleSearch*>(param);
 if(!obs_sceneitem_visible(item))return true;
 if(search.depth<64) {
  ++search.depth;visibleSource(obs_sceneitem_get_source(item),search);--search.depth;
 }
 return !search.found;
}
void visibleSource(obs_source_t *source,VisibleSearch &search) {
 if(!source||search.found||search.depth>=64)return;
 if(source==search.target){search.found=true;return;}
 auto *scene=obs_scene_from_source(source);
 if(!scene)scene=obs_group_from_source(source);
 if(scene)obs_scene_enum_items(scene,visibleItem,&search);
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
 if(obs_source_showing(source))obs_scene_enum_items(obs_scene_from_source(source),visibleItem,&search);
 return !search.found;
}
bool visibleInScene(obs_source_t *source) {
 VisibleSearch search{source};
 // Studio Mode's program scene is a private duplicate, absent from enum_scenes.
 auto *output=obs_get_output_source(0);visibleSource(output,search);obs_source_release(output);
 if(!search.found)obs_enum_scenes(visibleScene,&search);return search.found;
}
struct Control {
 obs_source_t *self;
 obs_weak_source_t *parent = nullptr;
 std::recursive_mutex mutex;
 bool managed = false, wasActive = false, wasShowing = false, armed = false, enabled = false;
 std::atomic<bool> mutePreview{false};
 std::atomic<int64_t> audioOut{0};
 bool preparing=false, priming=false, rangeEnded=false, offStopped=false, loopManaged=false, invalidRange=false, waitingRestart=false;
 int64_t inMs=0,outMs=0,cueIn=0,runIn=0,runOut=0;
 uint64_t revision=1,appliedRevision=0,prepareAt=0;
 std::string path;
 explicit Control(obs_source_t *source) : self(source) {}
 ~Control() { detach(); }
 void attach(obs_source_t *source) {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  parent = obs_source_get_weak_source(source);
  // Persist the original options with this filter so removal after a saved/reloaded
  // scene collection can restore the user's original settings.
  if (strcmp(obs_source_get_unversioned_id(source), "ffmpeg_source")) return;
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
  obs_data_release(saved);
 }
 void restore(obs_source_t *source) {
  if (!managed) return;
  auto *settings = obs_source_get_settings(source);
  auto *saved = obs_source_get_settings(self);
  for (const char *key : options) {
   // Preserve a user change made while this controller was attached.
   if (!obs_data_get_bool(settings,key))
    obs_data_set_bool(settings,key,obs_data_get_bool(saved,(std::string("original_")+key).c_str()));
  }
  if(loopManaged){obs_data_set_bool(settings,"looping",obs_data_get_bool(saved,"original_looping"));loopManaged=false;}
  obs_source_update(source,settings);
  obs_source_set_async_unbuffered(source,obs_data_get_bool(saved,"original_unbuffered"));
  obs_data_release(settings);obs_data_release(saved);managed=false;
 }
 void detach() {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (!parent) return;
  auto *source = obs_weak_source_get_source(parent);
  if (source) { restore(source); obs_source_release(source); }
  obs_weak_source_release(parent);parent=nullptr;
 }
 void update(obs_data_t *settings) {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  auto value=[](double v)->int64_t{return std::isfinite(v)?int64_t(std::clamp(v,0.0,604800.0)*1000.0+0.5):0;};
  auto in=value(obs_data_get_double(settings,"in_seconds")),out=value(obs_data_get_double(settings,"out_seconds"));
  if(in!=inMs||out!=outMs){inMs=in;outMs=out;++revision;}
 }
 void prepare(obs_source_t *source,obs_media_state state,int64_t target) {
  mutePreview=true;
  waitingRestart=state==OBS_MEDIA_STATE_ENDED||state==OBS_MEDIA_STATE_STOPPED||state==OBS_MEDIA_STATE_NONE;
  if(waitingRestart)obs_source_media_restart(source);
  else {obs_source_media_play_pause(source,target==0);obs_source_media_set_time(source,target);}
  cueIn=target;preparing=true;priming=target>0;armed=false;rangeEnded=false;prepareAt=os_gettime_ns();appliedRevision=revision;
 }
 void prepareTick(obs_source_t *source,bool active) {
  if(os_gettime_ns()-prepareAt<(cueIn>0&&!waitingRestart?20000000ull:80000000ull))return;
  // Seek while decoding, not while paused: the native paused-seek path flushes
  // audio after preloading video, which can leave an audio-only catch-up gap.
  if(waitingRestart){waitingRestart=false;obs_source_media_play_pause(source,cueIn==0);obs_source_media_set_time(source,cueIn);prepareAt=os_gettime_ns();return;}
  auto pos=obs_source_media_get_time(source);
  // Native seeking can land at an earlier keyframe. Decode silently up to In.
  if(pos<cueIn-20) {
   if(!priming){obs_source_media_play_pause(source,false);priming=true;}
   return;
  }
  if(pos>cueIn+80&&os_gettime_ns()-prepareAt<500000000)return;
  if(cueIn==0) {
   // Drop frames queued during native restart; they must not overwrite the
   // seek-to-zero preload after a rapid preview hide/show cycle.
   obs_source_output_video(source,nullptr);obs_source_show_preloaded_video(source);
  }
  preparing=false;armed=!active;
  obs_source_media_play_pause(source,!active);
 }
 void tick() {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  auto *source=parent?obs_weak_source_get_source(parent):nullptr;
  if(!source)return;
  const bool run=obs_source_enabled(self);
  if(!run) {
   mutePreview=false;
   restore(source);enabled=false;armed=false;wasActive=false;wasShowing=false;preparing=false;offStopped=false;
   obs_source_release(source);return;
  }
  if(strcmp(obs_source_get_unversioned_id(source),"ffmpeg_source")) {mutePreview=false;obs_source_release(source);return;}
  const bool activeCount=obs_source_active(source),showingCount=obs_source_showing(source);
  // Dormant media cannot be visible. Avoid enumerating all scenes for every
  // hidden filter at every video tick (common in large broadcast collections).
  bool visible=(activeCount||showingCount)&&visibleInScene(source);
  bool active=activeCount&&visible,showing=showingCount&&visible;
  // Inspect scene-item visibility too: OBS can retain show/active counts briefly
  // while the eye-OFF audio transition is draining.
  if(!visible||(!showing&&!active)) {
   // A managed clip that leaves both buses must not continue decoding/audio.
   // Issue once on departure; do not repeatedly control unrelated hidden items.
   if(managed&&(wasShowing||wasActive||preparing)) {mutePreview=true;obs_source_media_stop(source);offStopped=true;}
   mutePreview=offStopped;preparing=false;priming=false;
   wasShowing=false;wasActive=false;armed=false;
   obs_source_release(source);return;
  }
  auto *settings=obs_source_get_settings(source);
  if(!obs_data_get_bool(settings,"is_local_file")) {mutePreview=false;obs_data_release(settings);obs_source_release(source);return;}
  std::string current=obs_data_get_string(settings,"local_file");
  bool changed=current!=path;
  bool settingsChanged=false;
  for(const char *key:options) if(obs_data_get_bool(settings,key)) {obs_data_set_bool(settings,key,false);settingsChanged=true;}
  const auto loopIn=active&&wasActive?runIn:inMs,loopOut=active&&wasActive?runOut:outMs;
  if(loopIn>0||loopOut>0) {
   if(obs_data_get_bool(settings,"looping")){obs_data_set_bool(settings,"looping",false);settingsChanged=true;}
   loopManaged=true;
  } else if(loopManaged) {
   auto *saved=obs_source_get_settings(self);obs_data_set_bool(settings,"looping",obs_data_get_bool(saved,"original_looping"));obs_data_release(saved);settingsChanged=true;loopManaged=false;
  }
  if(settingsChanged)obs_source_update(source,settings);
  // Decoder output is already paced. Discard OBS's stale preview timestamp
  // backlog when resuming instead of waiting for its buffered clock to catch up.
  obs_source_set_async_unbuffered(source,true);
  managed=true;obs_data_release(settings);
  if(current.empty()){path=current;armed=false;obs_source_release(source);return;}
  auto state=obs_source_media_get_state(source);
  auto duration=obs_source_media_get_duration(source);
  // Snapshot even a rejected range on entry. Otherwise the early invalid-range
  // return marks wasActive without updating runIn, and the next tick resurrects
  // the previous valid range and releases the audio gate.
  if(active&&!wasActive){runIn=inMs;runOut=outMs>0?(duration>0?std::min(outMs,duration):outMs):0;audioOut=runOut;rangeEnded=false;}
  auto checkedIn=active&&wasActive?runIn:inMs,checkedOut=active&&wasActive?runOut:outMs;
  invalidRange=(checkedOut>0&&checkedOut<=checkedIn)||(duration>0&&checkedIn>=duration);
  if(invalidRange){obs_source_media_play_pause(source,true);mutePreview=true;armed=false;preparing=false;enabled=true;wasShowing=showing;wasActive=active;obs_source_release(source);return;}
  offStopped=false;
  if(active) {
   // Attaching/enabling on an already-live source must not rewind it.
   if(!wasActive) {
    if(armed&&!preparing)obs_source_media_play_pause(source,false);
    else if(!preparing&&((enabled&&state!=OBS_MEDIA_STATE_PLAYING)||inMs>0))prepare(source,state,inMs);
   }
   if(preparing)prepareTick(source,true);
   if(!preparing&&runOut>0&&obs_source_media_get_time(source)>=runOut){obs_source_media_play_pause(source,true);rangeEnded=true;}
   if(rangeEnded&&state==OBS_MEDIA_STATE_PLAYING)obs_source_media_play_pause(source,true);
   mutePreview=preparing||rangeEnded;
   armed=false;
  } else {
   if(!enabled||!wasShowing||wasActive||changed||appliedRevision!=revision||(!armed&&!preparing)||state==OBS_MEDIA_STATE_ENDED||state==OBS_MEDIA_STATE_STOPPED) {
    prepare(source,state,inMs);
   } else if(!preparing&&(state==OBS_MEDIA_STATE_PLAYING||std::abs(obs_source_media_get_time(source)-cueIn)>80)) {
    prepare(source,state,inMs);
   }
   if(preparing)prepareTick(source,false);
   mutePreview=true;
   // Native paused seeking updates the preload texture but does not set its
   // async_active flag. Explicitly expose that texture on initial preparation.
   if(!obs_source_get_width(source))obs_source_show_preloaded_video(source);
  }
  enabled=true;wasActive=active;wasShowing=showing;path=std::move(current);
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
  if(preparing)return "In 지점 준비 중 — 키프레임부터 무음으로 준비합니다.";
  if(rangeEnded)return "Out 도달 — 정지 / 무음";
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
 obs_properties_add_text(p,"usage","기본 미디어 소스 전용 · MP4 / MOV / WebM\n프리뷰: In 지점 정지 / 전환 시작: 재생 / Out: 정지\n프로그램·프리뷰 어디에도 없으면 영상·오디오 정지\n숨겨진 소스는 새로 시작하지 않습니다. 프로그램에서 공유 중이면 재생 우선\n구간 변경은 다음 프리뷰 준비·프로그램 진입에 적용합니다.\n필터를 끄거나 제거하면 원래 미디어 설정을 복구합니다.",OBS_TEXT_INFO);
 return p;
}
}
bool obs_module_load() {
 obs_source_info info={};info.id="sunjoo_media_cue_control";info.type=OBS_SOURCE_TYPE_FILTER;
 info.output_flags=OBS_SOURCE_ASYNC_VIDEO|OBS_SOURCE_AUDIO;
 info.get_name=name;
 info.create=[](obs_data_t *settings,obs_source_t *s)->void*{auto *c=new Control(s);c->update(settings);return c;};
 info.update=[](void *p,obs_data_t *s){static_cast<Control*>(p)->update(s);};
 info.destroy=[](void *p){delete static_cast<Control*>(p);};
 info.filter_add=[](void *p,obs_source_t *s){static_cast<Control*>(p)->attach(s);};
 info.filter_remove=[](void *p,obs_source_t*){static_cast<Control*>(p)->detach();};
 info.video_tick=[](void *p,float){static_cast<Control*>(p)->tick();};
 info.filter_video=[](void*,obs_source_frame *frame)->obs_source_frame*{return frame;};
 info.filter_audio=[](void *p,obs_audio_data *audio)->obs_audio_data* {
  auto *c=static_cast<Control*>(p);
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
void obs_module_unload(){cue_editor_shutdown();}
