function Get-UamRunnerSourceContract([string]$SourceRoot) {
    $SourceRoot = [IO.Path]::GetFullPath($SourceRoot)
    $paths = [Collections.Generic.List[string]]::new()
    foreach ($area in @('src/common','src/remote')) {
        Get-ChildItem -LiteralPath (Join-Path $SourceRoot $area) -File -Recurse | Where-Object { $_.Extension -in @('.h','.cpp','.mm') } | ForEach-Object { $paths.Add([IO.Path]::GetRelativePath($SourceRoot, $_.FullName).Replace('\','/')) }
    }
    $paths.Add('CMakeLists.txt')
    $sorted = $paths.ToArray()
    [Array]::Sort($sorted, [StringComparer]::Ordinal)
    $records = [Text.StringBuilder]::new()
    foreach ($relative in $sorted) {
        $content = [Text.Encoding]::UTF8.GetString([IO.File]::ReadAllBytes((Join-Path $SourceRoot $relative))).Replace("`r`n","`n")
        $algorithm = [Security.Cryptography.SHA256]::Create()
        try { $hash = ([BitConverter]::ToString($algorithm.ComputeHash([Text.Encoding]::UTF8.GetBytes($content)))).Replace('-','').ToLowerInvariant() } finally { $algorithm.Dispose() }
        [void]$records.Append($relative + ':' + $hash + "`n")
    }
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { $fingerprint = ([BitConverter]::ToString($algorithm.ComputeHash([Text.Encoding]::UTF8.GetBytes($records.ToString())))).Replace('-','').ToLowerInvariant() } finally { $algorithm.Dispose() }
    $header = Get-Content -LiteralPath (Join-Path $SourceRoot 'src/remote/runner_protocol.h') -Raw
    if ($header -notmatch 'kRunnerProtocolVersion = ([0-9]+)') { throw 'Runner protocol version is missing.' }
    return @{ sourceFingerprint = $fingerprint; protocolVersion = [int]$matches[1] }
}
