# 検証ツール

リポジトリのルートから実行する。出力ログはGit管理外の `build/` に保存する。
循環DMA評価時のソース・専用ツールはコミット `100cccf` に保存済み。
現在のエンコーダー実機ツールは通常DMAのログ形式を対象とする。
CubeMX再生成と書き込みが終わるまでは、実機テストを実行しない。

## 実機を使わない回帰テスト

依存ライブラリは `unicorn` と `pyelftools`。
`test_current_pi.py` の `Firmware` は他のテストの共通基盤。
`test_cal_map.py` もH2校正テストから参照されるため残している。

| ファイル | 検証対象 |
|---|---|
| `test_current_pi.py` | PI演算・飽和・コマンドの制約 |
| `test_encoder_h2.py` | 角度のH2補正 |
| `test_h2_calibration.py` | H2校正・保存形式 |
| `test_cal_map.py` | 校正・異常時の停止 |
| `test_motor_period.py` | TIM1の50 µs周期・CCR計算 |
| `test_foc_trig.py` | 変更前ELFとのPWM一致、同一ADC周期のsin/cos共有、呼出し回数、無効観測の拒否 |
| `test_encoder_request_order.py` | 角度コピー後の取得開始、周期末尾との重複防止、起動準備中の取得、無効/古い角度の拒否 |

```powershell
$env:PYTHONPATH='build/encoder_test_deps;tools'
python tools/test_motor_period.py build/Debug/HiradoraGen3FW.elf build/Release/HiradoraGen3FW.elf
python tools/test_current_pi.py build/Debug/HiradoraGen3FW.elf build/Release/HiradoraGen3FW.elf
python tools/test_foc_trig.py build/foc_before_trig_20261009.elf build/Debug/HiradoraGen3FW.elf build/Release/HiradoraGen3FW.elf
```

実ELFのARM命令を実行するが、電気的なSPI波形・実際の割り込み遅延は対象外。
循環DMA専用の周辺モデルと開始位相テストは削除した。通常DMAの実通信は再生成後に実機で確認する。

## 実機でのログ取得

| ファイル | 用途 |
|---|---|
| `check_encoder_stopped.ps1` | 停止指令後の状態確認 |
| `test_encoder_stopped_soak.ps1` | PWM停止・ADCログ有無の継続試験 |
| `test_encoder_hardware.ps1` | 低電圧FOCの回転試験。通常DMA用に改名・整理 |
| `test_cal_map_hardware.ps1` | 校正の実機試験 |
| `measure_current_pi.ps1` | 電流PIの実機測定 |
| `measure_h2_voltage.ps1` | 電圧FOCでのH2比較測定 |

回転・校正ツールはモーターを駆動する。電源・負荷条件と各パラメーターを確認して使用する。
エンコーダー回転試験は既存の電圧・速度・電流上限と `finally` の停止処理を維持した。
`busy_ticks` / `decode_waits` は待ちの累積値として記録し、通信エラーとは分けて評価する。

再生成・書き込み後の停止確認：

```powershell
powershell -NoProfile -File tools/check_encoder_stopped.ps1 -Port COM6
```

## 保存済みログの解析

`analyze_encoder_timing_probe.py <log>...`は調査ビルドの`angle timing dump`を解析する。
要求時刻基準のµsへ変換し、ADC/DMA同時active、公開前後の角度sequence対応を検証する。
通常FWの待ち頻度を測るツールではない。詳細は`docs/encoder_wait_timing_2026-10-10.md`。

`analyze_timing_log.py`、`analyze_current_observation.py`、`analyze_cal_map.py`、
`analyze_pi_tuning.py`、`analyze_pi_endurance.py`、`analyze_h2_endurance.py` を残した。
実機接続なしで解析できる。引数は各ファイルのヘルプまたは末尾のCLI定義を参照。

削除したもの：循環DMA専用テスト、固定のA/B・50 µs・位相比較解析、IRQトレース解析、
特定日付のH2集計、ソース変更・書き込み・6/10 A試験を一括実行するPI候補探索スクリプト。
過去文書に出てくる削除済みツール名は当時の記録として残す。再現には `100cccf` を参照する。
既存の `build/` 内ログ・校正バックアップ・依存ライブラリは削除していない。
