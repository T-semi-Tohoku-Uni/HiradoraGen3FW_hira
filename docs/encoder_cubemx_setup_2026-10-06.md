# エンコーダー移植用のCubeMX変更依頼（2026-10-06）

現在の `HiradoraGen3FW.ioc` と同梱HALを確認したうえでの初期設定案。
この文書の作成時点では `.ioc`、生成コード、FW実装は変更していない。
ユーザーがCubeMXで以下を設定・再生成し、その生成結果を確認してから通信処理を実装する。

## 1. 構成とタイミング

TIM8_CH1がPA15のCSを生成し、TIM8_CH2の比較イベントがDMA経由でSPI1のDRへ1語送る。
受信はSPI1_RXの循環DMA。TIM8の周期割り込みは使わない。

TIM8入力クロック160 MHz、PSC=0、ARR=7999で周期50 µs（20 kHz）。

| 周期内の時刻 | 動作 |
|---|---|
| 0 µs | CS High |
| 40 µs（CCR1=6400） | CS Low |
| 41 µs（CCR2=6560） | TX DMA要求発生 |
| 約44.2 µs＋DMA等の遅延 | SPI受信完了 |
| 50 µs | CS High、次周期へ |

CS下降からDMA要求まで1 µs、転送は5 MHz×16 bitで3.2 µs。
要求発生からCS上昇まで9 µsあり、理想転送終了から5.8 µsの余裕を取る。
これは設定上の計算であり、DMA競合・SPI起動遅延を含めた端子波形は実機で確認する。
この周期は通信フレームの周期。受信データは前フレームの要求に対応するため、角度の遅延が3.2 µsになるわけではない。

## 2. ピン設定

| ピン | 設定 |
|---|---|
| PA15 | GPIO_Outputから **TIM8_CH1（AF2）** へ変更 |
| PA15 User Label | `SPI1_SS` を維持 |
| PA15 GPIO mode | Alternate Function Push Pull |
| PA15 Pull | Pull-up |
| PA15 Speed | High |
| PB3/PB4/PB5 | SPI1 SCK/MISO/MOSIのまま |

PA15がロックされていて変更できない場合だけロック解除する。
CH2は内部比較専用で、GPIOピンは割り当てない。PA13/PA14のSWD設定も維持する。
Pull-upは出力が駆動されていない間の補助であり、CS停止処理の代わりにはしない。

## 3. TIM8を追加

Timers → TIM8で設定する。CubeMXの版により項目名の表記は多少異なる。

| 項目 | 値 |
|---|---|
| Clock Source | Internal Clock |
| Channel 1 | PWM Generation CH1（PA15へ出力） |
| Channel 2 | Output Compare No Output（内部比較のみ） |
| Prescaler | `0` |
| Counter Mode | Up |
| Counter Period / ARR | `7999` |
| Clock Division | DIV1 |
| Repetition Counter | `0` |
| Auto-reload preload | Disable |
| CH1 OC Mode | PWM mode 1 |
| CH1 Pulse / CCR1 | `6400` |
| CH1 polarity | High |
| CH1 Fast Mode | Disable |
| CH1 Output Idle State | Set（High） |
| CH2 OC Mode | Timing / Frozen（HALでは `TIM_OCMODE_TIMING`） |
| CH2 Pulse / CCR2 | `6560` |
| CH2 Fast Mode | Disable |
| Slave Mode | Disable |
| Master Output Trigger / Trigger 2 | Reset |
| Master/Slave Mode | Disable |
| Dead Time | `0` |
| Break / Break2 | Disable |
| Automatic Output | Disable |
| Off-State Selection for Idle mode（OSSI） | Enable |
| Off-State Selection for Run mode（OSSR） | Disable |
| Lock level | Off |

CH1はPWM1・極性Highなので、CNT<6400でCS High、その後Lowになる。
Idle StateとOSSIはMOE解除時のHigh保持に使う。実装側でも起動・停止時のCC1E/MOE/CENの順序を管理する。
CH1N/CH2Nの相補出力は使わない。

Capture/Compare DMA request selectionが表示される場合は **Capture/Compare event** を選ぶ（Update eventではない）。
表示されない場合は再生成後にCR2.CCDS=0となる初期化経路を確認し、通信モジュール側の起動処理でも設定する。

## 4. DMA Settings

先にSPI1の **SPI1_TX** DMA要求を削除してDMA1_Channel3を空け、その後TIM8に **TIM8_CH2** を追加する。
SPI1_RXは残し、ModeとPriorityを変更する。

| 項目 | SPI1_RX | TIM8_CH2（追加） |
|---|---|---|
| DMA channel | DMA1_Channel2 | DMA1_Channel3 |
| Direction | Peripheral to Memory | Memory to Peripheral |
| Mode | Circular | Circular |
| Peripheral increment | Disable | Disable |
| Memory increment | Enable | Disable |
| Peripheral data width | Half Word | Half Word |
| Memory data width | Half Word | Half Word |
| Priority | High | High |
| DMAMUX synchronization | Disable | Disable |
| Request generator | 使用しない | 使用しない |

USART1_TXのDMA1_Channel1は変更しない。

TXはRAM上の1語を繰り返し送る最小構成とするため、Memory incrementはDisable。
初期実装のRXは2語を交互に使い、HT/TCで各フレームの完了を通知する。
バッファ・転送長・SPI DRのアドレス・診断コマンドへの切替はFW側で実装するため、CubeMXでは入力しない。
診断コマンドへの変更は転送境界と対応づけ、次回DMA読出しと競合させない。

