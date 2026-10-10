param([string]$Port='COM6', [string]$Log='build/cordic_stopped.log', [switch]$StatusOnly)
$ErrorActionPreference='Stop'
$serial=[IO.Ports.SerialPort]::new($Port,921600,[IO.Ports.Parity]::None,8,[IO.Ports.StopBits]::One)
$serial.ReadBufferSize=262144
$serial.WriteTimeout=1000
$raw=[IO.StreamWriter]::new((Join-Path (Get-Location) $Log),$false)
function Cmd([string]$command,[int]$ms=400) {
 $serial.Write($command+"`n")
 $buffer=[Text.StringBuilder]::new()
 $watch=[Diagnostics.Stopwatch]::StartNew()
 while($watch.ElapsedMilliseconds -lt $ms) {
  $s=$serial.ReadExisting()
  if($s){[void]$buffer.Append($s);$raw.Write($s)}
  Start-Sleep -Milliseconds 2
 }
 $raw.Flush()
 return $buffer.ToString()
}
try {
 $serial.Open()
 foreach($command in @('stop','adc stop','angle stop','ntc stop')) { $null=Cmd $command }
 $status=Cmd 'status'; Write-Output $status
 if($status -notmatch 'PWM: stopped'){throw 'PWM stop not confirmed'}
 if(!$StatusOnly) {
  $test=Cmd 'cal trig test' 4000; Write-Output $test
  if($test -notmatch 'TRIG_TEST.*failures=0 ' -or $test -notmatch 'TRIG_CYCLES'){throw 'Trig test failed'}
  $caltest=Cmd 'cal test' 4000; Write-Output $caltest
  if($caltest -notmatch '659 checks, 0 failures'){throw 'Calibration selftest failed'}
 }
 foreach($command in @('cal status','foc status','angle status','serial status')) { Write-Output (Cmd $command) }
} finally {
 if($serial.IsOpen) { try {$null=Cmd 'stop';$null=Cmd 'adc stop'} finally {$serial.Close()} }
 $raw.Dispose();$serial.Dispose()
}
