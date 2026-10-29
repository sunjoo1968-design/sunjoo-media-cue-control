param(
 [ValidateSet('32','33')][string]$ObsMajor='33',
 [Parameter(Mandatory=$true)][string]$ObsSdk,
 [Parameter(Mandatory=$true)][string]$ObsImports,
 [Parameter(Mandatory=$true)][string]$FFmpegSdk,
 [Parameter(Mandatory=$true)][string]$QtSdk
)
$ErrorActionPreference='Stop'
$cueRoot=Split-Path $PSScriptRoot -Parent
$cueBuild=Join-Path $cueRoot "build$ObsMajor"
cmake -S $cueRoot -B $cueBuild -G 'Visual Studio 17 2022' -A x64 "-DOBS_MAJOR=$ObsMajor" "-DOBS_SDK=$ObsSdk" "-DOBS_IMPORTS=$ObsImports" "-DFFMPEG_SDK=$FFmpegSdk" "-DQT_SDK=$QtSdk" -DFILTER_FRONTEND_TEST=OFF
if($LASTEXITCODE){throw 'Configure failed'}
cmake --build $cueBuild --config Release --target sunjoo-media-cue-filter
if($LASTEXITCODE){throw 'Build failed'}
cmake --install $cueBuild --config Release --prefix "$cueRoot/package$ObsMajor"
if($LASTEXITCODE){throw 'Staging failed'}
