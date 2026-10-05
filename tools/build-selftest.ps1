# Builds all audio paths with illustrative GPIOs. Never flash this as a board configuration.
param([switch]$Afe)
$ErrorActionPreference = 'Stop'
$env:PYTHONUTF8 = '1'
. 'C:/Espressif/tools/Microsoft.v6.1.PowerShell_profile.ps1'
Push-Location (Join-Path $PSScriptRoot '../firmware')
try {
    if ($Afe) {
        idf.py -B build-selftest-afe -D SDKCONFIG=sdkconfig.afe -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.ci.defaults;sdkconfig.afe.defaults' build
    } else {
        idf.py -B build-selftest -D SDKCONFIG=sdkconfig.ci -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.ci.defaults' build
    }
    if ($LASTEXITCODE -ne 0) { throw 'Self-test build failed' }
}
finally { Pop-Location }
