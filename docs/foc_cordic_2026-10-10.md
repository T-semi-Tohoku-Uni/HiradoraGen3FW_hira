# FOC sin/cosのCORDIC化と実機比較（2026-10-10）

## 結論

STM32G431 / 160 MHzで、既存CMSIS-DSPのsin/cosをハードウェアCORDICへ置換した。
同じ負荷・Id=0 / Iq=+0.5 Aで2回ずつ比較し、ADC制御区間最大の代表値は
H2 OFFで0.650 µs（2.34%）、ONで0.682 µs（2.21%）短縮した。
全8運転が保護停止・通信エラーなしで完了したため、開発用の既定設定をCORDICへ切り替えた。
高速回転・実運用電流・詳細ログ併用時の評価は今回の範囲に含まない。

## 実装

- `FOC_USE_CORDIC=ON`をCMakeとDebug/Releaseプリセットの既定に設定。
  `-DFOC_USE_CORDIC=OFF`でCMSIS版へ戻せる。
- `VoltageVector_Init()`をmainのUSER CODE内で呼び、ADC/PWM制御開始前に初期化。
  cosine機能、Q1.31、6サイクル精度、2入力（角度・単位modulus）、2出力（cos・sin）。
- 正規化済みラジアンを[-π,π)へ折り返し、Q1.31へ変換。
  float丸めによる+2^31への到達をガードし、整数変換の範囲外を避ける。
  Wrapが丸めで返すちょうど2πは従来どおり0として扱う。
- 周期処理はWDATA/RDATAへ直接アクセス。RDATAの待機機能を使い、両出力を読み切る。
  校正等のmain側呼出しとADC ISRの競合を防ぐため、入出力部分だけPRIMASKで保護し、元の状態へ戻す。
  CORDICはこのsin/cos実装が専有する。NMI等からの使用は想定しない。
- 同じADC観測のsin/cosをPark・逆Parkへ共有する構造、H2式、PI式、SPI取得順序を維持。
  `fmodf`による角度正規化は今回変更していない。
- `cal trig test`を追加。FOC/PWM/校正/ADCログ停止時だけ実行し、PWM・校正Flashへ書き込まない。

