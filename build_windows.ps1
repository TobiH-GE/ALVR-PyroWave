# Builds the PyroWave streamer on Windows, from scratch or as an update.
#
#   powershell -ExecutionPolicy Bypass -File build_windows.ps1            # everything under C:\Temp
#   powershell -ExecutionPolicy Bypass -File build_windows.ps1 -Root D:\x # somewhere else
#   ... -PrepareDeps   first run `cargo xtask prepare-deps` (installs LLVM, needed by bindgen,
#                      via Chocolatey; admin). Only needed on a machine that never built ALVR.
#
# Needs: Git for Windows (with Git Bash), Visual Studio 2022 with the C++ workload, CMake 3.27+,
# Rust (rustup), and a GPU driver with Vulkan 1.3 (current NVIDIA drivers have it).
#
# Steps: clone or update this repository and PyroWave (at the commit the client uses), apply
# pyrowave-patches\*.patch, build PyroWave's shared library, copy its header, the Vulkan headers and the DLL to
# deps\windows\pyrowave, and build the streamer. The result is in
# <Root>\ALVR-PyroWave-private\build\alvr_streamer_windows. --keep-config keeps session.json
# (settings and headset pairing) of an earlier build there.

param(
    [string]$Root = "C:\Temp",
    [string]$Branch = "main",
    [switch]$PrepareDeps
)

$ErrorActionPreference = "Stop"

# Must match deps/windows/pyrowave/README.md and ALVRClient/PyroWave/VENDORED.md in the client.
$PyroWaveCommit = "89f7e47d4abbf650c91fae766728af866c5e32a0"
$RepoUrl = "https://github.com/TobiH-GE/ALVR-PyroWave.git"
$Repo = Join-Path $Root "ALVR-PyroWave-private"
$PyroWave = Join-Path $Root "pyrowave"

function Step($text) { Write-Host "=== $text ===" -ForegroundColor Cyan }
function Run($exe, [string[]]$arguments) {
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "$exe $($arguments -join ' ') failed with exit code $LASTEXITCODE" }
}

New-Item -ItemType Directory -Force -Path $Root | Out-Null

Step "[1/5] Streamer repository ($Branch)"
if (Test-Path (Join-Path $Repo ".git")) {
    Run git @("-C", $Repo, "fetch", "origin", $Branch)
    Run git @("-C", $Repo, "checkout", $Branch)
    Run git @("-C", $Repo, "pull", "--ff-only", "origin", $Branch)
} else {
    Run git @("clone", "--branch", $Branch, $RepoUrl, $Repo)
}
Run git @("-C", $Repo, "submodule", "update", "--init", "--recursive")
Write-Host "streamer at $(git -C $Repo rev-parse --short HEAD)"

Step "[2/5] PyroWave $PyroWaveCommit"
if (-not (Test-Path (Join-Path $PyroWave ".git"))) {
    Run git @("clone", "https://github.com/Themaister/pyrowave", $PyroWave)
}
Run git @("-C", $PyroWave, "fetch", "origin")
# -f drops the patches applied by an earlier run, so they apply cleanly again below.
# --ignore-whitespace below: Git for Windows usually checks out with CRLF, the patches are LF.
Run git @("-C", $PyroWave, "checkout", "-f", $PyroWaveCommit)
# The streamer's own changes to PyroWave (encoder side only; the bitstream is unchanged, so the
# client's Metal decoder is not affected). See pyrowave-patches\README.md.
# *.granite.patch go to PyroWave's Granite checkout (checkout_granite.sh, not a submodule) below.
$Patches = Get-ChildItem (Join-Path $Repo "pyrowave-patches") -Filter "*.patch" | Sort-Object Name
foreach ($patch in $Patches | Where-Object { $_.Name -notlike "*.granite.patch" }) {
    Run git @("-C", $PyroWave, "apply", "--ignore-whitespace", "--whitespace=nowarn", $patch.FullName)
    Write-Host "applied $($patch.Name)"
}
$GitBash = Join-Path (Split-Path (Split-Path (Get-Command git).Source)) "bin\bash.exe"
if (-not (Test-Path $GitBash)) { $GitBash = "bash" }
Push-Location $PyroWave
try { Run $GitBash @("checkout_granite.sh") } finally { Pop-Location }
$Granite = Join-Path $PyroWave "Granite"
# checkout_granite.sh keeps local changes, so drop an earlier run's patches first.
Run git @("-C", $Granite, "checkout", "--", ".")
foreach ($patch in $Patches | Where-Object { $_.Name -like "*.granite.patch" }) {
    Run git @("-C", $Granite, "apply", "--ignore-whitespace", "--whitespace=nowarn", $patch.FullName)
    Write-Host "applied $($patch.Name) to Granite"
}

Step "[3/5] Building PyroWave (Release)"
$PyroBuild = Join-Path $PyroWave "build"
Run cmake @("-S", $PyroWave, "-B", $PyroBuild, "-G", "Visual Studio 17 2022", "-A", "x64",
            "-DCMAKE_INSTALL_PREFIX=$PyroBuild\output")
Run cmake @("--build", $PyroBuild, "--config", "Release", "--target", "install")

if ($PrepareDeps) {
    # Runs before deps\windows\pyrowave is filled: prepare-deps deletes deps\windows.
    Step "[3b/5] cargo xtask prepare-deps"
    Push-Location $Repo
    try { Run cargo @("xtask", "prepare-deps", "--platform", "windows") } finally { Pop-Location }
    Run git @("-C", $Repo, "checkout", "--", "deps/windows/pyrowave/README.md")
}

Step "[4/5] deps\windows\pyrowave"
$Deps = Join-Path $Repo "deps\windows\pyrowave"
New-Item -ItemType Directory -Force -Path "$Deps\include\pyrowave", "$Deps\bin" | Out-Null
Copy-Item "$PyroBuild\output\include\pyrowave\pyrowave.h" "$Deps\include\pyrowave\" -Force
$VulkanHeaders = Join-Path $PyroWave "Granite\third_party\khronos\vulkan-headers\include"
foreach ($dir in @("vulkan", "vk_video")) {
    if (Test-Path "$Deps\include\$dir") { Remove-Item -Recurse -Force "$Deps\include\$dir" }
    Copy-Item -Recurse "$VulkanHeaders\$dir" "$Deps\include\$dir"
}
$Dlls = Get-ChildItem "$PyroBuild\output\bin" -Filter "*pyrowave*.dll"
if ($Dlls.Count -eq 0) { throw "no PyroWave DLL in $PyroBuild\output\bin" }
$Dlls | Copy-Item -Destination "$Deps\bin\" -Force
Write-Host "copied $($Dlls.Name -join ', ')"

Step "[5/5] Building the streamer"
Push-Location $Repo
try { Run cargo @("xtask", "build-streamer", "--release", "--keep-config") } finally { Pop-Location }

$Out = Join-Path $Repo "build\alvr_streamer_windows"
Write-Host ""
Write-Host "=== DONE: $Out ===" -ForegroundColor Green
Write-Host "The build log above must contain 'Building with the PyroWave encoder'; without it the"
Write-Host "streamer was built without PyroWave. Check the GPU with:"
Write-Host "  $PyroBuild\output\bin\pyrowave-device-validation.exe"
