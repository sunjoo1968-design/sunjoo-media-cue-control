// Copyright (C) 2026 SunjooAn. SPDX-License-Identifier: GPL-2.0-or-later.
#include "editor.hpp"
#include "preview-decoder.hpp"
#include "waveform.hpp"
#include <QCheckBox>
#include <obs-frontend-api.h>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLabel>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QScrollBar>
#include <QMap>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <Windows.h>
#endif
namespace {
// Limit aggregate editor decoding, independently of the number of filtered
// sources and open dialogs. Never acquire this from OBS video/audio callbacks.
std::timed_mutex previewSlot;
class PreviewPermit {
 std::unique_lock<std::timed_mutex> lock_{previewSlot,std::defer_lock};
public:
 explicit PreviewPermit(const std::function<bool()> &cancel){while(!cancel()){if(lock_.try_lock_for(std::chrono::milliseconds(10)))break;}}
 bool acquired()const{return lock_.owns_lock();}
};
static QString timecode(int64_t ms){ms=std::max<int64_t>(0,ms);return QString("%1:%2:%3.%4").arg(ms/3600000,2,10,QChar('0')).arg(ms/60000%60,2,10,QChar('0')).arg(ms/1000%60,2,10,QChar('0')).arg(ms%1000,3,10,QChar('0'));}
class Canvas final:public QWidget {
 QImage image_;
public:
 explicit Canvas(QWidget *parent):QWidget(parent){setObjectName("cue_video_preview");setMinimumSize(320,180);setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Expanding);}
 void setPicture(const cue_editor::Picture &p){image_=QImage(p.rgba.data(),p.width,p.height,p.width*4,QImage::Format_RGBA8888).copy();setProperty("frame_ms",qlonglong(p.timeMs));setProperty("next_frame_ms",qlonglong(p.nextTimeMs));setProperty("frame_count",property("frame_count").toInt()+1);update();}
 void paintEvent(QPaintEvent*)override{QPainter painter(this);painter.fillRect(rect(),Qt::black);if(!image_.isNull()){auto size=image_.size();size.scale(this->size(),Qt::KeepAspectRatio);painter.drawImage(QRect(QPoint((width()-size.width())/2,(height()-size.height())/2),size),image_);}else{painter.setPen(Qt::lightGray);painter.drawText(rect(),Qt::AlignCenter,QString::fromUtf8("영상 준비 중…"));}}
};
enum class Part {Cursor,In,Out};
class Timeline final:public QWidget {
 int64_t duration_=1,in_=0,out_=1,cursor_=0,step_=33,start_=0,span_=1;Part drag_=Part::Cursor;bool dragging_=false;
 QMap<qint64,QImage> thumbnails_;
 std::deque<int64_t> thumbnailOrder_;
 bool showThumbnails_=false,showWaveform_=false;
 cue_editor::Waveform waveform_;QString waveMessage_;
 int barY()const{return 28+(showThumbnails_?66:0)+(showWaveform_?66:0);}
 void heightForOptions(){setFixedHeight(barY()+56);setProperty("thumbnails_visible",showThumbnails_);setProperty("waveform_visible",showWaveform_);publish();}
 int x(int64_t ms)const{return 24+int(std::clamp(double(ms-start_)*(width()-48)/span_,-double(width())*2,double(width())*3));}
 int64_t time(double pos)const{return std::clamp<int64_t>(start_+int64_t(std::llround((pos-24)*span_/std::max(1,width()-48))),0,duration_);}
 void publish(bool viewport=false){setProperty("duration_ms",qlonglong(duration_));setProperty("in_ms",qlonglong(in_));setProperty("out_ms",qlonglong(out_));setProperty("cursor_ms",qlonglong(cursor_));setProperty("view_start_ms",qlonglong(start_));setProperty("view_end_ms",qlonglong(start_+span_));setProperty("zoom_factor",double(duration_)/span_);setProperty("thumbnail_count",thumbnails_.size());setProperty("axis_left",24);setProperty("axis_right",width()-24);update();if(viewport&&viewChanged)viewChanged(start_,start_+span_);}
 void move(double pos){auto value=time(pos);if(drag_==Part::In){in_=std::min(value,out_-1);cursor_=in_;}else if(drag_==Part::Out){out_=std::max(value,in_+1);cursor_=std::max(in_,out_-step_);}else cursor_=std::min(value,duration_-1);publish();if(changed)changed(drag_,in_,out_,cursor_);}
public:
 std::function<void(Part,int64_t,int64_t,int64_t)> changed;
 std::function<void(int64_t,int64_t)> viewChanged;
 explicit Timeline(QWidget *parent):QWidget(parent){setObjectName("cue_timeline");heightForOptions();setFocusPolicy(Qt::StrongFocus);setCursor(Qt::PointingHandCursor);setToolTip(QString::fromUtf8("In/Out 손잡이 드래그 · 타임라인 클릭/드래그로 탐색 · Ctrl+휠 확대/축소 · 휠로 구간 이동"));}
 void showThumbnails(bool enabled){showThumbnails_=enabled;if(!enabled){thumbnails_.clear();thumbnailOrder_.clear();}heightForOptions();}
 void showWaveform(bool enabled){showWaveform_=enabled;if(!enabled){waveform_={};setProperty("waveform_bins",0);}heightForOptions();}
 void waveMessage(QString text){waveMessage_=std::move(text);update();}
 void setWaveform(cue_editor::Waveform wave){waveform_=std::move(wave);setProperty("waveform_bins",int(waveform_.peaks.size()));setProperty("waveform_start_ms",qlonglong(waveform_.startMs));setProperty("waveform_end_ms",qlonglong(waveform_.endMs));waveMessage_=waveform_.hasAudio?QString():QString::fromUtf8("오디오 트랙 없음");update();}
 void setRange(int64_t duration,int64_t in,int64_t out,int64_t cursor,int64_t step){bool view=duration_!=duration;duration_=std::max<int64_t>(1,duration);in_=in;out_=out;cursor_=cursor;step_=step;if(view){start_=0;span_=duration_;thumbnails_.clear();thumbnailOrder_.clear();}if(cursor_<start_||cursor_>=start_+span_){start_=std::clamp<int64_t>(cursor_-span_/2,0,duration_-span_);view=true;}publish(view);}
 void zoom(bool in){auto span=in?std::max<int64_t>(std::min(duration_,std::max<int64_t>(100,step_*8)),(span_+1)/2):std::min(duration_,span_*2);span_=span;start_=std::clamp<int64_t>(cursor_-span_/2,0,duration_-span_);publish(true);}
 void resetZoom(){span_=duration_;start_=0;publish(true);}
 void pan(int value){start_=int64_t(std::llround(double(duration_-span_)*value/1000000));publish(true);}
 std::vector<int64_t> thumbnailTargets()const{std::vector<int64_t> result;int cellCount=std::clamp((width()-48)/110,4,12);for(int i=0;i<cellCount;++i)result.push_back(std::min(duration_-1,start_+int64_t(double(span_)*(i+0.5)/cellCount)));return result;}
 bool hasThumbnail(int64_t time)const{return thumbnails_.contains(time);}
 void setThumbnail(int64_t time,QImage image){if(!thumbnails_.contains(time))thumbnailOrder_.push_back(time);thumbnails_.insert(time,std::move(image));while(thumbnailOrder_.size()>64){thumbnails_.remove(thumbnailOrder_.front());thumbnailOrder_.pop_front();}publish();}
 void paintEvent(QPaintEvent*)override{
  QPainter p(this);p.setRenderHint(QPainter::Antialiasing);p.fillRect(rect(),QColor(30,33,39));
  if(showThumbnails_){auto targets=thumbnailTargets();for(size_t i=0;i<targets.size();++i){int left=24+int(i)*(width()-48)/int(targets.size()),right=24+int(i+1)*(width()-48)/int(targets.size());QRect tile(left,28,right-left-1,60);p.fillRect(tile,QColor(50,55,63));auto it=thumbnails_.constFind(targets[i]);if(it!=thumbnails_.cend()){auto size=it.value().size();size.scale(tile.size(),Qt::KeepAspectRatio);p.drawImage(QRect(tile.center()-QPoint(size.width()/2,size.height()/2),size),it.value());}else{p.setPen(Qt::gray);p.drawText(tile,Qt::AlignCenter,"…");}}}
  if(showWaveform_){int top=28+(showThumbnails_?66:0),middle=top+30;QRect area(24,top,width()-48,60);p.fillRect(area,QColor(24,40,48));p.setPen(QColor(68,94,105));p.drawLine(24,middle,width()-24,middle);
   if(waveform_.hasAudio&&!waveform_.peaks.empty()){p.save();p.setClipRect(area);p.setPen(QColor(81,208,203));auto count=waveform_.peaks.size();double length=double(waveform_.endMs-waveform_.startMs);
    for(int px=24;px<width()-24;++px){auto from=time(px),to=time(px+1);if(to<waveform_.startMs||from>=waveform_.endMs)continue;size_t begin=size_t(std::clamp(double(from-waveform_.startMs)*count/length,0.0,double(count-1))),end=size_t(std::clamp(std::ceil(double(to-waveform_.startMs)*count/length),double(begin+1),double(count)));float peak=0;for(size_t i=begin;i<end;++i)peak=std::max(peak,waveform_.peaks[i]);int height=int(std::round(std::sqrt(peak)*27));if(height)p.drawLine(px,middle-height,px,middle+height);}
    p.restore();}
   if(!waveMessage_.isEmpty()){p.setPen(Qt::lightGray);p.drawText(area,Qt::AlignCenter,waveMessage_);}
  }
  int a=std::clamp(x(in_),24,width()-24),b=std::clamp(x(out_),24,width()-24);
  int bar=barY();p.fillRect(QRect(24,28,std::max(0,a-24),bar-28),QColor(0,0,0,145));p.fillRect(QRect(b,28,std::max(0,width()-24-b),bar-28),QColor(0,0,0,145));
  p.fillRect(QRect(24,bar,width()-48,23),QColor(65,70,80));p.fillRect(QRect(a,bar,std::max(1,b-a),23),QColor(34,111,153));
  for(int i=0;i<=4;++i){auto t=start_+span_*i/4;int px=x(t);p.setPen(QColor(180,184,190));p.drawLine(px,bar+26,px,bar+31);p.drawText(QRect(std::clamp(px-65,0,width()-130),bar+34,130,20),Qt::AlignCenter,timecode(t));}
  for(auto part:{Part::In,Part::Out}){auto value=part==Part::In?in_:out_;if(value<start_||value>start_+span_)continue;int px=x(value);p.setPen(Qt::NoPen);p.setBrush(part==Part::In?QColor(69,203,129):QColor(244,101,101));p.drawRoundedRect(QRect(px-7,24,14,bar+4),3,3);p.setPen(Qt::white);p.drawText(QRect(px-22,2,44,20),Qt::AlignCenter,part==Part::In?"In":"Out");}
  if(cursor_>=start_&&cursor_<=start_+span_){p.setPen(QPen(Qt::white,2));p.drawLine(x(cursor_),26,x(cursor_),bar+27);p.setBrush(Qt::white);p.setPen(Qt::NoPen);QPolygon marker;marker<<QPoint(x(cursor_)-5,17)<<QPoint(x(cursor_)+5,17)<<QPoint(x(cursor_),24);p.drawPolygon(marker);}
 }
 void mousePressEvent(QMouseEvent *e)override{if(e->button()!=Qt::LeftButton)return;setFocus();double px=e->position().x();int di=std::abs(int(px)-x(in_)),dout=std::abs(int(px)-x(out_));drag_=std::min(di,dout)<=12?(di<=dout?Part::In:Part::Out):Part::Cursor;dragging_=true;move(px);}
 void mouseMoveEvent(QMouseEvent *e)override{if(dragging_)move(e->position().x());}
 void mouseReleaseEvent(QMouseEvent *e)override{if(dragging_){move(e->position().x());dragging_=false;}}
 void keyPressEvent(QKeyEvent *e)override{if(e->key()!=Qt::Key_Left&&e->key()!=Qt::Key_Right){QWidget::keyPressEvent(e);return;}int64_t current=drag_==Part::In?in_:drag_==Part::Out?out_:cursor_;move(x(std::clamp<int64_t>(current+(e->key()==Qt::Key_Left?-step_:step_),0,duration_)));}
 void wheelEvent(QWheelEvent *e)override{if(e->modifiers()&Qt::ControlModifier)zoom(e->angleDelta().y()>0);else{start_=std::clamp<int64_t>(start_+(e->angleDelta().y()>0?-span_/5:span_/5),0,duration_-span_);publish(true);}e->accept();}
 void resizeEvent(QResizeEvent *e)override{QWidget::resizeEvent(e);publish(true);}
};
class Editor final:public QDialog {
 obs_weak_source_t *filter_;QString path_,owner_,parentUuid_;
 Canvas *canvas_;Timeline *timeline_;QLabel *status_,*clock_,*zoomLabel_;QScrollBar *pan_;QDoubleSpinBox *inSpin_,*outSpin_;QPushButton *play_,*apply_;
 QTimer timer_,thumbnailTimer_;QElapsedTimer playClock_;int64_t duration_=0,in_=0,out_=0,cursor_=0,playStart_=0;double frameMs_=1000.0/30;
 bool playing_=false,ready_=false,busy_=false;std::atomic<bool> stop_{false};std::atomic<uint64_t> sequence_{0};
 std::mutex workMutex_;std::condition_variable wake_;int64_t requested_=0;bool pending_=true,requestedBefore_=false;std::deque<int64_t> thumbnails_;std::atomic<uint64_t> thumbnailSequence_{0};std::thread worker_;
 QCheckBox *thumbnailOption_,*waveformOption_;QTimer waveformTimer_;
 std::thread waveformWorker_;std::mutex waveformMutex_;std::condition_variable waveformWake_;
 bool waveformPending_=false;int64_t waveStart_=0,waveEnd_=1;std::atomic<uint64_t> waveformSequence_{0};
 void queueWaveform(){if(!ready_||!waveformOption_->isChecked())return;{std::lock_guard<std::mutex> lock(waveformMutex_);waveStart_=timeline_->property("view_start_ms").toLongLong();waveEnd_=timeline_->property("view_end_ms").toLongLong();waveformPending_=true;++waveformSequence_;}timeline_->waveMessage(QString::fromUtf8("파형 분석 중…"));if(!waveformWorker_.joinable())waveformWorker_=std::thread([this]{waveformWork();});waveformWake_.notify_one();}
 void waveformWork(){
#ifdef _WIN32
  SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_LOWEST);
#endif
  while(!stop_){uint64_t sequence;int64_t start,end;{std::unique_lock<std::mutex> lock(waveformMutex_);waveformWake_.wait(lock,[this]{return stop_||waveformPending_;});if(stop_)break;sequence=waveformSequence_.load();start=waveStart_;end=waveEnd_;waveformPending_=false;}
   auto cancel=[this,sequence]{return stop_||sequence!=waveformSequence_.load();};
   try{auto wave=cue_editor::analyseWaveform(path_.toUtf8().constData(),start,end,cancel);if(cancel())continue;QMetaObject::invokeMethod(this,[this,sequence,wave=std::move(wave)]()mutable{if(sequence!=waveformSequence_.load()||!waveformOption_->isChecked())return;timeline_->setWaveform(std::move(wave));},Qt::QueuedConnection);}
   catch(const std::exception &e){if(cancel())continue;auto message=QString::fromUtf8(e.what());QMetaObject::invokeMethod(this,[this,sequence,message]{if(sequence==waveformSequence_.load())timeline_->waveMessage(QString::fromUtf8("파형 분석 실패: ")+message);},Qt::QueuedConnection);}
  }
 }
 void pause(){playing_=false;timer_.stop();play_->setText(QString::fromUtf8("▶ 재생"));}
 void sync(){if(!ready_)return;QSignalBlocker a(inSpin_),b(outSpin_);inSpin_->setValue(in_/1000.0);outSpin_->setValue(out_/1000.0);timeline_->setRange(duration_,in_,out_,cursor_,std::max<int64_t>(1,std::llround(frameMs_)));clock_->setText(QString::fromUtf8("현재 %1  /  전체 %2    선택 구간 %3").arg(timecode(cursor_),timecode(duration_),timecode(out_-in_)));setProperty("in_ms",qlonglong(in_));setProperty("out_ms",qlonglong(out_));}
 void request(int64_t target,bool before=false){if(!ready_)return;cursor_=std::clamp<int64_t>(target,0,duration_-1);sync();{std::lock_guard<std::mutex> lock(workMutex_);requested_=cursor_;requestedBefore_=before;pending_=true;++sequence_;}busy_=true;wake_.notify_one();}
 void viewport(int64_t start,int64_t end){auto span=end-start;{QSignalBlocker blocker(pan_);pan_->setEnabled(span<duration_);pan_->setValue(duration_>span?int(double(start)*1000000/(duration_-span)):0);}zoomLabel_->setText(QString::fromUtf8("×%1  %2 – %3").arg(duration_>0?double(duration_)/span:1.0,0,'f',1).arg(timecode(start),timecode(end)));if(ready_&&thumbnailOption_->isChecked())thumbnailTimer_.start(80);if(ready_&&waveformOption_->isChecked()){++waveformSequence_;waveformTimer_.start(150);}}
 void error(const QString &text){pause();busy_=false;apply_->setEnabled(false);status_->setText(text);setProperty("editor_error",text);}
 void apply(){
  auto *filter=obs_weak_source_get_source(filter_);if(!filter){error(QString::fromUtf8("원본 필터가 제거되어 적용할 수 없습니다."));return;}
  auto *parent=obs_get_source_by_uuid(parentUuid_.toUtf8().constData());
  bool matches=false;
  if(parent){auto *attached=obs_source_get_filter_by_name(parent,obs_source_get_name(filter));auto *settings=obs_source_get_settings(parent);matches=attached==filter&&obs_data_get_bool(settings,"is_local_file")&&QString::fromUtf8(obs_data_get_string(settings,"local_file"))==path_;obs_data_release(settings);obs_source_release(attached);obs_source_release(parent);}
  if(!matches){obs_source_release(filter);error(QString::fromUtf8("원본 소스·파일이 변경되었거나 필터가 제거되었습니다. 창을 닫고 다시 여세요."));return;}
  auto *settings=obs_source_get_settings(filter);obs_data_set_double(settings,"in_seconds",in_/1000.0);obs_data_set_double(settings,"out_seconds",out_==duration_?0.0:out_/1000.0);obs_source_update(filter,settings);obs_data_release(settings);obs_source_update_properties(filter);obs_source_release(filter);accept();
 }
 void work(){
#ifdef _WIN32
  SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
#endif
  try {
   cue_editor::PreviewDecoder decoder(stop_);{PreviewPermit permit([this]{return stop_.load();});if(!permit.acquired())return;decoder.open(path_.toUtf8().constData());}
   auto duration=decoder.durationMs;auto frame=decoder.frameMs;
   QMetaObject::invokeMethod(this,[this,duration,frame]{duration_=duration;frameMs_=frame;in_=std::clamp<int64_t>(in_,0,duration_-1);out_=out_<=0?duration_:std::clamp<int64_t>(out_,in_+1,duration_);cursor_=in_;ready_=true;inSpin_->setMaximum((duration_-1)/1000.0);outSpin_->setMaximum(duration_/1000.0);timeline_->setEnabled(true);inSpin_->setEnabled(true);outSpin_->setEnabled(true);apply_->setEnabled(true);status_->setText(QString::fromUtf8("편집 전용 무음 미리보기 · 프로그램 재생에 영향을 주지 않습니다."));setProperty("editor_ready",true);sync();request(in_);},Qt::QueuedConnection);
   while(!stop_){int64_t target;uint64_t sequence,thumbnailSequence;bool thumbnail=false,before=false;{std::unique_lock<std::mutex> lock(workMutex_);wake_.wait(lock,[&]{return stop_||pending_||!thumbnails_.empty();});if(stop_)break;sequence=sequence_.load();thumbnailSequence=thumbnailSequence_.load();if(pending_){target=requested_;before=requestedBefore_;pending_=false;}else{thumbnail=true;target=thumbnails_.front();thumbnails_.pop_front();}}
    auto cancel=[&]{return stop_||sequence_.load()!=sequence||(thumbnail&&thumbnailSequence_.load()!=thumbnailSequence);};
    cue_editor::Picture picture;{PreviewPermit permit(cancel);if(permit.acquired()&&!cancel())picture=before?decoder.frameBefore(target,cancel):decoder.frameAt(target,cancel);}if(stop_)break;
    if(picture.rgba.empty()){if(thumbnail&&thumbnailSequence_.load()==thumbnailSequence){std::lock_guard<std::mutex> lock(workMutex_);if(thumbnails_.size()<16)thumbnails_.push_back(target);}continue;}
    if(thumbnail){auto image=QImage(picture.rgba.data(),picture.width,picture.height,picture.width*4,QImage::Format_RGBA8888).scaled(160,90,Qt::KeepAspectRatio,Qt::SmoothTransformation).copy();QMetaObject::invokeMethod(this,[this,thumbnailSequence,target,image=std::move(image)]{if(thumbnailSequence!=thumbnailSequence_.load())return;timeline_->setThumbnail(target,image);},Qt::QueuedConnection);}
    else QMetaObject::invokeMethod(this,[this,sequence,before,picture=std::move(picture)]{if(sequence!=sequence_.load())return;busy_=false;if(before){cursor_=picture.timeMs;sync();}canvas_->setPicture(picture);status_->setText(QString::fromUtf8("표시 프레임 %1 · 편집 미리보기는 무음입니다.").arg(timecode(picture.timeMs)));},Qt::QueuedConnection);
   }
  }catch(const std::exception &e){if(!stop_){auto message=QString::fromUtf8(e.what());QMetaObject::invokeMethod(this,[this,message]{error(QString::fromUtf8("영상 준비 실패: ")+message);},Qt::QueuedConnection);}}
 }
