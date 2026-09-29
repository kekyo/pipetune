# EffeTune 2.9.0対応計画

## 目的と対象

EffeTuneの参照をv2.8.0 (`ed3d9666067a166d386f01630ed8b68185fcbaf4`) から公式v2.9.0 (`71a48971165e8608740f49075bb95ef403534d8f`) へ更新し、PipeTuneで音声処理に必要な変更を取り込む。

2026-09-29に現在のPipeTuneコードと両タグ間の差分を調査した。計画作成時のPipeTune HEADは`357b80d`。以下の設計・手順は実装前に作成した計画であり、実施結果と完了条件の照合は末尾の実行記録に記載する。

ユーザー指定により、Note Spectrogramを含め、音声出力を加工しない可視化専用DSPは無視する。既存の解析DSPも対象とし、解析結果の表示や配信は追加しない。

## 調査結果と対応方針

| 対象 | 2.8.0からの変更・現在の状態 | PipeTuneの対応 |
| --- | --- | --- |
| Note Spectrogram | 新規のネイティブDSP。音声は通過し、推定音高・信頼度・音量をテレメトリへ出力 | バックエンドには取り込み、プリセットの実行時には無視する |
| Level Meter、Oscilloscope、Spectrogram、Spectrum Analyzer、Stereo Meter | 既存の可視化専用DSP。現在はPipeTuneでもインスタンスを生成して処理している | Note Spectrogramと同様に実行対象から外す |
| FIR Crossover | ネイティブカーネルに2ch時の無加工通過を追加。JS側のFIR設計もChルーティング後のチャンネル数を使用するよう変更 | 2ch通過と実際のチャンネル選択へ追従する |
| その他のネイティブDSP | 既存カーネルの実装差分はFIR Crossoverのみ。既存params.json、公開C ABI、core、vendorに差分なし | 参照更新と既存全体テストで回帰を確認する |
| プラグインのJS/CSS | テーマ、コントロール同期、解析グラフの時間軸表示などを変更 | 音声パラメータやプリセットの解釈に影響する部分を確認する。ブラウザUIの移植は対象外 |
| 既存goldenデータ | 多数のJSONメタデータに参照ハッシュの更新。既存の音声バイナリには差分なし | JSON更新件数をDSP実装の変更件数として扱わず、従来の音声比較を継続する |

Room EQ、IR Reverbの新規対応、測定UI、テーマ、ブラウザ拡張、Graph APIの公開は今回の範囲外。Crosstalk Cancellationの設計・測定ストアには今回の差分がなく、既存対応の回帰確認を行う。

## 設計

### 可視化専用DSPの扱い

対象は次の6種類とする。

- `LevelMeterPlugin` / Level Meter
- `NoteSpectrogramPlugin` / Note Spectrogram
- `OscilloscopePlugin` / Oscilloscope
- `SpectrogramPlugin` / Spectrogram
- `SpectrumAnalyzerPlugin` / Spectrum Analyzer
- `StereoMeterPlugin` / Stereo Meter

上流カーネルの音声バッファへの書き込みの有無を確認した分類を、PipeTune側のカタログ情報として持たせる。`generate-dsp-catalog.mjs`と`DspDefinition`に必要最小限の属性を追加する。表示名の部分一致や「テレメトリがある」という条件では判定しない。Compressor、Auto Leveler、Transient Shaper、Multiband Transient、Power Amp Sagなど、表示機能も音声加工もあるDSPは引き続き処理する。

`DspPipeline::buildFromRecipe()`で、対象ノードはパラメータのパック・アセット準備・インスタンス生成・96ノード制限の計数より前に除外する。意図した無視なので警告は出さず、有効DSP数と追加遅延にも含めない。プリセット全体のJSON構造チェックは維持する。未知のDSPや必要アセットが欠落した音声加工DSPは従来どおり警告する。

