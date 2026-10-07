param([string]$Port='COM6', [int]$Seconds=600)
$ErrorActionPreference='Stop'
$stamp=Get-Date -Format 'yyyyMMdd_HHmmss'
$prefix="build/encoder_stopped_soak_$stamp"
$p=[IO.Ports.SerialPort]::new($Port,921600,[IO.Ports.Parity]::None,8,[IO.Ports.StopBits]::One)
$p.ReadBufferSize=262144; $p.WriteTimeout=1000
$writer=[IO.StreamWriter]::new((Join-Path (Get-Location) "$prefix.log"),$false)
function Receive([int]$ms) {
 $b=[Text.StringBuilder]::new(); $t=[Diagnostics.Stopwatch]::StartNew()
 while($t.ElapsedMilliseconds -lt $ms) {
  $s=$p.ReadExisting()
  if($s){[void]$b.Append($s);$writer.Write($s)}
  Start-Sleep -Milliseconds 5
 }
 $writer.Flush(); return $b.ToString()
}
function Cmd([string]$command) {
 $writer.WriteLine("# $(Get-Date -Format o) COMMAND $command")
 $p.Write($command+"`n"); Receive 500
}
function Snapshot([string]$label) {
 $writer.WriteLine("# SNAPSHOT $label")
 $status=Cmd 'foc status'; if($status -notmatch 'FOC: off'){throw "FOC not stopped: $status"}
 $enc=Cmd 'angle status'; $serial=Cmd 'serial status'; $adc=Cmd 'adc status'
 Write-Output $label
 Write-Output (("$status$enc$serial$adc" -split "`n" | Where-Object {$_ -notmatch '^>' -and $_.Trim()}) -join "`n")
 if($enc -notmatch 'AS5047P OK'){throw 'Encoder sample invalid or response missing'}
 if($enc -notmatch 'Encoder errors: spi=0, parity=0, sensor=0, timeout=0, overruns=0,'){throw 'Encoder error detected; preserving first-fault evidence'}
 if($serial -notmatch 'Serial RX: errors=0, overrun=0, queue_drops=0, long_lines=0,'){throw 'Serial error detected'}
}
try {
 $p.Open()
 Write-Output "LOG=$prefix.log"
 foreach($c in @('stop','adc stop','angle stop','ntc stop')){Write-Output (Cmd $c)}
 $status=Cmd 'status'; Write-Output $status
 if($status -notmatch 'PWM: stopped'){throw 'PWM not stopped before test'}
 Write-Output (Cmd 'vm')
 foreach($phase in @('no_adc','adc_100hz')) {
  if($phase -eq 'adc_100hz'){
   $start=Cmd 'adc 200'
   Write-Output (($start -split "`n" | Where-Object {$_ -notmatch '^>'}) -join "`n")
   if($start -notmatch 'ADC DMA logger started: decimation=200,'){throw 'ADC logging did not start'}
  }
  Snapshot "$phase start"
  $timer=[Diagnostics.Stopwatch]::StartNew(); $next=60
  while($timer.Elapsed.TotalSeconds -lt $Seconds) {
   $null=Receive 1000
   if($timer.Elapsed.TotalSeconds -ge $next) {
    Snapshot "$phase elapsed=$([int]$timer.Elapsed.TotalSeconds)s"
    $next+=60
   }
  }
  Snapshot "$phase end"
  Write-Output (Cmd 'adc stop')
 }
 Write-Output 'PASS: both stopped phases completed'
} finally {
 if($p.IsOpen){try {
  foreach($c in @('stop','adc stop','angle status','foc status','serial status','status','vm')){Write-Output (Cmd $c)}
 } finally {$p.Close()}}
 $p.Dispose();$writer.Dispose()
}
