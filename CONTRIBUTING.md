# Contributing

Author: **SunjooAn** · Current version: **0.5.0**

버그 보고 시 OBS 버전, 플러그인 버전, 스튜디오 모드 여부, 전환 종류, 미디어 코덱·프레임레이트와 재현 순서를 알려주세요. 로그를 공유할 때 계정 정보·스트림 키·개인 파일 경로를 지우세요. 원본 영상의 공개 권한이 없으면 대신 재현용 테스트 영상을 제공해 주세요.

For bug reports, include the OBS/plugin versions, Studio Mode setting, transition type, media codec/frame rate and reproduction steps. Redact credentials, stream keys and private paths from logs. Share only media you have permission to publish.

Before submitting changes, build against the matching OBS SDK and run relevant tests in an isolated runtime. Check program video/audio continuity, preview silence and cleanup when changing media control or worker lifetimes. Keep the SunjooAn creator/version display and third-party notices. Contributions must be compatible with GPL-2.0-or-later.
