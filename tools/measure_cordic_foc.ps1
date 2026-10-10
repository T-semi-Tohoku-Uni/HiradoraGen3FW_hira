$ErrorActionPreference='Stop'
$stamp=Get-Date -Format 'yyyyMMdd_HHmmss'
$base=Join-Path (Get-Location) "build/cordic_foc_$stamp"
$p=[IO.Ports.SerialPort]::new('COM6',921600,[IO.Ports.Parity]::None,8,[IO.Ports.StopBits]::One)
$p.ReadBufferSize=262144; $p.WriteTimeout=1000
$raw=[IO.StreamWriter]::new($base+'.log',$false)
$events=[IO.StreamWriter]::new($base+'_commands.log',$false)
$rows=[Collections.Generic.List[object]]::new()
function Receive([int]$ms) {
 $b=[Text.StringBuilder]::new(); $watch=[Diagnostics.Stopwatch]::StartNew()
 while($watch.ElapsedMilliseconds -lt $ms) {
  $s=$p.ReadExisting()
  if($s){[void]$b.Append($s);$raw.Write($s)}
  Start-Sleep -Milliseconds 2
 }
 $raw.Flush(); return $b.ToString()
}
function Cmd([string]$command,[int]$ms=300) {
 $events.WriteLine("$([DateTimeOffset]::Now.ToString('o')) $command");$events.Flush()
 $p.Write($command+"`n"); Receive $ms
}
try {
 $p.Open()
 foreach($command in @('stop','adc stop','angle stop','ntc stop')){$null=Cmd $command}
 $vm=Cmd 'vm'; if($vm -notmatch 'VM=([\d.]+) V' -or [double]$Matches[1] -lt 20 -or [double]$Matches[1] -gt 26){throw "VM: $vm"}
 $cal=Cmd 'cal status'; if($cal -notmatch 'Calibration: VALID'){throw $cal}
 $temp=Cmd 'ntc' 1200; $null=Cmd 'ntc stop'
 if($temp -notmatch 'T=([-\d.]+) C' -or [double]$Matches[1] -ge 50){throw "Temperature: $temp"}
 Write-Output $temp
 foreach($mode in @('current')) {
  foreach($h2 in @('off','on')) {
   $null=Cmd 'stop' 1000
   $enc=Cmd 'angle status'; Write-Output $enc
   if($enc -notmatch 'AS5047P OK' -or $enc -notmatch 'Encoder errors: spi=0, parity=0, sensor=0, timeout=0,'){throw 'Encoder not clean'}
   $null=Cmd 'foc h2 gain 1'; $null=Cmd "foc h2 $h2"
   $target=if($mode -eq 'voltage'){'0.3'}else{'0.5'}
   $set=Cmd "foc $mode 0 $target"
   if($set -notmatch "FOC $mode set:"){throw $set}
   $start=Cmd 'foc start' 200
   if($start -notmatch "FOC $mode start:"){throw $start}
   Write-Output "RUN mode=$mode h2=$h2 target=$target"
   $polls=50
   for($i=0;$i -lt $polls;$i++) {
    $null=Receive 50
    $s=Cmd 'foc status' 150
    if($s -notmatch 'FOC: running' -or $s -notmatch 'fault=none'){throw $s}
    if($s -notmatch 'rpm=(-?\d+)' -or [math]::Abs([int]$Matches[1]) -gt 200){throw "Speed: $s"}
    if($s -notmatch 'peak=(\d+) mA' -or [int]$Matches[1] -gt 5000){throw "Current: $s"}
    if($s -notmatch 'ticks=(\d+), compute_max=(\d+) ns, adc_control_max=(\d+) ns, angle_age_max=(\d+) ns'){throw "Timing: $s"}
    $rows.Add([pscustomobject]@{mode=$mode;h2=$h2;target=$target;poll=$i+1;ticks=[long]$Matches[1];compute_max_ns=[long]$Matches[2];adc_control_max_ns=[long]$Matches[3];angle_age_max_ns=[long]$Matches[4]})
    if($i -eq ($polls-1)){Write-Output $s}
   }
   $null=Cmd 'stop' 500
   Write-Output (Cmd 'angle status')
   Write-Output (Cmd 'adc status')
   $temp=Cmd 'ntc' 1200; $null=Cmd 'ntc stop'; Write-Output $temp
   if($temp -notmatch 'T=([-\d.]+) C' -or [double]$Matches[1] -ge 50){throw "Temperature: $temp"}
  }
 }
} finally {
 if($p.IsOpen){try {
  $null=Cmd 'stop' 500; $null=Cmd 'adc stop'; $null=Cmd 'foc h2 off'
  foreach($command in @('foc status','status','angle status','serial status','vm')){Write-Output (Cmd $command)}
 } finally {$p.Close()}}
 $p.Dispose();$raw.Dispose();$events.Dispose()
 $rows | ConvertTo-Json | Set-Content -Encoding utf8 ($base+'.json')
 Write-Output "ARTIFACT_BASE=$base"
}