TIM8_CH2のDMAハンドルは `htim8.hdma[TIM_DMA_ID_CC2]` に結び付く想定。
`hspi1.hdmatx`には結び付けない。通常のHAL_TIM_OC_Start_DMAはCCRへの転送用なので、今回のSPI DR宛て転送の起動には使用しない。

## 5. SPI1とNVIC

SPI1本体は現行設定を維持する。

| SPI1項目 | 値 |
|---|---|
| Mode | Full-Duplex Master |
| Data Size | 16 Bits |
| First Bit | MSB First |
| CPOL / CPHA | Low / 2 Edge（Mode 1） |
| Prescaler | 32（APB2=160 MHzで5 MHz） |
| NSS | Software |
| NSS Pulse | Disable |
| CRC | Disable |

| NVIC項目 | Enable | Preemption / Subpriority |
|---|---|---|
| DMA1_Channel2 | Yes | 1 / 0（現状維持） |
| DMA1_Channel3 | Yes | 1 / 0（現状維持） |
| SPI1 global interrupt | Yes | 1 / 0（現状維持） |
| TIM8関連のUpdate / CC / Break / Trigger割り込み | No | 不要 |

DMA要求とタイマーのCPU割り込みは別物なので、TIM8のNVICを有効にする必要はない。
RXはHT/TC/TE、TXはエラー通知を利用する方針で、個別の割り込みビットは通信処理側で設定する。
ADC・TIM1の優先度0、UARTの優先度2など、その他のNVIC設定は変更しない。

## 6. 再生成時の依頼

1. 既存のプロジェクト出力先・ツールチェーン設定を維持する。
2. Code Generatorの「Keep User Code when re-generating」を有効にする。
3. 上記設定を保存してGenerate Codeを実行する。
4. 再生成したことを知らせていただく。その差分を確認してFW実装へ進む。

設定候補が画面に出ない場合は別のピンやDMA要求へ置き換えず、該当項目名を知らせていただく。

再生成だけでは新方式への移行は完了しない。既存USER CODEに残る `hdma_spi1_tx` 参照が未定義になる可能性があり、現行ドライバーも通常DMAとGPIO CSを前提としている。
この中間状態は書き込まず、通信実装とビルド確認を終えてから使用する。
これらUSER CODEの接続修正は後続で担当し、生成領域は手編集しない。

## 7. 再生成後に確認する内容

- `MX_TIM8_Init()`、`htim8`、TIM8_CH2用DMAハンドルが追加されたこと。
- PA15のAF2、CCR1=6400、CCR2=6560、ARR=7999、PSC=0。
- TIM8_CH2のDMA要求がChannel3へ、SPI1_RXがChannel2へ割り当てられたこと。
- TX/RXの方向・転送幅・循環モード・増分設定と、CC2のDMAハンドル関連付け。
- DMA初期化が使用前に行われ、TIM8の初期化で意図せずカウンターが起動しないこと。
- TIM1、ADC、UART、クロック等に意図しない変更がないこと。

比較の基準は既存Git版に保持する。GPIO CSとタイマーCSでは生成設定も異なるため、実行中の方式切替を追加することは前提にしない。

## 再生成の確認結果

Memory incrementのDisableへの修正・再生成を確認し、通信処理を実装済み。
生成領域と `.ioc` は再生成直後から変更せず、main/IRQの接続変更はUSER CODE内だけで行った。
Debug/Releaseビルドとソフトウェア試験は合格。書込み・実機試験は未実施。
[実装記録](encoder_tim8_2026-10-06.md)を参照。

### 初回再生成時の指摘（修正済み）

TIM8のPSC/ARR/CCR1/CCR2、CH1のPWM・Idle State・OSSI、CH2の内部比較、PA15のAF2、SPI1_RX循環DMA、TIM8_CH2のDMA要求とハンドル関連付けは確認済み。
TIM1・ADC・UART・クロック設定に今回の移植と無関係な変更は見当たらない。

修正依頼は **TIM8_CH2のDMA Memory incrementをDisableにする** 1点。
初回生成では `.ioc` と `stm32g4xx_hal_msp.c` の両方で `DMA_MINC_ENABLE` だった。
長さ1の循環転送では通常、各周回でアドレスが戻るため、これだけで直ちに不正転送になるという意味ではない。
ただし今回の固定1語送信の意図に設定を合わせ、実装の初期化チェックでもその条件を確認する。

Auto-reload preloadは依頼のDisableに対してEnableだったが、この設定はそのままでよい。
周期を動作中に変更しない構成であり、開始前に更新イベントでARR/PSCを反映し、フラグを消してからDMA要求を許可する。

既存USER CODEの `AS5047P_Init(&hspi1, &htim1)` とDMA1_Channel3 IRQ内の `hdma_spi1_tx` 参照は、後続のFW実装で修正する。
ユーザーによる手編集は不要。初回再生成の確認段階では生成コードもUSER CODEも変更していない。

## 参照

- 現在の `HiradoraGen3FW.ioc`: PA15、SPI1、DMA1_Channel1～3、APB2/TIM=160 MHz。
- 同梱 `stm32g4xx_hal_dma.h`: `DMA_REQUEST_TIM8_CH2=50`。
- 同梱 `stm32g4xx_hal_tim.h`: `TIM_DMA_CC2`、`TIM_DMA_ID_CC2`、`TIM_CCDMAREQUEST_CC`、Idle State/OSSI。
- [ST STM32G431データシート](https://www.st.com/resource/en/datasheet/stm32g431r8.pdf): PA15のAF2=TIM8_CH1。
- [ST RM0440](https://www.st.com/resource/en/reference_manual/rm0440-stm32g4-series-advanced-armbased-32bit-mcus-stmicroelectronics.pdf): Advanced-control timersの比較イベントとDMA要求。
