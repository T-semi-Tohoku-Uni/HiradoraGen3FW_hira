param([string]$Port='COM6',[ValidateRange(-0.4,0.4)][double]$Vq=0.1,
 [ValidateRange(500,600000)][int]$DurationMs=3000,
 [string]$Log=("build/encoder_run_{0}.log" -f (Get-Date -Format 'yyyyMMdd_HHmmss')),[switch]$StressStatus,[switch]$NoAdc,
 [ValidateRange(100,200)][int]$Decimation=200)
$ErrorActionPreference='Stop'
if([math]::Abs($Vq) -lt 0.05){throw 'Use magnitude 0.05..0.4 V'}
$p=[IO.Ports.SerialPort]::new($Port,921600,[IO.Ports.Parity]::None,8,[IO.Ports.StopBits]::One)
$p.ReadBufferSize=262144;$p.WriteTimeout=1000
$writer=[IO.StreamWriter]::new((Join-Path (Get-Location) $Log),$false)
function Receive([int]$ms){
 $b=[Text.StringBuilder]::new();$t=[Diagnostics.Stopwatch]::StartNew()
 while($t.ElapsedMilliseconds -lt $ms){
  $s=$p.ReadExisting();if($s){[void]$b.Append($s);$writer.Write($s)}
  Start-Sleep -Milliseconds 2
 }
 $writer.Flush();return $b.ToString()
}
function Cmd([string]$s,[int]$ms=200){$writer.WriteLine("# COMMAND $s");$p.Write($s+"`n");Receive $ms}
function CheckEncoder([string]$s){
 $clean='Encoder errors: spi=0, parity=0, sensor=0, timeout=0, busy_ticks=\d+, decode_waits=\d+,'
 if($s -notmatch 'AS5047P OK' -or $s -notmatch $clean){
  throw "Encoder not clean: $s"
 }
}
try {
 $p.Open();$null=Cmd 'stop';$null=Cmd 'adc stop' 400
 $vm=Cmd 'vm';Write-Output $vm
 if($vm -notmatch 'VM=([\d.]+) V' -or [double]$Matches[1] -lt 20 -or [double]$Matches[1] -gt 26){throw 'Unexpected bus voltage'}
 $cal=Cmd 'cal status';Write-Output $cal
 if($cal -notmatch 'Calibration: VALID'){throw 'Invalid calibration'}
 $enc=Cmd 'angle status';Write-Output $enc;CheckEncoder $enc
 $temp=Cmd 'ntc' 1200;Write-Output $temp;$null=Cmd 'ntc stop'
 if($temp -notmatch 'T=([-\d.]+) C'){throw 'Missing pre-run temperature status'}
 if([double]$Matches[1] -ge 50){throw 'Temperature threshold 50 C'}
 $set=Cmd ('foc voltage 0 '+$Vq.ToString([Globalization.CultureInfo]::InvariantCulture));Write-Output $set
 if($set -notmatch 'FOC voltage set:'){throw 'Voltage command rejected'}
 $run=[Diagnostics.Stopwatch]::StartNew()
 $start=Cmd 'foc start' 100;Write-Output $start
 if($start -notmatch 'FOC voltage start:'){throw 'Start rejected'}
 if(!$NoAdc){
  $adc=Cmd "adc $Decimation" 100
  if($adc -notmatch 'ADC DMA logger started:'){throw 'ADC logger did not start'}
 }
 while($run.ElapsedMilliseconds -lt $DurationMs){
  $status=Cmd 'foc status' 150
  Write-Output (($status -split "`n" | Where-Object {$_ -match '^FOC'}) -join "`n")
  if($status -notmatch 'FOC: running' -or $status -notmatch 'fault=none'){throw 'FOC stopped/faulted'}
  if($status -notmatch 'rpm=(-?\d+)'){throw 'Missing speed status'}
  if([math]::Abs([int]$Matches[1]) -gt 200){throw 'Host speed threshold 200 rpm'}
  if($status -notmatch 'peak=(\d+) mA'){throw 'Missing peak status'}
  if([int]$Matches[1] -gt 5000){throw 'Host phase peak threshold 5 A'}
  # NTC shares ADC2 and is unavailable throughout FOC, even without logging.
  # Measure before/after; keep current/speed/fault monitoring during the run.
  if($StressStatus){$enc=Cmd 'angle status' 120;CheckEncoder $enc}
  else {$null=Receive 350}
 }
} finally {
 if($p.IsOpen){try {
  $null=Cmd 'stop';$null=Cmd 'adc stop' 400
  foreach($s in @('foc status','angle status','serial status','status','vm')){Write-Output (Cmd $s 300)}
  $temp=Cmd 'ntc' 1200;Write-Output $temp;$null=Cmd 'ntc stop'
 } finally {$p.Close()}}
 $p.Dispose();$writer.Dispose()
}
