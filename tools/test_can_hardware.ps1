param([string]$Uart='COM6',[string]$Adapter='COM13',
      [string]$Log='build/can_hardware.log')
$ErrorActionPreference='Stop'
$u=[IO.Ports.SerialPort]::new($Uart,921600,[IO.Ports.Parity]::None,8,[IO.Ports.StopBits]::One)
$a=[IO.Ports.SerialPort]::new($Adapter,115200,[IO.Ports.Parity]::None,8,[IO.Ports.StopBits]::One)
$u.ReadBufferSize=262144; $a.ReadBufferSize=262144
$u.WriteTimeout=1000; $a.WriteTimeout=1000
$writer=[IO.StreamWriter]::new((Join-Path (Get-Location) $Log),$false)
function Note([string]$s) { $writer.WriteLine($s); $writer.Flush(); Write-Output $s }
function ReadPort($p,[int]$ms) {
  $b=[Text.StringBuilder]::new(); $t=[Diagnostics.Stopwatch]::StartNew()
  while($t.ElapsedMilliseconds -lt $ms) {
    [void]$b.Append($p.ReadExisting()); Start-Sleep -Milliseconds 2
  }
  return $b.ToString()
}
function Cmd([string]$s) {
  $u.Write($s+"`n"); $reply=ReadPort $u 150
  Note ("UART > $s`n$reply")
}
try {
  $u.Open(); $a.Open()
  Cmd 'stop'; Cmd 'can monitor off'
  $a.Write("C`r"); $null=ReadPort $a 50
  $a.Write("S8`r"); $reply=ReadPort $a 50
  if($reply.Contains([char]7)){throw 'Adapter rejected S8'}
  $a.Write("O`r"); $reply=ReadPort $a 100
  if($reply.Contains([char]7)){throw 'Adapter rejected O'}
  Cmd 'can status'
  $null=$a.ReadExisting()
  $timer=[Diagnostics.Stopwatch]::StartNew()
  $frames=ReadPort $a 2000
  $elapsed=$timer.Elapsed.TotalSeconds
  $writer.WriteLine("# RAW adapter capture`n$frames")
  $matches=[regex]::Matches($frames,'t2018[0-9A-Fa-f]{14}00')
  Note ("CAPTURE seconds={0:F3} feedback_frames={1} rate_hz={2:F2}" -f $elapsed,$matches.Count,($matches.Count/$elapsed))
  if($matches.Count -lt 150){throw 'Insufficient 0x201 feedback'}
  foreach($frame in @('t20084000000000000000','t2008C000000000000000','t20080000400000000000',
                     't1FF84000000000000000','t200700000000000000','t12380000000000000000',
                     'T0000020080000000000000000','r2008')) {
    $a.Write($frame+"`r"); $reply=ReadPort $a 50
    if($reply.Contains([char]7)){throw "Adapter rejected $frame"}
    Note "CAN > $frame"; Cmd 'can status'
  }
  Cmd 'can monitor on'
  for($i=0;$i -lt 100;$i++) { $a.Write("t20080000000000000000`r"); Start-Sleep -Milliseconds 2 }
  Note (ReadPort $u 300)
  Cmd 'can monitor off'; Cmd 'can arm'; Cmd 'can disarm'
  Cmd 'can status'; Cmd 'status'; Cmd 'foc status'; Cmd 'angle status'
} finally {
  # PWM停止と表示停止だけ実行。アダプターはACK可能な状態を維持する。
  if($u.IsOpen){$u.Write("can monitor off`nstop`n"); $u.Close()}
  if($a.IsOpen){$a.Close()}
  $u.Dispose(); $a.Dispose(); $writer.Dispose()
}
