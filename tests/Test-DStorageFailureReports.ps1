param([Parameter(Mandatory=$true)][string]$BuildDir, [string]$ExpectedVersion='0.5.3')
$ErrorActionPreference='Stop'
$BuildDir=(Resolve-Path -LiteralPath $BuildDir).Path
$report=Join-Path $BuildDir 'reframework/data/DStorageFileLifetimeGuard'
Push-Location $BuildDir
try {
    & (Join-Path $BuildDir 'DStorageFailurePolicyTest.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Failure policy tests failed' }
    & (Join-Path $BuildDir 'DStorageResourceRecoveryNegativeHost.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Negative host failed' }
    $text=Get-Content -LiteralPath (Join-Path $report 'failures.log') -Raw
    foreach ($pattern in @("failure_report build=$ExpectedVersion",'protection_health','resource=Failed','component=resource stage=locate_ready','target_module=','pe_timestamp=','mask_bits=','detail_limit_per_category=16')) {
        if ($text -notmatch [regex]::Escape($pattern)) { throw "Missing fault detail: $pattern" }
    }
    Copy-Item -LiteralPath (Join-Path $report 'failures.log') -Destination (Join-Path $BuildDir 'acceptance_protection_failure.log')
    1..10 | ForEach-Object {
        & (Join-Path $BuildDir 'DStorageResourceRecoveryNegativeHost.exe') | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'History fixture failed' }
    }
    foreach ($i in 0..8) {
        $suffix=if ($i -eq 0) { '' } else { ".$i" }
        $event=Get-Content -LiteralPath (Join-Path $report "events$suffix.log") -Raw
        $failure=Get-Content -LiteralPath (Join-Path $report "failures$suffix.log") -Raw
        $e=[regex]::Match($event,'startup version=9 pid=(\d+) utc_filetime=(\d+)')
        $f=[regex]::Match($failure,('failure_report build='+[regex]::Escape($ExpectedVersion)+' pid=(\d+) utc_filetime=(\d+)'))
        if (-not $e.Success -or -not $f.Success -or $e.Groups[1].Value -ne $f.Groups[1].Value -or $e.Groups[2].Value -ne $f.Groups[2].Value) { throw "Session correlation mismatch: $i" }
    }
    # Seed the filesystem state of a session with no fault file. History slots
    # must not slide forward and accidentally pair different process sessions.
    Remove-Item -LiteralPath (Join-Path $report 'failures.log')
    & (Join-Path $BuildDir 'DStorageResourceRecoveryNegativeHost.exe') | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Gap history fixture failed' }
    if (Test-Path -LiteralPath (Join-Path $report 'failures.1.log')) { throw 'Healthy-session gap was incorrectly filled' }
    if (-not (Test-Path -LiteralPath (Join-Path $report 'events.1.log'))) { throw 'Missing event history for gap' }
    $event=Get-Content -LiteralPath (Join-Path $report 'events.2.log') -Raw
    $failure=Get-Content -LiteralPath (Join-Path $report 'failures.2.log') -Raw
    $e=[regex]::Match($event,'startup version=9 pid=(\d+) utc_filetime=(\d+)')
    $f=[regex]::Match($failure,('failure_report build='+[regex]::Escape($ExpectedVersion)+' pid=(\d+) utc_filetime=(\d+)'))
    if (-not $e.Success -or -not $f.Success -or $e.Groups[1].Value -ne $f.Groups[1].Value -or $e.Groups[2].Value -ne $f.Groups[2].Value) { throw 'Gap shifted history correlation' }
    Write-Output 'failure_report=PASS failed_layer_detail=1 correlated_history=9 healthy_gap_preserved=1 native_state_unchanged=1'
} finally { Pop-Location }