設定の参考：[ST公式CORDIC sin/cos例](https://github.com/STMicroelectronics/STM32CubeG4/blob/master/Projects/NUCLEO-G474RE/Examples_LL/CORDIC/CORDIC_CosSin/Src/main.c)。

## 停止中の実機数値・時間検査

1025点の1回転走査と8境界点（0、負の微小角、2π、π前後等）で、
CMSIS版とのsin/cos差・単位円誤差を検査した。
各角度で通常電圧、ゼロ出力、電圧飽和の3条件のPWM dutyも比較し、4132チェック・失敗0。

| 指標 | 最大絶対差 |
|---|---:|
| sin/cos対 CMSIS | 0.000001252 |
| PWM duty対 CMSIS | 0.000001013 |
| sin²+cos²と1の差 | 0.000002563 |

合格閾値はsin/cos 5e-6、duty 3e-6、単位円1e-5。
既存`cal test`も659チェック・失敗0。

DWT計測は1024角度点。両経路をウォームアップし、各呼出し中だけ外部割り込みを抑止。
角度・出力変換、関数呼出し、CORDIC内のPRIMASK保存・復元を含む。
正規化済み角度を渡しており、`fmodf`は含まない。カウンター読出し等の計測オーバーヘッドは差し引いていない。

| 経路 | 合計cycles / 1024 | min / max cycles | 平均時間 [µs] |
|---|---:|---:|---:|
| CMSIS参照 | 123170 | 118 / 148 | 0.751770 |
| CORDIC | 75792 | 72 / 92 | 0.462598 |

単体の平均短縮は0.289172 µs、38.47%。この値をADC区間最大の短縮量と同一視しない。

## 負荷付きFOC反復比較

ユーザーが前回と同じ負荷を接続済みと確認。COM6 / 921600 baud、20 kHz、
Id=0 / Iq=+0.5 A、H2 gain=1、校正VALID。ADC詳細ログ・角度表示・NTC表示は運転中OFF。
各条件50回の状態照会、最後の照会まで約11秒。A1→B1→B2→A2の順で、各FWでH2 OFF→ON。
毎回書込みVerify・再起動。速度200 rpm超、相ピーク5 A超、異常状態で中断し、前後NTC 50 ℃未満を確認。

A/Bはいずれも今回の診断コードを含み、CORDIC選択だけが異なるビルド。
SPI要求のコピー直後開始は双方で有効。

| 運転 | H2 | ADC区間最大 [µs] | angle_age最大 [µs] | decode_waits増分 |
|---|---|---:|---:|---:|
| A1 CMSIS | OFF | 27.787 | 108.925 | 0 |
| A1 CMSIS | ON | 30.950 | 112.331 | 49 |
| B1 CORDIC | OFF | 27.137 | 108.275 | 0 |
| B1 CORDIC | ON | 30.243 | 111.637 | 6 |
| B2 CORDIC | OFF | 27.137 | 108.318 | 0 |
| B2 CORDIC | ON | 30.243 | 112.750 | 9 |
| A2 CMSIS | OFF | 27.787 | 108.925 | 0 |
| A2 CMSIS | ON | 30.900 | 112.237 | 47 |

代表値は各運転の累積最大値2個の中央値。制御周期ごとの中央値・平均値や保証されたWCETではない。
`adc_control_max`は全IRQの入口から出口までの時間ではなく、既存ADC制御区間の指標。
今回の要求開始位置は双方同じなのでA/Bの計測範囲は揃っている。
最大値には割り込みの重なりやコード配置の影響もあり、単体計測との差の内訳は未分離。

`compute_max`は双方・全条件12.487 µs。三角関数はCurrentISR側で実行されるため、
TickISR側のこの指標では今回の短縮は観測されない。
H2 ONのdecode_waitsは減ったが0ではなく、age最大も一貫して減っていない。
待ち回数は停止前後を含む差分で、厳密な周期当たり発生率として扱わない。
UARTログ負荷による取得見送りの解消を示す試験ではない。

全運転でSPI/parity/sensor/timeout/busy_ticks増分0、UART errors/overrun/queue_drops=0。
観測rpmは-4～+5、観測相ピーク最大0.890 A。全運転の状態照会でfault=none。
VMは24.189～24.242 V、NTCは38.75～39.18 ℃。
終了処理の`user stop`は計画停止によるもの。

## ビルド・回帰検証

- Debug/Releaseビルド成功。Release FLASH 105904 B / 126 KiB、RAM 15152 B / 32 KiB。
  Debug FLASH 127008 B / 126 KiB（98.44%）、RAM 15144 B。DebugのFlash余裕は2016 B。
- `test_cordic.py`：実ARMコードと理想CORDIC周辺モデルで4097点＋境界、入出力順、
  2結果読み切り、PRIMASK復元、非有限値拒否、duty差をDebug/ReleaseでPASS。
  周辺モデルは実機CORDICの精度・待機時間を再現しないため、その検証は上記実機測定で行った。
- 共有回帰：同じ周期のsin/cos共有、H2 OFF=1 / ON=2回、古い観測・NaN拒否をPASS。
- H2回帰：ON/OFF、gain、回転方向、位相、wrap、raw保持、Park・観測値をPASS。
- PI回帰：計算、clamp、飽和・回復、reset、UART/modeガードをDebug/ReleaseでPASS。
- 最終Release ELFは実機比較BとSHA256一致：
  `0A60AAA5A121791D28D8BA1E478512A23BABA778E22A8C6E7A7ADD1CD3AB532D`。
  最終Debugのbinも周辺モデルで検証したDebugビルドのbinとSHA256一致。

## 最終実機状態

最終Releaseを書込みVerify・再起動し、`cal trig test`と`cal test`を再実行して失敗0。
最終書込み後の校正ページも読出し、変更前とSHA256一致を確認した：
`EF71DD686D933F0A3DFA59F6B6654CA0E304F246908DBFA99AEA4A7B51153ADF`。
読出し後の再起動でもPWM停止、FOC off、fault=none、校正VALID/stored=yes、
H2 OFF、ADC/角度/NTCストリーム停止、COM6解放を確認。コミット・pushは未実施。

## 再実行と証跡

CMSIS比較版は、プリセット設定を上書きして独立ディレクトリへ生成する：

```powershell
cmake --preset Release -B build/CmsisRelease -DFOC_USE_CORDIC=OFF
cmake --build build/CmsisRelease
cmake --preset Release
cmake --build --preset Release
powershell -NoProfile -ExecutionPolicy Bypass -File tools/run_cordic_ab.ps1 -After build/Release/HiradoraGen3FW.elf
python tools/analyze_cordic_ab.py build/cordic_ab_<timestamp>
```

実行スクリプトはCOM6、上記ST-LINK・負荷・電流条件を前提にする。
A/B終了時はCMSISへ復帰し、採否確認後に選択したELFを別途書き込む。

Git管理外の証跡：

- `build/cordic_ab_20261010_140803/`：全A/Bログ、JSON、コマンド時刻、Flash Verify、ELFハッシュ、集計。
- 同ディレクトリ`cal_before.bin` / `cal_after.bin`：校正ページ0x0801F800 / 2048 Bの一致確認。
- `build/cordic_stopped_release_20261010.log`：実機数値・DWT検査。
- `build/cordic_before_flash_20261010.bin`：変更前Flash全128 KiBのバックアップ。
- `build/cordic_before_20261010.elf`：作業開始時のコピー直後SPI取得開始版。
- `build/cordic_cmsis_reference_20261010.elf`：A側CMSIS比較版。
- `build/cordic_final_flash_20261010.log` / `build/cordic_final_status_20261010.log`：最終書込み・停止中再検査。
- `build/cordic_final_cal_20261010.bin` / `build/cordic_final_cal_hashes_20261010.json`：最終校正ページ一致。
- `build/cordic_final_reboot_20261010.log`：最終再起動後の停止状態。

関連：[三角関数共有の最適化](foc_trig_optimization_2026-10-09.md)、
[SPI取得開始の移動](encoder_early_request_2026-10-10.md)。
