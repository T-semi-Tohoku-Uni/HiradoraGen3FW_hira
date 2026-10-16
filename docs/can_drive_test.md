# CAN→FOC接続と実機試験（2026-10-16）

**最新状態：フィードバックを1msへ統一し、10ms選択肢とCPU占有率測定コードを削除済み。** 実機には直前の計測OFF通常Debug版を書き込み済みで、最終確認時はPWM停止・CAN未アーム。計測コード削除後の実機書き込みは行っていない。正逆5A試験と占有率計測は末尾および[CPU占有率記録](cpu_profile_test.md)を参照。以下の10ms版・選択設定の説明は当時の試験記録。

## 現在の動作

起動時はDISARMEDでPWM停止。`can arm`後50ms以内の新しいゼロ指令で、mainから`Foc_StartCurrent`を呼び、Id=Iq=0AでPWMを開始する。**ゼロ指令でも通電する。** 以後、自ノードのCAN指令を±16384→±20Aに換算し、既存の±15A上限へ制限して`Foc_SetCurrentTarget(0, iq)`へ渡す。既存の60A/sスルーレート、相電流20A保護、1000rpm速度保護を維持した。

起動時は既存UARTと同じ校正・静止・角度・電圧・ADC検査を共有する。モーター校正IDとCAN_NODE_IDは独立。起動失敗やFOC保護停止はCANのFAULTにラッチする（reason=7）。通信途絶はreason=1。再送だけでは復帰せず、停止処理完了後の明示的な再アームと新しいゼロ指令が必要。

- `MotorControl_StartVoltageGuarded`は、I2C待ちを含む起動準備後に、ブートストラップ充電前と通常PWM出力開始前の両方で許可を検査する。検査と出力許可を同じ短い排他区間で行い、許可失効後に再通電しない。
- 通信期限をHAL tickとDWTの双方で検査し、SysTickと既存FOC TIM監視から停止する。受信フレームのハードウェアtimestampによる年齢も反映する。完全な割り込み停止やCPU停止への保証ではない。
- CAN割り込みは指令の取り出しと安全状態の更新だけを行う。FOC開始・電流更新はmain、保護停止はmain/SysTick/FOC TIMで行う。
- 指令の読出しから更新までCAN IRQだけをマスクし、FOC割り込みを維持する。電流API内部でId/Iqを同時更新し、異常・許可失効を最終確認する。
- CAN所有中のUART操作は既存の読み取り専用一覧と停止だけに限定。`can disarm`/`stop`はCAN所有権を解除しPWMを停止する。
- `can status`はmainループごとに1行を表示し、行間でFOC heartbeatを更新できるようにした。

CubeMX設定は変更していない。フィードバックは従来の10ms周期。NTCはモーター温度ではなく基板温度で、運転中は無効になることがある。その場合CAN温度byte=0、UART `valid=0`。温度0℃の実測値とCANだけでは区別できない。

## 検証

Debug/Releaseビルド成功。Flash使用量は128184/115544 byte、RAMは15744/15736 byte。DebugのFlash残りは840 byteで、校正領域は維持。Debugを実機へ書き込み、verify成功。

両ELFで次の10種類が合格した。

- `test_pwm_start_guard.py`：実起動コードのMOE書込みを監視。I2C待ち中の許可失効をモデル化し、充電前・充電後とも許可失効後のPWM有効化がないことを確認。
- `test_can_foc.py`：ゼロhandshake前の開始禁止、一度だけの起動、電流制限、SysTickを止めたDWT/TIM期限検査、DWT周回、起動失敗のラッチ。
- `test_foc_current_target.py`、`test_current_pi.py`、`test_can_safety.py`、`test_can_rx_freshness.py`、`test_can_protocol.py`、`test_can_callbacks.py`、`test_can_telemetry.py`、`test_can_samples.py`。

実機はCOM6、CANアダプターはCOM13。負荷接続済みで、PWM開始前に各試験を通知した。`tools/test_can_drive_hardware.ps1`は**PWMを開始する試験**。当初のRaw範囲は0〜410（約0.5A）で、追加試験準備により0〜1638（約2A）へ拡張した。`-Steps`は約0.5→1→2→0Aの各段階で、整定後の目標/refと実Iq（誤差300mA以内）を検査する。

合格ログ：

- `build/can_drive_zero_pass.log`：ゼロ電流起動、運転中status、UART制御拒否、送信停止によるFAULT(reason=1)、PWM停止、再送だけでは復帰しないこと、明示再アーム、disarm停止。
- `build/can_drive_half_amp_final.log`：同じ手順をRaw=410（約0.5005A）で実施。指令到達後のUART実Iqは0.500〜0.524A、相電流ピーク0.909A、速度表示はほぼ静止。CAN statusでもraw=410、実Iq raw=411/valid=1を確認。運転中の最大計測値はcompute=12.768µs、ADC control=27.993µs。最後はPWM stopped/FOC off/CAN state=0。

