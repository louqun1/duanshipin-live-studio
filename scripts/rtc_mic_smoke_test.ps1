param(
    [string]$GuestJoinParamsFile = ".\\guest-join.json",
    [string]$ControllerJoinParamsFile = ".\\host-join.json",
    [string]$IceServer = "turn://probe:probe_turn_123@192.168.3.28:3478?transport=udp",
    [int]$MediaRunMs = 5000,
    [int]$TimeoutMs = 60000,
    [switch]$RelayOnly = $true
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $scriptDir
$guestExe = Join-Path $repoRoot "build-rtc\\apps\\rtc_guest_probe\\rtc_guest_probe.exe"
$controllerExe = Join-Path $repoRoot "build-rtc\\apps\\rtc_probe\\rtc_probe.exe"

if (-not (Test-Path $guestExe)) {
    throw "guest probe not found: $guestExe"
}
if (-not (Test-Path $controllerExe)) {
    throw "controller probe not found: $controllerExe"
}
if (-not (Test-Path (Join-Path $repoRoot $GuestJoinParamsFile))) {
    throw "guest join params file not found: $GuestJoinParamsFile"
}
if (-not (Test-Path (Join-Path $repoRoot $ControllerJoinParamsFile))) {
    throw "controller join params file not found: $ControllerJoinParamsFile"
}

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$logDir = Join-Path $repoRoot ("logs\\rtc-mic-smoke-" + $stamp)
New-Item -ItemType Directory -Path $logDir | Out-Null

$guestOut = Join-Path $logDir "guest.out.log"
$guestErr = Join-Path $logDir "guest.err.log"
$controllerOut = Join-Path $logDir "controller.out.log"
$controllerErr = Join-Path $logDir "controller.err.log"

$guestArgs = @(
    "--rtc-join-params-file", $GuestJoinParamsFile,
    "--expect-remote-audio",
    "--ice-server", $IceServer,
    "--timeout-ms", $TimeoutMs
)

$controllerArgs = @(
    "--rtc-join-params-file", $ControllerJoinParamsFile,
    "--send-mic",
    "--ice-server", $IceServer,
    "--media-run-ms", $MediaRunMs,
    "--timeout-ms", $TimeoutMs
)

if ($RelayOnly) {
    $guestArgs += "--relay-only"
    $controllerArgs += "--relay-only"
}

$guestProcess = $null
$controllerProcess = $null

try {
    $guestProcess = Start-Process -FilePath $guestExe `
        -ArgumentList $guestArgs `
        -WorkingDirectory $repoRoot `
        -RedirectStandardOutput $guestOut `
        -RedirectStandardError $guestErr `
        -WindowStyle Hidden `
        -PassThru

    Start-Sleep -Seconds 2

    $controllerProcess = Start-Process -FilePath $controllerExe `
        -ArgumentList $controllerArgs `
        -WorkingDirectory $repoRoot `
        -RedirectStandardOutput $controllerOut `
        -RedirectStandardError $controllerErr `
        -WindowStyle Hidden `
        -PassThru

    $waitBudgetMs = [Math]::Max($TimeoutMs + 15000, 30000)
    $controllerExited = $controllerProcess.WaitForExit($waitBudgetMs)
    $guestExited = $guestProcess.WaitForExit($waitBudgetMs)

    if (-not $controllerExited) {
        try { $controllerProcess.Kill() } catch {}
        throw "controller probe timed out after $waitBudgetMs ms"
    }
    if (-not $guestExited) {
        try { $guestProcess.Kill() } catch {}
        throw "guest probe timed out after $waitBudgetMs ms"
    }

    $controllerErrorBytes = if (Test-Path $controllerErr) { (Get-Item $controllerErr).Length } else { 0 }
    $guestErrorBytes = if (Test-Path $guestErr) { (Get-Item $guestErr).Length } else { 0 }
    $controllerFailed = $controllerErrorBytes -gt 0
    $guestFailed = $guestErrorBytes -gt 0

    Write-Host "RTC mic smoke test finished."
    Write-Host "  logs: $logDir"
    Write-Host "  controller error bytes: $controllerErrorBytes"
    Write-Host "  guest error bytes: $guestErrorBytes"

    if ($controllerFailed -or $guestFailed) {
        Write-Host ""
        Write-Host "controller tail:"
        Get-Content $controllerOut -Tail 40
        Write-Host ""
        Write-Host "guest tail:"
        Get-Content $guestOut -Tail 40
        exit 1
    }
} finally {
    foreach ($process in @($controllerProcess, $guestProcess)) {
        if ($null -ne $process) {
            try {
                if (-not $process.HasExited) {
                    $process.Kill()
                }
            } catch {}
        }
    }
}
