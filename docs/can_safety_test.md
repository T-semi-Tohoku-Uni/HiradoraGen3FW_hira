# CAN安全状態管理の試験（2026-10-15）

## 範囲

CANからのPWM開始とFOC電流目標更新は**まだ未接続**。本段階は、実測フィードバックを維持したままアーム、制御所有者の排他、受信指令の鮮度、タイムアウトを検証する。

can_safety.c/.hはHAL非依存の状態遷移。can_comm.cが排他・timestamp・FIFO・UARTを担当する。SysTickのUSER CODE区間から1msごとに監視する。既存ADC/TIM/SPIのFOC IRQとCubeMX設定は変更していない。

## 状態と操作

| state | 意味 | UART制御 |
|---|---|---|
| 0 | DISARMED、起動時状態 | 従来のUART操作を許可 |
| 1 | WAIT_ZERO、新しいゼロ指令待ち | 読み取りと停止のみ |
| 2 | READY、安全状態試験の準備完了 | 読み取りと停止のみ。PWMは開始しない |
| 3 | FAULT、再アーム要求 | 読み取りと停止のみ |

- `can arm`はPWM/FOC/校正/ADC停止、CAN初期化成功、bus-offなし、nFAULT正常を要求する。FIFOを最大3フレーム破棄し、アーム時刻を保存する。
- アーム後50ms以内の、自ノード宛ての**新しいゼロ指令**でREADYになる。先に非ゼロ指令が来ても待機期限を延長しない。二重armも期限を更新しない。
- READY中の指令は状態内のraw値だけを更新する。PWM開始/FOC更新関数へ渡さない。
- 50ms以上新しい有効指令がなければFAULT、raw=0、PWM停止を要求する。遅れて届いたフレームを処理するときも先に期限を検査する。
- FAULT後にゼロ/非ゼロが届いても自動復帰しない。`can arm`で再びWAIT_ZEROへ進む。bus-offは従来どおりラッチし、この段階ではリセットを必要とする。
- `can disarm`、`stop`、`foc stop`でCAN所有権を解除する。所有中はPWMも停止する。DISARMEDのcan disarmはUART側の既存動作を停止するコマンドとしては扱わない。
- CAN所有中は`status`、`foc status`、`angle status`、`serial status`、`cal status`、`adc status`、`vm`、`fault`のみ従来ハンドラーへ渡す。その他の駆動・設定・校正・fault clear等は拒否する。can自身のstatus/monitor/disarmは使用可能。

reason値：0=なし、1=50ms timeout、2=FIFO lost、3=bus-off、4=古い/時刻不明の受信、5=nFAULT、6=HAL受信失敗。

## 鮮度と割り込み

- FDCANの内部timestampをHALで有効化。1Mbps、分周16で1count=16µs、16bit周回は約1.048576秒。
- 読出し時刻を受信時刻として扱わず、フレームのRxTimestampから年齢を求める。5ms超のフレームは破棄し、アーム中ならFAULT。
- FIFOが空だったことを確認できない期間が1秒以上なら、周回数を判別できないためそのバッチを破棄する。単純な16bit差だけで古いフレームを新着と誤認しない。
- アーム前にFIFOへ入ったフレームは排出する。さらにWAIT_ZEROで、アーム以前に送信開始したフレームや同一timestampのフレームを拒否する。
- FIFO lostが通知されたバッチは採用せずFAULT。単純なBit1エラーだけでは本試験のアームを解除しない。エラー統計は引き続き保持する。
- 共有状態はPRIMASKを保存・復元する短い区間で更新。FIFO排出中はCAN IRQだけをマスクする。CAN ISRは状態更新・停止要求だけを行い、PWM/FOC計算・printfは呼ばない。停止要求はSysTickでMotorControl_Stopへ渡す。
- timeoutは受信timestampの年齢をmsへ切り上げて扱うため、HAL tickの量子化で保守的に早く失効する場合がある。

本段階ではモーターを回していない。実制御へ接続する前に既存FOCの校正/静止/角度/電圧検査を共有する起動API、電流制限付き更新API、FOC保護停止時の所有権解除を追加する必要がある。SysTickは既存優先度15のため、高優先度IRQが停止しない異常や割り込み全面禁止中の50ms停止を保証するものではない。FOC接続時には既存TIM側監視との接続と最悪実行時間の評価を行う。

## 検証

Debug/Releaseビルド成功。FlashはDebug 127056/129024 byte、Release 114432 byte、RAMは両方15592 byte。Debug残量1968byte。実機へDebugを書き込み・verify・reset済み。校正Flashページは保持した。

両ELFで合格：

- test_can_safety.py：起動時拒否、ゼロ待ち、非ゼロで延長しない、49/50ms境界、復帰禁止、各故障理由、32bit tick周回、UART所有権ガード。
- test_can_rx_freshness.py：実受信callbackにHALモデルを接続して、5ms超、timestamp周回曖昧性、アーム前/同一timestamp、正常な周回、FIFO lostを検証。
- test_can_telemetry.py、test_can_samples.py、test_can_protocol.py、test_can_callbacks.py、test_current_pi.pyの回帰。

実機スクリプト：`tools/test_can_safety_hardware.ps1`。COM6/COM13、ログ`build/can_safety_hardware.log`。

1. 最初はnFAULT=LOWでarmを拒否。ドライバーSTATUS=0x88（RESET=1、VDS/THSD/VCC_UVLO=0）。既存`stop`→`fault clear`でSTATUS=0x80、nFAULT=HIGHを確認した。FW内で自動fault clearや保護の迂回は追加していない。
2. 指令なしでarm→state=3/reason=1/raw=0。
3. timeout後にゼロを送信してもFAULTのまま。
4. 再arm後に非ゼロだけ送ってもREADYにならずtimeout。
5. 再arm後の新しいゼロでstate=2/reason=0。
6. UART `foc current 0 1`を拒否。駆動開始コマンドは送信していない。
7. CAN +16384を受信すると状態のrawだけが更新。
8. 指令送信を止めると再びstate=3/reason=1/raw=0。
9. `stop`でstate=0、最後までPWM stopped/FOC off。テスト終了時はcan disarmとstopを送信済み。

FIFO overflow/長時間IRQ遅延/bus-off/nFAULT運転中停止は実機注入していない。これらのモデル試験と、無負荷停止状態での実機状態遷移を区別する。50msの厳密な物理停止遅延や駆動負荷下のCPU負荷は未測定。
