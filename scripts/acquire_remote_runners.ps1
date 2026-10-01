param(
    [Parameter(Mandatory=$true)][string]$Version,
    [Parameter(Mandatory=$true)][string]$ArtifactRoot,
    [string]$ArchiveDirectory = '',
    [switch]$Offline,
    [switch]$ValidateOnly
)
$ErrorActionPreference = 'Stop'
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw 'Use the exact released X.Y.Z helper version.' }
Add-Type -AssemblyName System.IO.Compression.FileSystem
. "$PSScriptRoot/runner_source_contract.ps1"
$sourceContract = Get-UamRunnerSourceContract (Join-Path $PSScriptRoot '..')
$targets = @('linux-arm64', 'linux-x86_64', 'windows-x86_64')
function Test-RunnerContract([string]$Root) {
    foreach ($target in $targets) {
        $binary = if ($target -eq 'windows-x86_64') { 'uam-runner.exe' } else { 'uam-runner' }
        $directory = Join-Path $Root $target
        foreach ($name in @($binary, 'uam-runner.sha256', 'uam-runner.version', 'uam-runner.manifest.json')) {
            $path = Join-Path $directory $name
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing $target/$name. Obtain UAM-runner-$target.zip for v$Version or copy that version's CI runner artifact to $directory." }
            if ((Get-Item -LiteralPath $path).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Helper artifacts cannot be links: $path" }
        }
        $manifest = Get-Content -LiteralPath (Join-Path $directory 'uam-runner.manifest.json') -Raw | ConvertFrom-Json
        if ($manifest.sourceFingerprint -cne $sourceContract.sourceFingerprint) { throw "Helper source mismatch for $target. Acquire this source revision's CI artifacts; an older release with the same version cannot provide current runner capabilities." }
        if ($manifest.protocolVersion -ne $sourceContract.protocolVersion) { throw "Helper protocol mismatch for $target." }
        $actualVersion = (Get-Content -LiteralPath (Join-Path $directory 'uam-runner.version') -Raw).Trim()
        if ($actualVersion -ne $Version) { throw "Helper version mismatch for $target. Expected $Version; found $actualVersion." }
        $hash = (Get-Content -LiteralPath (Join-Path $directory 'uam-runner.sha256') -Raw).Trim()
        if ($hash -cnotmatch '^[0-9a-f]{64}$' -or (Get-FileHash -LiteralPath (Join-Path $directory $binary) -Algorithm SHA256).Hash.ToLowerInvariant() -ne $hash) { throw "Helper checksum mismatch for $target. Replace it with the verified v$Version CI/release artifact." }
    }
}
$ArtifactRoot = [IO.Path]::GetFullPath($ArtifactRoot)
if ($ValidateOnly) { Test-RunnerContract $ArtifactRoot; Write-Host "Verified all three UAM helper artifacts for $Version."; return }
try { Test-RunnerContract $ArtifactRoot; Write-Host "Using verified UAM helpers for $Version."; return } catch { if ($Offline -and -not $ArchiveDirectory) { throw "Offline helper acquisition failed. $($_.Exception.Message) Supply -ArchiveDirectory with all three matching runner ZIPs, or populate -ArtifactRoot from CI." } }
if (Test-Path -LiteralPath $ArtifactRoot) {
    $rootItem = Get-Item -LiteralPath $ArtifactRoot
    if (-not $rootItem.PSIsContainer -or ($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'The artifact root must be an ordinary dedicated directory.' }
    foreach ($item in Get-ChildItem -LiteralPath $ArtifactRoot -Force) {
        if ($targets -cnotcontains $item.Name -or -not $item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "The artifact root contains unrelated files: $($item.Name). Use a dedicated helper output directory." }
        $binary = if ($item.Name -eq 'windows-x86_64') { 'uam-runner.exe' } else { 'uam-runner' }
        foreach ($file in Get-ChildItem -LiteralPath $item.FullName -Force) {
            if (@($binary, 'uam-runner.sha256', 'uam-runner.version', 'uam-runner.manifest.json') -cnotcontains $file.Name -or $file.PSIsContainer -or ($file.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "The artifact root contains unrelated or linked files: $($file.FullName)." }
        }
    }
}
$parent = Split-Path -Parent $ArtifactRoot
New-Item -ItemType Directory -Path $parent -Force | Out-Null
$stage = Join-Path $parent ('.uam-helper-stage-' + [guid]::NewGuid().ToString('N'))
$backup = Join-Path $parent ('.uam-helper-backup-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage | Out-Null
try {
    foreach ($target in $targets) {
        $name = "UAM-runner-$target.zip"
        $zip = Join-Path $stage $name
        if ($ArchiveDirectory) {
            $source = Join-Path $ArchiveDirectory $name
            if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing $source. Supply all three runner ZIPs for v$Version." }
            Copy-Item -LiteralPath $source -Destination $zip
        } elseif ($Offline) { throw "Offline: missing $name for v$Version." }
        else {
            $url = "https://github.com/davidtaylor6130/Universal-Agent-Manager/releases/download/v$Version/$name"
            Write-Host "Downloading $name for v$Version..."
            try { Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing -TimeoutSec 120 | Out-Null }
            catch { throw "Could not obtain $name for v$Version. This version may not be released or the connection is offline. Download matching CI runner artifacts into $ArtifactRoot, or supply -ArchiveDirectory. $($_.Exception.Message)" }
        }
        if ((Get-Item -LiteralPath $zip).Length -gt 64MB) { throw "Helper archive exceeds 64 MB: $name" }
        $directory = Join-Path $stage $target
        New-Item -ItemType Directory -Path $directory | Out-Null
        $binary = if ($target -eq 'windows-x86_64') { 'uam-runner.exe' } else { 'uam-runner' }
        $allowed = @($binary, 'uam-runner.sha256', 'uam-runner.version', 'uam-runner.manifest.json')
        $archive = [IO.Compression.ZipFile]::OpenRead($zip)
        try {
            if ($archive.Entries.Count -ne 4) { throw "Expected exactly four helper files in $name." }
            $seen = @{}
            foreach ($entry in $archive.Entries) {
                if ($allowed -cnotcontains $entry.FullName -or $seen.ContainsKey($entry.FullName) -or $entry.Length -gt 64MB -or ($entry.FullName -ne $binary -and $entry.Length -gt 256)) { throw "Unexpected helper archive entry: $($entry.FullName)" }
                $seen[$entry.FullName] = $true
                [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, (Join-Path $directory $entry.FullName), $false)
            }
        } finally { $archive.Dispose() }
        Remove-Item -LiteralPath $zip
    }
    Test-RunnerContract $stage
    if (Test-Path -LiteralPath $ArtifactRoot) { Move-Item -LiteralPath $ArtifactRoot -Destination $backup }
    try { Move-Item -LiteralPath $stage -Destination $ArtifactRoot }
    catch { if (Test-Path -LiteralPath $backup) { Move-Item -LiteralPath $backup -Destination $ArtifactRoot }; throw }
    if (Test-Path -LiteralPath $backup) { Remove-Item -LiteralPath $backup -Recurse -Force }
    Write-Host "Verified and acquired all three UAM helper artifacts for $Version."
} finally { if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force } }
