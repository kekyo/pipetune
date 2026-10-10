# EffeTune 2.13.0対応計画

## 目的と対象

PipeTuneが参照するEffeTuneを、公式アプリタグ`v2.12.0`（`1d4d33b2e631a840ae178295e3ea2b66b06b1e43`）から`v2.13.0`（`6791afdfc6f4e6c913b9f48c8189a44e29a09bde`）へ更新する。利用者の指定に従い、追従先はアプリタグとする。DSPライブラリの独立したリリースタグを更新基準にしない。

調査基準は2026-10-09時点のPipeTune HEAD `d4d13b4e0b17ccb9454b807b2172a3118db6f22c`。アプリ2.13.0に同梱されるDSPライブラリの版は0.13.0である。今回の両タグは同じコミットへ解決されるが、これは今回の一致であり、今後の追従方針を変えるものではない。[公式アプリリリース](https://github.com/Frieve-A/effetune/releases/tag/v2.13.0)、[アプリタグ間差分](https://github.com/Frieve-A/effetune/compare/v2.12.0...v2.13.0)

Adaptive Predictionの追加、Cassette ArtifactsとRhythm Analyzerの変更、ネイティブバックエンド、既存DSP、現在の複数出力機能までを検証対象とする。SFZ Note Playerは、EffeTuneの登録情報を自動参照するA案と、準備処理をC++で実装するC案を組み合わせ、変換結果をキャッシュする方式に確定した。初回読込、キャッシュ再生成、再生のいずれにもNode.jsを実行時依存として要求しない。

追加指示に従い、現在外部アセットを理由に除外しているIR ReverbとRoom EQも今回対応する。既存のCrosstalk Cancellationとパラメータから生成するFIRも同時に回帰検証する。SFZのJS由来処理はPipeTuneのホスト機能から構造的に分離し、将来上流に対応するC++実装が追加された際に置き換えられる構成とする。上流の将来の移植は利用者の予想であり、発表済みの予定とは扱わない。

計画の承認後、2026-10-09から段階的な実装を開始した。以下の設計・完了条件と、末尾に追記する実施結果を区別する。

2026-10-10の追加指示により、システムのGPL構成FFmpegを参照していた実装はタグ`ffmpeg`（`38c09d7`）に保存し、実装開始前の`b2a4911`からLGPL構成の自前ビルドへ再構成する。旧タグの機能とテストを基準とするが、旧コミットを一対一に再現する必要はない。旧検証記録はタグ側に保持し、新構成の成功として流用しない。

## 上流の変更と対応範囲

| 対象 | アプリ2.12.0からの変更 | PipeTuneでの対応 |
| --- | --- | --- |
| Adaptive Prediction | 学習による予測・残差出力・自己帰還音の生成を追加 | 通常の加工DSPとして実行し、処理幅を1〜2chに制限する |
| SFZ Note Player | 入力音から音符を推定し、外部SFZ音源で再生 | 登録情報の自動参照、独立したC++準備処理、キャッシュ、音源再生まで実装する |
| IR Reverb / Room EQ | 従来から存在する外部アセットDSP | 今回の追加範囲として、IRライブラリ読込と測定に基づく補正FIR生成を実装する |
| Cassette Artifacts | `md`による5種類の処理モードを追加 | 新しいパラメータ配置と各モードの音声を検証する |
| Rhythm Analyzer | Rd6系の解析へ移行、モデルとクリック検証を更新 | 拍検出後のクリック、無音、非対応レートの契約を検証する |
| Note Spectrogram | HarmNetへ移行し、既定の音域を変更 | カタログ・配布バックエンドへ反映し、製品では解析除外を維持する |
| モデル埋め込み | 外部binの埋め込み対象が8個からRhythm用3個へ変更 | 既存の共通モデルターゲットとARM用生成物変換を適用する |
| エンジン | 遅延更新API、音楽トランスポート、経路PCMの観測を追加 | 新しいコアをビルドし、既存の再構築・遅延補償を回帰検証する |
| 演算・メモリ | FIR・遅延線などを最適化し、処理中の解放もDebug監視対象に追加 | Scalarと実行可能SIMDでPCM、遅延、reset、処理負荷を確認する |
| アプリ機能 | VisualizerのGuitar、LANリモート操作などを追加 | 今回のPipeTuneへの移植対象には含めない |

登録カーネルは110から112になる。無警告で除外する解析専用DSPは既存の9種類を維持する。登録数は、音源なしで利用できるDSP数ではない。[registry](https://github.com/Frieve-A/effetune/blob/v2.13.0/dsp/registry.inc)

パラメータ編集UI、ライブオートメーション、音符・スペクトル表示、ホストの音楽トランスポート連携、独立した音源管理・事前取込みUI、旧DSPを選択する互換モード、PipeTune自身のリリース番号変更は対象外とする。外部アセットの利用に必要な診断は既存CLI / GTKへ追加する。

## 現行コードに必要な統合変更

### カタログと共有バックエンド

`pipetune/tools/generate-dsp-catalog.mjs`の`nativeAssetCapacity`は現在、`32 MiB convolution cap`だけを受け付ける。SFZの`1 GiB bank and index cap`を認識し、slot 0の容量を1,073,741,824 bytesとして生成する。既存の畳み込みアセットの上限は32 MiBのままとする。未知の容量表記を推測で解釈する処理は追加しない。

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

`et_instance_set_analysis_source`はヘッダーで`App-internal`と明記されているため使わない。SFZはカーネル内の独立した音符解析を使い、Note Spectrogramを実行必須にしない。音楽トランスポートも今回供給しない。[C ABIとコメント](https://github.com/Frieve-A/effetune/blob/v2.13.0/dsp/include/effetune/abi.h)、[ProcessInfoとカーネル契約](https://github.com/Frieve-A/effetune/blob/v2.13.0/dsp/include/effetune/kernel.h)

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

## 外部アセットDSPを今回対応する範囲

対象タグのパラメータ仕様でアセットを持つDSPは9種類である。`requiresExternalAssets`を一律に解除するのではなく、各DSPに必要な準備処理を実装してから製品経路へ接続する。アセットをコピーできることと、プリセットから正しいアセットを作れることを別々に検証する。

| DSP | 入力と現行状態 | 今回の成果物 |
| --- | --- | --- |
| SFZ Note Player | `sf`の音源ID。新規で未対応 | 登録フォルダーのSFZ・include・サンプルから再生用バンクを生成 |
| IR Reverb | `ir`のIRライブラリID。警告付き除外 | 保存された原音から、プリセットの加工・経路・レートに対応するIRを生成 |
| Room EQ | `ms`と`ms0`〜`ms15`の測定ID。警告付き除外 | 保存測定の周波数応答・IRから補正FIRを設計 |
| Crosstalk Cancellation | `ll` / `lr` / `rl` / `rr`の測定ID。既に対応 | 読込・変更監視を新しい共通部分へ接続し、既存の補正動作を維持 |
| Bass Management、FIR Crossover、5Band FIR PEQ、Group Delay EQ、Group Delay PEQ | パラメータからアセットを生成。既に対応 | 外部ファイル不要の生成経路を維持し、他の外部アセットDSPとの混在を検証 |

外部データの出発点は、現行の`resolveEffeTuneDirectory`が参照する`$XDG_CONFIG_HOME/effetune`、未指定時は`~/.config/effetune`とする。EffeTuneの登録情報・原音・測定バックアップは読取専用で利用し、Electronの起動やIPC接続を要求しない。プリセットには実データが含まれないため、ブラウザ内にしかないIDや他PCにしかないファイルは自動取得できない。EffeTuneデスクトップ版に対象データを登録し、その環境で保存したプリセットを利用する前提とする。独自のID登録・事前取込み機能は今回追加しない。[SFZ保存形式](https://github.com/Frieve-A/effetune/blob/v2.13.0/electron/sfz-library-ipc.js)、[IR保存形式](https://github.com/Frieve-A/effetune/blob/v2.13.0/electron/ir-library-ipc.js)、[測定ストア](https://github.com/Frieve-A/effetune/blob/v2.13.0/js/measurement-store/client.js)

### 共通の読込・デコード・キャッシュ

`PipelineLoadContext`を測定ディレクトリーだけの指定から、EffeTuneデータルートとPipeTuneキャッシュの解決に必要な設定へ拡張する。試験では一時ディレクトリーを注入できるようにする。`measurement_store.cpp`のJSON・base64・有限PCMの検証は再利用するが、Crosstalk専用の測定条件は個別検証へ残す。

新規I/Oは制御側で`cardio`による非同期処理とし、取得済みバイト列をネイティブ音声デコーダーへ渡す。デコーダーはSFZとIRで共用し、元レート・チャンネル順を保持したplanar float32 PCMと実際のフレーム数を返す。SFZのmono / stereo制約とIRの1〜16ch制約は各準備処理で検証する。デコード後のサイズ・有限値を確認し、メタデータだけを信用した無制限確保をしない。

音声デコーダーにはFFmpegのネイティブライブラリを使う方針とする。WAV（`.irs`を含む）・AIFF・FLACに加え、IRで使われるMP3・Ogg・M4Aも配布先のデコーダーで検証する。`libavformat` / `libavcodec`等の公開APIを小さなアダプターに隠し、メモリ入力とPCM形式の変換を行う。`ffmpeg`コマンドやNode.jsを実行時に起動しない。既存のストリーム用レート変換とアセット準備用の変換は契約を分け、上流の解析・窓付きsinc等を別アルゴリズムへ無条件に置き換えない。ブラウザのデコーダーとの丸め差は、同一PCMによる準備処理の比較と実ファイルのデコード試験を分けて評価する。[libavformat公式文書](https://ffmpeg.org/libavformat.html)、[libavcodec公式文書](https://ffmpeg.org/libavcodec.html)、[メモリ入力例](https://ffmpeg.org/doxygen/trunk/avio_read_callback_8c-example.html)、[I/O APIと所有権コメント](https://ffmpeg.org/doxygen/trunk/avio_8h_source.html)

キャッシュは`$XDG_CACHE_HOME/pipetune`、未指定時は`~/.cache/pipetune`の配下に置く。EffeTune自身のPCMキャッシュへ書き込まず、原本から再生成できるPipeTune専用データとして扱う。次を共通契約とする。

- キーにアプリコミット、準備処理の版、アセット形式、元データの内容ハッシュ、デコーダーの版、加工パラメータを含める。Room EQ / IRには生成レート・実効処理幅・経路も含める。SFZは元レートのPCMを保持し、生成に影響しない再生レートやSIMDの違いでバンクを分けない。
- 有効なキャッシュでも参照元の存在・変更を確認する。登録IDの再割当、include追加、サンプル差替え、IR原音・測定の変更を検出する。読込中に原本が変化した結果を完成品として保存・公開しない。
- 一時ファイルからの原子的な置換、形式・長さ・ハッシュ検証を行う。破損・旧版キャッシュは原本から作り直す。容量不足や書込不可の場合も、準備が成功したアセットはメモリ上で使用できるようにする。
- 単一アセットの上限と、準備中のPCM・旧新パイプライン共存・キャッシュの合計を区別して管理する。同一入力の準備を重複させず、不要なコピーを減らす。キャッシュ総量は既定2 GiBを上限に古い未使用項目から回収し、削除は稼働中アセットの所有権に影響させない。

工程4.3の具体化として、プロセス内で外部アセットの準備を非同期mutexにより順番に実行し、完了した不変の準備結果を内容キーで共有する。各パイプラインの構築中だけ共有結果を保持し、構築後はネイティブDSP内のコピーとキャッシュ識別子を保持する。同一入力でも参照元の検証は省略しない。待機中の取消は個別要求へ適用し、別のdispatcherのpromiseを共有しない。

アセット用メモリのプロセス合計上限は64bitで4 GiB、32bitで1 GiBとする。これはアセットの準備領域・保持payload・ネイティブ側footprintの予約合計で、プロセス全体のRSS上限ではない。IRの準備は、原音・JSON・デコード・リサンプル・倍精度の解析配列を含む保守的な768 MiBの作業領域を確保前に予約し、完了後は保持payloadの実容量へ縮小する。実際に768 MiBを常時確保するわけではない。SFZとRoom EQは各工程でそれぞれの上限見積りを接続する。旧DSPの予約は破棄まで保持し、新候補との合計が上限に入らなければ現在の音声を保持して読込エラーとする。小さい上限の注入と合成メタデータで、巨大な実アセットを用意せず境界を検証する。

### ファイル監視、準備状態、エラー

`DspPipeline::measurementFiles()`と`PipelineLoadResult.measurementFiles`を汎用の依存ファイル一覧へ変更し、`ActivePresetFileMonitor`と全呼出し元を揃える。監視対象はSFZ登録情報・定義・include・参照サンプル、IR indexと対象原音、Room EQ / Crosstalkの測定JSON。容量選別の判断に使ったSFZサンプルも依存に含める。未作成ファイルと親ディレクトリーの差替えを扱う既存inotify実装を使い、生成キャッシュは監視しない。

準備は既存パイプラインを動かしたまま行い、コピーABIで転送後、必要なwarmupを処理フレーム数で進め、アセットが有効になったことを確認してからreset・交換する。既存のパイプライン変更処理を、重い準備中ずっと変更用mutexを保持する構造にしない。準備開始時のプリセット・レート・バックエンド・ch構成を世代で識別し、設定変更や終了で不要になった結果を公開しない。音声コールバックでの読込・デコード・確保・解放、固定sleep、ポーリング、新規の場当たり的なワーカースレッドを追加しない。

新規対応の3種類では、指定された参照を解決できない、デコード・設計・アセット転送に失敗した、設定に矛盾がある等により準備結果が成立しない場合を読込エラーにする。上流が認める部分音源・縮小は次段落の警告として扱う。未指定の場合は上流の未読込動作を保ち、音源・測定未設定の診断を出す。空IDと、指定済みIDが見つからない状態を区別する。Room EQの意図的な未割当chも区別し、割当済みchの欠損を単位インパルスで隠さない。再読込失敗時は既存パイプライン・実効設定・遅延・音声を保持する。現在のCrosstalkの不正測定時の警告付き除外は今回変更しない。

有効な部分音源や補正の縮小を上流が認める場合は、稼働中ノードにも準備時の警告を表示する。現在の`PipelineWarning`は「除外理由」の契約なので、そのまま流用せず、稼働状態と診断を区別できるよう型・CLI / GTK表示を拡張する。無効ノード・無効Sectionはoffのままとし、外部データを読まない。失敗した準備で分かった依存先も現在の監視対象に追加し、ファイル復旧時に再試行できるようにする。

## SFZ Note Playerの採用方式と分離設計

### 比較した方式と確定事項

| 案 | 役割と利用者の操作 | 評価と採否 |
| --- | --- | --- |
| A EffeTuneの登録音源を自動参照 | デスクトップ版の`sfz-references.json`からIDを元フォルダーへ解決 | 採用。既存プリセットを使え、音源の二重登録が不要 |
| B PipeTuneへ事前取込み | CLI等でIDと音源を指定し、再生用バンクを先に保存 | 今回は不採用。初回操作と再取込み管理が増えるため、自動準備・キャッシュを選ぶ |
| C C++で直接準備 | SFZ解析、音声デコード、容量選別、バンク生成をネイティブ側で実行 | 採用。Aと組み合わせ、初回読込を含めNode.jsへの実行時依存をなくす |

当初のA＋キャッシュ案は上流JSを動かす補助ツールを想定していたが、利用者との確認によりA＋C＋キャッシュへ変更した。Node.jsは従来のビルド処理と上流JSとの比較テストにのみ使用する。配布製品の初回変換をNode.jsへ逃がす構成にはしない。

プリセットの`parameters.sf`は24桁のIDで、SFZ本文や音声ではない。登録情報の`id`、`name`、`root`、`path`から元の`.sfz`を特定する。定義・include・サンプルは登録root内で解決し、相対パス、区切り文字、root外参照、循環includeを上流契約に合わせて扱う。上流の`.sfzbank`ソース格納形式を、PCM入りの再生用バンクとして誤って渡さない。[プラグイン](https://github.com/Frieve-A/effetune/blob/v2.13.0/plugins/others/sfz_note_player.js)、[登録情報の検証](https://github.com/Frieve-A/effetune/blob/v2.13.0/electron/sfz-library-ipc.js)、[ソース格納形式](https://github.com/Frieve-A/effetune/blob/v2.13.0/js/sfz/bank.js)

### モジュール境界と将来の差替え

準備アルゴリズムは`pipetune/asset-preparation/`配下に置き、`sfz/`、`ir/`、`room-eq/`を個別の非公開C++ターゲットとしてビルドする。これらとメモリ入力のドライバーは、PipeWire / GTKや製品全体をビルドせずに実行できる構成にする。実際に共有するPCM・アセット記述等の小さな値型だけを共通部分へ置く。ホストの大きなクラスを継承する汎用フレームワークは作らない。

| 層 | 担当 | 依存させないもの |
| --- | --- | --- |
| SFZ準備モジュール | 文書解析、領域の正規化、必要サンプルの列挙、容量選別、バンク生成、準備時の診断 | PipeWire、GTK、`DspPipeline`、XDG、登録JSON、キャッシュ、ファイル監視、デコーダー実装 |
| PipeTuneのアセット連携層 | ID解決、非同期I/O、デコード依頼、入力スナップショット、キャッシュ、監視対象、世代と所有権 | SFZ opcodeの解釈、領域選別、バンクの内部レイアウト |
| バックエンド接続 | 準備結果の記述から既存コピーABIへの変換、warmup、ACTIVE確認、遅延取得 | SFZの構文、元ファイルの探索 |

SFZ準備モジュールへの入力はメモリ上の文書集合・サンプルのメタデータ・デコード済みPCMと制限値にし、不足する文書・PCMの要求を連携層が満たす。出力は所有権が明確なバンクバイト列、形式・容量情報、診断とする。yyjsonの木、FFmpegの型、EffeTuneエンジンのポインター、ホストの寿命を境界の外へ漏らさない。I/Oを行わず、メモリ入力だけで動かせるドライバーを最初の実装単位に含める。

`parser`、`selection`、`bank`の責務をファイルで分け、上流の`parser.js`、`service.js`、`asset.js`との対応を追えるようにする。ETA1ヘッダーとSFZテーブルv2のエンコードは`bank`側へ局所化し、ホストが数値オフセットを扱わない。移植元のアプリコミット・ファイル・対応する関数と、PipeTune固有の差を内部文書と必要なDoxygenコメントに記録し、上流由来のライセンス表示を保つ。[パーサー](https://github.com/Frieve-A/effetune/blob/v2.13.0/js/sfz/parser.js)、[容量選別](https://github.com/Frieve-A/effetune/blob/v2.13.0/js/sfz/service.js)、[バンク生成](https://github.com/Frieve-A/effetune/blob/v2.13.0/js/sfz/asset.js)、[ネイティブ連携側](https://github.com/Frieve-A/effetune/blob/v2.13.0/js/sfz/native-service.js)

将来上流へ同等の公開C++機能が入った場合は、この小さな準備インターフェイスへ接続するアダプターを置き、同じ適合テストを通した後にPipeTune側の重複実装を削除する。登録情報・キャッシュ・監視・DSPライフサイクルはそのまま使う。現時点で存在しないAPIを想定した実装、実行時の旧新切替、恒久的な二重実装は作らない。

### 2.13.0との意味上の一致

SFZ全仕様の実装ではなく、指定アプリタグのSFZ読込の動作を基準にする。

- `#include`、`#define`、コメント・引用文字列、`control/default_path`、global / master / group / regionの継承を扱う。includeは読み込んだ文書から、サンプルは選択SFZとdefault_pathの規則から解決する。上流の深さ32・展開回数10,000の制限も検証する。
- 音域・ベロシティ・ランダム範囲・シーケンス、ピッチ中心・追従・transpose / tune、volume / pan / amp_veltrack、offset / end、4種類のloop_mode、ループ境界、ADSHRを対応するopcode単位で比較する。初期CCと`set_ccN`、`sw_default`を反映し、releaseトリガーや動的条件等の非対応領域は上流の除外・警告を再現する。
- バンク予算は既定256 MiB、64 / 128 / 256 / 512 / 1024 MiBを指定できる設定とする。元ファイル総量、デコード後PCM、バンクと索引の容量を別々に検証する。上限1 GiBの判定に索引を含める。
- 予算内で全再生可能キーを残す上流の縮小を実装する。ベロシティ64近傍等の選別順と同順位の決定、ランダム・ラウンドロビンの簡略化、診断を一致させる。全キーを覆う最小構成でも収まらない場合は失敗にし、黙って音域を失わない。
- デコードした元レートとチャンネル順を保持する。テーブルの整数ビット列、ソート順、ループの補正・無効領域、フレーム境界は上流と照合する。文字列の順序をC++のバイト順に置換するだけでは非ASCII名で一致しない点も検証する。

上流JSを変更せずにテスト時だけ実行し、同一の文書・メタデータ・PCMから領域、選別結果、警告区分、バンクを比較する。純粋な解析・パックは決定的な一致を確認し、演算がある値は根拠のある数値許容誤差を定める。テストはソースの文字列を検索するものにせず、生成バンクを実カーネルで再生した音も検証する。

### 再生とライフサイクル

再生エンジンは上流SFZカーネルを使い、独自のサンプラーを作らない。既存の`pipetune_effetune_instance_asset_copy_v1`で転送し、準備処理がACTIVEになるまでオフラインで進めてから使用する。reset後は音符・履歴を消し、読込済みバンクは再利用する。レート・バックエンド切替ではバンクの再利用とDSPインスタンスの再構築を分ける。

報告遅延はカーネルから取得する。既定Timingで約80 ms、負のTimingではdry遅延が増えるため、値を固定せず既存の遅延補償へ渡す。音符を検出できる長さの合成入力、dry / wet、Timing、選択chと未選択ch、端数ブロック、reset・休止・切替を実製品経路で確認する。[カーネル](https://github.com/Frieve-A/effetune/blob/v2.13.0/dsp/plugins/others/sfz_note_player/kernel.cpp)、[バンク契約](https://github.com/Frieve-A/effetune/blob/v2.13.0/dsp/plugins/others/sfz_note_player/bank.h)

公式11 goldenを独自共有バックエンドでもすべて検証する。既存のIR専用goldenアセット生成へ、公式の`sfz-sine-v1`に対応する合成バンクを追加する。バンクなしのgoldenと、実ファイル・登録IDから楽器音が出る製品試験を分け、dry出力だけで音源対応が成功したと判定しない。[上流goldenランナー](https://github.com/Frieve-A/effetune/blob/v2.13.0/tools/dsp-parity/runners.mjs)

## IR Reverb

`parameters.ir`をEffeTuneデータ下の`ir-library/index.json`から解決し、entryの`originals`が示す原音を読む。単一ファイルとL / Rのペアを扱い、保存名・サイズ・ハッシュ・compositionを検証する。IDは単一原音のSHA-256先頭24桁、ペアは左右のハッシュを連結して再ハッシュした先頭24桁という上流の規則で照合する。SFZのランダムIDとは混同しない。上流の解析sidecarやPCMキャッシュがなくても原音から準備できるようにする。[ライブラリ形式](https://github.com/Frieve-A/effetune/blob/v2.13.0/js/ir-library/ir-library-store.js)、[ID生成](https://github.com/Frieve-A/effetune/blob/v2.13.0/js/ir-library/ir-library-id.js)、[原音解決](https://github.com/Frieve-A/effetune/blob/v2.13.0/js/ir-library/service.js)

原音読込だけではアプリと同じ効果にならないため、`ir/`モジュールで次の準備処理を実装する。

- `cm`のAuto / Mono / Independent / True Stereo / Diagonal Matrixを、実効処理chとIRのch数から解決する。4ch・2ch選択時のAutoはLL / LR / RL / RRとして扱う。ペア原音は各2chを検証し、同一レート化、短い側のゼロ詰め、LL / LR / RL / RRの順に結合する。
- `lt`と`cr`から畳み込みレート・head blockを決める。Zero latencyはFullのみ、Quarterは176.4 kHz以上という上流の制約を明示的に検証する。Autoとレート変更時の解決結果もキャッシュ・遅延へ反映する。
- `dc` / `co` / `dt` / `tr`に対応するDirect Cut、Cut Offset、Decay、Trim、フェード、トポロジーごとの正規化を移植する。Direct Cut後の正規化は未切断の原音を基準にする。加工後の解析と実IRを上流JSへ比較する。
- ETA1 payloadを生成し、32 MiBの上限はPCM長だけでなく畳み込み器のcommit時メモリ見積りで確認する。上流どおり必要な長さへ制限・フェードして、縮小を診断する。原音64 MiB・デコードPCM64 MiB・最大16chの読込制限も別途適用する。

チャンネル数不一致を自動的な混合で隠さない。Wet / Dry Enabled / Dry Level / Pre DelayはDSPパラメータとして接続し、wetのみの既知IRにインパルスを通して、各経路の振幅・到達時刻・残響を検証する。mono、独立2ch、多ch、true stereoとL / Rペア、選択ch・バス、レート変換、加工設定、縮小、欠損・更新・再読込を含める。[準備処理](https://github.com/Frieve-A/effetune/blob/v2.13.0/js/ir-library/ir-preparation.js)、[処理設定と容量計算](https://github.com/Frieve-A/effetune/blob/v2.13.0/js/ir-library/ir-plugin-contract.js)、[ペア結合](https://github.com/Frieve-A/effetune/blob/v2.13.0/js/ir-library/ir-true-stereo-pair.js)、[アプリ側の接続](https://github.com/Frieve-A/effetune/blob/v2.13.0/plugins/reverb/ir_reverb.js)

## Room EQ

`measurement-backups/<id>.json`から共有測定`ms`とch別測定`ms0`〜`ms15`を解決する。`<id>::ch=<channel>`という仮想ch IDは元の測定ファイルへ解決し、`channelResponses`の平均応答と各点の`irId`に対応するIRを投影する。スロット番号は選択された処理範囲内の相対位置であり、3/4ch選択では`ms0/ms1`を出力3/4chへ適用する。実効幅より後のスロットは参照せず、単一chでは共有`ms`のみを使う。ch別の空欄は共有測定へフォールバックする。既存Crosstalkの「4測定・1点・所定の耳ch」という制約を適用しない。複数測定点、平均周波数応答、各点のIR・レート・基準スケールを読み取る。割当済みIDを1つでも解決できなければ全体を失敗とする。[測定選択と設定](https://github.com/Frieve-A/effetune/blob/v2.13.0/plugins/eq/room_eq.js)、[仮想chの解決](https://github.com/Frieve-A/effetune/blob/v2.13.0/js/measurement-store/client.js)

測定IRは部屋の応答であり、そのまま畳み込む補正フィルターではない。`room-eq/`モジュールで`design-core.js`と必要な群遅延・平滑化等の処理を移植し、次を実装する。

- Minimum / Linearの振幅補正から開始し、Taps、Smoothing、補正帯域、Max Boost、Level Correction、5バンドAdditional EQを反映する。Max Boostは自動反転のboostにだけ適用し、cutや意図的なAdditional EQと区別する。
- Correctionでは全測定点の有効なIRを要求し、直接音のexcess phase、Direct Window、Phase Correction、低域位相拡張、Reverb Correction / Window / Max Freq / Smoothingを対応する。Minimum / Linearでは周波数応答のみの測定も使える。
- Consensusと指定Reference Pointを分ける。複数点の振幅合成、時間整列、信頼度を使った位相合成、欠落したReference Pointのフォールバックを上流へ合わせる。
- 観測窓が不足する場合と、設計した補正がFIRの時間範囲へ収まらない場合を分ける。上流が補正を縮小・省略して残りを使用する場合は、その診断と出力を再現する。Correctionで必要なIRそのものが不足する場合は、別モードへ黙って切り替えない。
- 選択chに対応するmono / independent FIRとETA1 payloadを作り、実際のtap数から`fd`を導出する。Minimumは0、その他は実効tap数の半分とする。Allで8chを超えるときの65,536 taps上限、32 MiBのcommit制限、Gain / Delay・レート変更を扱う。プリセットに古いレートの派生値が残っていても再計算する。

FFTは既存の設計処理で使う数値基盤を調査して、小さな演算インターフェイス経由で接続する。WASMを製品へ持ち込まず、演算精度やFFTの正規化を上流JSとの比較で固定する。IR Reverb向けの残響正規化・Direct CutをRoom EQの補正FIRへ適用してはならない。[設計本体](https://github.com/Frieve-A/effetune/blob/v2.13.0/js/room-eq/design-core.js)、[群遅延解析](https://github.com/Frieve-A/effetune/blob/v2.13.0/js/room-eq/group-delay-analysis.js)、[公式の効果・制約](https://github.com/Frieve-A/effetune/blob/v2.13.0/docs/dsp/effects/room-eq/index.md)

上流JSと同一の合成測定を使い、生成FIR、実際の畳み込み出力、補正後周波数応答、位相・遅延、警告区分を比較する。平坦測定、既知のピーク・ディップ、複数点、ch別の異なる測定、IRなし、短いIR、Correctionの縮小を含める。Scalarと実行可能SIMDの実共有バックエンドを通し、フィルターが存在するだけの試験にしない。

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

### 作業ブランチと統合

実装開始の指示に従い、以下の単位で`develop`から作業ブランチを作る。各小段階のRED / GREENとコミットを残し、そのブランチの完了条件を満たした後、`develop`へ`git merge --no-ff`で統合する。次のブランチは統合済みの`develop`から作る。作業ブランチはレビュー用に残し、リモートへのpushやリリースはこの作業に含めない。

| 再構成工程 | ブランチ | 観測可能な成果物と完了条件 |
| --- | --- | --- |
| 計画更新 | `doc/effetune-2.13-lgpl-plan` | 再開始点、依存構成、工程、合格条件を記録 |
| アプリ更新・既存DSP（従来0〜3） | `feat/effetune-2.13-lgpl-backends` | アプリ2.13.0、Adaptive・Cassette・Rhythmの対象試験が成功 |
| LGPL FFmpeg基盤 | `feat/lgpl-ffmpeg` | 固定submoduleの共有ライブラリと検証driverが動作し、GPL版でRED・自前LGPL版でGREEN |
| IR・非同期アセット基盤（従来4） | `feat/effetune-2.13-lgpl-assets` | 自前FFmpegでIR音声、容量、cache、取消、旧音声保持の試験が成功 |
| SFZ（従来5） | `feat/effetune-2.13-lgpl-sfz` | 独立C++準備、登録済み音源再生、i686精度対策を維持し適合試験が成功 |
| Room EQ（従来6） | `feat/effetune-2.13-lgpl-room` | 全位相モード、ch割当、cache、監視、混在PCMの試験が成功 |
| 配布・回帰・文書（従来7〜9） | `chore/effetune-2.13-lgpl-distribution` | 全体試験、インストール、9配布構成、ソース同梱とライセンス・リンク検証が成功 |

各工程では旧タグの関連する実装・試験を再利用し、システムFFmpegへの依存設定を新履歴へ取り込まない。旧ブランチ・タグは変更しない。新規変更は振る舞いを検証する試験でRED/GREENを確認する。成果物が動作する単位にまとめてコミットし、対象試験が成功してからdevelopへno-ffで統合する。

### 0 基準状態と入力データの契約を記録する

現行2.12.0で`make test`を実行し、既存失敗と今回の差分を分ける。環境、CPUで実行可能なISA、現行9配布構成のデコーダー・非同期I/O・ハッシュ依存と公開APIを確認する。使用するAPIは、対象環境の版の公式文書とヘッダーコメントの両方を読む。不足パッケージがあればインストールを促す。

EffeTune登録形式に従う最小SFZ、単一・ペアIR、周波数応答のみ・複数点IR付き測定を、テスト側で生成できる入力として整理する。外部の大容量音源を検証の前提にしない。上流JSとの対応表と、デコード・準備アルゴリズム・DSPそれぞれの数値比較条件を記録する。SFZのA＋C＋キャッシュとIR Reverb / Room EQの対応は確定済みであり、方式選択を再度求めない。

成果物は基準ログ、入力契約、依存と比較条件の記録。完了条件は、既存問題を識別でき、後続工程の合成入力と実行環境を用意できること。記録のコミットは`doc: record EffeTune asset preparation contracts`。

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

### 4 IR Reverbを動かし、共通の外部アセット経路を作る

各小段階で振る舞いのテストを先に実行してREDを確認し、その段階の実装・GREEN・コミットまで終えてから進む。共通基盤だけを先に完成させる工程にはしない。

| 小段階 | 実装と観測できる成果物 | 完了条件とコミット |
| --- | --- | --- |
| 4.1 最小の実ファイル読込 | データルート、非同期原音読込、ネイティブデコーダー、単一IRの解決と標準加工を実装。最小mono WAVを登録したプリセットをCLIドライバーで処理 | wetのみのインパルス応答が期待どおりで、上流の標準準備結果に一致する。`feat: load native IR Reverb assets` |
| 4.2 IRの全設定 | 各chモード、L / Rペア、レート、Direct Cut / Decay / Trim、容量計算、各音声形式を追加。IR準備モジュールと上流JS比較ドライバーを独立して実行 | 設定ごとの経路・加工・遅延・制限が一致し、不正設定と欠損を診断する。`feat: support IR Reverb preparation modes` |
| 4.3 共通の依存・キャッシュ | 汎用依存一覧とキャッシュ、稼働中ノードの診断、世代を持つ準備・交換を接続。既存Crosstalkの読込も同じ監視へ移す | 原音差替えで音が更新され、再起動時のキャッシュ使用・破損時再生成・失敗時旧音声保持を確認する。Crosstalkは従来の音声と診断を維持。`feat: cache and monitor external DSP assets` |

この段階では、既存の未対応DSPのうちIR Reverbが実プリセットから使用できる状態を成果物とする。後続SFZ / Room EQ用の空の抽象だけを実装して終わらせない。

4.3は、稼働中診断と失敗時の依存保持、IRで観測可能な永続キャッシュ、世代を持つ非同期準備と交換の順に小さくコミットする。最初の単位でもCLI / GTKから未設定状態と稼働状態を同時に確認できるようにする。ブランチ全体は、欠損復旧・破損再生成・準備中の切替・旧音声保持まで検証してからマージする。

### 5 SFZの独立C++準備と製品再生を完成させる

| 小段階 | 実装と観測できる成果物 | 完了条件とコミット |
| --- | --- | --- |
| 5.1 最小バンクと音源再生 | `sfz/`の独立ターゲット、最小regionの解析、PCMからのバンク生成、ドライバーを実装。段階4のデコーダーで小さなWAVを読み、実カーネルへ渡す | PipeWire / GTKなしでも入力音から楽器音を得られ、dryとは異なるPCMを確認できる。`feat: add isolated native SFZ preparation` |
| 5.2 上流の読込動作 | include / define・スコープ・opcode・初期条件・ループ・容量選別と診断を追加。上流JS比較とデコード済みPCMによる検証を実行 | 正規化領域、選別、バンク、実音声が上流に一致する。全キーを保持できない容量不足を明示する。`feat: match EffeTune SFZ preparation semantics` |
| 5.3 登録IDとキャッシュ | `sfz-references.json`から解決し、共通の非同期読込・キャッシュ・依存監視へ接続。予算設定と診断を既存の設定・状態表示へ追加 | デスクトップ版のプリセットを再登録なしで再生し、空キャッシュ・include更新・サンプル差替え・ID変更・欠損後の復旧を確認する。`feat: resolve and cache registered SFZ instruments` |
| 5.4 製品ライフサイクル | warmup、reset、遅延、dry / wet、ch選択、切替・休止を検証し、goldenランナーに合成バンクを追加 | 公式11 goldenと実ファイル試験が成功し、Node.jsなしの初回読込・再生成・再生が成立する。`feat: integrate SFZ playback lifecycle` |

各小段階はRED → 実装 → GREENでコミットする。独立モジュールのテストと実共有バックエンドの再生ドライバーを毎段階実行できること、ホストにSFZ解釈が漏れていないことをレビューする。

### 6 Room EQの測定読込と補正設計を完成させる

| 小段階 | 実装と観測できる成果物 | 完了条件とコミット |
| --- | --- | --- |
| 6.1 Minimum / Linear | 汎用測定読込とCrosstalk固有検証を分離し、周波数応答からの補正、追加EQ、FIR出力を独立ターゲットで実装。共有`ms`から実DSPを動かす | 合成ピーク・ディップが設定どおり補正され、生成FIR・遅延・PCMが上流比較を満たす。Crosstalkの回帰が成功する。`feat: design Room EQ filters from measurements` |
| 6.2 Correction | 複数点IR、直接音・低域・残響の位相補正、Reference PointとConsensus、縮小判定・診断を追加 | 既知の位相・群遅延と補正後応答を確認でき、IR不足と有効な縮小を区別する。`feat: support Room EQ phase correction` |
| 6.3 ch割当と再読込 | `ms0`〜`ms15`、選択ペア・単一ch・All、実効tap数、Gain / Delay、キャッシュと全依存監視を接続 | ch別の測定が正しい出力だけを補正し、変更・欠損・レート切替・再起動を反映する。Node.jsなしで初回設計と再設計が成功する。`feat: integrate multichannel Room EQ assets` |

各小段階で上流JSとの比較・製品プリセットの音声処理を実行し、RED → 実装 → GREENを確認する。IRをコピーしただけ、FIR配列が非空というだけでは完成としない。

### 7 外部アセット全体の運用を検証する

SFZ・IR Reverb・Room EQ・Crosstalk・生成FIRを混在させたプリセットを使用する。再起動、レート・バックエンド・ch構成変更、原本の原子的な差替え、連続変更、準備中の別プリセット選択と終了、欠損復旧、キャッシュ破損・書込不可を検証する。欠損時の旧パイプラインと監視の保持、準備結果の世代、警告がある稼働中ノードとoff / ignoredの区別を確認する。

Node.jsと`ffmpeg`実行ファイルがない実行環境へインストール済み製品・必要な共有ライブラリ・原本だけを置き、空キャッシュから3種類すべてを動かす。開発時に作ったバンクが残ることで実行時依存が隠れないようにする。冷たいキャッシュ・再利用・再生成時の時間とピークRSSを記録し、32bitを含め容量超過を確保前に検出する。上限境界は合成メタデータによる試験と実際の小規模デコードを分け、常に巨大な音源を要求するテストにしない。

完了条件は、9種類のアセットDSPが、それぞれの有効な入力により実際に加工し、未対応という理由で除外される型がなく、故障時の契約とリアルタイム制約を満たすこと。コミットは`feat: verify external asset lifecycle`。

### 8 既存機能と実運用経路を回帰検証する

FIR・オーバーサンプリング共通処理、Brickwall Limiter、MD / MP3 Codec Simulator、Click Remover、遅延線、変更された既存DSPを上流native・parityと製品テストで確認する。既存goldenにはPCM自体の変更もあるため、JSONの参照ハッシュ更新と区別する。AM Radio、Vinyl、Phaser、Tube Simulatorの対象ケースもアプリタグの参照PCMを基準に比較する。

レート・バックエンド変更、プリセット再読込、エラー時の旧状態保持、入力遅延補償、複数出力へのチャンネル配分、無音時休止、GTKのenabled / off / ignoredを確認する。Debugの割当・解放監視を無効化せず、失敗した場合は音声経路での操作を特定する。

ReleaseでAdaptive、Rhythm、既存オーバーサンプリングDSPの処理時間、初期化、メモリ、共有ライブラリサイズを記録する。SFZのバンク読込、IR加工、Room EQ設計と、旧新パイプライン共存時のピークも測る。コンパイルと負荷測定を競合させない。

完了条件は、実行時の切替・遅延・状態表示と既存DSPに説明できない退行がなく、ホスト全体テストが成功し、測定条件と結果が揃うこと。コミットは`feat: verify EffeTune 2.13 runtime integration`。不要な試行修正は残さない。

### 9 利用者文書と配布検証を完了する

英日README、`docs/en/details.md`、`docs/ja/details.md`、`pipetune/README.md`を更新し、Adaptiveの処理幅・学習・休止、Cassetteのモード、Rhythmの非対応レートと内部無音reset、SFZ / IR / 測定データの保存先と読込条件を利用者の視点で説明する。現在のRoom EQ / IR Reverb未対応という記述を更新し、実行時Node.js不要、音声形式、予算と縮小の診断、再読込失敗時の保持を記載する。内部のモデル・ABI・検証方法は既存`pipetune/docs/dsp-backends.md`と必要な`architecture.md`へ反映する。SFZ等の移植対応表・差替え境界は内部文書へ保存する。過去の計画は履歴として残す。

NOTICEと実際の配布ソースを照合する。今回の上流NOTICEの追加にはWASM用Emscripten・muslが含まれるため、ネイティブのみの製品へ一律に転記しない。Rhythmのfdlibm由来コードは`g2_math.h`から`rd6_math.h`へ移っており、引き続き表示対象である。移植した準備処理の出典・ライセンス、デコーダー等の共有ライブラリの配布条件とパッケージ依存を確認する。Node.jsは製品の実行時パッケージ依存へ追加しない。[上流NOTICE](https://github.com/Frieve-A/effetune/blob/v2.13.0/plugins/dsp/NOTICE.txt)、[Rhythmの数学処理](https://github.com/Frieve-A/effetune/blob/v2.13.0/dsp/plugins/analyzer/rhythm_analyzer/rd6_math.h)、[FFmpegのライセンス構成](https://ffmpeg.org/legal.html)

作業ツリーを固定し、Debug全体、クリーンRelease全体、単独ビルド、GTK E2E、一時DESTDIRへのインストール、現行9構成のパッケージ生成・ロード・PCMを確認する。最終完了条件を照合し、実行していないISAと実機未検証範囲を記録する。

完了条件は、SFZ・IR Reverb・Room EQを含む最終条件を満たし、文書と検証記録が揃うこと。コミットは`doc: describe EffeTune 2.13.0 support`。パッケージ実装の修正が必要なら`fix:`または`chore:`として分離する。

## LGPL FFmpegを自前ビルドする設計

PipeTune自身はMITを維持する。GStreamer/avdec_aacはシステムFFmpegのGPL構成を自動的に解消せず、調査では圧縮音声の時間軸と破損時の扱いにも差があったため採用しない。既存のメモリ入力アダプターを保ち、その依存を自前FFmpegへ置き換える。

- 公式ミラー`https://github.com/FFmpeg/FFmpeg.git`を`deps/ffmpeg`サブモジュールに追加する。既存API系列の公式修正版`n6.1.6`（`f1e3a2bf7a2f2cde936d1ed97f09a26853d20125`）を固定し、上流ソースは変更しない。更新時は同じライセンス・音声・配布試験を実行する。
- FFmpeg標準のconfigure/Makefileをビルドディレクトリーから実行する。GPL、version3、nonfree、外部ライブラリの自動検出、ネットワーク、実行プログラム、デバイス、フィルター、映像変換を無効にする。必要なlibavformat、libavcodec、libavutil、libswresampleを共有ライブラリとして生成する。静的リンクやシステムFFmpegへ戻る選択肢を作らない。
- デマルチプレクサーは現行と同じWAV / AIFF / FLAC / MP3 / Ogg / MOV系列に限定する。PCM各形式、FLAC、MP3、Vorbis、AAC、ALAC、Opusなど現行コンテナで必要な音声デコーダーとパーサーを明示する。試験用エンコードには既存のffmpegコマンドを使えるが、製品へリンク・同梱しない。
- FFmpeg標準のbuild-suffixでライブラリ名・SONAMEをPipeTune専用にし、システムに同じABIのGPL版が存在しても混在しないようにする。製品用の私有ライブラリディレクトリーと相対RUNPATHを設定する。専用ライブラリがなければ起動に失敗し、通常名のGPLライブラリへフォールバックしない。利用者は互換な改変LGPLライブラリへ差し替えられる。
- ビルドした各ライブラリのlicense/configuration公開APIとELFの実参照先を検証する。`LGPL version 2.1 or later`であること、GPL/nonfree/version3が無効であること、通常名のシステムlibav*を参照しないことを合格条件にする。CMakeがpkg-configでシステムFFmpegを探す構成は採用しない。
- 配布物には共有ライブラリだけでなく、対応するFFmpegソース、ライセンス全文、著作権情報、固定コミット、configure条件、再ビルド・差替えの手順を含める。ソースアーカイブは固定submoduleから生成し、ビルドスクリプトも添付する。FFmpeg全ソース中に含まれる未使用GPLファイルのライセンスと、実際にビルドするLGPLライブラリのライセンスを区別する。
- 既存Makefile、単独ビルド、インストール・アンインストール、Debian/Ubuntuの9構成へ同じ依存経路を適用する。配布用前提からlibav*-dev / libswresample-devを除き、x86アセンブリに必要なnasmを加える。共有ライブラリの依存は同梱分を重複したシステム依存にせず、その先のlibc等を正しく算出する。
- SFZのJS由来C++準備処理とIR/Room EQの純粋計算は変更せず、従来の分離境界を維持する。cardioによるファイルI/O・cache・取消も維持する。デコーダー版は既存cacheキーに反映される。

根拠: [公式ソースと保守リリース](https://ffmpeg.org/download.html)、[LGPL構成と配布条件](https://ffmpeg.org/legal.html)、[configureオプション](https://github.com/FFmpeg/FFmpeg/blob/n6.1.6/configure)、[ビルド手順](https://ffmpeg.org/platform.html)。

## 検証方法と実施条件

基本の入口は既存Makefileを使う。実装の最終段階で全体テストを省略しない。

```sh
make test
cmake -S . -B build/effetune-2.13-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build/effetune-2.13-release --parallel
ctest --test-dir build/effetune-2.13-release --output-on-failure
DESTDIR="$PWD/artifacts/verification/effetune-2.13-lgpl/install" cmake --install build/effetune-2.13-release --prefix /usr
./build_package_all.sh
```

- 現在の`build_package.sh`はDebian trixieのx86_64 / i686 / arm64 / armv7l / riscv64、Ubuntu 24.04 / 26.04のx86_64 / arm64の9構成。前回計画の13構成を流用してbookwormを復活させない。
- ホスト全体テストには上流native、全DSPパッカー、上流native parity、独自共有バックエンドgolden、単独ビルド、インストール、GTK E2Eを含める。新規nativeテストとモデル検証のCTest登録を確認する。
- 新しい公式golden、既存の更新PCM、製品プリセットの挙動をそれぞれ記録する。goldenは各メタデータのフレーム数・パラメータ・イベント・許容誤差に従い、今回のために参照PCMを再生成しない。
- 各配布先でScalarと実行可能SIMDをロードし、Adaptive・Cassette・Rhythm、SFZ・IR Reverb・Room EQと既存の外部アセット処理を検証する。デコーダーを含む必要な共有ライブラリと対応形式も配布物から確認する。前回記録にあるi686の旧golden差は今回も更新前後比較で切り分け、新しい失敗を既知の差として処理しない。
- SFZ・IR準備・Room EQ設計は同一入力による上流JSとの比較、実ファイルの参照解決・デコード、実共有バックエンドのPCMを分けて検証する。JSランナーは開発テスト専用とし、Node.jsなしの製品試験を別に実行する。
- 長い学習・拍検出は処理フレーム数で時間を進める。エミュレーションを含む実測速度からタイムアウトを設定し、時間がかかることを理由に検証を省略しない。
- GTKの既存表示とE2E基盤を使う。ブラウザを介する試行が必要になった場合はPlaywright MCPを使い、UIの時間軸の問題が見つかった場合は動画による検証を追加する。画像マスターを変更する場合は目視確認する。
- 追加パッケージが不足している場合はインストールを促す。新規のTypeScript / JavaScriptツールはnpm、TypeScript、Vite、Vitest、`prettier-max`、`resolved-killer`のプロジェクト規則に従う。C++側の新規I/Oが必要なら`cardio`の非同期処理を使う。外部APIは公式文書とAPIコメントの両方を確認する。
- 版管理は既存の`screw-up format`経路を維持する。コミット後の検証では再構成し、全体テスト中にソースを変更しない。計画外の問題は設計前提を見直して本書へ反映し、テストと計画の契約が矛盾する場合は確認する。
- 機能削除が必要になった場合は、削除を確認する一時テストでRED、削除後GREENとコミット、その後に一時テストを削除する手順を守る。単なる参照版の更新と製品機能の削除を混同しない。

実行コマンド、対象コミット、環境、ログ、誤差、負荷、配布物ハッシュは`artifacts/verification/effetune-2.13-lgpl/`へ保存し、本書へ工程ごとの結果を追記する。

## 実装の最終完了条件

- [ ] 指定の公式アプリタグ`v2.13.0`を参照し、外部ソースを編集していない。
- [ ] CLI / GTKの表示が2.13.0で、実行可能な各バックエンドの112カーネル・パラメータ・容量が一致する。
- [ ] 新しいコア、HarmNet、3モデルの埋め込み、ARM生成物が単独ビルドと配布物で利用できる。
- [ ] Adaptiveの学習・音声・1〜2ch制約・reset・休止を製品経路で確認し、17 goldenが成功する。
- [ ] Cassetteの5モードと18 golden、Rhythmの6 goldenと長時間クリック・非対応レート・無音後の再検出が成功する。
- [ ] SFZはA＋C＋キャッシュで登録済み音源を再生し、上流JSの解析・容量選別・バンク生成との適合と公式11 goldenを満たす。
- [ ] SFZのJS由来C++処理をホストから分離し、独立ビルド・メモリ入力のドライバー・適合テスト・移植対応表が揃い、差替え境界をレビューした。
- [ ] IR Reverbは単一 / L・Rペア原音、各ch・レート・加工モードと容量制約を反映し、期待するwet音声を出力する。
- [ ] Room EQは共有 / ch別・複数点測定から3位相モードの補正FIRを設計し、周波数応答・位相・遅延と診断が上流比較を満たす。
- [ ] 9種類のアセットDSPに未対応を理由とした除外がなく、無効ノードを読み込まず、未設定・部分対応警告・読込失敗を状態表示で区別する。
- [ ] 原本変更・キャッシュ破損・欠損復旧・準備中の切替を扱い、失敗時は旧パイプラインを保ち、音声コールバックで準備処理をしない。
- [ ] Node.jsとffmpeg実行ファイルがない配布環境で、空キャッシュからSFZ / IR / Room EQの初回準備・再生成・再生が成功する。
- [ ] 解析9種類の除外、既存DSP、生成FIR、遅延補償、複数出力、再構築・失敗時の保持に退行がない。
- [ ] Debug / Release全体、単独ビルド、GTK E2E、インストール、現行9配布構成の結果を記録した。
- [ ] 利用者文書、著作権表示、性能・メモリの測定条件と未検証範囲が揃っている。

- [ ] 自前FFmpegが固定submoduleから再現でき、4共有ライブラリすべてがLGPL-2.1-or-later構成である。
- [ ] ビルド・インストール・全9配布物の実参照先が私有FFmpegであり、システムGPL版へリンクしない。
- [ ] LGPLライセンス・正確な対応ソース・構成・差替え手順を配布物へ同梱し、既存の音声・アセット・製品試験を維持した。

## 計画段階の確認

- [ ] 追従先を利用者指定のアプリタグに固定した。
- [ ] 現行コード、公式タグ間差分、対象版の文書・APIコメントを基に対応箇所を整理した。
- [ ] 各工程・小段階に実行可能な成果物、TDD・回帰検証、コミット、完了条件を定義した。
- [ ] SFZの3案とNode.js依存の議論を保存し、A＋C＋キャッシュに確定した。
- [ ] SFZのJS由来処理の構造的分離、移植元との適合テスト、将来の上流C++への差替え方針を定義した。
- [ ] アセットを持つ9種類を洗い出し、IR Reverb・Room EQの未対応を今回解消する工程を追加した。
- [ ] 共通のネイティブ読込・キャッシュ・監視と、初回準備を含めた実行時Node.js不要の条件を定義した。

計画作成時は本書のみを更新した。ビルドに影響しない文書変更のため、計画段階のビルド・テストは行っていない。上記の計画段階の条件は差分レビューで確認した。

## LGPL再構成の実施結果

開始時にタグ`ffmpeg`が`38c09d7`を保持し、作業ツリー・既存サブモジュールがクリーンであることを確認した。利用者の指示に従いdevelopを`b2a4911`へ戻した。ホストのnasmが未導入のため導入を依頼し、依存しない工程を先行する。

### アプリ更新・既存DSPの再構成

旧履歴の工程0〜3を機能単位で再利用した。EffeTuneの参照はアプリv2.13.0へ更新し、システムFFmpegを参照する設定は含めていない。クリーンなDebugビルドと、バックエンド・Adaptive・Cassette・Rhythm・パッカー等の対象10試験が成功した（263.29秒、`backends-build.log`、`backends-tests.log`）。
