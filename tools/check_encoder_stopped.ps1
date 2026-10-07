param([string]$Port='COM6', [string]$Log='build/encoder_stopped_2026-10-07.log')
$ErrorActionPreference='Stop'
$p=[IO.Ports.SerialPort]::new($Port,921600,[IO.Ports.Parity]::None,8,[IO.Ports.StopBits]::One)
$p.ReadBufferSize=262144
$p.WriteTimeout=1000
$writer=[IO.StreamWriter]::new((Join-Path (Get-Location) $Log),$false)
function Receive([int]$ms) {
  $text=[Text.StringBuilder]::new()
  $timer=[Diagnostics.Stopwatch]::StartNew()
  while($timer.ElapsedMilliseconds -lt $ms) {
    $s=$p.ReadExisting()
    if($s){[void]$text.Append($s);$writer.Write($s)}
    Start-Sleep -Milliseconds 5
  }
  $writer.Flush()
  return $text.ToString()
}
function Cmd([string]$command,[int]$ms=300) {
  $writer.WriteLine("# COMMAND $command")
  $p.Write($command+"`n")
  Receive $ms
}
try {
  $p.Open()
  Write-Output (Cmd 'stop')
  Write-Output (Cmd 'adc stop')
  Write-Output (Cmd 'angle stop')
  Write-Output (Cmd 'ntc stop')
  foreach($command in @('status','vm','cal status','angle status','foc status','serial status')) {
    Write-Output (Cmd $command 600)
  }
} finally {
  if($p.IsOpen){$p.Close()}
  $p.Dispose()
  $writer.Dispose()
}
