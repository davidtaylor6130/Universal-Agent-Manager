$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$root = Join-Path ([IO.Path]::GetTempPath()) ('uam-helper-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
$script = Join-Path $PSScriptRoot 'acquire_remote_runners.ps1'
$count = 0
. "$PSScriptRoot/runner_source_contract.ps1"
$sourceContract = Get-UamRunnerSourceContract (Join-Path $PSScriptRoot "..")
function Assert-Failure([scriptblock]$Action, [string]$Detail) {
    $failed = $false
    try { & $Action } catch { $failed = $true; if ($_.Exception.Message -notlike "*$Detail*") { throw "Unexpected error: $($_.Exception.Message)" } }
    if (-not $failed) { throw "Expected failure: $Detail" }
}
try {
    $fixture = Join-Path $root 'source'
    $contents = @{
        'CMakeLists.txt' = "# Synthetic source contract`n"
        'src/remote/runner_protocol.h' = "constexpr int kRunnerProtocolVersion = 3;`n"
        'src/common/a/Foo.cpp' = "// Synthetic nested source`n"
        'src/common/aZ.cpp' = "// Synthetic sibling source`n"
    }
    foreach ($relative in $contents.Keys) {
        $path = Join-Path $fixture $relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
        [IO.File]::WriteAllText($path, $contents[$relative])
    }
    $probe = Join-Path $root 'contract.cmake'
    $result = Join-Path $root 'fingerprint.txt'
    $canonicalRoot = $fixture.Replace('\','/')
    $canonicalContract = (Join-Path $PSScriptRoot '../cmake/runner_source_contract.cmake').Replace('\','/')
    $canonicalResult = $result.Replace('\','/')
    [IO.File]::WriteAllText($probe, @"
set(UAM_SOURCE_ROOT "$canonicalRoot")
include("$canonicalContract")
file(WRITE "$canonicalResult" "`${UAM_RUNNER_SOURCE_FINGERPRINT}")
"@)
    function Assert-SourceContract {
        & cmake -P $probe
        if ($LASTEXITCODE -ne 0) { throw 'CMake source contract probe failed.' }
        $contract = Get-UamRunnerSourceContract $fixture
        if ($contract.sourceFingerprint -cne [IO.File]::ReadAllText($result)) { throw 'CMake and PowerShell source contracts disagree.' }
        return $contract.sourceFingerprint
    }
    $lfFingerprint = Assert-SourceContract
    $count++
    foreach ($relative in $contents.Keys) { [IO.File]::WriteAllText((Join-Path $fixture $relative), $contents[$relative].Replace("`n","`r`n")) }
    if ((Assert-SourceContract) -cne $lfFingerprint) { throw 'Source fingerprint changed between LF and CRLF checkouts.' }
    $count++
    [IO.File]::AppendAllText((Join-Path $fixture 'src/common/aZ.cpp'), '// Actual code change')
    if ((Assert-SourceContract) -ceq $lfFingerprint) { throw 'Source fingerprint ignored changed source content.' }
    $count++

    $archives = Join-Path $root 'archives'
    New-Item -ItemType Directory -Path $archives | Out-Null
    foreach ($target in @('linux-arm64','linux-x86_64','windows-x86_64')) {
        $files = Join-Path $root $target
        New-Item -ItemType Directory -Path $files | Out-Null
        $binary = if ($target -eq 'windows-x86_64') { 'uam-runner.exe' } else { 'uam-runner' }
        [IO.File]::WriteAllText((Join-Path $files $binary), "fixture-$target")
        [IO.File]::WriteAllText((Join-Path $files 'uam-runner.version'), '4.9.0')
        [IO.File]::WriteAllText((Join-Path $files 'uam-runner.manifest.json'), ($sourceContract | ConvertTo-Json -Compress))
        [IO.File]::WriteAllText((Join-Path $files 'uam-runner.sha256'), (Get-FileHash (Join-Path $files $binary) -Algorithm SHA256).Hash.ToLowerInvariant())
        [IO.Compression.ZipFile]::CreateFromDirectory($files, (Join-Path $archives "UAM-runner-$target.zip"))
    }
    $output = Join-Path $root 'output'
    & $script -Version 4.9.0 -ArtifactRoot $output -ArchiveDirectory $archives -Offline
    & $script -Version 4.9.0 -ArtifactRoot $output -ValidateOnly
    $count++
    & $script -Version 4.9.0 -ArtifactRoot $output -Offline
    $count++
    Assert-Failure { & $script -Version 4.8.0 -ArtifactRoot $output -ValidateOnly } 'version mismatch'
    $count++
    $manifestPath = Join-Path $output 'linux-arm64/uam-runner.manifest.json'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw
    [IO.File]::WriteAllText($manifestPath, '{"sourceFingerprint":"old","protocolVersion":3}')
    Assert-Failure { & $script -Version 4.9.0 -ArtifactRoot $output -ValidateOnly } 'source mismatch'
    $count++
    [IO.File]::WriteAllText($manifestPath, (@{ sourceFingerprint=$sourceContract.sourceFingerprint; protocolVersion=0 } | ConvertTo-Json -Compress))
    Assert-Failure { & $script -Version 4.9.0 -ArtifactRoot $output -ValidateOnly } 'protocol mismatch'
    $count++
    [IO.File]::WriteAllText($manifestPath, $manifest)
    [IO.File]::WriteAllText((Join-Path $output 'linux-arm64/uam-runner'), 'corrupt')
    Assert-Failure { & $script -Version 4.9.0 -ArtifactRoot $output -ValidateOnly } 'checksum mismatch'
    $count++
    Assert-Failure { & $script -Version 4.9.0 -ArtifactRoot $output -Offline } 'Offline helper acquisition failed'
    $count++
    Assert-Failure { & $script -Version 4.9.0 -ArtifactRoot (Join-Path $root 'missing') -ArchiveDirectory (Join-Path $root 'none') -Offline } 'Missing'
    $count++
    $badZip = Join-Path $archives 'UAM-runner-linux-arm64.zip'
    Remove-Item -LiteralPath $badZip
    $archive = [IO.Compression.ZipFile]::Open($badZip, [IO.Compression.ZipArchiveMode]::Create)
    try { foreach ($name in @('../escaped','uam-runner.sha256','uam-runner.version','uam-runner.manifest.json')) { $entry = $archive.CreateEntry($name); $writer = [IO.StreamWriter]::new($entry.Open()); try { $writer.Write('bad') } finally { $writer.Dispose() } } } finally { $archive.Dispose() }
    Assert-Failure { & $script -Version 4.9.0 -ArtifactRoot $output -ArchiveDirectory $archives -Offline } 'Unexpected helper archive entry'
    if ([IO.File]::ReadAllText((Join-Path $output 'linux-arm64/uam-runner')) -ne 'corrupt') { throw 'Failed acquisition changed existing artifacts.' }
    if (Test-Path -LiteralPath (Join-Path $root 'escaped')) { throw 'Archive traversal escaped staging.' }
    $count++
    [IO.File]::WriteAllText((Join-Path $output 'keep-user-file.txt'), 'preserve me')
    Assert-Failure { & $script -Version 4.9.0 -ArtifactRoot $output -ArchiveDirectory $archives -Offline } 'unrelated files'
    if ([IO.File]::ReadAllText((Join-Path $output 'keep-user-file.txt')) -ne 'preserve me') { throw 'Acquisition removed an unrelated file.' }
    $count++
    Write-Host "Remote helper acquisition: $count tests passed."
} finally { Remove-Item -LiteralPath $root -Recurse -Force }
