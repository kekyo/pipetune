# EffeTune 2.10.0対応計画

## 目的と調査時点

EffeTuneの参照をv2.9.0 (`71a48971165e8608740f49075bb95ef403534d8f`) から公式v2.10.0 (`abca7ff96f48f9cacace4d3c6aef5c4d3407bd8f`) へ更新し、追加・変更されたネイティブDSPをPipeTuneのプリセット実行環境で扱えるようにする。上流DSPライブラリの版は0.9.0から0.10.0になる。アプリ版とDSPライブラリ版を区別する。

2026-09-29、PipeTune HEAD `c9cdbf4`のコード、上流の両タグ間の差分、公式リリースとタグ固定の文書・APIコメントを調査した。本書は実装前の計画であり、2.10.0への参照更新や製品コード変更、ビルド・実行検証はまだ行っていない。

基本方針は、Spatial MapperとTV Audio Simulatorを音声加工DSPとして実行し、Pitch Meterは既存の可視化専用DSPと同様に無警告で無視すること。これは現在のPipeTuneが持つ「音声を加工しない解析DSPは実行しない」という方針を継続する設計である。

## 差分と対応範囲

| 対象 | 2.9.0からの差分 | PipeTuneでの対応 |
| --- | --- | --- |
| Spatial Mapper | 新規。Direct / Diffuse / Residualの分離と最大16chの行列ルーティング。外部アセット不要 | 既存の配列パラメータパックとネイティブパイプラインを利用。処理幅、出力経路、遅延を重点検証 |
| TV Audio Simulator | 新規。アナログTV・NICAM音声、受信障害・ノイズの生成。外部アセット不要 | 通常の音声加工DSPとして実行。浮動小数点コンパイル条件、対応レート、無音時処理を確認 |
| Pitch Meter | 新規。音声を変更せず、基本周波数・音程等をテレメトリ出力 | カタログへ登録し、プリセット実行では除外 |
| Spectrogram / Spectrum Analyzer | 既存カーネル変更。`hq`パラメータと高品質対数周波数解析を追加 | パラメータ数・ハッシュを再生成しバックエンド契約へ反映。引き続きプリセット実行では除外 |
| 共通DSPヘッダー | `multires_spectrum.h`追加。`denormal_noise.h`の最大内部注入箇所数を38から44へ変更 | 上流のまま取り込む。後者はTV Audio Simulatorを含む上限の更新で、ノイズ振幅自体は変更なし |
| ネイティブビルド | 3種類のテスト追加、TV Audio Simulatorの`-ffp-contract=off`指定追加 | PipeTune独自共有バックエンドにも同じソース単位の条件を反映 |
| その他の既存音声加工DSP | 既存カーネルとその`params.json`に変更なし。JSにはスライダーの対数化等のUI変更 | 既存の音声golden・パラメータ比較を継続。ブラウザUIの移植は不要 |
| 上流core / 公開C ABI / vendor / パラメータ生成器 | 今回のタグ間に変更なし | 現行統合を維持し、実ロードで互換性を確認 |
| golden | 新規DSPと解析の新ケースを追加。既存音声バイナリの変更はなく、多数の既存JSONは参照ハッシュ更新 | メタデータ変更件数と音声仕様変更を混同しない。新規DSPの実PCM比較を追加 |

全バックエンドの登録数は100から103、実行を除外する可視化専用DSPは6から7になる。登録数は利用可能な音声加工DSP数ではない。Room EQ / IR Reverbの外部アセット対応状況も別に説明する。

周波数グラフの試聴、Sync Visuals to Audio、Windows更新、ブラウザ拡張、解析結果表示、パラメータ編集UI、Graph API公開、Room EQ / IR Reverbの新規対応は範囲外とする。上流に存在するすべてのUI機能を再実装する計画にはしない。

## 現行コードから分かる統合方法

