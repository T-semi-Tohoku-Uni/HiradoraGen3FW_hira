# FOC角度コピー直後へのSPI取得開始移動（2026-10-10）

## 追加評価：H2 ONで約66秒×2条件（2026-10-10 13:28）

同じ負荷の接続をユーザーに確認し、現在のコピー直後開始版を評価した。
**H2 ONでの開発基準として暫定採用を推奨する。ただし詳細ログ併用時の取得見送りは残る。**
高速回転や実運用電流での評価は今回の範囲に含まない。

FWの再書込み・演算変更はしていない。ST-LINKでFlash先頭103960 Bを読み出し、
Release ELFから生成したbinとSHA256一致を確認して再起動した。
両binのSHA256は`6A152F1159A583607008749BE045C9B64201672F7E7AF1EE03C4F70C9B5E8D47`。
Programmerの単独`-v ELF`は非対応だったため、読出し比較で同一性を確認した。

条件はCOM6 / 921600 baud、Id=0 / Iq=+0.5 A、H2 ON / gain=1、校正VALID。
ログなし→`adc 200`（100 Hz）の順に各300回状態照会し、各運転間に停止・温度確認した。
両条件とも約5 Hzの`foc status`照会を含む。「ログなし」はADC詳細ログなしを指す。
速度200 rpm超・相ピーク5 A超・異常状態で中断し、停止時のNTCは50 ℃未満を確認する手順。

| 条件 | 最終運転中照会ticks / 20 kHz | compute_max [µs] | adc_control_max [µs] | angle_age_max [µs] | decode_waits増分 |
|---|---:|---:|---:|---:|---:|
| 詳細ログなし | 66.11秒相当 | 12.443 | 30.875 | 112.306 | 137 |
| 詳細ログ100 Hz | 66.65秒相当 | 12.443 | 33.025 | 162.700 | 97,962 |

時間の最大値は各運転の累積値。待ち回数は停止前後の角度状態照会差分で、
表のticksと集計区間は完全には一致しない。概算の発生頻度は約2回/秒→約1470回/秒。
ログなしの137回は前回の約11秒で14～20回と同程度の低い頻度だが、完全な0ではない。
100 Hzログでは頻度と最大ageが増えた。今回、割り込み時刻の個別追跡は行っておらず、
UART送信・文字列整形などのどの処理が支配的かは未分離。

100 Hzログの6661点・66.60秒分を解析し、以下を確認した。

| ADCコールバック入口でのage [µs] | 最小 | 中央値 | p95 | 最大 |
|---|---:|---:|---:|---:|
| 要求開始から（angle_age_us） | 43 | 43 | 43 | 143 |
| 応答解析開始から（angle_rx_age_us） | 5 | 6 | 6 | 105 |

- 要求ageが100 µs以上だった記録は13点 / 6661点（約0.20%）。
- 9系列の時刻一致、sampleの増分200、全点log_overrun=0を確認。記録欠落なし。
- Id中央値=-0.001 A、Iq中央値=0.500 A。Iqの記録範囲は0.257～0.790 A。
- 両運転中600回の状態照会でfault=none、観測rpm=-5～+4、最大相ピーク0.953 A。
- SPI/parity/sensor/timeout/busy_ticks増分0。最終UARTエラー・overrun・queue_dropsも0。
- NTCは開始31.16 ℃、1運転後32.73 ℃、2運転後最大34.06 ℃。最終VM=24.195 V。

この分布は20 kHz制御から200回に1回を抜き出したもので、全周期の分布ではない。
間引きと取得タイミングの関係で偏る可能性があり、13点の割合を全制御周期の遅延率とはしない。
表の`angle_age_max`はTickISRまで、詳細ログはADC入口までなので直接比較しない。
どちらもセンサー内部の実測時刻を表すものではない。

