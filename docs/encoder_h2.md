# AS5047P 2/rev補正の比較試験

起動時は補正OFF、gain=+1。設定はRAMのみで保持し、`foc stop`では保持、再起動で初期値へ戻ります。
CubeMX設定・センサー取得・校正値・速度推定には変更を加えません。
補正はFOCの電圧モードと電流モードに共通で、Park変換と電圧生成に同じ角度を使います。

## 式と符号

`theta_m`はセンサーゼロ基準のraw機械角[rad]です。

```text
error_e = 0.0750 * cos(2*theta_m - 2.327)  [rad_elec]
base_e  = wrap(direction * pole_pairs * theta_m - offset)
delta_e = -gain * error_e  （OFF時は0）
foc_e   = wrap(base_e + delta_e)
```

gain=+1は指定の電気角誤差を引く向き、-1は逆向きです。位相はcosの引数に加える−2.327 radです。
指定式の`theta_e,raw`は上記`base_e`（方向・offset校正済み、2/rev補正前）に対応します。
誤差はFOC座標の電気角として指定されているため、極対数7や校正directionを再度掛けません。
振幅・位相は`motor_control_config.h`の`MOTOR_CONTROL_ENCODER_H2_AMPLITUDE_ELEC_RAD`と`MOTOR_CONTROL_ENCODER_H2_PHASE_RAD`で変更できます。
電気角振幅0.0750 radは約4.297°、7極対で機械角約0.614°相当です。LUTや12/14次成分は追加していません。

旧版は`sin(2*theta_m + 133°)`を使用していました。新版の誤差は`sin(2*theta_m - 43.325°)`と等価です。
校正direction=+1の場合、新版gain=+1の加算補正は`+0.0750*sin(2*theta_m + 136.675°)`となり、旧版gain=-1に近い波形です。
位相差は約3.675°（2次成分の引数）、振幅差は約1%あります。旧版のgain比較結果を新版へそのまま移さず、gain=+1から再比較してください。

## コマンド

| コマンド | 動作 |
|---|---|
| `foc h2 status` | ON/OFF、gain、振幅・位相を表示 |
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
