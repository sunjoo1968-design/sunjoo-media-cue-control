param(
 [ValidateSet('32','33')][string]$ObsMajor='33',
 [Parameter(Mandatory=$true)][string]$RuntimeRoot,
 [string[]]$Cases=@('whole-mp4','whole-webm','range-mp4','range-webm','preview-audio-mp4','editor-mp4','editor-webm','editor-fractional')
)
$ErrorActionPreference='Stop'
$cueRoot=Split-Path $PSScriptRoot -Parent
$cueWorkspace=[IO.Path]::GetFullPath((Split-Path $cueRoot -Parent)).TrimEnd('\')+'\'
$cueRuntime=(Resolve-Path -LiteralPath $RuntimeRoot).Path
if(!$cueRuntime.StartsWith($cueWorkspace,[StringComparison]::OrdinalIgnoreCase)){throw 'UI tests may run only in an isolated runtime inside this workspace.'}
$cueBuild=Join-Path $cueRoot "build$ObsMajor"
if($ObsMajor -eq '33'){
 Copy-Item "$cueBuild/Release/sunjoo-media-cue-filter.dll" "$cueRuntime/plugins/sunjoo-media-cue-filter/" -Force
 Copy-Item "$cueBuild/manifest.json" "$cueRuntime/plugins/sunjoo-media-cue-filter/data/manifest.json" -Force
 Copy-Item "$cueBuild/Release/filter-frontend-test.dll" "$cueRuntime/plugins/filter-frontend-test/" -Force
}else{
 Copy-Item "$cueBuild/Release/sunjoo-media-cue-filter.dll" "$cueRuntime/obs-plugins/64bit/" -Force
 Copy-Item "$cueBuild/Release/filter-frontend-test.dll" "$cueRuntime/obs-plugins/64bit/" -Force
}
$cueFlags=@('FILTER_TEST_WEBM','FILTER_TEST_RANGE','FILTER_TEST_PREVIEW_AUDIO','FILTER_TEST_EDITOR','FILTER_TEST_FRACTIONAL')
$cuePrevious=@{};foreach($cueFlag in $cueFlags){$cuePrevious[$cueFlag]=[Environment]::GetEnvironmentVariable($cueFlag,'Process')}
try{
 foreach($cueCase in $Cases){
  foreach($cueFlag in $cueFlags){[Environment]::SetEnvironmentVariable($cueFlag,$null,'Process')}
  if($cueCase -like '*webm'){$env:FILTER_TEST_WEBM='1'}
  if($cueCase -like 'range-*'){$env:FILTER_TEST_RANGE='1'}
  if($cueCase -like 'preview-audio-*'){$env:FILTER_TEST_PREVIEW_AUDIO='1'}
  if($cueCase -like 'editor-*'){$env:FILTER_TEST_EDITOR='1'}
  if($cueCase -like '*fractional'){$env:FILTER_TEST_FRACTIONAL='1'}
  $cueProcess=Start-Process -FilePath "$cueRuntime/bin/64bit/obs64.exe" -ArgumentList '--portable','--multi','--disable-shutdown-check','--minimize-to-tray' -WorkingDirectory "$cueRuntime/bin/64bit" -WindowStyle Hidden -PassThru
  if(!$cueProcess.WaitForExit(45000)){$cueProcess.Kill();throw "UI test timeout: $cueCase"}
  $cueLog=Get-ChildItem "$cueRuntime/config/obs-studio/logs" -Filter '*.txt' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
  $cueDestination=Join-Path $PSScriptRoot "frontend$ObsMajor-$cueCase.log";Copy-Item $cueLog.FullName $cueDestination
  $cueText=Get-Content $cueDestination -Raw
  $cuePasses=([regex]::Matches($cueText,'\[FILTER-UI\] PASS:')).Count
  Write-Output "OBS $ObsMajor $cueCase exit=$($cueProcess.ExitCode) passes=$cuePasses"
  if($cueProcess.ExitCode -ne 0 -or $cueText -match '\[FILTER-UI\] FAIL:' -or $cueText -notmatch '\[FILTER-UI\] COMPLETE failures=0' -or $cueText -notmatch 'Number of memory leaks: 0'){
   Select-String -Path $cueDestination -Pattern '\[FILTER-UI\].*(FAIL|COMPLETE)|Number of memory leaks'
   throw "UI validation failed: $cueCase"
  }
 }
}finally{foreach($cueFlag in $cueFlags){[Environment]::SetEnvironmentVariable($cueFlag,$cuePrevious[$cueFlag],'Process')}}
