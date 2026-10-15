param([ValidateRange(0,4096)][int]$Raw=0,[string]$Log='build/can_drive_hardware.log',[switch]$Steps,[switch]$LoadSteps)
# This script STARTS PWM. Notify the operator before running it.
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
if($Steps -and $LoadSteps){throw 'Select Steps or LoadSteps'}
$peakLimit=if($LoadSteps -or $Raw -gt 1638){7000}else{3000}
Add-Type -TypeDefinition @'
using System;
using System.IO.Ports;
using System.Threading;
using System.Diagnostics;
using System.Collections.Concurrent;
public sealed class CanTestSender {
  readonly SerialPort port;
  Thread worker;
  volatile bool stop;
  volatile string frame;
  public readonly ConcurrentQueue<string> Events = new ConcurrentQueue<string>();
  public CanTestSender(SerialPort p) { port=p; }
  public void Send(string value) {
    frame=value;
    if(worker!=null && worker.IsAlive) return;
    stop=false;
    worker=new Thread(()=>{
      var clock=Stopwatch.StartNew();
      try {
        while(!stop && clock.ElapsedMilliseconds<10000) {
          port.Write(frame+"\r");
          Events.Enqueue("TX_MS "+clock.ElapsedMilliseconds);
          Thread.Sleep(5);
        }
      } catch(Exception e) { Events.Enqueue("SENDER ERROR "+e.Message); }
    });
    worker.IsBackground=true;worker.Priority=ThreadPriority.AboveNormal;worker.Start();
  }
  public void Stop() { stop=true;if(worker!=null) worker.Join(1500); }
}
'@
$u=[IO.Ports.SerialPort]::new('COM6',921600,[IO.Ports.Parity]::None,8,[IO.Ports.StopBits]::One)
$a=[IO.Ports.SerialPort]::new('COM13',115200,[IO.Ports.Parity]::None,8,[IO.Ports.StopBits]::One)
$u.WriteTimeout=1000;$a.WriteTimeout=1000;$a.ReadBufferSize=262144
$sender=[CanTestSender]::new($a)
$writer=[IO.StreamWriter]::new((Join-Path (Get-Location) $Log),$false)
$zero='t20080000000000000000'
$command='t2008'+$Raw.ToString('X4')+'000000000000'
function Note([string]$s){$writer.WriteLine($s);$writer.Flush()}
function Pump([int]$ms,[string]$frame='') {
  if($frame){$sender.Send($frame)}
  $clock=[Diagnostics.Stopwatch]::StartNew();$text=[Text.StringBuilder]::new()
  while($clock.ElapsedMilliseconds -lt $ms) {
    $eventText='';while($sender.Events.TryDequeue([ref]$eventText)){$writer.WriteLine($eventText)}
    [void]$text.Append($u.ReadExisting())
    if($a.IsOpen){$can=$a.ReadExisting();if($can){$writer.WriteLine('BUS '+$can)}}
    Start-Sleep -Milliseconds 1
  }
  $writer.Write($text.ToString());$writer.Flush();return $text.ToString()
}
function Cmd([string]$cmd,[string]$frame='') {
  $writer.WriteLine('# '+$cmd);$u.Write($cmd+"`n");return (Pump 150 $frame)
}
function Check([string]$s,[string]$pattern){if($s -notmatch $pattern){throw "Missing $pattern in $s"}}
function Running([string]$frame,[int]$ExpectedMa=-1) {
  $s=Cmd 'foc status' $frame;Note $s
  Check $s 'FOC: running';Check $s 'fault=none'
  if($s -notmatch 'rpm=(-?\d+)'){throw 'Missing speed'}
  if([math]::Abs([int]$Matches[1]) -gt 450){throw 'Host speed limit'}
  if($s -notmatch 'peak=(\d+) mA'){throw 'Missing current'}
  if([int]$Matches[1] -gt $peakLimit){throw 'Host current limit'}
  if($ExpectedMa -ge 0) {
    Check $s "FOC PI: target=0/$ExpectedMa mA, ref=0/$ExpectedMa mA"
    if($s -notmatch 'FOC current: id=(-?\d+) iq=(-?\d+) mA'){throw 'Missing dq current'}
    if([math]::Abs([int]$Matches[2]-$ExpectedMa) -gt 300){throw 'Iq tracking error exceeds 300 mA'}
  }
}
try {
  $u.Open();$a.Open()
  $a.Write("C`rS8`rO`r");$null=Pump 100
  Note (Cmd 'stop');Note (Cmd 'can disarm');Note (Cmd 'can monitor off')
  $s=Cmd 'status';Note $s;Check $s 'PWM: stopped, mode=off'
  $s=Cmd 'can status';Note $s;Check $s 'PWM=arm-controlled';Check $s 'bus_off=0'
  $s=Cmd 'cal status';Note $s;Check $s 'Calibration: VALID'
  $s=Cmd 'fault';Note $s;Check $s 'HIGH, inactive'
  $s=Cmd 'vm';Note $s;if($s -notmatch 'VM=([\d.]+) V'){throw 'Missing VM'}
  if([double]$Matches[1] -lt 20 -or [double]$Matches[1] -gt 26){throw 'VM out of test range'}
  # Frames while disarmed must never start PWM.
  $null=Pump 200 $command
  $s=Cmd 'status' $command;Note $s;Check $s 'PWM: stopped, mode=off'
  Note (Cmd 'can arm' $zero)
  Running $zero
  if($Steps -or $LoadSteps) {
    # Verify settled targets and measured Iq across positive steps and zero return.
    $stepValues=if($LoadSteps){@(1638,2458,3277,4096,0)}else{@(410,819,1638,0)}
    foreach($stepRaw in $stepValues) {
      $command='t2008'+$stepRaw.ToString('X4')+'000000000000'
      $expected=[int][math]::Truncate($stepRaw*20000.0/16384)
      Note "STEP raw=$stepRaw expected_mA=$expected"
      $null=Pump 120 $command
      for($i=0;$i -lt 5;$i++){Running $command $expected}
    }
  } else {for($i=0;$i -lt 5;$i++){Running $command}}
  $s=Cmd 'foc current 0 1' $command;Note $s;Check $s 'UART control blocked'
  $s=Cmd 'can status' $command;Note $s;Check $s 'state=2 reason=0'
  # Stop transmitting, not a UART stop: firmware must disable PWM itself.
  $sender.Stop()
  Note (Pump 150)
  $s=Cmd 'can status';Note $s;Check $s 'state=3 reason=1 raw=0'
  $s=Cmd 'status';Note $s;Check $s 'PWM: stopped, mode=off'
  $s=Cmd 'foc status';Note $s;Check $s 'FOC: off'
  $null=Pump 200 $zero
  $s=Cmd 'can status' $zero;Note $s;Check $s 'state=3 reason=1'
  $s=Cmd 'status' $zero;Note $s;Check $s 'PWM: stopped, mode=off'
  # Explicit re-arm permits a second zero-current start, then disarm stops it.
  Note (Cmd 'can arm' $zero);Running $zero
  Note (Cmd 'can disarm' $zero)
  $s=Cmd 'status';Note $s;Check $s 'PWM: stopped, mode=off'
  Note 'PASS: disarmed reject, zero start, current, UART exclusion, timeout, no auto-restart, rearm, disarm'
} finally {
  $sender.Stop()
  if($u.IsOpen){try{Note (Cmd 'can status');Note (Cmd 'can disarm');Note (Cmd 'stop');Note (Cmd 'status');Note (Cmd 'foc status')}finally{$u.Close()}}
  if($a.IsOpen){$a.Write("C`r");$a.Close()}
  $u.Dispose();$a.Dispose();$writer.Dispose()
}
