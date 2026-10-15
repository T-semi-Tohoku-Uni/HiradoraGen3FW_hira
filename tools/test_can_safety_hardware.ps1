param([string]$Log='build/can_safety_hardware.log')
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$u=[IO.Ports.SerialPort]::new('COM6',921600,[IO.Ports.Parity]::None,8,[IO.Ports.StopBits]::One)
$a=[IO.Ports.SerialPort]::new('COM13',115200,[IO.Ports.Parity]::None,8,[IO.Ports.StopBits]::One)
$u.WriteTimeout=1000;$a.WriteTimeout=1000;$a.ReadBufferSize=262144
$writer=[IO.StreamWriter]::new((Join-Path (Get-Location) $Log),$false)
function Pump([int]$ms,[string]$frame='') {
  $clock=[Diagnostics.Stopwatch]::StartNew();$next=0;$text=[Text.StringBuilder]::new()
  while($clock.ElapsedMilliseconds -lt $ms) {
    if($frame -and $clock.ElapsedMilliseconds -ge $next){$a.Write($frame+"`r");$next=$clock.ElapsedMilliseconds+10}
    [void]$text.Append($u.ReadExisting());$null=$a.ReadExisting();Start-Sleep -Milliseconds 1
  }
  return $text.ToString()
}
function Note([string]$text){$writer.WriteLine($text);$writer.Flush();Write-Output $text}
function Status([string]$label,[string]$frame='') {
  $u.Write("can status`n");$text=Pump 150 $frame;Note "$label`n$text"
  return $text
}
function Check([string]$text,[string]$pattern){if($text -notmatch $pattern){throw "Missing $pattern"}}
$zero='t20080000000000000000';$nonzero='t20084000000000000000'
try {
  $u.Open();$a.Open();$u.DiscardInBuffer()
  $u.Write("stop`ncan disarm`ncan monitor off`n");Note (Pump 200)
  $u.Write("status`n");$text=Pump 100;Note $text;Check $text 'PWM: stopped, mode=off'
  $a.Write("C`rS8`rO`r");$null=Pump 200
  $u.Write("can arm`n");Note (Pump 100)
  $text=Status 'NO FRAME TIMEOUT';Check ($text -join "`n") 'state=3 reason=1 raw=0'
  $null=Pump 150 $zero
  $text=Status 'NO AUTO REARM' $zero;Check ($text -join "`n") 'state=3 reason=1 raw=0'
  $u.Write("can disarm`ncan arm`n");Note (Pump 200 $nonzero)
  $text=Status 'NONZERO CANNOT HANDSHAKE' $nonzero;Check ($text -join "`n") 'state=3 reason=1 raw=0'
  $u.Write("can disarm`ncan arm`n");Note (Pump 200 $zero)
  $text=Status 'NEW ZERO READY' $zero;Check ($text -join "`n") 'state=2 reason=0 raw=0'
  # Setting only: even a broken guard cannot start PWM with this command.
  $u.Write("foc current 0 1`n");$text=Pump 150 $zero;Note $text;Check $text 'UART control blocked'
  $null=Pump 200 $nonzero
  $text=Status 'DISPLAY ONLY NONZERO' $nonzero;Check ($text -join "`n") 'state=2 reason=0 raw=16384'
  $null=Pump 100
  $text=Status 'STREAM TIMEOUT';Check ($text -join "`n") 'state=3 reason=1 raw=0'
  $u.Write("stop`n");Note (Pump 100)
  $text=Status 'STOP RELEASES OWNER';Check ($text -join "`n") 'state=0 reason=0 raw=0'
  $u.Write("status`nfoc status`n");$text=Pump 300;Note $text;Check $text 'PWM: stopped, mode=off'
  Note 'PASS: arm/zero/timeout/lockout/UART guard; no PWM start command sent'
} finally {
  if($u.IsOpen){$u.Write("can disarm`nstop`n");$null=Pump 100;$u.Close()}
  if($a.IsOpen){$a.Close()};$u.Dispose();$a.Dispose();$writer.Dispose()
}
