# AS5600の角度取得のみの実装（2026-10-19）

I2C1（PB7=SDA / PB8=SCL）、400 kHz、DMA1 Channel4 RXで取得する。
`as5600.c/.h`は、既存AS5047PのStartRead、FrameComplete、GetSample、Task、
PrintSample、PrintStatus、ProcessCommand、Pause/Resumeの構造をAS5600用に移植した。
共通ドライバー層やFOCへの接続は追加していない。

今回は旧AS5047PのFOC・校正・CAN側の呼び出しを維持するため、旧ファイルを残している。
mainは旧SPIドライバーを初期化せず、AS5600のInit/Task/ProcessCommandだけを使用する。
したがってFOC・校正・CAN角度フィードバックにはAS5600の値を渡さない。
旧CAN角度フィードバックは無効のままであり、AS5600のCAN対応は未実装。

`MOTOR_CONTROL_ANGLE_ACQUISITION_ONLY=1`で、manual / six-step / 校正 / FOC / CANの
共通PWM開始処理をブートストラップ通電前に拒否する。新モーター用設定・校正は未変更。
この定数を0にするだけではAS5600のFOC統合は完了しない。

## 確認手順

COM6接続の実機で書き込み・角度取得を確認済み。結果は末尾に記載する。

1. AS5600の電源、共通GND、SDA/SCLの外付けプルアップを確認する。
2. UARTコンソールで `angle status` を送る。
3. `raw=0～4095`、`mech=0～359.912 deg`、MD=1 / ML=0 / MH=0を確認する。
4. `angle` または `angle start` で100 ms間隔の表示を開始し、軸を手回しする。
5. 一周の折り返し、停止中もseqが増えること、磁石異常・通信断でINVALID/STALEとなることを確認する。
6. `angle stop` で表示を止める。角度取得自体は継続する。

取得はmainループから最大1 msに1回開始する。ADC取得やコンソール処理により間隔は延びる。
FOC用5 kHz取得や厳密な取得周期は今回の対象外。
RAW ANGLE（0x0Cから2バイト）とSTATUS（0x0Bから1バイト）を別トランザクションで読む。
STATUSを起動時、正常時は20 msごとに確認し、磁石異常時は角度を無効としてSTATUSを再確認する。

## 時刻と異常処理

- request_cycles / received_cyclesは通信開始・完了コールバック時のDWT時刻。
  センサー内部の測定時刻や電気角は公開しない。
- 通信タイムアウトは5 ms、角度のstale判定は50 ms、診断のstale判定は100 ms。
  mainループ用の値であり、FOCの許容遅延を示す値ではない。
- I2C異常・タイムアウトでは直ちに無効化し、mainでDMA停止・I2C再初期化を行う。
  再試行の待機は100 ms。遅れて届いた完了通知からの再公開を防ぐ。
- `angle status`で現在状態、通信時間、異常回数、磁石状態、最初の異常を確認できる。
  最初の異常と累積回数は回復後も残す。
- センサーのCONF/OTPには書き込まない。内部フィルターの設定と遅延評価は後続作業。
- SDA固定などの物理的なバス固着を解除するSCLパルス生成は実装していない。

## ソフトウェア検証

Debug / Releaseの通常ビルドを確認する。
`tools/test_as5600.py`では実際のARMコードをUnicornで実行し、I2Cと時刻を模擬する。
取得値の変換、周回、同値の連続取得、二重起動防止、磁石異常、NACK、通信開始失敗、
stale、タイムアウト、遅い完了通知、再試行、停止、DWT/SysTick周回、PWM開始拒否を確認する。
物理I2C信号、DMA/IRQの実機タイミングや磁気精度を検証するものではない。

```powershell
$env:PYTHONPATH='build/prediction_test_deps;tools'
python tools/test_as5600.py build/Debug/HiradoraGen3FW.elf
```

通常Releaseでは外部から未使用のGetSampleがインライン化・削除されるため、
Releaseの関数単位試験は別ディレクトリでインライン化を無効にして実行する。
通常Releaseの成果物は上書きしない。

```powershell
cmake --preset Release -B build/As5600Test -DCMAKE_C_FLAGS=-fno-inline
cmake --build build/As5600Test
python tools/test_as5600.py build/As5600Test/HiradoraGen3FW.elf
```

## 実機確認（2026-10-19）

COM6 / 921600 baud、ST-LINK SN `0048003F3234510A37333934`で実施。
書き込み前にPWM停止・FOC offを確認し、Flash全128 KiBを退避した。
通常Releaseを書き込み、STM32CubeProgrammerの`Download verified successfully`を確認した。
初期確認時のVMは約15.1 V。モーター運転開始指令は一度も送っていない。

`tools/test_as5600_hardware.ps1`で30秒記録を2回行った。
2回目はユーザーが軸を手回しした。各回の前後も状態を取得しているため、
下表の304行には30秒間の連続表示に加えて前後の角度表示を含む。

| 項目 | 1回目 | 手回し確認 |
|---|---:|---:|
| 有効な角度表示 | 304行 | 304行 |
| raw最小～最大 | 43～3914 | 25～4092 |
| 表示角度の最小～最大 | 3.779～344.004° | 2.197～359.648° |
| 0/4095境界の横断 | 2回 | 10回 |
| 前後のsequence増加 | 29500 | 29528 |
| 表示されたサンプル年齢の最大 | 2 ms | 2 ms |
| INVALID/STALE表示 | 0 | 0 |

境界横断は隣接表示のraw差が半回転を超える箇所を数えたもの。
独立した角度基準器との比較や精度保証ではない。

両試験の終了時にI2C / sensor / timeoutの累積エラーはすべて0、first faultなし。
磁石状態はMD=1 / ML=0 / MH=0（STATUS=0x67）。
通信開始から完了コールバックまでの記録最大は142.09 µs。
UART errors / overrun / queue_dropsも0、PWM stopped / FOC offを確認した。

最後に校正用Flash領域2048バイトを読み戻し、書き込み前後のSHA256一致を確認した。
校正状態はUNCALIBRATEDであり、今回校正やセンサー設定の書き込みは行っていない。

- 書き込んだRelease ELFのSHA256：`315982996F5559C7E62887DB7B9A95F1BEE140083B571CEF7BD438EFC18C0592`
- 校正領域のSHA256：`D0FF1B294B5288D1AE1421EADF5B2D38A8752B76D472FF30BED9028E25B1C5B8`
- 退避・書き込み・記録・解析：`build/as5600_hw_20261019/`
- 手回しログ：`build/as5600_hw_20261019/manual_rotation/angle.log`
- 最終状態：`build/as5600_hw_20261019/final_stopped.log`

角度取得版Releaseを実機に残し、確認後にCOM6を解放した。
FOC・モーター駆動・通信断や磁石取り外しの実機試験は今回の対象外。
