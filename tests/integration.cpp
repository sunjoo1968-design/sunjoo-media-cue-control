#include <obs.h>
#include <util/platform.h>
#include <graphics/vec4.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <thread>
#include <vector>
#include <fstream>
#include <Windows.h>
static int passes=0,fails=0;
static void check(bool value,const char *msg){printf("%s: %s\n",value?"PASS":"FAIL",msg);fflush(stdout);value?++passes:++fails;}
static obs_source_t *preview=nullptr;
static std::vector<uint8_t> render(obs_source_t *s);
static void sleep(int ms){for(int i=0;i<ms;i+=10){if(preview)render(preview);std::this_thread::sleep_for(std::chrono::milliseconds(std::min(10,ms-i)));}}
static bool until(std::function<bool()> fn,int ms=5000){for(int i=0;i<ms;i+=10){if(fn())return true;sleep(10);}return fn();}
static std::vector<uint8_t> render(obs_source_t *s){
 std::vector<uint8_t> result;obs_enter_graphics();auto *rt=gs_texrender_create(GS_RGBA,GS_ZS_NONE);auto *stage=gs_stagesurface_create(320,180,GS_RGBA);
 if(gs_texrender_begin(rt,320,180)){vec4 clear={};gs_clear(GS_CLEAR_COLOR,&clear,0,0);gs_ortho(0,320,0,180,-100,100);obs_source_video_render(s);gs_texrender_end(rt);gs_stage_texture(stage,gs_texrender_get_texture(rt));
 uint8_t *p=nullptr;uint32_t stride=0;if(gs_stagesurface_map(stage,&p,&stride)){result.resize(320*180*4);for(int y=0;y<180;++y)memcpy(result.data()+y*320*4,p+y*stride,320*4);gs_stagesurface_unmap(stage);}}
 gs_stagesurface_destroy(stage);gs_texrender_destroy(rt);obs_leave_graphics();return result;
}
static std::atomic<int> audioSamples{0};
static std::atomic<int> unexpectedPreviewSamples{0};
static void strictAudio(void*,obs_source_t*,const audio_data *d,bool){for(size_t ch=0;ch<audio_output_get_channels(obs_get_audio());++ch)if(d->data[ch]){auto *f=reinterpret_cast<float*>(d->data[ch]);for(uint32_t i=0;i<d->frames;++i)if(f[i]!=0.0f||!std::isfinite(f[i]))++unexpectedPreviewSamples;}}
static void audio(void*,obs_source_t*,const audio_data *d,bool){if(d->data[0]){auto *f=reinterpret_cast<float*>(d->data[0]);for(uint32_t i=0;i<d->frames;++i)if(std::abs(f[i])>0.01f)++audioSamples;}}
struct Clip {obs_source_t *source,*filter;obs_sceneitem_t *item;};
static Clip make(obs_scene_t *scene,const std::filesystem::path &file,const char *name){
 auto *s=obs_data_create();obs_data_set_bool(s,"is_local_file",true);obs_data_set_string(s,"local_file",file.u8string().c_str());
 obs_data_set_bool(s,"restart_on_activate",true);obs_data_set_bool(s,"close_when_inactive",true);obs_data_set_bool(s,"clear_on_media_end",true);
 auto *source=obs_source_create("ffmpeg_source",name,s,nullptr);obs_data_release(s);
 auto *filter=obs_source_create("sunjoo_media_cue_control","Sunjoo Media Cue Control",nullptr,nullptr);
 if(source&&filter)obs_source_filter_add(source,filter);
 auto *item=source?obs_scene_add(scene,source):nullptr;return {source,filter,item};
}
int main(int argc,char **argv){
 if(argc!=4)return 2;setvbuf(stdout,nullptr,_IONBF,0);
 auto runtime=std::filesystem::absolute(argv[1]),plugin=std::filesystem::absolute(argv[2]),fixtures=std::filesystem::absolute(argv[3]);
 std::filesystem::current_path(runtime/"bin/64bit");
 SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
 AddDllDirectory((runtime/L"bin/64bit").c_str());
 if(!obs_startup("en-US",nullptr,nullptr))return 2;
 obs_audio_info ai={48000,SPEAKERS_STEREO};check(obs_reset_audio(&ai),"OBS audio initialized");
 auto graphics=(runtime/"bin/64bit/libobs-d3d11.dll").string();obs_video_info vi={};vi.graphics_module=graphics.c_str();vi.fps_num=60;vi.fps_den=1;vi.base_width=320;vi.base_height=180;vi.output_width=320;vi.output_height=180;vi.output_format=VIDEO_FORMAT_NV12;vi.gpu_conversion=true;vi.colorspace=VIDEO_CS_709;vi.range=VIDEO_RANGE_PARTIAL;vi.scale_type=OBS_SCALE_BILINEAR;
 if(obs_reset_video(&vi)!=OBS_VIDEO_SUCCESS){check(false,"D3D11 initialization");obs_shutdown();return 1;}check(true,"OBS D3D11 initialized");
 auto open=[&](std::filesystem::path dll,std::filesystem::path data){obs_module_t *m=nullptr;bool ok=obs_open_module(&m,dll.string().c_str(),data.string().c_str())==MODULE_SUCCESS&&obs_init_module(m);check(ok,dll.filename().string().c_str());return ok;};
 auto module=[&](const char *name){auto dir=runtime/"core"/name;if(std::filesystem::exists(dir))return open(dir/(std::string(name)+".dll"),dir/"data");return open(runtime/"obs-plugins/64bit"/(std::string(name)+".dll"),runtime/"data/obs-plugins"/name);};
 if(!module("obs-ffmpeg")||!module("obs-transitions")||!open(plugin,fixtures)){obs_shutdown();return 1;}
 // Reproduce audio arriving on the decoder thread before the graphics tick
 // updates a newly visible filter. Hold graphics to make that window deterministic.
 obs_enter_graphics();
 auto *earlySettings=obs_data_create();obs_data_set_bool(earlySettings,"is_local_file",true);
 auto *early=obs_source_create("ffmpeg_source","Preview audio race",earlySettings,nullptr);obs_data_release(earlySettings);
 auto *earlyFilter=obs_source_create("sunjoo_media_cue_control","Before first tick",nullptr,nullptr);obs_source_filter_add(early,earlyFilter);
 obs_source_add_audio_capture_callback(early,strictAudio,nullptr);obs_source_inc_showing(early);
 std::vector<float> pulse(256,0.25f);obs_source_audio packet={};packet.data[0]=packet.data[1]=reinterpret_cast<uint8_t*>(pulse.data());packet.frames=256;packet.speakers=SPEAKERS_STEREO;packet.samples_per_sec=48000;packet.format=AUDIO_FORMAT_FLOAT_PLANAR;packet.timestamp=os_gettime_ns();
 obs_source_output_audio(early,&packet);
 check(unexpectedPreviewSamples==0,"Preview audio arriving BEFORE first video tick is exactly silent on every channel");
 obs_source_dec_showing(early);obs_source_remove_audio_capture_callback(early,strictAudio,nullptr);obs_source_filter_remove(early,earlyFilter);obs_source_release(earlyFilter);obs_source_release(early);
 obs_leave_graphics();
 auto *blank=obs_scene_create("Blank"),*scene=obs_scene_create("Multiple visible and hidden clips");
 auto mp4=make(scene,fixtures/"cue-test.mp4","MP4"),webm=make(scene,fixtures/"cue-test.webm","WebM"),mov=make(scene,fixtures/"cue-test.mov","MOV");
 auto *group=obs_scene_add_group(scene,"MOV group");obs_sceneitem_group_add_item(group,mov.item);
 check(mp4.source&&mp4.filter&&webm.source&&mov.source,"Native media sources and control filters registered");
 obs_source_add_audio_capture_callback(mp4.source,audio,nullptr);
 obs_source_inc_showing(obs_scene_get_source(scene));
 preview=obs_scene_get_source(scene);
 auto ready=[](obs_source_t *s){return obs_source_media_get_state(s)==OBS_MEDIA_STATE_PAUSED&&obs_source_media_get_time(s)<40;};
 check(until([&]{return ready(mp4.source)&&ready(webm.source)&&ready(mov.source);}),"MP4 / WebM VP9 Opus / MOV all pause at preview frame zero");
 sleep(400);
 auto first=render(mp4.source);int colored=0;for(size_t i=0;i<first.size();i+=4)colored+=first[i]+first[i+1]+first[i+2]>40;
 check(colored>1000,"Native first preview frame contains decoded video pixels");
 std::ifstream raw(fixtures/"first.rgba",std::ios::binary);std::vector<uint8_t> expected((std::istreambuf_iterator<char>(raw)),{});double error=1000;
 if(expected.size()==first.size()&&!first.empty()){error=0;for(size_t i=0;i<first.size();++i)error+=std::abs(int(first[i])-expected[i]);error/=first.size();}
 printf("First-frame reference mean pixel error=%.3f\n",error);check(error<8,"Preview is actual first frame against independent FFmpeg reference");
 sleep(500);check(first==render(mp4.source)&&ready(mp4.source)&&ready(webm.source),"Preview remains frozen with multiple visible media sources");
 check(audioSamples==0,"Preview audio remains silent");
 unexpectedPreviewSamples=0;for(auto &c:{mp4,webm,mov})obs_source_add_audio_capture_callback(c.source,strictAudio,nullptr);
 bool previewVisits=true;
 for(int visit=0;visit<10;++visit){preview=nullptr;obs_source_dec_showing(obs_scene_get_source(scene));sleep(80);obs_source_inc_showing(obs_scene_get_source(scene));preview=obs_scene_get_source(scene);previewVisits&=until([&]{return ready(mp4.source)&&ready(webm.source)&&ready(mov.source);},1500);sleep(100);}
 check(previewVisits,"10 preview-away / preview-return cycles prepare MP4 MOV WebM correctly");
 check(unexpectedPreviewSamples==0,"Repeated preview returns produce bit-exact silence, including quiet clicks and right channel");
 for(auto &c:{mp4,webm,mov})obs_source_remove_audio_capture_callback(c.source,strictAudio,nullptr);
 obs_sceneitem_set_visible(group,false);sleep(150);auto groupTime=obs_source_media_get_time(mov.source);sleep(200);
 check(obs_source_media_get_state(mov.source)==OBS_MEDIA_STATE_STOPPED&&std::abs(obs_source_media_get_time(mov.source)-groupTime)<40,"Hidden parent group stops its previously managed child");
 obs_sceneitem_set_visible(group,true);check(until([&]{return ready(mov.source);}),"Showing parent group re-arms visible child");
 auto *props=obs_source_properties(mp4.filter);check(props&&obs_properties_get(props,"creator"),"SunjooAn and version visible in filter properties");obs_properties_destroy(props);
 obs_sceneitem_set_visible(webm.item,false);sleep(120);
 auto hiddenTime=obs_source_media_get_time(webm.source);sleep(250);
 printf("HIDDEN showing=%d active=%d time=%lld state=%d\n",obs_source_showing(webm.source),obs_source_active(webm.source),(long long)obs_source_media_get_time(webm.source),obs_source_media_get_state(webm.source));
 check(!obs_sceneitem_visible(webm.item)&&std::abs(obs_source_media_get_time(webm.source)-hiddenTime)<40&&obs_source_media_get_state(webm.source)==OBS_MEDIA_STATE_STOPPED,"Eye-OFF source stops and is not started again by controller");
 obs_sceneitem_set_visible(webm.item,true);
 check(until([&]{return ready(webm.source);}),"Eye-ON source in preview re-arms first frame");
 auto *fade=obs_source_create_private("fade_transition","Fade",nullptr);obs_transition_set_size(fade,320,180);obs_transition_set(fade,obs_scene_get_source(blank));obs_set_output_source(0,fade);sleep(100);
 check(obs_transition_start(fade,OBS_TRANSITION_MODE_AUTO,600,obs_scene_get_source(scene)),"Real 600ms fade started");
 check(until([&]{return obs_source_media_get_state(mp4.source)==OBS_MEDIA_STATE_PLAYING&&obs_source_media_get_time(mp4.source)>40&&obs_source_media_get_state(webm.source)==OBS_MEDIA_STATE_PLAYING;},250),"Visible MP4 and WebM play during fade before transition completion");
 sleep(300);check(obs_source_media_get_time(mov.source)>150,"All visible sources in same scene play together");
 check(first!=render(mp4.source),"Native program video advances through pass-through filter");
 check(until([]{return audioSamples>1000;},1000),"Program audio outputs non-silent samples");
 auto t=obs_source_media_get_time(mp4.source);obs_source_inc_showing(mp4.source);sleep(200);check(obs_source_media_get_time(mp4.source)>t+100,"Additional preview reference cannot rewind shared program source");obs_source_dec_showing(mp4.source);
 // Regression: remove this clip from BOTH program and preview while playing.
 preview=nullptr;obs_source_dec_showing(obs_scene_get_source(scene));
 obs_transition_set(fade,obs_scene_get_source(blank));sleep(250);
 check(obs_source_media_get_state(mp4.source)==OBS_MEDIA_STATE_PAUSED||obs_source_media_get_state(mp4.source)==OBS_MEDIA_STATE_STOPPED,"Leaving BOTH buses stops off-screen playback");
 auto offTime=obs_source_media_get_time(mp4.source);auto offAudio=audioSamples.load();sleep(300);
 check(std::abs(obs_source_media_get_time(mp4.source)-offTime)<40&&audioSamples==offAudio,"Off-screen media clock AND audio remain stopped");
 obs_source_inc_showing(obs_scene_get_source(scene));preview=obs_scene_get_source(scene);
 obs_transition_set(fade,obs_scene_get_source(blank));check(until([&]{return ready(mp4.source)&&ready(webm.source)&&ready(mov.source);}),"Leaving program re-arms all visible preview clips");
 sleep(200);check(first==render(mp4.source),"Re-armed preview matches initial decoded frame exactly");
 bool cycles=true;for(int i=0;i<10;++i){obs_transition_set(fade,obs_scene_get_source(scene));cycles&=until([&]{return obs_source_media_get_state(mp4.source)==OBS_MEDIA_STATE_PLAYING;},300);obs_transition_set(fade,obs_scene_get_source(blank));cycles&=until([&]{return ready(mp4.source);},1000);}check(cycles,"10 rapid CUT cycles cue and play without hanging");
 obs_sceneitem_set_visible(webm.item,false);sleep(150);hiddenTime=obs_source_media_get_time(webm.source);
 obs_transition_set(fade,obs_scene_get_source(scene));sleep(400);check(!obs_source_active(webm.source)&&std::abs(obs_source_media_get_time(webm.source)-hiddenTime)<40&&obs_source_media_get_state(webm.source)==OBS_MEDIA_STATE_STOPPED,"Eye-OFF WebM stays stopped while other scene clips enter program");
 obs_sceneitem_set_visible(webm.item,true);check(until([&]{return obs_source_active(webm.source)&&obs_source_media_get_state(webm.source)==OBS_MEDIA_STATE_PLAYING;}),"Eye-ON in live scene plays natively");
 obs_transition_set(fade,obs_scene_get_source(blank));check(until([&]{return ready(mp4.source);}),"Return to preview after visibility toggles");
 auto vp8=make(scene,fixtures/"cue-test-vp8.webm","WebM VP8 Vorbis");
 check(until([&]{return ready(vp8.source)&&obs_source_get_width(vp8.source)>0;}),"WebM VP8 Vorbis also prepares in preview");
 obs_transition_set(fade,obs_scene_get_source(scene));sleep(300);check(obs_source_media_get_state(vp8.source)==OBS_MEDIA_STATE_PLAYING&&obs_source_media_get_time(vp8.source)>100,"WebM VP8 Vorbis plays on CUT");
 obs_transition_set(fade,obs_scene_get_source(blank));check(until([&]{return ready(vp8.source);}),"WebM VP8 re-arms after program exit");
 auto *trim=obs_data_create();obs_data_set_double(trim,"in_seconds",0.5);obs_data_set_double(trim,"out_seconds",1.2);obs_source_update(webm.filter,trim);obs_data_release(trim);
 auto atIn=[&]{auto pos=obs_source_media_get_time(webm.source);return obs_source_media_get_state(webm.source)==OBS_MEDIA_STATE_PAUSED&&pos>=470&&pos<=580;};
 check(until(atIn,4000),"WebM preview prepares selected In point (0.500s), not preceding keyframe");
 // Native pause/seek and the async GPU queue settle on different OBS ticks.
 sleep(100);auto inPicture=render(webm.source);sleep(200);check(atIn()&&inPicture==render(webm.source),"Selected In picture stays paused in preview");
 auto trimProps=obs_source_properties(webm.filter);check(obs_properties_get(trimProps,"in_seconds")&&obs_properties_get(trimProps,"out_seconds")&&obs_properties_get(trimProps,"mark_in")&&obs_properties_get(trimProps,"mark_out"),"In/Out fields and mark-current-position buttons available");obs_properties_destroy(trimProps);
 obs_transition_set(fade,obs_scene_get_source(scene));
 check(until([&]{return obs_source_media_get_state(webm.source)==OBS_MEDIA_STATE_PLAYING&&obs_source_media_get_time(webm.source)>600;},700),"Range plays from In on program entry");
 check(until([&]{return obs_source_media_get_state(webm.source)==OBS_MEDIA_STATE_PAUSED&&obs_source_media_get_time(webm.source)>=1150;},1800),"Range automatically pauses at Out (1.200s)");
 auto outTime=obs_source_media_get_time(webm.source);sleep(250);check(outTime<=1300&&std::abs(obs_source_media_get_time(webm.source)-outTime)<40,"Out holds its picture and clock without playing beyond range");
 obs_transition_set(fade,obs_scene_get_source(blank));check(until(atIn,4000),"Program exit returns range clip to selected In point");
 trim=obs_data_create();obs_data_set_double(trim,"in_seconds",0.0);obs_data_set_double(trim,"out_seconds",0.0);obs_source_update(webm.filter,trim);obs_data_release(trim);
 check(until([&]{return ready(webm.source);}),"Clearing range restores frame-zero cue");
 auto setRange=[](obs_source_t *filter,double in,double out){auto *s=obs_data_create();obs_data_set_double(s,"in_seconds",in);obs_data_set_double(s,"out_seconds",out);obs_source_update(filter,s);obs_data_release(s);};
 for(auto &c:{mp4,mov,vp8})setRange(c.filter,0.5,1.2);
 auto allIn=[&]{for(auto &c:{mp4,mov,vp8})if(obs_source_media_get_state(c.source)!=OBS_MEDIA_STATE_PAUSED||std::abs(obs_source_media_get_time(c.source)-500)>80)return false;return true;};
 check(until(allIn,5000),"MP4, MOV and VP8 WebM all prepare arbitrary In positions");
 obs_transition_start(fade,OBS_TRANSITION_MODE_AUTO,600,obs_scene_get_source(scene));sleep(230);
 check(obs_source_media_get_state(mp4.source)==OBS_MEDIA_STATE_PLAYING&&obs_source_media_get_time(mp4.source)>540,"MP4 range begins during fade without waiting until completion");
 check(until([&]{for(auto &c:{mp4,mov,vp8})if(obs_source_media_get_state(c.source)!=OBS_MEDIA_STATE_PAUSED||obs_source_media_get_time(c.source)<1150||obs_source_media_get_time(c.source)>1300)return false;return true;},2000),"MP4, MOV and VP8 ranges all stop at Out");
 sleep(150);auto stoppedAudio=audioSamples.load();auto heldOut=render(mp4.source);sleep(250);
 check(audioSamples==stoppedAudio&&heldOut==render(mp4.source),"Out stops actual MP4 audio samples and holds its GPU picture");
 obs_transition_set(fade,obs_scene_get_source(blank));check(until(allIn,5000),"All range formats return to In on program exit");
 for(auto &c:{mp4,mov,vp8})setRange(c.filter,0,0);
 check(until([&]{return ready(mp4.source)&&ready(mov.source)&&ready(vp8.source);}),"All range formats restore whole-file cue after reset");
 setRange(webm.filter,1.0,0.5);sleep(150);auto *invalidProps=obs_source_properties(webm.filter);
 check(obs_source_media_get_state(webm.source)==OBS_MEDIA_STATE_PAUSED&&std::string(obs_property_description(obs_properties_get(invalidProps,"status"))).find("구간 오류")!=std::string::npos,"Invalid Out-before-In is paused and explained in properties");obs_properties_destroy(invalidProps);
 setRange(webm.filter,4.0,0);sleep(150);check(obs_source_media_get_state(webm.source)==OBS_MEDIA_STATE_PAUSED,"In beyond file duration is rejected without starting playback");
 // Decoder callbacks can interleave any part of video_tick. Even an active
 // source in a rejected range must stay muted for the entire tick.
 obs_transition_set(fade,obs_scene_get_source(scene));sleep(150);unexpectedPreviewSamples=0;obs_source_add_audio_capture_callback(webm.source,strictAudio,nullptr);
 auto pulseUntil=os_gettime_ns()+500000000ull;int pulseCount=0;
 while(os_gettime_ns()<pulseUntil){std::fill(pulse.begin(),pulse.end(),0.25f);packet.timestamp=os_gettime_ns();obs_source_output_audio(webm.source,&packet);++pulseCount;std::this_thread::yield();}
 printf("Active invalid-range audio probe packets=%d leaked_samples=%d\n",pulseCount,unexpectedPreviewSamples.load());check(unexpectedPreviewSamples==0,"Active invalid-range audio stays exactly silent throughout concurrent video ticks");obs_source_remove_audio_capture_callback(webm.source,strictAudio,nullptr);obs_transition_set(fade,obs_scene_get_source(blank));
 setRange(webm.filter,0,0);check(until([&]{return ready(webm.source);}),"Valid settings recover from invalid range");
 obs_source_set_enabled(mp4.filter,false);sleep(150);auto *settings=obs_source_get_settings(mp4.source);check(obs_data_get_bool(settings,"restart_on_activate")&&obs_data_get_bool(settings,"close_when_inactive")&&obs_data_get_bool(settings,"clear_on_media_end"),"Disabling filter restores original native settings");obs_data_release(settings);
 obs_source_set_enabled(mp4.filter,true);check(until([&]{return ready(mp4.source);}),"Re-enabling filter re-arms preview");
 obs_source_filter_remove(mp4.source,mp4.filter);sleep(100);settings=obs_source_get_settings(mp4.source);check(obs_data_get_bool(settings,"restart_on_activate")&&obs_data_get_bool(settings,"close_when_inactive"),"Removing filter restores original native settings");obs_data_release(settings);
 obs_transition_set(fade,obs_scene_get_source(scene));sleep(300);t=obs_source_media_get_time(mp4.source);
 auto *liveFilter=obs_source_create("sunjoo_media_cue_control","Attach while live",nullptr,nullptr);obs_source_filter_add(mp4.source,liveFilter);sleep(300);
 check(obs_source_media_get_time(mp4.source)>t+150,"Attaching filter to already-live native source preserves playback");
 auto *buttons=obs_source_properties(liveFilter);obs_property_button_clicked(obs_properties_get(buttons,"mark_in"),liveFilter);sleep(100);
 auto *buttonSettings=obs_source_get_settings(liveFilter);auto markedIn=obs_data_get_double(buttonSettings,"in_seconds");obs_data_release(buttonSettings);
 check(markedIn>0.4&&markedIn<1.2,"Mark In button stores current native playback position");
 obs_property_button_clicked(obs_properties_get(buttons,"mark_out"),liveFilter);sleep(80);buttonSettings=obs_source_get_settings(liveFilter);
 check(obs_data_get_double(buttonSettings,"out_seconds")>markedIn,"Mark Out button stores later native playback position");obs_data_release(buttonSettings);
 obs_property_button_clicked(obs_properties_get(buttons,"reset_range"),liveFilter);sleep(80);buttonSettings=obs_source_get_settings(liveFilter);
 check(obs_data_get_double(buttonSettings,"in_seconds")==0&&obs_data_get_double(buttonSettings,"out_seconds")==0,"Reset button restores whole-file In/Out values");obs_data_release(buttonSettings);obs_properties_destroy(buttons);
 auto *saved=obs_source_get_settings(liveFilter);obs_source_filter_remove(mp4.source,liveFilter);obs_source_release(liveFilter);
 obs_transition_set(fade,obs_scene_get_source(blank));
 auto *loaded=obs_source_create("sunjoo_media_cue_control","Reloaded settings",saved,nullptr);obs_data_release(saved);obs_source_filter_add(mp4.source,loaded);
 check(until([&]{return ready(mp4.source);}),"Controller recreated from persisted filter settings re-arms preview");obs_source_filter_remove(mp4.source,loaded);obs_source_release(loaded);sleep(100);
 settings=obs_source_get_settings(mp4.source);check(obs_data_get_bool(settings,"restart_on_activate")&&obs_data_get_bool(settings,"close_when_inactive"),"Persisted original options still restore after filter recreation");obs_data_release(settings);
 preview=nullptr;obs_source_remove_audio_capture_callback(mp4.source,audio,nullptr);obs_source_dec_showing(obs_scene_get_source(scene));obs_set_output_source(0,nullptr);obs_source_release(fade);
 for(auto &c:{mp4,webm,mov,vp8}){obs_source_release(c.filter);obs_source_release(c.source);}
 obs_canvas_scene_remove(scene);obs_scene_release(scene);obs_canvas_scene_remove(blank);obs_scene_release(blank);obs_wait_for_destroy_queue();obs_shutdown();
 auto allocations=bnum_allocs();printf("OBS remaining allocations=%ld\n",allocations);check(allocations==0,"OBS tracked allocations return to zero");printf("COMPLETE passes=%d failures=%d\n",passes,fails);return fails?1:0;
}
