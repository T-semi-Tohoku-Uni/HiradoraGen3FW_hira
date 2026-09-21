# UART DMAロガーとTeleplot

4000点の保存配列を撤去し、64点×2のrawバッファと320 BのASCII送信バッファを使用します。
起動時の1000点オフセット校正も配列を使わず積算します。
DebugビルドのRAM使用量は29,648 Bから6,624 Bへ減り、23,024 B（約22.5 KiB）を削減しました。
ADCのdual injected同期取得は継続し、DMAを使用するのはUSART1の送信です。

## 接続と使い方

1. USART1（PA9 TX / PA10 RX、GND共通）をPCへ接続します。
2. Teleplotで該当COMポートを選び、**921600 bps / 8N1 / フロー制御なし**で接続します。
3. Teleplotのシリアル送信欄から`adc`を改行付きで送信します。

専用スクリプト・Python・UDP変換は不要です。
Teleplotの[標準シリアル形式](https://github.com/nesnes/teleplot/blob/main/vscode/README.md)を直接送信します。
通常のコマンド応答はテキスト、先頭が`>`の行はグラフとして扱われます。

| コマンド | 動作 |
|---|---|
| `adc` | 標準の間引き率100で開始。20 kHz取得時は約200 Hz |
| `adc <N>` | ADC取得N回につき1回記録。Nは1～1000 |
| `adc status` | 状態と累積欠落数を表示 |
| `adc stop` | 取得停止、端数を含む残データを送信。モーターの状態は維持 |
| `stop` | PWMとログを停止、残データを送信 |

モーター設定を先に行う場合は通常のコマンドで設定後、`adc`を送ります。
ロギング中は`adc stop`、`adc status`、`stop`を使用できます。
間引き率やモーター設定を変更する場合は`adc stop`の完了を待ってから操作してください。
NTC・角度ストリームはログ停止完了まで休止します。
PC側でシリアル接続を閉じるだけではモーターやロガーは停止しません。

## 出力形式

```text
>u1_a:5.000:1.234
>v_a:5.000:-2.345
>u2_a:5.000:1.230
>w_a:5.000:1.111
>sector:5.000:3
>sample:100
>log_overrun:0
```

電流4値とsectorは`>名前:取得時刻[ms]:値`です。
時刻はログ開始からのサンプル番号とTIM1周期から計算し、DMA送信待ちの遅延を除きます。
ミリ秒整数部は32 bitなので約49.7日で周回します。ログ再開時には時刻とサンプル番号をリセットします。
`sample`と`log_overrun`は`>名前:値`形式で、表示時刻には受信時刻を使います。
数値をprintf同様に整形してからDMAへ渡しており、バイナリフレームや独自CRCはありません。

- `u1_a` / `v_a`：ADC1 / ADC2のRank 1から換算した電流[A]。
- `u2_a` / `w_a`：ADC1 / ADC2のRank 2から換算した電流[A]。
- `sector`：120度駆動のステップ1～6。停止・手動PWM時は0。
- `sample`：間引き前のADC取得番号。32 bit、周回あり。
- `log_overrun`：FWで記録できなかった点数とTX DMAエラー・整形エラーで破棄した点数。

電流の校正方法は[シリアルコマンド一覧](serial_commands.md)を参照してください。
間引きは平均化やアンチエイリアスフィルターではありません。
CRC・受信側欠落計数はありません。UART経路での欠落はFWのoverrunには含まれません。

## 帯域とバッファ

921600 bps / 8N1の理論上限は92,160 B/sです。
1点は電流4行・sector・sample・overrunのASCII文字列で、おおむね150～220 Bです。
数字の桁数や他のコンソール出力によって送信量は変わります。

| 間引き率 | 20 kHz取得時の記録頻度 | 概算帯域 |
|---|---:|---:|
| `adc 400` | 50 Hz | 7.5～11 kB/s |
| `adc` / `adc 100` | 200 Hz | 30～44 kB/s |
| `adc 50` | 400 Hz | 60～88 kB/s（余裕が小さい） |
| `adc 10` | 2 kHz | 300～440 kB/s（帯域超過、欠落する） |
| `adc 1` | 20 kHz | 3～4.4 MB/s（帯域超過、欠落する） |

ADC ISRは整数値の格納と間引きだけを行います。文字列整形、電流換算、UART呼び出しは行いません。
メインループで1点ずつASCIIへ整形し、320 Bの送信バッファからDMAで送ります。
送信中の文字列は完了コールバックまで変更せず、通常のprintfも行の途中に割り込みません。
送信元のrawバッファは全点を送信するまで再利用しません。
64点未満でも約20 msごとに送信対象にするため、低い記録頻度でも64点が揃うまで待ちません。
`adc stop`でも端数を送信します。両バッファが使用中ならISRは待たず新しい点を破棄し、overrunを増やします。

## CubeMX設定

ボーレートは了承いただいた921600 bpsを維持しています。
それ以外のCubeMX生成設定は元の状態へ戻しました。新規DMAチャネルの追加は不要です。
既存設定を確認・変更する場合はCubeMXで行い、コードを再生成してください。

| 項目 | 設定 |
|---|---|
| USART1 | Asynchronous、TX/RX、921600 bps、8 bit、Parity None、1 Stop bit、Flow control None |
| USART1_TX DMA | 既存のDMA1 Channel1、Memory to Peripheral、Normalモード |
| DMAインクリメント | Memory有効、Peripheral無効 |
| DMAデータ幅 | Memory / PeripheralともByte |
| NVIC | DMA1 Channel1 global interruptとUSART1 global interruptを有効（両方必要） |

推奨：NVICのUSART1とDMA1 Channel1のpreemption priorityを**2**、subpriorityを**0**に設定してください。
ADC1_2とTIM1_UP_TIM16は現在の**0**を維持すると、ADC・モーター処理がUART処理を優先して実行できます。
この優先度変更は生成コード・.iocに適用していません。現在のUART/DMA優先度は元と同じ0です。
ADC regular DMAへの変更やDMA Circularモードへの変更は不要です。

## 検証

Debugビルドを確認済みです。実機のTeleplot接続・波形・送信負荷は未確認です。
実機では起動校正、`adc`による7信号の表示、`adc stop`の端数送信、`stop`のPWM停止、
`adc 10`など帯域超過時のoverrun増加を確認してください。
