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
    (Join-Path $packageRoot 'universal_agent_manager.dll'),
    (Join-Path $packageRoot 'chrome_elf.dll'),
    (Join-Path $packagedUi 'index.html'),
    (Join-Path $packageRoot 'libcef.dll'),
    (Join-Path $packageRoot 'icudtl.dat'),
    (Join-Path $packageRoot 'resources.pak'),
    (Join-Path $packageRoot 'locales'),
    $nativeRunner,
    (Join-Path $packageRoot 'remote/uam-runner.sha256'),
    (Join-Path $packageRoot 'remote/uam-runner.version'),
    (Join-Path $packageRoot 'remote/linux-arm64/uam-runner'),
    (Join-Path $packageRoot 'remote/linux-arm64/uam-runner.sha256'),
    (Join-Path $packageRoot 'remote/linux-arm64/uam-runner.version'),
    (Join-Path $packageRoot 'remote/linux-x86_64/uam-runner'),
    (Join-Path $packageRoot 'remote/linux-x86_64/uam-runner.sha256'),
    (Join-Path $packageRoot 'remote/linux-x86_64/uam-runner.version'),
    (Join-Path $packageRoot 'remote/windows-x86_64/uam-runner.exe'),
    (Join-Path $packageRoot 'remote/windows-x86_64/uam-runner.sha256'),
    (Join-Path $packageRoot 'remote/windows-x86_64/uam-runner.version')
)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Packaged Windows artifact is missing $required." }
}
# Gemini Plan and goal-review launches refuse to start without UAM's policy beside the executable.
$geminiPolicy = Join-Path $packageRoot 'gemini-plan-ceiling.toml'
$sourcePolicy = Join-Path $PSScriptRoot '../assets/providers/gemini-plan-ceiling.toml'
if (-not (Test-Path -LiteralPath $geminiPolicy) -or (Get-FileHash -LiteralPath $geminiPolicy).Hash -ne (Get-FileHash -LiteralPath $sourcePolicy).Hash) {
    throw "Packaged Windows artifact is missing the current Gemini Plan policy: $geminiPolicy."
}
foreach ($remoteRunner in @(
    $nativeRunner,
    (Join-Path $packageRoot 'remote/linux-arm64/uam-runner'),
    (Join-Path $packageRoot 'remote/linux-x86_64/uam-runner'),
    (Join-Path $packageRoot 'remote/windows-x86_64/uam-runner.exe')
)) {
    $metadataRoot = Split-Path -Parent $remoteRunner
    $expectedHash = (Get-Content -LiteralPath (Join-Path $metadataRoot 'uam-runner.sha256') -Raw).Trim()
    $actualHash = (Get-FileHash -LiteralPath $remoteRunner -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $expectedHash) { throw "Packaged remote runner checksum mismatch: $remoteRunner." }
}
$nativeRunnerVersion = & $nativeRunner --version
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace(($nativeRunnerVersion -join ''))) {
    throw 'Packaged native Windows runner did not execute.'
}
$nativeRunnerVersion = ($nativeRunnerVersion -join '').Trim()
foreach ($versionFile in @(
    (Join-Path $packageRoot 'remote/uam-runner.version'),
    (Join-Path $packageRoot 'remote/linux-arm64/uam-runner.version'),
    (Join-Path $packageRoot 'remote/linux-x86_64/uam-runner.version'),
    (Join-Path $packageRoot 'remote/windows-x86_64/uam-runner.version')
)) {
    if ((Get-Content -LiteralPath $versionFile -Raw).Trim() -ne $nativeRunnerVersion) {
        throw "Packaged remote runner version mismatch: $versionFile."
    }
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

# universal_agent_manager.exe is CEF's bootstrap.exe, which loads universal_agent_manager.dll inside the sandbox.
$sandboxHelper = @'
using System;
using System.Runtime.InteropServices;
public static class UamProcessIntegrity {
    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr OpenProcess(uint access, bool inherit, int id);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool GetTokenInformation(IntPtr token, int kind, IntPtr data, int length, out int needed);
    [DllImport("advapi32.dll")] static extern IntPtr GetSidSubAuthorityCount(IntPtr sid);
    [DllImport("advapi32.dll")] static extern IntPtr GetSidSubAuthority(IntPtr sid, uint index);
    // Mandatory integrity RID of a process: 0x0000 untrusted, 0x1000 low, 0x2000 medium; -1 when unreadable.
    public static int Level(int id) {
        IntPtr process = OpenProcess(0x1000, false, id); // PROCESS_QUERY_LIMITED_INFORMATION
        if (process == IntPtr.Zero) return -1;
        IntPtr token;
        try { if (!OpenProcessToken(process, 0x0008, out token)) return -1; } finally { CloseHandle(process); }
        try {
            int needed;
            GetTokenInformation(token, 25, IntPtr.Zero, 0, out needed); // TokenIntegrityLevel
            IntPtr data = Marshal.AllocHGlobal(needed);
            try {
                if (!GetTokenInformation(token, 25, data, needed, out needed)) return -1;
                IntPtr sid = Marshal.ReadIntPtr(data);
                int count = Marshal.ReadByte(GetSidSubAuthorityCount(sid));
                return Marshal.ReadInt32(GetSidSubAuthority(sid, (uint)(count - 1)));
            } finally { Marshal.FreeHGlobal(data); }
        } finally { CloseHandle(token); }
    }
}
'@
Add-Type -TypeDefinition $sandboxHelper

New-Item -ItemType Directory -Path $dataRoot | Out-Null
$env:UAM_DATA_DIR = $dataRoot
$process = $null
try {
    $process = Start-Process $executable -PassThru
    # The UI logs this after its first native bridge reply, so it proves the renderer ran and the bridge answered.
    $bridgeMarker = '[frontend] [UI startup] Initial state loaded through the native bridge.'
    $log = Join-Path $dataRoot 'uam.log'
    $bridged = $false
    for ($i = 0; $i -lt 60 -and -not $process.HasExited; $i++) {
        if ((Test-Path -LiteralPath $log) -and (Select-String -LiteralPath $log -SimpleMatch $bridgeMarker -Quiet)) { $bridged = $true; break }
        Start-Sleep -Seconds 1
    }
    if ($process.HasExited) { throw "Packaged Windows app exited during startup with code $($process.ExitCode)." }
    if (-not $bridged) { throw 'Packaged Windows UI did not complete a native bridge round-trip.' }
    # Every CEF process is a copy of the bootstrap executable. Renderers must run sandboxed: below medium integrity
    # (Chromium lowers them to untrusted) and never with --no-sandbox.
    $cefProcesses = @(Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -eq $executable -and $_.CommandLine -match '--type=' })
    $unsandboxed = @($cefProcesses | Where-Object { $_.CommandLine -match '--no-sandbox' })
    if ($unsandboxed.Count -gt 0) { throw "Packaged Windows CEF processes run with --no-sandbox: $(($unsandboxed | ForEach-Object { ($_.CommandLine -split ' ' | Where-Object { $_ -like '--type=*' }) }) -join ', ')." }
    $renderers = @($cefProcesses | Where-Object { $_.CommandLine -match '--type=renderer' })
    if ($renderers.Count -eq 0) { throw 'Packaged Windows app has no CEF renderer process to check.' }
    foreach ($renderer in $renderers) {
        $level = [UamProcessIntegrity]::Level([int]$renderer.ProcessId)
        if ($level -lt 0 -or $level -ge 0x2000) { throw "Packaged Windows renderer $($renderer.ProcessId) is not inside the Chromium sandbox (integrity 0x$('{0:X}' -f $level))." }
    }
    foreach ($process in $cefProcesses) {
        $type = ($process.CommandLine -split ' ' | Where-Object { $_ -like '--type=*' } | Select-Object -First 1)
        Write-Host ("CEF {0} integrity 0x{1:X}" -f $type, [UamProcessIntegrity]::Level([int]$process.ProcessId))
    }
    Stop-Process -Id $process.Id -Force
    if (-not $process.WaitForExit(10000)) { throw 'Packaged Windows app did not stop.' }
} finally {
    if ($null -ne $process -and -not $process.HasExited) { Stop-Process -Id $process.Id -Force }
}

Write-Host 'Packaged Windows smoke test passed.'
