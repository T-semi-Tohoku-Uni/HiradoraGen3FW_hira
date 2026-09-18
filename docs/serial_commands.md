# シリアルコマンド一覧

## NTC温度計測

| コマンド | 動作 |
|---|---|
| `ntc` または `ntc start` | NTCの定期出力を開始。最初の計測結果を出力し、その後は約1秒間隔 |
| `ntc stop` | NTCの定期出力を停止（PWMの動作は変更しない） |

大文字・小文字と前後の空白は許容します。`ntc start` / `ntc stop`の単語間は半角スペース1文字です。
起動時の定期出力はOFFです。計測自体はメインループで約10ms周期に行い、
1秒ごとの出力には直近の計測値を使います（1秒間の平均値ではありません）。
ADCの休止後の変換に関するエラッタ対策として、2回続けて変換し2回目を採用します。

出力例（数値は例）：

```text
NTC raw=1309, R=10003 ohm, T=24.99 C
```

- `raw`: PC4 / ADC2_IN5の12bit ADC値。
- `R`: NTCの推定抵抗値（Ω）。
- `T`: 推定温度（℃）。シリアルの文字コードに依存しないよう単位を`C`と表示します。
- 回路は3.3V → NCP18XH103F03RB → PC4 → 4.7kΩ → GND、PC4からGNDに33nFを想定します。
- ADC基準電圧VREF+が分圧電源と同じであることが前提です。
- 25℃で10kΩ、B25/50=3380Kの近似式を使用します。特に高温域では特性表による換算との差があり、表示の小数桁は精度を保証しません。
- ADCエラーまたは生値0/4095では、温度の代わりにエラーを出力します。推定値が−40～125℃の範囲外なら注記します。
- `adc`による電流キャプチャ・CSV転送中はNTCの計測と出力を休止し、完了後に自動再開します。この間は従来どおり`stop`だけを受け付けます。
- メインループ内の他の処理により周期は延びる場合があります。温度によるPWM停止機能はありません。