可視化専用ノードを取り除いたプリセットと同じ音声処理を行う。これらのノードに指定された入出力バス間のコピーも行わない。「音声バッファを書き換えない解析ノード」であっても、ルーティングまで含めるとバスのコピーが生じ得るため、比較対象は元のEffeTuneチェーンではなく対象ノードを除いたチェーンとする。

可視化専用DSPだけのプリセットは読み込み成功・有効DSP数0・追加遅延0・入力音声の無加工通過とする。Sectionの境界や、後続の音声加工DSPの順序・チャンネル指定には影響させない。プリセット再読込、動作レート変更、Scalar/SIMD切替で再構築しても同じ扱いにする。

現在のPipeTuneは`enginePrepare(..., telemetry_ring_bytes=0)`を使っているが、テレメトリを無効にしても解析カーネルの処理そのものは停止しない。今回の除外はインスタンスを作らない段階で行い、解析のCPU負荷とインスタンス用メモリ確保を避ける。

### ネイティブバックエンドとNote Spectrogramのモデル

実行時の除外と、共有ライブラリの上流カタログの検証を分ける。現在のバックエンドは上流registryと全カーネルのパラメータハッシュ・アセット容量を照合しているため、この構造を使い、各バックエンドに100種類を登録する。可視化専用6種類は登録されていてもPipeTuneのプリセット処理からは呼び出さない。100種類すべてが利用者向けに実行可能という表記はしない。

上流2.9.0の`models.cmake`は`effetune_note_models`という静的ターゲットを追加する。3個のモデルは合計5,779,072 bytes（約5.51 MiB）で、Python 3.10以上により検証・埋め込み用ヘッダーとASMを生成する。ユーザーが準備するIR等の外部アセットではなく、ビルドに同梱する定数データである。

`pipetune/cmake/EffeTuneNativeBackends.cmake`は独自に全`kernel.cpp`を収集するため、参照更新だけでは新カーネルの生成ヘッダーとモデルシンボルを解決できない。全共有バックエンドから上流の`effetune_note_models`をリンクし、生成順序とincludeディレクトリを引き継ぐ。上流の静的coreを丸ごとリンクしてScalar/SIMDの実装を混在させない。

上流の必須生成器としてPythonを利用し、独自のPythonスクリプトは追加しない。`prereq.sh`の前提パッケージに`python3`を明示し、ホストも3.10以上か確認する。不足時はインストールを案内する。既存の前提イメージは自動的に再生成されるとは限らないため、必要な構成を`prereq.sh --force`で更新してからパッケージ検証する。実行時Python依存は追加しない。PipeTuneで新たに必要なスクリプトはNode.jsを使用し、ビルド時テキスト処理が必要になった場合は`funcity`を使用する。

共有ライブラリごとにモデルを含むため、配置サイズの増加を記録する。外部モデルの実行時配布や別共有ライブラリへの分離は導入しない。既存のABIエンジンアクセスパッチは上流coreに差分がないため引き続き適用可能と見込むが、ゼロfuzz適用と共有ライブラリのロードで確認する。

### FIR Crossoverの2ch通過とルーティング

現状の`buildFromRecipe()`はFIR Crossoverに限り`processingChannels = options.maxChannels`、`channelSpec = -2`（All）へ上書きする。`designFirCrossover()`は4〜16の偶数チャンネル以外を警告付きで除外する。このままでは2.9.0の挙動にならない。

- プリセットのチャンネル指定を尊重し、既存の`selectedProcessingChannels()`で得たルーティング後の幅を用いる。指定なしは既存規則どおり先頭のステレオペアとして扱う。
- 処理対象が2chなら、FIR Crossoverのネイティブインスタンスをアセットなしで構築し、入力をそのまま通す。FIR設計・アップロードは行わず、警告と追加遅延は0とする。FIR Crossoverは音声加工DSPであり、可視化専用の除外分類には入れない。
- 処理対象が4〜16の偶数chなら、従来のFIR設計を使う。バンド数は要求値、利用可能なステレオペア数、上流の最大4バンドの範囲に収める。
- 1ch・奇数chなど、それ以外の処理幅は従来の警告付き除外方針を適用する。
- 16chのパイプラインでもステレオペア指定なら通過し、All指定なら分割する。異なる入出力バスの場合は、通常のパイプラインのバス転送規則を適用する。

