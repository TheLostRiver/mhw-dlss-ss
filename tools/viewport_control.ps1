param(
    [Parameter(Mandatory=$true)][int]$GameProcessId,
    [Parameter(Mandatory=$true)][ValidateSet('Start','Shutdown')][string]$Action
)
$ErrorActionPreference = 'Stop'
$targetProcess = Get-Process -Id $GameProcessId
if ($targetProcess.ProcessName -ne 'MonsterHunterWorld') { throw 'The target process is not MonsterHunterWorld.' }
$eventName = 'Local\MhwViewportProbe.' + $GameProcessId + '.' + $Action
$captureEvent = [System.Threading.EventWaitHandle]::OpenExisting($eventName)
try {
    if (-not $captureEvent.Set()) { throw 'The observer did not accept the event signal.' }
    [pscustomobject]@{ProcessId=$GameProcessId;Action=$Action;SignaledAt=(Get-Date).ToString('o')} | ConvertTo-Json
} finally { $captureEvent.Dispose() }
