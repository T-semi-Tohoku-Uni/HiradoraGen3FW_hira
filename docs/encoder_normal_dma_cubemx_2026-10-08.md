# 通常DMAへの切り戻しとCubeMX再生成

最新状態：2026-10-09に通常DMA版を書き込み、基本機能の実機確認まで実施。
結果と未解決事項は[実機確認記録](encoder_normal_dma_hardware_2026-10-09.md)を参照。
以下の未実施表記は各確認時点の記録。

循環DMA評価一式の保存先：コミット `100cccf`。
通常DMA通信の復元元：`84f831c`。採用判断は `encoder_adoption_decision_2026-10-08.md` を参照。

## ソース側の変更

- AS5047Pを要求＋NOP応答の2フレーム・通常DMAへ戻した。
- PWM動作中は非FOCのTIM1 ISR、またはFOC ADC処理末尾から要求を開始する。
  TIM1停止中はmainから約1 ms間隔で取得する（mainの実行状況で変動する）。
- TIM8との起動位相合わせ、公開位相統計、IRQトレースと `angle trace` コマンドを削除。
- TIM1のARR=4000に対応した周期・デューティ計算、HAL/IRQ最適化、整数・行分割の状態表示は維持。
- 通信異常からの再初期化はPWM停止中に限定。最初の異常記録は維持する。
- 試験ツールの分類と削除内容は [tools/README.md](../tools/README.md) を参照。

## CubeMXで修正をお願いする項目

**生成部分・`.ioc` はこちらで変更していない。以下の設定はユーザーによる再生成で反映済み（10月9日確認）。**

1. TIM8を無効化する。CH1 PWM、CH2とTIM8_CH2のDMA設定を削除する。
2. PA15を `GPIO_Output` に戻す。User Labelは `SPI1_SS`。
   初期出力High、Push Pull、No Pull、Low speedとする。
3. SPI1のDMAを次の構成に戻す。

| 設定 | SPI1_RX | SPI1_TX |
|---|---|---|
| DMAチャネル | DMA1 Channel2 | DMA1 Channel3 |
| Request | SPI1_RX | SPI1_TX（TIM8_CH2ではない） |
| Direction | Peripheral to Memory | Memory to Peripheral |
| Mode | Normal | Normal |
| Peripheral / Memory width | Half Word / Half Word | Half Word / Half Word |
| Peripheral increment | Disabled | Disabled |
| Memory increment | Enabled | Enabled |
| DMA priority | High（現状維持） | High |

転送長は各1ワードなので、Memory incrementは旧通常DMA版と同じEnabledでよい。
DMA1 Channel2/3、SPI1の割り込みを有効、NVIC preemption priority=1、subpriority=0とする。
SPI1の16 bit、Master、Full Duplex、Software NSS、CPOL Low/CPHA 2 Edge、/32は維持する。

**TIM1は変更しない。** PSC=0、Center-aligned mode 1、ARR=4000、RCR=1、CH4 pulse=3999を維持する。
TIM1/ADCの割り込み優先度と他のADC・UART・DMA設定も維持する。

## 再生成後に確認する点

- `hspi1.hdmatx` が `hdma_spi1_tx` にリンクされ、両DMAがNormalになっている。
- PA15が出力Highで初期化され、TIM8の初期化・DMAハンドルが消えている。
- `AS5047P_Init(&hspi1, &htim1)` とDMA IRQのUSER CODEが保持されている。
- DMA1 Channel3の生成HAL呼び出しも `hdma_spi1_tx` を対象としている。
- Release/Debugを再ビルドし、停止状態で初期化と手回しを確認する。その後、ADCログ併用、低電圧正逆試験へ進む。

切り戻し直後（10月8日）の生成コードは循環DMA用だったため、新ドライバは設定不一致として初期化を拒否する状態だった。
ソースのビルド成功と、実機で通常DMAを使える状態になったことは別である。
既存Flash校正データは変更していない。今回の作業では実機書き込み・回転試験を実施しない。

## 再生成前の検証結果

- Release / Debugビルド成功。RAM 15,216 B、FlashはRelease 104,192 B / Debug 125,392 B。
- `test_current_pi.py`、`test_encoder_h2.py`、`test_motor_period.py`、
  `test_h2_calibration.py`、`test_cal_map.py` は両ビルドで成功。
  校正の659項目、正逆取得、停止操作、10種の異常シナリオも成功。
- 整理後のPython全ファイルと変更したPowerShell 3ファイルの構文確認成功。
- `.ioc`、MSP、生成CMakeは不変。main/IRQの変更はUSER CODE内だけであることを比較確認済み。
- 通常DMAの実通信・遅延・回転性能は未再検証。上記エミュレーションの成功では代替できない。

## 2026-10-09 再生成後の確認

ユーザーによるCubeMX再生成を確認。生成部分への手修正は行っていない。

- SPI1_RX/TX：DMA1 Channel2/3、Normal、Half Word、High、リンク先ともに指定どおり。
- PA15：初期High、Output Push Pull、No Pull、Low speed。
- TIM8の初期化・DMA設定は削除済み。
- TIM1：PSC=0、Center-aligned mode 1、ARR=4000、RCR=1、CH4=3999を維持。
- mainのAS5047P初期化、RX/TX/SPI IRQのUSER CODEを保持。
- Release/Debugビルド成功。Flashは103,656 / 124,720 B、RAMは15,144 / 15,136 B。
- 再生成後の両ELFで既存5種類の回帰テストが成功。PI、H2補正、50 µs周期、H2校正、
  校正の659項目・正逆取得・停止操作・10種類の異常シナリオを確認した。
- 通常DMAの実通信と回転は未検証。書き込み・コミット・pushは行っていない。

**初回再生成時の追加修正（対応済み）：CubeMXのNVICで「DMA1 channel3 global interrupt」のPreemption Priorityを0から1へ変更。Sub Priorityは0のまま。**

`Core/Src/main.c` の `HAL_NVIC_SetPriority(DMA1_Channel3_IRQn, 0, 0)` と
`.ioc` の両方で0になっていた。DMA設定画面のHigh（バス仲裁優先度）は変更しない。
RXとSPI IRQは既にNVIC優先度1なので変更不要。
通常のTX完了割り込みはドライバで無効にするが、TXエラー時は共通の状態を更新するため、
RX/SPIと同じ優先度に揃えて相互の割り込みを避ける。

### 追加修正後の最終確認（2026-10-09）

- `.ioc` と生成された `main.c` でDMA1 Channel3がNVIC優先度1／0になったことを確認。
  RX（Channel2）とSPI1も1／0で一致している。
- Release／Debugの再ビルド成功。サイズは上記と同じ。`git diff --check` も成功。
- 今回のビルド更新対象はmainのみ。前回成功した5種類の演算・校正回帰テストは再実行していない。
- CubeMXの追加修正依頼は解消。次は実機への書き込みとPWM停止状態の通信確認。
  実機検証、コミット、pushはまだ行っていない。
