Hiradora第3世代のFWを試す場所。
https://github.com/T-semi-Tohoku-Uni/hiradora_currentsense_test から移行。

電流ロガーは64点×2バッファのUART DMA方式です（4000点保存方式は撤去）。
USART1は **921600 bps / 8N1**、標準は約200 Hzで記録します。
TeleplotでCOMポートに直接接続し、`adc`を送るとグラフ表示できます。
専用スクリプトやPythonのインストールは不要です。

- [DMAロガー・Teleplotの接続手順とCubeMX設定](docs/dma_logger.md)
- [シリアルコマンド一覧](docs/serial_commands.md)

`adc stop`でログ停止、`stop`でモーターも停止します。
