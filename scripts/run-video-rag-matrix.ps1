param(
    [Parameter(Mandatory=$true)][string]$ThirdPartyRoot,
    [string]$QtBin='D:/Qt/6.9.1/msvc2022_64/bin',
    [int]$Parallel=2
)
$ErrorActionPreference='Stop'
$repositoryRoot=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$previousPath=$env:PATH
try {
    $env:PATH="$QtBin;$previousPath"
    foreach($variant in @(@('ON','ON'),@('ON','OFF'),@('OFF','ON'),@('OFF','OFF'))) {
        $label="onnx-$($variant[0])-whisper-$($variant[1])"
        $buildDirectory=Join-Path $repositoryRoot "build/matrix/$label"
        & cmake -S $repositoryRoot -B $buildDirectory -G 'Visual Studio 17 2022' -A x64 "-DFRAMEMIND_THIRD_PARTY_ROOT=$ThirdPartyRoot" "-DFRAMEMIND_ENABLE_ONNX=$($variant[0])" "-DFRAMEMIND_ENABLE_WHISPER=$($variant[1])" -DBUILD_TESTING=ON
        if($LASTEXITCODE -ne 0) {throw "Configure failed: $label"}
        & cmake --build $buildDirectory --config Debug --parallel $Parallel
        if($LASTEXITCODE -ne 0) {throw "Build failed: $label"}
        & ctest --test-dir $buildDirectory -C Debug --output-on-failure
        if($LASTEXITCODE -ne 0) {throw "Tests failed: $label"}
    }
} finally {$env:PATH=$previousPath}
