# Host-native Mock tests, compiled with Zig C++ (no ESP32 board needed).
# Includes AddressSanitizer, UndefinedBehaviorSanitizer and combined
# ASAN+UBSAN regression builds, plus canaries proving each is really active.
param(
  [string]$Mirror = "C:\Users\Public\rtb-v01-build",
  [string]$AsanLibDir = "",
  [string]$AsanDllDir = ""
)

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot
New-Item -ItemType Directory -Force -Path $Mirror | Out-Null
& robocopy.exe $root $Mirror /E /PURGE /XD .git .pio build __pycache__ /NFL /NDL /NJH /NJS /NP | Out-Null

$zig = (& python -c "import ziglang,os;print(os.path.join(os.path.dirname(ziglang.__file__),'zig.exe'))").Trim()
if (-not (Test-Path $zig)) { Write-Error "zig.exe not found: $zig"; exit 2 }

# Zig's Windows package has no bundled ASAN runtime; use the one shipped with
# the Visual Studio BuildTools (clang_rt.asan_dynamic-x86_64).
if (-not $AsanLibDir -or -not $AsanDllDir) {
  $msvcBase = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC"
  $msvc = Get-ChildItem $msvcBase -Directory -ErrorAction SilentlyContinue |
          Sort-Object Name -Descending | Select-Object -First 1
  if ($msvc) {
    if (-not $AsanLibDir) { $AsanLibDir = Join-Path $msvc.FullName "lib\x64" }
    if (-not $AsanDllDir) { $AsanDllDir = Join-Path $msvc.FullName "bin\Hostx64\x64" }
  }
}
$asanLib = Join-Path $AsanLibDir "clang_rt.asan_dynamic-x86_64.lib"
$asanDll = Join-Path $AsanDllDir "clang_rt.asan_dynamic-x86_64.dll"
if (-not (Test-Path $asanLib) -or -not (Test-Path $asanDll)) {
  Write-Error "ASAN runtime not found (lib=$asanLib dll=$asanDll). Install VS BuildTools C++ ASAN components or pass -AsanLibDir/-AsanDllDir."
  exit 2
}
$env:PATH = "$AsanDllDir;$env:PATH"
$env:ASAN_OPTIONS = "detect_leaks=0:halt_on_error=1:abort_on_error=0"

$outDir = Join-Path $Mirror "build\host"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$commonSources = @(
  "src\boards\BoardCapabilities.cpp",
  "src\transport\mock\MockTransport.cpp",
  "src\transport\ble\BleTransport.cpp",
  "src\transport\bt_classic\BluetoothClassicTransport.cpp",
  "src\transport\bt_classic\BtClassicMockTransport.cpp",
  "src\protocols\base\ProtocolFingerprint.cpp",
  "src\protocols\tiny_iprint\TinyProtocol.cpp",
  "src\printers\catalog\BuiltinProfiles.cpp",
  "src\printers\catalog\DeviceCatalog.cpp",
  "src\bitmap\BitmapPipeline.cpp",
  "src\app\PrintQueue.cpp",
  "src\app\Discovery.cpp",
  "src\app\Base64.cpp",
  "src\app\JsonMini.cpp",
  "src\app\PrintRequestParser.cpp",
  "src\app\PrintSubmission.cpp",
  "src\app\RawPrintAssembler.cpp",
  "src\app\SafeTestRegistry.cpp",
  "src\app\SerialControl.cpp",
  "src\app\CompatibilityReport.cpp",
  "src\app\OtaService.cpp",
  "src\app\WifiService.cpp",
  "src\app\BridgeApp.cpp",
  "src\app\HttpDebugApi.cpp",
  "src\runtime\PrintSession.cpp"
) | ForEach-Object { Join-Path $Mirror $_ }

$catalogSources = @(
  "src\printers\catalog\BuiltinProfiles.cpp",
  "src\printers\catalog\DeviceCatalog.cpp",
  "src\protocols\base\ProtocolFingerprint.cpp"
) | ForEach-Object { Join-Path $Mirror $_ }

$testMain = Join-Path $Mirror "tests\host\test_main.cpp"
$catalogDump = Join-Path $Mirror "tests\host\catalog_dump.cpp"
$asanCanary = Join-Path $Mirror "tests\host\asan_canary.cpp"
$ubsanCanary = Join-Path $Mirror "tests\host\ubsan_canary.cpp"
$include = "-I" + (Join-Path $Mirror "src")

$asanLibFlags = @(
  "-L$AsanLibDir",
  "-lclang_rt.asan_dynamic-x86_64",
  "-lclang_rt.asan_dynamic_runtime_thunk-x86_64"
)
$asanFlags = @("-fsanitize=address", "-fno-omit-frame-pointer", "-g") + $asanLibFlags
$ubsanFlags = @("-fsanitize=undefined", "-fno-omit-frame-pointer", "-g")
# Zig/Clang need the sanitizer set in ONE flag; two -fsanitize flags override.
$comboFlags = @("-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g") + $asanLibFlags

function Compile-Host([string[]]$Defines, [string[]]$Sources, [string]$Output,
                      [string[]]$ExtraFlags = @()) {
  $argv = @("c++", "-std=c++11", "-O0", "-DRTB_HOST_TEST=1", $include) +
          $Defines + $ExtraFlags + $Sources + @("-o", $Output)
  $compileOut = & $zig @argv 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    Write-Host $compileOut
    Write-Error "compile failed: $Output"
    exit 1
  }
}

