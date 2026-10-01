# cal map 実装・検証記録（2026-09-30）

## 変更範囲

| ファイル | 変更 |
|---|---|
| `Core/Inc/motor_control_config.h` | map専用0.4 V、5 A、1000点、28000 ms、移動量5%許容を追加 |
| `Core/Src/motor_calibration.c` | 独立したmap状態遷移、既存保護の共有、CSV、円周統計、校正値を保持する終了処理、selftest追加 |
| `Core/Inc/motor_calibration.h` | main用即時中止APIを追加 |
| `Core/Src/motor_control.c` | 2つの`stop`コマンド経路から校正中止APIを呼ぶ |
| `docs/serial_commands.md` | 操作条件、出力・式・停止条件を説明 |
| `tools/test_cal_map.py` | ビルドしたARM ELFを使う実機非接続テスト |
| `tools/test_cal_map_hardware.ps1` | COM11の実機selftest・往復測定・stage別中止試験、ログ保存、終了時の停止 |
| `tools/analyze_cal_map.py` | CSV点数・連番・誤差式・円周統計・校正値保持を独立に検証 |
| 本書 | 検証結果と実機手順 |

Flashレコード、通常校正の予備整列・往復・offset計算、FOC、CubeMX、PWM/ADC/SPI設定は変更しない。
`console.c`、`as5047p.c`は既存DMA出力・freshness判定を利用し、変更していない。

## メモリ・通信量

Releaseの校正モジュールBSSは145 → 190 bytes（+45 bytes）、リンク時の整列を含め約48 bytes増。
1000点分のRAMバッファはない。既存Consoleの512-byteリングを利用する。
ReleaseのTask自身のスタック使用量は176 → 224 bytes（+48 bytes、呼び先のprintf/libm等を除く）。
DebugではTask→MapTaskのスタックが重なるため、Releaseより余裕が小さくなる。
全体の最大スタック深さ・実時間余裕は実機で別途確認する。

CSVは約120～140 bytes/点 × 1000点/28秒 = 約4.3～5.0 kB/s。
8N1では約43～50 kbit/sで、921600 bpsの約5%。他のADCログ等は含まない。
printfはmainのみから既存Consoleリング→DMAを使う。追加のHAL_UART_Transmit呼出しはない。

## ビルド確認

STM32同梱GNU Arm 14.3.1、CMake/NinjaでRelease・Debugとも成功、コンパイラ警告なし。

| 構成 | Flash使用量（126 KiB中） | RAM使用量（32 KiB中） |
|---|---:|---:|
| Release | 95,164 bytes | 14,824 bytes |
| Debug | 116,252 bytes | 14,816 bytes |

## ARMエミュレーション

Unicorn 2.1.4 / pyelftools 0.33を使用する。ELF内の校正処理・CRC・角度計算・
CMSIS電圧変換・libmを実行し、PWM/ADC/VM/encoder取得・時刻・printfのみをスタブ化する。
物理モーターは理想追従モデルとし、脱調は移動量を6/7にして注入する。
Flash書込みAPIの呼出しはテスト失敗にする。

```powershell
python -m pip install --target build/cal_map_test_deps unicorn pyelftools
python -u tools/test_cal_map.py build/Release/HiradoraGen3FW.elf
python -u tools/test_cal_map.py build/Debug/HiradoraGen3FW.elf
```

確認項目:

- `cal test`: 既存586項目＋9項目（計595項目）、0 failures。
- direction ±1の両方で0/2πを跨いで完走し、F/R各1000点、連続index、誤差符号・戻り位置を確認。
- 全7段階で`cal stop`と`stop`が利用する中止APIを検査。PWM停止→ADC終了の順序を確認。
- 5 A超過、ADC飽和、encoder無効、VM低下・過電圧、main watchdog、ADC stale、nFAULT、
  1電気回転相当の脱調、測定target取りこぼしの10ケースで停止し、成功サマリを出さないことを確認。
