param([string]$Port='COM6',[ValidateRange(-15,15)][double]$Iq=1,[string]$Log='build/pi_baseline.log',[ValidateRange(200,30000)][int]$DurationMs=2500,[switch]$Transitions,[switch]$Temperature,[ValidateSet('keep','on','off')][string]$H2='keep')
$ErrorActionPreference='Stop'
if([double]::IsNaN($Iq) -or [double]::IsInfinity($Iq)){throw 'Iq must be finite'}
if($Transitions -and $DurationMs -gt 9000){throw 'Three-stage run must stay below about 30 seconds'}
if($Transitions -and $Iq -lt 0){throw 'Negative-current tests require separate stopped runs'}
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
  if($Temperature){
    $temp=Cmd 'ntc' 250; Write-Output $temp; $null=Cmd 'ntc stop'
    if($temp -match 'T=([-\d.]+) C' -and [double]$Matches[1] -ge 50){throw 'Host NTC threshold 50 C before run'}
  }
  if($H2 -ne 'keep') {
    $null=Cmd 'foc h2 gain +1'; $h2status=Cmd "foc h2 $H2"; Write-Output $h2status
    if($h2status -notmatch "FOC h2: $H2, gain=1, valid=1"){throw 'H2 state rejected'}
  }
  $set=Cmd ('foc current 0 '+$Iq.ToString([Globalization.CultureInfo]::InvariantCulture)); Write-Output $set
  if($set -notmatch 'FOC current set:'){throw 'Current command rejected'}
  $runTimer=[Diagnostics.Stopwatch]::StartNew()
  $start=Cmd 'foc start' 150; Write-Output $start
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
      $status=Cmd 'foc status' 80
      # Status is emitted across main-loop iterations; UART may split it.
      for($retry=0;$retry -lt 4 -and $status -notmatch 'FOC PI: integral=[^\r\n]*saturated=\d+';$retry++) {
        if($status -match 'FOC: off' -or $status -match 'FOC stopped:'){break}
        $status+=Receive 80
      }
      Write-Output (Show $status)
      if($status -notmatch 'FOC: running' -or $status -notmatch 'fault=none'){throw 'FOC stopped or faulted'}
      if($status -match 'rpm=(-?\d+)' -and [math]::Abs([int]$Matches[1]) -gt 450){throw 'Host speed threshold 450 rpm'}
      $peakLimit=[math]::Max(15000,([math]::Abs($Iq)+2)*1000)
      if($status -match 'peak=(\d+) mA' -and [int]$Matches[1] -gt $peakLimit){throw "Host phase peak threshold $peakLimit mA"}
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
