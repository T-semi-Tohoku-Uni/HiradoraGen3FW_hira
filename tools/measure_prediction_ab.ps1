param([string]$Folder='build/prediction_ab_20261017')
# Starts PWM at +/-5 A in separate stopped runs, with existing firmware limits.
$ErrorActionPreference='Stop'
New-Item -ItemType Directory -Force -Path $Folder | Out-Null
$programmer='C:/Program Files/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI.exe'
$on='build/Release/HiradoraGen3FW.elf'
$off='build/PredictionOff/HiradoraGen3FW.elf'
function Flash([string]$elf,[string]$label) {
  & $programmer -c port=SWD sn=0048003F3234510A37333934 mode=UR -d $elf -v -rst 2>&1 | Out-File -Encoding utf8 "$Folder/flash_$label.log"
  if($LASTEXITCODE -ne 0){throw "Flash failed $label"}
  if((Get-Content -Raw "$Folder/flash_$label.log") -notmatch 'Download verified successfully'){throw "Verify missing $label"}
  Write-Output "FLASH VERIFIED $label"
}
Get-FileHash $on,$off | ConvertTo-Json | Set-Content -Encoding utf8 "$Folder/elf_hashes.json"
& powershell -NoProfile -ExecutionPolicy Bypass -File tools/check_encoder_stopped.ps1 -Log "$Folder/before.log"
if($LASTEXITCODE -ne 0){throw 'Initial stop failed'}
& $programmer -c port=SWD sn=0048003F3234510A37333934 mode=UR -u 0x0801F800 2048 "$Folder/cal_before.bin" -rst > "$Folder/cal_before.log"
if($LASTEXITCODE -ne 0){throw 'Calibration backup failed'}
try {
  foreach($label in @('A1','B1','B2','A2')) {
    $elf=if($label.StartsWith('A')){$off}else{$on}
    Flash $elf $label
    foreach($iq in @(5,-5)) {
      $direction=if($iq -gt 0){'pos'}else{'neg'}
      Write-Output "BEGIN $label $direction (5 A, H2 ON, 5 s)"
      & powershell -NoProfile -ExecutionPolicy Bypass -File tools/measure_current_pi.ps1 -Iq $iq -DurationMs 5000 -Temperature -H2 on -Log "$Folder/${label}_${direction}.log" > "$Folder/${label}_${direction}_console.log"
      if($LASTEXITCODE -ne 0){throw "Run failed $label $direction"}
      Write-Output "PASS $label $direction"
    }
  }
} finally {
  Flash $on 'restore_on'
  & $programmer -c port=SWD sn=0048003F3234510A37333934 mode=UR -u 0x0801F800 2048 "$Folder/cal_after.bin" -rst > "$Folder/cal_after.log"
  if($LASTEXITCODE -ne 0){throw 'Calibration read failed'}
  $hashes=Get-FileHash "$Folder/cal_before.bin","$Folder/cal_after.bin"
  $hashes | ConvertTo-Json | Set-Content -Encoding utf8 "$Folder/cal_hashes.json"
  if($hashes[0].Hash -ne $hashes[1].Hash){throw 'Calibration changed'}
  & powershell -NoProfile -ExecutionPolicy Bypass -File tools/check_encoder_stopped.ps1 -Log "$Folder/final_stopped.log"
  if($LASTEXITCODE -ne 0){throw 'Final stop check failed'}
}
