# Regenerates the corpus, builds the harness and runs it from the repository
# root (the tests use paths relative to it).
#
#   powershell -ExecutionPolicy Bypass -File tools\run_tests.ps1

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$tmp  = Join-Path $root "build\tmp"

# The corpus is cheap to rebuild and its archives must be newer than the
# stability window the tests assert on, so it is always regenerated.
python (Join-Path $root "tools\gen_corpus.py")
if ($LASTEXITCODE -ne 0) { throw "corpus generation failed" }

# Extraction tests assert that a refused job leaves no destination behind, so
# they need a clean tree rather than leftovers from the last run.
if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
New-Item -ItemType Directory -Path $tmp | Out-Null

& (Join-Path $root "tools\build_host.ps1")

Push-Location $root
try {
    & (Join-Path $root "build\ua_tests.exe")
    $code = $LASTEXITCODE
} finally {
    Pop-Location
}

exit $code
