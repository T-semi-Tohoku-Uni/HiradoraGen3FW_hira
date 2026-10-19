param(
  [ValidateSet('Calibration','Voltage','Current')][string]$Mode='Calibration',
  [ValidateRange(-0.2,0.2)][double]$Value=0.05,
  [ValidateRange(200,2000)][int]$DurationMs=500,
  [string]$Log='build/as5600_energized_20261020/calibration.log'
)
# Explicitly authorized energized tests for the unloaded 360 KV motor.
# Firmware phase trip=10 A, speed trip=300 rpm; host abort=200 rpm / 3 A.
$ErrorActionPreference='Stop'
$p=[IO.Ports.SerialPort]::new('COM6',921600,[IO.Ports.Parity]::None,8,[IO.Ports.StopBits]::One)
$p.ReadBufferSize=262144; $p.WriteTimeout=500
$writer=[IO.StreamWriter]::new((Join-Path (Get-Location) $Log),$false)
$events=[IO.StreamWriter]::new((Join-Path (Get-Location) ($Log+'.commands.log')),$false)
function Receive([int]$ms) {
  $b=[Text.StringBuilder]::new(); $timer=[Diagnostics.Stopwatch]::StartNew()
  while ($timer.ElapsedMilliseconds -lt $ms) {
    $s=$p.ReadExisting(); if ($s) { [void]$b.Append($s); $writer.Write($s) }
    Start-Sleep -Milliseconds 2
  }
  $writer.Flush(); return $b.ToString()
}
function Cmd([string]$s,[int]$ms=150) {
  $events.WriteLine(('{0:o} {1}' -f [DateTime]::UtcNow,$s)); $events.Flush()
  $p.Write($s+"`n"); return Receive $ms
}
function Show([string]$s) { ($s -split "`n" | Where-Object {$_ -match '^(Calibration|Cal |Manual|FOC|AS5600|PWM|VM|Serial)'}) -join "`n" }
try {
  $p.Open(); $null=Cmd 'stop'; $null=Cmd 'adc stop' 400; $null=Cmd 'angle stop'
  $vm=Cmd 'vm'; Write-Output $vm
  if ($vm -notmatch 'VM=([\d.]+) V' -or [double]$Matches[1] -lt 20 -or [double]$Matches[1] -gt 26) { throw 'Unexpected bus voltage' }
  $angle=Cmd 'angle status' 300; Write-Output $angle
  if ($angle -notmatch 'AS5600 OK' -or $angle -notmatch 'i2c=0, sensor=0, timeout=0,') { throw 'Encoder not clean' }
  if ($Mode -eq 'Calibration') {
    $start=Cmd 'cal start' 150; Write-Output $start
    if ($start -notmatch 'Manual calibration started:') { throw 'Calibration start rejected' }
    $timer=[Diagnostics.Stopwatch]::StartNew(); $done=$false
    while ($timer.ElapsedMilliseconds -lt 30000) {
      $s=Cmd 'cal status' 150
      Write-Output (Show $s)
      if ($s -match 'Calibration: VALID, stage=0,' -and $s -match 'fault=none') { $done=$true; break }
      if ($s -match 'Calibration: UNCALIBRATED, stage=0,' -or $s -match 'Calibration trip:') { throw 'Calibration failed' }
      if ($s -match 'peak=([\d.]+) A' -and [double]$Matches[1] -gt 3.0) { throw 'Host calibration phase peak limit 3 A' }
    }
    if (!$done) { throw 'Calibration timeout' }
    $null=Cmd 'adc stop' 500
    $saved=Cmd 'cal save' 800; Write-Output $saved
    if ($saved -notmatch 'Calibration flash saved and verified') { throw 'Calibration save failed' }
  } else {
    $cal=Cmd 'cal status'; Write-Output $cal
    if ($cal -notmatch 'Calibration: VALID') { throw 'Valid calibration required' }
    $null=Cmd 'foc h2 off'; $null=Receive 500
    $set=Cmd ('foc '+$Mode.ToLowerInvariant()+' 0 '+$Value.ToString([Globalization.CultureInfo]::InvariantCulture))
    Write-Output $set
    if ($set -notmatch ('FOC '+$Mode.ToLowerInvariant()+' set:')) { throw 'FOC command rejected' }
    $timer=[Diagnostics.Stopwatch]::StartNew()
    $start=Cmd 'foc start' 80; Write-Output $start
    if ($start -notmatch ('FOC '+$Mode.ToLowerInvariant()+' start:')) { throw 'FOC start rejected' }
    while ($timer.ElapsedMilliseconds -lt $DurationMs) {
      $s=Cmd 'foc status' 70; Write-Output (Show $s)
      if ($s -notmatch 'FOC: running' -or $s -notmatch 'fault=none') { throw 'FOC stopped or faulted' }
      if ($s -notmatch 'rpm=(-?\d+)') { throw 'No speed report' }
      if ([math]::Abs([int]$Matches[1]) -gt 200) { throw 'Host speed limit 200 rpm' }
      if ($s -notmatch 'peak=(\d+) mA') { throw 'No current report' }
      if ([int]$Matches[1] -gt 3000) { throw 'Host phase peak limit 3 A' }
    }
  }
} finally {
  if ($p.IsOpen) { try {
    $null=Cmd 'stop'; $null=Cmd 'adc stop' 400
    foreach ($s in @('status','cal status','foc status','angle status','serial status','vm')) { Write-Output (Show (Cmd $s 200)) }
  } finally { $p.Close() } }
  $p.Dispose(); $writer.Dispose(); $events.Dispose()
}
