param([string]$Port='COM6',[ValidateRange(0,14)][double]$Iq=1,[string]$Log='build/pi_baseline.log',[ValidateRange(200,30000)][int]$DurationMs=2500,[switch]$Transitions,[switch]$Temperature)
$ErrorActionPreference='Stop'
if([double]::IsNaN($Iq) -or [double]::IsInfinity($Iq)){throw 'Iq must be finite'}
if($Transitions -and $DurationMs -gt 9000){throw 'Three-stage run must stay below about 30 seconds'}
$p=[IO.Ports.SerialPort]::new($Port,921600,[IO.Ports.Parity]::None,8,[IO.Ports.StopBits]::One)
$p.WriteTimeout=1000
$p.ReadBufferSize=262144
$writer=[IO.StreamWriter]::new((Join-Path (Get-Location) $Log),$false)
function Receive([int]$ms){
  $b=[Text.StringBuilder]::new(); $timer=[Diagnostics.Stopwatch]::StartNew()
  while($timer.ElapsedMilliseconds -lt $ms){
    $s=$p.ReadExisting(); if($s){[void]$b.Append($s);$writer.Write($s)}
    Start-Sleep -Milliseconds 2
  }
  $writer.Flush(); return $b.ToString()
}
function Cmd([string]$s,[int]$ms=150){$writer.WriteLine("# COMMAND $s");$p.Write($s+"`n");Receive $ms}
function Show([string]$s){($s -split "`n" | Where-Object {$_ -match '^(FOC|PWM|Serial|VM|Calibration)'}) -join "`n"}
try {
  $p.Open(); $null=Cmd 'stop'; $null=Cmd 'adc stop'; $null=Receive 500
  Write-Output (Cmd 'vm'); Write-Output (Cmd 'cal status')
  if($Temperature){Write-Output (Cmd 'ntc' 250);$null=Cmd 'ntc stop'}
  $set=Cmd ('foc current 0 '+$Iq.ToString([Globalization.CultureInfo]::InvariantCulture)); Write-Output $set
  if($set -notmatch 'FOC current set:'){throw 'Current command rejected'}
  $runTimer=[Diagnostics.Stopwatch]::StartNew()
  $start=Cmd 'foc start' 30; Write-Output $start
  if($start -notmatch 'FOC current start:'){throw 'Start rejected'}
  $null=Cmd 'adc 200' 20
  $stages=@($Iq)
  if($Transitions){$stages=@($Iq,10.0,6.0)}
  for($stage=0;$stage -lt $stages.Count;$stage++) {
    if($stage -gt 0){
      $set=Cmd ('foc current 0 '+$stages[$stage].ToString([Globalization.CultureInfo]::InvariantCulture)) 30
      if($set -notmatch 'FOC current set:'){throw 'Transition command rejected'}
    }
    $timer=[Diagnostics.Stopwatch]::StartNew()
    while($timer.ElapsedMilliseconds -lt $DurationMs){
      $null=Receive 100
      $status=Cmd 'foc status' 80; Write-Output (Show $status)
      if($status -notmatch 'FOC: running' -or $status -notmatch 'fault=none'){throw 'FOC stopped or faulted'}
      if($status -match 'rpm=(-?\d+)' -and [math]::Abs([int]$Matches[1]) -gt 450){throw 'Host speed threshold 450 rpm'}
      if($status -match 'peak=(\d+) mA' -and [int]$Matches[1] -gt 15000){throw 'Host phase peak threshold 15 A'}
    }
  }
} finally {
  if($p.IsOpen){try {
    if($runTimer){$writer.WriteLine("# STOP_REQUEST elapsed_ms="+$runTimer.ElapsedMilliseconds)}
    $null=Cmd 'stop';$null=Cmd 'adc stop'
    if($Temperature){Write-Output (Cmd 'ntc' 250);$null=Cmd 'ntc stop'}
    Write-Output (Show (Cmd 'foc status'));Write-Output (Cmd 'status');Write-Output (Cmd 'serial status');Write-Output (Cmd 'vm')
  } finally {$p.Close()}}
  $p.Dispose();$writer.Dispose()
}
