# AS5047P 2/rev補正の比較試験

[電流・方向別の再現性試験（±4/8/12/15 A）](encoder_h2_matrix_2026-10-06.md)：8～15 Aは両方向・2回とも低減（約44～95%）。4 Aでは低減を確認できず、未始動例もありました。

[2026-10-06の実機検証結果](encoder_h2_hardware_2026-10-06.md)：map、適用・保存・再読込み、低電流FOC、map中止時の値保持を確認しました。

[負荷ありIq=8 A・約30秒比較](encoder_h2_loaded_2026-10-06.md)：Idの2/rev振幅はOFF 12.246 mA、ON 3.251 mA。今回の試験では約73.5%低減し、後半の悪化傾向はありませんでした。

起動時は補正OFF、gain=+1。ON/OFFとgainはRAMのみで保持し、再起動で初期値へ戻ります。
補正係数a2/b2は校正レコードに含めてFlash保存し、起動時に読み込みます。
CubeMX設定・センサー取得・速度推定は変更しません。
補正はFOCの電圧モードと電流モードに共通で、Park変換と電圧生成に同じ角度を使います。

## 式と符号

`theta_m`はセンサーゼロ基準のraw機械角[rad]です。

```text
error_e = a2*cos(2*theta_m) + b2*sin(2*theta_m)  [rad_elec]
base_e  = wrap(direction * pole_pairs * theta_m - offset)
delta_e = -gain * error_e  （OFF時は0）
foc_e   = wrap(base_e + delta_e)
```

gain=+1は推定された電気角誤差を引く向き、-1は逆向きです。
指定式の`theta_e,raw`は上記`base_e`（方向・offset校正済み、2/rev補正前）に対応します。
誤差はFOC座標の電気角として指定されているため、極対数7や校正directionを再度掛けません。
固定振幅・位相へのフォールバック、補正LUT、12/14次成分はありません。

## mapからの係数更新

1. `stop`、`adc stop`で停止し、ログ送信完了を待ちます。
2. `cal map`で往復測定します。F/Rそれぞれで`error_e=c+a*cos(2*mech_raw_rad)+b*sin(2*mech_raw_rad)`を最小二乗フィットします。
3. 正常終了時にF/Rのc,a,b、振幅、位相、平均後の候補a2,b2を確認します。この時点では現在値・Flashは変わりません。
4. `cal map apply`で候補a2,b2だけをRAMへ適用します。direction/offsetは維持し、未保存状態になります。
5. `cal save`でdirection/offset/a2/b2を一緒に保存・読み戻し検証します。
6. `foc h2 gain +1`、`foc h2 on`を設定してFOCを開始します。FOCは開始時にRAMの校正値を取得します。

F/Rの係数を等重みで平均し、`a2=(a_F+a_R)/2`、`b2=(b_F+b_R)/2`とします。
積和と3元連立方程式の求解はdouble、保存値はfloatです。1000点の配列は追加しません。
定数項cや既存のcandidate_offsetは表示のみで、offsetへの自動反映はしません。

表示する振幅・位相は`A=hypot(a,b)`、`phase=atan2(b,a)`、すなわち`A*cos(2*theta_m-phase)`です。
振幅はrad_elec、位相はrad/degreeです。振幅が正確にゼロなら位相undefinedと表示します。

applyにはFOC/PWM/校正/ADCログの停止が必要です。適用済み候補は消費されます。
新しいmap要求または通常校正開始で古い候補を無効化します。点数不足、異常停止、
非有限値、誤差の±π折り返しによる隣接点のπ超の跳び、特異・悪条件な行列では候補を作りません。
map失敗時は適用中のレコード・保存状態・Flashを保持します。
通常の`cal start`成功時はH2をゼロに戻すため、mapを再取得してください。

## Flash互換性

