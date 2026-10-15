# Hiradora Gen3 CAN導入調査・CubeMX設定手順

2026-10-15。今回は調査・設計のみ。CubeMX反映後にCAN単独送受信を実装し、FOC接続は別段階とする。

## 現行コードの確認結果

- MCU設定はSTM32G431V系、HSE 16MHz、SYSCLK/APB1/APB2 160MHz。FDCAN未有効、HAL_FDCAN_MODULE_ENABLEDも無効。HALドライバのソースは同梱済み。
- main.cの起動処理はPWM停止を維持し、明示的なstartを要求する。
- foc_voltage.cに電圧FOCと電流PIが共存する。電流目標の更新は現在UARTの文字列解析内部にある。起動時には校正、エンコーダー、母線電圧、静止状態などを確認する。
- motor_control_config.hの電流指令上限は15A、スルーレート60A/s、相電流保護20A、PI出力上限5V。CANの±16384→±20A換算は通信上の単位であり、適用指令は既存の±15Aで制限する。
- ADC1_2とTIM1更新IRQは優先度0、SPI1とSPI DMAは1、UARTとUART DMAは2。CANは3を提案する（数値が大きいほど低優先度）。
- FocVoltage_WatchdogISRにはmain監視20ms、ADC監視、nFAULT監視がある。CAN監視追加時も維持する。
- AS5047P_TaskはTIM1停止中にも約1msごとに取得を要求する。AS5047P_GetSampleは短い割り込み禁止区間で整合したサンプルを返す。停止中にPWMを開始する必要はない。
- FOCのrpmは運転中の制御用。停止中・手回し時のrpmには機械角の連続サンプルと経過時間による別の推定が必要。未更新・異常なサンプルを有効値として扱わない。
- FocVoltage_GetObservationISRは同じADC ISR内向けで、mainから直接使用できるAPIではない。フィードバックには整合した観測値のコピーAPIが必要。
- NTCは基板温度でありモーター温度ではない。現状は表示用で取得APIがなく、CurrentSense_IsBusy中は取得を休止する。FOC中の取得可否と鮮度を整理する必要がある。
- mainはHAL_Delay(1)と母線電圧取得中のcontinueを含む。10ms試験はmainで可能だが、厳密な1ms送信と50ms停止をmainだけに依存させない。
- MOTOR_CONTROL_MOTOR_IDは校正レコードの照合用。CAN_NODE_ID（初期値1、範囲1～8）を別定義とする。

## ユーザーが反映するCubeMX設定

基板接続：PA11 RX、PA12 TX、ATA6561、MCUによるEN/STB制御なし（ユーザー確認済み）。

1. HiradoraGen3FW.iocを開く。Connectivity → FDCAN1を有効化する。
2. PA11をFDCAN1_RX、PA12をFDCAN1_TXに割り当てる。別の既存周辺機能を移動しない。
3. Clock ConfigurationでFDCANのクロック源をHSEにする。既存HSEは16MHz。SYSCLK、PLL、ADC、TIM、SPIのクロックは現状を保つ。
4. 次のパラメーターを設定する。画面上の名称はCubeMX版によって多少異なる。

| 項目 | 設定 |
|---|---|
| Frame Format | Classic mode |
| Mode | Normal mode |
| Clock Divider | Divide by 1 |
| Nominal Prescaler | 1 |
| Nominal Time Seg1 | 13 |
| Nominal Time Seg2 | 2 |
| Nominal Sync Jump Width | 2 |
| Standard Filters Nbr | 1 |
| Extended Filters Nbr | 0 |
| Tx FIFO/Queue Mode | FIFO operation |
| Auto Retransmission | Disable（初期試験で古い送信を滞留させない） |
| Transmit Pause | Disable |
| Protocol Exception | Disable |

計算：16MHz / 1 / (1+13+2) = 1Mbps。サンプルポイントは14/16 = 87.5%。Classic CANなのでData Bit Timingは使用しない。BRSは送信フレームでも無効にする。

5. NVICのFDCAN1 interrupt 0をEnable、Preemption Priority=3、Sub Priority=0。初期実装では通知をline 0に集約し、interrupt 1は不要。
6. 既存のCMake生成方式とUSER CODE保持設定を保ってGenerate Codeする。生成後、差分にFDCAN初期化、GPIO AF、クロック選択、IRQ、HAL有効化、HALソース追加が含まれることを確認する。FOC用のADC/TIM/SPI設定が変わっていたら実装前に確認する。

同梱G4 HALではRx FIFO0/1とTx FIFOは各3要素固定。他シリーズ向けの可変Message RAMサイズ設定は追加しない。標準ID/DLC 8はフレームとフィルターで指定する。HALでdual-IDフィルター（0x200、0x1FF）を1個設定し、非一致・拡張ID・リモートフレームはglobal filterで拒否する。

ATA6561のSTBYがハードウェアで通常動作レベルに固定されていること、VIOとMCUの電圧が一致することを確認する。CAN両端の120Ω終端、共通GND、1MbpsでACKする相手機器が必要。試験対象ノードID=1の送信ID 0x201が既存機器と衝突しない試験バスを使う。