- 正常終了・中止・異常終了後のCalibrationRecordがバイト単位で一致し、VALIDとstored=yesが保持される。

これは実機の`cal test`実行、UART DMAの実測、割り込み周期の保証、保護回路試験ではない。
エンコーダーやVMの実際の鮮度判定・ハードウェア制御はスタブの外であり、実機試験が必要。

## 実機確認（2026-09-30）

STM32G43x/G44x（128 KiB）、STLINK-V3MINIE（SN `002200473235510F37333439`）、
COM11 / 921600 bpsで試験。ユーザーが両方向へ自由に1回転できる状態であることを確認した。
開始時VMは約24.23 V。保存済み校正はdirection=1、offset=5.150699 rad、stored=yes。
書込み前の全128 KiBを`build/cal_map_before_flash_20260930.bin`に退避した。
書込みはELFの使用ページだけを消去し、最後の校正ページは対象に含めない。

Release・Debug版それぞれ1回ずつ、往復各1000点を正常取得。CSVをPC側で再計算し、誤差の符号、円周平均、
resultant、候補offset、正逆差がFW出力と一致した（照合許容0.0001 rad）。
測定点の欠落・重複はなく、取得間隔27～31 ms。CSV量は約3.3 kB/sだった。

| 観測量 | Release | Debug |
|---|---:|---:|
| `cal test` | 595 checks / 0 failures | 595 checks / 0 failures |
| 正転 / 逆転点数 | 1000 / 1000 | 1000 / 1000 |
| forward_travel（機械rad） | 6.259011 | 6.247499 |
| return_error_e（電気rad） | -0.034431 | -0.083537 |
| mean_error_forward（電気rad） | -0.219999 | -0.220468 |
| mean_error_reverse（電気rad） | -0.013208 | -0.013097 |
| mean_error_combined（電気rad） | -0.116590 | -0.116771 |
| bidirectional_difference（電気rad） | -0.206792 | -0.207371 |
| resultant_forward / reverse | 0.996161 / 0.996426 | 0.996172 / 0.996391 |
| candidate_offset（rad、表示のみ） | 5.034109 | 5.033928 |
| 全ADCサンプル最大電流（A） | 3.751 | 3.759 |
| encoder / serialエラー | 0 / 0 | 0 / 0 |

中止試験は以下の12条件すべて成功。各試行の終了後、PWM停止、ADC idle、校正値・保存状態の保持を確認した。
中止試験中もencoder/serialエラーは0だった。

| 中止時の段階 | Release `stop` | Release `cal stop` | Debug `stop` | Debug `cal stop` |
|---|---|---|---|---|
| MAP_RAMP | 成功 | 成功 | 成功 | 成功 |
| MAP_FORWARD | 成功 | 成功 | 成功 | 成功 |
| MAP_BACKWARD | 成功 | 成功 | 成功 | 成功 |

終了時はRelease版を書込み・照合して再起動し、595項目のselftestを再度確認した。
PWM停止、ADC idle、fault=none、COM11解放済み。
校正Flashの最後2 KiBを読み出し、試験開始前のバックアップと**2048 bytesすべて一致**した。
校正ページSHA-256: `5ed054057782a1d4ec6fbb1ea5d8caa0fb26e2e5862094ce3e0242b12e3164a5`。

保存値は変更せず、`cal save`・`cal start`は実行していない。
正逆差があるため、candidate_offsetをセンサー単体の誤差補正値と断定しない。
各CSVには摩擦・追従遅れ・コギング等が含まれる。

実機ではdirection=+1の既存配線だけを使用した。direction=-1はエミュレーションのみ。
今回の実機試験で過電流・ADC飽和・VM異常・nFAULT・watchdogを意図的に発生させてはおらず、
これらの異常注入10ケースの結果は上記エミュレーションと区別する。
停止応答はシリアル応答とPWM停止状態で確認し、オシロスコープによる停止遅延の測定は行っていない。
発熱は温度センサーによる記録を行っておらず、今回の低電圧試験は熱評価を代替しない。

