# Push build\s3_playground.bin to the board over Wi-Fi. Usage: tools\ota.ps1 [-Target s3-playground.local]
# Build first (tools\idf.ps1 build). The token comes from main\secrets.h.
param([string]$Target = "s3-playground.local")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot
$bin = Join-Path $root "build\s3_playground.bin"
if (-not (Test-Path $bin)) { throw "no $bin - run tools\idf.ps1 build first" }
$m = Select-String -Path (Join-Path $root "main\secrets.h") -Pattern '#define\s+OTA_TOKEN\s+"([^"]+)"'
if (-not $m) { throw "OTA_TOKEN not found in main\secrets.h" }
$token = $m.Matches[0].Groups[1].Value
$size = (Get-Item $bin).Length
Write-Host "OTA: $([math]::Round($size / 1KB)) KB -> http://$Target/ota"
curl.exe --fail-with-body -sS --max-time 120 -H "X-OTA-Token: $token" -H "Content-Type: application/octet-stream" --data-binary "@$bin" "http://$Target/ota"
if ($LASTEXITCODE) { throw "OTA failed (curl exit $LASTEXITCODE)" }
Write-Host "Board is rebooting into the new image; it confirms itself once Wi-Fi is up (else it rolls back)."
