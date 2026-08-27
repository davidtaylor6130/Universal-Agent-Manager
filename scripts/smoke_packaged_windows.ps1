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
foreach ($required in @(
    $executable,
    (Join-Path $packagedUi 'index.html'),
    (Join-Path $packageRoot 'libcef.dll'),
    (Join-Path $packageRoot 'icudtl.dat'),
    (Join-Path $packageRoot 'resources.pak'),
    (Join-Path $packageRoot 'locales')
)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Packaged Windows artifact is missing $required." }
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
