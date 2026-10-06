param(
    [Parameter(Mandatory=$true)][int]$GameProcessId,
    [Parameter(Mandatory=$true)][ValidateSet('Start','Cancel')][string]$Action,
    [ValidateSet('MhwScalePilot','MhwQuadPilot','MhwSrBridge','MhwNativeHost')][string]$Observer = 'MhwScalePilot',
    [switch]$WaitForForeground,
    [switch]$WaitForF8,
    [ValidateRange(1,300)][int]$ForegroundTimeoutSeconds = 120
)
$ErrorActionPreference = 'Stop'
$gameProcess = Get-Process -Id $GameProcessId
if ($gameProcess.ProcessName -ne 'MonsterHunterWorld') { throw 'The target process is not MonsterHunterWorld.' }
if (($WaitForForeground -or $WaitForF8) -and $Action -ne 'Start') { throw 'Foreground/key waiting is only supported for Start; Cancel must be immediate.' }
$event = [System.Threading.EventWaitHandle]::OpenExisting('Local\' + $Observer + '.' + $GameProcessId + '.' + $Action)
try {
    if ($WaitForForeground -or $WaitForF8) {
        if (-not ('MhwSrForeground' -as [type])) {
            Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class MhwSrForeground {
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("user32.dll")] static extern short GetAsyncKeyState(int key);
    public static bool F8Down() { return (GetAsyncKeyState(0x77) & 0x8000) != 0; }
    public static uint ProcessId() {
        uint processId;
        GetWindowThreadProcessId(GetForegroundWindow(), out processId);
        return processId;
    }
}
'@
        }
        $state = if ($WaitForF8) { 'waiting_for_f8_in_game' } else { 'waiting_for_game_foreground' }
        [pscustomobject]@{ProcessId=$GameProcessId;State=$state;TimeoutSeconds=$ForegroundTimeoutSeconds} | ConvertTo-Json -Compress
        $watch = [System.Diagnostics.Stopwatch]::StartNew()
        $foregroundSince = -1L
        $keyReleased = $false
        while ($true) {
            if ($gameProcess.HasExited) { throw 'The game exited before the control signal.' }
            $elapsed = $watch.ElapsedMilliseconds
            if ($elapsed -ge 1000L * $ForegroundTimeoutSeconds) { throw 'Foreground/key wait timed out; no control signal was sent.' }
            if ([MhwSrForeground]::ProcessId() -eq $GameProcessId) {
                if ($foregroundSince -lt 0) { $foregroundSince = $elapsed }
                $keyDown = [MhwSrForeground]::F8Down()
                if (-not $keyDown) { $keyReleased = $true }
                if ($elapsed - $foregroundSince -ge 2000 -and (-not $WaitForF8 -or ($keyReleased -and $keyDown))) { break }
            } else { $foregroundSince = -1L; $keyReleased = $false }
            Start-Sleep -Milliseconds 25
        }
    }
    if (-not $event.Set()) { throw 'Control signal failed.' }
    [pscustomobject]@{ProcessId=$GameProcessId;Action=$Action;Time=(Get-Date).ToString('o')} | ConvertTo-Json
} finally { $event.Dispose() }
