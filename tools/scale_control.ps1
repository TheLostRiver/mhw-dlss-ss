param(
    [Parameter(Mandatory=$true)][int]$GameProcessId,
    [Parameter(Mandatory=$true)][ValidateSet('Start','Cancel')][string]$Action,
    [ValidateSet('MhwScalePilot','MhwQuadPilot','MhwSrBridge')][string]$Observer = 'MhwScalePilot'
)
$ErrorActionPreference = 'Stop'
$gameProcess = Get-Process -Id $GameProcessId
if ($gameProcess.ProcessName -ne 'MonsterHunterWorld') { throw 'The target process is not MonsterHunterWorld.' }
$event = [System.Threading.EventWaitHandle]::OpenExisting('Local\' + $Observer + '.' + $GameProcessId + '.' + $Action)
try {
    if (-not $event.Set()) { throw 'Control signal failed.' }
    [pscustomobject]@{ProcessId=$GameProcessId;Action=$Action;Time=(Get-Date).ToString('o')} | ConvertTo-Json
} finally { $event.Dispose() }
