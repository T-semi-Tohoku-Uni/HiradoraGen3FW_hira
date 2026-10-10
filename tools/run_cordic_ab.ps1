param([string]$Before='build/CmsisRelease/HiradoraGen3FW.elf',
      [string]$After='build/CordicRelease/HiradoraGen3FW.elf')
$ErrorActionPreference='Stop'
$stamp=Get-Date -Format 'yyyyMMdd_HHmmss'
$folder="build/cordic_ab_$stamp"
$null=New-Item -ItemType Directory -Path $folder
$programmer='C:/Program Files/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI.exe'
$template=Get-Content -Raw -Encoding utf8 tools/measure_cordic_foc.ps1
Get-FileHash $Before,$After -Algorithm SHA256 | ConvertTo-Json | Set-Content -Encoding utf8 "$folder/elf_hashes.json"
function Flash([string]$elf,[string]$label) {
 $result=& $programmer -c port=SWD sn=0048003F3234510A37333934 mode=UR -d $elf -v -rst 2>&1
 $code=$LASTEXITCODE
 $result | Set-Content -Encoding utf8 "$folder/flash_$label.log"
 if($code -ne 0 -or ($result -join "`n") -notmatch 'Download verified successfully'){throw "Flash failed: $label"}
 Write-Output "FLASH VERIFIED $label"
 Start-Sleep -Seconds 2
}
& $programmer -c port=SWD sn=0048003F3234510A37333934 mode=UR -u 0x0801F800 2048 "$folder/cal_before.bin" -rst > "$folder/cal_before_read.log"
if($LASTEXITCODE -ne 0){throw 'Calibration backup failed'}
try {
 foreach($label in @('A1','B1','B2','A2')) {
  $elf=if($label.StartsWith('A')){$Before}else{$After}
  Flash $elf $label
  $script=$template.Replace('build/cordic_foc_$stamp', "$folder/$label")
  $script | Set-Content -Encoding utf8 "$folder/measure_$label.ps1"
  Write-Output "BEGIN $label"
  & powershell -NoProfile -ExecutionPolicy Bypass -File "$folder/measure_$label.ps1"
  if($LASTEXITCODE -ne 0){throw "Measurement failed: $label"}
  Write-Output "COMPLETE $label"
 }
} finally {
 # Measurement scripts stop PWM in finally; return to CMSIS for result review.
 Flash $Before 'restore_baseline'
 & $programmer -c port=SWD sn=0048003F3234510A37333934 mode=UR -u 0x0801F800 2048 "$folder/cal_after.bin" -rst > "$folder/cal_after_read.log"
 if($LASTEXITCODE -ne 0){throw 'Calibration verification failed'}
 $hashes=Get-FileHash "$folder/cal_before.bin","$folder/cal_after.bin" -Algorithm SHA256
 $hashes | ConvertTo-Json | Set-Content -Encoding utf8 "$folder/cal_hashes.json"
 if($hashes[0].Hash -ne $hashes[1].Hash){throw 'Calibration changed'}
 Write-Output "CALIBRATION UNCHANGED; ARTIFACTS=$folder"
}
