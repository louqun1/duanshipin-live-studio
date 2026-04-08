$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$outDir = Join-Path $repoRoot "build-manual"
$sourceFile = Join-Path $repoRoot "apps\stage1_video_trace\main.cpp"
$exeFile = Join-Path $outDir "stage1_video_trace.exe"
$includeDir = Join-Path $repoRoot "src"
$spdlogIncludeDir = Join-Path $repoRoot "third_party\spdlog\include"

New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$compiler = (Get-Command g++ -ErrorAction Stop).Source

& $compiler -std=c++17 -I $includeDir -I $spdlogIncludeDir $sourceFile -o $exeFile

if ($LASTEXITCODE -ne 0) {
    throw "g++ build failed with exit code $LASTEXITCODE"
}

& $exeFile
