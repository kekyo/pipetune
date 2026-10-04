# 詳細

## Crosstalk Cancellation

EffeTuneで設定したCrosstalk Cancellationを、ステレオスピーカー再生に適用できます。
EffeTuneで左右それぞれの耳位置から左右スピーカーを測定し、4経路を割り当てて
プリセットを保存してください。各耳の2経路は同じ単一ポイント測定から選択します。
測定と設定方法は[EffeTuneの説明](https://github.com/Frieve-A/effetune/blob/v2.12.0/docs/i18n/ja/plugins/spatial.md#crosstalk-cancellation)を参照してください。

PipeTuneは保存済みプリセットと同じEffeTune設定ディレクトリの
`measurement-backups/<測定ID>.json`を自動で読み込みます。
設定ディレクトリは`$XDG_CONFIG_HOME/effetune`、未設定なら`~/.config/effetune`です。
デスクトップ版EffeTune 2.12.0は、測定をこの場所へ
[JSONとして自動バックアップ](https://github.com/Frieve-A/effetune/blob/v2.12.0/electron/measurement-backup-ipc.cjs)します。
プリセットだけを別のPCへコピーする場合は、参照される測定JSONもコピーしてください。
ブラウザ版ではIRを含む測定JSONをエクスポートし、元の測定IDをファイル名として
同じ場所に配置できます。

測定JSONの更新・削除・復旧はサービスが監視し、GTKを閉じていても反映します。
動作サンプリング周波数が変わった場合はフィルターを再生成します。
測定が欠落・不正な場合やステレオ以外を選択した場合は、このDSPだけを警告付きで
除外します。波形を含む測定バックアップがあることと、プリセットの測定IDを確認してください。
ヘッドホン用ではなく、測定した聴取位置で使用する機能です。
追加遅延はLatencyの指定値とタップ数の半分の合計（サンプル数）です。

## Spatial MapperとTV Audio Simulator

[Spatial Mapper](https://github.com/Frieve-A/effetune/blob/v2.12.0/docs/dsp/effects/spatial-mapper/index.md)
は、プリセットに保存されたDirect・Diffuse・Residualの行列で処理します。
Transparentも遅延付きの通過で、48 kHzでは2,560フレーム（約53 ms）の遅延が
加わります。報告する遅延は動作サンプリング周波数に追従します。

多チャンネルへアップミックスするには、EffeTuneでCh = Allを選択し、PipeTuneの
起動時に`--channels`で十分なチャンネル数を指定してください。既定は2chで、
プリセットを読み込んでもストリームの幅は変わりません。ペアを選択すると、そのペア内で
処理します。出力接続も確認してください。PipeTuneの6ch配置はFL, FR, FC, LFE,
RL, RRで、9ch以上はAUX配置です。12chの7.1.4プリセットは出力先への明示的な
接続が必要で、高さ方向のスピーカーを自動的に割り当てる機能はありません。

[TV Audio Simulator](https://github.com/Frieve-A/effetune/blob/v2.12.0/docs/dsp/effects/tv-audio-simulator/index.md)
は44.1、48、88.2、96、176.4、192、352.8、384 kHzに対応します。
Automaticで非対応のレートになった場合は、対応する固定レートを選択してください。
処理対象は単一chまたはステレオペアで指定します。
[上流カーネル](https://github.com/Frieve-A/effetune/blob/v2.12.0/dsp/plugins/lofi/tv_audio_simulator/kernel.cpp)
はAll指定でも先頭2chのみを加工し、3ch目以降は遅延を揃えずに通過させます。
複数ペアを加工する場合は、ペアごとにノードを追加してください。

Broadcast Offでも受信ノイズは出力され、Mix = 0でもdry経路の遅延は残ります。
無音時停止の既定値Ignoreでは、入力が無音でも受信ノイズを出力し続けます。
停止時間を明示した場合は、フェードして処理を停止し、入力再開時に復帰します。

## Attack Tonal BalanceとBass Extender

[Attack Tonal Balance](https://github.com/Frieve-A/effetune/blob/v2.12.0/docs/dsp/effects/attack-tonal-balance/index.md)
はアタック成分と持続音成分を調整します。48 kHzでは5,120フレーム（約106.67 ms）の
遅延が加わり、両ゲインが0 dBでも遅延は残ります。遅延は動作レートに応じて変わります。
両成分を無効にしても残差音が残る場合があります。元の音声経路へ戻すにはPipeTuneの
Bypassを使用してください。高レート・多チャンネルでは処理負荷が増えます。

[Bass Extender](https://github.com/Frieve-A/effetune/blob/v2.12.0/dsp/plugins/saturation/bass_extender/kernel.cpp)
は44.1、48、88.2、96、176.4、192 kHz、処理対象1〜2chに対応し、追加の処理遅延は
ありません。ステレオでは左右入力の平均から共通の低域成分を生成します。
多チャンネルのストリームでは、単一chまたはステレオペアを指定してください。
3ch以上をAllで選択すると読込エラーになります。Automaticで非対応レートになる場合は、
48 kHzなどの対応する固定レートを選択してください。

## Tonal Balance EQ

[Tonal Balance EQ](https://github.com/Frieve-A/effetune/blob/v2.12.0/docs/dsp/effects/tonal-balance-eq/index.md)
は、音楽のスペクトルを測定し、音色のバランスを徐々に補正します。EffeTuneでAll、
スタイル別の目標、またはTiltとTarget Adjustを設定し、保存したプリセットを
PipeTuneで読み込んでください。補正には測定の蓄積が必要です。既定の平均時間では、
約30秒の音楽を処理してから比較してください。選択された全チャンネルに同じ補正を
適用し、処理遅延は追加しません。選択外のチャンネルは変更しません。

Averaging Time = 100（`at: 100`）は100秒の窓ではなく、作成またはリセット以降の
累積測定です。Measurement Paused（`mp`）は測定を止めますが、既に得られた補正で
音声処理を続けます。Amount = 0またはRange = 0では音声を変更しません。
Target Adjustは補正の目標を変更する設定で、固定EQを別に追加するものではありません。
ラウドネス調整によりピークが上がる場合があります。必要に応じて後段のゲインを下げるか、
リミッターを追加してください。[上流の処理契約](https://github.com/Frieve-A/effetune/blob/v2.12.0/dsp/plugins/eq/tonal_balance_eq/kernel.cpp)に従います。

## Rhythm Analyzerと無音時の休止

[Rhythm Analyzer](https://github.com/Frieve-A/effetune/blob/v2.12.0/docs/dsp/effects/rhythm-analyzer/index.md)
は、PipeTuneでも解析を実行します。Metronome Click（`ck`）は既定でオフで、
PCMを変更しませんが、解析の処理負荷があり、有効DSP数にも含まれます。オンの場合は、
一定の拍を検出してからクリック音を加えます。意図したテンポの半分や倍で検出される場合は、
EffeTuneでMin BPM / Max BPMを調整してください。

クリックは選択範囲の先頭2チャンネルへ同じ音を加え、モノラルの場合は1チャンネルへ
加えます。多チャンネルでCh = Allを選んでも、3チャンネル目以降は変更しません。
別のペアへ出したい場合は、そのペアを選択してください。報告する音声遅延は0ですが、
拍の検出には時間が必要です。後段のエフェクトはクリックも処理します。
PipeTuneにはプリセット内の名前と状態を表示し、上流の解析グラフは表示しません。
[上流のチャンネル・クリック契約](https://github.com/Frieve-A/effetune/blob/v2.12.0/dsp/plugins/analyzer/rhythm_analyzer/kernel.cpp)を参照してください。

「無音時にDSPを休止」がオンの場合、これらのDSPも、無音入力が続くとフェード後に
リセットして休止します。Tonalの累積測定とRhythmの拍検出はクリアされ、入力再開後に
測定し直します。クリックがオンの場合も同様です。無音中もホストによるリセットをせず
処理を続けたい場合は、無音時の休止をオフ（CLIではIgnore）にしてください。
静かな入力を測定に含めるかどうかは、各DSPの処理に従います。休止をオフにしても、
プリセットの再読込、サンプルレート変更、バックエンド変更では測定を開始し直します。

## Bass Management

[Bass Management](https://github.com/Frieve-A/effetune/blob/v2.12.0/docs/dsp/effects/bass-management/index.md)
のIIRとLinearに対応します。Ch = Allを選択し、PipeTuneの起動時に
`--channels`で必要なチャンネル数（1〜16）を指定してください。EffeTuneで入力の
役割とサブウーファーへの宛先を設定してからプリセットを保存します。サブ出力は
Full RangeまたはManagedのメイン出力を兼ねられません。サブ出力を設定した場合、ManagedとLFEには、
設定済みサブ出力の中から有効な宛先が必要です。不正な経路は読込エラーになり、
現在動作中のパイプラインを維持します。

実際の出力接続も確認してください。プリセットのch番号から物理スピーカーを自動的に
割り当てる機能はなく、9ch以上はAUX配置です。複数サブへの分配は入力を等分する
`1 / 宛先数`です。IIRには追加の処理遅延がありません。Linearはプリセットから
必要なフィルターを生成するため、別途IRファイルを用意する必要はありません。

| Linearのタップ数 | 追加遅延 | 48 kHzの場合 |
| --- | --- | --- |
| 8,192 | 4,224フレーム | 88 ms |
| 16,384 | 8,320フレーム | 約173.33 ms |
| 32,768 | 16,512フレーム | 344 ms |

サブが未設定でもLinearの遅延は残ります。レートやバックエンド変更時には
フィルターを再生成し、準備中はサブ出力が無音になる場合があります。
アセットと処理領域の32 MiB制限を超える構成はエラーになり、タップ数を自動的に
減らすことはありません。[上流の処理・アセット契約](https://github.com/Frieve-A/effetune/blob/v2.12.0/dsp/plugins/basics/bass_management/kernel.cpp)に従います。

## オーバーサンプリングと遅延調整

Saturation、Dynamic Saturation、Exciter、Hard Clipping、Harmonic Distortion、
Multiband Saturationは1・2・4・8倍、Hard Clippingはさらに16倍に対応します。
既定は1倍で、有効な2倍以上の設定ではdry混合を含め64フレームの遅延が加わります。
プリセット再読込時に遅延表示も更新されます。
[上流のオーバーサンプリング契約](https://github.com/Frieve-A/effetune/blob/v2.12.0/dsp/include/effetune/dsp/oversampled_shaper.h)を参照してください。

Brickwall LimiterのOS有効時の遅延はlookahead＋64フレームです。
1倍では近似逆数計算により、設定した上限をわずかに超える場合があります。
PipeTuneはこの上流の音声を維持します。出力サンプルを設定上限内に収める必要が
ある場合は2倍以上を選択してください。
[Limiterの実装](https://github.com/Frieve-A/effetune/blob/v2.12.0/dsp/plugins/dynamics/brickwall_limiter/kernel.cpp)を参照してください。

[Time Alignment](https://github.com/Frieve-A/effetune/blob/v2.12.0/dsp/plugins/delay/time_alignment/kernel.cpp)
は最大500 msに対応します。これはスピーカー配置を合わせるための意図的な遅延で、
ホストの自動遅延補償には加算しません。

## 制約

FIR Crossover、5Band FIR PEQ、Group Delay EQ、Group Delay PEQに対応しています。
PipeTuneは、プリセットのパラメータと現在のサンプリング周波数から、これらのDSPに
必要な畳み込み係数を再生成します。FIR CrossoverはプリセットのCh指定に従います。
処理対象が2chなら音声を加工せず、追加遅延もありません。4〜16の偶数chなら、
入力のステレオペアを周波数帯域に分割します。多チャンネルのバスで分割するには
Allを選択してください。既定のステレオペアや別のステレオペアを選択した場合は、
バスの幅にかかわらず無加工で通過します。単一chや奇数chの指定では警告付きで除外します。

Level Meter、Note Spectrogram、Pitch Meter、Oscilloscope、Spectrogram、Spectrum Analyzer、
Stereo Meter、Chroma Spiral、Analog Meterの9種類は警告なしで無視します。有効DSP数や遅延には含めず、バス間の転送も
行いません。表示機能に加えて音声を加工するDSPは、引き続き処理します。

Room EQとIR Reverbには対応していません。Room EQプリセットが参照する測定データは、
EffeTuneの
[measurement store](https://github.com/Frieve-A/effetune/blob/bedc6c662a6edc88c9644b7e00cec9122a250cfb/js/measurement-store/client.js#L71)
を介して解決されます。IR Reverbは、EffeTuneの
[IR library](https://github.com/Frieve-A/effetune/blob/bedc6c662a6edc88c9644b7e00cec9122a250cfb/plugins/reverb/ir_reverb.js#L766-L802)
を介してコンテンツ識別子を解決します。必要なPCMデータは`.effetune_preset`に含まれないため、
PipeTuneはこれらのDSPを警告付きで除外します。
