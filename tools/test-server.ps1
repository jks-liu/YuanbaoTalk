# Usage: ./tools/test-server.ps1. Uses workspace venv; install server/requirements.txt first.
$ErrorActionPreference = 'Stop'
$env:PYTHONPATH = Join-Path $PSScriptRoot '../server'
& (Join-Path $PSScriptRoot '../.venv/Scripts/python.exe') -m unittest discover -s (Join-Path $PSScriptRoot '../server/tests') -v
if ($LASTEXITCODE -ne 0) { throw 'Server tests failed' }
