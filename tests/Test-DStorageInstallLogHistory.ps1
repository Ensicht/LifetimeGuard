param(
    [Parameter(Mandatory = $true)]
    [string]$BuildDir
)

$ErrorActionPreference = "Stop"
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
$reportDir = Join-Path $BuildDir "reframework\data\DStorageFileLifetimeGuard"

function Invoke-CheckedHarness {
    param([Parameter(Mandatory = $true)][string]$Name)
    & (Join-Path $BuildDir $Name)
    if ($LASTEXITCODE -ne 0) {
        throw "$Name failed with exit code $LASTEXITCODE"
    }
}

function Read-CurrentReport {
    $path = Join-Path $reportDir "events.log"
    if (-not (Test-Path -LiteralPath $path)) {
        throw "missing current report: $path"
    }
    return Get-Content -LiteralPath $path -Raw
}

if (Test-Path -LiteralPath $reportDir) {
    $resolvedReport = [IO.Path]::GetFullPath($reportDir)
    $allowedRoot = [IO.Path]::GetFullPath($BuildDir).TrimEnd('\') + '\'
    if (-not $resolvedReport.StartsWith($allowedRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe report cleanup path.' }
    Remove-Item -LiteralPath $reportDir -Recurse -Force
}

Push-Location $BuildDir
try {
    Invoke-CheckedHarness "DStorageResourceRecoveryHookHarness.exe"
    $successLog = Read-CurrentReport
    $requiredSuccessPatterns = @(
        'hook_install .*component=resource stage=locate_ready .*status_name=found',
        'hook_install .*component=resource stage=mh_create .*status=0 status_name=MH_OK',
        'hook_install .*component=resource stage=mh_enable .*status=0 status_name=MH_OK',
        'target_module="[^"]*DStorageResourceRecoveryHookHarness\.exe" bytes=[0-9A-F]{32}',
        'redirect_final_module="[^"]*DStorageFileLifetimeGuard\.dll"',
        'win32_error=0 win32_message="ERROR_SUCCESS"'
    )
    foreach ($pattern in $requiredSuccessPatterns) {
        if ($successLog -notmatch $pattern) {
            throw "successful install report is missing pattern: $pattern"
        }
    }
    Copy-Item -LiteralPath (Join-Path $reportDir "events.log") `
        -Destination (Join-Path $BuildDir "acceptance_install_success.log") -Force

    Invoke-CheckedHarness "DStorageResourceRecoveryNegativeHost.exe"
    $failureLog = Read-CurrentReport
    $requiredFailurePatterns = @(
        'hook_install .*component=resource stage=locate_ready .*status_name=missing',
        'hook_install .*component=resource stage=locate_finalizer .*status_name=missing',
        'hook_install .*component=resource stage=locate_texture_vtable .*status_name=missing'
    )
    foreach ($pattern in $requiredFailurePatterns) {
        if ($failureLog -notmatch $pattern) {
            throw "failed install report is missing pattern: $pattern"
        }
    }
    Copy-Item -LiteralPath (Join-Path $reportDir "events.log") `
        -Destination (Join-Path $BuildDir "acceptance_install_failure.log") -Force

    1..10 | ForEach-Object {
        Invoke-CheckedHarness "DStorageLifecycleIntegrationHarness.exe"
    }

    $historyNames = @("events.log") +
        (1..8 | ForEach-Object { "events.$_.log" })
    $sessionTimes = @()
    foreach ($name in $historyNames) {
        $path = Join-Path $reportDir $name
        if (-not (Test-Path -LiteralPath $path)) {
            throw "missing rotated report: $path"
        }
        $content = Get-Content -LiteralPath $path -Raw
        if ($content -notmatch 'startup version=9 ') {
            throw "rotated report has no version 9 startup record: $path"
        }
        if ($content -notmatch 'report_thread_state=3 report_rotation_failures=0 report_rotation_last_error=0') {
            throw "report rotation recorded a failure: $path"
        }
        if ($content -notmatch 'utc_filetime=([0-9]+)') {
            throw "rotated report has no session timestamp: $path"
        }
        $sessionTimes += $Matches[1]
    }
    if (($sessionTimes | Sort-Object -Unique).Count -ne 9) {
        throw "rotated reports do not contain nine distinct sessions"
    }
    if (Test-Path -LiteralPath (Join-Path $reportDir "events.9.log")) {
        throw "history exceeded the fixed eight-session limit"
    }

    Write-Host "install_log_success_fields=PASS"
    Write-Host "install_log_failure_fields=PASS"
    Write-Host "install_log_history_current_plus_8=PASS"
}
finally {
    Pop-Location
}
