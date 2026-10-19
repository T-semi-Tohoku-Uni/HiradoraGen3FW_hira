# AS5600の角度取得とFOC接続

I2C1（PB7=SDA / PB8=SCL）、400 kHz、DMA1 Channel4 RXで取得する。
`as5600.c/.h`は、既存AS5047PのStartRead、FrameComplete、GetSample、Task、
PrintSample、PrintStatus、ProcessCommand、Pause/Resumeの構造をAS5600用に移植した。
共通ドライバー層は追加していない。2026-10-20にFOC・校正・CANの呼び出し先も
`AS5600_*`へ変更した。

旧as5047p.c/.hは参照用に残すが、ビルド対象と専用IRQフックから除外した。
モーターは360 KV、コイル12個・磁石14個の7極対、AS5600はモーター軸直結。
FOC・校正・マップ診断の相電流過電流停止は10 A、電流指令ベクトル上限は9 A。
モーターIDは2とし、旧モーターの校正値を拒否する。

2026-10-20の通電許可により、`MOTOR_CONTROL_ANGLE_ACQUISITION_ONLY=0`へ変更し、
無負荷で校正と低出力FOCを実施した。現在の実機には通電試験済みReleaseが入り、
校正値を保存済み。PWM stopped / FOC offで終了し、COM6は解放した。
この定数を1にすると、manual / six-step / 校正 / FOC / CANの共通PWM開始処理を
ブートストラップ通電前に拒否する。以下のADC単独試験と旧版試験は通電前の履歴。

## 無負荷での校正・通電FOC（2026-10-20、現在の状態）

VM約24.4 V。校正電圧を1.0 Vから0.3 V、FOC速度停止上限を1000 rpmから300 rpm、
電流モードの出力電圧上限を5.0 Vから0.3 Vへ下げた。相電流過電流停止10 A、
電流指令ベクトル上限9 Aは維持。今回の試験指令は最大0.2 V / 0.2 Aに限定した。
試験スクリプトは`tools/test_as5600_energized.ps1`。FOC中はホスト側でも
速度200 rpm超・相電流ピーク3 A超を検出すると停止し、終了処理で必ずstopを送る。
ホスト監視はUART周期の監視であり、連続的な上限保証ではない。

校正は約16秒で成功。direction=-1、offset=5.142798 rad、相電流ピーク2.284 A。
`cal save`で書込み・照合に成功し、リセット後もVALID / stored=yesを確認した。
H2補正係数は0で、今回のFOCはH2 offで実施した。

最初の0.05 V FOCでは、I2Cデジタルフィルター0の状態でNACKによるencoder invalid停止を
2回再現した。取得要求をADC処理末尾へ移す試行も改善せず、要求位置は元に戻した。
I2Cアナログフィルターを有効のままデジタルフィルターを8へ変更すると、以下の短時間試験では
再発しなかった。原因をノイズと断定するものではなく、長時間・高負荷での検証は未実施。
I2C異常による即時無効化・FOC停止と、300 µsタイムアウト・750 µs鮮度上限は維持している。

試験時点ではドライバーからデジタルフィルター8を設定していた。その後CubeMXで
I2C1のDigital Filter=8を設定し、生成コードも更新した。タイミング値は
`0x10F32A82`から`0x10F0267E`へ再計算された。
ドライバー側のフィルター設定と定数は削除し、起動時・復帰時ともCubeMX生成の
`MX_I2C1_Init()`を使用する。復帰時はUSER CODE内の`I2C1_Reinitialize()`を経由する。
今後もCubeMXの設定項目は生成コードに集約し、ドライバーから上書きしない。
この整理後はDebug / Releaseビルドと取得・復帰・FOC接続のARM試験に合格し、
実機への再書込みと低出力FOCの再試験も完了した（次節）。
本節の通電結果・ELFハッシュは整理前のもの。
センサー側CONFは0x0300を維持し、OTPには書き込んでいない。

| 試験 | 時間 | 結果 | 相電流ピーク |
|---|---:|---|---:|
| 電圧FOC +0.05 V | 0.5秒 | ほぼ静止、正常終了 | 0.818 A |
| 電圧FOC +0.1 V | 1秒 | ほぼ静止、正常終了 | 1.071 A |
| 電圧FOC +0.2 V | 1秒 | 正転、最大94 rpm | 1.167 A |
| 電圧FOC -0.2 V | 1秒 | 逆転、最大絶対値90 rpm | 1.341 A |
| 電流FOC Id=0 / Iq=+0.2 A | 1秒 | ほぼ静止、正常終了 | 0.934 A |

