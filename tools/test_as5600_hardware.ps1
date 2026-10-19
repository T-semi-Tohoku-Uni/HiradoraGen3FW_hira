param(
  [string]$Port = 'COM6',
  [ValidateRange(5, 600)][int]$Seconds = 30,
  [string]$Folder = 'build/as5600_hw_20261019'
)
# Observation only: never sends a PWM, calibration or FOC start command.
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $Folder | Out-Null
$p = [IO.Ports.SerialPort]::new($Port, 921600, [IO.Ports.Parity]::None, 8, [IO.Ports.StopBits]::One)
$p.ReadBufferSize = 262144
$p.WriteTimeout = 1000
$writer = [IO.StreamWriter]::new((Join-Path (Resolve-Path $Folder) 'angle.log'), $false)
$commands = [IO.StreamWriter]::new((Join-Path (Resolve-Path $Folder) 'commands.log'), $false)
$record = [Text.StringBuilder]::new()
function Receive([int]$ms) {
  $b = [Text.StringBuilder]::new()
  $timer = [Diagnostics.Stopwatch]::StartNew()
  while ($timer.ElapsedMilliseconds -lt $ms) {
    $s = $p.ReadExisting()
    if ($s) { [void]$b.Append($s); [void]$record.Append($s); $writer.Write($s) }
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
try {
  $p.Open()
  $null = Receive 100
  $null = Cmd 'stop'
  $null = Cmd 'angle stop'
  $before = Cmd 'angle status' 600
  Write-Output $before
  if ($before -notmatch 'AS5600 OK' -or $before -notmatch 'AS5600 errors: i2c=0, sensor=0, timeout=0,') {
    throw 'AS5600 acquisition is invalid or has accumulated errors'
  }
  $null = Cmd 'angle start' 100
  Write-Output "Recording angle for $Seconds seconds; PWM stays stopped."
  $null = Receive ($Seconds * 1000)
  $null = Cmd 'angle stop'
  $after = Cmd 'angle status' 600
  $motor = Cmd 'status'
  $foc = Cmd 'foc status'
  $serial = Cmd 'serial status'
  Write-Output $after
  Write-Output $motor
  Write-Output $foc
  Write-Output $serial
  if ($after -notmatch 'AS5600 OK' -or $after -notmatch 'AS5600 errors: i2c=0, sensor=0, timeout=0,' -or
      $after -notmatch 'AS5600 first fault: none' -or $motor -notmatch 'PWM: stopped' -or
      $foc -notmatch 'FOC: off' -or $serial -notmatch 'errors=0, overrun=0, queue_drops=0,') {
    throw 'Final sensor, UART or stopped-state check failed'
  }
  $samples = [regex]::Matches($record.ToString(), 'AS5600 OK raw=(\d+), mech=([\d.]+) deg, seq=(\d+), age=(\d+) ms')
  if ($samples.Count -lt $Seconds * 5) { throw 'Insufficient complete angle reports' }
  $raw = @($samples | ForEach-Object { [int]$_.Groups[1].Value })
  $seq = @($samples | ForEach-Object { [uint32]$_.Groups[3].Value })
  $age = @($samples | ForEach-Object { [int]$_.Groups[4].Value })
  if ($seq[-1] -le $seq[0] -or ($raw | Measure-Object -Maximum).Maximum -gt 4095) {
    throw 'Angle sequence stalled or raw value out of range'
  }
  $summary = [ordered]@{
    port = $Port; seconds = $Seconds; reports = $samples.Count
    raw_min = ($raw | Measure-Object -Minimum).Minimum
    raw_max = ($raw | Measure-Object -Maximum).Maximum
    sequence_first = $seq[0]; sequence_last = $seq[-1]
    report_age_max_ms = ($age | Measure-Object -Maximum).Maximum
    sensor_errors = 0; i2c_errors = 0; timeouts = 0
    pwm = 'stopped'; foc = 'off'
  }
  $summary | ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $Folder 'summary.json')
  Write-Output ($summary | ConvertTo-Json)
} finally {
  if ($p.IsOpen) {
    try { $null = Cmd 'angle stop'; $null = Cmd 'stop' } finally { $p.Close() }
  }
  $p.Dispose(); $writer.Dispose(); $commands.Dispose()
}
