param(
  [ValidateSet('Selftest','Map','Stop','StopMatrix')][string]$Mode='Selftest',
  [string]$Port='COM11',
  [string]$Log='build/cal_map_hardware.log',
  [ValidateSet('MAP_RAMP','MAP_FORWARD','MAP_BACKWARD')][string]$Stage='MAP_RAMP',
  [ValidateSet('stop','cal stop')][string]$StopCommand='stop'
)
$ErrorActionPreference='Stop'
if($Mode -eq 'StopMatrix') {
  foreach($testStage in @('MAP_RAMP','MAP_FORWARD','MAP_BACKWARD')) {
    foreach($testStop in @('stop','cal stop')) {
      $caseLog=$Log.Replace('.log',"_${testStage}_$($testStop.Replace(' ','_')).log")
      & $PSCommandPath -Mode Stop -Port $Port -Log $caseLog -Stage $testStage -StopCommand $testStop
    }
  }
  return
}
$p=[IO.Ports.SerialPort]::new($Port,921600,[IO.Ports.Parity]::None,8,[IO.Ports.StopBits]::One)
$p.WriteTimeout=1000
$p.ReadBufferSize=65536
$writer=[IO.StreamWriter]::new((Join-Path (Get-Location) $Log),$false,[Text.UTF8Encoding]::new($false))
$all=[Text.StringBuilder]::new()
function Receive([int]$ms) {
  $b=[Text.StringBuilder]::new(); $t=[Diagnostics.Stopwatch]::StartNew()
  while($t.ElapsedMilliseconds -lt $ms) {
    $r=$p.ReadExisting()
    if($r){[void]$b.Append($r);[void]$all.Append($r);$writer.Write($r)}
    Start-Sleep -Milliseconds 5
  }
  $writer.Flush(); return $b.ToString()
}
function Cmd([string]$s,[int]$ms=200){$p.Write($s+"`n");return (Receive $ms)}
function Identity([string]$text) {
  $m=[regex]::Match($text,'Calibration: VALID, stage=0, stored=(yes|no), direction=(-?1), offset=([0-9.]+) rad')
  if(!$m.Success){throw "Calibration not valid and idle: $text"}
  return $m.Groups[1].Value+','+$m.Groups[2].Value+','+$m.Groups[3].Value
}
try {
  $p.Open()
  $null=Cmd 'stop'; $null=Cmd 'adc stop' 700
  $before=Cmd 'cal status'; $identity=Identity $before; Write-Output $before
  Write-Output (Cmd "status`nvm`nangle status`nserial status")
  $test=Cmd 'cal test' 700; Write-Output $test
  if($test -notmatch '595 checks, 0 failures'){throw 'cal test failed or unexpected firmware'}
  if($Mode -ne 'Selftest') {
    $start=Cmd 'cal map' 100; Write-Output $start
    if($start -notmatch 'CALMAP_BEGIN,'){throw "Map did not start: $start"}
    $timer=[Diagnostics.Stopwatch]::StartNew(); $nextProgress=10000; $done=$false
    while($timer.ElapsedMilliseconds -lt 65000) {
      $null=Receive 100
      $output=$all.ToString()
      if($output -match 'CALMAP_STOP,reason=(?!map complete)[^\r\n]+'){throw $Matches[0]}
      if($Mode -eq 'Stop') {
        $status=Cmd 'cal status' 50
        if($status -match "stage=$Stage,") {
          $stopped=Cmd $StopCommand 300; Write-Output $stopped
          if($stopped -notmatch 'CALMAP_STOP,reason=aborted'){throw 'Missing abort acknowledgement'}
          $done=$true; break
        }
      } elseif($output -match 'CALMAP_END') {$done=$true;break}
      if($timer.ElapsedMilliseconds -ge $nextProgress) {
        $rows=[regex]::Matches($output,'(?m)^CALMAP,[FR],').Count
        Write-Output "Progress: $($timer.ElapsedMilliseconds) ms, $rows CSV rows"
        $nextProgress+=10000
      }
    }
    if(!$done){throw 'Map/stop test timed out'}
    $null=Receive 300
    if($Mode -eq 'Map') {
      $output=$all.ToString()
      foreach($pass in @('F','R')) {
        $rows=[regex]::Matches($output,"(?m)^CALMAP,$pass,(\d+),[^\r\n]+")
        if($rows.Count -ne 1000){throw "Pass $pass has $($rows.Count) rows"}
        for($i=0;$i -lt 1000;$i++){if([int]$rows[$i].Groups[1].Value -ne $i){throw 'CSV index gap'}}
      }
      Write-Output $output.Substring($output.IndexOf('CALMAP_SUMMARY'))
    }
  }
  $after=Cmd 'cal status'; Write-Output $after
  if((Identity $after) -ne $identity){throw 'Calibration changed'}
  $motor=Cmd 'status'; Write-Output $motor
  if($motor -notmatch 'PWM: stopped'){throw 'PWM not stopped'}
  Write-Output (Cmd "adc status`nangle status`nserial status")
  Write-Output "PASS: $Mode $Stage $StopCommand; calibration unchanged"
} finally {
  if($p.IsOpen) {
    try{$null=Cmd 'stop' 300;$null=Cmd 'adc stop' 300;$null=Cmd 'cal status' 200}
    finally{$p.Close()}
  }
  $p.Dispose();$writer.Dispose()
}