public:
 Editor(obs_source_t *filter,QString parentUuid,QString path,double in,double out):QDialog(static_cast<QWidget*>(obs_frontend_get_main_window())),filter_(obs_source_get_weak_source(filter)),path_(std::move(path)),owner_(QString::fromUtf8(obs_source_get_uuid(filter))),parentUuid_(std::move(parentUuid)){
  setObjectName("sunjoo_cue_editor");setProperty("owner_uuid",owner_);setWindowTitle(QString::fromUtf8("Sunjoo Media Cue Control — 구간 편집"));setAttribute(Qt::WA_DeleteOnClose);setModal(false);resize(1000,850);setMinimumSize(760,680);
  in_=int64_t(std::max(0.0,in)*1000);out_=int64_t(std::max(0.0,out)*1000);requested_=in_;
  auto *layout=new QVBoxLayout(this);layout->setSpacing(10);auto *file=new QLabel(QFileInfo(path_).fileName(),this);file->setToolTip(path_);file->setTextInteractionFlags(Qt::TextSelectableByMouse);layout->addWidget(file);
  canvas_=new Canvas(this);layout->addWidget(canvas_,1);clock_=new QLabel(this);layout->addWidget(clock_);
  auto *options=new QHBoxLayout;thumbnailOption_=new QCheckBox(QString::fromUtf8("영상 썸네일"),this);thumbnailOption_->setObjectName("cue_show_thumbnails");waveformOption_=new QCheckBox(QString::fromUtf8("오디오 파형"),this);waveformOption_->setObjectName("cue_show_waveform");options->addWidget(thumbnailOption_);options->addWidget(waveformOption_);options->addStretch();layout->addLayout(options);
  auto *zoomRow=new QHBoxLayout;auto *zoomOut=new QPushButton(QString::fromUtf8("− 축소"),this);zoomOut->setObjectName("cue_zoom_out");auto *zoomIn=new QPushButton(QString::fromUtf8("+ 확대"),this);zoomIn->setObjectName("cue_zoom_in");auto *zoomReset=new QPushButton(QString::fromUtf8("전체 보기"),this);zoomReset->setObjectName("cue_zoom_reset");zoomLabel_=new QLabel(this);zoomLabel_->setObjectName("cue_zoom_label");zoomRow->addWidget(zoomOut);zoomRow->addWidget(zoomIn);zoomRow->addWidget(zoomReset);zoomRow->addStretch();zoomRow->addWidget(zoomLabel_);layout->addLayout(zoomRow);
  timeline_=new Timeline(this);timeline_->setEnabled(false);layout->addWidget(timeline_);pan_=new QScrollBar(Qt::Horizontal,this);pan_->setObjectName("cue_timeline_pan");pan_->setRange(0,1000000);pan_->setPageStep(100000);pan_->setEnabled(false);layout->addWidget(pan_);
  connect(thumbnailOption_,&QCheckBox::toggled,this,[this](bool enabled){thumbnailTimer_.stop();{std::lock_guard<std::mutex> lock(workMutex_);thumbnails_.clear();++thumbnailSequence_;}timeline_->showThumbnails(enabled);if(enabled&&ready_)thumbnailTimer_.start(80);});
  connect(waveformOption_,&QCheckBox::toggled,this,[this](bool enabled){waveformTimer_.stop();{std::lock_guard<std::mutex> lock(waveformMutex_);++waveformSequence_;waveformPending_=false;}timeline_->showWaveform(enabled);if(enabled)queueWaveform();});
  waveformTimer_.setSingleShot(true);connect(&waveformTimer_,&QTimer::timeout,this,[this]{queueWaveform();});
  connect(zoomIn,&QPushButton::clicked,this,[this]{if(ready_)timeline_->zoom(true);});connect(zoomOut,&QPushButton::clicked,this,[this]{if(ready_)timeline_->zoom(false);});connect(zoomReset,&QPushButton::clicked,this,[this]{if(ready_)timeline_->resetZoom();});connect(pan_,&QScrollBar::valueChanged,this,[this](int value){timeline_->pan(value);});timeline_->viewChanged=[this](int64_t start,int64_t end){viewport(start,end);};
  auto *controls=new QHBoxLayout;auto button=[&](QString label,const char *id,std::function<void()> fn){auto *b=new QPushButton(label,this);b->setObjectName(id);controls->addWidget(b);connect(b,&QPushButton::clicked,this,std::move(fn));return b;};
  button(QString::fromUtf8("◀ 1프레임"),"cue_previous",[this]{pause();request(canvas_->property("frame_ms").toLongLong(),true);});
  play_=button(QString::fromUtf8("▶ 재생"),"cue_play",[this]{if(!ready_)return;if(playing_){pause();return;}if(cursor_<in_||cursor_>=out_-1)request(in_);playing_=true;playStart_=cursor_;playClock_.restart();play_->setText(QString::fromUtf8("Ⅱ 일시정지"));timer_.start(30);});
  button(QString::fromUtf8("1프레임 ▶"),"cue_next",[this]{pause();request(canvas_->property("next_frame_ms").toLongLong());});
  button(QString::fromUtf8("현재 → In"),"cue_mark_in",[this]{if(!ready_)return;pause();in_=std::min(cursor_,out_-1);sync();});
  button(QString::fromUtf8("현재 → Out"),"cue_mark_out",[this]{if(!ready_)return;pause();out_=std::clamp<int64_t>(cursor_+std::llround(frameMs_),in_+1,duration_);sync();});
  button(QString::fromUtf8("전체 구간"),"cue_reset",[this]{if(!ready_)return;pause();in_=0;out_=duration_;request(0);});layout->addLayout(controls);
  auto *numbers=new QHBoxLayout;inSpin_=new QDoubleSpinBox(this);outSpin_=new QDoubleSpinBox(this);inSpin_->setObjectName("cue_in_seconds");outSpin_->setObjectName("cue_out_seconds");for(auto *spin:{inSpin_,outSpin_}){spin->setDecimals(3);spin->setRange(0,604800);spin->setSingleStep(0.001);spin->setSuffix(QString::fromUtf8(" 초"));spin->setEnabled(false);}
  numbers->addWidget(new QLabel("In",this));numbers->addWidget(inSpin_);numbers->addWidget(new QLabel("Out",this));numbers->addWidget(outSpin_);layout->addLayout(numbers);
  connect(inSpin_,&QDoubleSpinBox::valueChanged,this,[this](double value){pause();in_=std::clamp<int64_t>(std::llround(value*1000),0,out_-1);request(in_);});
  connect(outSpin_,&QDoubleSpinBox::valueChanged,this,[this](double value){pause();out_=std::clamp<int64_t>(std::llround(value*1000),in_+1,duration_);request(std::max(in_,out_-int64_t(std::llround(frameMs_))));});
  timeline_->changed=[this](Part,int64_t in,int64_t out,int64_t cursor){pause();in_=in;out_=out;request(cursor);};
  status_=new QLabel(QString::fromUtf8("편집용 영상 디코더를 준비하고 있습니다…"),this);status_->setWordWrap(true);layout->addWidget(status_);
  auto *footer=new QHBoxLayout;auto *signature=new QLabel("SunjooAn · " FILTER_VERSION " · OBS " FILTER_OBS_MAJOR,this);signature->setObjectName("cue_editor_signature");footer->addWidget(signature);footer->addStretch();auto *buttons=new QDialogButtonBox(QDialogButtonBox::Apply|QDialogButtonBox::Cancel,this);apply_=buttons->button(QDialogButtonBox::Apply);apply_->setText(QString::fromUtf8("적용"));apply_->setObjectName("cue_apply");apply_->setEnabled(false);buttons->button(QDialogButtonBox::Cancel)->setText(QString::fromUtf8("취소"));buttons->button(QDialogButtonBox::Cancel)->setObjectName("cue_cancel");footer->addWidget(buttons);layout->addLayout(footer);
  connect(buttons->button(QDialogButtonBox::Cancel),&QPushButton::clicked,this,&QDialog::reject);connect(apply_,&QPushButton::clicked,this,[this]{apply();});
  connect(&timer_,&QTimer::timeout,this,[this]{auto target=playStart_+playClock_.elapsed();if(target>=out_){pause();request(std::max(in_,out_-int64_t(std::llround(frameMs_))));}else if(!busy_)request(target);});
  thumbnailTimer_.setSingleShot(true);connect(&thumbnailTimer_,&QTimer::timeout,this,[this]{if(!thumbnailOption_->isChecked())return;auto targets=timeline_->thumbnailTargets();{std::lock_guard<std::mutex> lock(workMutex_);thumbnails_.clear();++thumbnailSequence_;for(auto target:targets)if(!timeline_->hasThumbnail(target))thumbnails_.push_back(target);}wake_.notify_one();});
  worker_=std::thread([this]{work();});
 }
 ~Editor()override{pause();thumbnailTimer_.stop();waveformTimer_.stop();stop_=true;wake_.notify_all();waveformWake_.notify_all();if(worker_.joinable())worker_.join();if(waveformWorker_.joinable())waveformWorker_.join();obs_weak_source_release(filter_);}
};
std::vector<QPointer<Editor>> editors;
QPointer<QObject> editorDispatcher;
}
void cue_editor_open(obs_source_t *filter,const char *parentUuid,const char *localFile,double in,double out){
 if(!qApp||!filter||!localFile||!*localFile)return;
 // OBS properties invoke this on the UI thread. Use a module-owned queued-call
 // context, so shutdown also cancels open requests that have not run yet.
 if(QThread::currentThread()!=qApp->thread())return;
 if(!editorDispatcher)editorDispatcher=new QObject(qApp);
 auto weak=std::shared_ptr<obs_weak_source_t>(obs_source_get_weak_source(filter),obs_weak_source_release);auto path=QString::fromUtf8(localFile);auto parent=QString::fromUtf8(parentUuid);
 QMetaObject::invokeMethod(editorDispatcher,[weak,path,parent,in,out]{auto *source=obs_weak_source_get_source(weak.get());if(!source)return;auto uuid=QString::fromUtf8(obs_source_get_uuid(source));
  editors.erase(std::remove_if(editors.begin(),editors.end(),[](auto &p){return p.isNull();}),editors.end());for(auto &editor:editors)if(editor&&editor->property("owner_uuid").toString()==uuid){editor->show();editor->raise();editor->activateWindow();obs_source_release(source);return;}
  auto *editor=new Editor(source,parent,path,in,out);editors.push_back(editor);editor->show();obs_source_release(source);
 },Qt::QueuedConnection);
}
void cue_editor_shutdown(){if(!qApp)return;auto close=[] {delete editorDispatcher.data();editorDispatcher=nullptr;for(auto &editor:editors)if(editor)delete editor.data();editors.clear();};if(QThread::currentThread()==qApp->thread())close();else QMetaObject::invokeMethod(qApp,close,Qt::BlockingQueuedConnection);}

