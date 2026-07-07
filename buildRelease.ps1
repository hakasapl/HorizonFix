#Requires -Version 5.1
<#
.SYNOPSIS
    Configures and builds both the SE (1.5.97) and AE (1.6.x) variants of the
    plugin in RelWithDebInfo, then stages the artifacts into dist/.

.DESCRIPTION
    Produces two flavors from the one source tree (see CMakeLists.txt):
      - AE: SKYRIM_SUPPORT_AE=ON  -> HorizonFix.dll   -> dist/ae/
      - SE: SKYRIM_SUPPORT_AE=OFF -> HorizonFixSE.dll  -> dist/se/

    For each flavor the built DLL and PDB, plus everything in the package/
    folder, are copied into the matching dist subfolder.

.PARAMETER Config
    CMake build configuration. Defaults to RelWithDebInfo.

.PARAMETER VcpkgRoot
    Path to the vcpkg root (containing scripts/buildsystems/vcpkg.cmake).
    Defaults to $env:VCPKG_ROOT, then the Visual Studio bundled vcpkg.
#>
[CmdletBinding()]
param(
    [string]$Config = "RelWithDebInfo",
    [string]$VcpkgRoot = $env:VCPKG_ROOT
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = $PSScriptRoot
$BuildRoot = Join-Path $RepoRoot "buildRel"
$DistRoot = Join-Path $RepoRoot "dist"
$PackageDir = Join-Path $RepoRoot "package"

# variant name, CMake flag value, resulting plugin/DLL base name
$Variants = @(
    [pscustomobject]@{ Name = "ae"; SupportAE = "ON"; Plugin = "HorizonFix" }
    [pscustomobject]@{ Name = "se"; SupportAE = "OFF"; Plugin = "HorizonFixSE" }
)

function Write-Step($msg) { Write-Host "==> $msg" -ForegroundColor Cyan }

# --- Set up the MSVC / Ninja developer environment ---------------------------
function Enter-DevEnvironment {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
        return # already in a developer shell
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        throw "vswhere.exe not found. Run this script from a Developer PowerShell for VS, or install Visual Studio."
    }

    $vsPath = & $vswhere -latest -prerelease -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if (-not $vsPath) {
        throw "No Visual Studio installation with the C++ toolset was found."
    }

    $devShell = Join-Path $vsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
    if (-not (Test-Path $devShell)) {
        throw "DevShell module not found at $devShell."
    }

    Write-Step "Entering Visual Studio developer environment ($vsPath)"
    Import-Module $devShell
    Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation `
        -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
}

# --- Resolve the vcpkg toolchain file ----------------------------------------
function Resolve-VcpkgToolchain {
    $candidates = @()
    if ($VcpkgRoot) { $candidates += $VcpkgRoot }
    if ($env:VCPKG_INSTALLATION_ROOT) { $candidates += $env:VCPKG_INSTALLATION_ROOT }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vsPath = & $vswhere -latest -prerelease -products * -property installationPath
        if ($vsPath) { $candidates += (Join-Path $vsPath "VC\vcpkg") }
    }

    foreach ($root in $candidates) {
        $toolchain = Join-Path $root "scripts\buildsystems\vcpkg.cmake"
        if (Test-Path $toolchain) { return $toolchain }
    }

    throw "Could not locate vcpkg.cmake. Set VCPKG_ROOT or pass -VcpkgRoot."
}

# --- Clean and recreate output folders ---------------------------------------
Write-Step "Cleaning output folders"
foreach ($dir in @($BuildRoot, $DistRoot)) {
    if (Test-Path $dir) { Remove-Item -Path $dir -Recurse -Force }
}
New-Item -ItemType Directory -Path $BuildRoot | Out-Null
New-Item -ItemType Directory -Path $DistRoot  | Out-Null

Enter-DevEnvironment
$Toolchain = Resolve-VcpkgToolchain
Write-Step "Using vcpkg toolchain: $Toolchain"

# --- Configure, build and stage each variant ---------------------------------
foreach ($v in $Variants) {
    $buildDir = Join-Path $BuildRoot $v.Name
    $distDir = Join-Path (Join-Path $DistRoot $v.Name) "SKSE\plugins"
    New-Item -ItemType Directory -Path $distDir | Out-Null

    Write-Step "Configuring $($v.Name.ToUpper()) variant (SKYRIM_SUPPORT_AE=$($v.SupportAE))"
    & cmake -S $RepoRoot -B $buildDir -G Ninja `
        "-DCMAKE_BUILD_TYPE=$Config" `
        "-DCMAKE_TOOLCHAIN_FILE=$Toolchain" `
        "-DVCPKG_TARGET_TRIPLET=x64-windows-static" `
        "-DSKYRIM_SUPPORT_AE=$($v.SupportAE)"
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed for $($v.Name)." }

    Write-Step "Building $($v.Name.ToUpper()) variant"
    & cmake --build $buildDir --config $Config
    if ($LASTEXITCODE -ne 0) { throw "Build failed for $($v.Name)." }

    Write-Step "Staging artifacts into $distDir"
    $dll = Get-ChildItem -Path $buildDir -Recurse -Filter "$($v.Plugin).dll" | Select-Object -First 1
    if (-not $dll) { throw "Built DLL '$($v.Plugin).dll' not found under $buildDir." }
    Copy-Item -Path $dll.FullName -Destination $distDir -Force

    $pdb = Get-ChildItem -Path $buildDir -Recurse -Filter "$($v.Plugin).pdb" | Select-Object -First 1
    if ($pdb) {
        Copy-Item -Path $pdb.FullName -Destination $distDir -Force
    }
    else {
        Write-Warning "PDB '$($v.Plugin).pdb' not found under $buildDir."
    }

    if (Test-Path $PackageDir) {
        Copy-Item -Path (Join-Path $PackageDir "*") -Destination $distDir -Recurse -Force
    }
}

Write-Step "Done. Artifacts staged in $DistRoot"
Get-ChildItem -Path $DistRoot -Recurse -File | ForEach-Object {
    Write-Host "  $($_.FullName.Substring($DistRoot.Length + 1))"
}
