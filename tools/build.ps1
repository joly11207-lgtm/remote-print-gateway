# Real firmware builds for all V0.1 targets.
#
# Windows/xtensa limitation: the ESP32 cross compiler cannot read paths that
# contain non-ASCII characters. This script mirrors the project into an ASCII
# directory and builds there. The workspace remains the single source of truth.
param(
  [string[]]$Envs = @("s3_n16r8", "s3_supermini", "wroom32e_future"),
  [string]$Mirror = "C:\Users\Public\rtb-v01-build",
  [string]$CoreDir = "C:\Users\Public\rtb-pio-core"
)

$root = Split-Path -Parent $PSScriptRoot
$logDir = Join-Path $root "build_logs"
New-Item -ItemType Directory -Force -Path $Mirror, $logDir | Out-Null

& robocopy.exe $root $Mirror /E /PURGE /XD .git .pio build __pycache__ /NFL /NDL /NJH /NJS /NP | Out-Null

$env:PLATFORMIO_CORE_DIR = $CoreDir
$results = @()

foreach ($envName in $Envs) {
  Write-Host "=== BUILD TARGET: $envName ==="
  $log = Join-Path $logDir "$envName.log"
  & python -m platformio run --project-dir $Mirror -e $envName *>&1 |
      Tee-Object -FilePath $log
  $code = $LASTEXITCODE
  $results += [pscustomobject]@{ Target = $envName; ExitCode = $code; Log = $log }
  Write-Host ("--- {0}: {1} ---" -f $envName, $(if ($code -eq 0) { "SUCCESS" } else { "FAILED" }))
}

Write-Host "=== BUILD SUMMARY ==="
$results | Format-Table -AutoSize | Out-String | Write-Host
$failed = @($results | Where-Object { $_.ExitCode -ne 0 })
if ($failed.Count -gt 0) { exit 1 }
exit 0
