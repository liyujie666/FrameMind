param(
    [string]$BuildDirectory = 'build/content-quality-p01-msvc',
    [string]$VisualStudioRoot = 'D:/Visual Studio/Visual Studio2022',
    [string]$MsvcVersion = '14.42.34433',
    [string]$WindowsSdkRoot = 'D:/Windows Kits/10',
    [string]$WindowsSdkVersion = '10.0.20348.0',
    [string]$QtRoot = 'D:/Qt/6.9.1/msvc2022_64',
    [int]$Parallel = 3,
    [switch]$TestsOnly
)
$ErrorActionPreference = 'Stop'
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$validationRoot = Join-Path $repositoryRoot 'build/content-quality-validation'
New-Item -ItemType Directory -Force -Path $validationRoot | Out-Null
$vsDevCmd = Join-Path $VisualStudioRoot 'Common7/Tools/VsDevCmd.bat'
$compilerBin = Join-Path $VisualStudioRoot "VC/Tools/MSVC/$MsvcVersion/bin/HostX64/x64"
$sdkBin = Join-Path $WindowsSdkRoot "bin/$WindowsSdkVersion/x64"
$debugCrt = Join-Path $VisualStudioRoot "VC/Redist/MSVC/$MsvcVersion/debug_nonredist/x64/Microsoft.VC143.DebugCRT"
$ninja = Join-Path $VisualStudioRoot 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
foreach ($required in @($vsDevCmd, "$compilerBin/cl.exe", "$sdkBin/rc.exe", "$sdkBin/mt.exe", $ninja,
                        "$QtRoot/bin/Qt6Cored.dll", "$debugCrt/msvcp140d.dll", "$sdkBin/ucrt/ucrtbased.dll")) {
    if (!(Test-Path -LiteralPath $required)) { throw "Toolchain file missing: $required" }
}
$savedEnvironment = @{}
foreach ($name in @('PATH', 'INCLUDE', 'LIB', 'LIBPATH', 'QT_PLUGIN_PATH')) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
try {
    # Import the MSVC headers/libraries, then explicitly choose executable and DLL paths.
    $taskEnvironment = & cmd /c "`"$vsDevCmd`" -arch=x64 -host_arch=x64 >nul && set"
    if ($LASTEXITCODE -ne 0) { throw 'VsDevCmd failed' }
    foreach ($line in $taskEnvironment) {
        if ($line -match '^(INCLUDE|LIB|LIBPATH)=(.*)$') {
            [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
        }
    }
    $env:PATH = "$compilerBin;$sdkBin;$QtRoot/bin;$debugCrt;$sdkBin/ucrt;$env:PATH"
    $env:QT_PLUGIN_PATH = "$QtRoot/plugins"
    if (!$TestsOnly) {
        & cmake -S $repositoryRoot -B $BuildDirectory -G Ninja "-DCMAKE_CXX_COMPILER=$compilerBin/cl.exe" `
            "-DCMAKE_RC_COMPILER=$sdkBin/rc.exe" "-DCMAKE_MT=$sdkBin/mt.exe" "-DCMAKE_MAKE_PROGRAM=$ninja" `
            "-DCMAKE_PREFIX_PATH=$QtRoot" -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON `
            -DFRAMEMIND_ENABLE_ONNX=ON -DFRAMEMIND_ENABLE_WHISPER=ON *> "$validationRoot/p01-msvc-configure.log"
        if ($LASTEXITCODE -ne 0) { throw "Configure failed; see $validationRoot/p01-msvc-configure.log" }
        & cmake --build $BuildDirectory --parallel $Parallel *> "$validationRoot/p01-msvc-build.log"
        if ($LASTEXITCODE -ne 0) { throw "Build failed; see $validationRoot/p01-msvc-build.log" }
    }
    & ctest --test-dir $BuildDirectory --output-on-failure *> "$validationRoot/p01-tests.log"
    $testExit = $LASTEXITCODE
    Get-Content "$validationRoot/p01-tests.log"
    if ($testExit -ne 0) { throw "Tests failed; see $validationRoot/p01-tests.log" }
} finally {
    foreach ($name in $savedEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process')
    }
}
