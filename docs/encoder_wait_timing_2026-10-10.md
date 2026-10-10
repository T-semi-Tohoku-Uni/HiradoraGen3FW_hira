# 角度取得待ちのタイミング調査（2026-10-10）

## 判明したこと

`decode_waits`はCPUがその場で待機した時間ではなく、応答処理・角度公開が未完了のため
次の取得要求を見送った回数である。`StartRead()`はカウント後に直ちにreturnする。

調査ビルドで、応答DMAの完了処理が高優先度ADCに中断され、ADC内の次取得要求が
DECODING状態を見て見送られ、そのADC終了後にDMA処理が再開して角度が公開される順序を捕捉した。
その新角度が採用されたのは次のADC周期だった。

ソース上の優先度はADC1_2とTIM1更新が0、SPI1 RX/TX DMAが1。
FOC中の取得要求は`MotorControl_FocAdcISR()`末尾にあり、演算・CCR更新の後に発行する。
下位優先度DMAを中断したADC内から要求すると、DMAはそのADCが終わるまで公開を完了できない。

これは記録した4事例で確認した発生機構。通常FWの全待ちを網羅した証明ではなく、
TIM割り込みがADCに先行したか、TIM/ADCハンドラー内のどの命令で中断されたかまでは記録していない。

## 方法と条件

- 負荷接続を維持、COM6 / 921600 baud、Id=0 / Iq=0.5 A。
- 最適化済みFWに`ENCODER_TIMING_PROBE`を定義した専用ビルドを使用。
- H2 OFF→ONを2組、各50回の状態照会（約11秒）。詳細ADCログはOFF。
- 各運転の11回目の照会前（約2秒後）にarm。最初の対象待ち1件だけを保持し、停止後にdump。
- DWT時計160 MHz。ISR内printf・イベントリング・待ちループの追加は行わない。
- 要求フレームRX TC処理点、応答フレームRX TC処理点、DECODINGへの遷移直前、
  ADCコールバック内の開始点、角度コピー後、要求見送り、角度公開、次ADCの角度使用を記録。
- 待ち時にIPSRとADC/DMAのNVIC active状態も取得。
- NTCは約32.4～33.1 ℃。全4運転で保護停止なし、SPI/parity/sensor/timeout・UARTエラー0。

## H2 ONの時系列

進行中の角度取得の要求時刻を0 µsとする。全てソフトウェアの観測点であり、
センサー内部測定時刻やSCKのエッジを測った値ではない。

| 観測点 | 1回目 [µs] | 2回目 [µs] |
|---|---:|---:|
| 角度取得要求 | 0 | 0 |
| 要求フレームRX TC処理点 | 6.781 | 6.781 |
| 応答フレームRX TC処理点 | 14.438 | 14.269 |
| DECODING遷移直前 | 15.300 | 15.131 |
| ADC制御区間開始 | 21.056 | 21.038 |
| ADCが公開済みの旧角度をコピー | 26.281 | 26.263 |
| ADC末尾で次取得要求を見送り | 49.431 | 49.413 |
| DMA処理再開後、新角度を公開 | 53.919 | 53.725 |
| 次ADCがその新角度をコピー | 76.356 | 76.356 |

要求見送り時のIPSR=34（ADC1_2）、ADC active=1、RX DMA active=1を両回で確認。
RX DMAがまだactiveのまま、ADCがその上で実行されている。
1回目の角度sequenceは258460→258461、2回目は261529→261530。
次ADCの使用sequenceとrequest_cyclesは今回公開したサンプルに一致した。
角度コピー間隔は50.075 / 50.094 µsで、新角度の採用が1制御周期後へ送られたことを確認できる。
ここではさらに前のADCのsequenceを記録していないため、旧角度が合計何周期使われたかは断定しない。

H2 OFFでも記録した2事例で同じ割り込み関係を確認。要求見送りは49.506 / 49.469 µs、
公開は54.113 / 52.413 µs、次ADC使用は79.175 / 79.238 µs。
OFFの2回目はarmが転送途中に入り、要求フレームIRQ時刻が未取得（0）だったため、その項目は欠測として扱う。

## 既存transfer表示の意味