$s3exe = Join-Path $outDir "mock_tests_s3.exe"
$wroomexe = Join-Path $outDir "mock_tests_wroom32e.exe"
$dumpexe = Join-Path $outDir "catalog_dump.exe"
$asanexe = Join-Path $outDir "mock_tests_asan.exe"
$ubsanexe = Join-Path $outDir "mock_tests_ubsan.exe"
$comboexe = Join-Path $outDir "mock_tests_asan_ubsan.exe"
$canaryexe = Join-Path $outDir "asan_canary.exe"
$ubsanCanaryExe = Join-Path $outDir "ubsan_canary.exe"
$logDir = Join-Path $root "build_logs"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null

Write-Host "=== COMPILE host tests (S3 capability emulation) ==="
Compile-Host @() ($commonSources + @($testMain)) $s3exe
Write-Host "=== COMPILE host tests (WROOM-32E capability emulation) ==="
Compile-Host @("-DRTB_HOST_EMULATE_WROOM32E=1") ($commonSources + @($testMain)) $wroomexe
Write-Host "=== COMPILE catalog dump ==="
Compile-Host @() ($catalogSources + @($catalogDump)) $dumpexe
Write-Host "=== COMPILE ASAN regression suite ==="
Compile-Host @() ($commonSources + @($testMain)) $asanexe $asanFlags
Write-Host "=== COMPILE ASAN canary (deliberate stack-use-after-scope) ==="
Compile-Host @() @($asanCanary) $canaryexe $asanFlags
Write-Host "=== COMPILE UBSAN regression suite ==="
Compile-Host @() ($commonSources + @($testMain)) $ubsanexe $ubsanFlags
Write-Host "=== COMPILE UBSAN canary (deliberate signed overflow) ==="
Compile-Host @() @($ubsanCanary) $ubsanCanaryExe $ubsanFlags
Write-Host "=== COMPILE ASAN+UBSAN combined regression suite ==="
Compile-Host @() ($commonSources + @($testMain)) $comboexe $comboFlags

$failures = 0
Write-Host "=== RUN S3 mock tests ==="
& $s3exe
if ($LASTEXITCODE -ne 0) { $failures++ }
Write-Host "=== RUN WROOM-32E capability mock tests ==="
& $wroomexe
if ($LASTEXITCODE -ne 0) { $failures++ }

Write-Host "=== RUN profile consistency + Classic API audit ==="
& python (Join-Path $Mirror "tests\test_profiles.py") $dumpexe
if ($LASTEXITCODE -ne 0) { $failures++ }

Write-Host "=== ASAN canary: sanitizer must REPORT a stack-use-after-scope ==="
$canaryLog = Join-Path $logDir "asan_canary.log"
$canaryOut = & $canaryexe *>&1 | Tee-Object -FilePath $canaryLog | Out-String
$canaryCode = $LASTEXITCODE
if ($canaryCode -eq 0 -or $canaryOut -notmatch "AddressSanitizer") {
  Write-Error "ASAN is NOT active (canary exit=$canaryCode); ASAN results would be meaningless"
  $failures++
} else {
  Write-Host "PASS: ASAN correctly caught the deliberate stack-use-after-scope"
}

Write-Host "=== RUN ASAN regression suite ==="
$asanLog = Join-Path $logDir "asan_regression.log"
$asanOut = & $asanexe *>&1 | Tee-Object -FilePath $asanLog | Out-String
$asanCode = $LASTEXITCODE
if ($asanCode -ne 0 -or $asanOut -match "ERROR: AddressSanitizer") {
  Write-Error "ASAN regression suite failed (exit=$asanCode); see $asanLog"
  $failures++
} else {
  $summary = ($asanOut -split "`n" | Where-Object { $_ -match "checks, .* failures" } | Select-Object -Last 1)
  Write-Host ("PASS: ASAN regression suite clean" + $(if ($summary) { " - $($summary.Trim())" } else { "" }))
}

Write-Host "=== UBSAN canary: sanitizer must REPORT a signed overflow ==="
$ubsanCanaryLog = Join-Path $logDir "ubsan_canary.log"
$ubsanCanaryOut = & $ubsanCanaryExe *>&1 | Tee-Object -FilePath $ubsanCanaryLog | Out-String
if ($ubsanCanaryOut -notmatch "signed integer overflow") {
  Write-Error "UBSAN is NOT active; UBSAN results would be meaningless"
  $failures++
} else {
  Write-Host "PASS: UBSAN correctly caught the deliberate signed overflow"
}

$sanitizerError = "ERROR: AddressSanitizer|runtime error:|panic:"
function Run-SanitizerSuite([string]$Exe, [string]$LogName, [string]$Label) {
  $log = Join-Path $logDir $LogName
  $out = & $Exe *>&1 | Tee-Object -FilePath $log | Out-String
  $code = $LASTEXITCODE
  if ($code -ne 0 -or $out -match $sanitizerError) {
    Write-Error "$Label failed (exit=$code); see $log"
    return $false
  }
  $summary = ($out -split "`n" | Where-Object { $_ -match "checks, .* failures" } | Select-Object -Last 1)
  Write-Host ("PASS: $Label clean" + $(if ($summary) { " - $($summary.Trim())" } else { "" }))
  return $true
}

Write-Host "=== RUN UBSAN regression suite ==="
if (-not (Run-SanitizerSuite $ubsanexe "ubsan_regression.log" "UBSAN regression suite")) { $failures++ }
Write-Host "=== RUN ASAN+UBSAN combined regression suite ==="
if (-not (Run-SanitizerSuite $comboexe "asan_ubsan_regression.log" "ASAN+UBSAN combined suite")) { $failures++ }

Write-Host ("=== MOCK TEST SUMMARY: {0} failing suites ===" -f $failures)
if ($failures -gt 0) { exit 1 }
exit 0
