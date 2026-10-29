# 다음 버전 계획 / Roadmap

제작자 / Author: **SunjooAn** · 현재 공개 버전 / Current release: **0.5.0**

## 보류: 화면 밖 미디어의 디코더 메모리 회수

사용자 요청에 따라 다음 버전으로 보류했습니다. **0.5.0에는 자동 해제 기능이 없습니다.**

- 프로그램·프리뷰·진행 중 전환에서 모두 사용하지 않는 미디어만 대상으로 합니다.
- 즉시 재생·오디오 정지 후 일정 유예 시간을 두고 디코더를 닫는 방식을 검토합니다. 초기 제안은 5초이며 구현 후 조정할 수 있습니다.
- 프로그램과 프리뷰에서 공유 중인 소스, 전환의 이전·다음 장면에 사용 중인 소스는 유지합니다.
- 다시 프리뷰에 올리면 무음으로 첫 프레임 또는 In 지점을 준비하고 정지합니다.
- 디코더를 닫은 상태에서 준비 없이 바로 CUT하면 파일 재개 시간이 필요할 수 있습니다.
- OBS 기본 디코더의 종료·재개를 사용하는 과정에서 재생 상태와 실제 자원 해제를 따로 추적하고, 해제 중 재진입하는 경합을 검사해야 합니다.
- 64개 필터를 적용해 두고 고해상도 영상 몇 개를 재생·녹화하는 조건에서 메모리 감소, 빠른 전환, 오디오 무음, 렌더링 지연을 검증해야 합니다.

구현·호환성 검사가 끝나기 전까지 출시 일정과 절감량은 확정하지 않습니다. 현재 버전의 측정과 제한은 [방송 운용 검토](BROADCAST-REVIEW.md)를 참조하세요.

## Deferred: unload decoders for unused media

Decoder unloading is deferred to a future version and is **not implemented in 0.5.0**. The proposed design keeps program, preview, shared and transitioning sources loaded; stops unused media immediately; and closes idle decoders after a grace period. Re-entering preview must silently prepare the first frame or configured In point. A direct CUT before preparation can require reload time. Memory savings, shutdown/re-entry races and recording performance must be measured before release.
