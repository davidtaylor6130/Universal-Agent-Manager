function Get-UamRunnerSourceContract([string]$SourceRoot) {
    $SourceRoot = [IO.Path]::GetFullPath($SourceRoot)
    $paths = [Collections.Generic.List[string]]::new()
    foreach ($area in @('src/common','src/remote')) {
        Get-ChildItem -LiteralPath (Join-Path $SourceRoot $area) -File -Recurse | Where-Object { $_.Extension -in @('.h','.cpp','.mm') } | ForEach-Object { $paths.Add($_.FullName) }
    }
    $paths.Add((Join-Path $SourceRoot 'CMakeLists.txt'))
    $sorted = $paths.ToArray()
    [Array]::Sort($sorted, [StringComparer]::Ordinal)
    $records = [Text.StringBuilder]::new()
    foreach ($path in $sorted) {
        $relative = $path.Substring($SourceRoot.Length).TrimStart([char[]]@('/','\')).Replace('\','/')
        $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        [void]$records.Append($relative + ':' + $hash + "`n")
    }
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { $fingerprint = ([BitConverter]::ToString($algorithm.ComputeHash([Text.Encoding]::UTF8.GetBytes($records.ToString())))).Replace('-','').ToLowerInvariant() } finally { $algorithm.Dispose() }
    $header = Get-Content -LiteralPath (Join-Path $SourceRoot 'src/remote/runner_protocol.h') -Raw
    if ($header -notmatch 'kRunnerProtocolVersion = ([0-9]+)') { throw 'Runner protocol version is missing.' }
    return @{ sourceFingerprint = $fingerprint; protocolVersion = [int]$matches[1] }
}
