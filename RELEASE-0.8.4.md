# Sunjoo Media Cue Control 0.8.4

**제작자 SunjooAn · Windows x64**

OBS 기본 미디어 소스에 붙이는 Media Cue Control 필터에서 프리뷰 Play를 누르면 필터가 자동으로 큐 대기로 되돌리던 문제를 수정했습니다.

- **Play**를 누르면 프리뷰에서 영상을 무음으로 확인할 수 있습니다.
- **Pause**를 누르면 현재 프레임을 유지합니다.
- **Stop**을 누르면 첫 프레임 또는 설정한 In 지점으로 돌아갑니다.

OBS 32.2.2와 OBS 33.0.0에서 전체 구간 및 In/Out 구간 MP4의 프리뷰 조작과 전체 구간의 프로그램 대기를 검증했습니다.

## 메디힐 Cut 검증

격리 OBS 33.0.0에서 메디힐 9.625~10.750초 구간을 프리뷰에서 무음 재생하다 Cut했습니다. 30fps 로컬 출력 기준으로 명령 후 **46.47ms**에 9.625초 In 스틸, **78.03ms**에 첫 움직임이 나왔습니다. 두 프레임은 연속된 출력 틱에 표시됐고, Out에서는 마지막 화면을 유지하며 무음으로 정지했습니다.

## 설치

OBS 버전에 맞는 ZIP 전체를 풀고, OBS를 종료한 상태에서 `Install-SunjooMediaCueControl.cmd`를 관리자 권한으로 실행하세요. 포터블 설치는 OBS 32용 ZIP의 `obs-plugins` 또는 OBS 33용 ZIP의 `plugins` 폴더를 해당 OBS 루트에 합칩니다.
