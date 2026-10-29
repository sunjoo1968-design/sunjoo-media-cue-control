#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QApplication>
#include <QDialog>
#include <QTimer>
#include <QWidget>
#include <QPixmap>
#include <QImage>
#include <graphics/vec4.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <QPushButton>
#include <QLabel>
#include <QDoubleSpinBox>
#include <QMouseEvent>
#include <QScrollBar>
#include <QCheckBox>
OBS_DECLARE_MODULE()
static int failures=0;
static bool hostBaseline=false;
static bool rangeMode=false;
static bool previewAudioMode=false;
static bool editorMode=false;
static std::atomic<int> previewNoise{0},programSound{0};
static std::atomic<bool> watchPreview{true};
static void captureAudio(void*,obs_source_t*,const audio_data *d,bool){for(size_t ch=0;ch<audio_output_get_channels(obs_get_audio());++ch)if(d->data[ch]){auto *f=reinterpret_cast<float*>(d->data[ch]);for(uint32_t i=0;i<d->frames;++i)if(f[i]!=0.0f||!std::isfinite(f[i])){if(watchPreview)++previewNoise;else ++programSound;}}}
static uint64_t firstHash=0;
static uint64_t clipHash(obs_source_t *source){
 uint64_t hash=1469598103934665603ull;
 obs_enter_graphics();auto *rt=gs_texrender_create(GS_RGBA,GS_ZS_NONE);auto *stage=gs_stagesurface_create(320,180,GS_RGBA);
 if(gs_texrender_begin(rt,320,180)){vec4 clear={};gs_clear(GS_CLEAR_COLOR,&clear,0,0);gs_ortho(0,320,0,180,-100,100);obs_source_video_render(source);gs_texrender_end(rt);gs_stage_texture(stage,gs_texrender_get_texture(rt));uint8_t *p=nullptr;uint32_t stride=0;if(gs_stagesurface_map(stage,&p,&stride)){for(int y=0;y<180;++y)for(int x=0;x<320*4;++x){hash^=p[y*stride+x];hash*=1099511628211ull;}gs_stagesurface_unmap(stage);}}
 gs_stagesurface_destroy(stage);gs_texrender_destroy(rt);obs_leave_graphics();return hash;
}
static void check(bool ok,const char *s){if(hostBaseline){blog(LOG_INFO,"[CUE-HOST-BASELINE] observed=%d: %s",ok,s);return;}blog(ok?LOG_INFO:LOG_ERROR,"[FILTER-UI] %s: %s",ok?"PASS":"FAIL",s);failures+=!ok;}
static bool programImage(){
 auto *root=obs_get_output_source(0);if(!root)return false;
 obs_video_info vi={};obs_get_video_info(&vi);obs_enter_graphics();auto *rt=gs_texrender_create(GS_RGBA,GS_ZS_NONE);auto *stage=gs_stagesurface_create(640,360,GS_RGBA);bool ok=false;
 if(gs_texrender_begin(rt,640,360)){
  vec4 clear={};gs_clear(GS_CLEAR_COLOR,&clear,0,0);gs_ortho(0,float(vi.base_width),0,float(vi.base_height),-100,100);obs_source_video_render(root);gs_texrender_end(rt);gs_stage_texture(stage,gs_texrender_get_texture(rt));uint8_t *p=nullptr;uint32_t stride=0;
  if(gs_stagesurface_map(stage,&p,&stride)){int colored=0;for(int y=0;y<360;++y)for(int x=0;x<640;++x)colored+=p[y*stride+x*4]+p[y*stride+x*4+1]+p[y*stride+x*4+2]>50;QImage image(p,640,360,stride,QImage::Format_RGBA8888);ok=colored>500&&image.save(TEST_IMAGE);blog(LOG_INFO,"[FILTER-UI] Program GPU colored pixels=%d",colored);gs_stagesurface_unmap(stage);}
 }
 gs_stagesurface_destroy(stage);gs_texrender_destroy(rt);obs_leave_graphics();obs_source_release(root);return ok;
}
static void finish(obs_source_t *clip,obs_scene_t *blank,obs_scene_t *media){
 if(clip&&(previewAudioMode||editorMode))obs_source_remove_audio_capture_callback(clip,captureAudio,nullptr);
 if(blank)obs_frontend_set_current_preview_scene(obs_scene_get_source(blank));
 if(clip)obs_source_remove(clip);
 if(media)obs_canvas_scene_remove(media);
 obs_source_release(clip);obs_scene_release(blank);obs_scene_release(media);
 blog(LOG_INFO,"[FILTER-UI] COMPLETE failures=%d",failures);
 QTimer::singleShot(1500,[]{for(auto *w:QApplication::topLevelWidgets())if(qobject_cast<QDialog*>(w))w->close();static_cast<QWidget*>(obs_frontend_get_main_window())->close();QCoreApplication::quit();});
}
static void previewAudioVisit(obs_source_t *clip,obs_scene_t *blank,obs_scene_t *media,int remaining){
 obs_frontend_set_current_preview_scene(obs_scene_get_source(blank));
 QTimer::singleShot(150,[clip,blank,media,remaining]{obs_frontend_set_current_preview_scene(obs_scene_get_source(media));
  QTimer::singleShot(400,[clip,blank,media,remaining]{
   if(remaining>1){previewAudioVisit(clip,blank,media,remaining-1);return;}
   check(previewNoise==0,"Real 10 preview-away / return cycles contain ZERO nonzero samples on all channels");
   check(obs_source_media_get_state(clip)==OBS_MEDIA_STATE_PAUSED&&obs_source_media_get_time(clip)<40,"Real repeated preview return holds first frame paused");
   check(clipHash(clip)==firstHash,"Real repeated preview return keeps the original first GPU picture");
   watchPreview=false;obs_frontend_preview_program_trigger_transition();
   QTimer::singleShot(1000,[clip,blank,media]{blog(LOG_INFO,"[FILTER-UI] audio samples program=%d preview=%d active=%d state=%d time=%lld",programSound.load(),previewNoise.load(),obs_source_active(clip),obs_source_media_get_state(clip),(long long)obs_source_media_get_time(clip));check(programSound>1000,"Audio gate still passes actual program audio after preview cycles");
    obs_frontend_set_current_preview_scene(obs_scene_get_source(blank));obs_frontend_preview_program_trigger_transition();
    QTimer::singleShot(900,[clip,blank,media]{check(!obs_source_active(clip),"Real audio regression test has fully left program before checking preview silence");obs_frontend_set_current_preview_scene(obs_scene_get_source(media));watchPreview=true;
     QTimer::singleShot(500,[clip,blank,media]{check(previewNoise==0,"Real preview return after program playback is also exactly silent");finish(clip,blank,media);});
    });
   });
  });
 });
}
static QDialog *editorDialog(){for(auto *w:QApplication::topLevelWidgets())if(w->objectName()=="sunjoo_cue_editor")return qobject_cast<QDialog*>(w);return nullptr;}
static void openEditor(obs_source_t *clip){auto *filter=obs_source_get_filter_by_name(clip,"Cue Control");auto *p=obs_source_properties(filter);check(p&&obs_properties_get(p,"edit_range"),"Timeline editor button available in filter properties");if(p&&obs_properties_get(p,"edit_range"))obs_property_button_clicked(obs_properties_get(p,"edit_range"),filter);obs_properties_destroy(p);obs_source_release(filter);}
static void dragTimeline(QWidget *timeline,double from,double to){
 if(!timeline)return;auto point=[&](double ratio){return QPointF(24+ratio*(timeline->width()-48),44);};
 for(auto type:{QEvent::MouseButtonPress,QEvent::MouseMove,QEvent::MouseButtonRelease}){auto pos=point(type==QEvent::MouseButtonPress?from:to);QMouseEvent e(type,pos,QPointF(timeline->mapToGlobal(pos.toPoint())),type==QEvent::MouseMove?Qt::NoButton:Qt::LeftButton,type==QEvent::MouseButtonRelease?Qt::NoButton:Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(timeline,&e);}
}
static void editorTest(obs_source_t *clip,obs_scene_t *blank,obs_scene_t *media){
 obs_frontend_preview_program_trigger_transition();
 QTimer::singleShot(850,[clip,blank,media]{openEditor(clip);
  QTimer::singleShot(700,[clip,blank,media]{auto *dialog=editorDialog();check(dialog&&dialog->property("editor_ready").toBool(),"Independent timeline editor loads actual video");if(!dialog){finish(clip,blank,media);return;}
   check(dialog->findChild<QLabel*>("cue_editor_signature")!=nullptr,"Editor displays SunjooAn and version signature");
   auto *timeline=dialog->findChild<QWidget*>("cue_timeline");auto *canvas=dialog->findChild<QWidget*>("cue_video_preview");check(timeline&&canvas&&canvas->property("frame_count").toInt()>0,"Editor decodes and displays its own video frame");
   check(!dialog->findChild<QCheckBox*>("cue_show_thumbnails")->isChecked()&&!dialog->findChild<QCheckBox*>("cue_show_waveform")->isChecked(),"Both visual options are OFF by default");
   check(timeline->height()==84&&timeline->property("thumbnail_count").toInt()==0&&timeline->property("waveform_bins").toInt()==0,"Default timeline has only editing bar and generates no optional data");
   dialog->findChild<QCheckBox*>("cue_show_thumbnails")->setChecked(true);dialog->findChild<QCheckBox*>("cue_show_waveform")->setChecked(true);
   auto before=obs_source_media_get_time(clip);auto originalPicture=clipHash(clip);auto audioBefore=programSound.load();dragTimeline(timeline,0,0.25);dragTimeline(timeline,1,0.75);dragTimeline(timeline,0.5,0.5);
   QTimer::singleShot(600,[clip,blank,media,dialog,timeline,canvas,before,originalPicture,audioBefore]{
    auto duration=timeline->property("duration_ms").toLongLong();check(std::abs(dialog->property("in_ms").toLongLong()-duration/4)<50&&std::abs(dialog->property("out_ms").toLongLong()-duration*3/4)<50,"Mouse dragging independently selects In and Out handles");
    check(std::abs(canvas->property("frame_ms").toLongLong()-duration/2)<70,"Dragging timeline shows decoded frame near selected position, not only keyframe");
    check(obs_source_media_get_state(clip)==OBS_MEDIA_STATE_PLAYING&&obs_source_media_get_time(clip)>before+350&&clipHash(clip)!=originalPicture,"Scrubbing editor cannot pause, rewind or freeze live program");
    check(programSound>audioBefore+1000,"Live program audio continues while the separate editor scrubs");
    check(timeline->property("thumbnail_count").toInt()>0,"Actual decoded video thumbnails appear on timeline");
    check(timeline->property("waveform_bins").toInt()>0&&timeline->height()==216,"Opt-in displays independently decoded audio waveform and thumbnails");
    dialog->grab().save(TEST_EDITOR_IMAGE);
    dialog->findChild<QPushButton*>("cue_zoom_in")->click();
    check(timeline->property("view_end_ms").toLongLong()-timeline->property("view_start_ms").toLongLong()<duration,"Zoom in reduces visible timeline interval");
    dialog->findChild<QScrollBar*>("cue_timeline_pan")->setValue(1000000);
    check(timeline->property("view_start_ms").toLongLong()>0,"Scrollbar pans the enlarged timeline");
    dialog->findChild<QPushButton*>("cue_zoom_out")->click();
    check(timeline->property("view_end_ms").toLongLong()-timeline->property("view_start_ms").toLongLong()==duration,"Zoom out restores full visible interval");
    dragTimeline(timeline,0.25,0.25);dialog->findChild<QPushButton*>("cue_zoom_in")->click();dialog->findChild<QPushButton*>("cue_zoom_in")->click();
    auto previousIn=dialog->property("in_ms").toLongLong();auto viewStart=timeline->property("view_start_ms").toLongLong(),viewEnd=timeline->property("view_end_ms").toLongLong();auto fraction=double(previousIn-viewStart)/(viewEnd-viewStart);dragTimeline(timeline,fraction,fraction+0.01);
    check(dialog->property("in_ms").toLongLong()>previousIn&&dialog->property("in_ms").toLongLong()-previousIn<50,"Zoomed handle drag makes fine sub-50ms In adjustment");
    dialog->findChild<QDoubleSpinBox*>("cue_in_seconds")->setValue(previousIn/1000.0);dialog->findChild<QPushButton*>("cue_zoom_reset")->click();
    check(timeline->property("view_start_ms").toLongLong()==0&&timeline->property("view_end_ms").toLongLong()==duration,"Full-view button resets zoom and panning");
    dialog->findChild<QCheckBox*>("cue_show_thumbnails")->setChecked(false);check(timeline->height()==150&&timeline->property("waveform_visible").toBool(),"Thumbnails can be hidden independently while retaining waveform");
    dialog->findChild<QCheckBox*>("cue_show_waveform")->setChecked(false);check(timeline->height()==84&&timeline->property("waveform_bins").toInt()==0&&timeline->property("thumbnail_count").toInt()==0,"Hiding both clears optional caches and collapses rows");
    for(int i=0;i<20;++i){dialog->findChild<QCheckBox*>("cue_show_waveform")->setChecked(true);dialog->findChild<QCheckBox*>("cue_show_waveform")->setChecked(false);}check(timeline->property("waveform_bins").toInt()==0,"Rapid waveform toggles discard cancelled work");
    auto *filter=obs_source_get_filter_by_name(clip,"Cue Control");auto *s=obs_source_get_settings(filter);check(obs_data_get_double(s,"in_seconds")==0&&obs_data_get_double(s,"out_seconds")==0,"Dragging edits only dialog draft before Apply");obs_data_release(s);obs_source_release(filter);
    dialog->findChild<QPushButton*>("cue_cancel")->click();
    QTimer::singleShot(120,[clip,blank,media]{check(editorDialog()==nullptr,"Cancel closes editor and releases decoder");openEditor(clip);
     QTimer::singleShot(500,[clip,blank,media]{auto *dialog=editorDialog();if(!dialog){check(false,"Editor reopened");finish(clip,blank,media);return;}auto *timeline=dialog->findChild<QWidget*>("cue_timeline");
      check(dialog->property("in_ms").toLongLong()==0,"Cancel did not save draft range");dragTimeline(timeline,0,0.125);dragTimeline(timeline,1,0.5);dragTimeline(timeline,0.25,0.25);
      QTimer::singleShot(400,[clip,blank,media,dialog]{dialog->grab().save(QString(TEST_EDITOR_IMAGE).replace(".png","-default.png"));auto *canvas=dialog->findChild<QWidget*>("cue_video_preview");auto frame=canvas->property("frame_ms").toLongLong();dialog->findChild<QPushButton*>("cue_next")->click();
       QTimer::singleShot(200,[clip,blank,media,dialog,canvas,frame]{check(canvas->property("frame_ms").toLongLong()>frame,"One-frame step changes editor preview independently");dialog->findChild<QPushButton*>("cue_previous")->click();
        QTimer::singleShot(200,[clip,blank,media,dialog,canvas,frame]{check(canvas->property("frame_ms").toLongLong()==frame,"Previous-frame button returns to the exact earlier displayed frame");dialog->findChild<QPushButton*>("cue_next")->click();
        QTimer::singleShot(200,[clip,blank,media,dialog,canvas,frame]{check(canvas->property("frame_ms").toLongLong()>frame,"Next-frame button works again after stepping backward");dialog->findChild<QPushButton*>("cue_play")->click();
        QTimer::singleShot(250,[clip,blank,media,dialog,canvas,frame]{dialog->findChild<QPushButton*>("cue_play")->click();check(canvas->property("frame_ms").toLongLong()>frame+80,"Editor has independent preview playback");auto beforeApply=obs_source_media_get_time(clip);auto chosenIn=dialog->property("in_ms").toLongLong(),chosenOut=dialog->property("out_ms").toLongLong();dialog->findChild<QPushButton*>("cue_apply")->click();
         QTimer::singleShot(250,[clip,blank,media,beforeApply,chosenIn,chosenOut]{auto *f=obs_source_get_filter_by_name(clip,"Cue Control");auto *s=obs_source_get_settings(f);check(std::abs(obs_data_get_double(s,"in_seconds")*1000-chosenIn)<2&&std::abs(obs_data_get_double(s,"out_seconds")*1000-chosenOut)<2,"Apply persists dragged In/Out values to existing filter");obs_data_release(s);obs_source_release(f);
          check(obs_source_media_get_time(clip)>beforeApply+120&&obs_source_media_get_state(clip)==OBS_MEDIA_STATE_PLAYING,"Applying range preserves current live program playback");
          auto *parentSettings=obs_source_get_settings(clip);check(obs_data_get_bool(parentSettings,"looping"),"Applying draft does not change live program loop settings");obs_data_release(parentSettings);
          obs_frontend_source_list transitions={};obs_frontend_get_transitions(&transitions);for(size_t i=0;i<transitions.sources.num;++i)if(!strcmp(obs_source_get_id(transitions.sources.array[i]),"cut_transition"))obs_frontend_set_current_transition(transitions.sources.array[i]);obs_frontend_source_list_free(&transitions);
          obs_frontend_set_current_preview_scene(obs_scene_get_source(blank));obs_frontend_preview_program_trigger_transition();
          QTimer::singleShot(3500,[clip,blank,media,chosenIn]{blog(LOG_INFO,"[FILTER-UI] edited In expected=%lld actual=%lld state=%d",(long long)chosenIn,(long long)obs_source_media_get_time(clip),obs_source_media_get_state(clip));check(obs_source_media_get_state(clip)==OBS_MEDIA_STATE_PAUSED&&std::abs(obs_source_media_get_time(clip)-chosenIn)<100,"Next preview cue uses the visually edited In point");openEditor(clip);QTimer::singleShot(500,[clip,blank,media]{auto *dialog=editorDialog();if(!dialog){check(false,"Lifecycle editor opens");finish(clip,blank,media);return;}dialog->findChild<QCheckBox*>("cue_show_waveform")->setChecked(true);auto *filter=obs_source_get_filter_by_name(clip,"Cue Control");obs_source_filter_remove(clip,filter);obs_source_release(filter);dialog->findChild<QPushButton*>("cue_apply")->click();check(!dialog->property("editor_error").toString().isEmpty(),"Applying after original filter removal is safely rejected");dialog->findChild<QPushButton*>("cue_cancel")->click();QTimer::singleShot(150,[clip,blank,media]{check(editorDialog()==nullptr,"Closing during waveform analysis cancels and joins workers");finish(clip,blank,media);});});});
         });
        });
       });});});
      });
     });
    });
   });
  });
 });
}
static void run(){
 hostBaseline=qEnvironmentVariableIsSet("CUE_HOST_BASELINE");
 editorMode=(qEnvironmentVariableIntValue("FILTER_TEST_EDITOR")==1);
 auto *settings=obs_data_create();obs_data_set_string(settings,"local_file",editorMode?((qEnvironmentVariableIntValue("FILTER_TEST_FRACTIONAL")==1)?TEST_FRACTIONAL_FILE:(qEnvironmentVariableIntValue("FILTER_TEST_WEBM")==1)?TEST_EDITOR_WEBM_FILE:TEST_EDITOR_FILE):((qEnvironmentVariableIntValue("FILTER_TEST_WEBM")==1)?TEST_WEBM_FILE:TEST_FILE));
 obs_data_set_bool(settings,"is_local_file",true);obs_data_set_bool(settings,"clear_on_media_end",false);
 if(editorMode)obs_data_set_bool(settings,"looping",true);
 auto *clip=obs_source_create("ffmpeg_source","Media Cue Filter UI test",settings,nullptr);obs_data_release(settings);
 previewAudioMode=(qEnvironmentVariableIntValue("FILTER_TEST_PREVIEW_AUDIO")==1);
 if(editorMode)watchPreview=false;
 if(clip&&(previewAudioMode||editorMode))obs_source_add_audio_capture_callback(clip,captureAudio,nullptr);
 rangeMode=(qEnvironmentVariableIntValue("FILTER_TEST_RANGE")==1);
 auto *rangeSettings=obs_data_create();if(rangeMode){obs_data_set_double(rangeSettings,"in_seconds",0.5);obs_data_set_double(rangeSettings,"out_seconds",1.2);}
 auto *control=obs_source_create("sunjoo_media_cue_control","Cue Control",rangeSettings,nullptr);obs_data_release(rangeSettings); if(clip&&control)obs_source_filter_add(clip,control);obs_source_release(control);
 check(clip!=nullptr,"Source discovered through real OBS plugin loader");if(!clip){finish(nullptr,nullptr,nullptr);return;}
 auto *blank=obs_scene_create("Cue Blank"),*media=obs_scene_create("Cue Preview");auto *item=obs_scene_add(media,clip);
 obs_video_info vi={};obs_get_video_info(&vi);vec2 scale={};scale.x=scale.y=std::min(float(vi.base_width)/320,float(vi.base_height)/180);obs_sceneitem_set_scale(item,&scale);
 obs_frontend_set_preview_program_mode(false);obs_frontend_set_current_scene(obs_scene_get_source(blank));
 obs_frontend_set_preview_program_mode(true);obs_frontend_set_current_preview_scene(obs_scene_get_source(media));
 obs_frontend_source_list list={};obs_frontend_get_transitions(&list);
 for(size_t i=0;i<list.sources.num;++i)if(!strcmp(obs_source_get_id(list.sources.array[i]),"fade_transition"))obs_frontend_set_current_transition(list.sources.array[i]);
 obs_frontend_source_list_free(&list);obs_frontend_set_transition_duration(600);
 QTimer::singleShot(1200,[clip,blank,media]{
  if(editorMode){editorTest(clip,blank,media);return;}
  if(previewAudioMode){firstHash=clipHash(clip);check(previewNoise==0,"Real initial preview preparation is silent");previewAudioVisit(clip,blank,media,10);return;}
  if(rangeMode){
   check(obs_source_media_get_state(clip)==OBS_MEDIA_STATE_PAUSED&&std::abs(obs_source_media_get_time(clip)-500)<80,"Real range preview holds selected In point");firstHash=clipHash(clip);
   obs_frontend_preview_program_trigger_transition();
   QTimer::singleShot(220,[clip]{blog(LOG_INFO,"[FILTER-UI] range transition time=%lld",(long long)obs_source_media_get_time(clip));check(obs_source_media_get_state(clip)==OBS_MEDIA_STATE_PLAYING&&obs_source_media_get_time(clip)>540,"Real range plays from In during transition");check(clipHash(clip)!=firstHash,"Real range GPU picture advances during transition");});
   QTimer::singleShot(1500,[clip,blank,media]{
    check(obs_source_media_get_state(clip)==OBS_MEDIA_STATE_PAUSED&&obs_source_media_get_time(clip)>=1150&&obs_source_media_get_time(clip)<1300,"Real range stops at selected Out");
    obs_frontend_set_current_preview_scene(obs_scene_get_source(blank));obs_frontend_preview_program_trigger_transition();
    QTimer::singleShot(900,[clip,blank,media]{
     obs_frontend_set_current_preview_scene(obs_scene_get_source(blank));
     QTimer::singleShot(350,[clip,blank,media]{
      check(obs_source_media_get_state(clip)==OBS_MEDIA_STATE_STOPPED,"Real clip absent from BOTH buses is stopped");
      obs_frontend_set_current_preview_scene(obs_scene_get_source(media));
      QTimer::singleShot(1000,[clip,blank,media]{check(obs_source_media_get_state(clip)==OBS_MEDIA_STATE_PAUSED&&std::abs(obs_source_media_get_time(clip)-500)<80,"Real clip reappears at In after off-screen stop");finish(clip,blank,media);});
     });
    });
   });
   return;
  }
  check(obs_frontend_preview_program_mode_active(),"Real Studio Mode enabled");
  firstHash=clipHash(clip);
  check(obs_source_media_get_state(clip)==OBS_MEDIA_STATE_PAUSED&&obs_source_media_get_time(clip)<40,"Real preview holds first frame paused");
  auto *f=obs_source_get_filter_by_name(clip,"Cue Control");auto *p=obs_source_properties(f);obs_source_release(f);check(p&&obs_properties_get(p,"creator"),"Source status and author/version properties available");obs_properties_destroy(p);
  obs_frontend_preview_program_trigger_transition();
  QTimer::singleShot(220,[clip]{check(obs_source_media_get_state(clip)==OBS_MEDIA_STATE_PLAYING&&obs_source_media_get_time(clip)>40,"Real Studio fade starts playback before fade completion");check(clipHash(clip)!=firstHash,"GPU picture actually advances during fade, not only media clock");});
  QTimer::singleShot(1000,[clip,blank,media]{
   check(obs_source_media_get_state(clip)==OBS_MEDIA_STATE_PLAYING&&obs_source_media_get_time(clip)>700,"Real program advances media");
   auto t=obs_source_media_get_time(clip);obs_frontend_set_current_preview_scene(obs_scene_get_source(media));
   QTimer::singleShot(250,[clip,blank,media,t]{
    blog(LOG_INFO,"[FILTER-UI] Shared preview clock before=%lld after=%lld state=%d active=%d",(long long)t,(long long)obs_source_media_get_time(clip),obs_source_media_get_state(clip),obs_source_active(clip));
    check(obs_source_media_get_time(clip)>t+100,"Selecting same live source in preview cannot rewind program");
    check(programImage(),"Real OBS program output contains media pixels (GPU readback)");
    obs_frontend_set_current_preview_scene(obs_scene_get_source(blank));obs_frontend_preview_program_trigger_transition();
    QTimer::singleShot(1000,[clip,blank,media]{
     check(obs_source_media_get_state(clip)==OBS_MEDIA_STATE_PAUSED&&obs_source_media_get_time(clip)<40,"Leaving real program re-arms preview frame zero");
     check(clipHash(clip)==firstHash,"Real preview returns to original first picture exactly");
     obs_frontend_source_list transitions={};obs_frontend_get_transitions(&transitions);bool found=false;
     for(size_t i=0;i<transitions.sources.num;++i)if(!strcmp(obs_source_get_id(transitions.sources.array[i]),"cut_transition")){obs_frontend_set_current_transition(transitions.sources.array[i]);found=true;}
     obs_frontend_source_list_free(&transitions);check(found,"Real CUT transition selected");
     obs_frontend_set_current_preview_scene(obs_scene_get_source(media));obs_frontend_preview_program_trigger_transition();
     QTimer::singleShot(150,[clip,blank,media]{
      check(obs_source_media_get_state(clip)==OBS_MEDIA_STATE_PLAYING&&obs_source_media_get_time(clip)>40,"Real CUT starts media playback");
      obs_frontend_set_current_preview_scene(obs_scene_get_source(blank));obs_frontend_preview_program_trigger_transition();
      QTimer::singleShot(500,[clip,blank,media]{check(obs_source_media_get_state(clip)==OBS_MEDIA_STATE_PAUSED&&obs_source_media_get_time(clip)<40,"Real CUT out re-arms media");finish(clip,blank,media);});
     });
    });
   });
  });
 });
}
static void event(enum obs_frontend_event e,void*){if(e==OBS_FRONTEND_EVENT_FINISHED_LOADING)QTimer::singleShot(1000,run);}
bool obs_module_load(){obs_frontend_add_event_callback(event,nullptr);return true;}
void obs_module_unload(){obs_frontend_remove_event_callback(event,nullptr);}


