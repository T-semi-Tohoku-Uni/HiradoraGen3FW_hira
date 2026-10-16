# cangaroo周期送信時の停止調査（2026-10-16）

## 確認できたこと

- 使用中プロセスの配置先は `cangaroo-win32-ccdcb64`。対応する公開コミット `ccdcb64` のソースを調査した。実行バイナリーとソースの完全一致は未検証。
- ユーザーの停止ログは `state=3 reason=1`（通信期限切れ）。`lost=0 stale=0 invalid=0`。FAULT後も受信統計は更新されるが制御用時刻は更新されないため、`age_ms=139521` と `read_age_ms=10` は矛盾しない。
- `FOC timing: ticks=9418` は20kHz制御で約0.471秒の実行に相当する。起動時にticksをクリアしており、ゼロ指令で起動した直後だけの問題ではない。期限判定が正常なら、この間にも指令を継続して受信していたと考えられる。
- `tools/test_can_drive_hardware.ps1` はGUIから独立した送信スレッドでSLCAN文字列を書き込み、`Thread.Sleep(5)` を挟む。受信は別経路で継続して読み出す。
- `build/can_drive_1ms_zero.log` の `TX_MS` を集計すると、同一送信区間の隣接差187件は14〜19ms、50ms以上は0件。これはPCの書き込み要求時刻であり、CANバス上の到着時刻ではない。試験名の1msはフィードバック周期を指す。

## cangarooとの違い

公開ソース：

- [RawTxWindow.cpp](https://github.com/normaldotcom/cangaroo/blob/ccdcb64/src/window/RawTxWindow/RawTxWindow.cpp)：GUIのQTimerで周期送信要求を生成する。
- [SLCANInterface.cpp](https://github.com/normaldotcom/cangaroo/blob/ccdcb64/src/driver/SLCANDriver/SLCANInterface.cpp)：`sendMessage()` は `_msg_queue` に追加するのみ。`readMessage()` 側でキューを取り出し、write/flush/`waitForBytesWritten(300)` を実行してから受信を読む。書き込み結果・待機結果を確認していない。
- [CanListener.cpp](https://github.com/normaldotcom/cangaroo/blob/ccdcb64/src/driver/CanListener.cpp)：`readMessage()` は別QThreadで呼び出す。送信キューの追加・取り出しに共通のmutexがなく、スレッド間の競合がある。

300msは待機の上限であり、毎フレーム300ms待つという意味ではない。GUIのタイマー遅延、シリアル待機、キューの競合は候補だが、今回どれが発生したかは未確定。1msを10msへ変更しても一時的な停止を排除できるとは限らない。以前の「10msなら十分」という案内は、実到着間隔が50ms未満に保たれるという前提が必要だった。

## 次に必要な切り分け

同じFW・HWで専用スクリプトとcangarooを比較し、FW側で最大受信間隔および停止時のHAL/DWT期限を記録すれば、送信側の空白とFW側の期限判定を切り分けられる。cangarooの送信表示はキューへの追加を示すため、それだけではバス上の送信成功を証明できない。

今回の調査ではFWの変更・書き込み・PWM開始・COMポート操作は行っていない。50ms保護は維持。