- `pipetune/tools/generate-dsp-catalog.mjs`は上流registry・manifest・`params.json`からカタログを生成する。数値配列も展開可能で、新しいパラメータ形式の実装は現時点では不要と見込む。
- `pipetune/src/dsp_pipeline.cpp`はプリセット名の解決後、可視化専用DSPをパック・インスタンス生成・96ノード制限の計数より前に除外する。Pitch Meterもこの分類に追加する。
- 通常の音声加工DSPは`instanceCreate`、`instanceSetParams`、`pipelineConfigure`の既存経路を使う。遅延は`pipelineLatency`から取得する。新規2種類も外部アセットや独自の係数設計を必要としない。
- `pipetune/cmake/EffeTuneNativeBackends.cmake`は全`kernel.cpp`を収集するため、新規カーネルは参照更新で収集対象になる。ただし上流CMakeのソース固有コンパイル条件は自動継承されず、明示的な追従が必要。
- `pipetune/test/parameter-packer-parity.mjs`は全上流仕様を列挙してJSパッカーと比較する。既定値、境界値、配列経路を新規DSPにも適用できる。
- 現行artifactテストの音声入力生成はノイズを前提としている。Spatial Mapperのインパルスgoldenを比較する場合は、ノイズのまま流用せず実際のstimulusに対応させる。

### Spatial Mapper

#### パラメータと音声処理

`ic`、`bd`、`dr`、`sp`、`de`、`ph`、`ts`、`ep`と、`dm` / `fm` / `rm`各256要素を既存のカタログ生成とパッカーで扱う。合計776 float、3,104 bytesである。行列は出力行×入力列、16要素刻みの一次元配列。省略時は上流スキーマの単位行列を使用する。

完成したEffeTuneプリセットに保存される配列を入力契約とし、上流のパッカーと同じ結果を得る。上流UIの短い配列の正規化と、生のパッカーでの欠落要素の既定値処理は同一とは限らない。独自の補完を追加せず、完全な256要素の配列と、欠落・短い配列・境界値のパッカー比較を分けて確認する。システムプリセット名だけから行列を生成する機能は追加しない。

Transparentは遅延付きの入力再現であり、遅延0のバイパスではない。入力範囲内でどの成分も割り当てられない出力行は無音となり、入力範囲外で書き込み先になっていないチャンネルは同じ遅延を付けて通過する。負の係数とEnergy Preservationの有無も上流カーネルに委ねる。

#### 処理幅とスピーカー配置

現在の`parseChannel()`では指定なしは先頭ペア、`All` / `A`は全幅、`34`等はペア、数値は単一chとなる。Spatial Mapperのためにこれを強制上書きしない。`ic`は選択後の処理幅以下に上流カーネル内で制限され、行列の入力・出力番号もその選択範囲を基準とする。

アップミックスするには、プリセット側で`ch: "All"`を指定し、PipeTuneを十分な`--channels`で起動する必要がある。既定の2chではステレオ処理が可能だが、設定だけで6ch / 12chストリームに拡張されることはない。プリセット読込に伴うPipeWireストリーム幅の自動変更は追加しない。1chの検証では`All`または単一chを明示する。上流エンジンは実際の幅を超えるペア指定をスキップするため、指定なしで1chのDSP処理を検証したことにはしない。

上流の5.1 UpmixはL, R, C, LFE, Ls, Rs、7.1.4 UpmixはL, R, C, LFE, Ls, Rs, Lb, Rb, Ltf, Rtf, Ltb, Rtbを前提とする。現行`makeAudioInfo()`の6chはFL, FR, FC, LFE, RL, RR、8chはrearがsideより前、9ch以上はAUX列となる。DSP上のチャンネル番号の正しさと物理スピーカーへの接続を区別する。

今回の対応は既存チャンネル配置の範囲で行う。6chの出力位置は実接続で確認し、12ch以上はAUXと出力先の明示的な接続を前提とする。7.1.4の自動スピーカーマッピング対応とは表記しない。新しいスピーカー配置設定APIが必要になった場合は独立した計画として扱い、本計画に紛れ込ませない。

#### 遅延と負荷

上流はサンプルレートからFFT長を決め、FFT長とその1/4の和を遅延として報告する。48 kHzでは2,560 frames（約53.33 ms）、96 kHzでは5,120 frames、192 kHzでは10,240 framesとなる。`bd`の変更による解析バンド数とFFT長を混同しない。

既存の遅延取得・PipeWire通知経路を使い、別の固定遅延を足さない。音声の実遅延、複数DSPやバスを含む集計、レート変更とScalar/SIMD切替後の更新を検証する。最大16chや高レートは処理コストが大きくなるため、2/6/12/16chと代表レートの処理時間・メモリを記録し、すべてのCPUでリアルタイム処理可能とは主張しない。