UART表示の値は低頻度スナップショットであり、過渡応答やリップル全幅を測ったものではない。通信停止後の物理PWM停止時刻をオシロスコープ等で50ms精度評価したわけではない。負荷が回転する条件、逆転、上限電流、運転中のbus-off/FIFO overflow/nFAULT注入、1kHzフィードバック負荷は今後の検証。

## 試験中に見つかった問題

1. PowerShell内で送信と表示解析を交互に行うと52msの送信空白が生じ、通信保護で停止した。独立スレッドへ分離後も一度69msの空白が発生したため、送信スレッドの優先度を上げた。最終試験は合格したが、Windows/USB送信はハードリアルタイムではない。50ms保護期限は緩和していない。
2. 運転中`can status`の8行連続出力がmain監視期限を超え、FOCが`main stale`で停止した。1ループ1行へ修正して再ビルド・全モデル試験・実機再試験を行い、正常継続を確認した。
3. 再試験時にゲートドライバーRESETラッチ（STATUS=0x88、nFAULT LOW）を検出し、通電前に中断した。既存`stop`→`fault clear`で0x80/HIGHを確認して再試験した。FWのcan armに自動fault clearは追加していない。

既知のBit1エラーは残っている。アダプターを閉じていた期間にはACKなしのエラーも蓄積するため、起動後の累積errorsをそのまま運転中のBit1エラー率として扱わない。

旧`test_can_safety_hardware.ps1`には、`PWM=start-disabled`を要求するガードを追加済み。旧スクリプトは大きい指令を表示専用として送るため、新FWではアーム前に拒否する。

## 2Aまでのステップ試験

COM6でVM=24.260V、校正VALID、nFAULT HIGH、エンコーダー通信正常を確認した。追加試験の起動時にCOM13のOpenが「アクセス拒否」となり、PWM開始には到達していない。COM6へstop/can disarmを送ってPWM stoppedを再確認済み。COM13解放後に再試験する。ポートOpen失敗時の後処理で、未接続アダプターのReadExisting例外が元のエラーを隠す不具合も修正した。

その後ユーザーがCOM13を解放し、PWM開始前の通知後に`-Raw 1638 -Steps`で再試験して合格した。ログは`build/can_drive_steps_2a_retry.log`。FWの変更・書き込みは行っていない。

| CAN raw | 指令（表示mA） | 整定後UART実Iq（mA） | 速度表示（rpm） |
|---|---:|---:|---:|
| 410 | 500 | 430〜638 | −2〜2 |
| 819 | 999 | 904〜1065 | −1〜1 |
| 1638 | 1999 | 1852〜2096 | −1〜3 |
| 0 | 0 | −209〜128 | −1〜1 |

各段階でtarget/ref一致と実Iq誤差±300mA以内をスクリプトで検査した。相電流ピーク2.369A。最大計測値はcompute=12.768µs、ADC control=27.993µs、angle age=110.793µs。ほぼ静止状態での電流追従であり、回転中の検証ではない。

ゼロへ戻した後、CAN送信停止によりstate=3/reason=1で停止した。ゼロ再送だけでは復帰せず、明示再アームでゼロ電流運転に復帰し、disarmで停止することを確認。終了時はPWM stopped/FOC off/CAN state=0。

## 5Aでの負荷回転試験（2026-10-16）

ユーザーから「現状の負荷は5A程度で回転する」との指示を受け、PWM開始前に2→3→4→5→0A、各約1秒の試験を通知した。`-Raw 4096 -LoadSteps`で実施し合格。ログは`build/can_drive_steps_5a.log`。FWは変更せず、試験スクリプトの上限をRaw=4096（5A）へ拡張した。ホスト側の中断しきい値は450rpm・相電流ピーク7Aで、FWの保護設定は維持した。

| CAN raw | 指令（表示mA） | 整定後UART実Iq（mA） | 速度表示（rpm） |
|---|---:|---:|---:|
| 1638 | 1999 | 1948〜2110 | −1〜14 |
| 2458 | 3000 | 2990〜3181 | 0〜2 |
| 3277 | 4000 | 3898〜4063 | 0〜4 |
| 4096 | 5000 | 4892〜5042 | 57〜134 |
| 0 | 0 | −79〜77 | −1〜37 |

5A段階で負荷の回転を確認。各段階のtarget/ref一致と実Iq誤差±300mA以内を確認した。相電流ピークは5.487A。ゼロ指令はPWM停止ではなくゼロ電流制御であり、速度表示には減速中の値も含まれる。

最大計測値はcompute=12.768µs、ADC control=28.243µs、angle age=111.218µs。指令送信中の予期しない保護停止はなく、送信を意図的に止めると通信期限で停止した。自動復帰禁止、明示再アーム、disarm停止も合格。終了時はPWM stopped/FOC off/CAN state=0。

これで低速回転中の正方向電流指令を確認した。5Aを流したままの通信途絶、逆転、上限電流、長時間運転、1kHzフィードバックは未検証。