### 生ログ・解析結果

ファイルはすべてローカルの`build`配下にある。`.log`は基板から受信したUART出力、
`.json`はPC側でログを解析した結果。`.bin`はFlashから読み出したバイナリである。
角度誤差の測定結果を見たい場合は、まず`cal_map_analysis_20260930.json`で要約を確認し、
個々の測定点は`cal_map_release_full_20260930.log`を参照する。

| ファイル | 何を記録したものか・確認する箇所 |
|---|---|
| [cal_map_preflight_20260930.log](../build/cal_map_preflight_20260930.log) | 新FWを書き込む前の接続確認。元の校正値・保存状態、PWM停止、VM、encoder・通信エラーを記録。試験前の状態との比較用。 |
| [cal_map_release_selftest_20260930.log](../build/cal_map_release_selftest_20260930.log) | Release書込み直後の停止状態での試験。`Calibration selftest: 595 checks, 0 failures`と、校正値・PWM停止・ADC idleを確認できる。往復測定のCSVは含まない。 |
| [cal_map_release_full_20260930.log](../build/cal_map_release_full_20260930.log) | Releaseで往復を完走した測定の生ログ。`CALMAP_BEGIN`に設定、`CALMAP,F,...` / `CALMAP,R,...`に正逆各1000点、`CALMAP_SUMMARY`以降に統計・移動量・最大電流がある。前後の校正状態とエラーカウンターも含む。 |
| [cal_map_debug_full_20260930.log](../build/cal_map_debug_full_20260930.log) | Debugで同じ往復測定を完走した生ログ。構成はRelease版と同じ。ビルド構成による測定値・欠落・エラーの違いを比較するために使う。 |
| `cal_map_release_stops_20260930_MAP_*_*.log` / `cal_map_debug_stops_20260930_MAP_*_*.log` | 指定段階で意図的に中止した試験。各構成6ファイル、計12ファイル。`CALMAP_STOP,reason=aborted`、中止stage、最大電流、終了後のPWM停止・ADC idle・校正値保持を見る。途中までのCSVであり、正常完走の`CALMAP_END`がないのは想定どおり。 |
| [cal_map_analysis_20260930.json](../build/cal_map_analysis_20260930.json) | ReleaseとDebugの完走ログを独立に解析した要約。`firmware_summary`はFW出力の転記、`passes.F` / `passes.R`はCSVから再計算した点数・間隔・平均誤差・resultant・誤差範囲・通信量。`candidate_offset`等の再計算値とFW値の一致も検証している。 |
| [cal_map_stops_analysis_20260930.json](../build/cal_map_stops_analysis_20260930.json) | 中止試験12条件の一覧。`log`が元ログ、`configuration`がRelease/Debug、`stage`が中止段階、`peak_current_A`が試行中の最大電流。解析時にPWM停止・ADC idle・校正値保持も照合しているが、それらの状態文字列は元ログで確認する。 |
| [cal_map_final_release_20260930.log](../build/cal_map_final_release_20260930.log) | 全試験後にReleaseへ戻して再起動した際の最終確認。595項目のselftest、校正値、PWM停止、ADC idle、fault=noneを記録。試験終了時の状態を知りたい場合に参照する。 |
| [cal_map_before_flash_20260930.bin](../build/cal_map_before_flash_20260930.bin) | 書込み前のFlash全128 KiBのバックアップ。末尾2 KiB（ファイル内offset `0x1F800`～`0x1FFFF`）が元の校正ページ。テキストエディターで読むログではない。 |
| [cal_map_after_calibration_page_20260930.bin](../build/cal_map_after_calibration_page_20260930.bin) | 全試験後に`0x0801F800`から読み出した校正ページ2 KiB。上記バックアップの末尾2 KiBとバイト単位で比較した対象。 |
| [cal_map_flash_verification_20260930.txt](../build/cal_map_flash_verification_20260930.txt) | 校正ページ照合の結果。`2048 bytes identical`が完全一致を示し、SHA-256も記録。Flashが保持されたことを簡潔に確認するためのファイル。 |