上流goldenの参照は`production-native-promoted-v1`である。JSによる独立した音声実装との一致を証明するものではないため、Transparentの遅延付き再現、特定出力へのルーティング、無音行、未使用チャンネル通過などの意味的な検証を併用する。

### TV Audio Simulator

14 floatのパラメータを通常経路でパックする。テレメトリの有無で可視化専用に分類してはならない。`rd: false`は送信停止であってDSPバイパスではなく、受信ノイズが出る設定である。`mx: 0`でも内部のdry経路の遅延は残る。

上流が要求する`-ffp-contract=off`を、PipeTune共有バックエンドの既存ソース別指定へ追加する。ScalarだけでなくFMAを使用できるSIMD構成で、公式goldenとの誤差を確認する。goldenの許容絶対誤差は`1e-4`。誤差を隠すために許容値を緩めない。

対応レートは44.1 / 48 / 88.2 / 96 / 176.4 / 192 / 352.8 / 384 kHz。PipeTuneの固定選択5種類は含まれるが、Automaticで取得する任意のレートまで対応するわけではない。例えば32 kHzでは上流のprepareが失敗し、現行PipeTuneではインスタンス生成エラーとなる。この失敗を無警告の通過やサイレントなノード除外に変更しない。起動時・ライブプリセット変更時・レート変更時の既存の失敗処理と状態表示を全体テストで確認し、利用者には必要に応じて対応する固定レートを選ぶ方法を説明する。新たな汎用レート変換器は追加しない。

上流カーネルが加工するのは渡された範囲の先頭1〜2chで、3ch目以降は書き換えない。全ペアへ自動複製しない。ペア指定で対象を選び、複数ペアを処理する場合は複数ノードを使用する。All指定時の余剰chと遅延集計は実PCMで確認し、全chを均等に加工・遅延整合するという保証は付けない。

無音でも発音するDSPとして、既存の無音時停止ポリシーを維持する。既定の`ignore`では処理を継続し、タイムアウトを明示した場合は入力無音を基準にdraining、5 msのフェード、reset、sleepへ進む。TV Audio Simulatorだけ自動停止対象から例外扱いしない。フレーム数に基づくテストで、受信ノイズの継続、停止、入力復帰、レート・バックエンド再構築後の動作を確認する。

artifactテストでは既存のseed設定APIを用いる。製品のプリセット形式やCLIへseed項目は追加しない。8規格の処理、Stereo / Mono / Dual、Main / Sub、NICAMとアナログ経路の代表例を検証する。

### Pitch Meterと解析DSPの更新

`visualizationOnlyTypes`へ`PitchMeterPlugin`を追加する。既存6種類を含め、表示専用ノードは有効DSP数、追加遅延、96ノード上限に含めない。異なる入出力バスが指定されていても、そのノード由来のコピーは行わない。

Pitch Meter単独、既存解析との混在、加工DSP・Sectionとの混在、上限を超える個数を含むプリセットを検証する。比較対象は解析ノードを取り除いたプリセットであり、音声・遅延・警告が一致することを確認する。

Spectrogram / Spectrum Analyzerの`hq`を含むカタログとバックエンドのパラメータ契約は更新する。解析ノードのパラメータは実行準備に使わないため、`hq`の値によってPipeTuneの音声処理や負荷が変わらないことを確認する。新規Pitch Meterには外部モデルやアセットの追加はなく、既存Note Spectrogramのモデル組み込みも変更不要と見込む。

## インクリメンタルな実装手順

各段階はビルド・全体テストが可能で、CLIとPCM処理から結果を観測できる単位とする。製品コードを変更する前に問題を再現するテストを追加し、全体REDを確認してから修正し、全体GREENを確認する。既存実装で最初から成功する検証は回帰テストとして扱い、不要な製品変更や人為的なREDを作らない。

### 1. 2.10.0の取り込みと基本実行

まず2.9.0で現在の全体テストを実行し、基準を記録する。103カーネルの実ロード、新規2種類のプリセット処理、Pitch Meterの無警告除外を確認するテストを追加して全体REDを確認する。この段階のテストは未取得の新goldenファイルに依存せず、旧版の機能不足で失敗するものとする。

