$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $projectRoot
try {
    New-Item -ItemType Directory -Force build | Out-Null
    & gcc -std=c11 -Wall -Wextra -Werror -pedantic -O2 -I include tools/edufs.c -o build/edufs.exe
    if ($LASTEXITCODE -ne 0) { throw 'C compilation failed' }
    & python tests/test_images.py
    if ($LASTEXITCODE -ne 0) { throw 'Image integration tests failed' }
} finally {
    Pop-Location
}