仕様: [村田製作所 NCP18XH103F03RB](https://www.murata.com/products/productdetail?partno=NCP18XH103F03RB)

## 接続設定

- USART1
- ボーレート: 115200 bps
- データ長: 8 bit
- パリティ: なし
- ストップビット: 1 bit
- フロー制御: なし
- コマンド終端: CR、LF、またはCRLF

## モーターPWMコマンド

| コマンド | 動作 | 例 |
|---|---|---|
| `<offset>` | 現在選択中の相へ、50%からのDuty差分を設定 | `5` → 選択相を55% |
| `u <offset>` | U相を選択してDuty差分を設定。V/W相は50%へ戻す | `u -3` → U=47%、V/W=50% |
| `v <offset>` | V相を選択してDuty差分を設定。U/W相は50%へ戻す | `v 4` → V=54%、U/W=50% |
| `w <offset>` | W相を選択してDuty差分を設定。U/V相は50%へ戻す | `w -2` → W=48%、U/V=50% |
| `mid` | U/V/W相をすべて50%へ戻す | `mid` |
| `stop` | TIM1のメイン出力と補完出力を停止 | `stop` |
| `start` | U/V/W相をすべて50%にしてPWMを再開 | `start` |
| `run cw <rpm>` | CW方向へオープンループ120度駆動を開始 | `run cw 600` |
| `run ccw <rpm>` | CCW方向へオープンループ120度駆動を開始 | `run ccw 600` |
| `status` | PWMの動作状態、回転数指令、現在のDutyなどを表示 | `status` |

`run`では位置合わせ後、開始回転数から指定回転数まで加速します。Dutyは指令中の
回転数に連動して増加します。極対数、位置合わせDuty、開始Duty、Duty上昇率、
最大Duty、位置合わせ時間、加速時間、開始回転数、指令可能範囲は
`Core/Inc/motor_control_config.h`で変更できます。Duty設定の単位は0.1%です。

現在の設定では、位置合わせと60 rpmで5.0%、120 rpmで5.7%、240 rpmで7.0%と
なり、180 rpm上昇するごとにDutyを2.0%増やします。最大Dutyは20.0%、位置合わせは
200 ms、加速は6 s、開始回転数は60 rpm、指令範囲は60～3000 rpmです。
逆起電力による同期確認は行わないため、負荷や加速条件によっては脱調します。
`status`に表示される`reference`は転流周期から計算した指令値であり、実測回転数
ではありません。`duty`は現在TIM1へ適用しているDutyです。
`run`の直後に`adc`を送ると位置合わせまたは加速中のデータになります。指定した
回転数で取得する場合は、現在の設定では6.2秒以上待ってから`adc`を送ってください。
CW/CCWと実際の回転方向の対応はモーター相の配線順によって逆になることがあります。

## ADC取得コマンド

| コマンド | 動作 | 例 |
|---|---|---|
| `adc` | TIM1_CH4に同期してU相を2回、V/W相を各1回、4000組取得し、取得完了後にCSVを連続送信 | `adc` |

`adc`を受信すると、まず次の開始メッセージを返します。

```text
ADC capture started: 4000 samples
```

取得完了後、次のヘッダーと4000行のCSVデータを連続送信します。

```csv
sample,sector,u1_raw,v_raw,u2_raw,w_raw
0,1,2048,2051,2047,2049
1,1,2047,2050,2046,2048
```

- `sample`は0～3999の取得順番号です。
- `sector`は120度駆動中の通電ステップ番号1～6です。停止中および従来の
  手動PWMモードでは0です。
- `u1_raw`はADC1 Rank 1（VOPAMP1）、`v_raw`はADC2 Rank 1（VOPAMP2）、
  `u2_raw`はADC1 Rank 2（VOPAMP1）、`w_raw`はADC2 Rank 2
  （VOPAMP3）の12bit raw値です。
- `u1_raw`/`v_raw`は同時に変換され、その直後に`u2_raw`/`w_raw`が
  同時に変換されます。
- PWM動作中・停止中のどちらでも取得できます。
- PWM停止中はU/V/Wの出力をOFFのままTIM1をADCトリガ専用に一時動作させ、
  取得後にTIM1も停止状態へ戻します。このときの値は主にゼロ電流時の
  オフセット確認用です。
- 20kHz PWM時の取得時間は約0.20秒です。
- 115200bpsでのCSV送信には値によって約9～10秒かかります。
- 回転中に取得した場合も、4000点の取得完了後、CSV送信を始める直前にPWMを
  自動停止します。CSV送信完了後も停止状態を維持します。
- ADC取得中およびCSV送信中は緊急停止用の`stop`だけを受け付けます。

## 入力値について

- `<offset>`の単位はパーセントです。
- 指定値は絶対Dutyではなく、50%からの差分です。
- 起動時に選択されている相はU相です。
- 相を指定せず数値だけを送ると、最後に選択した相へ適用されます。
- 現在の許容範囲は`-10.0`～`+10.0`です。
- 許容範囲は`Core/Inc/motor_control_config.h`の
  `MOTOR_CONTROL_MAX_DUTY_OFFSET_PERCENT`で変更できます。

## 入力例

```text
run cw 600
status
adc
stop
u 2.5
status
v -4
mid
stop
start
```

## STSPIN32G4のFAULT発生時

起動時・`start`・`run cw/ccw <rpm>`では、毎回次の順でPWMを開始します。

1. PWMを停止し、既存のFAULTがあればCLEARして充電可能な状態か確認。
2. High-side 3相をOFF、Low-side 3相を約0.75 ms ONにしてbootstrapを充電。
3. 全相OFFにし、CLEARコマンドを送信。
4. 200 µs待ってSTATUSとPE15のnFAULTを確認し、正常ならPWMを開始。

充電時間は`Core/Inc/motor_control_config.h`の
`MOTOR_CONTROL_BOOTSTRAP_CHARGE_US`（初期値750 µs、設定範囲500～1000 µs）で変更できます。
DWTサイクルカウンタで待ち時間を作ります。割り込み処理による延長はあり得ます。
充電中はTIM1の強制非アクティブモードでHigh-sideをLow、相補Low-sideをHighにします。
充電後は通常のPWMモードへ戻します。

前後の確認でRESET、VDS、THSD、VCC_UVLOのいずれかが残る、nFAULTがLow、
またはI2Cエラーの場合はPWMを開始せず、停止を維持します。
起動時にFAULTで開始できなかった場合も、原因を取り除いた後に`start`や`run`で再試行できます。

### FAULT確認コマンド

`fault`（大文字の`FAULT`も可）を改行付きで送信すると、その時点の
STATUSレジスタ（0x80）をI2C3で読み、PE15のnFAULTピン状態とともに返信します。
前後の空白は許容します。FAULTのクリアやPWM状態の変更は行いません。
起動時のFAULTでPWMが開始されなかった場合も使用できます。
ADC取得中・CSV送信中は従来どおり`stop`のみ受け付けるため、完了後に送信してください。

返信例（FAULTなし、保護レジスタはロック中）:

```text
STSPIN32G4 STATUS current: 0x80 [LOCK=1 RESET=0 VDS=0 THSD=0 VCC_UVLO=0]
STSPIN32G4 nFAULT (PE15): 1 (HIGH, inactive)
```

- `RESET`: レジスタのリセット履歴、`VDS`: VDS保護の作動、
  `THSD`: 過熱保護、`VCC_UVLO`: ゲート駆動電源の低電圧保護。各ビットは1で該当状態です。
- `LOCK`は保護レジスタのロック状態であり、FAULT原因ではありません。
- nFAULTはアクティブLowです。`0 (LOW, asserted)`はFAULT信号が出ている状態、
  `1 (HIGH, inactive)`は信号が出ていない状態です。
- I2C読み出し失敗時は`STATUS read failed`とHALステータス・エラーコードを返信し、
  nFAULTピン状態も返信します。レジスタ値とピンは順番に読み出すため、同時刻の値ではありません。
- 原因を示すレジスタの正式名称は`STATUS`です。`NFAULT`レジスタ（0x08）は
  ピンに通知する保護の設定用であり、このコマンドではSTATUSを読みます。

### FAULTクリアコマンド

`fault clear`（大文字の`FAULT CLEAR`も可）を改行付きで送信すると、
PWMを停止してからSTATUSレジスタとnFAULTピンを表示し、CLEARレジスタ（0x09）へ
0xFFを書き込みます。1 ms待機後、STATUSとnFAULTを再度表示します。
前後の空白は許容します。`fault`と`clear`の間は半角スペース1文字です。

```text
fault clear
```

- `before clear`と`after clear`でクリア前後を確認できます。
  `CLEAR command sent`は書き込み成功を示し、FAULTが解消したことの保証ではありません。
  過熱や低電圧などの原因が残っていれば、クリア後もFAULTが残ります。
- クリア前のSTATUS読み出しに失敗した場合は、クリアを中止します。
  CLEAR書き込みやクリア後の読み出しに失敗した場合もエラーを返信します。
- クリア後もPWMは停止状態を維持します。通常の再開は`start`または`run`で行います。
  再開時にも上記のbootstrap充電とFAULT確認を行います。
- ADC取得中・CSV送信中は使用できません。完了後に送信してください。

参考: [STSPIN32G4データシート](https://www.st.com/resource/en/datasheet/stspin32g4.pdf)
（MCUとゲートドライバーの内部接続、NFAULT設定、STATUSレジスタ）。
