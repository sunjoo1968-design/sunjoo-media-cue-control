param([Parameter(Mandatory=$true)][string]$ObsRoot,[Parameter(Mandatory=$true)][string]$OutputDir)
$ErrorActionPreference='Stop'
$cueVswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$cueVs=& $cueVswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(!$cueVs){throw 'Visual Studio C++ x64 tools required'}
$cueToolset=Get-ChildItem "$cueVs/VC/Tools/MSVC" -Directory | Sort-Object Name -Descending | Select-Object -First 1
$cueTools=Join-Path $cueToolset.FullName 'bin/Hostx64/x64'
New-Item -ItemType Directory -Force $OutputDir | Out-Null
foreach($cueModule in @('obs','obs-frontend-api')){
    $cueExports=& "$cueTools/dumpbin.exe" /nologo /exports "$ObsRoot/bin/64bit/$cueModule.dll"
    if($LASTEXITCODE){throw 'Cannot inspect OBS exports'}
    $cueSymbols=@($cueExports | ForEach-Object {if($_ -match '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+(\S+)'){$Matches[1]}})
    if($cueSymbols.Count -lt 10){throw 'Unexpected OBS export table'}
    @("LIBRARY $cueModule.dll",'EXPORTS')+$cueSymbols | Set-Content "$OutputDir/$cueModule.def" -Encoding ascii
    & "$cueTools/lib.exe" /nologo /machine:x64 "/def:$OutputDir/$cueModule.def" "/out:$OutputDir/$cueModule.lib"
    if($LASTEXITCODE){throw 'Import library generation failed'}
}
