# CPU占有率測定（2026-10-16）

本書は計測当時の記録。CPU占有率測定モジュール、IRQ/mainの計測フック、`CPU_PROFILE`設定、専用テスト・集計ツールは削除済み。以下のビルド・実行手順は当時のもので、現在のソースには適用できない。測定ログと結果は保存している。

## 結果

1msフィードバック、CAN電流制御、5A負荷回転中の**待機ループを除く実処理占有率は、計測コード込みで約96.9%**。PWM停止中は約8.04%。運転時の余裕は大きくない。これは計測用Release版の値で、計測を外した通常FWやDebug版の正確な占有率を示すものではない。

| 区間 | PWM停止中 | 5A回転中・状態照会なし |
|---|---:|---:|
| ADC IRQ | 0% | 約62.36% |
| TIM IRQ | 0% | 約9.44% |
| SPI/DMA IRQ | 約0.89% | 約17.68% |
| CAN IRQ | 約0.28% | 約0.51% |
| UART/DMA IRQ | 約0.004% | 0% |
| SysTick | 約0.20% | 約0.21% |
| main | 約6.66% | 約6.72% |
| 待機（HAL_Delay内、IRQを除く） | 約91.96% | 約3.08% |

CANのパック・フィードバック生成・指令更新はmainに含まれるため、CAN全体の負荷が0.51%という意味ではない。mainには電源ADCの取得待ちなども含む。`HAL_Delay`自体はビジーウェイトなので、CPUが物理的にスリープしている割合ではない。

ログ：`build/cpu_profile_5a_buffered.log`。停止中は窓21〜23、5Aのホスト状態照会なし区間は窓29・30を採用。後者は96.931% / 96.906%、窓末尾の実Iqは4.955A / 4.859A。各窓約1秒。窓末尾Iqは窓内平均電流ではない。記録落ち0、全区分の合計が経過サイクルと一致した。

集計：`python tools/analyze_cpu_profile.py build/cpu_profile_5a_buffered.log --windows 21,22,23,29,30`。集計済み結果は`build/cpu_profile_summary.json`。UARTに残っていた前回FWの旧形式ログを除外し、同じ窓の二重記録は一度だけ扱う。

## 計測方法と影響

- `CPU_PROFILE=ON`専用ビルドで、DWT CYCCNTの差分をmain/待機/IRQに振り分ける。現在の区分をIRQ入口で保存し、出口で復元するため、待機中に入ったIRQと入れ子IRQを二重計上しない。
- CubeMXが生成するIRQファイルのUSER CODE区間だけにフックを置いた。SPI/DMAの早期returnも復元する。CubeMX設定は変更していない。
- mainのHAL_Delay前後で待機区間へ切り替える。1秒ごとに短いPRIMASK区間でスナップショットを取り、16窓のRAMリングへ保存する。運転中は計測値をprintfせず、FOC停止後に1ループ1行ずつ出す。
- 通常ビルドは`CPU_PROFILE=OFF`が既定。インラインの空関数になるため、IRQへの計測呼出しは残らない。通常Debug ELFのシンボルにもCpuProfile関数がないことを確認した。
- 計測関数自身・短い割り込み禁止区間のコストを含む。ハードウェアの例外entry/exit、Cのprologue/epilogueの一部は境界の前後区分に入る。通常FWの占有率を得るためのオーバーヘッド補正は行っていない。
- 最初の方式では運転中にも結果をUART出力していた。2A段階でFOCの`main stale`保護が作動したため、期限は緩和せず、停止後出力へ変更した。ログは`build/cpu_profile_5a.log`。これは計測が挙動へ影響する実例であり、計測値を通常FWの値と同一視しない。

RAM蓄積版では2→3→4→5A、5Aで約3秒の照会なし測定、通電中の通信途絶停止、非ゼロ再送時の再始動禁止、明示再アーム、disarm停止まで合格。計測版のADC制御区間最大27.212µs、角度age最大161.475µsで、計測前より角度取得の遅延が増えた。最悪実行時間や長期安定性を保証する測定ではない。

## ビルドと復帰

`-DCPU_PROFILE=ON`で計測版を作る。今回の計測は`build/cpu_profile_release`、Flash 116552 byte / RAM 16632 byte。`test_cpu_profile.py`で排他的集計・待機中IRQ・DWT周回・PRIMASK復元・運転中出力抑止・16窓バッファを検証した。計測版と通常Debug/Release版で既存10種類のモデル試験も合格。

ユーザー指示に従い、CANフィードバックの10ms選択肢とmainの10ms版用分岐を削除した。現在は1ms固定、main末尾はHAL_Delay(0)。過去の10ms試験記録とビルド成果物は比較資料として残している。

測定後は計測OFFの`build/can_baseline_debug/HiradoraGen3FW.elf`へ書き戻し、verify成功。名前にbaselineとあるが、現在は1ms固定版である。通常DebugはFlash 128176 byte / RAM 15744 byte、ReleaseはFlash 115536 byte / RAM 15736 byte。最後にPWM stopped/FOC off/CAN state=0とTX/1msを確認済み。

次の改善対象はADC/TIM/SPI経路と、表示・main処理の負荷。CAN IRQ単独は比較的小さい。計測をさらに軽くした方式か外部トレースで通常FWの余裕を再評価してから、処理追加や周期短縮を判断する。