フィルター変更後の上記試験はI2C / sensor / timeout / UART errorsがすべて0、
意図しない停止なし。FOCの最大通信基準角度年齢は660.325 µs（上限750 µs）、
ADC制御処理最大36.550 µs。TIMサンプル間隔最大499.98 µs、TIM観測角度年齢最大650.03 µs。
これらはセンサー内部フィルター遅延を含む物理角度の精度保証ではない。
0.2 A電流試験の表示Iqは-0.253～0.378 Aとばらつき、一時的な電圧飽和もあった。
電流PIの動作開始は確認できたが、PIゲイン調整・低電流精度・回転中の電流追従は未完了。
無負荷のため、大きな電流指令や高速回転の試験は行っていない。

最終Debug / ReleaseビルドとAS5600取得・FOC接続・角度予測・取得要求順序・
電流PI・電流目標値のARM試験に合格。Flash使用量はDebug 125564 B、Release 115444 B、
RAMは両方18768 B。実機にはReleaseを残している。

- 書込み・校正・通電ログ：`build/as5600_energized_20261020/`
- 実機のELF：`filter8_release.elf`、SHA256 `0591E0EAB1F214FF5203690EB7367FC3C8A4147571F6F0297A3D2BDDC340FFDD`
- 保存直後と最終読戻しの校正領域2048バイトは一致。SHA256 `69D69EFEF11B59C495D4F5C52A5139CCA6EE38241D50DBC9FD976A157C96E74E`
- リセット後の校正保持・PWM停止・AS5600正常の最終確認：`build/as5600_energized_20261020/final_stopped.log`

## CubeMX設定統一後の再書込み・再試験（2026-10-20）

I2C1のDigital Filter=8、Analog Filter=Enabled、Timing=0x10F0267Eを
CubeMX生成コードに集約したReleaseを書き込み、`Download verified successfully`を確認した。
ドライバーからのフィルター上書きはない。生成設定自体は今回変更していない。
VM約24.4 V、外部負荷なし。保存済み校正を使用し、再校正・再保存は行っていない。

| 試験 | 時間 | 結果 | 相電流ピーク |
|---|---:|---|---:|
| 電圧FOC +0.05 V | 0.5秒 | 静止、正常終了 | 0.975 A |
| 電圧FOC +0.2 V | 1秒 | 正転、最大93 rpm | 1.331 A |
| 電圧FOC -0.2 V | 1秒 | 逆転、最大絶対値92 rpm | 1.331 A |
| 電流FOC Id=0 / Iq=+0.2 A | 1秒 | ほぼ静止、正常終了 | 1.091 A |

全試験でI2C / sensor / timeout / UART errorsは0、予期しない停止なし。
電流PIの目標値は0.2 Aに到達したが、低電流表示のばらつきがあり、調整完了とはしない。
FOC最大通信基準角度年齢660.293 µs（上限750 µs）、ADC制御処理最大36.693 µs。
TIMサンプル間隔最大500.00 µs、TIM観測角度年齢最大650.11 µs。
実機で通信異常を故意に発生させる復帰試験や長時間・高負荷試験は行っていない。

書込み前のFlash全128 KiBを退避。試験後に校正領域2048バイトを読み戻し、
退避データの同領域と完全一致した。リセット後もdirection=-1 / offset=5.142798 rad /
VALID / stored=yes、AS5600正常を確認。PWM stopped / FOC offで終了し、COM6を解放した。
実機には今回のReleaseを残している。

- 記録：`build/as5600_cubemx_retest_20261020/`
- 書込みELF：`flashed_release.elf`、SHA256 `327DECEBD7701A25AF56516321B9B45FB54AD7F48B857D06C01AB36C1F81ADCA`
- 校正領域SHA256：`69D69EFEF11B59C495D4F5C52A5139CCA6EE38241D50DBC9FD976A157C96E74E`
- 最終確認：`build/as5600_cubemx_retest_20261020/final_stopped.log`

## 確認手順

COM6接続の実機での旧版の角度取得結果は末尾に記載する。
今回のFOC接続版のCONF読み戻し・ADC単独試験の結果も後述する。

1. AS5600の電源、共通GND、SDA/SCLの外付けプルアップを確認する。
2. UARTコンソールで `angle status` を送る。
3. `raw=0～4095`、`mech=0～359.912 deg`、MD=1 / ML=0 / MH=0を確認する。
4. `angle` または `angle start` で100 ms間隔の表示を開始し、軸を手回しする。
5. 一周の折り返し、停止中もseqが増えること、磁石異常・通信断でINVALID/STALEとなることを確認する。
6. `angle stop` で表示を止める。角度取得自体は継続する。