判断：通常のログなし運転では前回の改善が約66秒でも維持された。
H2 ONで開発を進めるため、この変更を残すのが妥当。
一方、100 Hz詳細ログを常時有効にする運用は別途評価が必要で、取得見送りが解消したとは扱わない。
次の評価はログ負荷を下げた際の変化と、予定する速度・電流・負荷範囲での確認とする。
今回は変更前版の長時間A/Bを行っておらず、高速運転までの最終採用判断は保留。

終了時はPWM停止、FOC off、H2 OFF、ADC logger idle、COM6解放。
最終fault表示は正常な手順停止による`user stop`で、運転中の保護停止ではない。

証跡（Git管理外）：

- `build/encoder_early_endurance_20261010_132847.log`：未加工受信ログ。
- 同名の`.json`、`_commands.log`、`_summary.json`：状態照会・コマンド・解析結果。
- `build/measure_encoder_early_endurance.ps1`、`build/analyze_encoder_early_endurance.py`：測定・解析。
- `build/encoder_early_eval_flash.bin`、`build/encoder_early_eval_expected.bin`：実機Flashと比較用bin。

## 結果

角度コピー直後へ次回取得開始を移動した試験版で、負荷付きPIのH2 ONにおける
decode_waitsは約1.3万回から14～20回へ減少した。2回の比較で再現したが、完全な0ではない。
角度age最大はH2 ONで約136→113 µsへ減少した一方、OFFでは約86→109 µsへ増加した。
全条件で鮮度が改善したとはいえない。全8運転は保護停止・通信エラーなし。

## 変更範囲

- 運転中は`FocVoltage_CurrentISR()`で`AS5047P_GetSample(&cycle_angle)`が成功し、
  鮮度を検証した直後に`AS5047P_Tick()`を呼ぶ。Park変換・H2補正・PIより前。
- `cycle_angle`はコピー済みのまま維持し、その角度のsin/cosをPark/逆Parkで共有する。
- `MotorControl_FocAdcISR()`末尾では運転中の取得要求を削除。
- 起動準備中はCurrentISRが角度コピー前にreturnするため、末尾の要求を残す。
  `FocVoltage_IsRunning()`で区別し、二重要求と起動時の取得停止を避ける。
- 割り込み優先度、タイマ位相、SPI設定、PI/H2演算式は変更しない。
- 今回の測定では調査用`ENCODER_TIMING_PROBE`は定義しない。

## ビルドと回帰検証

Debug/Releaseビルド成功。Release使用量はFLASH 103960 B / 126 KiB、RAM 15152 B / 32 KiB。
`test_encoder_request_order.py`で実ARM命令のCurrentISRとADC末尾を実行し、以下を確認した。

- 有効角度コピー→次回取得→Park→TickISRの順。
- 取得要求後にセンサーの公開データが変わっても、現在のcycle_angleは変化しない。
- 運転中の要求は1回のみで、ADC末尾では再要求しない。
- 起動準備中は末尾で1回要求する。
- 無効／古い角度では次回要求せず保護停止する。

同テストと`test_encoder_h2.py`、`test_foc_trig.py`はDebug/ReleaseでPASS。
`test_current_pi.py`、`test_motor_period.py`はReleaseでPASS。
新規テスト初回はテスト側の`control_tick_hz`未設定で締切保護が作動したため、
実機と同じ20 kHzを設定して再実行した。FW側の保護条件は変更していない。

## A/B実機条件

- A：sin/cos最適化済み、取得はFOC末尾。
  ELF：`build/foc_trig_production_20261010.elf`。
  SHA256：`9304C94D87CDE33542178DE5D2CCA6284D73E1C175A0675D5C36B89F444FC1C1`。
- B：今回のコピー直後開始版。ELF：`build/Release/HiradoraGen3FW.elf`。
  SHA256：`A8B29EFEB0F0D74419DF35E9F6300B05376AE89FF4B9CFC7C060F62A2C4A9BCB`。
