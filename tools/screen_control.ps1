param(
    [Parameter(Mandatory=$true)][int]$GameProcessId,
    [Parameter(Mandatory=$true)][ValidateSet('Capture','Shutdown')][string]$Action
)
$ErrorActionPreference = 'Stop'
$process = Get-Process -Id $GameProcessId
if ($process.ProcessName -ne 'MonsterHunterWorld') { throw 'The target process is not MonsterHunterWorld.' }
$control = [System.Threading.EventWaitHandle]::OpenExisting('Local\MhwScreenProbe.' + $GameProcessId + '.' + $Action)
try {
    if (-not $control.Set()) { throw 'The observer did not accept the signal.' }
    [pscustomobject]@{ProcessId=$GameProcessId;Action=$Action;Time=(Get-Date).ToString('o')} | ConvertTo-Json
} finally { $control.Dispose() }
