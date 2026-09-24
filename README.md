Hiradora第3世代のFWを試す場所。
https://github.com/T-semi-Tohoku-Uni/hiradora_currentsense_test から移行。

電流ロガーは64点×2バッファのUART DMA方式です（4000点保存方式は撤去）。
USART1は **921600 bps / 8N1**、標準は約200 Hzで記録します。
TeleplotでCOMポートに直接接続し、`adc`を送るとグラフ表示できます。
専用スクリプトやPythonのインストールは不要です。

- [DMAロガー・Teleplotの接続手順とCubeMX設定](docs/dma_logger.md)
- [シリアルコマンド一覧](docs/serial_commands.md)

`adc stop`でログ停止、`stop`でモーターも停止します。


FOCの基盤として、dq電圧からのPWM生成・手動エンコーダー校正・Flash保存を追加しました。
起動時はPWM停止で待機します。`cal start`でのみ校正し、成功後に`cal save`で保存します。
未保存・設定不一致なら起動時に警告します。`cal test`はPWM停止中の演算セルフテストです。
校正中も`adc`で電流を表示でき、`adc stop`後も電流保護は続きます。中止は`stop`です。
設定値と校正の操作手順は[シリアルコマンド一覧](docs/serial_commands.md)を参照してください。
dq電圧一定FOCを追加しました。`foc voltage 0 0.05`で指令を設定し、
静止・校正済みを確認して`foc start`で開始します。`foc status`で電流と処理時間を表示します。
`stop`で全出力OFF。`adc stop`後も電流監視は継続します。電流PI制御は未実装です。
FOC追加版はRelease/Debugビルド確認済み。Releaseで±0.4 Vの正逆回転、停止、100 HzのADCログを確認しました。
[実機確認結果と残課題](docs/foc_voltage_test_2026-09-24.md)を参照してください。

校正の既定値は **1.0 V / 過電流保護10 A**。FOC通常運転用は **5 A** とし、校正閾値と分離しています。

PWM反映を20 kHz（1周期に1回）へ変更する対応を追加しました。
TIM1のRepetition Counter=1へのCubeMX変更・再生成を確認済みです。
ADC完了を起点に電流処理→FOC→エンコーダー取得を直列化し、FOC時のPWM反映を頂点へ変更しました。
Release/Debugで正逆回転・再始動を確認済み。Releaseでは100 Hzログ併用とADC停止時の独立監視も確認しました。
Debugは周期処理を一部-O2でビルドします。エンコーダー取得の要求見送りが残るため、時間余裕の評価は継続が必要です。

UART受信をHAL ReceiveToIdle_IT＋8行キューへ変更し、ADCログの文字列整形を軽量化しました。
Release実機でFOC中の約200 Hzログ4,093点と、文字間待ちなしの3指令一括送信×20回を欠落なく確認しました。
[2026-09-25のUART・ADCログ試験](docs/uart_adc_test_2026-09-25.md)を参照してください。

Release実機で5分連続運転＋20回の起動停止を確認し、通信エラー・ADCログ欠落・保護停止はありませんでした。
[連続運転・起動停止の試験記録](docs/foc_endurance_test_2026-09-25.md)を参照してください。
