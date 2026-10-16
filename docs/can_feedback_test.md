# CAN実測フィードバック（第2段階、2026-10-15）

> 現行FWでは安全状態管理を追加済み。`can arm`は状態試験用として有効になったが、PWM開始は未接続。
> 最新のコマンド動作は [can_safety_test.md](can_safety_test.md) を参照。以下は第2段階の記録。

## 現在の仕様

FDCAN1/Classic/1Mbps、ノード1、送信ID 0x201、DLC=8、mainから約10ms周期。CAN電流指令は表示専用で、can armは拒否する。今回もPWMを開始していない。CubeMX設定は変更していない。

| byte | 内容 |
|---|---|
| 0～1 | AS5047P生機械角14bitを右シフト1した0～8191、BE |
| 2～3 | 機械rpm、符号付き16bit、BE |
| 4～5 | 実Iq×16384/20A、符号付き16bitへ飽和、BE。無効時0 |
| 6 | 基板NTC温度の整数℃。負温度は0、無効時0 |
| 7 | 予約、常に0 |

温度は**モーター温度ではない**。C620互換8byteにはvalidフラグを追加せず、無効Iq/温度はUARTの`can status`で区別する。CAN受信側は0が実測ゼロか無効かを判別できないので、この試験段階の制約として扱う。

## 取得方法・制限

- can_telemetry.cはHAL非依存。mainで取得したAS5047Pサンプルのrequest_cyclesと生角差を使用し、0/16383境界を補正して約10ms分を積算する。正方向は生角増加方向で、校正方向・電気角・H2補正とは独立。
- 推定窓内で停止中のセンサーノイズがrpmへ反映される。実機で-1～+2rpmを観測した。デッドバンドで手回しの低速を消す処理は追加していない。
- 同じsequenceは再利用せず、無効角度は推定状態をリセットする。観測間隔が10msを超えた場合も再同期し、新しい推定窓ができるまで送信を見送る。
- 角度/RPM無効時はフレーム自体を送信しない（invalid_skipへ記録）。古い角度を有効な固定値として送り続けない。
- 半回転以上進む観測間隔では速度の方向を一意に推定できない。既存FOC上限1000rpm内を想定。高速領域やmain停止に対する1kHz対応を保証する方式ではない。
- FocVoltage_GetIqSampleはmain専用の読み取りAPI。短いPRIMASK保存・復元区間で観測値を取得し、停止・異常・非有限値・2ms超の古い観測を拒否する。ISR側のPI計算や出力処理は変更していない。
- NTC_GetTemperatureはmain専用。既存のADC取得結果をキャッシュし、100ms超の古い値、ADC失敗/rail、範囲外温度を拒否する。追加のADC変換を要求しない。既存取得が休止する状態ではキャッシュが期限切れになる。
- CAN割り込み内では角度・速度・温度の計算、FOC計算、printfを行わない。

## ビルドとテスト

Debug/Release成功、今回のビルドログにコンパイラ警告なし。追加分の容量確保のため、ユーザー管理CMakeLists.txtでHAL UARTにも-Osを適用した。HALソース自体、CubeMX生成設定、既存FOC/ADC/TIM/SPIの最適化設定は維持する。

| 出力 | Flash / 129024 byte | RAM |
|---|---|---|
| build/can_baseline_debug/HiradoraGen3FW.elf | 125312 | 15560 |
| build/can_baseline_release/HiradoraGen3FW.elf | 112688 | 15560 |

Debug残量3712byte。実機にはDebugをST-LINKで書き込み・verify・reset済み。校正ページは消去していない。

両ELFで以下を合格：

- test_can_telemetry.py：停止、正逆300rpm、生角周回、DWT周回、欠測後の再同期、無効raw。
- test_can_samples.py：Iqの停止/異常/鮮度/NaN、NTCの温度変換/鮮度/無効値。
- test_can_protocol.py、test_can_callbacks.py：通信形式、統計二重集計の回帰。
- test_current_pi.py：既存PI計算・飽和・UART制約の回帰。