TIM1停止中はmainループから約1 msに1回取得する。
TIM1動作中は既存のTIM1/ADC取得要求位置からTickを呼び、200 µs以上の間隔で読み出す。
電流制御は20 kHz、角度取得要求は最大5 kHz。同じサンプルではオブザーバーを再更新しない。
RAW ANGLE（0x0Cから2バイト）とSTATUS（0x0Bから1バイト）を別トランザクションで読む。
STATUSを起動時、正常時は20 msごとに確認し、磁石異常時は角度を無効としてSTATUSを再確認する。
診断の入る区間は角度取得間隔が通常400 µsになる。実際の最大間隔はIRQ負荷によって延びる。

CANには12 bitのrawを2 bit左シフトして既存14 bit内部単位へ渡し、13 bit送信角度の
単位とRPM換算を維持する。実分解能が増えるわけではない。

## 時刻と異常処理

- request_cycles / reference_cyclesは通信開始処理、received_cyclesは完了コールバックのDWT時刻。
  referenceはセンサー測定ラッチの実測値ではなく近似時刻。受信完了を新しい測定時刻と見なさない。
  校正方向・オフセット・H2補正後の角度を既存オブザーバーで追跡し、ADCとPWM区間中央へ別々に予測する。
- 通信タイムアウトは300 µs、GetSampleのstaleは10 ms、診断のstaleは100 ms。
  FOC角度の最大年齢は通信開始基準で750 µs。旧250 µsから変更し、診断スロットとI2C/IRQ遅延を考慮した。
  センサー内部フィルターを含む実測角度の最大遅延を意味しない。
- I2C異常・タイムアウトでは公開データを無効化し、FOCは次の制御スナップショットで停止する。
  mainでPWM停止を確認してからDMA停止・I2C再初期化を行う。再試行待機は100 ms。
  復旧後の角度再取得でFOCを自動再始動しない。
- バッファ公開とIDLE復帰を短い排他区間でまとめ、コールバックの末尾で次の転送状態を上書きしない。
- `angle status`で現在状態、通信時間、異常回数、磁石状態、最初の異常を確認できる。
  最初の異常と累積回数は回復後も残す。
- `AS5600 TIM`行はTIM取得時の角度サンプル間隔・参照時刻からの年齢の最大値をµsで示す。
  main取得からTIM取得へ切り替わる最初のサンプルは計測対象外。値はリセットでクリアする。
- 起動・停止中の復旧時にCONFを設定して読み戻す。OTPは書き込まない（次節）。
- SDA固定などの物理的なバス固着を解除するSCLパルス生成は実装していない。

## 内部フィルターと予測の限界

