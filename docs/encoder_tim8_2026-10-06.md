# TIM8駆動エンコーダー通信の実装（2026-10-06）

追記: [2026-10-07の実機デバッグ](encoder_hardware_2026-10-07.md)で、運転中の状態表示によるmain staleを修正。
以下は10/6時点の実装・ソフトウェア検証記録。

ユーザーがCubeMXで設定・再生成したTIM8/SPI/DMA構成に合わせ、`as5047p.c`の通信処理を置き換えた。
生成領域は編集していない。mainのInit引数とDMA IRQの参照変更はUSER CODE内。
モーターFOC、H2補正、校正データ形式、保護閾値は維持した。
書込み・通電・実機試験は行っていない。

## 通信構成

- TIM8: 160 MHz、PSC=0、ARR=7999。50 µs周期。
- CH1: PA15のCSをPWM1で生成。CCR1=6400（40 µsでLow）。
- CH2: CCR2=6560（41 µs）でDMA要求。外部ピンなし、タイマーCPU割り込みなし。
- TX: TIM8_CH2 / DMA1_Channel3。RAM上の1語を循環送信。メモリー増分なし。
- RX: SPI1_RX / DMA1_Channel2。2語を交互に受信し、HT/TCで毎フレーム通知。
- SPI: Mode 1、16 bit、5 MHz。SPI側のTX DMA要求は有効化しない。
- 通常送信は0xFFFF。DIAAGCを約20 msごとに挿入し、必要時にERRFLを取得する。

フレームnの受信は要求n-1に対応する。最初の応答を必ず捨て、診断成功後の新しい角度のみ有効にする。
DMAが送信語を読み終わったことを確認し、次回要求まで5 µs以上ある区間でのみ次のコマンドを設定する。
毎回のDMA再設定・CPUによるCS操作はなく、CPUは応答の検証と公開を担当する。

## 時刻と異常処理

TIM8を開始する直前のDWTを基点に、送信予定時刻を8000 cycleずつ進める。
起動前の基点なので要求時刻は実際の送信よりわずかに早く、角度ageを過小評価しない方向となる。
対応する前フレームの時刻を `request_cycles` に保持する。受信IRQが遅れても時刻を付け直さない。
動作中のクロック変更は対象外。初期化時にcore/APB2=160 MHzと対応設定を検証する。

RXのHT/TC順序、残数、TX完了、受信時刻、公開前の締切を確認する。
応答の対応が不明・処理が次の送信に近すぎる・DMA/SPI異常ならサンプルを無効にして停止する。
停止中はTIM8のMOEを解除し、CubeMX設定のOIS1/OSSIでCSをHighにする。
SPI完了待ちは10 µsで打ち切り、通常完了後には500 nsのholdを確保する。

`AS5047P_Watchdog()` は受信停止100 µs超過やタイマー/DMA停止を監視する。
`GetSample()`、main、および既存のTIM/ADC接続から呼び、FOC側の250 µs制限も維持する。
通信障害の復旧はモーター停止後だけ行う。Flash保存時はPauseで停止・無効化し、Resume後は診断から再同期する。

取得APIとサンプル形式は維持した。電気角のdirection/offset/H2補正は引き続きFOC側で行う。
旧方式との実行時切替や共通ドライバー階層は追加していない。比較用の旧版はGit履歴に残る。

## 表示の変更

`angle` / `angle stop` / `angle status` は継続使用できる。

- `source=TIM8`、`frame_hz=20000`。
- `state`: 0=OFF、1=RUNNING、2=FAILED。
- `rx_delay_max`: 当該フレーム送信予定から受信ISRでの取得まで。
- `commit_delay_max`: 当該フレーム送信予定から公開直前まで。
- `overruns`: フレーム欠落、順序不一致、処理締切違反の累積。
- `transfers`: 初回の破棄応答や診断も含めた処理フレーム数。
- エラー時には最初の理由、SPI SR、DMAフラグ、TIM8 CNTを保存する。

旧 `transfer` / `interval` / `launch_max` / `busy_ticks` / `decode_waits` は廃止した。
トレースのRX/PUBLISHはCPU時刻。タイマー送信ではCPUイベントがないためREAD/LAUNCHは出力しない。

## ソフトウェア検証

Debug/Releaseともビルド成功、今回のビルドに警告なし。

| ビルド | RAM | Flash（校正領域を除いた126 KiB中） |
|---|---:|---:|
| Debug | 17,256 B | 125,896 B（97.58%） |
| Release | 17,256 B | 103,248 B（80.02%） |

`tools/test_encoder_tim8.py` は実際のARM ELFをUnicornで実行し、周辺レジスターをモデル化して検証する。
Debugでは生成されたSPI/TIM8初期化も実行する。Releaseではmainへインライン化されたTIM8設定をfixtureで与え、生成MSPを実行する。

両ビルドで以下が合格:

- 初回応答破棄、診断と角度の区別、通常角度・一周境界・同値の連続更新。
- 前フレームの要求時刻との対応、DWT周回。
- 診断挿入、パリティ・EF・磁界診断異常、ERRFL取得と復帰。
- 受信欠落、HT/TC蓄積、TX未完了、公開前の処理遅延。
- 100 µs超過、タイマー停止、RX/TX DMA異常、SPI OVR。
- 運転中に障害を即時復旧で隠さないこと、Pause/Resume後の再同期。
- 対応しないタイマー設定・TX増分設定の拒否。

既存の `test_encoder_h2.py` と `test_current_pi.py` もDebug/Releaseで合格。
生成直後のファイルとの比較で、main/IRQのUSER CODE外、MSP、`.ioc`にこちらの変更がないことを確認した。

再実行例（依存は既存の `build/encoder_test_deps`）:

```powershell
$env:PYTHONPATH='build/encoder_test_deps'
python tools/test_encoder_tim8.py build/Debug/HiradoraGen3FW.elf build/Release/HiradoraGen3FW.elf
python tools/test_encoder_h2.py build/Debug/HiradoraGen3FW.elf build/Release/HiradoraGen3FW.elf
python tools/test_current_pi.py build/Debug/HiradoraGen3FW.elf build/Release/HiradoraGen3FW.elf
```

エミュレーターは端子波形、DMAバス競合、CPU命令の実サイクル数を再現しない。
特にコマンド更新の排他区間が実機で5 µs以内に収まることと、負荷時の通信完了は未検証。

## 次の実機確認

1. モーターPWM停止で手回しし、角度の方向・境界・診断・エラーカウンターを確認。
2. CS/SCK/MOSI/MISOを測り、setup/high/hold、16クロック、0xFFFF、前フレーム応答との対応を確認。
3. UART/ADC併用で処理遅延・受信欠落がないか確認し、Flash保存後の再開も確認。
4. 低電圧FOCで正逆転・起動停止を確認。角度age最大値、ADC処理時間、ログ欠落を旧版と比較。

20 kHzの連続要求は応答が1周期遅れるため、要求から受信までは理想的にも約53.2 µsとなる。
ADCとの位相や診断フレームによって使用する角度のageはさらに増える。
旧方式の中央値23～24 µsより典型値が増える可能性があり、時間余裕の改善はまだ断定しない。
まず一定周期化とCPU負荷低減の効果を測り、その結果で周期や同期の追加変更を判断する。
