# 電流PI

既存の定電圧FOCに、ADC同期のd/q電流PIを追加しました。ゲインは暫定値です。
実機でのゲイン調整・電流追従・ISR時間余裕は未確認です。

```text
foc current 0 0.2
foc start
foc status
adc 100
stop
```

`foc current <Id> <Iq>` の単位はAです。停止中に電流モードを選び、
`foc start`で開始します。開始条件（校正済み、静止、VM/角度有効、
ADCログ停止）は従来と同じです。電流モード中は同コマンドで両軸の指令を変更でき、
減少・反転も電流指令slewを介して反映します。モード変更はFOC停止中だけです。

`foc current 0 0`はゼロ電流を制御し続けます。PWM停止は`stop`または`foc stop`です。
d軸だけの指令も使用できます。停止後は指令と積分項をゼロに戻します。
再起動時も電流指令はゼロから立ち上げます。

従来の`foc voltage <Vd> <Vq>`、`foc start`も使用できます。
電圧モードの電圧slew・起動条件・運転中の指令変更制限は維持しています。

## 調整値

`Core/Inc/motor_control_config.h`で変更し、再ビルドしてください。

| 定義（先頭の`MOTOR_CONTROL_`は省略） | 暫定値 | 意味 |
|---|---:|---|
| `CURRENT_KP_D` / `CURRENT_KP_Q` | 0.1 | 比例ゲイン V/A |
| `CURRENT_KI_D` / `CURRENT_KI_Q` | 1.0 | 積分ゲイン V/(A・s)、更新時にTsを乗算 |
| `CURRENT_INTEGRAL_LIMIT_D` / `CURRENT_INTEGRAL_LIMIT_Q` | 1.0 | 各積分項の絶対上限 V |
| `CURRENT_REF_MAX_A` | 1.0 | sqrt(Id_ref²+Iq_ref²)の上限 A |
| `CURRENT_SLEW_A_PER_SEC` | 1.0 | 電流指令ベクトルの移動速度 A/s |
| `CURRENT_MAX_VOLTS` | 1.0 | dq出力電圧ベクトルの上限 V |

相電流の過電流停止閾値`CURRENT_LIMIT_A`は別です。
電流指令上限はこの閾値未満とします。Ki=0で積分を無効にできます。

指令上限を過電流停止閾値より大きく設定すると、指令が小さくても設定エラーで拒否します。
`CURRENT_REF_MAX_A < CURRENT_LIMIT_A`が必須で、同値は許可していません。
例えば停止閾値10 Aを維持する場合、指令上限を1 Aにするとこの条件を満たします。
設定変更後は再ビルドとFW書き込みが必要です。
エラーの`config:`以降に違反条件を表示します。電流指令ベクトルの範囲外とは別のエラーです。

## 演算と表示

同一ADC周期のId/Iq・電気角を使用し、誤差は「slew後の指令－実測」です。
TIM1設定から求めた固定Tsで積分し、各軸をclampします。
PI出力は、設定上限・既存の電圧上限・VMとPWM余白から求めた線形SVPWM上限の
最小値で円形制限します。飽和を深める積分更新は取り消し、出力を再計算します。
`saturated`は当該周期で積分更新抑制または電圧制限が働いたことを示します。
既存inverse Park/SVPWMとq軸のdirection変換をそのまま使用します。

`foc status`はmode、実測Id/Iq、出力Vd/Vq、処理時間に加え、電流モードでは
電流target（UART指定）・ref（slew後）、積分項、飽和状態を表示します。
1行目のtarget[mV]は電圧モードの指令で、電流モードでは0です。
`adc`の既存Id/Iqログも両モードで使えます。

既存の相過電流・ADC・VM・角度鮮度・速度上限・watchdog・PWM締切監視を継続します。
電流指令はトルク方向を表すため、電圧モードの逆回転判定は電流モードでは適用しません。
速度制御、非干渉補償、角度外挿は含めていません。

## ソフトウェア確認

Release/Debugビルド成功。`tools/test_current_pi.py`で両ビルドのARMバイナリを
Unicorn上で実行し、PI符号・積分clamp・円形制限・飽和復帰・不正値拒否・
リセット・UART解析・運転中のモード変更拒否・電圧指令減少の既存制限を確認しました。
周辺機器呼び出しをスタブ化したテストであり、実機の電流応答や割り込み時間は検証しません。

```text
python tools/test_current_pi.py build/Release/HiradoraGen3FW.elf build/Debug/HiradoraGen3FW.elf
```

依存ライブラリは`unicorn`と`pyelftools`（既存`test_cal_map.py`と同じ）です。