サブモジュール参照だけを指定の公式コミットへ更新し、Pitch Meterの分類とTV Audio Simulatorのコンパイル条件を合わせる。カタログを再生成し、ハッシュ・配列容量とABI、既存の生成ABIパッチ、モデルリンクが成立することを確認する。`deps/`内のソースは編集しない。

完了条件: 全体GREEN、CLI/GTKのEffeTune版表示が2.10.0、全バックエンドの登録が103種類、新規加工DSPを含むプリセットが48 kHzで処理可能、可視化専用7種類が無警告で除外される。

コミット: `feat: update EffeTune dependency to 2.10.0`

### 2. Spatial Mapperの利用条件を確定する

776 floatのJS比較を確認し、行列末尾までの値、負係数、既定値、短い配列のケースを必要に応じて追加する。新goldenとの比較、Transparent、特定成分の別chへの出力、`ic`と処理幅、All / ペア / 単一ch、異なるバス、未使用chの遅延通過をPCMで検証する。

既存の実装で成立しない契約があれば全体REDを再現し、PipeTune側の最小限の統合修正を行う。DSPアルゴリズムを独自実装しない。CLIから読み込める完全な行列を持つ検証用プリセットを用意し、6chおよび12chの出力順と接続条件を観測・記録する。新しいUIは追加しない。

完了条件: 1/2/6/12/16chの代表ケース、5種類の解析バンド設定、代表レートで音声・経路・遅延が期待どおり。上流goldenの許容誤差`3e-5`以内で比較でき、ステレオ処理と多チャンネルアップミックスの設定条件を説明できる。物理出力の未検証条件は明示する。

コミット: `feat: validate Spatial Mapper preset support`。独立した不具合修正が必要なら、その修正を`fix:`で分ける。

### 3. TV Audio Simulatorの実行条件を確定する

公式goldenを共有ライブラリ経由で処理し、Scalarと実行可能なSIMDを比較する。刺激がnoise / impulse / sweepのいずれかをケースメタデータから確認して正しく生成する。8規格とモードの代表例、対応8レート、単一ch・別ペア・余剰ch、dry経路の遅延、非対応レートの失敗を検証する。

無音入力での発音と無音時停止、reset後の再開、プリセット再読込・レート変更・バックエンド切替の全体テストを追加する。製品修正が必要なら再現REDを確認してから行う。失敗した試行を積み重ねず、計画の前提と原因を見直して不要な修正を戻す。

完了条件: パラメータ比較と音声比較が成功し、無音時ポリシーと非対応レート時の失敗が既存契約どおり観測できる。All指定で全chを加工するなどの誤った期待を利用者文書に残さない。

コミット: `feat: validate TV Audio Simulator preset support`。独立した修正は`fix:`で分ける。

### 4. 配布検証と利用者向け説明

英日README、`pipetune/README.md`、`pipetune/docs/dsp-backends.md`、必要な公開APIコメントを更新する。内容は対応DSP、解析7種類の無視、Spatial Mapperの処理幅と遅延・出力配置、TV Audio Simulatorのレート・チャンネル・無音時停止に絞る。既存の2.8.0 / 2.9.0対応計画は履歴として維持する。

クリーンRelease、Debug/Release全体テスト、コンポーネント単独ビルド、一時DESTDIRへのインストール、13構成のパッケージ生成・インストール・起動確認を行う。GTK既存E2Eも全体実行に含める。今回アニメーションUIは追加しない。

検証したコミット、コマンド、構成、実行可否、ログ、成果物を`artifacts/verification/effetune-2.10.0/`へまとめ、本書へ最終条件との照合を追記する。リリース番号を決定する場合は既存の`screw-up`経路を使用し、本計画だけでPipeTuneのバージョンを変更しない。

完了条件: 下記の最終条件を満たし、検証できた範囲と未検証条件を記録する。

コミット: `doc: describe EffeTune 2.10.0 support`

## 検証と実施上の注意

