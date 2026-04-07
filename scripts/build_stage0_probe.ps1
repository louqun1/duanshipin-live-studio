$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$outDir = Join-Path $repoRoot "build-manual"
$sourceFile = Join-Path $repoRoot "apps\stage0_probe\main.cpp"
$exeFile = Join-Path $outDir "stage0_probe.exe"
$includeDir = Join-Path $repoRoot "src"

New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$compiler = (Get-Command g++ -ErrorAction Stop).Source

& $compiler -std=c++17 -I $includeDir $sourceFile -o $exeFile

if ($LASTEXITCODE -ne 0) {
    throw "g++ build failed with exit code $LASTEXITCODE"
}

& $exeFile
