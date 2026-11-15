param(
    [switch]$NoRun,
    [switch]$NoTest,
    [switch]$Editor,
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",
    [ValidateSet("Visual Studio 18 2026", "Visual Studio 17 2022")]
    [string]$Generator = "Visual Studio 18 2026"
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$buildDir = Join-Path $repoRoot "build\windows"

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw "CMake 3.24+ was not found. Install CMake and a supported Visual Studio C++ desktop workload, then rerun this script."
}
$cmakeCommand = (Get-Command cmake -ErrorAction Stop).Source
$cmakeVersionOutput = & $cmakeCommand --version
$cmakeVersionExitCode = $LASTEXITCODE
$cmakeVersion = $cmakeVersionOutput | Select-Object -First 1
if ($cmakeVersionExitCode -ne 0 -or $cmakeVersion -notmatch 'version\s+(\d+)\.(\d+)') {
    throw "Could not determine the installed CMake version."
}
if ([int]$Matches[1] -lt 3 -or ([int]$Matches[1] -eq 3 -and [int]$Matches[2] -lt 24)) {
    throw "CMake 3.24+ is required; found $cmakeVersion."
}

$cachePath = Join-Path $buildDir "CMakeCache.txt"
$configureArgs = @("-S", $repoRoot, "-B", $buildDir, "-G", $Generator, "-A", "x64")
if (Test-Path -LiteralPath $cachePath) {
    $cachedGeneratorLine = Select-String -LiteralPath $cachePath -Pattern '^CMAKE_GENERATOR:INTERNAL=' | Select-Object -First 1
    $cachedPlatformLine = Select-String -LiteralPath $cachePath -Pattern '^CMAKE_GENERATOR_PLATFORM:INTERNAL=' | Select-Object -First 1
    $cachedGenerator = if ($cachedGeneratorLine) { $cachedGeneratorLine.Line.Substring('CMAKE_GENERATOR:INTERNAL='.Length) } else { '' }
    $cachedPlatform = if ($cachedPlatformLine) { $cachedPlatformLine.Line.Substring('CMAKE_GENERATOR_PLATFORM:INTERNAL='.Length) } else { '' }
    if ($cachedGenerator -and ($cachedGenerator -ne $Generator -or $cachedPlatform -ne 'x64')) {
        Write-Host "CMake cache uses generator '$cachedGenerator' with platform '$cachedPlatform'. Refreshing only CMake's cache and generated metadata in '$buildDir'."
        $configureArgs = @('--fresh') + $configureArgs
    }
}

& $cmakeCommand @configureArgs
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed with exit code $LASTEXITCODE." }

& $cmakeCommand --build $buildDir --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE." }

if (-not $NoTest) {
    $ctestCommand = Get-Command ctest -ErrorAction SilentlyContinue
    $ctestPath = if ($ctestCommand) { $ctestCommand.Source } else { $null }
    if (-not $ctestPath) {
        $ctestPath = Join-Path (Split-Path $cmakeCommand) "ctest.exe"
        if (-not (Test-Path -LiteralPath $ctestPath)) { $ctestPath = $null }
    }
    if (-not $ctestPath) { throw "CTest was not found. Install the CMake testing tools or pass -NoTest." }
    & $ctestPath --test-dir $buildDir -C $Configuration --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "CTest failed with exit code $LASTEXITCODE." }
}

if (-not $NoRun) {
    $appName = if ($Editor) { "TogetherEngineEditor.exe" } else { "TogetherWeDo.exe" }
    $executable = Join-Path $buildDir "$Configuration\$appName"
    if (-not (Test-Path -LiteralPath $executable)) { throw "Expected executable was not produced: $executable" }
    Push-Location (Split-Path $executable)
    try { & $executable } finally { Pop-Location }
}
