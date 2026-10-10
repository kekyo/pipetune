# 詳細

## Crosstalk Cancellation

EffeTuneで設定したCrosstalk Cancellationを、ステレオスピーカー再生に適用できます。
EffeTuneで左右それぞれの耳位置から左右スピーカーを測定し、4経路を割り当てて
プリセットを保存してください。各耳の2経路は同じ単一ポイント測定から選択します。
測定と設定方法は[EffeTuneの説明](https://github.com/Frieve-A/effetune/blob/v2.13.0/docs/i18n/ja/plugins/spatial.md#crosstalk-cancellation)を参照してください。

PipeTuneは保存済みプリセットと同じEffeTune設定ディレクトリの
`measurement-backups/<測定ID>.json`を自動で読み込みます。
設定ディレクトリは`$XDG_CONFIG_HOME/effetune`、未設定なら`~/.config/effetune`です。
デスクトップ版EffeTune 2.13.0は、測定をこの場所へ
[JSONとして自動バックアップ](https://github.com/Frieve-A/effetune/blob/v2.13.0/electron/measurement-backup-ipc.cjs)します。
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

[Spatial Mapper](https://github.com/Frieve-A/effetune/blob/v2.13.0/docs/dsp/effects/spatial-mapper/index.md)
は、プリセットに保存されたDirect・Diffuse・Residualの行列で処理します。
Transparentも遅延付きの通過で、48 kHzでは2,560フレーム（約53 ms）の遅延が
加わります。報告する遅延は動作サンプリング周波数に追従します。

多チャンネルへアップミックスするには、EffeTuneでCh = Allを選択し、PipeTuneの
起動時に`--channels`で十分なチャンネル数を指定してください。既定は2chで、
プリセットを読み込んでもストリームの幅は変わりません。ペアを選択すると、そのペア内で
処理します。出力接続も確認してください。PipeTuneの6ch配置はFL, FR, FC, LFE,
RL, RRで、9ch以上はAUX配置です。12chの7.1.4プリセットは出力先への明示的な
接続が必要で、高さ方向のスピーカーを自動的に割り当てる機能はありません。

[TV Audio Simulator](https://github.com/Frieve-A/effetune/blob/v2.13.0/docs/dsp/effects/tv-audio-simulator/index.md)
は44.1、48、88.2、96、176.4、192、352.8、384 kHzに対応します。
Automaticで非対応のレートになった場合は、対応する固定レートを選択してください。
処理対象は単一chまたはステレオペアで指定します。
[上流カーネル](https://github.com/Frieve-A/effetune/blob/v2.13.0/dsp/plugins/lofi/tv_audio_simulator/kernel.cpp)
はAll指定でも先頭2chのみを加工し、3ch目以降は遅延を揃えずに通過させます。
複数ペアを加工する場合は、ペアごとにノードを追加してください。

Broadcast Offでも受信ノイズは出力され、Mix = 0でもdry経路の遅延は残ります。
無音時停止の既定値Ignoreでは、入力が無音でも受信ノイズを出力し続けます。
停止時間を明示した場合は、フェードして処理を停止し、入力再開時に復帰します。

## Attack Tonal BalanceとBass Extender

[Attack Tonal Balance](https://github.com/Frieve-A/effetune/blob/v2.13.0/docs/dsp/effects/attack-tonal-balance/index.md)
はアタック成分と持続音成分を調整します。48 kHzでは5,120フレーム（約106.67 ms）の
遅延が加わり、両ゲインが0 dBでも遅延は残ります。遅延は動作レートに応じて変わります。
両成分を無効にしても残差音が残る場合があります。元の音声経路へ戻すにはPipeTuneの
Bypassを使用してください。高レート・多チャンネルでは処理負荷が増えます。

[Bass Extender](https://github.com/Frieve-A/effetune/blob/v2.13.0/dsp/plugins/saturation/bass_extender/kernel.cpp)
は44.1、48、88.2、96、176.4、192 kHz、処理対象1〜2chに対応し、追加の処理遅延は
ありません。ステレオでは左右入力の平均から共通の低域成分を生成します。
多チャンネルのストリームでは、単一chまたはステレオペアを指定してください。
3ch以上をAllで選択すると読込エラーになります。Automaticで非対応レートになる場合は、
48 kHzなどの対応する固定レートを選択してください。

## Tonal Balance EQ

[Tonal Balance EQ](https://github.com/Frieve-A/effetune/blob/v2.13.0/docs/dsp/effects/tonal-balance-eq/index.md)
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
リミッターを追加してください。[上流の処理契約](https://github.com/Frieve-A/effetune/blob/v2.13.0/dsp/plugins/eq/tonal_balance_eq/kernel.cpp)に従います。

## Rhythm Analyzerと無音時の休止

[Rhythm Analyzer](https://github.com/Frieve-A/effetune/blob/v2.13.0/docs/dsp/effects/rhythm-analyzer/index.md)
は、PipeTuneでも解析を実行します。Metronome Click（`ck`）は既定でオフで、
PCMを変更しませんが、解析の処理負荷があり、有効DSP数にも含まれます。オンの場合は、
一定の拍を検出してからクリック音を加えます。意図したテンポの半分や倍で検出される場合は、
EffeTuneでMin BPM / Max BPMを調整してください。

クリックは選択範囲の先頭2チャンネルへ同じ音を加え、モノラルの場合は1チャンネルへ
加えます。多チャンネルでCh = Allを選んでも、3チャンネル目以降は変更しません。
別のペアへ出したい場合は、そのペアを選択してください。報告する音声遅延は0ですが、
拍の検出には時間が必要です。後段のエフェクトはクリックも処理します。
PipeTuneにはプリセット内の名前と状態を表示し、上流の解析グラフは表示しません。
[上流のチャンネル・クリック契約](https://github.com/Frieve-A/effetune/blob/v2.13.0/dsp/plugins/analyzer/rhythm_analyzer/kernel.cpp)を参照してください。

対応レートは8、11.025、16、22.05、24、32、44.1、48、88.2、96、176.4、192、352.8、384 kHzです。
それ以外では解析・クリックを行わず入力を通します。Automaticで別のレートになった場合は、
対応する固定レートを選択してください。[上流のレートフィルター](https://github.com/Frieve-A/effetune/blob/v2.13.0/dsp/plugins/analyzer/rhythm_analyzer/rhythm_d_tables.h)に従います。

「無音時にDSPを休止」がオンの場合、これらのDSPも、無音入力が続くとフェード後に
リセットして休止します。Tonalの累積測定とRhythmの拍検出はクリアされ、入力再開後に
測定し直します。クリックがオンの場合も同様です。無音中もホストによるリセットをせず
処理を続けたい場合は、無音時の休止をオフ（CLIではIgnore）にしてください。
静かな入力を測定に含めるかどうかは、各DSPの処理に従います。休止をオフにしても、
プリセットの再読込、サンプルレート変更、バックエンド変更では測定を開始し直します。

PipeTuneの休止設定とは独立して、EffeTune 2.13.0のRhythmは約1秒のデジタル無音で
内部の検出状態をクリアします。Ignoreでも、次の拍は再検出が必要です。
[上流の無音処理](https://github.com/Frieve-A/effetune/blob/v2.13.0/dsp/plugins/analyzer/rhythm_analyzer/rd6_engine.h)を参照してください。

## Bass Management

[Bass Management](https://github.com/Frieve-A/effetune/blob/v2.13.0/docs/dsp/effects/bass-management/index.md)
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
減らすことはありません。[上流の処理・アセット契約](https://github.com/Frieve-A/effetune/blob/v2.13.0/dsp/plugins/basics/bass_management/kernel.cpp)に従います。

## オーバーサンプリングと遅延調整

Saturation、Dynamic Saturation、Exciter、Hard Clipping、Harmonic Distortion、
Multiband Saturationは1・2・4・8倍、Hard Clippingはさらに16倍に対応します。
既定は1倍で、有効な2倍以上の設定ではdry混合を含め64フレームの遅延が加わります。
プリセット再読込時に遅延表示も更新されます。
[上流のオーバーサンプリング契約](https://github.com/Frieve-A/effetune/blob/v2.13.0/dsp/include/effetune/dsp/oversampled_shaper.h)を参照してください。

Brickwall LimiterのOS有効時の遅延はlookahead＋64フレームです。
1倍では近似逆数計算により、設定した上限をわずかに超える場合があります。
PipeTuneはこの上流の音声を維持します。出力サンプルを設定上限内に収める必要が
ある場合は2倍以上を選択してください。
[Limiterの実装](https://github.com/Frieve-A/effetune/blob/v2.13.0/dsp/plugins/dynamics/brickwall_limiter/kernel.cpp)を参照してください。

[Time Alignment](https://github.com/Frieve-A/effetune/blob/v2.13.0/dsp/plugins/delay/time_alignment/kernel.cpp)
は最大500 msに対応します。これはスピーカー配置を合わせるための意図的な遅延で、
ホストの自動遅延補償には加算しません。

## Adaptive PredictionとCassette Artifacts

[Adaptive Prediction](https://github.com/Frieve-A/effetune/blob/v2.13.0/docs/dsp/effects/adaptive-prediction-effect/index.md)
は入力を学習し、原音・予測音・残差を混合します。単一chまたはステレオペアを選択してください。
3ch以上のAllは読込エラーになります。左右の学習は独立し、学習内容はプリセットに保存されません。
再読込・レートやバックエンドの変更・無音時休止では学習状態をリセットします。
休止がオフの場合は、DSPのHold・Freeze・無音時の規則に従って処理を継続します。
Holdで音を生成するには学習済みの信号が必要です。予測のGapは報告レイテンシーではありません。

[Cassette Artifacts](https://github.com/Frieve-A/effetune/blob/v2.13.0/docs/dsp/effects/cassette-artifacts/index.md)
はAll、Encode Only、Encode + Artifacts、Artifacts + Decode、Decode Onlyの5モードに対応し、
EffeTuneで保存した選択を反映します。Noise ReductionがOffの場合はDolby段を通過するため、
Dolby段だけを使うモードでは音声が変化しない場合があります。

## 外部アセット

SFZ Note Player、IR Reverb、Room EQは、デスクトップ版EffeTuneが登録したデータを
`$XDG_CONFIG_HOME/effetune`から読み込みます。XDG未指定時は`~/.config/effetune`です。
プリセットに保存されるのは参照なので、プリセットだけをコピーしても音源・IR・測定は移動しません。
同じユーザーのデスクトップ版EffeTuneに原本を登録してからPipeTuneで読み込んでください。
ブラウザー内にしかないライブラリは利用できません。

| DSP | 必要な保存データ |
| --- | --- |
| SFZ Note Player | `sfz-references.json`、登録されたSFZルート、include先の定義と参照サンプル |
| IR Reverb | `ir-library/index.json`と、そのindexが参照する単一原音またはL/R原音ペア |
| Room EQ | `measurement-backups/<id>.json`内の対象chの応答。Correctionには全測定点のIRも必要 |

保存形式は上流の[SFZ登録](https://github.com/Frieve-A/effetune/blob/v2.13.0/electron/sfz-library-ipc.js)、
[IRライブラリ](https://github.com/Frieve-A/effetune/blob/v2.13.0/electron/ir-library-ipc.js)、
[測定バックアップ](https://github.com/Frieve-A/effetune/blob/v2.13.0/electron/measurement-backup-ipc.cjs)に従います。
PipeTuneは原本を読み取るだけで、EffeTuneのライブラリを書き換えません。
初回準備・再生・キャッシュ再生成は全てネイティブ処理です。Node.jsやffmpegコマンド、
EffeTune / Electronの起動は必要ありません。

SFZは[EffeTune 2.13.0が対応する範囲](https://github.com/Frieve-A/effetune/blob/v2.13.0/docs/dsp/effects/sfz-note-player/index.md)の
include、領域継承、ループ、エンベロープ等を扱います。WAV・AIFF・FLACのmono / stereoサンプルに対応します。
バンク予算は既定256 MiBで、設定画面から64 / 128 / 256 / 512 / 1024 MiBを選べます。
直接実行では`--sfz-max-size MIB`、daemon設定では`PIPETUNE_SFZ_MAX_SIZE_MIB`を使います。
必要に応じて上流と同じ規則で再生可能な全キーを残しながらレイヤーを減らします。
除外領域、使用可能な部分音源での欠損サンプル、縮小バンクは稼働中DSPの診断へ表示します。
全キーを保持する最小構成でも予算に入らない音源は読込エラーになります。
参照サンプルは登録されたルート内に置いてください。

IR ReverbはWAV / IRS、AIFF、FLAC、MP3、Ogg、M4Aの原音を、インストール済みのFFmpegライブラリで
デコードします。元のch順を保持し、独立ch・true stereo・L/Rペア、Direct Cut・Decay・Trimと保存された
畳み込みモードを反映します。原音ファイルとデコードPCMはそれぞれ64 MiBまで、最大16chです。
畳み込み器の作業領域は32 MiBまでで、必要な短縮は診断へ表示します。
[上流のIR設定](https://github.com/Frieve-A/effetune/blob/v2.13.0/docs/dsp/effects/ir-reverb/index.md)を参照してください。

Room EQは測定から補正FIRを設計します。測定IRをそのままリバーブとして再生するものではありません。
Minimum / Linearは周波数応答だけのバックアップも利用できます。Correctionには完全なIRセットが必要で、
直接音位相・低域拡張・残響補正・Consensus・Reference Pointに対応します。
共有測定とch別測定は選択範囲に対応し、3/4ペアでは先頭2つの測定スロットが出力3/4chに適用されます。
単一ch選択は共有測定を使い、ch別の空欄は共有測定へフォールバックします。
意図的な未割当chは他chと遅延を揃えます。Allで8chを超える場合、実効タップ数は65,536までです。
位相・残響補正が縮小または省略される場合は、理由と実効量を稼働中の診断に表示します。
Gain・手動Delayも反映し、手動Delayは自動レイテンシー補償へ含めません。
[上流のRoom EQ設定](https://github.com/Frieve-A/effetune/blob/v2.13.0/docs/dsp/effects/room-eq/index.md)を参照してください。

準備結果は`$XDG_CACHE_HOME/pipetune/assets-v1`、XDG未指定時は`~/.cache/pipetune/assets-v1`へ保存します。
キャッシュは再生成できるデータで、合計2 GiBを上限に管理します。原本の代わりにはなりません。
原本の変更や欠損ファイルの復元は自動再読込され、破損キャッシュは再生成します。
キャッシュに書けなくても準備に成功した音声処理は利用できます。
SFZ / IR / Room EQの再読込に失敗した場合は、それまでの音声処理を保持してエラーを表示します。
未設定DSPは診断付きで有効のままです。無効ノード・無効Sectionでは外部ファイルを読みません。
Crosstalk Cancellationは従来どおり、不正測定のノードを警告付きで除外します。

準備中と旧・新パイプラインのアセットは、プロセス内で64bitなら4 GiB、32bitなら1 GiBの
メモリ予算を共有します。SFZのバンク予算とは別で、プロセス全体のRSS制限ではありません。
予算に収まらず読込が失敗した場合は、音源の予算、FIRサイズ、アセットDSP数を減らして再試行してください。

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