`received_cycles`は`FrameComplete()`内でCSをHighに戻した後のソフトウェア時刻。
パリティ解析・公開より前だが、DMAハードウェアの完了時刻ではない。
調査のH2 ONではこのマーカーが1回目52.050 µs、2回目15.944 µsとなった。
どちらも応答RX TC処理点は約14 µsであり、前者には割り込み中断の影響が入る。
したがって`transfer_max`が約53 µsでも、SPI転送自体に53 µs掛かったとは解釈できない。
DECODING状態にはCS解放処理から角度公開までを含み、浮動小数点の角度計算だけを意味しない。

## 記録による影響と限界

計測は挙動に影響した。通常版の直前A/B比較では最適化後H2 OFFのdecode_waits増分0、
H2 ONで約1.28万だったが、調査版ではOFFで15 / 14、ONで114267 / 114772だった。
調査版ONのbusy_ticks増分も3 / 1。状態照会の最後のADC区間最大はOFF約25.0 µs、ON27.787 µs。
温度や時期も違うため差分全部を計測コードだけの影響とは定量できないが、
調査版の頻度・最大時間を通常版の値として採用しない。

約50 µs周期の境界に完了処理が近く、僅かな配置・処理時間の差で見送り頻度が変わるという解釈と整合する。
その解釈を全周期の位相分布として証明する計測は未実施。
今回の記録は割り込みの入れ子と公開・使用の順序を確認するためのもの。

## 次の変更候補

三角関数の追加高速化だけでなく、応答の公開をADC開始前に完了できる時間配置を検討する。
まずTIM/ADCとDMA完了処理の優先度・先行関係、ADC終了に連動した取得開始位置が対象。
優先度を変える場合はDMA側のCSガードやBSY待ちがADCを遅らせる影響も測り、
ADC制御締切・角度age・通信エラーを同時に評価する必要がある。
今回、割り込み優先度・取得開始位置・制御式は変更していない。

## ビルド、解析と最終状態

調査用：`build/TimingProbe/HiradoraGen3FW.elf`。
SHA256：`8C47680671AD8CAAADA665109669CE02DB54392A4D8DD4E9AB4639B332236835`。
構成はRelease相当で`CMAKE_C_FLAGS=-DENCODER_TIMING_PROBE`を追加。
`test_encoder_h2.py`、`test_current_pi.py`、`test_motor_period.py`、`test_foc_trig.py`は調査ELFでPASS。

通常Releaseビルドには記録用コード・変数・コマンドを含めない。
追加前後のobjcopy binary SHA256は一致：
`12E5C81E5CAAEE40F4BDA427E4360FAEED8FC54FA0396093FB5E68824B0597CF`。
通常ELF SHA256も従来の`9304C94D87CDE33542178DE5D2CCA6284D73E1C175A0675D5C36B89F444FC1C1`のまま。

終了時に通常の最適化済みFWへ書戻し、Verify成功・再起動。
校正ページの前後SHA256は`EF71DD686D933F0A3DFA59F6B6654CA0E304F246908DBFA99AEA4A7B51153ADF`で一致。
PWM停止、H2 OFF、校正VALID/stored=yes、通信エラー0、ストリーム停止、COM6解放を確認済み。

証跡（build以下はGit管理外）：

- `build/encoder_probe_20261010_023456.log`、`build/encoder_probe_20261010_023542.log`：生受信。
- 同名`.json`：FOC状態照会、`_commands.log`：コマンド送信時刻。
- `build/encoder_probe_lifecycle_20261010/`：書込み・校正退避・最終状態・集計。
- `build/run_encoder_probe.ps1`、`build/measure_encoder_probe.ps1`：今回の測定手順。
- `tools/analyze_encoder_timing_probe.py`：dumpの時刻換算・sequence/割り込み状態検証。

dump形式は全て整数のDWT cycles（sequence/address/IRQ識別値を除く）。
`TIMING transfer`はrequest, req_irq, resp_irq, decode, received, publish, address。
`TIMING wait`はadc, use_time, use_seq, use_request, wait, ipsr, active_dma, active_adc。
`TIMING next`はnew_seq, next_use, next_seq, next_request。
