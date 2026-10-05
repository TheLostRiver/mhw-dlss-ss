#requires -Version 5.1
<#
Installs an explicitly staged, hash-pinned experimental build after normal exit.
Does not start, terminate, inject into, or signal the game.
The staged manifest must contain bridge_sha256 and ini_sha256.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$GameDirectory,
    [Parameter(Mandatory=$true)][string]$StageDirectory
)
$ErrorActionPreference = 'Stop'
$gameRoot = (Resolve-Path -LiteralPath $GameDirectory).Path
$stageRoot = (Resolve-Path -LiteralPath $StageDirectory).Path
$manifest = Get-Content -LiteralPath (Join-Path $stageRoot 'manifest.json') -Raw | ConvertFrom-Json

function Require-Hash([string]$Path, [string]$Expected) {
    if ($Expected -notmatch '^[a-fA-F0-9]{64}$' -or (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ine $Expected) {
        throw "File identity differs: $Path"
    }
}
function Require-GameStopped {
    # Small stale launcher processes may still exist; inspect their loaded modules
    # too. An inaccessible module list is never treated as evidence of normal exit.
    foreach ($process in @(Get-Process -Name MonsterHunterWorld -ErrorAction SilentlyContinue)) {
        if ($process.HasExited) { continue }
        $modules = @($process.Modules)
        $loadsBridge = @($modules | Where-Object { $_.ModuleName -ieq 'MhwSrBridge.dll' }).Count -ne 0
        if ($process.WorkingSet64 -gt 64MB -or $loadsBridge) {
            throw "Exit the game normally before installation (PID $($process.Id))."
        }
    }
}
function Set-IniValue([string]$Text,[string]$Section,[string]$Key,[string]$Value) {
    $sectionPattern = '(?ms)^\[' + [regex]::Escape($Section) + '\][^\r\n]*\r?\n(?<body>.*?)(?=^\[|\z)'
    $match = [regex]::Match($Text, $sectionPattern)
    if (-not $match.Success) { throw "Missing INI section: $Section" }
    $body = $match.Groups['body'].Value
    $keyPattern = '(?m)^' + [regex]::Escape($Key) + '\s*=[^\r\n]*'
    if ([regex]::Matches($body,$keyPattern).Count -ne 1) { throw "Missing or duplicated INI key: [$Section] $Key" }
    $newBody = [regex]::Replace($body,$keyPattern,($Key + ' = ' + $Value))
    return $Text.Substring(0,$match.Groups['body'].Index) + $newBody + $Text.Substring($match.Groups['body'].Index+$body.Length)
}

Require-GameStopped
Require-Hash (Join-Path $gameRoot 'MonsterHunterWorld.exe') 'c2ebbbd2c49f216d484e31a5219bed419eb1e5e7d206d02cba040a3ab79d90ea'
Require-Hash (Join-Path $gameRoot 'MHWSS.dll') '55d52caf2e7bba5220ec721149bc067679bf9391bbf55d62bed3ec9522ccd09a'
Require-Hash (Join-Path $gameRoot 'd3d12.dll') 'e78c757c483631364985efe79806fe81ca7f8fc1b9d2b6451b4736052fa72011'
Require-Hash (Join-Path $gameRoot 'OptiScaler\nvngx_dlss.dll') '3975567b8943c53acce397f2b72380092f84f162d00b0d2c7d08a1025c563983'
Require-Hash (Join-Path $stageRoot 'MhwSrBridge.dll') $manifest.bridge_sha256
Require-Hash (Join-Path $stageRoot 'MhwSrBridge.ini') $manifest.ini_sha256

$proxyIni = Join-Path $gameRoot 'OptiScaler.ini'
$text = [IO.File]::ReadAllText($proxyIni)
$changes = @(
    @('Upscalers','Dx12Upscaler','dlss'),
    @('FrameGen','Enabled','false'),
    @('DLSS','Enabled','true'),
    @('DLSS','RenderPresetOverride','false'),
    @('Libraries','NvngxDlssPath','.\OptiScaler\nvngx_dlss.dll'),
    @('Libraries','NvngxFeaturePath','.\OptiScaler'),
    @('OutputScaling','Enabled','false'),
    @('CAS','Enabled','false'),
    @('UpscaleRatio','UpscaleRatioOverrideEnabled','false'),
    @('QualityOverrides','QualityRatioOverrideEnabled','false'),
    @('DRS','DrsMinOverrideEnabled','false'),
    @('DRS','DrsMaxOverrideEnabled','false'),
    @('Hotfix','ColorResourceBarrier','auto'),
    @('Hotfix','MotionVectorResourceBarrier','auto'),
    @('Hotfix','DepthResourceBarrier','auto'),
    @('Hotfix','OutputResourceBarrier','auto'),
    @('Log','LogFileName','MhwSrQuality-OptiScaler.log')
)
foreach ($change in $changes) { $text = Set-IniValue $text $change[0] $change[1] $change[2] }

$pluginRoot = Join-Path $gameRoot 'nativePC\plugins'
if (-not (Test-Path -LiteralPath $pluginRoot -PathType Container)) { throw 'The existing plugin directory is missing.' }
$bridgePath = Join-Path $pluginRoot 'MhwSrBridge.dll'
$bridgeIni = Join-Path $pluginRoot 'MhwSrBridge.ini'
$backupRoot = Join-Path $stageRoot ('backup-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
$null = New-Item -ItemType Directory -Path $backupRoot
foreach ($path in @($proxyIni,$bridgePath,$bridgeIni)) {
    if (Test-Path -LiteralPath $path) { Copy-Item -LiteralPath $path -Destination $backupRoot }
}
Require-GameStopped
try {
    Copy-Item -LiteralPath (Join-Path $stageRoot 'MhwSrBridge.dll') -Destination $bridgePath
    Copy-Item -LiteralPath (Join-Path $stageRoot 'MhwSrBridge.ini') -Destination $bridgeIni
    [IO.File]::WriteAllText($proxyIni,$text,(New-Object Text.UTF8Encoding($false)))
    Require-Hash $bridgePath $manifest.bridge_sha256
    Require-Hash $bridgeIni $manifest.ini_sha256
} catch {
    # Restore only the three concrete files backed up above; do not remove trees.
    foreach ($pair in @(@('OptiScaler.ini',$proxyIni),@('MhwSrBridge.dll',$bridgePath),@('MhwSrBridge.ini',$bridgeIni))) {
        $saved = Join-Path $backupRoot $pair[0]
        if (Test-Path -LiteralPath $saved) { Copy-Item -LiteralPath $saved -Destination $pair[1] }
    }
    throw
}
[pscustomobject]@{
    Installed = $bridgePath
    Backup = $backupRoot
    Mode = 'Quality prototype; waits for explicit scene signal'
    GameStarted = $false
    FrameGeneration = $false
} | ConvertTo-Json
