# CAN単独送受信試験（第1段階）

> 現行FWは第2段階の実測フィードバックへ移行済み。以下は第1段階の記録。
> 現在の仕様と実機結果は [can_feedback_test.md](can_feedback_test.md) を参照。
> 固定ゼロ送信ではなく機械角・rpm・Iq・基板温度を送信する。CAN制御は未接続。

## 実機デバッグ結果（2026-10-15）

COM6=ST-LINK UART、COM13=ESP32-S3-SLCAN Firmware 1.1.0を使用。SLCANのC/S8/Oで1Mbps通常モードに設定した。再現手順はtools/test_can_hardware.ps1。アダプター識別・プロトコル参考：[開発元](https://github.com/l2015/esp32s3-slcan-canable)。

- 修正済みDebug ELFをST-LINKで書き込み・verify・reset済み。消去範囲はsector 0～62で、校正領域を保持し、起動時の既存校正ロードを確認。
- 0x201全ゼロ8byteを受信。2.017秒の試験では194フレーム（約96.2Hz）。0x200の+16384/-16384/0を正しく表示し、他ノード用0x1FFではownを更新しない。不正DLCはinvalidに加算、無関係ID・拡張ID・リモートは受信数を増やさない。
- 100フレームの追加送信でownが100増加、FIFO lost=0。monitorは間引き表示。can arm拒否、PWM stopped、FOC offを確認。
- 送信完了数が投入数を超える不具合を修正。HALが返すTXBTOには前の送信完了ビットも残るため、未集計要求との積集合だけを数える。要求とマスクの公開中はCAN IRQだけをマスクし、FOCのIRQは止めない。
- エラー数が正常受信・送信でも増える不具合を修正。HALの累積ErrorCodeを履歴へ保存後に消費し、次の正常IRQで重複集計しない。TEC/REC/LECと最後のエラー時LECをstatusへ追加。
- エンコーダー接続後はAS5047P OK、約258.4度、age=2ms。SPI/parity/sensor/timeoutはすべて0。モーターは回していない。
- **実際のBit1エラー（LEC=4）は未解決。** アダプター再設定なしの連続試験でもerrorsが190→204（14増加）、queuedが10081→11149、doneが9893→10947。最終TEC=5、REC=0、bus_off=0。統計修正後も実エラーが残るため通信品質合格とはしない。
- ユーザー確認：GND線なし、両端終端あり、ケーブル約50cm。次は電源OFFでCANトランシーバーの信号GND間を接続して同条件で再試験する。GND未接続が原因とはまだ断定しない（USB等による別経路もあり得る）。

ログ：build/can_hardware_before.log、can_hardware_after.log、can_hardware_final.log、can_continuous.log。

修正後はDebug/Releaseビルド成功。CAN callback二重集計回帰、CAN protocol、既存PIの各テストを両ELFで合格。追加テストはtools/test_can_callbacks.py。最終Debug Flash=128816/129024 byte、RAM=15520 byte、Release Flash=111520 byte、RAM=15512 byte。下記の初回ビルド値より増加しており、Debug残量は208byte。

## 実装済みの範囲

CubeMX生成設定はHSE 16MHz、分周1、Prescaler 1、Seg1=13、Seg2=2、SJW=2で1Mbps。PA11 RX/PA12 TX、Classic/Normal、割り込み優先度3。

can_comm.cはHALによる通信・統計・UART表示、can_protocol.cはHAL非依存のパック/アンパックを担当する。CAN_NODE_ID=1は校正用IDから独立している。

起動後に0x201、DLC=8、全byte=0のダミーを約10ms周期で送信する。角度・rpm・Iq・温度の実測値ではない。mainの処理遅延で周期は変動し、遅れをまとめて送る追いつき処理は行わない。未完了送信は次周期に取消しを要求し、キューに積み重ねない。

受信フィルターは0x200/0x1FF。ノード1は0x200の先頭2byteのみ表示用電流値として採用する。拡張ID、リモート、不正DLCを拒否する。ISRは最大3フレームを読み取り、整数の復号と統計・最新値の保存だけを行う。FIFO lostが通知されたバッチは採用しない。

この段階はFOCへの接続・PWM開始・50ms通信停止保護を実装していない。受信値を制御に使用する経路はない。last validは表示用の受信履歴であり、新鮮な制御指令を意味しない。read_age_msはFIFO読出しからの時間であり、バス受信timestampではない。FOC接続前にハードウェアtimestampによる鮮度判定とアーム状態管理を追加する。

## 実機手順

1. ATA6561が通常動作し、CAN両端終端と共通GNDがある試験バスを用意する。相手は1Mbps、Classic、標準IDでACK可能な通常モードにする。既存ノードの0x201と重複させない。
2. FWを書き込み、UART起動表示の`CAN test`とPWM停止を確認する。書き込み・実機試験は今回未実施。
3. `can status`でready=1、armed=0を確認する。相手側で0x201の全ゼロ8byteを約10ms間隔で受信する。TX queuedだけでなくdoneの増加を確認する。
4. `can monitor on`を入力して、次のフレームを送る。モニターは最大10Hz、UART送信中は待たずに表示を見送り、最新値だけを表示する。

| 送信ID | データ | 期待結果 |
|---|---|---|
| 0x200 | 40 00 00 00 00 00 00 00 | own増加、raw=16384、mA=20000 |
| 0x200 | C0 00 00 00 00 00 00 00 | raw=-16384、mA=-20000 |
| 0x200 | 00 00 40 00 00 00 00 00 | ノード1はraw=0 |
| 0x1FF | 40 00 00 00 00 00 00 00 | other増加、ノード1の値は更新しない |
| 0x200 | DLC=7 | invalid増加 |
| 0x123 | 8byte任意 | ハードウェアフィルターで拒否 |

5. `can monitor off`で表示停止。`can arm`はFOC未接続として拒否、`can disarm`は常時disarmedである旨を表示する。どちらも既存UART制御には作用しない。
6. 通信エラー時はerrors/flags、FIFO lost、TX fail/skipを確認する。bus_offはラッチし、再開にはリセットを使う。自動復帰処理はない。

statusのraw/mAは自ノードの最後の値。monitorのid/dataは最後に読んだ有効形式フレームで、他ノード用IDの場合は自ノードrawとは対応しない。温度を接続する次段階では、NTCはモーター温度ではなく基板温度であることを維持する。

## ビルド・テスト

2026-10-15、GNU Arm 14.3.1。Debug/Releaseとも成功、ビルドログにコンパイラ警告なし。

| ビルド出力 | Flash（校正用2KB除外） | RAM |
|---|---|---|
| build/can_baseline_debug/HiradoraGen3FW.elf | 128384 / 129024 byte | 15512 byte |
| build/can_baseline_release/HiradoraGen3FW.elf | 111152 / 129024 byte | 15504 byte |

Debugの容量対策として、ユーザー管理CMakeLists.txtでCAN処理・HAL FDCAN・HAL I2C・main・MSP・consoleを-Osでビルドする。既存FOC/ADC/TIM/SPIの-O2設定と校正Flash予約は維持する。最適化対象のDebugステップ実行はソース行と一致しない場合がある。Debugの残容量は640byteであり、次段階の追加時に再評価する。

```powershell
$env:PYTHONPATH='build/encoder_test_deps;tools'
python tools/test_can_protocol.py build/can_baseline_debug/HiradoraGen3FW.elf build/can_baseline_release/HiradoraGen3FW.elf
python tools/test_current_pi.py build/can_baseline_debug/HiradoraGen3FW.elf build/can_baseline_release/HiradoraGen3FW.elf
```

両ビルドで成功。CAN試験はノード1～8、符号境界、ID選択、長さ、無効ノード、NULL、角度範囲、送信バイト列を実ELF上で確認した。既存PI試験も成功。

test_motor_period.pyは50µs周期検査を通過後、非ゼロ電圧のPWM比較で失敗。変更前の保存済みbuild/Debug/HiradoraGen3FW.elfと今回の両ビルドで同じ失敗（2000,2000,2000／期待2125,1875,1875）を再現した。合格扱いにはせず、テストとCORDIC周辺モデルの適合は別途調査が必要。実機の割り込み時間・CAN電気信号・バス負荷は未検証。
