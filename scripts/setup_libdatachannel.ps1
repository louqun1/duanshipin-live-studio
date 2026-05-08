[CmdletBinding()]
param(
    [string]$Version = "v0.24.2",
    [string]$SourceDir = "",
    [string]$BuildDir = "",
    [string]$InstallDir = "",
    [string]$Generator = "MinGW Makefiles",
    [string]$BuildType = "Release",
    [string]$OpenSslRootDir = "",
    [string]$CCompiler = "",
    [string]$CxxCompiler = "",
    [string]$GitHubUrlPrefix = "",
    [switch]$Offline,
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

function Invoke-CheckedCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Command,
        [string]$WorkingDirectory = ""
    )

    if ($WorkingDirectory) {
        Push-Location $WorkingDirectory
    }

    try {
        if ($Command.Length -gt 1) {
            & $Command[0] $Command[1..($Command.Length - 1)]
        } else {
            & $Command[0]
        }
        if ($LASTEXITCODE -ne 0) {
            throw "Command failed with exit code $($LASTEXITCODE): $($Command -join ' ')"
        }
    } finally {
        if ($WorkingDirectory) {
            Pop-Location
        }
    }
}

function Get-GitCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments
    )

    $command = @("git")
    if ($script:GitHubUrlPrefix) {
        $command += @(
            "-c",
            "url.$($script:GitHubUrlPrefix).insteadOf=https://github.com/"
        )
    }

    $command += $Arguments
    return $command
}

function Test-GitCommitishExists {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoDir,
        [Parameter(Mandatory = $true)]
        [string]$Commitish
    )

    & git -C $RepoDir rev-parse -q --verify "$Commitish`^{commit}" *> $null
    return ($LASTEXITCODE -eq 0)
}

function Resolve-OpenSslRootDir {
    param([string]$ExplicitPath)

    if ($ExplicitPath) {
        return $ExplicitPath
    }

    if ($env:OPENSSL_ROOT_DIR) {
        return $env:OPENSSL_ROOT_DIR
    }

    $candidates = @(
        "C:\Program Files\OpenSSL-Win64",
        "C:\Program Files (x86)\OpenSSL-Win32",
        "D:\itffmpeg\OpenSSL-Win64",
        "D:\itffmpeg\OpenSSL"
    )

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    return ""
}

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $SourceDir) {
    $SourceDir = Join-Path $repoRoot "third_party/libdatachannel-src"
}
if (-not $BuildDir) {
    $BuildDir = Join-Path $repoRoot "third_party/libdatachannel-build-clang"
}
if (-not $InstallDir) {
    $InstallDir = Join-Path $repoRoot "third_party/libdatachannel-install-clang"
}

$resolvedOpenSslRootDir = Resolve-OpenSslRootDir -ExplicitPath $OpenSslRootDir
if (-not $resolvedOpenSslRootDir) {
    throw "OpenSSL root was not found. Pass -OpenSslRootDir or set OPENSSL_ROOT_DIR before running this script."
}

if (-not (Test-Path $resolvedOpenSslRootDir)) {
    throw "OpenSSL root does not exist: $resolvedOpenSslRootDir"
}

if ($Clean) {
    foreach ($path in @($BuildDir, $InstallDir)) {
        if (Test-Path $path) {
            Remove-Item -Recurse -Force -LiteralPath $path
        }
    }
}

$sourceParent = Split-Path -Parent $SourceDir
if (-not (Test-Path $sourceParent)) {
    New-Item -ItemType Directory -Path $sourceParent | Out-Null
}

$repoUrl = "https://github.com/paullouisageneau/libdatachannel.git"
if (-not (Test-Path $SourceDir)) {
    Invoke-CheckedCommand -Command (Get-GitCommand @(
        "clone",
        "--branch", $Version,
        "--depth", "1",
        $repoUrl,
        $SourceDir
    ))
} else {
    $localVersionExists = Test-GitCommitishExists -RepoDir $SourceDir -Commitish $Version

    if (-not $Offline) {
        try {
            Invoke-CheckedCommand -Command (Get-GitCommand @("-C", $SourceDir, "fetch", "--tags", "origin"))
        } catch {
            if ($localVersionExists) {
                Write-Warning "git fetch failed, but local version '$Version' already exists. Continuing in offline-reuse mode."
            } else {
                throw
            }
        }
    } elseif (-not $localVersionExists) {
        throw "Offline mode was requested, but version '$Version' does not exist locally under $SourceDir."
    }

    if (-not (Test-GitCommitishExists -RepoDir $SourceDir -Commitish $Version)) {
        throw "Requested version '$Version' is not available locally in $SourceDir."
    }

    Invoke-CheckedCommand -Command (Get-GitCommand @("-C", $SourceDir, "checkout", $Version))
}

try {
    Invoke-CheckedCommand -Command (Get-GitCommand @(
        "-C", $SourceDir, "submodule", "update", "--init", "--recursive", "--depth", "1"
    ))
} catch {
    if ($Offline) {
        throw "libdatachannel submodule sources are not complete locally. Offline mode can reuse an existing checkout, but it cannot download missing deps. Original error: $($_.Exception.Message)"
    }

    throw "libdatachannel submodule download failed. This step requires network access to the GitHub submodule repositories. If direct GitHub access is blocked, retry with -Offline only after deps are already present, or pass -GitHubUrlPrefix with an accessible GitHub proxy/mirror prefix. Original error: $($_.Exception.Message)"
}

$cmakeConfigureArgs = @(
    "-S", $SourceDir,
    "-B", $BuildDir,
    "-G", $Generator,
    "-DCMAKE_BUILD_TYPE=$BuildType",
    "-DCMAKE_INSTALL_PREFIX=$InstallDir",
    "-DBUILD_SHARED_LIBS=ON",
    "-DNO_EXAMPLES=ON",
    "-DNO_TESTS=ON",
    "-DLIBSRTP_TEST_APPS=OFF",
    "-DENABLE_WARNINGS_AS_ERRORS=OFF",
    "-DUSE_GNUTLS=OFF",
    "-DUSE_MBEDTLS=OFF",
    "-DOPENSSL_ROOT_DIR=$resolvedOpenSslRootDir"
)

if ($Generator -eq "MinGW Makefiles") {
    $cmakeConfigureArgs += "-DCMAKE_SH=CMAKE_SH-NOTFOUND"
}
if ($CCompiler) {
    $cmakeConfigureArgs += "-DCMAKE_C_COMPILER=$CCompiler"
}
if ($CxxCompiler) {
    $cmakeConfigureArgs += "-DCMAKE_CXX_COMPILER=$CxxCompiler"
}

$cmakeConfigureCommand = @("cmake") + $cmakeConfigureArgs
Invoke-CheckedCommand -Command $cmakeConfigureCommand
Invoke-CheckedCommand -Command @(
    "cmake", "--build", $BuildDir, "--config", $BuildType, "--target", "install", "--parallel"
)

Write-Host ""
Write-Host "libdatachannel is installed into: $InstallDir"
Write-Host "Use scripts/configure_rtc_probes.ps1 to configure the rtc_probe targets."
