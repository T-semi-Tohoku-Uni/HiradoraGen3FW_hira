# H2補正・負荷あり約30秒比較（2026-10-06）

Id=0 A、Iq=8 Aで、補正OFFとON（gain=+1）を別運転として比較した。
今回の条件では、Idの2/rev振幅は12.246 mAから3.251 mAへ約73.5%低減した。
ONの序盤から終盤に向けて悪化する傾向はなく、補正演算と保存係数も維持されていた。

## 条件

- COM6、Release、既存PI設定のまま。FW・係数・Flashは変更していない。
- 負荷接続後にIq=1 Aの短時間確認を実施。OFF→ONの順に各約29.6秒、間に停止時間を設けた。
- a2=−0.053652、b2=+0.060009 rad_elec。direction=+1、offset=1.961604 rad。
- ログは`adc 200`、約100 Hz。OFF 2,962点、ON 2,960点で、sample差分はすべて200、角度とdqの時刻が一致。
- 2/rev評価は立ち上がりを除く2～29秒の各2,701点を使用。
- `Id=c+a*cos(2*mech_raw_rad)+b*sin(2*mech_raw_rad)`を最小二乗フィット。
  以下の振幅は`hypot(a,b)`（片振幅）で、peak-to-peakではない。

## 比較結果

| 指標 | OFF | ON（gain=+1） |
|---|---:|---:|
| 停止要求まで | 29.648 s | 29.638 s |
| 2～29秒の平均rpm（raw角度から算出） | 164.86 | 161.13 |
| 同区間の機械回転数 | 74.19回 | 72.51回 |
| 同区間の平均Iq | 7.9972 A | 7.9993 A |
| Idの2/rev振幅 | 12.246 mA | 3.251 mA |
| FW保持の相電流ピーク | 8.515 A | 8.460 A |
| ADC制御処理最大 | 29.606 µs | 32.612 µs |
| NTC・運転前→後 | 35.43→37.02 ℃ | 36.42→37.90 ℃ |
| ログoverrun | 0 | 0 |

全運転で異常停止なし。停止理由はuser stop。
UART errors/overrun/queue_drops/long_lines=0、取得したPI状態のsaturatedはすべて0。

## 時間経過

| 区間 | OFF Id 2/rev振幅 | ON Id 2/rev振幅 |
|---|---:|---:|
| 2～10秒 | 11.199 mA | 6.532 mA |
| 10～20秒 | 9.857 mA | 2.644 mA |
| 20～29秒 | 16.053 mA | 3.252 mA |

ONの全区間でOFFより小さく、後半に低減効果が失われる様子はなかった。
ただし各条件1回で、回転数・温度は完全一致していない。上記は今回観測した低減率であり、
再現性の統計評価や長時間の熱平衡試験ではない。

## 補正式とデータ整合性

全定常区間で、`elec_corr_rad`と`-(a2*cos(2θm)+b2*sin(2θm))`を照合。
ONの最大差は0.000574 rad、`elec_rad`と`wrap(elec_base_rad+elec_corr_rad)`の最大差は0.001186 rad。
いずれも角度ログ小数3桁の丸めに整合する。OFFは補正量ゼロ、base角とFOC角が完全一致。
試験終了時も保存係数・direction/offset・stored=yesが維持されていた。

最終エンコーダーspi/parity/sensor/timeout=0、first faultなし。
busy_ticks=351076、decode_waits=313798は累積しており、取得見送りは存在する。
ONのangle_age_maxは233.718 µsで250 µs制限以内。今回この上限による停止はなかった。

この試験で確認したのは補正演算の維持と閉ループIdの2/rev低減であり、独立角度基準による
エンコーダー絶対精度の測定ではない。以前の0.2～0.3 Aという観測値とは試験条件・PI設定の同一性が
確認できないため直接比較しない。NTCも巻線温度そのものではない。

## 最終状態・記録

PWM停止、ログidle、H2 OFF、gain=+1、保存係数は維持、COM6解放済み。
先に実施した無負荷電圧モードのOFF試験はユーザーの負荷接続指示で中止しており、この比較には使っていない。

記録：`build/h2_loaded_off_30s_20261006.log`、`build/h2_loaded_on_30s_20261006.log`と各同名JSON。
最終状態は`build/h2_loaded_final_20261006.log`。

解析の再実行：

```powershell
python tools/analyze_h2_endurance.py build/h2_loaded_off_30s_20261006.log --output build/h2_loaded_off_30s_20261006.json
python tools/analyze_h2_endurance.py build/h2_loaded_on_30s_20261006.log --enabled --output build/h2_loaded_on_30s_20261006.json
```
