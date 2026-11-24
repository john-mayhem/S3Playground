# Run idf.py for this project with ESP-IDF 5.5.4 loaded. Usage: tools\idf.ps1 build | flash | monitor ...
# Flashing/logs go through the COM port (CH343, COM9 on the owner's PC): the native USB port is TinyUSB's.
$ErrorActionPreference = "Stop"
$env:IDF_TARGET = "esp32s3"
. C:\Espressif\frameworks\esp-idf-v5.5.4\export.ps1 *> $null
Set-Location (Split-Path $PSScriptRoot)
idf.py @args
exit $LASTEXITCODE
