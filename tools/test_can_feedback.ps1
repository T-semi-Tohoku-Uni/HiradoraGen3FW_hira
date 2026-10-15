param([int]$DurationMs=5000,[string]$Log='build/can_feedback.log')
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$u=[IO.Ports.SerialPort]::new('COM6',921600,[IO.Ports.Parity]::None,8,[IO.Ports.StopBits]::One)
$a=[IO.Ports.SerialPort]::new('COM13',115200,[IO.Ports.Parity]::None,8,[IO.Ports.StopBits]::One)
$u.WriteTimeout=1000; $a.WriteTimeout=1000; $a.ReadBufferSize=262144
$writer=[IO.StreamWriter]::new((Join-Path (Get-Location) $Log),$false)
function Note([string]$s){$writer.WriteLine($s);$writer.Flush();Write-Output $s}
function Uart([string]$s){$u.Write($s+"`n");Start-Sleep -Milliseconds 300;Note "$s`n$($u.ReadExisting())"}
try {
  $u.Open();$a.Open();$u.DiscardInBuffer()
  Uart 'stop'; Uart 'can monitor off'
  $u.Write("status`n" ); Start-Sleep -Milliseconds 300
  $pwmStatus=$u.ReadExisting(); Note $pwmStatus
  if($pwmStatus -notmatch 'PWM: stopped, mode=off'){throw 'PWM stop not confirmed'}
  $a.Write("C`rS8`rO`r");Start-Sleep -Milliseconds 300;$a.DiscardInBuffer()
  Uart 'can status'; Uart 'angle status'
  $a.DiscardInBuffer() # UART確認中に溜まったフレームを測定区間に含めない。
  Note 'CAPTURE START: PWM stopped; manual rotation only'
  $captureClock=[Diagnostics.Stopwatch]::StartNew();$pending='';$count=0;$last=$null;$maxGap=0.0
  $minAngle=8192;$maxAngle=0;$minRpm=32767;$maxRpm=-32768
  $minTemp=255;$maxTemp=0
  while($captureClock.ElapsedMilliseconds -lt $DurationMs) {
    $chunk=$a.ReadExisting()
    if($chunk){
      $pending+=$chunk
      $lines=$pending.Split([char]13);$pending=$lines[-1]
      for($i=0;$i -lt $lines.Length-1;$i++) {
        if($lines[$i] -match '^t2018([0-9A-Fa-f]{16})$') {
          $hex=$Matches[1];$angle=[Convert]::ToInt32($hex.Substring(0,4),16)
          $rpm=[Convert]::ToInt32($hex.Substring(4,4),16);if($rpm -ge 32768){$rpm-=65536}
          $iq=[Convert]::ToInt32($hex.Substring(8,4),16);if($iq -ge 32768){$iq-=65536}
          $temp=[Convert]::ToInt32($hex.Substring(12,2),16)
          if($iq -ne 0){throw 'Expected unavailable Iq=0 while stopped'}
          $minTemp=[Math]::Min($minTemp,$temp);$maxTemp=[Math]::Max($maxTemp,$temp)
          if($angle -gt 8191 -or $hex.Substring(14,2) -ne '00'){throw "Invalid feedback $hex"}
          $captureNow=$captureClock.Elapsed.TotalMilliseconds
          if($null -ne $last){$maxGap=[Math]::Max($maxGap,$captureNow-$last)};$last=$captureNow
          $minAngle=[Math]::Min($minAngle,$angle);$maxAngle=[Math]::Max($maxAngle,$angle)
          $minRpm=[Math]::Min($minRpm,$rpm);$maxRpm=[Math]::Max($maxRpm,$rpm)
          $writer.WriteLine(('{0:F3} {1} angle={2} rpm={3} iq={4} board_temp={5}' -f $captureNow,$hex,$angle,$rpm,$iq,$temp));$count++
        }
      }
    }
    Start-Sleep -Milliseconds 1
  }
  Note ('frames={0} elapsed_ms={1:F1} host_max_gap_ms={2:F1} angle={3}..{4} rpm={5}..{6}' -f $count,$captureClock.Elapsed.TotalMilliseconds,$maxGap,$minAngle,$maxAngle,$minRpm,$maxRpm)
  Note "board_temp=$minTemp..$maxTemp C; stopped Iq=0"
  if($count -lt 100){throw 'Insufficient feedback'}
  Uart 'can status';Uart 'angle status';Uart 'status';Uart 'ntc';Start-Sleep -Milliseconds 1100;Uart 'ntc stop'
} finally {
  if($u.IsOpen){$u.Write("can monitor off`nntc stop`nstop`n");$u.Close()}
  if($a.IsOpen){$a.Close()};$u.Dispose();$a.Dispose();$writer.Dispose()
}