生成アセットの成功・通過・除外を区別し、2ch通過時に空のアセット情報で`lt`・`fd`・`bc`を誤って上書きしないようにする。複雑な共通化は避け、既存の生成FIR経路に必要な分岐だけを追加する。再構築時は現時点の幅で判断し、マルチチャンネル時のFIR遅延を2ch時へ残さない。

## インクリメンタルな実装手順

各段階はビルド可能・CLIから観測可能な成果物で区切る。製品コード変更前に期待する挙動のテストを追加して全体REDを確認し、実装後に全体GREENを確認する。テストは個別実行せず、常にその構成の全スイートを実行する。ドキュメントだけの変更ではビルドしない。

### 1. 可視化専用DSPを実行対象から除外する

2.8.0のまま基準となる全体テストを実行する。既存5種類のカタログ分類とプリセット除外を実装し、音声加工DSPとの混在、解析のみ、Section、未知DSPの警告を確認する。

機能削除の手順として、旧解析DSPが実行対象から消えたことを直接確認する一時テストを先に追加して全体REDを確認する。除外後の全体GREENを確認して`refactor: omit visualization-only DSP execution`で一旦コミットする。その後、一時的な削除確認テストを削除し、全体GREENを確認して`chore: remove temporary analyzer removal tests`でコミットする。恒久的なテストは、プリセット処理の結果・有効DSP数・遅延・警告という利用者に見える契約を検証するものに限る。

完了条件: 既存5種類が有効DSP数と遅延に含まれず、対象を取り除いたプリセットと同じ音声が得られる。Volume等の加工は正しく適用され、意図した除外で警告を出さない。

### 2. 2.9.0とモデルを全バックエンドへ取り込む

100カーネル契約とNote Spectrogramを含むプリセットの機能テストを追加し、2.8.0で全体REDを確認する。サブモジュール参照を指定コミットへ更新し、モデルのリンク、ビルド前提、カタログとNote Spectrogramの除外を揃える。CLI/GTKのバージョン表示とバックエンドロードを確認する。

この段階で13構成のクリーンビルド・ロードを実施し、ASMと32bitを含むモデル組み込みの実現性を確定する。上流生成器のELF構文（`@progbits`、`@object`）はarmhf等のアセンブラ差異を重点確認する。現時点では失敗を再現していないため、互換性問題は未確認リスクとして扱う。失敗した場合は原因と対処を本計画へ反映し、上流ソースを直接変更しない。生成物への最小限の調整が必要なら、既存の生成ABIソース方式と同様にビルド領域で行う。

完了条件: CLI/GTKがEffeTune DSP 2.9.0を表示し、全バックエンドが100種類を登録・検証できる。Note Spectrogramを含む6種類はプリセットから無警告で除外される。各構成が生成ヘッダーとモデルを解決でき、モデルファイルやPythonの実行時配置を要求しない。

コミット: `feat: update EffeTune dependency to 2.9.0`

### 3. FIR Crossoverの処理幅と2ch通過を合わせる

2ch無加工通過、Allとステレオペアの違い、バス転送、遅延のテストを追加して全体REDを確認する。チャンネルの強制上書きを解消し、処理幅に応じたアセット準備を実装する。既存の4〜16chテストも維持し、周波数分割と余剰出力チャンネルの扱いを確認する。

完了条件: 2chは無警告・無加工・遅延0、4〜16の偶数chは指定したルーティングでバンド分割、それ以外は警告付き除外となる。プリセット・レート・バックエンドの再構築後も正しい処理と遅延になる。

コミット: `fix: follow EffeTune FIR Crossover channel routing`

### 4. 配布検証と利用者向け説明

