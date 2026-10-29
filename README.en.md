# Sunjoo Media Cue Control 0.5.0

[한국어](README.md) · Author: **SunjooAn** · Windows x64 · GPL-2.0-or-later

An OBS Studio filter for the built-in **Media Source**. Drag a local video into OBS as usual, then add **Sunjoo Media Cue Control** as an audio/video filter.

- Studio preview: hold the first frame, or the configured In point, silently.
- Program entry: start playback when the transition begins.
- Out point: pause the picture and mute audio.
- Leaving both program and preview: stop video and audio.
- Sources shared with program keep playing. Visible sources in a scene are controlled individually; hidden items and hidden parent groups are excluded.
- A separate, silent video editor provides draggable In/Out handles, frame stepping, zoom and pan.
- Optional video thumbnails and audio waveforms are **both hidden by default**. Enable each checkbox as needed.
- Editing uses separate file readers. Applying a range during program playback takes effect on the next cue/program entry.

## Download and install

Download the ZIP matching your OBS version from [Releases](https://github.com/sunjoo1968-design/sunjoo-media-cue-control/releases/latest).

| Package | Tested target |
|---|---|
| `OBS32-SunjooMediaCueControl-0.5.0-Windows-x64.zip` | OBS Studio 32.2.2, Windows x64 |
| `OBS33-SunjooMediaCueControl-0.5.0-Windows-x64.zip` | OBS Studio 33.0.0, Windows x64 |

1. Close OBS and extract the complete ZIP.
2. Run `Install-SunjooMediaCueControl.cmd` as administrator.
3. For a custom or portable installation, run `Install.ps1 -ObsRoot 'D:\OBS'` in an elevated PowerShell window.
4. Open OBS, enable Studio Mode, and add the filter to each local Media Source, once per source.
5. Select the scene in preview and wait for its first/In frame before transitioning.

The packages use the matching OBS plugin folder layout and include no OBS, Qt or FFmpeg runtime DLLs. Use the runtimes provided by the tested OBS version. The installer checks the target version and dependencies and backs up an existing installation of this plugin.

## Range editor

Open the filter properties and click the range-editor button. Drag the green In and red Out handles, or click the timeline to scrub. Previous/next-frame controls, independent silent playback and numeric fields are available. Enable **영상 썸네일** for thumbnails and **오디오 파형** for waveforms. Ctrl+wheel zooms; the scrollbar or ordinary wheel pans. Apply saves the draft; Cancel discards it. The current UI uses Korean labels.

The waveform uses the maximum absolute amplitude across channels, preserving opposite-phase stereo. It stores at most 32,768 peak bins and reanalyses the visible interval when zoomed. It is a visual editing aid, not a dB meter or sample-accurate playback cutter.

## Validation and limitations

The 0.5.0 binaries passed **534 checks** across OBS 32.2.2 and 33.0.0. Tests used real libobs, D3D11, native media sources, CUT/fade transitions and Qt editor windows. The practical load test used 64 filtered media sources with four 1080p videos playing and local 1080p60 H.264/AAC recording. See [validation](VALIDATION.md), [structured results](validation-summary.json) and [broadcast review](BROADCAST-REVIEW.md).

The native media decoder can retain buffers after a source stops. Visiting all 64 HD sources used about 7.8 GiB of process private memory in the test. Automatic decoder unloading is **deferred to the next version**; see [Roadmap](ROADMAP.md). These results do not guarantee performance on every PC, codec or scene collection.

Local SDR video is the editor target. MP4 H.264/AAC, MOV ProRes/PCM and WebM VP9/Opus or VP8/Vorbis were tested. VLC sources, URLs, HDR preview and independent multi-canvas programs are outside the verified scope. Media preparation still takes time; the filter does not remove black frames already present in a file.

## Build

Use Visual Studio 2022 C++ Build Tools, CMake 3.28+, the matching OBS source SDK/import libraries, Qt 6 SDK and FFmpeg 8 shared SDK. Run `scripts/Build.ps1` with `-ObsMajor`, `-ObsSdk`, `-ObsImports`, `-FFmpegSdk` and `-QtSdk`; see the [build example](README.md#소스-빌드). No SDK or test media is committed.

Generate fixtures with `tests/Generate-Fixtures.ps1`. Tests require an isolated OBS runtime inside the parent workspace. Frontend helper modules automatically close OBS and must never be installed into a production OBS installation. `scripts/Package.ps1` is a maintainer script requiring the recorded validation logs.

## License

Copyright © 2026 **SunjooAn**. Distributed under [GPL-2.0-or-later](LICENSE). OBS Studio, Qt and FFmpeg retain their original copyrights and licenses; see [NOTICE.txt](NOTICE.txt). The creator/version display is not an Authenticode certificate signature. This is an independent project, not an official OBS Studio release.
