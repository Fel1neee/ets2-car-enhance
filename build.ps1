param(
    [string]$MingwBin = "D:\tools\llvm-mingw-20261117-ucrt-x86_64\bin",
    [string]$GamePluginsDir = "D:\Euro Truck Simulator 2\bin\win_x64\plugins"
)

$ErrorActionPreference = "Stop"
if (Test-Path $MingwBin) {
    $env:PATH = "$MingwBin;" + $env:PATH
}

$root = $PSScriptRoot
$buildDir = Join-Path $root "build"
if (!(Test-Path $buildDir)) { New-Item -ItemType Directory -Path $buildDir | Out-Null }

$prefixFlag = "-ffile-prefix-map=$root=."
$inc = @(
    $prefixFlag,
    "-I$root\src",
    "-I$root\third_party\scs_sdk\include",
    "-I$root\third_party\minhook-master\include",
    "-I$root\third_party\imgui-master",
    "-I$root\third_party\imgui-master\backends"
)

# Compile third-party objects once if missing
$objFiles = @()
$mhSrc = @(
    "$root\third_party\minhook-master\src\buffer.c",
    "$root\third_party\minhook-master\src\hook.c",
    "$root\third_party\minhook-master\src\trampoline.c",
    "$root\third_party\minhook-master\src\hde\hde64.c"
)
foreach ($f in $mhSrc) {
    $obj = Join-Path $buildDir ([IO.Path]::GetFileNameWithoutExtension($f) + ".o")
    if (!(Test-Path $obj)) { & gcc -O2 $prefixFlag -I"$root\third_party\minhook-master\include" -c $f -o $obj }
    $objFiles += $obj
}

$imguiSrc = @(
    "$root\third_party\imgui-master\imgui.cpp",
    "$root\third_party\imgui-master\imgui_draw.cpp",
    "$root\third_party\imgui-master\imgui_tables.cpp",
    "$root\third_party\imgui-master\imgui_widgets.cpp",
    "$root\third_party\imgui-master\backends\imgui_impl_dx11.cpp",
    "$root\third_party\imgui-master\backends\imgui_impl_win32.cpp"
)
foreach ($f in $imguiSrc) {
    $obj = Join-Path $buildDir ([IO.Path]::GetFileNameWithoutExtension($f) + ".o")
    if (!(Test-Path $obj)) { & g++ -std=c++17 -O2 $prefixFlag -I"$root\third_party\imgui-master" -I"$root\third_party\imgui-master\backends" -c $f -o $obj }
    $objFiles += $obj
}

$src = @(
    "$root\src\main.cpp",
    "$root\src\dx11_blur.cpp",
    "$root\src\physics_downforce.cpp",
    "$root\src\exports.def"
)
$libs = @("-ld3d11", "-ld3dcompiler", "-ldxgi", "-ldwmapi", "-lwinmm", "-luser32", "-lgdi32", "-static-libgcc", "-static-libstdc++")
$outDll = Join-Path $buildDir "ets2_car_enhance.dll"

& g++ -std=c++17 -O2 -s -shared @inc @src @objFiles -o $outDll @libs
Write-Host "Built: $outDll"

if (Test-Path $GamePluginsDir) {
    Copy-Item $outDll (Join-Path $GamePluginsDir "ets2_car_enhance.dll") -Force
    Write-Host "Deployed to: $GamePluginsDir"
}
