# cal map自動H2補正・実機検証（2026-10-06）

COM6、921600 bps、無負荷、VM約24.2 V。ReleaseをST-LINKから書き込み、照合成功。
CubeMX・FOCゲイン・保護閾値の変更は行っていない。今回の実機試験でFWの修正は不要だった。

## Flash移行とmap

書込み前の校正ページを`build/h2_before_calibration_20261006.bin`へ保存。
旧version 1からdirection=+1、offset=1.961604 radを維持して起動。
H2係数はゼロ、stored=noとなり、実機セルフテスト659項目に合格した。

`cal map`は0.8 V、F/R各1000点で正常終了。相電流ピーク8.005 A、戻り誤差−0.055245 rad_elec。

| 結果 | Forward | Reverse |
|---|---:|---:|
| 定数項c [rad_elec] | −0.083527 | 0.014903 |
| a [rad_elec] | −0.053128 | −0.054175 |
| b [rad_elec] | 0.059730 | 0.060287 |
| 振幅 [rad_elec] | 0.079939 | 0.081052 |
| 位相 [degree、A cos(2θ−phase)] | 131.652496 | 131.943039 |

平均候補はa2=−0.053651512、b2=+0.060008548 rad_elec。
振幅0.080495405 rad_elec、位相131.798777°。
CSVから独立に最小二乗で再計算し、FWの小数6桁表示と1e−6 rad以内で一致した。
map完了時点では現在の係数ゼロ・direction/offset・保存状態が変わらないことを確認。

`cal map apply`でRAMへ適用、`cal save`で保存・読み戻し成功。
校正ページを`build/h2_after_calibration_20261006.bin`へ読み出し、version 2、CRC32、
direction/offsetを含む旧レコードのbytes 4～23の一致をPC側でも検証した。
リセット後、同じ係数とstored=yesで起動し、補正OFF・gain=+1を確認。

## FOC・ログ

| 条件 | 結果 | ADC制御処理最大 |
|---|---|---:|
| H2 OFF、Id=0/Iq=0.2 A、約1.6秒 | ほぼ静止、正常にuser stop | 27.631 µs |
| H2 ON、gain=+1、Id=0/Iq=0.5 A | 無負荷加速で約0.74秒後に600 rpm速度保護停止 | 31.506 µs |
| H2 ON、gain=+1、Id=0/Iq=0.3 A、約0.59秒 | ほぼ静止、正常にuser stop | 32.381 µs |

0.5 A試験は2秒の予定を完走していない。ホスト監視の450 rpm超過を検出し停止処理に入った時点で、
FWも600 rpm速度保護に到達して停止した。相電流ピーク約0.811 A。保護閾値は緩和していない。
0.3 A短時間試験の相電流ピーク約0.639 A、異常停止なし。

OFF 157点、ON 70点、ON短時間54点のraw角度・base角度・FOC角度・補正量を照合。
OFFでは補正量ゼロ、FOC角度とbase角度が一致。
ONでは`-(a2*cos(2θm)+b2*sin(2θm))`との最大差0.000522 rad、
`wrap(base+correction)`との最大差0.001 radで、小数3桁ログの丸めに整合した。
各試験のlog_overrun=0、UART errors/overrun/queue_drops/long_lines=0。

最終エンコーダー状態はspi/parity/sensor/timeout=0、first faultなし。
busy_ticks=13267、decode_waits=16006は非ゼロ。角度age最大は約233.1 µsで、250 µs上限以内。
取得見送りカウンターと鮮度余裕は引き続き監視対象とする。
この無負荷試験は、Id=0/Iq=8 A負荷時の2/rev低減効果や連続運転を再検証したものではない。

## 中止と最終状態

保存後にmapのMAP_RAMP段階でstopを送り、PWM/ADC停止、a2/b2・direction/offset・stored=yesの保持を確認。
新しい候補は無効のまま。終了時はRelease、保存済み係数あり、H2 OFF、gain=+1、PWM停止、ログidle、COM6解放済み。

主な記録は`build/h2_map_release_20261006.log`、`h2_apply_save_20261006.log`、
`h2_reboot_20261006.log`、`h2_foc_*_20261006.log`、`h2_abort_20261006.log`。
独立解析は`build/h2_fit_verified_20261006.json`と`h2_hardware_verified_20261006.json`。
