param([string]$Port='COM6',[ValidateSet('on','off')][string]$H2='on',
  [ValidateRange(0.1,0.8)][double]$Vq=0.5,
  [ValidateRange(500,29500)][int]$DurationMs=29500,
  [string]$Log='build/h2_voltage.log')
$ErrorActionPreference='Stop'
$p=[IO.Ports.SerialPort]::new($Port,921600,[IO.Ports.Parity]::None,8,[IO.Ports.StopBits]::One)
$p.ReadBufferSize=262144; $p.WriteTimeout=1000
$writer=[IO.StreamWriter]::new((Join-Path (Get-Location) $Log),$false)
function Receive([int]$ms) {
  $b=[Text.StringBuilder]::new(); $timer=[Diagnostics.Stopwatch]::StartNew()
  while($timer.ElapsedMilliseconds -lt $ms) {
    $s=$p.ReadExisting(); if($s){[void]$b.Append($s);$writer.Write($s)}
    Start-Sleep -Milliseconds 2
  }
  $writer.Flush(); return $b.ToString()
}
function Cmd([string]$s,[int]$ms=150){$p.Write($s+"`n");Receive $ms}
try {
  $p.Open(); $null=Cmd 'stop'; $null=Cmd 'adc stop' 600
  Write-Output (Cmd 'cal status'); Write-Output (Cmd 'vm')
  Write-Output (Cmd 'ntc' 250); $null=Cmd 'ntc stop'
  $null=Cmd 'foc h2 gain +1'; Write-Output (Cmd "foc h2 $H2")
  $set=Cmd ('foc voltage 0 '+$Vq.ToString([Globalization.CultureInfo]::InvariantCulture))
  if($set -notmatch 'FOC voltage set:'){throw "Voltage rejected: $set"}
  $run=[Diagnostics.Stopwatch]::StartNew()
  $start=Cmd 'foc start' 40; Write-Output $start
  if($start -notmatch 'FOC voltage start:'){throw 'Start rejected'}
  $null=Cmd 'adc 200' 30
  $nextReport=0
  while($run.ElapsedMilliseconds -lt $DurationMs) {
    $null=Receive 150
    $status=Cmd 'foc status' 80
    if($status -notmatch 'FOC: running' -or $status -notmatch 'fault=none'){throw "Stopped/faulted: $status"}
    if($status -match 'rpm=(-?\d+)' -and [math]::Abs([int]$Matches[1]) -gt 400){throw 'Host speed limit 400 rpm'}
    if($status -match 'peak=(\d+) mA' -and [int]$Matches[1] -gt 10000){throw 'Host peak limit 10 A'}
    if($run.ElapsedMilliseconds -ge $nextReport) {
      Write-Output (($status -split "`n" | Where-Object {$_ -match '^FOC'}) -join "`n")
      $nextReport+=5000
    }
  }
} finally {
  if($p.IsOpen){try {
    if($run){Write-Output "Stop requested at $($run.ElapsedMilliseconds) ms"}
    $null=Cmd 'stop'; $null=Cmd 'adc stop' 500
    Write-Output (Cmd 'foc status'); Write-Output (Cmd 'ntc' 250); $null=Cmd 'ntc stop'
    Write-Output (Cmd 'status'); Write-Output (Cmd 'adc status'); Write-Output (Cmd 'serial status')
    Write-Output (Cmd 'angle status'); Write-Output (Cmd 'vm')
    Write-Output (Cmd 'cal status'); $null=Cmd 'foc h2 off'
  } finally {$p.Close()}}
  $p.Dispose();$writer.Dispose()
}
