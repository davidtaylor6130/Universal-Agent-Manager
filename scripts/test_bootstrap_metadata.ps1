param([Parameter(Mandatory=$true)][string]$ScriptsPath)
$ErrorActionPreference='Stop'
$templates=Get-Content -LiteralPath $ScriptsPath -Raw | ConvertFrom-Json
$fixtureRoot=Join-Path ([IO.Path]::GetTempPath()) ('uam-metadata-'+[Guid]::NewGuid().ToString('N'))
$script:passed=0
$oldBinary=[Text.Encoding]::ASCII.GetBytes('prior installed binary')
$newBinary=[Text.Encoding]::ASCII.GetBytes('verified candidate binary')
$oldManifest=[byte[]]@(0,255,13,10,65)
$oldChecksum=[byte[]]@(66,13,10,0,254)
$expectedFingerprint='b'*64

function Assert($condition,$message) { if (-not $condition) { throw $message } }
function Equal-Bytes($path,$bytes) {
    Assert (Test-Path -LiteralPath $path) "Missing file: $path"
    Assert ([Convert]::ToBase64String([IO.File]::ReadAllBytes($path)) -ceq [Convert]::ToBase64String($bytes)) "Changed bytes: $path"
}
function Expect-Failure([scriptblock]$action) {
    $failed=$false
    try { & $action | Out-Null } catch { $failed=$true }
    Assert $failed 'Expected failure did not occur.'
}
function New-Fixture($first=$false,$manifest=$true,$checksum=$true) {
    if (Test-Path -LiteralPath $fixtureRoot) { Remove-Item -LiteralPath $fixtureRoot -Recurse -Force }
    $script:FixtureHome=$fixtureRoot
    $script:directory=Join-Path $fixtureRoot 'r/4.5.7'
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    $script:installed=Join-Path $directory 'uam-runner.exe'
    $script:backup=Join-Path $directory 'uam-runner.rollback-metadata.exe'
    $script:marker=Join-Path $directory 'uam-runner.activation-metadata'
    $script:metadata=Join-Path $directory 'uam-runner.metadata-metadata'
    $script:candidate=Join-Path $directory 'uam-runner.tmp-metadata.exe'
    if (-not $first) { [IO.File]::WriteAllBytes($installed,$oldBinary) }
    if ($manifest) { [IO.File]::WriteAllBytes((Join-Path $directory 'uam-runner.manifest.json'),$oldManifest) }
    if ($checksum) { [IO.File]::WriteAllBytes((Join-Path $directory 'uam-runner.sha256'),$oldChecksum) }
    [IO.File]::WriteAllBytes($candidate,$newBinary)
    [IO.File]::WriteAllText((Join-Path $fixtureRoot 'r/install.lock'),'metadata')
    $script:digest=(Get-FileHash -LiteralPath $candidate -Algorithm SHA256).Hash.ToLowerInvariant()
    $script:mode='normal'
    $script:priorFirst=$first
    $script:priorManifest=$manifest
    $script:priorChecksum=$checksum
    $script:stops=0
    $script:starts=0
    $script:sets=[Collections.Generic.List[string]]::new()
    $script:scripts=@{}
    foreach ($property in $templates.PSObject.Properties) {
        if ($property.Name -ne 'protocol') {
            $script:scripts[$property.Name]=$property.Value.Replace('$HOME','$FixtureHome').Replace(('a'*64),$digest)
        }
    }
}
function Invoke-FixtureScript($name) {
    . ([scriptblock]::Create($script:scripts.common))
    # Parse and execute the production definitions, then inject only native helper
    # invocation and fault controls. Never execute a real helper, SSH or remote stop.
    $tokens=$null; $errors=$null
    $ast=[Management.Automation.Language.Parser]::ParseInput($script:scripts[$name],[ref]$tokens,[ref]$errors)
    Assert ($errors.Count -eq 0) "PowerShell parse failure: $name $errors"
    $body=$script:scripts[$name]
    # Ownership wraps execution in try/finally. Extract definitions recursively,
    # preserving the real fence body while injecting only helper/fault boundaries.
    $definitions=@($ast.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst]},$false))
    foreach ($definition in $definitions) {
        . ([scriptblock]::Create($definition.Extent.Text))
    }
    foreach ($definition in ($definitions | Sort-Object {$_.Extent.StartOffset} -Descending)) {
        $body=$body.Remove($definition.Extent.StartOffset,$definition.Extent.EndOffset-$definition.Extent.StartOffset)
    }
    $replacement=(Get-Command Set-UamRunnerFile -ErrorAction SilentlyContinue).ScriptBlock
    $remove=(Get-Command Remove-UamOptional).ScriptBlock
    function Set-UamRunnerFile($source,$target,$save) {
        $script:sets.Add($source+' -> '+$target)
        if ($script:mode -eq 'expire-publication' -and -not $script:expiry.IsSet) {
            $script:expiry.Set()
            Assert ($script:operationWaiting.Wait(5000)) 'Expired guard did not wait for the publication operation.'
            Assert (Test-Path -LiteralPath $script:marker) 'Expired guard recovered while publication could mutate.'
            Expect-Failure { $unexpected=[IO.File]::Open((Join-Path $FixtureHome 'r/install.lock'),'Open','ReadWrite','None'); $unexpected.Dispose() }
        }
        if ($script:mode -eq 'publication-failure' -and $source.EndsWith('uam-runner.sha256.new')) { throw 'Injected second sidecar failure.' }
        if ($script:mode -eq 'replacement-failure' -and $source.EndsWith('.exe') -and $source.Contains('.tmp-')) { throw 'Injected replacement failure.' }
        if ($script:mode -eq 'restore-failure' -and $source.EndsWith('uam-runner.manifest.json.restore')) { throw 'Injected metadata restoration failure.' }
        & $replacement $source $target $save
    }
    function Remove-UamOptional($paths) {
        if ($script:mode -eq 'commit-cleanup-failure' -and @($paths | ForEach-Object { [IO.Path]::GetFullPath($_) }) -contains [IO.Path]::GetFullPath($script:backup)) { throw 'Injected cleanup failure after commit.' }
        & $remove $paths
    }
    function Invoke-UamRunner($path,$argument,$failure) {
        switch ($argument) {
            stop {
                $script:stops++
                if ($script:mode -in @('busy','rollback-busy')) { throw $failure }
            }
            start {
                $script:starts++
                if ($script:mode -eq 'start-failure') { throw $failure }
                if ($script:mode -eq 'recover') {
                    Assert-Prior $script:priorFirst $script:priorManifest $script:priorChecksum
                    Assert (Test-Path -LiteralPath $script:marker) 'Recovery cleared its marker before restart.'
                }
            }
            --version {
                if ($script:mode -eq 'version-mismatch') { '4.5.6' } else { '4.5.7' }
            }
            --protocol-version {
                if ($script:mode -eq 'protocol-mismatch') { '0' } else { $templates.protocol }
            }
            --source-fingerprint {
                if ($script:mode -eq 'query-failure') { throw $failure }
                if ($script:mode -eq 'source-mismatch' -or ($script:mode -eq 'installed-source-mismatch' -and [IO.Path]::GetFullPath($path) -eq [IO.Path]::GetFullPath($script:installed))) { 'c'*64 }
                elseif ($script:mode -eq 'duplicate-query') { $expectedFingerprint; $expectedFingerprint }
                else { $expectedFingerprint }
            }
            default { throw "Unexpected injected helper command $argument" }
        }
    }
    function Start-Sleep {
        # Allow the real guard loop to exit immediately through its release receipt.
        [IO.File]::WriteAllText((Join-Path $FixtureHome 'r/install.release-metadata'),'')
    }
    $root=Join-Path $FixtureHome 'r'
    . ([scriptblock]::Create($body)) | Out-Null
}
function Assert-Published {
    Equal-Bytes $installed $newBinary
    $manifest=Get-Content -LiteralPath (Join-Path $directory 'uam-runner.manifest.json') -Raw | ConvertFrom-Json
    Assert ($manifest.sourceFingerprint -ceq $expectedFingerprint) 'Wrong installed source fingerprint.'
    Assert ($manifest.protocolVersion -eq [int]$templates.protocol) 'Wrong installed protocol.'
    Assert ([IO.File]::ReadAllText((Join-Path $directory 'uam-runner.sha256')) -ceq ($digest+"`n")) 'Wrong installed checksum.'
}
function Assert-Prior($first=$false,$manifest=$true,$checksum=$true) {
    if ($first) { Assert (-not (Test-Path -LiteralPath $installed)) 'First-install binary survived rollback.' }
    else { Equal-Bytes $installed $oldBinary }
    foreach ($entry in @(@('uam-runner.manifest.json',$manifest,$oldManifest),@('uam-runner.sha256',$checksum,$oldChecksum))) {
        $path=Join-Path $directory $entry[0]
        if ($entry[1]) { Equal-Bytes $path $entry[2] }
        else { Assert (-not (Test-Path -LiteralPath $path)) "Prior absence lost: $path" }
    }
}
function Assert-Clean {
    foreach ($path in @($marker,$backup,$metadata)) { Assert (-not (Test-Path -LiteralPath $path)) "Transaction not settled: $path" }
}
function Case($name,[scriptblock]$test) {
    & $test
    $script:passed++
    Write-Output "PASS $name"
}
try {
    Case 'all generated Windows phases parse' {
        foreach ($property in $templates.PSObject.Properties) {
            if ($property.Name -eq 'protocol') { continue }
            $tokens=$null; $errors=$null
            $null=[Management.Automation.Language.Parser]::ParseInput($property.Value,[ref]$tokens,[ref]$errors)
            Assert ($errors.Count -eq 0) "Parse error: $($property.Name): $errors"
        }
    }
    Case 'native command wrapper refuses a failed query even with expected output' {
        $tokens=$null; $errors=$null
        $ast=[Management.Automation.Language.Parser]::ParseInput($templates.common,[ref]$tokens,[ref]$errors)
        $definition=$ast.EndBlock.Statements | Where-Object { $_ -is [Management.Automation.Language.FunctionDefinitionAst] -and $_.Name -eq 'Invoke-UamRunner' }
        . ([scriptblock]::Create($definition.Extent.Text))
        function Fixture-Helper {
            $global:LASTEXITCODE=17
            '4.5.7'
        }
        Expect-Failure { Invoke-UamRunner Fixture-Helper --version 'Injected nonzero query exit.' }
    }
    Case 'commit publishes corresponding binary manifest checksum' {
        New-Fixture
        Invoke-FixtureScript prepare; Invoke-FixtureScript activate; Invoke-FixtureScript publish
        Assert-Published
        Assert (Test-Path -LiteralPath $marker) 'Lease transaction ended before finalization.'
        Invoke-FixtureScript commit; Invoke-FixtureScript rollback
        Assert-Published; Assert-Clean
    }
    Case 'rollback preserves all prior presence combinations and arbitrary bytes' {
        foreach ($first in @($false,$true)) { foreach ($manifest in @($false,$true)) { foreach ($checksum in @($false,$true)) {
            New-Fixture $first $manifest $checksum
            Invoke-FixtureScript prepare; Invoke-FixtureScript activate; Invoke-FixtureScript publish
            Invoke-FixtureScript rollback
            Assert-Prior $first $manifest $checksum; Assert-Clean
        } } }
    }
    Case 'candidate hash and identity failures occur before stop or publication' {
        foreach ($failure in @('hash-mismatch','version-mismatch','protocol-mismatch','source-mismatch','duplicate-query','query-failure')) {
            New-Fixture
            $script:mode=$failure
            if ($failure -eq 'hash-mismatch') { $script:scripts.prepare=$script:scripts.prepare.Replace($digest,('c'*64)) }
            Expect-Failure { Invoke-FixtureScript prepare }
            Invoke-FixtureScript rollback
            Assert ($stops -eq 0) 'Verification failure stopped a helper.'
            Assert-Prior; Assert-Clean
        }
    }
    Case 'busy helper refuses activation and retains prior files' {
        New-Fixture; Invoke-FixtureScript prepare; $script:mode='busy'
        Expect-Failure { Invoke-FixtureScript activate }
        Assert-Prior; Assert-Clean
    }
    Case 'partial sidecar publication restores binary and both prior sidecars' {
        New-Fixture; Invoke-FixtureScript prepare; Invoke-FixtureScript activate
        $script:mode='publication-failure'; Expect-Failure { Invoke-FixtureScript publish }
        $partial=Get-Content -LiteralPath (Join-Path $directory 'uam-runner.manifest.json') -Raw | ConvertFrom-Json
        Assert ($partial.sourceFingerprint -ceq $expectedFingerprint) 'First sidecar was not published before injected failure.'
        Invoke-FixtureScript rollback; Assert-Prior; Assert-Clean
    }
    Case 'installed identity mismatch refuses publication and rolls back' {
        New-Fixture; Invoke-FixtureScript prepare; Invoke-FixtureScript activate
        $script:mode='installed-source-mismatch'; Expect-Failure { Invoke-FixtureScript publish }
        Equal-Bytes (Join-Path $directory 'uam-runner.manifest.json') $oldManifest
        Invoke-FixtureScript rollback; Assert-Prior; Assert-Clean
    }
    Case 'candidate hash is rechecked between preparation and activation' {
        New-Fixture; Invoke-FixtureScript prepare
        [IO.File]::WriteAllText($candidate,'changed candidate')
        Expect-Failure { Invoke-FixtureScript activate }
        Assert ($stops -eq 0) 'Changed candidate stopped a helper.'
        Assert-Prior; Assert-Clean
    }
    Case 'installed hash is rechecked before sidecar publication' {
        New-Fixture; Invoke-FixtureScript prepare; Invoke-FixtureScript activate
        [IO.File]::WriteAllText($installed,'changed installed bytes')
        Expect-Failure { Invoke-FixtureScript publish }
        Equal-Bytes (Join-Path $directory 'uam-runner.manifest.json') $oldManifest
        Invoke-FixtureScript rollback; Assert-Prior; Assert-Clean
    }
    Case 'altered staged metadata cannot be published for a verified binary' {
        foreach ($field in @('source','protocol','protocol-type','source-type','checksum')) {
            New-Fixture; Invoke-FixtureScript prepare; Invoke-FixtureScript activate
            if ($field -eq 'checksum') { [IO.File]::WriteAllText((Join-Path $metadata 'uam-runner.sha256.new'),('c'*64)+"`n") }
            else {
                $value=@{sourceFingerprint=$expectedFingerprint;protocolVersion=[int]$templates.protocol}
                switch ($field) {
                    source { $value.sourceFingerprint='c'*64 }
                    protocol { $value.protocolVersion=0 }
                    protocol-type { $value.protocolVersion=[string]$templates.protocol }
                    source-type { $value.sourceFingerprint=@($expectedFingerprint,$expectedFingerprint) }
                }
                [IO.File]::WriteAllText((Join-Path $metadata 'uam-runner.manifest.json.new'),($value | ConvertTo-Json -Compress))
            }
            Expect-Failure { Invoke-FixtureScript publish }
            Equal-Bytes (Join-Path $directory 'uam-runner.manifest.json') $oldManifest
            Invoke-FixtureScript rollback; Assert-Prior; Assert-Clean
        }
    }
    Case 'missing snapshot readiness refuses activation or publication' {
        foreach ($phase in @('activate','publish')) {
            New-Fixture; Invoke-FixtureScript prepare
            if ($phase -eq 'publish') { Invoke-FixtureScript activate }
            Remove-Item -LiteralPath (Join-Path $metadata 'ready')
            Expect-Failure { Invoke-FixtureScript $phase }
            if ($phase -eq 'activate') { Assert ($stops -eq 0) 'Incomplete snapshot stopped a helper.' }
            Invoke-FixtureScript rollback; Assert-Prior; Assert-Clean
        }
    }
    Case 'replacement and start failures use the same transactional rollback' {
        foreach ($failure in @('replacement-failure','start-failure')) {
            New-Fixture; Invoke-FixtureScript prepare
            $script:mode=$failure; Expect-Failure { Invoke-FixtureScript activate }
            Assert-Prior; Assert-Clean
        }
    }
    Case 'lost acknowledgements after every mutation phase remain reversible' {
        foreach ($lostAfter in @('prepare','activate','publish')) {
            New-Fixture
            foreach ($phase in @('prepare','activate','publish')) { Invoke-FixtureScript $phase; if ($phase -eq $lostAfter) { break } }
            # Equivalent to the controller observing a failed transport result after mutation.
            Invoke-FixtureScript rollback; Assert-Prior; Assert-Clean
        }
    }
    Case 'interrupted journal recovery restores files before restarting prior helper' {
        foreach ($first in @($false,$true)) { foreach ($manifest in @($false,$true)) { foreach ($checksum in @($false,$true)) {
            New-Fixture $first $manifest $checksum
            Invoke-FixtureScript prepare; Invoke-FixtureScript activate; Invoke-FixtureScript publish
            $script:mode='recover'; Invoke-FixtureScript recover
            Assert-Prior $first $manifest $checksum; Assert-Clean
            Assert ($starts -eq $(if ($first) { 1 } else { 2 })) 'Recovery restarted the wrong prior binary state.'
            Assert (-not (Test-Path -LiteralPath (Join-Path $FixtureHome 'r/install.transaction'))) 'Journal survived successful recovery.'
        } } }
    }
    Case 'busy rollback and recovery retain all recovery authority for retry' {
        New-Fixture; Invoke-FixtureScript prepare; Invoke-FixtureScript activate; Invoke-FixtureScript publish
        $script:mode='rollback-busy'; Expect-Failure { Invoke-FixtureScript rollback }; Expect-Failure { Invoke-FixtureScript recover }
        Assert-Published
        foreach ($path in @($marker,$backup,$metadata)) { Assert (Test-Path -LiteralPath $path) 'Busy refusal lost its recovery receipt.' }
        $script:mode='normal'; Invoke-FixtureScript recover; Assert-Prior; Assert-Clean
    }
    Case 'failed metadata restoration retains snapshot and supports retry' {
        New-Fixture; Invoke-FixtureScript prepare; Invoke-FixtureScript activate; Invoke-FixtureScript publish
        $script:mode='restore-failure'; Expect-Failure { Invoke-FixtureScript rollback }
        Assert (Test-Path -LiteralPath $marker) 'Failed restore lost activation marker.'
        Equal-Bytes (Join-Path $metadata 'uam-runner.manifest.json') $oldManifest
        $script:mode='normal'; Invoke-FixtureScript recover; Assert-Prior; Assert-Clean
    }
    Case 'committed marker survives lost cleanup acknowledgement without rollback' {
        New-Fixture; Invoke-FixtureScript prepare; Invoke-FixtureScript activate; Invoke-FixtureScript publish
        $script:mode='commit-cleanup-failure'; Expect-Failure { Invoke-FixtureScript commit }
        Assert (-not (Test-Path -LiteralPath $marker)) 'Commit did not remove marker first.'
        $script:mode='normal'; $priorStops=$stops; Invoke-FixtureScript recover
        Assert ($stops -eq $priorStops) 'Recovery stopped a committed helper.'
        Assert-Published; Assert-Clean
    }
    Case 'legacy binary-only receipt does not invent prior metadata' {
        New-Fixture
        [IO.File]::WriteAllBytes($backup,$oldBinary); [IO.File]::WriteAllBytes($installed,$newBinary)
        [IO.File]::WriteAllText($marker,'')
        [IO.File]::WriteAllLines((Join-Path $FixtureHome 'r/install.transaction'),@('4.5.7','metadata',$templates.protocol))
        Invoke-FixtureScript recover; Assert-Prior; Assert-Clean
    }
    Case 'guard finally restores abandoned transaction under its real filesystem lock' {
        New-Fixture; Invoke-FixtureScript prepare; Invoke-FixtureScript activate; Invoke-FixtureScript publish
        # Suppress acquisition recovery to exercise only the guard's finally path,
        # with the same armed transaction that normal commands leave after readiness.
        Remove-Item -LiteralPath (Join-Path $FixtureHome 'r/install.transaction')
        Invoke-FixtureScript authorize; Invoke-FixtureScript guard; Assert-Prior; Assert-Clean
    }
    Case 'acquisition loss leaves prior transaction untouched before durable fence' {
        New-Fixture; Invoke-FixtureScript prepare; Invoke-FixtureScript activate; Invoke-FixtureScript publish
        $priorStops=$stops; Invoke-FixtureScript guard
        Assert-Published
        Assert ($stops -eq $priorStops) 'Acquisition recovered without durable authorisation.'
        foreach ($path in @($marker,$backup,$metadata,(Join-Path $FixtureHome 'r/install.transaction'))) { Assert (Test-Path -LiteralPath $path) 'Pre-fence loss changed recovery authority.' }
        # Guard invalidates its nonce; a successor reacquires before reconciliation.
        [IO.File]::WriteAllText((Join-Path $FixtureHome 'r/install.lock'),'metadata')
        Invoke-FixtureScript recover; Assert-Prior; Assert-Clean
    }
    Case 'sidecar link is refused without changing its target' {
        New-Fixture
        $target=Join-Path $FixtureHome 'untouched'
        [IO.File]::WriteAllBytes($target,$oldManifest)
        Remove-Item -LiteralPath (Join-Path $directory 'uam-runner.manifest.json')
        New-Item -ItemType SymbolicLink -Path (Join-Path $directory 'uam-runner.manifest.json') -Target $target | Out-Null
        Expect-Failure { Invoke-FixtureScript prepare }
        Equal-Bytes $target $oldManifest; Equal-Bytes $installed $oldBinary
        Assert ($stops -eq 0) 'Unsafe sidecar stopped a helper.'
        Invoke-FixtureScript rollback; Assert-Clean
    }
    Case 'old nonce and competing operation refuse publication before mutation' {
        New-Fixture; Invoke-FixtureScript prepare; Invoke-FixtureScript activate
        $leasePath=Join-Path $FixtureHome 'r/install.lock'
        [IO.File]::WriteAllText($leasePath,'different-owner')
        Expect-Failure { Invoke-FixtureScript publish }
        Equal-Bytes (Join-Path $directory 'uam-runner.manifest.json') $oldManifest
        [IO.File]::WriteAllText($leasePath,'metadata')
        $held=[IO.File]::Open((Join-Path $FixtureHome 'r/install.operation'),'OpenOrCreate','ReadWrite','None')
        try { Expect-Failure { Invoke-FixtureScript publish } } finally { $held.Dispose() }
        Equal-Bytes (Join-Path $directory 'uam-runner.manifest.json') $oldManifest
        Invoke-FixtureScript publish; Invoke-FixtureScript rollback; Assert-Prior; Assert-Clean
    }
    Case 'partial journal creation never publishes an invalid recovery receipt' {
        New-Fixture; Invoke-FixtureScript prepare
        $saved=$script:scripts.activate
        $script:scripts.activate=$saved.Replace("[IO.File]::WriteAllLines((`$backup+'.receipt'),", "[IO.File]::WriteAllText((`$backup+'.receipt'),'partial');throw 'Injected interrupted journal write';[IO.File]::WriteAllLines((`$backup+'.receipt'),")
        Assert ($script:scripts.activate -cne $saved) 'Journal fault control did not match production.'
        Expect-Failure { Invoke-FixtureScript activate }
        Assert (-not (Test-Path -LiteralPath (Join-Path $FixtureHome 'r/install.transaction'))) 'Partial journal became authoritative.'
        Invoke-FixtureScript recover; Assert-Prior; Assert-Clean
        [IO.File]::WriteAllBytes($candidate,$newBinary)
        $script:scripts.activate=$saved
        Invoke-FixtureScript prepare; Invoke-FixtureScript activate; Invoke-FixtureScript publish; Invoke-FixtureScript commit
        Assert-Published; Assert-Clean
    }
    Case 'missing activation marker cannot manufacture a commit acknowledgement' {
        New-Fixture
        Expect-Failure { Invoke-FixtureScript commit }
        Assert-Prior; Assert-Clean
    }
    Case 'expired guard waits for publication then fences late old-owner commands' {
        New-Fixture
        $script:expiry=[Threading.ManualResetEventSlim]::new($false)
        $script:operationWaiting=[Threading.ManualResetEventSlim]::new($false)
        $ready=[Threading.ManualResetEventSlim]::new($false)
        $runspace=[RunspaceFactory]::CreateRunspace(); $runspace.Open()
        foreach ($pair in @(@('FixtureHome',$FixtureHome),@('expiry',$script:expiry),@('operationWaiting',$script:operationWaiting),@('ready',$ready))) {
            $runspace.SessionStateProxy.SetVariable($pair[0],$pair[1])
        }
        $guard=$script:scripts.guard
        $tokens=$null; $errors=$null
        $ast=[Management.Automation.Language.Parser]::ParseInput($guard,[ref]$tokens,[ref]$errors)
        $definitions=@($ast.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst]},$false))
        $functions=($definitions | ForEach-Object {$_.Extent.Text}) -join ';'
        foreach ($definition in ($definitions | Sort-Object {$_.Extent.StartOffset} -Descending)) {
            $guard=$guard.Remove($definition.Extent.StartOffset,$definition.Extent.EndOffset-$definition.Extent.StartOffset)
        }
        $guard=$guard.Replace("[Console]::WriteLine('UAM_INSTALL_LOCK_READY:metadata')",'$ready.Set()')
        $control=@'
