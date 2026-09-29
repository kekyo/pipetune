# 詳細

## Crosstalk Cancellation

EffeTuneで設定したCrosstalk Cancellationを、ステレオスピーカー再生に適用できます。
EffeTuneで左右それぞれの耳位置から左右スピーカーを測定し、4経路を割り当てて
プリセットを保存してください。各耳の2経路は同じ単一ポイント測定から選択します。
測定と設定方法は[EffeTuneの説明](https://github.com/Frieve-A/effetune/blob/v2.10.0/docs/i18n/ja/plugins/spatial.md#crosstalk-cancellation)を参照してください。

PipeTuneは保存済みプリセットと同じEffeTune設定ディレクトリの
`measurement-backups/<測定ID>.json`を自動で読み込みます。
設定ディレクトリは`$XDG_CONFIG_HOME/effetune`、未設定なら`~/.config/effetune`です。
デスクトップ版EffeTune 2.10.0は、測定をこの場所へ
[JSONとして自動バックアップ](https://github.com/Frieve-A/effetune/blob/v2.10.0/electron/measurement-backup-ipc.cjs)します。
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

[Spatial Mapper](https://github.com/Frieve-A/effetune/blob/v2.10.0/docs/dsp/effects/spatial-mapper/index.md)
は、プリセットに保存されたDirect・Diffuse・Residualの行列で処理します。
Transparentも遅延付きの通過で、48 kHzでは2,560フレーム（約53 ms）の遅延が
加わります。報告する遅延は動作サンプリング周波数に追従します。

多チャンネルへアップミックスするには、EffeTuneでCh = Allを選択し、PipeTuneの
起動時に`--channels`で十分なチャンネル数を指定してください。既定は2chで、
プリセットを読み込んでもストリームの幅は変わりません。ペアを選択すると、そのペア内で
処理します。出力接続も確認してください。PipeTuneの6ch配置はFL, FR, FC, LFE,
RL, RRで、9ch以上はAUX配置です。12chの7.1.4プリセットは出力先への明示的な
接続が必要で、高さ方向のスピーカーを自動的に割り当てる機能はありません。

[TV Audio Simulator](https://github.com/Frieve-A/effetune/blob/v2.10.0/docs/dsp/effects/tv-audio-simulator/index.md)
は44.1、48、88.2、96、176.4、192、352.8、384 kHzに対応します。
Automaticで非対応のレートになった場合は、対応する固定レートを選択してください。
処理対象は単一chまたはステレオペアで指定します。
[上流カーネル](https://github.com/Frieve-A/effetune/blob/v2.10.0/dsp/plugins/lofi/tv_audio_simulator/kernel.cpp)
はAll指定でも先頭2chのみを加工し、3ch目以降は遅延を揃えずに通過させます。
複数ペアを加工する場合は、ペアごとにノードを追加してください。

Broadcast Offでも受信ノイズは出力され、Mix = 0でもdry経路の遅延は残ります。
無音時停止の既定値Ignoreでは、入力が無音でも受信ノイズを出力し続けます。
停止時間を明示した場合は、フェードして処理を停止し、入力再開時に復帰します。

## 制約

FIR Crossover、5Band FIR PEQ、Group Delay EQ、Group Delay PEQに対応しています。
PipeTuneは、プリセットのパラメータと現在のサンプリング周波数から、これらのDSPに
必要な畳み込み係数を再生成します。FIR CrossoverはプリセットのCh指定に従います。
処理対象が2chなら音声を加工せず、追加遅延もありません。4〜16の偶数chなら、
入力のステレオペアを周波数帯域に分割します。多チャンネルのバスで分割するには
Allを選択してください。既定のステレオペアや別のステレオペアを選択した場合は、
バスの幅にかかわらず無加工で通過します。単一chや奇数chの指定では警告付きで除外します。

Level Meter、Note Spectrogram、Pitch Meter、Oscilloscope、Spectrogram、Spectrum Analyzer、
Stereo Meterは警告なしで無視します。有効DSP数や遅延には含めず、バス間の転送も
行いません。表示機能に加えて音声を加工するDSPは、引き続き処理します。

Room EQとIR Reverbには対応していません。Room EQプリセットが参照する測定データは、
EffeTuneの
[measurement store](https://github.com/Frieve-A/effetune/blob/bedc6c662a6edc88c9644b7e00cec9122a250cfb/js/measurement-store/client.js#L71)
を介して解決されます。IR Reverbは、EffeTuneの
[IR library](https://github.com/Frieve-A/effetune/blob/bedc6c662a6edc88c9644b7e00cec9122a250cfb/plugins/reverb/ir_reverb.js#L766-L802)
を介してコンテンツ識別子を解決します。必要なPCMデータは`.effetune_preset`に含まれないため、
PipeTuneはこれらのDSPを警告付きで除外します。
