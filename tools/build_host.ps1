# Host test harness build (requirements Section 8: the extraction core is
# compiled for the developer machine and exercised against a corpus, so only
# platform integration needs the console).
#
#   powershell -ExecutionPolicy Bypass -File tools\build_host.ps1
#
# MSVC is used because Visual Studio is already installed here. The sources are
# portable C with no MSVC-specific constructs, so the PS5 SDK clang build in
# Makefile.ps5 compiles the same files.

$ErrorActionPreference = "Stop"

$root  = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build"
$obj   = Join-Path $build "obj"
$miniz = Join-Path $root "third_party\miniz"
$lzma  = Join-Path $root "third_party\lzma"

foreach ($d in @($build, $obj)) {
    if (-not (Test-Path $d)) { New-Item -ItemType Directory -Path $d | Out-Null }
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere not found; is Visual Studio installed?" }

$vsPath = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath
if (-not $vsPath) { throw "No MSVC toolset found. Install the VS C++ workload." }
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"

function Invoke-Cl([string]$clArgs, [string]$what) {
    # vcvars64.bat prints a banner and, on some installs, a harmless warning
    # about vswhere; only the exit code matters.
    cmd /c "`"$vcvars`" >nul 2>&1 && cl $clArgs"
    if ($LASTEXITCODE -ne 0) { throw "$what failed ($LASTEXITCODE)" }
}

# --- miniz -----------------------------------------------------------------
# Vendored third-party code is compiled separately at /W0: it is not ours to
# keep warning-clean, and mixing it into the strict build would force either
# local patches or a blanket warning suppression over our own code.

$minizSources = @("miniz.c", "miniz_tinfl.c", "miniz_tdef.c", "miniz_zip.c") |
    ForEach-Object { "`"" + (Join-Path $miniz $_) + "`"" }

$minizObjDir = Join-Path $obj "miniz"
if (-not (Test-Path $minizObjDir)) { New-Item -ItemType Directory -Path $minizObjDir | Out-Null }

$minizArgs = @(
    "/nologo", "/c", "/W0", "/O2", "/MT", "/D_CRT_SECURE_NO_WARNINGS",
    # The doubled trailing backslash is not a typo: cmd treats \" as an escaped
    # quote, so /Fo:"dir\" would swallow the rest of the command line.
    "/I", "`"$miniz`"", "/Fo:`"$minizObjDir\\`""
) + $minizSources

Invoke-Cl ($minizArgs -join " ") "miniz compile"

# --- LZMA SDK --------------------------------------------------------------
# 7zDec.c is required even though SzArEx_Extract is never called: a 7z header
# is itself LZMA-compressed and SzArEx_Open decodes it. _7ZIP_ST builds it
# single-threaded, matching FR-8.

$lzmaSources = @(
    "7zArcIn.c", "7zAlloc.c", "7zBuf.c", "7zCrc.c", "7zCrcOpt.c", "7zDec.c",
    "7zStream.c", "Bcj2.c", "Bra.c", "Bra86.c", "BraIA64.c", "CpuArch.c",
    "Delta.c", "LzmaDec.c", "Lzma2Dec.c"
) | ForEach-Object { "`"" + (Join-Path $lzma $_) + "`"" }

$lzmaObjDir = Join-Path $obj "lzma"
if (-not (Test-Path $lzmaObjDir)) { New-Item -ItemType Directory -Path $lzmaObjDir | Out-Null }

$lzmaArgs = @(
    "/nologo", "/c", "/W0", "/O2", "/MT", "/D_CRT_SECURE_NO_WARNINGS", "/D_7ZIP_ST",
    "/I", "`"$lzma`"", "/Fo:`"$lzmaObjDir\\`""
) + $lzmaSources

Invoke-Cl ($lzmaArgs -join " ") "lzma compile"

$lzmaObjs = (Get-ChildItem $lzmaObjDir -Filter *.obj | ForEach-Object { "`"" + $_.FullName + "`"" })

# --- FGG Unpack + tests ------------------------------------------------
# /W4 /WX on our own code: an archive parser is exactly where an ignored
# warning turns into a memory-safety bug.

$sources = @(
    "src\ua_common.c"
    "src\core\ua_path.c"
    "src\core\ua_log.c"
    "src\core\ua_extract.c"
    "src\core\ua_config.c"
    "src\core\ua_queue.c"
    "src\core\ua_status.c"
    "src\core\ua_driver.c"
    "src\backend\ua_backend.c"
    "src\backend\ua_detect.c"
    "src\backend\ua_be_zip.c"
    "src\backend\ua_be_7z.c"
    "src\platform\ua_plat_win.c"
    "tests\test_main.c"
    "tests\test_path.c"
    "tests\test_detect.c"
    "tests\test_extract.c"
    "tests\test_7z.c"
    "tests\test_config.c"
    "tests\test_driver.c"
) | ForEach-Object { "`"" + (Join-Path $root $_) + "`"" }

$minizObjs = (Get-ChildItem $minizObjDir -Filter *.obj | ForEach-Object { "`"" + $_.FullName + "`"" })

# MINIZ_NO_ZLIB_COMPATIBLE_NAMES matches Makefile.ps5: miniz.h otherwise
# defines unused static inline zlib aliases in every including translation
# unit, which the console build rejects under -Werror. Kept identical here so
# the harness compiles the same code the payload does.
$clArgs = @(
    "/nologo", "/W4", "/WX", "/O2", "/MT", "/Zi", "/utf-8",
    "/D_CRT_SECURE_NO_WARNINGS", "/DMINIZ_NO_ZLIB_COMPATIBLE_NAMES",
    "/I", "`"$miniz`"", "/I", "`"$lzma`""
) + $sources + $minizObjs + $lzmaObjs + @(
    "/Fe:`"$build\ua_tests.exe`"", "/Fo:`"$obj\\`"", "/Fd:`"$build\ua_tests.pdb`""
)

Invoke-Cl ($clArgs -join " ") "compile"

Write-Host "built $build\ua_tests.exe"

# --- command-line front end ------------------------------------------------
# Same core, driven from a PC: lets an archive be checked before it is copied
# to the console. Separate link because it has its own main().

$cliSources = @(
    "src\ua_common.c"
    "src\core\ua_path.c"
    "src\core\ua_log.c"
    "src\core\ua_extract.c"
    "src\backend\ua_backend.c"
    "src\backend\ua_detect.c"
    "src\backend\ua_be_zip.c"
    "src\backend\ua_be_7z.c"
    "src\platform\ua_plat_win.c"
    "src\main_host.c"
) | ForEach-Object { "`"" + (Join-Path $root $_) + "`"" }

$cliObj = Join-Path $obj "cli"
if (-not (Test-Path $cliObj)) { New-Item -ItemType Directory -Path $cliObj | Out-Null }

$cliArgs = @(
    "/nologo", "/W4", "/WX", "/O2", "/MT", "/utf-8",
    "/D_CRT_SECURE_NO_WARNINGS", "/DMINIZ_NO_ZLIB_COMPATIBLE_NAMES",
    "/I", "`"$miniz`"", "/I", "`"$lzma`""
) + $cliSources + $minizObjs + $lzmaObjs + @(
    "/Fe:`"$build\fgg-unpack.exe`"", "/Fo:`"$cliObj\\`""
)

Invoke-Cl ($cliArgs -join " ") "cli compile"

Write-Host "built $build\fgg-unpack.exe"
