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
シリアルでVd/Vqを指定するFOC運転と電流制御は、まだ実装していません。
