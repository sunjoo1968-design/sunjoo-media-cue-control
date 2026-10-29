# Package only locally verified binaries. Never installs or publishes anything.
$ErrorActionPreference='Stop'
$cueRoot=Split-Path $PSScriptRoot -Parent
$cueWorkspace=Split-Path $cueRoot -Parent
$cueVersion='0.5.0'
$cueRelease=Join-Path $cueRoot "release-$cueVersion"
$cueDist=Join-Path $cueWorkspace 'dist'
$cueCases=@{'whole-mp4'=14;'whole-webm'=14;'range-mp4'=7;'range-webm'=7;'preview-audio-mp4'=8;'editor-mp4'=36;'editor-webm'=36;'editor-fractional'=36}
$cueSummary=[ordered]@{version=$cueVersion;author='SunjooAn';date='2026-10-29';scope='64 filters, 4 simultaneous 1080p sources, local 1080p60 recording';targets=@();total_checks=0;limitations='See BROADCAST-REVIEW.md, including retained decoder memory and excluded overload probes.'}
foreach($cueMajor in @('32','33')){
 $cueCounts=[ordered]@{}
 foreach($cueTest in @(@('integration',64),@('preview',17),@('waveform',18),@('stress',10))){
  $cueName=$cueTest[0];$cueExpected=[int]$cueTest[1];$cueSuffix=if($cueName -eq 'stress'){'-practical'}else{''}
  $cueLog=Get-Content "$cueRoot/tests/$cueName$cueMajor$cueSuffix.log" -Raw
  if($cueLog -notmatch "COMPLETE passes=$cueExpected failures=0" -or $cueLog -match '(?m)^FAIL:'){throw "Invalid $cueName $cueMajor result"}
  $cueCounts[$cueName]=$cueExpected
 }
 foreach($cueCase in $cueCases.Keys){
  $cueLog=Get-Content "$cueRoot/tests/frontend$cueMajor-$cueCase.log" -Raw
  $cuePasses=([regex]::Matches($cueLog,'\[FILTER-UI\] PASS:')).Count
  if($cuePasses -ne $cueCases[$cueCase] -or $cueLog -match '\[FILTER-UI\] FAIL:' -or $cueLog -notmatch 'COMPLETE failures=0' -or $cueLog -notmatch 'Number of memory leaks: 0'){throw "Invalid UI $cueCase $cueMajor result"}
  $cueCounts[$cueCase]=$cuePasses
 }
 $cueDll=Get-Item "$cueRoot/build$cueMajor/Release/sunjoo-media-cue-filter.dll"
 if($cueDll.VersionInfo.FileVersion -ne $cueVersion -or $cueDll.VersionInfo.CompanyName -ne 'SunjooAn'){throw 'DLL version/creator mismatch'}
 $cueRecord=Get-Content "$cueRoot/tests/recording$cueMajor.json" -Raw | ConvertFrom-Json
 if(!$cueRecord.streams.Where({$_.codec_name -eq 'h264' -and $_.width -eq 1920 -and $_.height -eq 1080 -and $_.avg_frame_rate -eq '60/1' -and [int]$_.nb_read_frames -gt 300}) -or !$cueRecord.streams.Where({$_.codec_name -eq 'aac'})){throw 'Recording content validation missing'}
 $cueMetrics=@(Select-String -Path "$cueRoot/tests/stress$cueMajor-practical.log" -Pattern '^METRIC' | ForEach-Object {$_.Line})
 $cueCount=($cueCounts.Values | Measure-Object -Sum).Sum
 $cueSummary.targets+=@{obs_major=$cueMajor;checks=$cueCounts;total=$cueCount;dll_sha256=(Get-FileHash $cueDll.FullName -Algorithm SHA256).Hash;metrics=$cueMetrics;recording=$cueRecord.streams}
 $cueSummary.total_checks+=$cueCount
}
$cueSummary | ConvertTo-Json -Depth 9 | Set-Content "$cueRoot/validation-summary.json" -Encoding utf8
$cueDocuments=@('README.md','CHANGELOG.md','VALIDATION.md','BROADCAST-REVIEW.md','TIMELINE-PROPOSAL.md','LICENSE','NOTICE.txt','validation-summary.json')
$cueZips=@()
foreach($cueMajor in @('32','33')){
 $cueStage=Join-Path $cueRelease "obs$cueMajor"
 cmake --install "$cueRoot/build$cueMajor" --config Release --prefix $cueStage
 if($LASTEXITCODE){throw 'Install staging failed'}
 foreach($cueDoc in $cueDocuments){Copy-Item "$cueRoot/$cueDoc" $cueStage -Force}
 Copy-Item "$cueRoot/docs" $cueStage -Recurse -Force
 Copy-Item "$cueRoot/scripts/Install.ps1" $cueStage -Force
 Set-Content "$cueStage/target-obs.txt" $cueMajor -Encoding ascii
 @'
@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install.ps1"
if errorlevel 1 echo Installation failed. Read the message above.
pause
'@ | Set-Content "$cueStage/Install-SunjooMediaCueControl.cmd" -Encoding ascii
 Get-ChildItem $cueStage -Recurse -File | Where-Object Name -ne 'SHA256SUMS.txt' | Sort-Object FullName | ForEach-Object {"$((Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower())  $($_.FullName.Substring($cueStage.Length+1).Replace('\','/'))"} | Set-Content "$cueStage/SHA256SUMS.txt" -Encoding ascii
 $cueZip=Join-Path $cueDist "OBS$cueMajor-SunjooMediaCueControl-$cueVersion-Windows-x64.zip"
 Compress-Archive "$cueStage/*" $cueZip -Force
 $cueZips+=$cueZip
}
$cueSources=Join-Path $cueRelease 'source/media-cue-filter'
New-Item -ItemType Directory -Force "$cueSources/tests" | Out-Null
foreach($cueDir in @('src','scripts','docs')){Copy-Item "$cueRoot/$cueDir" $cueSources -Recurse -Force}
foreach($cueDoc in $cueDocuments+@('CMakeLists.txt')){Copy-Item "$cueRoot/$cueDoc" $cueSources -Force}
foreach($cueTest in @('integration.cpp','preview.cpp','waveform.cpp','frontend.cpp','stress.cpp','Generate-Fixtures.ps1','Run-Frontend.ps1','Run-Stress.ps1')){Copy-Item "$cueRoot/tests/$cueTest" "$cueSources/tests" -Force}
$cueSourceZip=Join-Path $cueDist "SunjooMediaCueControl-$cueVersion-Sources.zip"
Compress-Archive (Join-Path $cueRelease 'source/*') $cueSourceZip -Force
$cueZips+=$cueSourceZip
foreach($cueZip in $cueZips){$cueArchive=[IO.Compression.ZipFile]::OpenRead($cueZip);try{if($cueArchive.Entries.FullName -match '(filter-tests\.exe|preview-tests\.exe|waveform-tests\.exe|stress-tests\.exe|filter-frontend-test\.dll|obs64\.exe|obs-ffmpeg-mux\.exe|\.log$|\.mkv$)'){throw 'Test/runtime artifact found in release'};if($cueZip -like '*Windows-x64.zip' -and @($cueArchive.Entries.FullName | Where-Object {$_ -like '*.dll'}).Count -ne 1){throw 'Unexpected DLL count'}}finally{$cueArchive.Dispose()}}
$cueZips | ForEach-Object {"$((Get-FileHash $_ -Algorithm SHA256).Hash.ToLower())  $(Split-Path $_ -Leaf)"} | Set-Content (Join-Path $cueDist "SunjooMediaCueControl-$cueVersion-SHA256SUMS.txt") -Encoding ascii
Write-Output "Packaged ${cueVersion}: $($cueSummary.total_checks) verified checks."
Get-Item $cueZips | Select-Object Name,Length
