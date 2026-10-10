# FOC sin/cos共有・再正規化省略と実機測定（2026-10-09）

同じ負荷で変更前／変更後を各3回比較した結果は[負荷付きPI反復比較](foc_loaded_ab_2026-10-10.md)を参照。
ADC制御区間最大の代表値はH2 OFFで10.2%、ONで8.5%短縮。H2 ONのdecode待ちは残る。
残る待ちについて、[応答DMAの中断と角度公開のタイミング](encoder_wait_timing_2026-10-10.md)を調査した。
その後の[角度コピー直後への取得開始移動試験](encoder_early_request_2026-10-10.md)では、H2 ONの待ちを大幅に減らしたが、OFFのage最大は増加した。

## 実装

1. 電流Park変換で求めたsin/cosを同じADC周期の観測に保持し、逆Park変換へ渡す。
   `observation_valid`による同一周期の有効性確認を維持し、無効観測時には出力しない。
2. 正規化済みの`observed_electrical`に対する`fmodf`を省略する。
   `VoltageVector_SinCosWrapped()`を追加し、負の微小角の正規化が単精度丸めでちょうど2πになる端点も処理する。
3. 既存の角度指定APIは校正等のために保持し、sin/cos指定APIと電圧制限・SVPWM・CCR公開処理を共有する。
   非有限角度の拒否は、FOC観測からsin/cosを公開する前に明示的に保持する。

H2 OFFのsin/cosペア計算は2→1回、`fmodf`は3→1回。
H2 ON・gain≠0・補正値≠0ではsin/cosは3→2回、`fmodf`は5→3回。
H2の計算式、基準電気角の算出方法、CORDIC設定、PIゲイン、PWM/ADC/SPI設定は変更していない。

## ビルド・検証・書込み

- Debug/Releaseビルド成功。最終Release ELF SHA256：
  `9304C94D87CDE33542178DE5D2CCA6284D73E1C175A0675D5C36B89F444FC1C1`。
- `test_foc_trig.py`：変更前ELFとの1025点＋境界値のsin/cos一致とPWM duty比較。
  通常・ゼロ出力・電圧飽和条件で最大duty差0。非有限値・無効観測の拒否、
  CurrentISR→TickISRの共有ペア、H2 OFF=1/ON=2回の呼出しをDebug/Releaseで確認。
- `test_encoder_h2.py`、`test_current_pi.py`：最終Debug/ReleaseでPASS。
- `test_motor_period.py`：角度指定とsin/cos指定の両経路のCCR一致、50 µs周期、ARR検証がPASS。
- 校正共通経路の変更後に`test_h2_calibration.py`、`test_cal_map.py`がPASS。
  後者はcal test 659件・失敗0、正逆各1000＋1000点、停止・故障シナリオを含む。
- ST-LINK `0048003F3234510A37333934`で最終Releaseを書込み、Verify成功。
- 校正ページ0x0801F800 / 2048 Bの書込み前後SHA256一致：
  `EF71DD686D933F0A3DFA59F6B6654CA0E304F246908DBFA99AEA4A7B51153ADF`。
- `arm-none-eabi-size`による変更前比：text +280 B、data ±0 B、bss +8 B。
  実行時間を短縮する変更であり、コード容量は少し増える。

## 電圧FOC：変更前後の比較

COM6 / 921600 baud、Vd=0 / Vq=+0.3 V、各約10秒、状態照会約1秒間隔、
ADC詳細ログOFF。最終版の測定は21:47 JST。
VM約24.6 V、NTC約36.3～36.7 ℃。変更前は約28 ℃だったため完全な同条件ではない。
各条件1運転の累積最大値であり、平均・分位点・三角関数単体時間ではない。

| 条件 | compute_max 前→後 [µs] | adc_control_max 前→後 [µs] | ADC区間の短縮 |
|---|---:|---:|---:|
| H2 OFF | 11.050 → 8.525 | 23.718 → 20.850 | 2.868 µs / 12.1% |
| H2 ON | 11.050 → 8.525 | 26.850 → 24.812 | 2.038 µs / 7.6% |