function Invoke-UamRunner { } # no real executable is ever invoked
function Start-Sleep {
    if ($wait) { $operationWaiting.Set(); [Threading.Thread]::Sleep(10) }
    else { if (-not $expiry.Wait(5000)) { throw 'Publication did not reach expiry control.' }; Set-Variable -Name deadline -Scope 1 -Value ([DateTime]::MinValue) }
}
'@
        $worker=[PowerShell]::Create(); $worker.Runspace=$runspace
        $null=$worker.AddScript($functions+';'+$control+';'+$guard)
        $pending=$worker.BeginInvoke()
        try {
            Assert ($ready.Wait(5000)) 'Guard did not acquire its real file lease.'
            Invoke-FixtureScript authorize; Invoke-FixtureScript prepare; Invoke-FixtureScript activate
            $script:mode='expire-publication'; Invoke-FixtureScript publish
            Assert ($pending.AsyncWaitHandle.WaitOne(5000)) 'Expired guard did not settle after operation completion.'
            $null=$worker.EndInvoke($pending)
            Assert (-not $worker.HadErrors) "Guard control failed: $($worker.Streams.Error)"
            Assert-Prior; Assert-Clean
            Expect-Failure { Invoke-FixtureScript prepare }
        } finally {
            $script:expiry.Set(); $worker.Dispose(); $runspace.Dispose()
            $script:expiry.Dispose(); $script:operationWaiting.Dispose(); $ready.Dispose()
        }
    }
    Write-Output "Windows metadata filesystem/injected-command cases: $script:passed PASS"
    Write-Output 'Windows loaded-image and Windows PowerShell 5/OpenSSH acceptance remain pending; injected commands do not establish resident identity.'
} finally {
    if (Test-Path -LiteralPath $fixtureRoot) { Remove-Item -LiteralPath $fixtureRoot -Recurse -Force }
}
