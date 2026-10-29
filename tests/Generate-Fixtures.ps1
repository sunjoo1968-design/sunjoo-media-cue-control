param([Parameter(Mandatory=$true)][string]$FFmpeg)
$ErrorActionPreference='Stop'
$cueFixtures=Join-Path $PSScriptRoot 'fixtures'
New-Item -ItemType Directory -Force $cueFixtures | Out-Null
& $FFmpeg -hide_banner -loglevel error -y -f lavfi -i 'testsrc2=size=320x180:rate=30:duration=3' -f lavfi -i 'sine=frequency=440:sample_rate=48000:duration=3' -c:v libx264 -preset ultrafast -pix_fmt yuv420p -c:a aac -shortest "$cueFixtures/cue-test.mp4"
if($LASTEXITCODE){throw 'MP4 fixture failed'}
& $FFmpeg -hide_banner -loglevel error -y -i "$cueFixtures/cue-test.mp4" -frames:v 1 -pix_fmt rgba -f rawvideo "$cueFixtures/first.rgba"
if($LASTEXITCODE){throw 'Reference frame failed'}
& $FFmpeg -hide_banner -loglevel error -y -i "$cueFixtures/cue-test.mp4" -c:v prores_ks -profile:v 0 -pix_fmt yuv422p10le -c:a pcm_s16le -ar 44100 "$cueFixtures/cue-test.mov"
if($LASTEXITCODE){throw 'MOV fixture failed'}
& $FFmpeg -hide_banner -loglevel error -y -i "$cueFixtures/cue-test.mp4" -c:v libvpx-vp9 -b:v 400k -c:a libopus "$cueFixtures/cue-test.webm"
if($LASTEXITCODE){throw 'VP9 WebM fixture failed'}
& $FFmpeg -hide_banner -loglevel error -y -i "$cueFixtures/cue-test.mp4" -c:v libvpx -lag-in-frames 0 -b:v 400k -c:a libvorbis "$cueFixtures/cue-test-vp8.webm"
if($LASTEXITCODE){throw 'VP8 WebM fixture failed'}
& $FFmpeg -hide_banner -loglevel error -y -f lavfi -i 'testsrc2=size=320x180:rate=30:duration=12' -f lavfi -i 'sine=frequency=440:sample_rate=48000:duration=12' -c:v libx264 -g 300 -preset ultrafast -pix_fmt yuv420p -c:a aac -shortest "$cueFixtures/editor-test.mp4"
if($LASTEXITCODE){throw 'Long-GOP editor MP4 fixture failed'}
& $FFmpeg -hide_banner -loglevel error -y -i "$cueFixtures/editor-test.mp4" -c:v libvpx-vp9 -g 300 -b:v 400k -c:a libopus "$cueFixtures/editor-test.webm"
if($LASTEXITCODE){throw 'Editor WebM fixture failed'}
& $FFmpeg -hide_banner -loglevel error -y -ss 6 -i "$cueFixtures/editor-test.mp4" -frames:v 1 -pix_fmt rgba -f rawvideo "$cueFixtures/editor-6s.rgba"
if($LASTEXITCODE){throw 'Editor reference frame failed'}
& $FFmpeg -hide_banner -loglevel error -y -f lavfi -i 'testsrc2=size=320x180:rate=30000/1001:duration=12' -f lavfi -i 'sine=frequency=440:sample_rate=48000:duration=12' -c:v libx264 -g 300 -preset ultrafast -pix_fmt yuv420p -c:a aac -shortest "$cueFixtures/fractional-test.mp4"
if($LASTEXITCODE){throw 'Fractional frame-rate fixture failed'}
& $FFmpeg -hide_banner -loglevel error -y -f lavfi -i 'aevalsrc=if(between(t\,1\,2)\,0.5*sin(2*PI*440*t)\,0)|if(between(t\,1\,2)\,-0.5*sin(2*PI*440*t)\,0):s=48000:d=3' -c:a pcm_f32le "$cueFixtures/waveform-test.wav"
if($LASTEXITCODE){throw 'Waveform fixture failed'}
& $FFmpeg -hide_banner -loglevel error -y -i "$cueFixtures/cue-test.mp4" -an -c:v copy "$cueFixtures/no-audio.mp4"
if($LASTEXITCODE){throw 'Silent video fixture failed'}
& $FFmpeg -hide_banner -loglevel error -y -f lavfi -i 'testsrc2=size=1920x1080:rate=30:duration=3' -f lavfi -i 'sine=frequency=440:sample_rate=48000:duration=3' -c:v libx264 -preset ultrafast -g 30 -threads 2 -pix_fmt yuv420p -c:a aac -shortest "$cueFixtures/stress-hd.mp4"
if($LASTEXITCODE){throw 'HD stress fixture failed'}