参考：クロック源HSE対応は同梱stm32g4xx_hal_rcc_ex.hで確認。ST公式の[Classic CANサンプル](https://github.com/STMicroelectronics/STM32CubeG4/blob/master/Projects/STM32G474E-EVAL/Examples/FDCAN/FDCAN_Classic_Frame_Networking/readme.txt)と[クロック説明](https://community.st.com/stm32-mcus-60/faq-fixing-stm32-fdcan-communication-disruptions-apb-bus-kernel-and-time-quanta-clocks-141123)も参照。

## 実装段階

### 第1段階：FOC非接続のCAN単独試験

- can_protocol.c/.h：HALに依存しないパック・アンパック。ノードID、標準データフレーム、Classic、DLC=8を検証する。対象IDと2byteオフセットを選び、符号付きビッグエンディアンを復号する。
- can_comm.c/.h：HALによる初期化、受信、送信、統計、UARTデバッグ。初期送信は0x201、8byte全ゼロの固定テストデータ、10ms周期。実測フィードバックではないことをstatusに表示する。
- 割り込みは受信データ・受信時刻・番号・エラーの保存に限定。FIFO処理回数に上限を設ける。printf、文字列解析、FOC計算は実行しない。
- can statusとcan monitor on/offを用意する。表示はmainで間引き、取りこぼした表示数も記録する。全受信をUARTへ同期出力しない。
- この段階のcan armは「FOC未接続のため使用不可」と明示して拒否する。CANフレームからPWM開始・電流指令更新を行う経路は作らない。
- 受信数、採用数、不正形式、FIFO lost、バスエラー、bus-off、送信投入数と実送信完了数、送信skip、最後の受信値・経過時間を区別する。
- 例：0x200の先頭2byteが40 00ならノード1の表示値は+16384/+20A、C0 00なら-16384/-20A。他ノード分は採用しない。
- パック・アンパックをホスト単体テストし、Debug/Releaseビルド後に実機のACK・受信フィルター・10ms送信を確認する。

### 第2段階：観測値のみ接続

角度はAS5047Pの14bit値を右シフト1して0～8191。電気角やH2補正角は使わない。rpmは機械回転の符号付き値、Iqは実測値を20A/16384単位へ変換し、整数範囲へ飽和する。停止中の観測値と無効・古い値の扱いはstatusで区別する。温度byteは基板NTC温度の代用であることをUARTと文書に明記する。予約byteは常に0。速度・Iqは符号付き16bitビッグエンディアン。

### 第3段階：安全状態管理と電流PI接続

- FOC内部に文字列を経由しない起動・電流目標更新APIを設ける。既存の起動検査、制限、PI、保護処理を共有する。Id=0、Iq=制限後のCAN指令。
- 起動状態はDISARMED。UARTのcan armだけで許可し、既存動作・校正・ADCログとの競合は拒否する。アーム前の指令を破棄し、新規の有効フレームだけを起動条件とする。安全なゼロ指令ハンドシェイクを初期試験の仕様とする。
- 制御所有者をNONE/UART/CANで管理。CAN所有中のUART駆動・設定変更・校正開始を拒否し、statusやstop/can disarmは受け付ける。fault clearで再アームしない。
- 通信期限50ms、FIFO lost、bus-off、FOC保護停止で許可を解除しPWM停止。正常フレームが戻っても自動再起動せず、再アームを要求する。
- CANの安全監視は既存TIM側watchdogから定数時間で行う案。新規指令の公開と異常ラッチの整合性を保ち、CAN ISR中にFOCを呼ばない。
- HAL_GetTickを読み出した時刻だけでFIFO内フレームを「新着」と判定しない。ハードウェア受信timestampと現在timestampの差、世代番号、CPU側時刻を照合する。timestamp周回で鮮度が曖昧な場合は破棄・再同期する。FIFO lost時は全保留指令を無効化する。
- 最新値メールボックスを使い、古い指令のソフトウェアキューを再生しない。共有する値・時刻・番号・状態は短いクリティカルセクションで整合してコピーする。volatileだけで排他を済ませず、高優先度ISRで低優先度writerを待つスピンループも使わない。

## 1kHzへの移行評価

標準ID・8byteのClassic CANはintermissionを含み、スタッフなし111bit、スタッフを保守的に見込むと約135bit/フレーム。エラー・再送なしの概算：

| 構成 | バス占有率（1Mbps） |
|---|---|
| 1ノード、100Hzフィードバックのみ | 約1.11～1.35% |
| 1ノード、1kHzフィードバックのみ | 約11.1～13.5% |
| 1ノード、1kHz指令+1kHzフィードバック | 約22.2～27.0% |
| 8ノード、各1kHzフィードバックのみ | 約88.8～108% |
| 8ノード、各1kHzフィードバック+指令2IDを各1kHz | 約111～135% |

従って8ノードで指令2IDと全フィードバックを1kHzにする構成は1Mbpsでは成立しない。ノード数に応じて周期・バス分割を決める。送信キュー満杯時は待機せずskipし、古い未送信データを次周期へ蓄積しない。

CPU負荷は現段階では未測定。DWTでCAN IRQ最大cycles、main送信処理、既存FOCの最大時間・deadline違反・encoder ageを測る。160MHzで処理時間C cycles、頻度fならCPU比率はC*f/160000000。monitor off/on、受信集中、bus-off、FIFO lostも評価する。1ms化時にはmainのHAL_Delayやブロッキング処理の影響を測り、必要な場合だけ低優先度の周期処理を追加する。追加タイマが必要ならCubeMX設定を先に依頼する。

## 次の作業

調査時の現行コードはGNU Arm 14.3.1でDebug/Releaseとも新規ビルド成功。出力先はbuild/can_baseline_debugとbuild/can_baseline_release。DebugはFLASH 127008/129024 byte（残2016 byte）、RAM 15144 byte。ReleaseはFLASH 105904/129024 byte、RAM 15152 byte。Debugの空きが少ないため、CAN HAL追加後はサイズを必ず確認する。校正用Flash予約領域は削減せず、必要ならCAN関連ソースの最適化を検討する。実機試験・CPU負荷実測は未実施。

ユーザーによるCubeMX設定・再生成後、その差分を確認して第1段階のみ実装する。第1段階の実機送受信を確認してから観測値、最後にアームとFOC接続へ進む。現時点ではFW・.iocを変更せず、実機書き込みも行わない。
