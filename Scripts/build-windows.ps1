param(
    [switch]$NoRun,
    [switch]$NoTest,
    [switch]$Editor,
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug"
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$buildDir = Join-Path $repoRoot "build\windows"

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw "CMake 3.24+ was not found. Install CMake and Visual Studio 2022 with the C++ desktop workload, then rerun this script."
}
$cmakeVersion = (cmake --version | Select-Object -First 1)
if ($LASTEXITCODE -ne 0 -or $cmakeVersion -notmatch 'version\s+(\d+)\.(\d+)') {
    throw "Could not determine the installed CMake version."
}
if ([int]$Matches[1] -lt 3 -or ([int]$Matches[1] -eq 3 -and [int]$Matches[2] -lt 24)) {
    throw "CMake 3.24+ is required; found $cmakeVersion."
}

cmake -S $repoRoot -B $buildDir -G "Visual Studio 17 2022" -A x64
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed with exit code $LASTEXITCODE." }

cmake --build $buildDir --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE." }

if (-not $NoTest) {
    ctest --test-dir $buildDir -C $Configuration --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "CTest failed with exit code $LASTEXITCODE." }
}

if (-not $NoRun) {
    $appName = if ($Editor) { "TogetherEngineEditor.exe" } else { "TogetherWeDo.exe" }
    $executable = Join-Path $buildDir "$Configuration\$appName"
    if (-not (Test-Path -LiteralPath $executable)) { throw "Expected executable was not produced: $executable" }
    Push-Location (Split-Path $executable)
    try { & $executable } finally { Pop-Location }
}