README英日へ可視化専用6種類の無視とFIR Crossoverのチャンネル条件を記述する。`pipetune/docs/dsp-backends.md`のバージョン・登録数・モデル組み込みの前提も更新する。パイプライン読み込みの公開APIコメントにも無警告で無視する条件を追記する。既存の2.8.0対応の履歴は書き換えない。

クリーンRelease、Debug/Releaseの全体テスト、コンポーネント単独ビルド、一時DESTDIRへのインストール、最終版の13構成のパッケージ生成・インストール・起動確認を行う。段階2から製品ソースが変更されるため、最終版のマトリクスを実施する。ログと成果物を`artifacts/verification/effetune-2.9.0/`等の既存規則に沿って保存し、本書へ実行記録と完了条件の照合を追記する。

完了条件: 下記の全完了条件を満たし、検証したコミット・構成・実行可否と結果を記録する。

コミット: `doc: describe EffeTune 2.9.0 support`

## 検証の詳細

- 可視化専用DSPの単独・6種類混在・音声加工DSPとの混在。対象を除いたプリセットと実際の出力PCMを比較する。有効DSP数、警告、遅延も確認する。
- 対象ノードが96個を超えても音声加工ノード用の上限を消費しない。解析パラメータや表示設定は実行準備に使わず、正しい構造のプリセットを拒否する原因にしない。
- Sectionの有効/無効、プリセット再読込、レート変更、バックエンド切替後の再構築。未知DSPとRoom EQ/IR Reverb等の警告は継続する。
- 可視化専用ノードに別バスが設定されていても無視される。加工も行うDSPはテレメトリの有無にかかわらず実行される。
- FIR Crossoverは2/4/6/8/16ch、All・先頭ペア・別ペア・単一ch、異なる入出力バス、複数バンド設定を網羅する。インパルスと帯域別信号で通過・帯域分割・経路・遅延を検証する。無効幅も確認する。
- 共有ライブラリ経由の2ch FIR Crossover処理を上流2.9.0の契約と比較する。既存のパラメータパック対JS比較、音声golden比較、Crosstalk測定JSONとJS設計比較を全体実行に含める。
- Scalarと実行可能なSIMDで確認する。非対応ISAは実行不可とビルド成功を区別する。上流Note Spectrogramテストは上流全体テストの一部として実行するが、PipeTuneの表示機能の検証とは扱わない。
- 13構成はDebian bookwormのx86_64/i686/arm64/armv7l、Debian trixieの同4種とriscv64、Ubuntu 24.04/26.04のx86_64/arm64。モデルのアラインメント・PIC・シンボル可視性・リンクを確認する。
- 上流生成モデルはlittle-endianとIEEE 754のfloat/doubleを要求する。既存マトリクスの対象で検証し、未検証アーキテクチャへの対応は主張しない。
- GTKの既存E2Eを全体実行する。新規の可視化UIやアニメーションは追加しない。時間のかかる上流解析テストやエミュレーションは実測に応じてスイート予算を確保し、固定sleepで成功を推測しない。
- ソース文字列の一致やカタログ件数だけで対応完了とせず、ロードしたライブラリとプリセットを実際に動かす。

## 最終完了条件

- [x] 指定の公式v2.9.0を参照し、`deps/`配下の外部ソースを変更していない。
- [x] CLI/GTKが2.9.0を表示し、Scalarと全SIMDバックエンドに100種類が登録される。
- [x] 可視化専用6種類が実行ノード・遅延・ノード制限に含まれず、無警告で無視される。
- [x] 音声を加工するDSPは表示機能の有無にかかわらず正しく処理される。
- [x] FIR Crossoverが処理対象の幅に応じて2ch通過・偶数4〜16ch分割・その他の警告付き除外を行う。
- [x] プリセット再読込・レート変更・バックエンド切替と既存Crosstalk連携が正常に動く。
- [x] クリーンReleaseビルド、Debug/Releaseの全体テスト、単独ビルド、インストール検証に成功する。
- [x] 最終版の13構成でパッケージ生成・インストール・起動確認に成功し、ホスト全体テストの結果と区別して記録する。
- [x] 英日READMEとバックエンド文書が実際の対応範囲を説明し、本計画へ結果を記録する。

