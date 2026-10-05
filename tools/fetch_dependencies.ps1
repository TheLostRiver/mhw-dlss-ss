param()
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskDependencies = @(
    @{Name='MinHook'; Url='https://github.com/TsudaKageyu/minhook.git'; Commit='8af6b4acae5a9388fd742b56fa79ece89d96f823'; Sparse=@()},
    @{Name='NVIDIA-DLSS-SDK'; Url='https://github.com/NVIDIA/DLSS.git'; Commit='374959484e79a640feaba44c93ac8cfb0a03f5b5'; Sparse=@('include','lib/Windows_x86_64/x64')}
)
function Invoke-DependencyGit {
    param([string[]]$GitArguments)
    & git @GitArguments
    if ($LASTEXITCODE -ne 0) { throw ('Git dependency operation failed: ' + ($GitArguments -join ' ')) }
}
foreach ($taskDependency in $taskDependencies) {
    $taskDestination = Join-Path (Join-Path $taskRoot 'references') $taskDependency.Name
    if (Test-Path -LiteralPath $taskDestination) {
        $taskHead = & git -C $taskDestination rev-parse HEAD
        if ($LASTEXITCODE -ne 0 -or $taskHead -ne $taskDependency.Commit) {
            throw "Existing dependency is not at the pinned commit: $taskDestination. It was not changed."
        }
        Write-Output ($taskDependency.Name + ': pinned checkout already present')
        continue
    }
    Invoke-DependencyGit -GitArguments @('clone','--filter=blob:none','--no-checkout',$taskDependency.Url,$taskDestination)
    if ($taskDependency.Sparse.Count -gt 0) {
        Invoke-DependencyGit -GitArguments (@('-C',$taskDestination,'sparse-checkout','set','--cone') + $taskDependency.Sparse)
    }
    Invoke-DependencyGit -GitArguments @('-C',$taskDestination,'checkout','--detach',$taskDependency.Commit)
    Write-Output ($taskDependency.Name + ': ' + $taskDependency.Commit)
}