- `make test`はDebugの全体実行。Releaseも`BUILD_TESTING=ON`で構成し、ビルド後に`ctest --test-dir ... --output-on-failure`を全件実行する。`-R`等による個別実行は行わない。
- 上流CMakeで追加される3種類のnativeテストも全体スイートに含める。上流カーネルの直接テストだけで、PipeTune独自共有ライブラリ・プリセット経路の検証を代用しない。
- 新規DSPに加え、既存のパラメータ比較、8種類のartifact音声golden、Crosstalk測定ストアと設計比較、FIR Crossoverの2ch通過と多ch分割、解析除外、ライブ再構築を回帰確認する。
- 全レート×全ch×全パラメータの直積を無制限に作らない。TVの対応レートは全8種、Spatial MapperはPipeTuneの固定5レートを含め、幅・モード・バスの境界を代表ケースで分担する。ブロック境界をまたぐ長さと端数ブロックを含める。
- Spatial Mapperの約53 msの遅延以上の入力・末尾処理を確保して比較する。無音区間だけのgolden一致を成功としない。TVの受信状態の整定も処理フレーム数で管理する。
- 13構成はDebian bookwormのx86_64/i686/arm64/armv7l、Debian trixieの同4種とriscv64、Ubuntu 24.04/26.04のx86_64/arm64。パッケージ起動検証と全体DSPテストは別の検証として記録する。
- ホストで実行できないISAはビルド成功と実行済みを区別する。FMA差、32bit、ARM、RISC-V、Note Spectrogramモデルの既存移植対応を確認する。
- 不足するパッケージはインストールを案内する。上流モデル生成のPython依存は既存のまま扱い、独自スクリプトはNode.js、追加のビルド時テキスト処理は`funcity`を使う。
- 外部APIを新規利用・変更する場合は公式文書とAPIコメントの両方を確認する。今回、新しい内部API依存を加えず、既存のC ABI統合を使う。
- タイムアウトは実測とエミュレーションのコストを踏まえて設定する。固定sleepで音声の状態を推測しない。高負荷構成が実時間に収まらない場合は計測結果を記録し、テスト成功とリアルタイム性能を混同しない。
- 調査時点では2.10.0をビルドしていない。新規カーネルの移植性や性能、全バックエンドでのgolden一致は未確認である。障害が見つかった場合は原因・対応方針を本書に反映してから修正し、上流ソースを直接変更しない。

## 最終完了条件

- [ ] 指定の公式v2.10.0を参照し、外部ソースを変更していない。
- [ ] CLI/GTKがEffeTune 2.10.0を表示し、各バックエンドが103カーネルの契約を満たす。
- [ ] Spatial Mapperのパラメータ、音声、ルーティング、遅延が検証され、実行条件が説明されている。
- [ ] TV Audio Simulatorのgolden、対応レート、チャンネル、無音時停止と復帰が検証されている。
- [ ] Pitch Meterを含む解析7種類が実行ノード・遅延・上限に含まれず、無警告で無視される。
- [ ] Spectrogram / Spectrum Analyzerの新パラメータ契約がバックエンドと一致する。
- [ ] プリセット再読込、レート変更、Scalar/SIMD切替と既存DSPの全体回帰に成功する。
- [ ] クリーンビルド、Debug/Release全体テスト、単独ビルド、インストール、GTK E2Eが成功する。
- [ ] 13構成のパッケージ検証結果と実行可能ISAでの音声比較結果を区別して記録する。
- [ ] 利用者文書が対応条件・制限を説明し、本書に実行記録と条件照合が追記されている。

## 参照

### 公式情報・契約