CONF（0x07/0x08）を読み、出力・予約ビットを保持してPM=00（通常）、HYST=00、
SF=11（2x）、FTH=000（固定スローフィルター）、WD=0に設定する。
全16 bitの読み戻し一致を確認し、整定を待ってから取得する。
設定は[メーカーのAS5600データシートのCONFレジスター](https://look.ams-osram.com/m/7059eac7531a86fd/original/AS5600-DS000365.pdf)に基づく。

内部サンプリング位相・フィルター遅延は未測定であり、固定遅延補償は加えていない。
整定時間をそのまま純遅延として引くこともしない。
予測計算の整合が取れても、物理ローター角に対する精度の保証にはならない。
ADC単独取得時の周期取得は実測したが、実際のFOC演算負荷下の最悪間隔・IRQ時間、
新モーターのPIゲイン・校正電圧は実機では未検証。
次は低電圧校正・低速FOC試験の準備と、内部遅延を含む角度追従の確認が必要。

## ソフトウェア検証（2026-10-20）

Debug / Releaseの通常ビルドを確認する。
`tools/test_as5600.py`では実際のARMコードをUnicornで実行し、I2Cと時刻を模擬する。
取得値の変換、周回、同値の連続取得、二重起動防止、磁石異常、NACK、通信開始失敗、
stale、タイムアウト、遅い完了通知、再試行、停止、DWT/SysTick周回、PWM開始拒否を確認する。
200 µs制限、診断スロット、設定読み戻し不一致、復旧時の停止確認も試験する。
`test_as5600_foc.py`では実際の受信公開からFOC/Parkまでを接続し、10 A超過停止、
750 µs鮮度、I2C異常時の停止、旧校正値の拒否、CAN角度単位の維持を試験する。
物理I2C信号、DMA/IRQの実機タイミングや磁気精度を検証するものではない。

```powershell
$env:PYTHONPATH='build/prediction_test_deps;tools'
python tools/test_as5600.py build/Debug/HiradoraGen3FW.elf build/Release/HiradoraGen3FW.elf
python tools/test_as5600_foc.py build/Debug/HiradoraGen3FW.elf build/Release/HiradoraGen3FW.elf
python tools/test_angle_prediction.py build/Debug/HiradoraGen3FW.elf build/Release/HiradoraGen3FW.elf
```

今回の通常Debug/Releaseでは、上記3試験と取得要求順序、電流PI、電流目標API、
CAN/FOC、CANテレメトリー、CANプロトコルの試験に合格した。
H2/Parkの試験もCORDIC周辺回路モデルを使って両ビルドで合格した。
校正マップ試験は、自己診断コマンドを有効にしたCMSIS検証専用Releaseで実行し、
自己診断659件、正逆の各1000+1000点取得、全7段階での停止、10種類の異常停止に合格した。

```powershell
cmake --preset Release -B build/As5600CalTest -DFOC_USE_CORDIC=OFF -DCAL_ENABLE_TEST_COMMANDS=ON
cmake --build build/As5600CalTest
python tools/test_cal_map.py build/As5600CalTest/HiradoraGen3FW.elf
```

TIM計測追加後の通常DebugはFlash 123876 B / RAM 18752 B、通常ReleaseはFlash 113844 B / RAM 18752 B。
校正用の最終2 KiB予約は維持した。計測追加後にもAS5600取得・FOC接続のARM試験は両ビルドで合格した。

## FOC接続版の実機確認（2026-10-20、通電前の履歴）

COM6 / 921600 baud、ST-LINK SN `0048003F3234510A37333934`、VM約24.4 V。
PWM停止を確認してFlash全128 KiBを退避し、通常Releaseを書き込み、Verify成功を確認した。
CONFの書き込み・読み戻しが成功して0x0300になり、MD=1 / ML=0 / MH=0を確認した。

`tools/test_as5600_foc_hardware.ps1`で、`adc 200`によるADC単独取得を60秒以上実行した。
ADC単独取得は内部CH4とTIM1を使用し、UVWの全6出力を無効に保つ既存の経路。
モーター開始・FOC開始・校正開始・CAN armの指令は送っていない。
ADCは設定周期50 µs（20 kHz）、ログは間引き200（100 Hz）で取得した。

| 観測 | 結果 |
|---|---:|
| TIM取得時の角度サンプル間隔最大 | 450.00 µs |
| TIM Tickで観測した角度年齢最大 | 550.11 µs |
| FOC用通信鮮度上限 | 750 µs |
| 通信開始から完了コールバックまでの最大 | 140.41 µs |
| I2C / sensor / timeoutの累積エラー | すべて0 |
| INVALID/STALE表示 | 0 |
| ADCログoverrun / UART errors / overrun / queue_drops | すべて0 |
| ADCサンプル番号の最大 | 1256200 |
| 静止中のraw最小～最大 | 409～410 |

750 µs上限に対する差は約200 µsだったが、これはADC単独負荷での観測結果であり、
実際のFOC演算負荷・起動遷移・内部フィルター遅延を含む余裕の保証ではない。
今回は手回し・通電による回転・通信断・磁石取り外しは行っていない。
通電時の角度予測やPI制御の実機検証も未実施。

試験後に校正用Flash 2048バイトを読み戻し、書き込み前後のSHA256一致を確認した。
リセット後もCONF=0x0300、AS5600正常、エラー0、PWM stopped / FOC offを確認し、COM6を解放した。
校正状態はUNCALIBRATED。PWM開始禁止を維持したFOC接続版Releaseを実機に残した。

- 書き込みELF SHA256：`974882834D1B61A8F71E72A8D09C4DCAD8681E72312BFA9FA21A77674233126E`
- 校正領域SHA256：`D0FF1B294B5288D1AE1421EADF5B2D38A8752B76D472FF30BED9028E25B1C5B8`
- 退避・書き込み・生ログ・集計：`build/as5600_foc_hw_20261020/`
- 最終確認：`build/as5600_foc_hw_20261020/final_stopped.log`

## 実機確認（2026-10-19）

以下はCONF未変更・main取得のみだった旧版の結果であり、今回のFOC接続版の実機結果ではない。

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

## 新モーターの暫定設定（2026-10-20、通電前の履歴）

ユーザー指定によりKV=360、モーター軸直結、通常FOC・校正・マップ診断の
相電流過電流停止しきい値を10 Aに統一した。これは定格電流の確認結果ではなく、
今回指定された実験用の保護値。既存の比較は10 A超過で停止するソフトウェア監視であり、
瞬時電流を常に10 A以下に固定するものではない。
電流指令上限は、既存の「指令上限 < 過電流停止しきい値」の条件を満たす9 Aとした。

ユーザーが実機でコイル12個・ローター磁石14個を確認したため、極対数は7と確定した。
`MOTOR_CONTROL_POLE_PAIRS=7`は数値変更なし。軸直結なので、機械1回転に電気角7回転が対応する。
旧モーターの保存校正を受け入れないようモーターIDを1から2へ変更した。
旧モーター由来のPIゲイン・校正電圧は未調整であり、PWM開始禁止を維持する。
この設定変更では実機への再書き込みは行っていない。
