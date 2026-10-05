param(
    [Parameter(Mandatory=$true)][int]$GameProcessId,
    [Parameter(Mandatory=$true)][ValidateSet('Capture','Shutdown')][string]$Action
)
$ErrorActionPreference = 'Stop'
$targetProcess = Get-Process -Id $GameProcessId
if ($targetProcess.ProcessName -ne 'MonsterHunterWorld') { throw 'The target process is not MonsterHunterWorld.' }
$captureEvent = [System.Threading.EventWaitHandle]::OpenExisting('Local\MhwPassProbe.' + $GameProcessId + '.' + $Action)
try {
    if (-not $captureEvent.Set()) { throw 'The observer did not accept the signal.' }
    [pscustomobject]@{ProcessId=$GameProcessId;Action=$Action;SignaledAt=(Get-Date).ToString('o')} | ConvertTo-Json
} finally { $captureEvent.Dispose() }
