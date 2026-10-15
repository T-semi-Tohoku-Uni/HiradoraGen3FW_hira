# CAN安全状態管理の試験（2026-10-15）

**現在はCAN→FOC接続済み。最新の動作と実機結果は[CAN駆動試験](can_drive_test.md)を参照。以下の「未接続」は接続前の試験記録。**

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

## FOC接続準備：電流目標API

`Foc_SetCurrentTarget(id_a, iq_a)`を追加し、既存UARTの電流目標更新もこのAPIへ統一した。main専用であり、PWM開始、fault解除、文字列処理、printfは行わない。CANからはまだ呼ばないため、上記READYの動作は変わらない。

- 非有限値、既存ベクトル上限15A超過、校正中、運転中の電圧→電流モード変更、FOC保護停止処理中の更新は拒否する。
- Id/Iqの書き込みと状態の最終検査を短い割り込み禁止区間で行い、元のPRIMASKを復元する。入力検査中に保護ISRがfaultを設定した場合も拒否する。
- 運転中のPI積分器とランプ現在値を維持し、スルーレート処理は既存のADC周期処理に任せる。停止中の目標設定では従来UARTと同様にPIとランプ現在値をリセットする。
- 上限超過をAPI内でクランプせず拒否する。今後CAN接続側で±16384→±20A換算後、既存上限へ制限してId=0Aとして渡す。UART/CAN所有権と受信鮮度はCAN側で管理する。

Debug/Releaseビルド成功。Flashはそれぞれ127280/114656 byte、RAMは15592 byte。Debugの残りは1744 byte。`test_foc_current_target.py`で上限、非有限値、校正/モード/fault拒否、検査中fault注入、PRIMASK復元、PI状態保持を両ELFで確認した。既存のPIとCAN関連7種類の回帰試験も両ELFで合格した。実機操作・書き込み・PWM開始は行っていない。

次は起動APIとCANの接続。`MotorControl_PreparePwmStart`はブートストラップ充電でもPWMを有効にするため、CAN許可の検査は最終起動時だけでなく充電開始時にも必要。I2C待ちの途中でtimeout/停止が発生しても、その後にPWMを再度有効化しない仕組みを用意する。ゼロ電流でもPWMを開始する試験の前にはユーザーへ知らせる。

### 負荷接続後のAPI実機試験（2026-10-15）

上記のモデル試験後、ユーザーが負荷接続完了とコード上限内の試験を許可した。PWM開始前に各試験条件を通知した。API追加済みDebug ELFを書き込み、verify成功。消去範囲はsector 0〜62で、sector 63の校正を保持し、再起動後もCalibration VALIDを確認した。CANからの駆動は依然未接続で、以下はUART経由の試験である。

開始前はVM約24.31V、nFAULT HIGH、エンコーダーエラー0。Id=0Aで次を実施した。

| Iq指令 | 運転中の状態表示の実Iq | 結果 |
|---|---|---|
| 0A、約1秒 | −0.029〜+0.058A | 相電流ピーク0.413A、保護停止なし |
| +0.5A、約2秒 | +0.404〜+0.595A | 相電流ピーク0.855A、保護停止なし |
| +1A→+2A→0A、各約1秒 | 1A時0.857〜1.021A、2A時1.894〜2.032A、ゼロ復帰時−0.085〜+0.091A | 運転中API更新成功、相電流ピーク約2.37A、保護停止なし |

指令変更後のstatusではrefが指令値へ到達し、PI飽和なし。低速のUART状態表示からの値であり、過渡応答や電流リップルの全幅を示すものではない。速度表示は運転中おおむね−2〜+2rpmで、今回の電流では負荷はほぼ静止していた。回転中・逆転・15A上限付近の試験は未実施。

観測された最大値はFOC compute 12.612µs、ADC control 29.900µs。UART表示/ログと10ms周期CAN送信が動作した条件の値であり、CPU全体の占有率や1kHz CAN時の保証ではない。既知のBit1エラーは残り、最終CAN統計はTX queued=10144、done=9995、errors=149、bus_off=0だった（起動後全体の累積値）。

全試験後にPWM stopped/FOC off、CAN state=0、nFAULT HIGHを確認した。FOCの最後のfault表示は明示停止による`user stop`。基板NTCは35℃でありモーター温度ではない。

ログ：`build/can_api_zero_current.log`、`build/can_api_half_amp.log`、`build/can_api_current_steps.log`。

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