## 参照

上流のAPI利用時は、以下の公式文書とタグ固定のヘッダー/APIコメントの両方を確認する。

- [2.9.0リリース](https://github.com/Frieve-A/effetune/releases/tag/v2.9.0)
- [2.8.0から2.9.0への差分](https://github.com/Frieve-A/effetune/compare/v2.8.0...v2.9.0)
- [Note Spectrogramの公式契約](https://github.com/Frieve-A/effetune/blob/v2.9.0/docs/dsp/effects/note-spectrogram/index.md)
- [解析DSPの利用者向け説明](https://github.com/Frieve-A/effetune/blob/v2.9.0/docs/plugins/analyzer.md)
- [Note Spectrogramカーネル](https://github.com/Frieve-A/effetune/blob/v2.9.0/dsp/plugins/analyzer/note_spectrogram/kernel.cpp)
- [モデルのCMake定義](https://github.com/Frieve-A/effetune/blob/v2.9.0/dsp/plugins/analyzer/note_spectrogram/models.cmake)
- [モデルの検証・埋め込み生成器](https://github.com/Frieve-A/effetune/blob/v2.9.0/dsp/plugins/analyzer/note_spectrogram/embed_models.py)
- [FIR Crossoverの公式説明](https://github.com/Frieve-A/effetune/blob/v2.9.0/docs/plugins/basics.md#fir-crossover)
- [FIR Crossoverカーネル](https://github.com/Frieve-A/effetune/blob/v2.9.0/dsp/plugins/basics/fir_crossover/kernel.cpp)
- [FIR Crossoverのルーティング後の設計](https://github.com/Frieve-A/effetune/blob/v2.9.0/plugins/basics/fir_crossover.js)
- [公開C ABIとコメント](https://github.com/Frieve-A/effetune/blob/v2.9.0/dsp/include/effetune/abi.h)
- [処理モデルの公式説明](https://github.com/Frieve-A/effetune/blob/v2.9.0/docs/dsp/concepts/processing-model/index.md)

PipeTune側の主な変更対象:

- `pipetune/tools/generate-dsp-catalog.mjs`、`pipetune/src/dsp_catalog.h`
- `pipetune/src/dsp_pipeline.cpp`、`pipetune/src/generated_fir_asset.cpp`
- `pipetune/cmake/EffeTuneNativeBackends.cmake`、`pipetune/CMakeLists.txt`
- `pipetune/test/preset_pipeline_test.cpp`、`pipetune/test/effetune_backend_artifact_test.cpp`と既存の再構築・パッケージ検証
- `prereq.sh`、`README.md`、`README_ja.md`、`pipetune/docs/dsp-backends.md`

## 実行記録

2026-09-29、実装開始。

- 2.8.0の基準Debug全体テスト: 145/145成功、233.27秒。
- 手順1の全体RED: 可視化専用DSPが有効ノードに残ることを検出した。同じ実行の単独ビルド検証で、ソース変更後にCMakeを再構成しなかったため、screw-upの解決バージョンと実行ファイルの表示が一致しなかった。以後、変更後の検証前にCMakeを再構成する。製品のバージョン処理は変更しない。
- ホストPythonは3.12.3。既存13個の前提イメージも、各コンテナ内でPython 3.11.2〜3.14.4を実行できることを確認した。今回の依存追加のための再生成は不要だが、新規イメージでも導入されるよう`prereq.sh`へ明示する。
- 検証ログは`artifacts/verification/effetune-2.9.0/`に保存する。
- 手順1: 全体GREEN 145/145成功（113.32秒）。`730f126`でコミット。一時的な削除確認テストを除いた全体検証も145/145成功（113.25秒）、`0a1aa23`でコミットした。
- 手順2の全体RED: 100種類の登録不足とNote Spectrogramの警告を検出した。単独ビルド検証も同じ新しい登録契約で失敗した。
- 2.9.0へ更新したホスト全体テスト: 148/148成功（178.63秒）。
- 手順2のマトリクスでbookworm/trixieのarmhfにASM構文エラーを再現した。原因は上流生成器がARMでも`@progbits`/`@object`を出力すること。手順2を具体化し、上流生成物をfuncityで`%progbits`/`%object`に変換したビルド領域のコピーをコンパイルする。上流のモデル検証、元の生成物、モデルバイナリは変更しない。変換には固定した`funcity-cli@1.5.0`と`funcity@1.5.0`をnpm経由で使用し、前提にnpmを追加する。既存armhfイメージにはnpmがないため、この2構成の前提イメージを再生成する。
- armhf用変換のCMake定義後もホスト全体148/148成功（168.23秒）。funcity CLIの推移依存がNode 18のbookwormで動くよう、CLIの直接依存`commander`も12.1.0へ固定した。bookworm/trixieのarmhf前提イメージを再生成し、3モデルとも変換後ASMのコンパイルを通過した。
- x86_64/i686の6構成でパッケージ生成・インストール・CLI/GTKのバージョン確認が成功した。ベンチマークドライバーから各共有ライブラリをロードし、Note Spectrogram + Volumeのプリセットを処理して、有効1ノード・警告0・有限の出力を確認した。実行可能なScalar/baseline/x86-64-v3を実行し、x86-64-v4はホストCPU非対応のためビルド確認と区別する。
- Debian bookworm amd64の共有ライブラリ配置サイズはScalar 7,903,752 bytes、baseline 7,907,872 bytes、v3 7,879,192 bytes、v4 7,977,496 bytes。各ライブラリに新モデル5,779,072 bytesと解析カーネルが加わる。プリセットで可視化専用DSPを除外するため、モデル解析は実行されない。
- 初回13並列では各ビルドのコンパイラ並列数が1となり、エミュレーション構成の進行が遅かった。完了済みのネイティブ6構成を保存し、未完了7構成を各3〜4コンパイラでクリーン実行し直した。配布スクリプトのスケジューラー自体は変更しない。
- 手順2の最終ホスト全体確認: 148/148成功（229.02秒）。
- エミュレーションの待機中に手順3のテストだけを準備した。配布ビルドは`BUILD_TESTING=OFF`で、製品コードは変更していない。手順3の全体REDでは、2ch FIRで警告が出ることを検出して1件失敗し、残り147件は成功した（227.82秒）。
- 手順2の13構成すべてでクリーンに開始したビルドとバックエンドのロード・音声処理に成功した。arm64はScalar/NEON/SVE、armhfはScalar/NEON、riscv64はScalar/RVVをエミュレーションで実行した。各結果は`step2-load/*.json`。遅いarm64の2構成はコンパイル完了前に並列数を8へ増やしてビルド領域から再開したため、一括実行の中断結果と再開結果を分けて残した。手順2の条件である全構成のビルド・ロードと、ホスト148テストの成功を確認した。パッケージ生成・インストールは最後の2構成の依存関係検査を継続し、最終版でも再検証する。
- 手順2の更新は`688b92f`でコミットした。その後、残っていた2構成の依存関係検査とインストール検証も成功し、既存の`validate_deb_artifacts`で13パッケージ全体を検証した。2.8.7と表示されるPipeTuneパッケージのEffeTune DSP表示は2.9.0。段階2の成果物は`step2-deb/`へ保存した。
- 手順3: 全体GREEN 148/148成功（118.10秒）。2chの無加工・遅延0・バス加算、全幅/先頭ペア/別ペア/単一ch/奇数ch、4/6/8/16chの帯域分割・余剰出力の無音、48/96kHzとScalar/利用可能SIMDへの再構築、線形位相のインパルス再構成を確認した。2chでは空のアセット情報で`lt`/`fd`/`bc`を上書きせず、ネイティブのルーティングノードを保持する。手順3の完了条件を満たした。

- 手順3の実装コミットは`119a5ef`。手順4ではこの製品コードに英日README、バックエンド文書、公開APIコメントを加えた状態で検証した。解決されたPipeTune配布バージョンは2.8.9、EffeTune DSPは2.9.0。
- 最終版の配布ビルドは手順2の各ビルド領域を再利用し、変更ソースを再コンパイルした。ステージング領域は作り直し、既存配布処理で依存関係算出・パッケージ生成・インストール・CLI/GTKのバージョン確認を行う。別途、ホストReleaseは新しい`build/effetune-2.9-release/`でクリーンビルドした。
- Debugは148件、Releaseは147件の全体スイートを使用する。差の1件は上流`dsp/CMakeLists.txt`がDebugでのみ登録する`effetune_dsp_allocation_guard`で、意図的なテストの除外は行っていない。

### 最終検証結果と完了条件の照合

- 最終Debug全体: 148/148成功、215.25秒。ログ: artifacts/verification/effetune-2.9.0/final-debug.log。
- クリーンReleaseビルド成功、Release全体: 147/147成功、164.28秒。ログ: final-release-build.log、final-release.log。
- 両スイートにコンポーネント単独ビルド、GTK E2E、インストール配置、パラメータ/音声比較、Crosstalk測定・設計比較を含む。
- 最終13パッケージすべてで生成・インストール・CLI/GTKのバージョン表示に成功した。既存のvalidate_deb_artifactsによる13件全体の検証も成功した。
- 各インストール先の/usr/lib/pipetuneからバックエンドをロードし、Note Spectrogram + FIR Crossoverを2ch/4chで実行した。全26結果で有効1ノード・警告0・有限かつ非ゼロの出力を確認した。ログはfinal-load/*.json、final-*-install.log、final-packages.log、final-artifacts.log。
- 外部ソースは指定の公式タグを参照するサブモジュール更新だけで、deps/effetuneの作業ツリーに変更なし。
- 可視化専用6種の除外、音声加工DSPの継続実行、FIRのルーティング・遅延・帯域分割・再構築はプリセットの実出力を使うテストで成功した。未知DSPとRoom EQ/IR Reverbの警告も保持されている。
- 英日README、バックエンド文書、公開APIコメントを実装に合わせて更新した。以上により、最終完了条件9項目をすべて満たした。

| 配布構成 | 実行したバックエンド | パッケージ・インストール・音声処理 |
| --- | --- | --- |
| Debian bookworm x86_64 | Scalar / baseline / x86-64-v3 | 成功 |
| Debian bookworm i686 | Scalar / baseline / x86-64-v3 | 成功 |
| Debian bookworm arm64 | Scalar / NEON / SVE | 成功（エミュレーション） |
| Debian bookworm armv7l | Scalar / NEON | 成功（エミュレーション） |
| Debian trixie x86_64 | Scalar / baseline / x86-64-v3 | 成功 |
| Debian trixie i686 | Scalar / baseline / x86-64-v3 | 成功 |
| Debian trixie arm64 | Scalar / NEON / SVE | 成功（エミュレーション） |
| Debian trixie armv7l | Scalar / NEON | 成功（エミュレーション） |
| Debian trixie riscv64 | Scalar / RVV | 成功（エミュレーション） |
| Ubuntu 24.04 x86_64 | Scalar / baseline / x86-64-v3 | 成功 |
| Ubuntu 24.04 arm64 | Scalar / NEON / SVE | 成功（エミュレーション） |
| Ubuntu 26.04 x86_64 | Scalar / baseline / x86-64-v3 | 成功 |
| Ubuntu 26.04 arm64 | Scalar / NEON / SVE | 成功（エミュレーション） |

x86-64-v4は対象パッケージでビルド・配置を確認した。ホストCPUが対応しないため実行はしておらず、CPU検証による選択拒否と実行可能な各tierを区別した。

最終配布物はartifacts/deb/のPipeTune 2.8.9パッケージ13個。検証対象の製品コードは119a5ef（依存更新688b92fを含む）で、最後のコミットは文書と公開APIコメントの更新のみ。過去の2.8.0対応計画は変更していない。