- 同じ負荷を接続、COM6 / 921600 baud、Id=0 / Iq=+0.5 A、H2 gain=+1。
- A1→B1→A2→B2の順。各書込みをVerify・再起動し、H2 OFF→ONを測定。
- 各条件50回の状態照会（約200 ms間隔、約11秒）。ADC詳細ログはOFF。
- NTC約34.2～34.8 ℃。観測rpmは-6～+4、最大相ピーク表示0.822 A。
- SPI/parity/sensor/timeout、UARTエラー、busy_ticks増分は全条件0。
- 表の時間は最後の運転中照会の累積最大値。平均や全周期の分布ではない。

| 運転 | H2 | compute_max [µs] | adc_control_max [µs] | angle_age_max [µs] | decode_waits増分 |
|---|---|---:|---:|---:|---:|
| A1 | OFF | 12.568 | 24.537 | 85.856 | 0 |
| A1 | ON | 12.568 | 27.356 | 135.906 | 13,194 |
| B1 | OFF | 12.443 | 27.637 | 108.931 | 0 |
| B1 | ON | 12.443 | 30.393 | 112.968 | 20 |
| A2 | OFF | 12.568 | 24.537 | 85.837 | 0 |
| A2 | ON | 12.568 | 27.356 | 135.937 | 13,158 |
| B2 | OFF | 12.443 | 27.637 | 108.881 | 0 |
| B2 | ON | 12.443 | 30.400 | 112.968 | 14 |

待ちカウンターは停止前後を含む各運転前後の差分。時間が完全に同一ではないため、
減少率を厳密な毎周期の発生確率として扱わない。

## 読み方と制約

`adc_control_max`はSPI開始処理の移動により計測に含まれる処理が変わった。
Aでは締切チェック後にあった開始処理が、Bではチェック前に入る。
そのため約3 µsの表示増加を、そのまま全ADC IRQのCPU負荷増加とは解釈できない。
全IRQ入口から出口までを同じ範囲で比較する測定は今回していない。

`angle_age_max`はセンサー内部の測定時刻ではなく、要求時刻からTickISRまでの時間。
要求を早めるだけでもこの値は増える。H2 OFFの約23 µs増加はその方向と整合するが、
個々のサンプルの取得・公開時刻を追跡して内訳を分離したわけではない。
H2 ONでは要求見送りが大幅に減り、age最大も小さくなった。
一律に新鮮になった、全周期で新角度を使えた、という結論にはしない。

要求フレームはFOC演算と重ねられるが、DMA割り込みはADCより低優先度のまま。
応答フレームの開始はADC終了を待つため、SPI全体が並行するわけではない。
変更後のtransfer_maxはOFF約36.69 µs、ON約39.45 µsとなった。
この指標にはソフトウェア割り込み待ちが含まれ、SPI通信速度を変更した結果ではない。

この段階ではH2 ONの見送り低減を確認した試験結果として扱う。
H2 OFFのage増加とのトレードオフがあるため、最終採用の判断には通常時のage分布と
残る14～20回の発生条件を確認する余地がある。今回それ以上の変更は行っていない。

## 最終状態・証跡

全比較成功後、Bの試験版を書込み・Verify・再起動済み。
PWM停止、H2 OFF、FOC fault=none、校正VALID/stored=yes、ストリーム停止、COM6解放を確認。
校正ページ0x0801F800 / 2048 Bは前後SHA256一致：
`EF71DD686D933F0A3DFA59F6B6654CA0E304F246908DBFA99AEA4A7B51153ADF`。
変更前ELFは保持。コミット・pushは実施していない。

Git管理外の証跡：`build/encoder_early_ab_20261010_024843/`

- A1/B1/A2/B2の`.log`、`.json`、`_commands.log`。
- `summary.json`：各運転の集計。集計スクリプトのADC短縮率欄は計測範囲が異なるため性能比較に使わない。
- `flash_*.log`、`flash_restore_final.log`：書込み・Verify記録。
- `cal_before.bin`、`cal_after.bin`、`cal_hashes.json`。
- `final_state.log`：終了後の状態。
- 実行：`build/run_encoder_early_ab.ps1`、集計：`build/analyze_encoder_early_ab.py`。