`compute_max`はTickISR区間、`adc_control_max`は電流処理を含むADC制御区間。
正確な計測範囲は[変更前記録](foc_timing_baseline_2026-10-09.md)を参照。
後者にもIRQ入口・出口や締切チェック後の処理は含まれない。

最終運転中照会のticksはH2 OFF=206162、ON=202464。
angle_age_maxは89.775 / 89.843 µs。相電流ピーク表示は両条件1.352 A。
両条件ともfault=noneで計画終了し、SPI/parity/sensor/timeout、busy_ticks、decode_waits、UARTエラーは0。

## 負荷接続前の電流PI試験

非有限角度チェック追加前の中間版（ELF SHA256
`4A488AC9DCD725DF84C0CFCE66A50F615700EA121047477AAD6A8A9B347EE8B3`）で、
Iq=0.5 A、H2 OFFを測定したところ、約1秒で速度保護停止した。
照会時519 rpm、停止後表示600 rpm。相ピークは停止通知0.858 A。
compute_max=12.581 µs、adc_control_max=24.750 µs、angle_age_max=104.556 µs。
これらは最終版・安定10秒運転の比較値には採用しない。H2 ONは未実施で打ち切った。
PIは速度制御ではなく、前回のH2 ONだけでなくOFFでも加速した。

## 負荷接続後の電流PI再測定

ユーザーの負荷接続後、最終版FWで21:49～21:50 JSTに再測定。
Id=0 / Iq=+0.5 A、H2 gain=+1、各50回の運転中状態照会（各約11秒）。
監視頻度を約200 msへ上げ、ホストの速度上限200 rpm・相ピーク上限5 Aで停止する設定。
ADC詳細ログはOFF。VM約24.5 V、NTC約36.4～36.7 ℃。

| 条件 | compute_max [µs] | adc_control_max [µs] | angle_age_max [µs] | 最後の運転中ticks | 相ピーク表示 [A] |
|---|---:|---:|---:|---:|---:|
| 電流PI、H2 OFF | 12.568 | 23.675 | 85.768 | 221922 | 0.897 |
| 電流PI、H2 ON | 12.568 | 26.412 | 86.050 | 220365 | 0.956 |

両条件ともfault=noneで完了。照会で得たrpmは全体で-5～+4、ほぼ静止。
最後の運転中Iq表示はOFF=0.541 A、ON=0.476 A。
SPI/parity/sensor/timeout、UARTエラーとbusy_ticksは0。
decode_waitsはOFFで増加0、ONで45増加。待ちが完全になくなったわけではない。

変更前のPI測定から負荷・照会頻度・温度が変わったため、この表から最適化だけの短縮率は算出しない。
今回の表を負荷付き・最適化済みFWの基準値とする。値はいずれも運転中の累積最大値。
負荷の種類・トルクは未定量で、これ以外の負荷・電流条件への一般化はしていない。

最終状態はPWM停止、H2 OFF、ADC/角度/NTCストリームOFF、COM6解放済み。
停止理由の`user stop`は計画終了時の停止指令によるもの。

## 証跡（build以下はGit管理外）

- 最終電圧試験：`build/foc_trig_final_20261009_214708.log`、同名`.json`、`_commands.log`。
- 負荷付きPI：`build/foc_trig_loaded_pi_20261009_214924.log`、同名`.json`、`_commands.log`。
- 負荷接続前の中間版試験：`build/foc_trig_after_20261009_214403.log`、同名`.json`、`_commands.log`。
- 再起動確認：`build/foc_trig_final_reboot_20261009.log`。
- 校正退避：`build/trig_cal_before_20261009.bin`、`build/trig_cal_after_20261009.bin`。
- 比較元ELF：`build/foc_before_trig_20261009.elf`。
- 測定スクリプト：`build/measure_foc_trig_final.ps1`、`build/measure_foc_trig_loaded_pi.ps1`。

生受信とコマンド注記を別ファイルに保存し、受信行へ注記を混ぜない。
