param([ValidateSet('build', 'menuconfig', 'size')][string]$BuildAction = 'build', [switch]$AudioCheck)
# Usage: ./tools/build-firmware.ps1 [build|menuconfig|size] [-AudioCheck]
# AudioCheck compiles the complete runtime with illustrative GPIOs; never flash it as a board pinout.
$ErrorActionPreference = 'Stop'
$env:PYTHONUTF8 = '1'
. 'C:/Espressif/tools/Microsoft.v6.1.PowerShell_profile.ps1'
Push-Location (Join-Path $PSScriptRoot '../firmware')
try {
    if ($AudioCheck) {
        idf.py -B build-audio -D SDKCONFIG=sdkconfig.audio -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.ci.defaults;sdkconfig.audio.defaults' $BuildAction
    } else {
        idf.py $BuildAction
    }
    if ($LASTEXITCODE -ne 0) { throw "idf.py failed: $LASTEXITCODE" }
}
finally { Pop-Location }
