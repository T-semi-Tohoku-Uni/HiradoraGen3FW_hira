# UART一括送信・FOC中の200 Hz ADCログ試験（2026-09-25）

今回の対象はシリアル一括送信とADCログ。長時間運転・繰り返し起動停止試験は実施していません。

## 変更内容

- `console.c`: 1文字ごとのHAL受信再設定をHAL_UARTEx_ReceiveToIdle_ITによる64バイト交互バッファへ変更。既存UART FIFOを利用し、完成行を8行保持します。受信DMAの追加やCubeMX設定変更はありません。
- HALは受信エラーIRQでも受信完了を先に通知する場合があり、その場で受信を再開するとErrorCodeが消えます。エラー時はErrorCallbackに復旧を任せ、フラグを記録します。破損した作成中の行は改行まで破棄します。
- `dma_logger.c`: mainで行う4電流の`%f`整形を、整数mAへ丸めて符号・整数部・小数部を出力する方式へ変更。Teleplotのキー、A単位、小数3桁、取得時刻は維持します。
- `foc_voltage.c`: 起動準備中、最初の有効ADCがまだ来ていない段階に運転中のADC鮮度2 ms制限を適用していた問題を修正。この間は開始から20 msの期限で監視し、有効ADC取得後は通常の監視へ移ります。

## 実機試験

Release、COM11、921600 bps / 8N1。FOC指令Vd=0 V / Vq=0.4 V、通常電流保護5 A、`adc 100`を使用。
ホストは文字間の待ち時間を入れず、以下の3行を1回のWriteで送信しました。約1秒間隔で20回送信しています。

```text
foc voltage 0 0.4
serial status
adc status
```

| 確認項目 | 結果 |
|---|---|
| FOC中の一括指令 | 3行×20回＝60件、すべて応答 |
| 電圧設定応答 | 初期設定1件＋一括送信20件＝21件 |
| Serial RX応答 | 停止中8件＋運転中20件＋終了時1件＝29件 |
| ADC status応答 | streaming 20件、終了後idle 1件 |
| 受信エラー／overrun／queue_drops／long_lines／flags | すべて0 |
| ADCログ | 各4電流・sector・sample・log_overrunが各4,093点 |
| サンプル連続性 | 隣接sample番号の差4,092組がすべて100、欠落0 |
| ログ時刻の幅 | 20,454.680 ms、時刻から算出したレート約200.052 Hz |
| ログoverrun | 全点0、終了時も0 |
| FOC状態 | 最終確認時running、fault=none、最終停止報告のpeak=1.212 A |
| 最大処理時間 | FOC演算14.131 µs、ADC制御処理24.000 µs |
| 最大角度age | 235.200 µs（今回の観測値。長期的な余裕の保証ではありません） |
| エンコーダー | spi/parity/sensor/timeout/busy_ticksすべて0 |

上記レートはFWの取得時刻に基づく値で、独立した測定器による周波数測定ではありません。
生ログは作業環境の`build/uart_adc_200hz_idle_release.txt`に保存しています。

## 受信境界の追加確認（PWM停止中）

- `serial sta`を送信し100 ms後に`tus\r\n`を送信：途中では実行せず、完成後に1件だけ応答。
- 64文字の無効行の直後に`serial status`：無効行を通知し、次の正常行を受信。`long_lines=1`。
- `serial status\r\n`を8行1回のWriteで送信：8件応答、errors/overrun/queue_dropsは0。

生ログは`build/uart_rx_boundaries_release.txt`。この意図的な異常行試験のため、試験終了時のlong_linesは1です。
受信キューは有限です。8行を超えて応答待ちなしで送り続ける動作は保証しません。

## 変更前との比較・確認範囲

整数整形と行キューのみを追加し、1文字単位HAL受信を残した中間版では、200 Hzログは欠落0でしたが、
FOC中の一括指令で受信エラー58件と指令欠落が発生しました。ReceiveToIdle化後に上記条件でエラー0を確認しました。
中間版ではHALエラーフラグが再受信設定で消えていたため、58件すべての原因をoverrunとは断定できません。

Release/Debugともビルド成功。今回の200 Hz実機試験はReleaseのみです。
終了時はReleaseを書き込んだ状態でPWM停止、COM11を解放しています。

後続試験として、[5分連続運転・20回の起動停止](foc_endurance_test_2026-09-25.md)も実施しました。