既報のtest_motor_period.pyとCORDICモデルの適合問題は今回の変更範囲外。負荷運転・回転中の実Iq送信は未検証。

## COM6/COM13での確認

再現：`powershell -NoProfile -ExecutionPolicy Bypass -File tools/test_can_feedback.ps1 -DurationMs 10000`

最新ログ：`build/can_feedback_verified.log`。Windows PowerShell 5の日本語コメント誤読を避けるため、実機スクリプトはUTF-8 BOM付きとし、計測スクリプトではStrictModeを有効にした。計測開始前のUART確認中に蓄積したCANフレームは破棄する。

- 10.003秒で983フレーム、約98.3Hz。角度5879～5884（約258.4度）でUARTのraw約11762～11764と整合。
- rpm=-1～+2。モーターを動かしていないため、正逆回転の実機追従は未検証（計算の正逆テストは合格）。
- Iq=0、UART valid=0。停止中の無効観測を測定済みと表示していない。
- 基板温度27～28℃、UART NTC約28.17～28.21℃と整数切り捨てが一致。
- AS5047P OK、spi/parity/sensor/timeoutすべて0。PWM stoppedを最後に確認。
- CAN errors 554→571、queued 9802→10870、done 9253→10305。status間隔は計測区間より広いので受信フレーム数と直接引き算しない。最終LEC履歴=4、TEC=0、REC=0、bus_off=0。Bit1問題は継続保留。
- PC受信時刻の最大間隔77.2ms。これはWindowsのスケジューリング、USB/SLCANのバッファリングとポーリングを含む。CANバス上で77.2ms連続欠落したという測定ではない。正確なバス間隔にはハードウェアtimestamp対応の計測が必要。

初回の`build/can_angle_feedback.log`は測定前の蓄積分を含むためレート評価に使わない。`build/can_feedback_final.log`は文字コード問題で中断した記録であり、計時評価には使わない。

## 次の段階

手回しの正逆追従は下記試験で確認済み。運転中実Iqの検証を経て、制御所有者・アーム・通信50ms監視・受信指令の鮮度管理を設計する。現在のCAN受信データをそのまま電流目標へ接続してはいけない。1ms送信化、CPU負荷の実機評価は未実施。

## 手回し試験（2026-10-15）

COM6/COM13、同じ実測フィードバックFWで実施。開始前にUARTでPWM stopped/mode=offを確認できない場合は中止する検査をスクリプトに追加した。30秒間、ユーザーが正逆に手回しし、モーター駆動コマンドは送っていない。

- ログ：`build/can_handturn.log`、解析：`build/can_handturn_analysis.json`。
- 30.0145秒で2951フレーム（約98.3Hz）。機械角26～8171、rpm=-50～+77。
- +3rpm以上306サンプル、-3rpm以下429サンプル。隣接角度差が8count以上かつ|rpm|>=3の621組すべてで、周回補正した角度差とrpmの符号が一致。低速ノイズ領域は比較から除外。
- 逆方向境界：26→8171、補正差-47count、rpm=-34。
- 正方向境界：8150→33、補正差+75count、rpm=+43。
- 基板温度32℃、UART約32.6℃と整合。Iq=0（無効）、予約byte=0を全フレームで確認。
- 最後もPWM stopped/mode=off、エンコーダーspi/parity/sensor/timeout=0、invalid_skip=0。
- CAN errorsは1020→1069（status間で+49）、bus_off=0。既知のBit1問題は継続保留。PC観測の最大間隔55.8msはUSB/Windows待ち込みであり、バス上の連続欠落時間ではない。

これにより手回しの正逆方向と角度境界の追従を確認した。独立した回転計を使用していないため、rpmの絶対精度、回転中の実Iq、負荷運転の検証とは区別する。今回の追加変更はPC側試験スクリプトと記録のみで、FWの再書き込みはしていない。
