param([string]$ObsRoot = (Join-Path $env:ProgramFiles 'obs-studio'))
$ErrorActionPreference = 'Stop'
$cueMajor = (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'target-obs.txt') -Raw).Trim()
if ($cueMajor -notin @('32','33')) { throw 'Invalid package target.' }
if (!(Test-Path -LiteralPath "$ObsRoot/bin/64bit/obs.dll")) { throw 'OBS installation not found. Use -ObsRoot for a portable/custom OBS folder.' }
$cueVersion = (Get-Item -LiteralPath "$ObsRoot/bin/64bit/obs.dll").VersionInfo.FileVersion
if ($cueVersion -notmatch "^$cueMajor\.") { throw "This package is for OBS $cueMajor. Installed OBS is $cueVersion. Use the matching package." }
if ($cueMajor -eq '32' -and [version]$cueVersion -lt [version]'32.2.2') { throw 'The OBS 32 package requires OBS 32.2.2 or newer in the 32 series.' }
foreach($cueDependency in @('avformat-62.dll','avcodec-62.dll','avutil-60.dll','swscale-9.dll','Qt6Core.dll','Qt6Gui.dll','Qt6Widgets.dll')) {
 if(!(Test-Path -LiteralPath "$ObsRoot/bin/64bit/$cueDependency")){throw "Matching OBS FFmpeg 8 / Qt 6 runtime required. Missing $cueDependency."}
}
if (Get-Process obs64 -ErrorAction SilentlyContinue) { throw 'Close OBS before installing. OBS will not be stopped automatically.' }
$cueIsAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (!$cueIsAdmin) { throw 'Run the installer CMD as administrator.' }
$cueName = 'sunjoo-media-cue-filter'
$cueStamp = (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0,8)
if ($cueMajor -eq '33') {
    $cueSource = Join-Path $PSScriptRoot "plugins/$cueName"
    $cueRoot = [IO.Path]::GetFullPath((Join-Path $env:ProgramData 'obs-studio/plugins'))
    $cueTarget = [IO.Path]::GetFullPath((Join-Path $cueRoot $cueName))
    $cueBackups = [IO.Path]::GetFullPath((Join-Path $env:ProgramData 'obs-studio/plugin-backups'))
    $cueBackup = [IO.Path]::GetFullPath((Join-Path $cueBackups "$cueName-$cueStamp"))
    if (!(Test-Path -LiteralPath "$cueSource/$cueName.dll") -or !(Test-Path -LiteralPath "$cueSource/data/manifest.json")) { throw 'Extract the complete ZIP before installing.' }
    if ((Split-Path $cueTarget -Parent) -ne $cueRoot -or (Split-Path $cueBackup -Parent) -ne $cueBackups) { throw 'Unsafe installation path.' }
    if (Test-Path -LiteralPath $cueTarget) {
        if ((Get-Item -LiteralPath $cueTarget).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Refusing a linked plugin directory.' }
        New-Item -ItemType Directory -Force $cueBackups | Out-Null
        Copy-Item -LiteralPath $cueTarget -Destination $cueBackup -Recurse
        Write-Host "Backup: $cueBackup"
    }
    New-Item -ItemType Directory -Force $cueTarget | Out-Null
    Get-ChildItem -LiteralPath $cueSource | Copy-Item -Destination $cueTarget -Recurse -Force
} else {
    $cueSource = Join-Path $PSScriptRoot "obs-plugins/64bit/$cueName.dll"
    $cueTarget = Join-Path $ObsRoot "obs-plugins/64bit/$cueName.dll"
    if (!(Test-Path -LiteralPath $cueSource)) { throw 'Extract the complete ZIP before installing.' }
    if (Test-Path -LiteralPath $cueTarget) {
        $cueBackups = Join-Path $ObsRoot 'plugin-backups'
        New-Item -ItemType Directory -Force $cueBackups | Out-Null
        Copy-Item -LiteralPath $cueTarget -Destination (Join-Path $cueBackups "$cueName-$cueStamp.dll")
    }
    New-Item -ItemType Directory -Force (Split-Path $cueTarget -Parent) | Out-Null
    Copy-Item -LiteralPath $cueSource -Destination $cueTarget -Force
}
Write-Host "Installed Sunjoo Media Cue Control 0.5.0 / SunjooAn / OBS $cueMajor"
Write-Host 'Start OBS Studio Mode. Drag a video into OBS, then add Sunjoo Media Cue Control under Audio/Video Filters of that Media Source.'