version 2は40 Bで、`float h2_cos_rad_elec`と`float h2_sin_rad_elec`をCRC対象に含みます。
最終8 Bのmagic/CRCを最後に書き込みます。
旧version 1は旧CRCとモーター情報を検証し、direction/offsetを維持、a2=b2=0のv2へRAM上で移行します。
この場合stored=noで、次の`cal save`まではFlashを変更しません。旧固定補正値は復元しません。
不明version・CRC不一致・非有限係数は採用しません。
保存領域は既存の最終2 KiBページのままです。保存中の電断で旧値も失う既存方式は変更していません。

## コマンド

| コマンド | 動作 |
|---|---|
| `foc h2 status` | ON/OFF、gain、係数の有効性、a2/b2（µrad_elec）を表示 |
| `foc h2 on` / `foc h2 off` | 補正を有効化／無効化。gainは保持 |
| `foc h2 gain 0` | 補正量ゼロ |
| `foc h2 gain +1` | 推定誤差を減算 |
| `foc h2 gain -1` | 逆符号 |

運転・ログ中にも設定できます。次のADC周期から反映し、同一周期のParkと電圧生成の角度は一致します。
切り替えの補間はないため、切り替え直後は過渡区間として解析から除いてください。

## ログ

有効なFOC観測時に以下を同じADC周期・タイムスタンプで記録します。すべてrad、小数3桁です。

| キー | 内容 |
|---|---|
| `mech_raw_rad` | センサー由来の未補正機械角 |
| `elec_raw_rad` | センサー由来の未校正・未補正電気角（従来のAS5047P値） |
| `elec_base_rad` | 方向・offset校正済み、2/rev補正前の電気角 |
| `elec_rad` | 補正後のFOC使用角。既存キーを維持 |
| `elec_corr_rad` | `delta_e`。折り返し前の符号付き加算量。OFF/gain=0では0 |

`elec_rad = wrap(elec_base_rad + elec_corr_rad)`です。単純な角度差は0/2π境界で飛ぶため、補正量の評価には`elec_corr_rad`を使います。
rawの`AS5047P_Sample`は書き換えません。既存のId/Iq、相電流、角度ageも保持します。

## Id=0 A、Iq=8 Aでの比較

1. `foc h2 off` → `foc current 0 8` → `foc start`。立ち上がり・負荷が安定してから`adc 200`で記録します。
2. 同じ回転数・負荷で、OFF、ON/gain=0、ON/gain=+1、ON/gain=-1を別区間として記録します。設定はコマンド応答とともに保存してください。
3. 過渡区間を除き、`mech_raw_rad`基準でIdのsin(2θm)/cos(2θm)成分を比較します。複数回転を使い、Iq・回転数の変化も確認します。
4. `log_overrun`と`foc status`の`adc_control_max`、異常停止の有無を確認します。
5. `foc stop`で出力停止、`adc stop`でログを停止します。

追加ログによりFOCは15信号になります。921600 bpsではまず`adc 200`（20 kHz時100 Hz）を使ってください。
200 Hz記録は時刻の桁数・状態出力次第で帯域上限に近づきます。間引きはアンチエイリアス処理ではありません。
Id/Iqは補正後の座標で観測するため、raw相電流も残して比較します。
実機でのIdの2/rev低減効果と追加演算の締切余裕は別途確認が必要です。

## ソフトウェア検証

Debug/Releaseビルド成功。両ELFを使った`tools/test_encoder_h2.py`で、ON/OFF、gain=0/±1、方向±1、位相、1回転の角度折り返し、raw値保持、Park変換、観測値を検証しました。
既存の`tools/test_current_pi.py`も両ビルドで合格しました。ARMエミュレーションによる検証で、実機の処理時間・負荷試験結果ではありません。

`tools/test_h2_calibration.py`は非等間隔フィット、DC分離、F/R平均、apply/save分離、旧形式移行、CRC、保存順序をARMエミュレーションで確認します。`tools/test_cal_map.py`は中止・保護停止時のレコード保持を確認します。
