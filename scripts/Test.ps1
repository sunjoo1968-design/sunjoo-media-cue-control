param(
 [ValidateSet('32','33')][string]$ObsMajor='33',
 [Parameter(Mandatory=$true)][string]$ObsRuntime
)
$ErrorActionPreference='Stop'
$cueRoot=Split-Path $PSScriptRoot -Parent
$cueRuntime=(Resolve-Path -LiteralPath $ObsRuntime).Path
$cuePreviousPath=$env:PATH
try {
 $env:PATH=(Join-Path $cueRuntime 'bin/64bit')+';'+$env:PATH
 & "$cueRoot/build$ObsMajor/Release/preview-tests.exe" "$cueRoot/tests/fixtures"
 if($LASTEXITCODE){throw 'Editor preview tests failed'}
 & "$cueRoot/build$ObsMajor/Release/waveform-tests.exe" "$cueRoot/tests/fixtures"
 if($LASTEXITCODE){throw 'Waveform tests failed'}
 & "$cueRoot/build$ObsMajor/Release/idle-tests.exe" $cueRuntime "$cueRoot/build$ObsMajor/Release/sunjoo-media-cue-filter.dll" "$cueRoot/tests/fixtures"
 if($LASTEXITCODE){throw 'Idle lifecycle tests failed'}
 & "$cueRoot/build$ObsMajor/Release/standby-tests.exe" $cueRuntime "$cueRoot/build$ObsMajor/Release/sunjoo-media-cue-filter.dll" "$cueRoot/tests/fixtures"
 if($LASTEXITCODE){throw 'Program standby tests failed'}
 & "$cueRoot/build$ObsMajor/Release/replay-tests.exe" $cueRuntime "$cueRoot/build$ObsMajor/Release/sunjoo-media-cue-filter.dll" "$cueRoot/tests/fixtures"
 if($LASTEXITCODE){throw 'Replay tests failed'}
 & "$cueRoot/build$ObsMajor/Release/filter-tests.exe" $cueRuntime "$cueRoot/build$ObsMajor/Release/sunjoo-media-cue-filter.dll" "$cueRoot/tests/fixtures"
 if($LASTEXITCODE){throw 'Filter integration tests failed'}
} finally {$env:PATH=$cuePreviousPath}
