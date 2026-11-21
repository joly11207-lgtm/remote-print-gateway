# Acceptance driver for POST /api/print/raw (X18 over BLE/Tiny iPrint).
#
#   -Stage smoke  : connect the printer, send ONE raw bitmap
#   -Stage a      : 10 consecutive raw prints
#   -Stage b      : idle 60 s then 300 s, one print each
#   -Stage d      : oversized / malformed / big-but-legal requests
#
# The device serial log is the authority on memory ([rawprint] / [HEAP] lines,
# RTB_DEBUG_HEAP=1 on wroom32e_future). Attach a serial reader while running.
param(
  [string]$DeviceIp = '192.168.4.1',
  [ValidateSet('smoke', 'a', 'b', 'd', 'connect')][string]$Stage = 'smoke',
  [int]$Count = 10,
  [int[]]$Idles = @(60, 300),
  [int]$Rows = 64,
  [int]$RowBytes = 48,
  [string]$Address = 'e3:47:88:2e:e1:2c',
  [string]$Model = 'x18_aijixing'
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Net.Http | Out-Null

$base = "http://${DeviceIp}"
$http = New-Object System.Net.Http.HttpClient
$http.Timeout = [TimeSpan]::FromSeconds(120)

function New-Frame([int]$rb, [int]$rows) {
  $b = New-Object byte[] ($rb * $rows)
  for ($r = 0; $r -lt $rows; $r++) {
    $b[$r * $rb] = 0x80
    $b[$r * $rb + $rb - 1] = 0x01
  }
  $mid = [int]($rows / 2)
  for ($c = 0; $c -lt $rb; $c++) { $b[$mid * $rb + $c] = 0xFF }
  return , $b
}

function Get-Status {
  $text = (Invoke-WebRequest -Uri "$base/api/status" -UseBasicParsing -TimeoutSec 10).Content
  return $text | ConvertFrom-Json
}

function Send-Raw([string]$jobId, [byte[]]$body, [int]$rb, [int]$feed, [int]$width) {
  $content = [System.Net.Http.ByteArrayContent]::new($body, 0, $body.Length)
  $content.Headers.ContentType =
      [System.Net.Http.Headers.MediaTypeHeaderValue]::Parse('application/octet-stream')
  $content.Headers.Add('X-RTB-Job-Id', $jobId)
  $content.Headers.Add('X-RTB-Row-Bytes', "$rb")
  if ($feed -ge 0) { $content.Headers.Add('X-RTB-Feed-Dots', "$feed") }
  if ($width -gt 0) { $content.Headers.Add('X-RTB-Width', "$width") }

  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  $code = -1
  $text = ''
  try {
    $response = $http.PostAsync("$base/api/print/raw", $content).GetAwaiter().GetResult()
    $code = [int]$response.StatusCode
    $text = $response.Content.ReadAsStringAsync().GetAwaiter().GetResult()
    $response.Dispose()
  } catch {
    $text = 'ERROR: ' + $_.Exception.Message
  }
  $sw.Stop()
  $content.Dispose()
  return [pscustomobject]@{ Job = $jobId; Code = $code; Ms = $sw.ElapsedMilliseconds; Body = $text }
}

function Wait-JobDone([int]$doneBefore, [int]$uncertainBefore, [int]$timeoutSec) {
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  while ($sw.Elapsed.TotalSeconds -lt $timeoutSec) {
    $s = Get-Status
    if ($s.done_jobs -gt $doneBefore -or $s.uncertain_jobs -gt $uncertainBefore) {
      return $s
    }
    Start-Sleep -Milliseconds 400
  }
  return $null
}

function Connect-Printer {
  $payload = "{`"address`":`"$Address`",`"model_id`":`"$Model`"}"
  $content = New-Object System.Net.Http.StringContent($payload, [System.Text.Encoding]::UTF8, 'application/json')
  $response = $http.PostAsync("$base/api/connect", $content).GetAwaiter().GetResult()
  $text = $response.Content.ReadAsStringAsync().GetAwaiter().GetResult()
  $code = [int]$response.StatusCode
  $response.Dispose(); $content.Dispose()
  Write-Host "connect -> $code $text"
  return $text
}

function Print-One([string]$jobId, [byte[]]$frame, [int]$rows, [int]$rb, [int]$timeout = 45) {
  $before = Get-Status
  $r = Send-Raw $jobId $frame $rb -1 384
  Write-Host ("  {0}: http={1} {2}ms {3}" -f $r.Job, $r.Code, $r.Ms, $r.Body)
  if ($r.Code -ne 200) { return $null }
  $after = Wait-JobDone $before.done_jobs $before.uncertain_jobs $timeout
  if ($null -eq $after) {
    Write-Host "  $jobId -> TIMEOUT waiting for job_done"
    return $null
  }
  $delta = $after.done_jobs - $before.done_jobs
  $unc = $after.uncertain_jobs - $before.uncertain_jobs
  Write-Host ("  {0} -> done_delta={1} uncertain_delta={2} state={3} attempts_ok" -f $jobId, $delta, $unc, $after.printer_state)
  return [pscustomobject]@{ Job = $jobId; Done = $delta; Uncertain = $unc; Status = $after }
}

$frame = New-Frame $RowBytes $Rows
Write-Host ("frame: {0} bytes ({1} x {2})" -f $frame.Length, $RowBytes, $Rows)

switch ($Stage) {
  'connect' {
    Connect-Printer | Out-Null
  }

  'smoke' {
    Connect-Printer | Out-Null
    Print-One 'RAW-SMOKE-1' $frame $Rows $RowBytes 60 | Out-Null
  }

  'a' {
    Connect-Printer | Out-Null
    $ok = 0
    $fail = 0
    for ($i = 1; $i -le $Count; $i++) {
      $jobId = ('RAW-A-{0:D2}' -f $i)
      $result = Print-One $jobId $frame $Rows $RowBytes 60
      if ($null -ne $result -and $result.Done -eq 1 -and $result.Uncertain -eq 0) { $ok++ } else { $fail++ }
    }
    Write-Host ""
    Write-Host ("=== A: {0}/{1} printed, {2} failed ===" -f $ok, $Count, $fail)
    if ($fail -gt 0) { exit 1 }
  }

  'b' {
    Connect-Printer | Out-Null
    foreach ($idle in $Idles) {
      Write-Host "idle $idle s ..."
      Start-Sleep -Seconds $idle
      $jobId = "RAW-B-$idle"
      Print-One $jobId $frame $Rows $RowBytes 90 | Out-Null
    }
  }

  'd' {
    Connect-Printer | Out-Null
    $before = Get-Status

    Write-Host 'd1: 64 KB + 48 (over RTB_MAX_JOB_BITMAP_BYTES) -> expect 413'
    $tooBig = New-Object byte[] (64 * 1024 + 48)
    Send-Raw 'RAW-D-TOOBIG' $tooBig $RowBytes -1 384 | Format-List | Out-String | Write-Host

    Write-Host 'd2: 65536 bytes with row_bytes=1024 (invalid row_bytes) -> expect 400'
    Send-Raw 'RAW-D-BADROW' (New-Object byte[] 65536) 1024 -1 384 | Format-List | Out-String | Write-Host

    Write-Host 'd3: 48*64+1 bytes (not row aligned) -> expect 400'
    Send-Raw 'RAW-D-UNALIGNED' (New-Object byte[] (48 * 64 + 1)) $RowBytes -1 384 | Format-List | Out-String | Write-Host

    Write-Host 'd4: 60 KB legal body -> expect 200 + one job'
    $big = New-Object byte[] (61440)  # 1280 rows x 48
    $bigResult = Send-Raw 'RAW-D-60K' $big $RowBytes -1 384
    Write-Host ("  http={0} {1}" -f $bigResult.Code, $bigResult.Body)
    $after = Wait-JobDone $before.done_jobs $before.uncertain_jobs 120
    if ($after) { Write-Host ("  done_jobs {0} -> {1}, state={2}" -f $before.done_jobs, $after.done_jobs, $after.printer_state) }
    else { Write-Host '  TIMEOUT' }

    Write-Host 'd5: normal 3 KB print after the failures -> must still work'
    Print-One 'RAW-D-RECOVER' $frame $Rows $RowBytes 60 | Out-Null
  }
}

$http.Dispose()
