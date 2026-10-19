param(
  [string]$Port = 'COM6',
  [ValidateRange(10, 600)][int]$Seconds = 60,
  [string]$Folder = 'build/as5600_foc_hw_20261020'
)
# ADC standalone acquisition: CH4 is internal; all six UVW outputs stay disabled.
# Never sends motor/PWM/FOC/calibration start or CAN arm commands.
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $Folder | Out-Null
$p = [IO.Ports.SerialPort]::new($Port, 921600, [IO.Ports.Parity]::None, 8, [IO.Ports.StopBits]::One)
$p.ReadBufferSize = 262144
$p.WriteTimeout = 1000
$writer = [IO.StreamWriter]::new((Join-Path (Resolve-Path $Folder) 'adc_angle.log'), $false)
$commands = [IO.StreamWriter]::new((Join-Path (Resolve-Path $Folder) 'commands.log'), $false)
function Receive([int]$ms) {
  $b = [Text.StringBuilder]::new()
  $timer = [Diagnostics.Stopwatch]::StartNew()
  while ($timer.ElapsedMilliseconds -lt $ms) {
    $s = $p.ReadExisting()
    if ($s) { [void]$b.Append($s); $writer.Write($s) }
    Start-Sleep -Milliseconds 5
  }
  $writer.Flush()
  return $b.ToString()
}
function Cmd([string]$command, [int]$ms = 300) {
  $commands.WriteLine(('{0:o} {1}' -f [DateTime]::UtcNow, $command))
  $commands.Flush()
  $p.Write($command + "`n")
  return Receive $ms
}
function CheckSensor([string]$s, [bool]$TimerRunning) {
  if ($s -notmatch 'AS5600 OK' -or
      $s -notmatch 'AS5600 errors: i2c=0, sensor=0, timeout=0,' -or
      $s -notmatch 'MD=1 ML=0 MH=0' -or $s -notmatch 'conf=0x0300' -or
      $s -notmatch 'AS5600 first fault: none') { throw "AS5600 check failed: $s" }
  if ($TimerRunning -and $s -notmatch 'source=TIM1') { throw 'TIM1 sampling not active' }
}
try {
  $p.Open()
  $null = Receive 200
  $null = Cmd 'stop'
  $null = Cmd 'adc stop' 600
  $null = Cmd 'angle stop'
  $motor = Cmd 'status'
  if ($motor -notmatch 'PWM: stopped') { throw 'PWM not stopped' }
  $before = Cmd 'angle status' 500
  CheckSensor $before $false
  Write-Output $before
  $null = Cmd 'angle start' 100
  $start = Cmd 'adc 200' 500
  if ($start -notmatch 'ADC DMA logger started: decimation=200, period_ns=50000') {
    throw "ADC-only start failed: $start"
  }
  $timer = [Diagnostics.Stopwatch]::StartNew()
  $firstSequence = $null
  $lastSequence = $null
  $lastTiming = $null
  while ($timer.Elapsed.TotalSeconds -lt $Seconds) {
    $null = Receive 4000
    $status = Cmd 'angle status' 400
    CheckSensor $status $true
    $angles = [regex]::Matches($status, 'AS5600 OK raw=\d+, mech=[\d.]+ deg, seq=(\d+), age=\d+ ms')
    if (!$angles.Count) { throw 'No sequence in status' }
    $sequence = [uint32]$angles[$angles.Count-1].Groups[1].Value
    if ($null -eq $firstSequence) { $firstSequence = $sequence }
    if ($null -ne $lastSequence -and $sequence -le $lastSequence) { throw 'Sequence stopped' }
    $lastSequence = $sequence
    $lastTiming = [regex]::Match($status, 'AS5600 TIM: interval_max=([\d.]+) us, age_max=([\d.]+) us')
    if (!$lastTiming.Success -or [double]$lastTiming.Groups[2].Value -gt 750.0) {
      throw "Angle age exceeds configured FOC limit: $status"
    }
    Write-Output ("{0:F1}s seq={1} interval_max={2}us age_max={3}us" -f
      $timer.Elapsed.TotalSeconds, $sequence, $lastTiming.Groups[1].Value, $lastTiming.Groups[2].Value)
  }
  $adc = Cmd 'adc status' 400
  if ($adc -notmatch 'ADC logger: streaming, overrun=0') { throw "ADC check failed: $adc" }
  $null = Cmd 'adc stop' 1000
  $null = Cmd 'angle stop'
  $null = Cmd 'stop'
  $after = Cmd 'angle status' 500
  CheckSensor $after $false
  $motor = Cmd 'status'
  $foc = Cmd 'foc status'
  $serial = Cmd 'serial status'
  Write-Output $after
  Write-Output (($adc -split "`n" | Where-Object { $_ -match '^ADC logger:' }) -join "`n")
  Write-Output $motor
  Write-Output $foc
  Write-Output $serial
  if ($motor -notmatch 'PWM: stopped' -or $foc -notmatch 'FOC: off' -or
      $serial -notmatch 'errors=0, overrun=0, queue_drops=0,') { throw 'Final stopped/UART check failed' }
  $summary = [ordered]@{
    port=$Port; requested_seconds=$Seconds; observed_seconds=$timer.Elapsed.TotalSeconds
    sequence_first=$firstSequence; sequence_last=$lastSequence
    tim_interval_max_us=[double]$lastTiming.Groups[1].Value
    tim_age_max_us=[double]$lastTiming.Groups[2].Value
    config='0x0300'; i2c_errors=0; sensor_errors=0; timeouts=0; adc_log_overruns=0
    pwm='stopped'; foc='off'; note='ADC-only TIM1 load; does not measure active FOC execution or internal sensor delay'
  }
  $summary | ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $Folder 'summary.json')
  Write-Output ($summary | ConvertTo-Json)
} finally {
  if ($p.IsOpen) {
    try { $null = Cmd 'adc stop' 600; $null = Cmd 'angle stop'; $null = Cmd 'stop' } finally { $p.Close() }
  }
  $p.Dispose(); $writer.Dispose(); $commands.Dispose()
}
