#requires -Version 5.1
[CmdletBinding(DefaultParameterSetName='Install')]
param(
    [Parameter(Mandatory=$true,ParameterSetName='Install')][string]$GameDirectory,
    [Parameter(Mandatory=$true,ParameterSetName='Install')][string]$StageDirectory,
    [Parameter(Mandatory=$true,ParameterSetName='Restore')][string]$RestoreBackup
)
$ErrorActionPreference = 'Stop'
$supportedKinds = @('standalone-input-capture-v1','standalone-projection-capture-v2')
$allowedFiles = @('MhwSrLauncher.exe','MhwNativeHost.dll','MhwNativeHost.ini','MhwNativeMethods.ini','MhwSr-MinHook-LICENSE.txt')
$allowedDisable = @('d3d12.dll','MHWSS.dll','nativePC\plugins\MhwSrProbe.dll','nativePC\plugins\MhwSrBridge.dll',
    'nativePC\plugins\MhwScreenProbe.dll','nativePC\plugins\MhwScreenProbe_v2.dll','nativePC\plugins\MhwScreenProbe_v3.dll',
    'nativePC\plugins\MhwScreenProbe_v4.dll','nativePC\plugins\MhwScalePilot_v4.dll','nativePC\plugins\MhwQuadPilot.dll','nativePC\plugins\MhwPassProbe.dll')