## 正逆5A通電中の通信途絶（追加検証）

PWM開始前に各条件を通知し、`-LoadSteps -DropUnderLoad`で2→3→4→5Aまで上げ、ゼロへ戻さずCAN送信を停止した。逆方向は`-Reverse`を追加して−2→−5Aで同じ試験を行った。符号付き16bitの負電流は2の補数で送信する。停止後も直前の非ゼロ指令を再送し、自動再始動しないことを確認した。

| 10ms版のログ | 5A段階の実Iq | 速度表示 | 相電流ピーク |
|---|---|---|---|
| `build/can_drive_drop_5a.log` | 4.960〜5.136A | 109〜117rpm | 5.465A |
| `build/can_drive_reverse_drop_5a.log` | −4.992〜−4.906A | −125〜−112rpm | 5.537A |

両方とも通信期限FAULT、PWM停止、非ゼロ再送時の再始動禁止、明示再アーム、disarm停止に合格。逆方向は停止から起動しており、回転中の正負反転ではない。

## 1msフィードバックの検証

CMakeの`CAN_FEEDBACK_PERIOD_MS`で1/10msを選択できるようにした。既定値10msを維持。例：構成時に`-DCAN_FEEDBACK_PERIOD_MS=1`を指定する。CubeMX設定は変更していない。

初回は周期だけ変更しても約492Hzだった。main末尾の`HAL_Delay(1)`が、このHAL実装では1tick追加して待つため約2ms周期になっていた。1msビルドに限り`HAL_Delay(0)`で次のtickまで待つ形にした。変更はUSER CODE内で、10ms版の待機は従来のまま。後にユーザーからもHAL_Delay変更の許可を受けた。

10ms/1msそれぞれDebug/Releaseのビルドと既存10種類のモデル試験に合格した。その後のmain待機値修正でも4構成をビルドし、1ms版を実機で再検証した。1ms DebugはFlash=128176 byte、RAM=15744 byte、ReleaseはFlash=115536 byte、RAM=15736 byte。校正Flashは保持した。

停止状態の測定：`build/can_feedback_1ms_stopped_fixed.log`。5008.1msで4979フレーム、PC受信集計で約994Hz。角度範囲・予約byte・停止中Iqを検査して合格。USB受信はまとめて届くため、host_max_gap=34.5msはCANバス上の最大間隔ではない。1msごとの厳密な送信保証もしていない。

1ms版の実機試験もすべて合格：

| ログ | 5A段階の実Iq | 速度表示 |
|---|---|---|
| `build/can_drive_1ms_zero.log` | ゼロ電流試験 | ほぼ静止 |
| `build/can_drive_1ms_drop_5a.log` | 4.897〜5.063A | 108〜118rpm |
| `build/can_drive_1ms_reverse_drop_5a.log` | −5.179〜−4.916A | −126〜−119rpm |

正逆とも相電流ピークは表示で5.366A。通電中の通信停止、非ゼロ指令再送時の再始動禁止、再アーム、disarm停止を確認した。現在の実機は`build/can_1ms_debug/HiradoraGen3FW.elf`で、最終状態はPWM stopped/FOC off/CAN state=0。

### 処理時間・通信負荷の評価と限界

1ms版の負荷試験で観測された最大値はcompute=12.600µs、ADC control=28.037µs、angle age=111.362µs。直前の10ms版5A試験ではADC control最大28.243µs。今回の短時間条件で処理時間の明確な悪化や期限超過は観測しなかった。ただし計測区間の最大値であり、CPU全体の平均占有率ではない。CAN/main/SPIを含むCPU占有率の定量測定、長時間の最悪値評価は未実施。

標準ID・Classic CAN・8byteをフレーム間隔込みで約111〜135bit/フレームと見積もると、1台の1kHzフィードバックは1Mbpsバスの約11.1〜13.5%を使用する。指令も1kHzなら1台で合計約22.2〜27.0%。8台が1kHzで応答し、2グループの指令も各1kHzなら約111〜135%となり、1Mbpsには収まらない。複数台では周期・位相・指令頻度の設計が必要。エラー/再送等はこの概算に含まない。

今回のPC指令送信はWindows/USBで約5〜16ms間隔の試験用送信であり、指令自体を1kHzで送る負荷試験ではない。Bit1エラーは継続。正方向1ms試験の開始status〜運転中statusではqueuedが4467増、errorsが92増、skip増加なし。逆方向ではqueuedが4399増、errorsが71増、skipが10増えた。少なくともこの条件では無欠落通信を達成していない。累積エラーにはアダプター閉鎖中のACKなしも含まれるため、比較は測定区間の差分で行う。

残る検証は回転中の方向反転、上限電流、長時間運転、運転中のbus-off/FIFO overflow/nFAULT注入、物理的な50ms停止時間、CPU全体の占有率、複数ノードや1kHz指令を含む負荷評価。
