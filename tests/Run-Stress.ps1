param(
 [ValidateSet('32','33')][string]$ObsMajor='33',
 [Parameter(Mandatory=$true)][string]$RuntimeRoot,
 [switch]$NativeBaseline
)
$ErrorActionPreference='Stop'
$cueRoot=Split-Path $PSScriptRoot -Parent
$cueWorkspace=[IO.Path]::GetFullPath((Split-Path $cueRoot -Parent)).TrimEnd('\')+'\'
$cueRuntime=(Resolve-Path -LiteralPath $RuntimeRoot).Path
if(!$cueRuntime.StartsWith($cueWorkspace,[StringComparison]::OrdinalIgnoreCase)){throw 'Stress tests require an isolated workspace runtime.'}
$cueBuild=Join-Path $cueRoot "build$ObsMajor/Release"
# libobs locates the mux helper beside the harness executable, not the cwd.
Copy-Item -LiteralPath "$cueRuntime/bin/64bit/obs-ffmpeg-mux.exe" -Destination "$cueBuild/obs-ffmpeg-mux.exe" -Force
$cuePriorPath=$env:PATH
try{
 $env:PATH="$cueRuntime/bin/64bit;"+$env:PATH
 $cueArgs=@($cueRuntime,"$cueBuild/sunjoo-media-cue-filter.dll","$PSScriptRoot/fixtures",'record','hd','practical')
 $cueSuffix=if($NativeBaseline){'-native'}else{''}
 if($NativeBaseline){$cueArgs+='native'}
 $cueLog=Join-Path $PSScriptRoot "stress$ObsMajor-practical$cueSuffix.log"
 & "$cueBuild/stress-tests.exe" @cueArgs *> $cueLog
 $cueExit=$LASTEXITCODE
 Select-String -Path $cueLog -Pattern '^METRIC|^RECORD|^FAIL|^COMPLETE|skipped frames|lagged frames'
 if($cueExit -or (Get-Content $cueLog -Raw) -match '^FAIL:'){throw 'Stress validation failed; inspect log.'}
}finally{$env:PATH=$cuePriorPath}
