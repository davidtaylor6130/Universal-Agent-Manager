param(
    [Parameter(Mandatory = $true)] [string] $ArchivePath,
    [Parameter(Mandatory = $true)] [string] $ExpectedUiDist
)

$ErrorActionPreference = 'Stop'
$packageRoot = Join-Path $env:RUNNER_TEMP ("uam-package-" + [guid]::NewGuid().ToString('N'))
$dataRoot = Join-Path $env:RUNNER_TEMP ("uam-smoke-" + [guid]::NewGuid().ToString('N'))

Expand-Archive -LiteralPath $ArchivePath -DestinationPath $packageRoot
$executable = Join-Path $packageRoot 'universal_agent_manager.exe'
$packagedUi = Join-Path $packageRoot 'UI-V2/dist'
$nativeRunner = Join-Path $packageRoot 'remote/uam-runner.exe'
foreach ($required in @(
    $executable,
    (Join-Path $packagedUi 'index.html'),
    (Join-Path $packageRoot 'libcef.dll'),
    (Join-Path $packageRoot 'icudtl.dat'),
    (Join-Path $packageRoot 'resources.pak'),
    (Join-Path $packageRoot 'locales'),
    $nativeRunner,
    ($nativeRunner + '.sha256'),
    (Join-Path $packageRoot 'remote/linux-arm64/uam-runner'),
    (Join-Path $packageRoot 'remote/linux-arm64/uam-runner.sha256'),
    (Join-Path $packageRoot 'remote/linux-x86_64/uam-runner'),
    (Join-Path $packageRoot 'remote/linux-x86_64/uam-runner.sha256'),
    (Join-Path $packageRoot 'remote/windows-x86_64/uam-runner.exe'),
    (Join-Path $packageRoot 'remote/windows-x86_64/uam-runner.sha256')
)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Packaged Windows artifact is missing $required." }
}
foreach ($remoteRunner in @(
    $nativeRunner,
    (Join-Path $packageRoot 'remote/linux-arm64/uam-runner'),
    (Join-Path $packageRoot 'remote/linux-x86_64/uam-runner'),
    (Join-Path $packageRoot 'remote/windows-x86_64/uam-runner.exe')
)) {
    $expectedHash = (Get-Content -LiteralPath ($remoteRunner + '.sha256') -Raw).Trim()
    $actualHash = (Get-FileHash -LiteralPath $remoteRunner -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $expectedHash) { throw "Packaged remote runner checksum mismatch: $remoteRunner." }
}
$nativeRunnerVersion = & $nativeRunner --version
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace(($nativeRunnerVersion -join ''))) {
    throw 'Packaged native Windows runner did not execute.'
}

function Get-TreeDigest([string] $Root) {
    $resolved = (Resolve-Path -LiteralPath $Root).Path
    Get-ChildItem -LiteralPath $resolved -Recurse -File | ForEach-Object {
        "{0}`t{1}" -f [IO.Path]::GetRelativePath($resolved, $_.FullName), (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
    } | Sort-Object
}

if (Compare-Object (Get-TreeDigest $ExpectedUiDist) (Get-TreeDigest $packagedUi)) {
    throw 'Packaged Windows UI does not match the production React build.'
}

New-Item -ItemType Directory -Path $dataRoot | Out-Null
$env:UAM_DATA_DIR = $dataRoot
$process = $null
try {
    $process = Start-Process $executable -PassThru
    Start-Sleep -Seconds 5
    if ($process.HasExited) { throw "Packaged Windows app exited during startup with code $($process.ExitCode)." }
    Stop-Process -Id $process.Id -Force
    if (-not $process.WaitForExit(10000)) { throw 'Packaged Windows app did not stop.' }
} finally {
    if ($null -ne $process -and -not $process.HasExited) { Stop-Process -Id $process.Id -Force }
}

Write-Host 'Packaged Windows smoke test passed.'