中止試験のファイル名は、例えば
[cal_map_release_stops_20260930_MAP_BACKWARD_cal_stop.log](../build/cal_map_release_stops_20260930_MAP_BACKWARD_cal_stop.log)
なら「Release版で、復路の途中に`cal stop`を送った試験」を意味する。
`MAP_RAMP`は電圧立ち上げ中、`MAP_FORWARD`は往路、`MAP_BACKWARD`は復路。
末尾の`_stop`と`_cal_stop`は、それぞれ送信した`stop`と`cal stop`を区別する。

中止ログの`elapsed_ms`と中止解析JSONの`stage_elapsed_ms`は、**そのstageが始まってから中止するまでの時間**。
コマンド受信からPWM停止までの応答遅延を測った値ではない。
また、解析JSONの`max_logged_current_A`はCSVに間引いて出力された点の最大値であり、
`firmware_summary.peak_current`は全ADCサンプルの最大値なので、両者は一致しないことがある。

`cal_map_release_analysis_20260930.json`と`cal_map_release_stops_analysis_20260930.json`は、
Release試験終了時点で作成した途中の解析結果。Release/Debugの両方を見る場合は、
上表の`cal_map_analysis_20260930.json`と`cal_map_stops_analysis_20260930.json`を参照する。

再現用コマンド（停止・可動条件を確認してから実行）:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/test_cal_map_hardware.ps1 -Mode Selftest -Log build/cal_map_selftest.log
powershell -NoProfile -ExecutionPolicy Bypass -File tools/test_cal_map_hardware.ps1 -Mode Map -Log build/cal_map_full.log
powershell -NoProfile -ExecutionPolicy Bypass -File tools/test_cal_map_hardware.ps1 -Mode StopMatrix -Log build/cal_map_stops.log
python tools/analyze_cal_map.py build/cal_map_full.log --output build/cal_map_analysis.json
```

PowerShellの実行許可はそのプロセスだけに指定する。Windows全体の実行ポリシーは変更しない。
試験スクリプトは例外・正常終了のどちらでも最後に`stop`、`adc stop`を送り、COMポートを閉じる。

## 実機確認の再現手順

1. 自由に両方向へ1回転できるローターを用意し、既存の有効な校正値と保存状態を控える。
2. Releaseを書き込み、921600 bpsで生のUARTログを保存する。`stop`、`adc stop`後、idleを待つ。
3. `cal test`が595 checks / 0 failuresとなることを確認する。未校正なら先に従来の`cal start`を実施する。
4. `cal status`を記録し、`cal map`を開始する。0.4 V ramp、往復各約28秒、計約59秒の挙動と電流・発熱を確認する。
5. F/R各1000行、index 0～999、全列、`CALMAP_END`、往路約direction×2π、戻り電気角誤差0.15 rad以内を確認する。
6. 前後の`cal status`でdirection/offset/VALID/storedが同じことを確認する。candidate_offsetは自動適用しない。
7. 別試行でramp・往路・復路それぞれに`stop`と`cal stop`を送り、即時出力停止と校正値保持を確認する。
8. 管理された試験環境で各保護停止と通信負荷を確認する。`adc stop`後も電流保護が継続することを確認する。
9. Debugでも同じ手順を行い、main watchdog・encoder stale・UART欠落の有無を確認する。

誤差・候補offset・正逆差はセンサー固有誤差だけでなく摩擦・追従遅れを含む。
校正値の更新や補正LUT化は、この測定結果を評価してから別変更で扱う。
