# EffeTune 2.13.0対応計画

## 目的と対象

PipeTuneが参照するEffeTuneを、公式アプリタグ`v2.12.0`（`1d4d33b2e631a840ae178295e3ea2b66b06b1e43`）から`v2.13.0`（`6791afdfc6f4e6c913b9f48c8189a44e29a09bde`）へ更新する。利用者の指定に従い、追従先はアプリタグとする。DSPライブラリの独立したリリースタグを更新基準にしない。

調査基準は2026-10-09時点のPipeTune HEAD `d4d13b4e0b17ccb9454b807b2172a3118db6f22c`。アプリ2.13.0に同梱されるDSPライブラリの版は0.13.0である。今回の両タグは同じコミットへ解決されるが、これは今回の一致であり、今後の追従方針を変えるものではない。[公式アプリリリース](https://github.com/Frieve-A/effetune/releases/tag/v2.13.0)、[アプリタグ間差分](https://github.com/Frieve-A/effetune/compare/v2.12.0...v2.13.0)

Adaptive Predictionの追加、Cassette ArtifactsとRhythm Analyzerの変更、ネイティブバックエンド、既存DSP、現在の複数出力機能までを検証対象とする。SFZ Note Playerは実現方法を比較中であり、警告付き除外の継続も音源読込の実装も、現時点では確定していない。後述の選択工程を確定してから、その工程の実装へ進む。

この作業段階の成果物は計画書である。製品コード、サブモジュールの参照、配布物は変更せず、実装後の検証結果と計画を区別する。

## 上流の変更と対応範囲

| 対象 | アプリ2.12.0からの変更 | PipeTuneでの対応 |
| --- | --- | --- |
| Adaptive Prediction | 学習による予測・残差出力・自己帰還音の生成を追加 | 通常の加工DSPとして実行し、処理幅を1〜2chに制限する |
| SFZ Note Player | 入力音から音符を推定し、外部SFZ音源で再生 | カタログとバックエンドへ取り込み、製品での音源読込方式は比較案から選ぶ |
| Cassette Artifacts | `md`による5種類の処理モードを追加 | 新しいパラメータ配置と各モードの音声を検証する |
| Rhythm Analyzer | Rd6系の解析へ移行、モデルとクリック検証を更新 | 拍検出後のクリック、無音、非対応レートの契約を検証する |
| Note Spectrogram | HarmNetへ移行し、既定の音域を変更 | カタログ・配布バックエンドへ反映し、製品では解析除外を維持する |
| モデル埋め込み | 外部binの埋め込み対象が8個からRhythm用3個へ変更 | 既存の共通モデルターゲットとARM用生成物変換を適用する |
| エンジン | 遅延更新API、音楽トランスポート、経路PCMの観測を追加 | 新しいコアをビルドし、既存の再構築・遅延補償を回帰検証する |
| 演算・メモリ | FIR・遅延線などを最適化し、処理中の解放もDebug監視対象に追加 | Scalarと実行可能SIMDでPCM、遅延、reset、処理負荷を確認する |
| アプリ機能 | VisualizerのGuitar、LANリモート操作などを追加 | 今回のPipeTuneへの移植対象には含めない |

登録カーネルは110から112になる。無警告で除外する解析専用DSPは既存の9種類を維持する。登録数は、音源なしで利用できるDSP数ではない。[registry](https://github.com/Frieve-A/effetune/blob/v2.13.0/dsp/registry.inc)

パラメータ編集UI、ライブオートメーション、音符・スペクトル表示、ホストの音楽トランスポート連携、Room EQ / IR Reverbの読込、旧DSPを選択する互換モード、PipeTune自身のリリース番号変更は対象外とする。SFZの読込を選んだ場合も、そのための音源指定・診断を超えて編集UIを追加しない。

## 現行コードに必要な統合変更

### カタログと共有バックエンド

`pipetune/tools/generate-dsp-catalog.mjs`の`nativeAssetCapacity`は現在、`32 MiB convolution cap`だけを受け付ける。SFZの`1 GiB bank and index cap`を認識し、slot 0の容量を1,073,741,824 bytesとして生成する必要がある。SFZを製品で除外する場合にも必要な変更であり、既存の畳み込みアセットの上限は32 MiBのままとする。未知の容量表記を推測で解釈する処理は追加しない。

カタログは引き続き上流のregistry・manifest・パラメータ仕様から生成する。新規・変更型の手書きパッカーは作らない。重要な契約は次のとおりである。

| 型 | float数 | パラメータハッシュ | 外部アセット |
| --- | ---: | --- | --- |
| `AdaptivePredictionEffectPlugin` | 10 | `0xebd8a6f0` | 不要 |
| `SFZNotePlayerPlugin` | 16 | `0x0f17627c` | slot 0、音源と索引で最大1 GiB |
| `CassetteArtifactsPlugin` | 13 | `0x328491ae` | 不要 |
| `NoteSpectrogramPlugin` | 2 | `0x9d70750b` | 不要 |

`pipetune/cmake/EffeTuneNativeBackends.cmake`の製品ソース一覧へ`core/spectrum_tap.cpp`を加え、上流ネイティブ構成に揃える。独自共有バックエンドの`-ffp-contract=off`対象には、Adaptive Prediction、SFZ Note Player、Note Spectrogramを追加する。上流のsourceプロパティはディレクトリ単位なので、上流CMakeだけの指定では独自共有バックエンドへ伝わらない。[上流CMake](https://github.com/Frieve-A/effetune/blob/v2.13.0/dsp/CMakeLists.txt)

`effetune_tree_models`にはRhythmの`rhythm_d_low`、`rhythm_d_mid`、`rhythm_d_high`が残る。新しいoblivious tree形式は上流の生成・検証器へ任せる。Note SpectrogramのHarmNet重みはヘッダーに移っており、binが減ったことを共有ライブラリ全体の縮小と同一視しない。モデルの再学習・再生成は行わない。[モデル構成](https://github.com/Frieve-A/effetune/blob/v2.13.0/dsp/plugins/analyzer/tree_models/models.cmake)、[HarmNet重み](https://github.com/Frieve-A/effetune/blob/v2.13.0/dsp/plugins/analyzer/note_spectrogram/harmnet_weights.h)

既存の`EffeTuneTreeModels.cmake`と`arm-tree-model.fc`は、列挙された全生成アセンブリへ適用する方式を維持する。ARM向け`@`から`%`への変換と、空のnpmキャッシュで変換処理が競合しない依存順序を、新しい3モデルで検証する。上流が必要とするPython 3.10以上は既存のビルド依存として扱う。PipeTuneが新たに書くスクリプトはNode.js、ビルド時テキスト処理は`funcity`を使う。

### ABIとライフサイクル

公開C ABIの版は1のままで、遅延のrefresh / reserve APIなどが追加されている。PipeTuneは設定変更時にパイプラインを作り直すため、今回これらのAPIをローダーの必須シンボルへ追加する必要はない。既存のエクスポート境界とアセットコピーABIを維持し、最終的にロードされる共有ライブラリで確認する。

`et_instance_set_analysis_source`はヘッダーで`App-internal`と明記されているため使わない。SFZを実行する場合はカーネル内の独立した音符解析を使い、Note Spectrogramを実行必須にしない。音楽トランスポートも今回供給しない。[C ABIとコメント](https://github.com/Frieve-A/effetune/blob/v2.13.0/dsp/include/effetune/abi.h)、[ProcessInfoとカーネル契約](https://github.com/Frieve-A/effetune/blob/v2.13.0/dsp/include/effetune/kernel.h)

既存の`abi-engine-access.patch`は対象タグの一時コピーへの`patch --dry-run --fuzz=0`で適用可能だった。実装時には通常のビルド生成物での適用とアセット転送を確認する。`deps/`内の外部ソース・vendorは編集しない。参照更新は指定の公式アプリコミットへの変更だけとする。

エンジンの遅延計画と処理情報の変更に対し、ペア内の入力整列、All、別バスへの送信・加算、最終ch間整列、バイパス、resetをインパルスとPCMで確認する。現行の複数出力へも、同じDSP遅延が正しく通知・配分されることを確認する。音声コールバックにファイル読込、モデル構築、新たなワーカーやポーリングを追加しない。

## Adaptive Prediction

`gap`、`learn`、`weightDecay`、`autonomy`、`original`、`residual`、`prediction`、`freeze`、`hold`、`resetToken`を上流どおりパックする。`gap`は予測に使う過去の距離で、報告音声遅延ではない。報告遅延は0である。左右の学習状態は独立している。[公式説明](https://github.com/Frieve-A/effetune/blob/v2.13.0/docs/dsp/effects/adaptive-prediction-effect/index.md)、[カーネル](https://github.com/Frieve-A/effetune/blob/v2.13.0/dsp/plugins/resonator/adaptive_prediction_effect/kernel.cpp)

カーネルは処理幅が2chを超えると音声を加工せずに戻る。現行ホストではそのまま有効DSPに数えられるため、`pipetune/src/dsp_pipeline.cpp`で、既存Bass Extenderと同様に処理幅を検証する。1ch・ステレオペアは許可し、3ch以上のAllは明示的な読込エラーとする。各ペアへの自動複製は行わない。無効ノード・無効Sectionは既存どおりoffとし、非実行ノードをこの制約で拒否しない。

検証は短い出力の有無だけで終わらせず、次を分担して行う。

- パッカー比較で全10フィールドの既定値、上下限、不正値、bool、符号付きミックス、`resetToken`を上流JSと照合する。
- 公式17 goldenを実共有バックエンドで比較する。許容絶対誤差`5e-4`を変更しない。既存ランナーが持つブロック途中のパラメータイベント処理を利用し、Hold、Freeze、減衰、リセット、制御値の平滑化を含める。
- 製品プリセットから、周期入力の学習による残差の減少、予測音の生成、左右の独立性、補完ミックス、有限出力、報告遅延0を確認する。1 / 2chと16chバス内の選択ペア・単一ch、未選択ch保持、端数ブロックを含める。
- 44.1 / 48 / 96 / 192 / 384 kHzと、Automaticで取り得る代表レートを分担する。上流goldenが含む8 / 32 / 50 / 352.8 kHzも元の条件で実行する。
- 学習後の無音、学習停止、有限のWeight Decay、reset後の再学習を確認する。学習前のHoldは音を生成しないため、既定でHoldをオンにしたプリセットの無音を不具合扱いしない。

上流では、十分に静かな外部入力の間は学習と減衰を停止する。Freezeは学習・減衰だけを止め、HoldはFreezeとAutonomy 1を組み合わせる。学習状態はプリセットに保存されない。PipeTuneの再読込・レート変更・バックエンド変更は再構築なので、EffeTuneアプリ内で設定だけを変更して学習を保持する操作とは区別する。

計画案では既存の無音時休止方針を適用する。休止オンではフェード後にresetし、学習状態と生成音をクリアする。休止オフでは処理を続ける。DSPごとの休止例外や学習状態の保存は追加しない。製品経路の学習済みパイプラインで両設定を検証し、利用者文書へ記載する。

履歴領域の確保量はサンプリング周波数に応じて増えるため、Releaseで48 / 96 / 384 kHzの初期化時間、RSS、定常処理負荷を測る。プリセット再構築時の旧・新パイプライン共存も測定に含める。[上流nativeテスト](https://github.com/Frieve-A/effetune/blob/v2.13.0/dsp/plugins/resonator/adaptive_prediction_effect/native_test.cpp)

## SFZ Note Playerの実現案

### 現行コードから使える部分

プリセットが持つ`parameters.sf`は24桁の音源IDで、SFZ本文や音声データではない。デスクトップ版はEffeTuneのユーザーデータ下の`sfz-references.json`に`id`、`name`、`root`、`path`を保存し、元の音源フォルダーを読む。ブラウザ版は別の音源ストレージを使う。[プリセットとの連携](https://github.com/Frieve-A/effetune/blob/v2.13.0/plugins/others/sfz_note_player.js)、[デスクトップ版の登録形式](https://github.com/Frieve-A/effetune/blob/v2.13.0/electron/sfz-library-ipc.js)、[利用者向け説明](https://github.com/Frieve-A/effetune/blob/v2.13.0/docs/plugins/others.md#sfz-note-player)

SFZの読込には、音源IDの解決、SFZとincludeの解析、サンプル音声のデコード、再生領域のテーブル化が必要になる。上流はJavaScript側にこれらの処理を分離し、C++カーネルには変換済みバンクを渡している。PipeTuneの既存`pipetune_effetune_instance_asset_copy_v1`は、そのバンクを渡す経路として再利用できる。現在のCrosstalk用測定データ読込は外部データを解決する先例だが、SFZの解析・デコード機能までは持たない。

### 方式の比較

| 案 | 利用者の操作と実装方法 | 長所 | 主な追加作業と制約 |
| --- | --- | --- | --- |
| A EffeTuneの登録音源を自動参照 | デスクトップ版で登録したIDを`sfz-references.json`から解決し、プリセット読込時に元フォルダーから変換する | 既存のEffeTuneプリセットを使う流れに合い、音源を二重登録しなくてよい | SFZ解析・音声デコード用の補助処理と、EffeTune登録形式への追従が必要。ブラウザ版のIDだけでは解決できない |
| B PipeTuneへ事前取込み | CLIなどでSFZフォルダーと対象IDを指定し、再生用バンクと対応表を保存する。再生時は保存データを読む | 変換処理を再生プロセスから分離でき、C++デーモンにNode.jsを常駐させる必要がない | 初回取込み、元音源更新時の再取込み、バンク版・容量・IDの管理が必要 |
| C C++で直接読込み | PipeTune側で音源パスを登録し、C++のSFZ解析・音声デコーダーから上流形式のバンクを構築する | Node.jsを実行時依存にせず、読込から再生までネイティブ側で扱える | 実装量が最も大きい。include、階層的な設定、ループ、音量・音域・ベロシティ等の解釈を上流へ追従させる必要がある |

Aの音源解決とBの事前変換は組み合わせられる。既存のEffeTune利用を優先するなら、登録情報を自動参照し、読込時に必要なものだけ変換・キャッシュする構成が候補になる。完全に独立した運用を優先するならBが適している。これらは今回追加する機能の案であり、EffeTuneにPipeTune向けのバンク書出し操作が既にあるという意味ではない。

A / Bでは、アプリタグに固定した上流`js/sfz/parser.js`と`asset.js`などを無改変で利用する補助ツールを検討する。ファイル読込と音声デコードのアダプターが必要で、上流プラグインが使う`OfflineAudioContext`をC++デーモンへそのまま持ち込むことはできない。採用時に依存範囲、公開された呼出し口とコメント、Node.jsでの実行性を確認する。Cを選ぶ場合も再生エンジンは上流SFZカーネルを使用し、SFZ一般仕様すべてを独自実装することは目標にしない。[SFZパーサー](https://github.com/Frieve-A/effetune/blob/v2.13.0/js/sfz/parser.js)、[バンク生成](https://github.com/Frieve-A/effetune/blob/v2.13.0/js/sfz/asset.js)、[ファイル読込の分離](https://github.com/Frieve-A/effetune/blob/v2.13.0/js/sfz/native-service.js)

### 音源読込を選ぶ場合の共通設計

- 音源IDとファイルの対応を解決し、定義・include・サンプルを制御側で読み込む。EffeTuneの登録情報を使う場合は読取専用とし、登録されていないブラウザ版IDは元のフォルダーを別途指定する。
- 上流と同じ音域・ループ・エンベロープ等を持つバンクを生成し、既存のコピーABIへ渡す。既定256 MiB・上限1 GiBという上流の容量設計を基に、索引とデコード後PCMを含めた上限を定める。上限超過時の部分取込みを再現するか、明示的なエラーにするかは方式選択時に確定する。
- 音源を有効化してから新パイプラインを交換する。準備中の状態は上流の処理フレームに基づいて進め、固定sleepや音声コールバックでの読込を使わない。再読込失敗時は既存パイプラインと設定を保持する。
- 音源・キャッシュ・登録情報の変更を検出する単位を定める。レート・バックエンド切替で再利用できるバンクと、再構築が必要なDSP状態を区別する。キャッシュにはアプリコミットとバンク形式を記録し、異なる形式をそのまま渡さない。
- SFZの音符推定はカーネル自身に任せる。解析専用Note Spectrogramの除外を維持し、`App-internal`な解析共有APIは使わない。
- SFZの報告遅延を既存のホスト補償へ取り込む。既定Timingで約80 msの遅延があり、負のTimingではdryの遅延が増える。値はカーネルから取得し、固定値として実装しない。[カーネルとアセット契約](https://github.com/Frieve-A/effetune/blob/v2.13.0/dsp/plugins/others/sfz_note_player/kernel.cpp)、[バンク形式コメント](https://github.com/Frieve-A/effetune/blob/v2.13.0/dsp/plugins/others/sfz_note_player/bank.h)

最小成果物は、合成した小さなSFZ音源を読み込み、既存CLIドライバーから入力音に応じた楽器音を確認できる状態とする。その後にID解決、複数サンプル、欠損・不正データ、容量、再読込、遅延、GTK状態表示を積み上げる。方式選択後、この順序で実行可能な工程と完了条件を追記する。

### 音源読込を今回見送る場合

`requiresExternalAssets`による既存の警告付き除外を適用する。SFZを解析専用の無警告除外へ追加しない。有効ノードはignoredと外部アセット警告、無効ノードや無効Section内はoffという現行の規則を維持する。除外によって音声、バス転送、遅延、有効ノード数が変わらないことを確認する。

どちらの製品方針でも、SFZカーネルの登録・パラメータ・容量、上流nativeテストは検証する。公式11 goldenのバンクありケースを独自共有バックエンドでも検証する場合は、既存のIR専用goldenアセット生成に小さな合成SFZバンクを追加する。これは配布バックエンドの検証であり、製品の音源管理を実装したことにはならない。[上流の合成音源](https://github.com/Frieve-A/effetune/blob/v2.13.0/tools/dsp-parity/runners.mjs)

## Cassette ArtifactsとRhythm Analyzer

### Cassette Artifacts

`md`はEncode Only / Encode + Artifacts / All / Artifacts + Decode / Decode Onlyの5値で、既定はAll。パッカー比較に各値、省略時、不正値を追加する。現在の`effetune_backend_artifact_test.cpp`はCassetteのパラメータを12 floatで固定しており、更新後の13 floatと不整合になる。既存のメタデータ読込経路へ揃え、公式18 goldenを、途中のモード・Dolby設定変更も含めて検証する。[パラメータ](https://github.com/Frieve-A/effetune/blob/v2.13.0/dsp/plugins/lofi/cassette_artifacts/params.json)、[公式ケース](https://github.com/Frieve-A/effetune/blob/v2.13.0/dsp/plugins/lofi/cassette_artifacts/cases.json)

製品では5モードをプリセットとして読み込み、音声の差を確認する。初期値、1 / 2 / 6ch、チャンネル選択、seed、reset、レート変更後の再構築を含める。許容絶対誤差`1e-5`を維持する。`md`のない入力には新しい上流仕様の既定値を使い、旧パラメータ配置を残す互換実装は作らない。

### Rhythm Analyzer

`mn`、`mx`、`ck`の配置は変わらず、報告遅延も0を維持する。クリックなしはPCMを保持し、クリックありは処理範囲の先頭1〜2chへ音を加える。現行`rhythm_analyzer_test.cpp`の決定的な120 BPM入力を使い、新解析方式での検出開始、安定後の周期、reset、ブロック分割、未選択ch保持を検証する。[公式説明](https://github.com/Frieve-A/effetune/blob/v2.13.0/docs/dsp/effects/rhythm-analyzer/index.md)、[カーネル](https://github.com/Frieve-A/effetune/blob/v2.13.0/dsp/plugins/analyzer/rhythm_analyzer/kernel.cpp)

上流の対応レート外、例えば64 kHzでは、2.12の簡易解析から、音声を通過させて解析・クリックを行わない動作へ変更された。PipeTuneのAutomaticを考慮し、この動作を製品プリセットから確認する。独自の簡易解析やレート変換は追加しない。

新解析には約1秒のデジタル無音で内部状態をクリアする仕組みがある。PipeTuneの休止をオフにしても、このカーネル自身のresetは起こる。短いゼロ入力、長いゼロ入力、微小な非ゼロ入力、音声再開を区別し、休止オフを「解析状態を必ず保持する」と説明しない。[無音の契約](https://github.com/Frieve-A/effetune/blob/v2.13.0/dsp/plugins/analyzer/rhythm_analyzer/rd6_engine.h)、[上流nativeテスト](https://github.com/Frieve-A/effetune/blob/v2.13.0/dsp/plugins/analyzer/rhythm_analyzer/native_test.cpp)

公式6 goldenのclick-onは192,000フレームへ延長されている。メタデータのフレーム数とPCMをそのまま使い、旧ケースの長さや古いクリック検出時刻を固定しない。許容絶対誤差`1e-6`を維持する。上流仕様と既存テストの期待が矛盾した場合は、仕様を確定してからテストを変更する。

## 段階的な実施計画

### 0 基準状態とSFZの範囲を確定する

現行2.12.0で`make test`を実行し、既存失敗と今回の差分を分ける。環境、CPUで実行可能なISA、パッケージ生成環境、必要なツールを記録する。SFZはA / B / Cまたは見送りを選び、読込・デコード・容量・更新検出の範囲を本書へ反映する。

成果物は基準ログと確定した工程。完了条件は、既存問題を識別できることと、SFZの選択工程が実装可能な粒度で決まっていること。文書変更は`doc: finalize EffeTune 2.13.0 integration scope`としてコミットする。

### 1 アプリタグを更新し最小プリセットを実行する

現在の製品経路でAdaptive Predictionの実行とSFZの識別を検証するテストを追加し、2.12でREDを確認する。SFZの製品動作が未実装の段階では、外部アセットが必要であることを識別する。更新、カタログ容量対応、コアソースと浮動小数点条件、カタログ期待値、Cassetteの固定パラメータ修正をまとめて行い、ビルド可能な状態でGREENを確認する。

最小のAdaptiveプリセットを`pipetune/test/presets/effetune-2.13/`へ追加する。CLI/GTKで2.13.0の表示、共有ライブラリのロード、112カーネルの契約一致、最小PCM処理を確認する。armv7lで空キャッシュからモデルを生成し、インストール済みバックエンドをロードする。

完了条件は、ホストDebugと対象テストが成功し、最小プリセットの音声処理を観測でき、ARMのモデル構築が成立すること。コミットは`feat: integrate EffeTune app v2.13.0 backends`。

### 2 Adaptive Predictionの学習と処理幅を完成させる

学習、チャンネル選択、3ch以上のAllの拒否、reset・休止の再現テストを追加する。処理幅の不備はREDを確認してから製品を修正する。17 goldenと上流nativeテスト、製品プリセットからの長い入力を分担する。CLIで予測音・残差の変化が分かる観測用プリセットを揃える。

完了条件は、Scalarと実行可能SIMDで公式goldenに一致し、学習の成果とチャンネル制約を製品経路で確認でき、再構築・休止の状態消去が説明と一致すること。コミットは`feat: support Adaptive Prediction presets`。

### 3 CassetteとRhythmの変更を検証する

Cassetteの5モード・全18 goldenと、Rhythmの全6 golden・長時間クリック・非対応レート・無音後の再検出を検証する。Cassetteのモード別とRhythmのクリック有無を観測できるプリセットを追加する。Note Spectrogramの新しい音域既定値もパッカー比較へ反映し、9種類の解析除外を回帰確認する。

完了条件は、上流の変更を新しい仕様のまま実行でき、既存のレート・ch・ブロック分割・reset検証が成功すること。コミットは`feat: validate EffeTune 2.13 processor changes`。製品の不具合が見つかった場合は、根本修正を独立した`fix:`コミットにする。

### 4 選択したSFZ対応を完成させる

見送りを選ぶ場合は、警告・ignored / off表示、他DSPとの混在、PCM・バス・遅延への非干渉を検証し、利用者文書へ制約を記載する。

音源読込を選ぶ場合は、最小音源の再生、選択したID解決・取込み方式、失敗時の保持、レート・バックエンド切替、インストール後の読込を順に完成させる。各単位で再現テストのREDからGREENへ進み、実際に音を出せる成果物をコミットする。合成音源による入力音符と再生音、dry / wet、Timing、未選択ch、reset、容量境界を検証する。複雑なGUI管理画面を先に作る工程にはしない。

完了条件は、選択した方針が製品のPCMと状態表示で確認できること。音源読込を実装する場合は、公式11 goldenを実共有バックエンドで検証し、アセット未読込時のdry出力だけを再生成功と判定しない。コミット例は`feat: load SFZ Note Player assets`、見送りなら`feat: classify SFZ Note Player preset entries`。詳細は段階0の選択結果で置き換える。

### 5 既存機能と実運用経路を回帰検証する

FIR・オーバーサンプリング共通処理、Brickwall Limiter、MD / MP3 Codec Simulator、Click Remover、遅延線、変更された既存DSPを上流native・parityと製品テストで確認する。既存goldenにはPCM自体の変更もあるため、JSONの参照ハッシュ更新と区別する。AM Radio、Vinyl、Phaser、Tube Simulatorの対象ケースもアプリタグの参照PCMを基準に比較する。

レート・バックエンド変更、プリセット再読込、エラー時の旧状態保持、入力遅延補償、複数出力へのチャンネル配分、無音時休止、GTKのenabled / off / ignoredを確認する。Debugの割当・解放監視を無効化せず、失敗した場合は音声経路での操作を特定する。

ReleaseでAdaptive、Rhythm、既存オーバーサンプリングDSPの処理時間、初期化、メモリ、共有ライブラリサイズを記録する。SFZを対応する場合はバンク読込と再構築時のピークも測る。コンパイルと負荷測定を競合させない。

完了条件は、実行時の切替・遅延・状態表示と既存DSPに説明できない退行がなく、ホスト全体テストが成功し、測定条件と結果が揃うこと。コミットは`feat: verify EffeTune 2.13 runtime integration`。不要な試行修正は残さない。

### 6 利用者文書と配布検証を完了する

英日README、`docs/en/details.md`、`docs/ja/details.md`、`pipetune/README.md`を更新し、Adaptiveの処理幅・学習・休止、Cassetteのモード、Rhythmの非対応レートと内部無音reset、選んだSFZ方針を利用者の視点で説明する。内部のモデル・ABI・検証方法は既存`pipetune/docs/dsp-backends.md`と必要な`architecture.md`へ反映する。過去の計画は履歴として残す。

NOTICEと実際の配布ソースを照合する。今回の上流NOTICEの追加にはWASM用Emscripten・muslが含まれるため、ネイティブのみの製品へ一律に転記しない。Rhythmのfdlibm由来コードは`g2_math.h`から`rd6_math.h`へ移っており、引き続き表示対象である。SFZの補助ツールやデコーダーを配布する場合は、それらの出典・ライセンス表示も含める。[上流NOTICE](https://github.com/Frieve-A/effetune/blob/v2.13.0/plugins/dsp/NOTICE.txt)、[Rhythmの数学処理](https://github.com/Frieve-A/effetune/blob/v2.13.0/dsp/plugins/analyzer/rhythm_analyzer/rd6_math.h)

作業ツリーを固定し、Debug全体、クリーンRelease全体、単独ビルド、GTK E2E、一時DESTDIRへのインストール、現行9構成のパッケージ生成・ロード・PCMを確認する。最終完了条件を照合し、実行していないISAと実機未検証範囲を記録する。

完了条件は、選択したSFZ範囲を含む最終条件を満たし、文書と検証記録が揃うこと。コミットは`doc: describe EffeTune 2.13.0 support`。パッケージ実装の修正が必要なら`fix:`または`chore:`として分離する。

## 検証方法と実施条件

基本の入口は既存Makefileを使う。実装の最終段階で全体テストを省略しない。

```sh
make test
cmake -S . -B build/effetune-2.13-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build/effetune-2.13-release --parallel
ctest --test-dir build/effetune-2.13-release --output-on-failure
DESTDIR="$PWD/artifacts/verification/effetune-2.13.0/install" cmake --install build/effetune-2.13-release --prefix /usr
./build_package_all.sh
```

- 現在の`build_package.sh`はDebian trixieのx86_64 / i686 / arm64 / armv7l / riscv64、Ubuntu 24.04 / 26.04のx86_64 / arm64の9構成。前回計画の13構成を流用してbookwormを復活させない。
- ホスト全体テストには上流native、全DSPパッカー、上流native parity、独自共有バックエンドgolden、単独ビルド、インストール、GTK E2Eを含める。新規nativeテストとモデル検証のCTest登録を確認する。
- 新しい公式golden、既存の更新PCM、製品プリセットの挙動をそれぞれ記録する。goldenは各メタデータのフレーム数・パラメータ・イベント・許容誤差に従い、今回のために参照PCMを再生成しない。
- 各配布先でScalarと実行可能SIMDをロードし、Adaptive・Cassette・RhythmとSFZの選択範囲を検証する。前回記録にあるi686の旧golden差は今回も更新前後比較で切り分け、新しい失敗を既知の差として処理しない。
- 長い学習・拍検出は処理フレーム数で時間を進める。エミュレーションを含む実測速度からタイムアウトを設定し、時間がかかることを理由に検証を省略しない。
- GTKの既存表示とE2E基盤を使う。ブラウザを介する試行が必要になった場合はPlaywright MCPを使い、UIの時間軸の問題が見つかった場合は動画による検証を追加する。画像マスターを変更する場合は目視確認する。
- 追加パッケージが不足している場合はインストールを促す。新規のTypeScript / JavaScriptツールはnpm、TypeScript、Vite、Vitest、`prettier-max`、`resolved-killer`のプロジェクト規則に従う。C++側の新規I/Oが必要なら`cardio`の非同期処理を使う。外部APIは公式文書とAPIコメントの両方を確認する。
- 版管理は既存の`screw-up format`経路を維持する。コミット後の検証では再構成し、全体テスト中にソースを変更しない。計画外の問題は設計前提を見直して本書へ反映し、テストと計画の契約が矛盾する場合は確認する。
- 機能削除が必要になった場合は、削除を確認する一時テストでRED、削除後GREENとコミット、その後に一時テストを削除する手順を守る。単なる参照版の更新と製品機能の削除を混同しない。

実行コマンド、対象コミット、環境、ログ、誤差、負荷、配布物ハッシュは`artifacts/verification/effetune-2.13.0/`へ保存し、本書へ工程ごとの結果を追記する。

## 実装の最終完了条件

- [ ] 指定の公式アプリタグ`v2.13.0`を参照し、外部ソースを編集していない。
- [ ] CLI / GTKの表示が2.13.0で、実行可能な各バックエンドの112カーネル・パラメータ・容量が一致する。
- [ ] 新しいコア、HarmNet、3モデルの埋め込み、ARM生成物が単独ビルドと配布物で利用できる。
- [ ] Adaptiveの学習・音声・1〜2ch制約・reset・休止を製品経路で確認し、17 goldenが成功する。
- [ ] Cassetteの5モードと18 golden、Rhythmの6 goldenと長時間クリック・非対応レート・無音後の再検出が成功する。
- [ ] SFZの対応方式または見送りが確定し、その音声・警告・状態表示・配布の条件を満たす。
- [ ] 解析9種類の除外、既存DSP、生成FIR、遅延補償、複数出力、再構築・失敗時の保持に退行がない。
- [ ] Debug / Release全体、単独ビルド、GTK E2E、インストール、現行9配布構成の結果を記録した。
- [ ] 利用者文書、著作権表示、性能・メモリの測定条件と未検証範囲が揃っている。

## 計画段階の確認

- [x] 追従先を利用者指定のアプリタグに固定した。
- [x] 現行コード、公式タグ間差分、対象版の文書・APIコメントを基に対応箇所を整理した。
- [x] 各共通工程に実行可能な成果物、TDD・回帰検証、コミット、完了条件を定義した。
- [x] SFZについて、既存連携を再利用できる部分と追加が必要な部分を分け、3案を比較した。
- [ ] SFZの採用方式または見送りを確定し、段階4を具体化した。

計画段階では本書のみを追加する。ビルドに影響しない文書変更のため、この段階のビルド・テストは行わない。
