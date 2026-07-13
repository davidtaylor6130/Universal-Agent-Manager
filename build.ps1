$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

$providers = @(
    [ordered]@{ Name = 'Gemini CLI'; Flag = 'UAM_ENABLE_RUNTIME_GEMINI_CLI'; Enabled = $true }
    [ordered]@{ Name = 'Codex CLI'; Flag = 'UAM_ENABLE_RUNTIME_CODEX_CLI'; Enabled = $true }
    [ordered]@{ Name = 'Claude Code CLI'; Flag = 'UAM_ENABLE_RUNTIME_CLAUDE_CLI'; Enabled = $true }
    [ordered]@{ Name = 'OpenCode CLI'; Flag = 'UAM_ENABLE_RUNTIME_OPENCODE_CLI'; Enabled = $true }
    [ordered]@{ Name = 'GitHub Copilot CLI'; Flag = 'UAM_ENABLE_RUNTIME_COPILOT_CLI'; Enabled = $true }
)

while ($true) {
    Write-Host ''
    Write-Host 'Universal Agent Manager build (Windows)'
    for ($i = 0; $i -lt $providers.Count; $i++) {
        $state = if ($providers[$i].Enabled) { 'ON' } else { 'OFF' }
        Write-Host ('  {0}) [{1}] {2}' -f ($i + 1), $state, $providers[$i].Name)
    }
    Write-Host '  B) Build'
    Write-Host '  Q) Quit'
    $choice = (Read-Host 'Toggle a runtime or build').Trim()

    if ($choice -match '^[1-5]$') {
        $index = [int]$choice - 1
        if ($providers[$index].Enabled -and @($providers | Where-Object Enabled).Count -eq 1) {
            Write-Host 'At least one runtime must remain selected.'
        } else {
            $providers[$index].Enabled = -not $providers[$index].Enabled
        }
        continue
    }
    if ($choice -match '^[Bb]$') { break }
    if ($choice -match '^[Qq]$') { exit 0 }
    Write-Host 'Choose 1-5, B, or Q.'
}

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vcvars = if ($env:UAM_VCVARS64) {
        $env:UAM_VCVARS64
    } else {
        'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
    }
    if (-not (Test-Path $vcvars)) {
        throw "MSVC initialization script not found: $vcvars (set UAM_VCVARS64 to override)."
    }
    & $env:COMSPEC /s /c "`"$vcvars`" >nul && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') {
            [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
        }
    }
    if ($LASTEXITCODE -ne 0) { throw 'MSVC initialization failed.' }
}

& npm --prefix UI-V2 ci
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$cmakeArgs = @('-S', '.', '-B', 'Builds', '-DUAM_BUILD_TESTS=OFF')
foreach ($provider in $providers) {
    $state = if ($provider.Enabled) { 'ON' } else { 'OFF' }
    $cmakeArgs += "-D$($provider.Flag)=$state"
}

& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& cmake --build Builds --config Release
exit $LASTEXITCODE
