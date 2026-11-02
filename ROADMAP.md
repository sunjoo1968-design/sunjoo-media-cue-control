# 현재 구현 / Implemented features

제작자 / Author: **SunjooAn** · 버전 / Version: **0.7.0**

- 0.7.0: 프로그램 Stop 후 첫 프레임 또는 In 프레임 무음 스틸, Play로 cue 재생. 프리뷰 → 프로그램 자동 재생 유지.
- 0.6.0: 두 화면·전환 밖 미디어 즉시 정지, 연속5초 미사용 시 디코더와 파일 핸들 정리, 재진입 무음 cue.
- 기존: 시각적 In/Out 편집, 프레임 이동·확대·축소, 기본 숨김 썸네일·오디오 파형.

The formerly deferred idle decoder unloading is implemented. Program manual still standby is also available. Preparation latency, native decoder destruction and allocator retention remain documented limitations; see [Validation](VALIDATION.md) and [Broadcast review](BROADCAST-REVIEW.md). No further feature or release date is promised here.