function Require-Stopped {
    foreach ($process in @(Get-Process -Name MonsterHunterWorld -ErrorAction SilentlyContinue)) {
        if ($process.HasExited -eq $true) { continue }
        $modules = @($process.Modules | Where-Object { $null -ne $_ })
        if ($process.Threads.Count -gt 0 -or $process.WorkingSet64 -gt 64MB -or $modules.Count -gt 0) {
            throw "Exit the game normally before changing files (PID $($process.Id))."
        }
    }
}
function Inside-File([string]$Root,[string]$Relative) {
    $base = [IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    $path = [IO.Path]::GetFullPath((Join-Path $Root $Relative))
    if (-not $path.StartsWith($base,[StringComparison]::OrdinalIgnoreCase)) { throw 'File path leaves the named directory.' }
    if (Test-Path -LiteralPath $path -PathType Container) { throw 'Only individual files can be changed.' }
    return $path
}
function Require-Hash([string]$Path,[string]$Hash) {
    if ($Hash -notmatch '^[0-9a-fA-F]{64}$' -or (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ine $Hash) { throw "File identity differs: $Path" }
}
function Ensure-Parent([string]$File) { $null = New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($File)) -Force }
function Restore-Install([string]$Backup,[object]$State) {
    Require-Stopped
    $root = (Resolve-Path -LiteralPath $State.game_directory).Path
    $retired = Join-Path $Backup ('retired-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
    foreach ($entry in $State.installed) { if ($allowedFiles -notcontains $entry.name) { throw 'Unknown installed-file entry.' } }
    foreach ($entry in $State.disabled) {
        if ($allowedDisable -notcontains $entry.relative) { throw 'Unknown disabled-file entry.' }
        $saved = Inside-File $Backup ('disabled\' + $entry.relative)
        Require-Hash $saved $entry.sha256
        if (Test-Path -LiteralPath (Inside-File $root $entry.relative)) { throw "Restore destination already exists: $($entry.relative)" }
    }
    foreach ($entry in $State.installed) {
        $target = Inside-File $root $entry.name
        if (Test-Path -LiteralPath $target) {
            $preserved = Inside-File $retired $entry.name
            Ensure-Parent $preserved
            Move-Item -LiteralPath $target -Destination $preserved
        }
        if ($entry.existed) { Copy-Item -LiteralPath (Inside-File $Backup ('previous\' + $entry.name)) -Destination $target }
    }
    foreach ($entry in $State.disabled) {
        $target = Inside-File $root $entry.relative
        Ensure-Parent $target
        Move-Item -LiteralPath (Inside-File $Backup ('disabled\' + $entry.relative)) -Destination $target
    }
    [pscustomobject]@{Restored=$root;PreservedStandaloneFiles=$retired;SteamLaunchOptionChanged=$false;GameStarted=$false} | ConvertTo-Json
}

Require-Stopped
if ($PSCmdlet.ParameterSetName -eq 'Restore') {
    $backupRoot = (Resolve-Path -LiteralPath $RestoreBackup).Path
    $state = Get-Content -LiteralPath (Join-Path $backupRoot 'install-state.json') -Raw | ConvertFrom-Json
    if ($supportedKinds -notcontains $state.kind) { throw 'Unknown backup format.' }
    Restore-Install $backupRoot $state
    return
}
$gameRoot = (Resolve-Path -LiteralPath $GameDirectory).Path
$stageRoot = (Resolve-Path -LiteralPath $StageDirectory).Path
$manifest = Get-Content -LiteralPath (Join-Path $stageRoot 'manifest.json') -Raw | ConvertFrom-Json
if ($supportedKinds -notcontains $manifest.kind -or $manifest.files.Count -ne $allowedFiles.Count) { throw 'Unexpected stage manifest.' }
Require-Hash (Inside-File $gameRoot 'MonsterHunterWorld.exe') 'c2ebbbd2c49f216d484e31a5219bed419eb1e5e7d206d02cba040a3ab79d90ea'
$seen = @{}
foreach ($entry in $manifest.files) {
    if ($allowedFiles -notcontains $entry.name -or $seen.ContainsKey($entry.name)) { throw 'Unexpected or duplicate staged-file entry.' }
    $seen[$entry.name] = $true
    Require-Hash (Inside-File $stageRoot $entry.name) $entry.sha256
}
foreach ($entry in $manifest.disable) {
    if ($allowedDisable -notcontains $entry.relative) { throw 'Unexpected disable entry.' }
    Require-Hash (Inside-File $gameRoot $entry.relative) $entry.sha256
}
$backupRoot = Join-Path $stageRoot ('backup-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
$null = New-Item -ItemType Directory -Path $backupRoot
$state = [ordered]@{kind=$manifest.kind;game_directory=$gameRoot;installed=@();disabled=@();game_started=$false}
foreach ($entry in $manifest.files) {
    $target = Inside-File $gameRoot $entry.name
    $existed = Test-Path -LiteralPath $target
    if ($existed) { $saved = Inside-File $backupRoot ('previous\' + $entry.name); Ensure-Parent $saved; Copy-Item -LiteralPath $target -Destination $saved }
    $state.installed += [pscustomobject]@{name=$entry.name;existed=$existed;sha256=$entry.sha256}
}
Require-Stopped
try {
    foreach ($entry in $manifest.disable) {
        $saved = Inside-File $backupRoot ('disabled\' + $entry.relative)
        Ensure-Parent $saved
        Move-Item -LiteralPath (Inside-File $gameRoot $entry.relative) -Destination $saved
        $state.disabled += $entry
        $state | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $backupRoot 'install-state.json') -Encoding UTF8
    }
    foreach ($entry in $manifest.files) {
        $target = Inside-File $gameRoot $entry.name
        Copy-Item -LiteralPath (Inside-File $stageRoot $entry.name) -Destination $target
        Require-Hash $target $entry.sha256
    }
    $state | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $backupRoot 'install-state.json') -Encoding UTF8
} catch {
    $failure = $_
    $state | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $backupRoot 'install-state.json') -Encoding UTF8
    Restore-Install $backupRoot ([pscustomobject]$state)
    throw $failure
}
[pscustomobject]@{Installed=$gameRoot;Backup=$backupRoot;Stage='Native input capture and optional bounded projection pulse; SR is not enabled';
    SteamLaunchOption='cmd /c start "" MhwSrLauncher.exe & rem %command%';GameStarted=$false} | ConvertTo-Json