- [2.10.0リリース](https://github.com/Frieve-A/effetune/releases/tag/v2.10.0)
- [2.9.0から2.10.0の差分](https://github.com/Frieve-A/effetune/compare/v2.9.0...v2.10.0)
- [Spatial Mapperの公式契約・配置条件](https://github.com/Frieve-A/effetune/blob/v2.10.0/docs/dsp/effects/spatial-mapper/index.md)
- [Spatial Mapperのカーネル](https://github.com/Frieve-A/effetune/blob/v2.10.0/dsp/plugins/spatial/spatial_mapper/kernel.cpp)、[パラメータ](https://github.com/Frieve-A/effetune/blob/v2.10.0/dsp/plugins/spatial/spatial_mapper/params.json)、[保存配列・システムプリセット](https://github.com/Frieve-A/effetune/blob/v2.10.0/plugins/spatial/spatial_mapper.js)
- [Spatial Mapper goldenの出自](https://github.com/Frieve-A/effetune/blob/v2.10.0/dsp/plugins/spatial/spatial_mapper/golden/case-001.json)
- [TV Audio Simulatorの公式契約](https://github.com/Frieve-A/effetune/blob/v2.10.0/docs/dsp/effects/tv-audio-simulator/index.md)
- [TV Audio Simulatorの対応レート・遅延・ch処理とコメント](https://github.com/Frieve-A/effetune/blob/v2.10.0/dsp/plugins/lofi/tv_audio_simulator/kernel.cpp)、[パラメータ](https://github.com/Frieve-A/effetune/blob/v2.10.0/dsp/plugins/lofi/tv_audio_simulator/params.json)
- [Pitch Meterの公式契約](https://github.com/Frieve-A/effetune/blob/v2.10.0/docs/dsp/effects/pitch-meter/index.md)、[カーネル](https://github.com/Frieve-A/effetune/blob/v2.10.0/dsp/plugins/analyzer/pitch_meter/kernel.cpp)
- [Spectrogramの契約](https://github.com/Frieve-A/effetune/blob/v2.10.0/docs/dsp/effects/spectrogram/index.md)、[Spectrum Analyzerの契約](https://github.com/Frieve-A/effetune/blob/v2.10.0/docs/dsp/effects/spectrum-analyzer/index.md)
- [上流CMakeの浮動小数点指定とテスト](https://github.com/Frieve-A/effetune/blob/v2.10.0/dsp/CMakeLists.txt)
- [DSP公式説明](https://github.com/Frieve-A/effetune/blob/v2.10.0/dsp/README.md)、[公開C ABI・APIコメント](https://github.com/Frieve-A/effetune/blob/v2.10.0/dsp/include/effetune/abi.h)
- [エンジンのprepare失敗・ルーティング・遅延処理](https://github.com/Frieve-A/effetune/blob/v2.10.0/dsp/core/engine.cpp)

### PipeTuneの主な確認・変更対象

- `pipetune/tools/generate-dsp-catalog.mjs`、`pipetune/src/dsp_catalog.cpp`、`pipetune/src/dsp_pipeline.cpp`
- `pipetune/cmake/EffeTuneNativeBackends.cmake`、`pipetune/CMakeLists.txt`
- `pipetune/test/preset_pipeline_test.cpp`、`pipetune/test/parameter-packer-parity.mjs`、`pipetune/test/effetune_backend_artifact_test.cpp`
- `pipetune/src/dsp_pipeline_slot.cpp`と対応テスト、`pipetune/src/pipewire_pipeline.cpp`のチャンネル配置と遅延通知
- 既存の再読込・レート変更・バックエンド切替テスト、英日README、`pipetune/README.md`、`pipetune/docs/dsp-backends.md`

## 計画作成の完了確認

- [x] 現行PipeTuneの実装を基に、上流追加3種類と既存変更2種類の扱いを整理した。
- [x] 音声処理、パラメータ、ルーティング、遅延、無音時停止、ビルド条件、配布を検討した。
- [x] 段階ごとの成果物、検証、コミットと完了条件を定義した。
- [x] 未実施の実行検証と、調査済みの事実を区別した。

製品実装の完了条件は未達成。本書のみの変更であるため、計画作成時のビルド・テストは実行しない。

## 実行記録

- 基準の2.9.0: `make test`で148/148成功（611.17秒）。ログは`artifacts/verification/effetune-2.10.0/baseline.log`。以降もテストを選別せず、全体をCTestの並列実行で検証する。
- 段階1 RED: 全148件中3件が期待どおり失敗（120.14秒）。103カーネルの登録とPitch Meterの無警告除外が旧版では成立せず、単独Releaseバックエンドでも同じカタログ不足を確認した。ログは`step1-red.log`。
- 段階1 GREEN: 151/151成功（124.87秒）。新規3種類の上流nativeテストも実行。CLI/GTKのEffeTune表示2.10.0、新規加工DSPの有限・非ゼロ音声と遅延、解析7種類の除外、全バックエンドの103カーネル契約を確認した。ログは`step1-green-build.log`、`step1-green.log`。段階1の完了条件を満たした。
