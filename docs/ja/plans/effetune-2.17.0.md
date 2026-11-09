# EffeTune 2.17.0対応計画

## 目的と対象範囲

PipeTuneが使うEffeTuneを2.16.0から2.17.0へ更新する。既存DSPとSFZ音源を新しいエンジンで使えるようにし、製品の音声経路、メモリ使用量、配布物まで検証する。

利用者との確認により、MIDI入力・演奏機能とSF2/SF3音源の読込は別計画とする。今回の音源対応は、現在のPipeTuneが扱うSFZ音源の更新を完了点とする。2.17.0で追加された音源のストリーミングや、SFZスクリプト・内蔵エフェクトなどの全面移植も含めない。現在の全量読込を維持し、対応していない音源機能は診断で区別する。

上流では、入力音から音符を検出する「SFZ Note Player」が「SoundFont Note Player」へ改名された。本書では、この既存DSPを「Note Player」と呼ぶ。一方、新設された「SoundFont Player」はMIDIイベントで演奏する別のDSPである。名前が似ているため、カタログへの登録とPipeTuneでの利用可否を分けて扱う。[公式プラグインガイド](https://github.com/Frieve-A/effetune/blob/v2.17.0/docs/i18n/ja/plugins/others.md)

DSD、MIDIファイル、MusicXMLの再生、Visualizerの新表示、ネットワーク再生は今回の対象に含めない。これらはEffeTuneアプリ側の機能であり、現在のPipeWire音声入力を更新するためには不要である。[公式リリース](https://github.com/Frieve-A/effetune/releases/tag/v2.17.0)

2026-11-10の利用者回答により、SFZの容量超過はEffeTune 2.17.0と同じ読込拒否へ揃える。従来の自動レイヤー削減は削除する。PipeWireの既存問題と原因未判明の失敗も今回調査・修正し、独立した修正はコミットを分ける。以下の実施記録にある回答待ちは、この決定で解消した。

## 調査の基準

調査日は2026-11-09。PipeTuneの基準は`develop`の`f491ddce092a4ee70bdbd7351f43bc1c75a30618`で、調査開始時の作業ツリーはクリーンだった。計画を保存するブランチは`feature/effetune-2.17.0`とする。

| 項目 | 現行 | 更新先 |
| --- | --- | --- |
| 公式アプリタグ | `v2.16.0` | `v2.17.0` |
| 公式コミット | `3769b9aefebf2dce2de3169690d4a95e1716da4c` | `d026b1664ec9444a66297efd36e251bc8aae3d05` |
| DSPのCMakeプロジェクト版 | 1.16.0 | 1.16.0 |
| DSPのJSバインディング版 | 0.16.0 | 0.17.0 |
| DSPの登録カーネル数 | 112 | 113 |
| `EFFETUNE_DSP_ABI_VERSION` | 1 | 1 |

ローカルタグと公式リモートのコミットが一致することを確認した。製品の版表示は、従来どおりEffeTuneの`package.json`から取得する。DSPのCMake版は今回も1.16.0なので、これを2.17.0の判定に使わない。[タグ間差分](https://github.com/Frieve-A/effetune/compare/v2.16.0...v2.17.0)、[アプリ版](https://github.com/Frieve-A/effetune/blob/v2.17.0/package.json)、[DSPのCMake定義](https://github.com/Frieve-A/effetune/blob/v2.17.0/dsp/CMakeLists.txt)

## 現行コードへの影響

### 表示名とパラメータ

| DSP | 現行 | 2.17.0と対応方針 |
| --- | --- | --- |
| `SFZNotePlayerPlugin` | 表示名はSFZ Note Player。20 float、`0xdfb4bbc5` | SoundFont Note Playerへ改名。21 float、`0x3785c6ed`。音色選択の`sp0`が追加され、既定値は-1 |
| `SoundFontPlayerPlugin` | 未登録 | 新規登録。4 float、`0x844a0998`。MIDI演奏機能は今回の対象外として診断する |
| `RhythmAnalyzerPlugin` | 4 float、`0xf854c585` | 契約は同じ。ドラム検出モデルの更新を検証する |
| `NoteSpectrogramPlugin` | 4 float、`0xb8518943` | 契約は同じ。音色判定モデルと共有解析への影響を検証する |

Note PlayerとSoundFont Playerのアセット容量は、ともにslot 0の1 GiBである。生成カタログには113カーネルを取り込むが、それだけで113種類すべての製品機能に対応したとは扱わない。[公式メタデータ](https://github.com/Frieve-A/effetune/blob/v2.17.0/plugins/dsp/effetune-dsp.meta.json)、[Note Playerのパラメータ](https://github.com/Frieve-A/effetune/blob/v2.17.0/dsp/plugins/others/sfz_note_player/params.json)、[SoundFont Playerのパラメータ](https://github.com/Frieve-A/effetune/blob/v2.17.0/dsp/plugins/others/soundfont_player/params.json)

現在の`dsp_pipeline.cpp`は、アセット読込、チャンネル構成、無音時の動作を表示名`SFZ Note Player`で判定している。生成カタログだけ更新すると、この分岐に入らなくなる。Note Playerの判定を安定した型名`SFZNotePlayerPlugin`へ揃え、2.17.0の表示名で保存したプリセットを製品経路へ通す。旧表示名への別名対応は追加しない。現行版を表すテスト入力も新しい表示名へ更新する。[上流の表示名変更](https://github.com/Frieve-A/effetune/blob/v2.17.0/plugins/plugins.txt)

SoundFont Playerを有効にしたプリセットは、MIDI演奏が未対応であることを警告し、そのノードを除外する。同じプリセットの対応済みDSPは処理を続ける。表示専用DSPとして登録したり、音源不足だけが理由であるかのように案内したりしない。無効ノードと無効なSectionは既存の扱いを保つ。

### 共有DSPバックエンドと公開API

PipeTuneは`EffeTuneNativeBackends.cmake`で共有ライブラリのソースを独自に列挙している。上流に追加された`plugins/others/sfz/sfz_effect_cost_bridge.cpp`は`kernel.cpp`の収集では取り込めないため、明示的に追加する。SoundFont Playerにも上流と同じ`-ffp-contract=off`を適用する。i686の浮動小数点設定とARMの木モデル変換も、新しいカーネルとモデルに対して確認する。[上流のビルド定義](https://github.com/Frieve-A/effetune/blob/v2.17.0/dsp/CMakeLists.txt)

上流のパラメータ生成器は、SFZ変調と内蔵エフェクトのスキーマ生成器を新たにimportする。PipeTuneのカタログ生成に必要な入力とCMakeの依存関係を照合する。生成物はビルドディレクトリへ置き、サブモジュール内の生成済みファイルを書き換えない。[上流の生成器](https://github.com/Frieve-A/effetune/blob/v2.17.0/scripts/gen-dsp-params.mjs)

2.17.0では、64 bit環境でもネイティブポインタを返す`et_instance_asset_begin_ptr`が公開された。現在の`effetune_backend_asset_bridge.cpp`は、パッチで取り出した内部Engineを直接操作している。コピー用のPipeTune APIは維持し、公開APIでbeginとcommitを行う。その間でバイト列をコピーする。失敗時のabortと状態保持も確認する。[公式のネイティブホスト説明](https://github.com/Frieve-A/effetune/blob/v2.17.0/dsp/README.md#abi-contract)、[API宣言とコメント](https://github.com/Frieve-A/effetune/blob/v2.17.0/dsp/include/effetune/abi.h)

移行後は`abi-engine-access.patch`、`effetune_backend_engine_access.*`、ABIを加工するビルド工程を削除する。旧`et_instance_asset_begin`はWASM32向けであり、native 64 bitでは失敗する。PipeTune側に不要となるローダー項目やexportも削除する。`App-internal`と明記された解析共有・ストリーム境界APIや、内部用のoffline graph APIは追加利用しない。

ABI番号1は、版をまたぐネイティブバイナリ互換の保証ではない。ヘッダー、共有バックエンド、製品を同じ公式コミットから再ビルドし、既存のカタログ照合も維持する。[上流のABIとメモリ規則](https://github.com/Frieve-A/effetune/blob/v2.17.0/dsp/README.md)

### SFZバンク形式とメモリ

バンクは、SFZの定義と音声サンプルをDSPへ渡すためにまとめたデータである。現在の`asset-preparation/sfz/bank.cpp`は、ヘッダー8 word、region 30 wordのSFZ table v2を生成する。2.17.0のカーネルはこの形式を受け付けない。

今回の基礎形式には、全量読込に使えるSFZ table v6を採用する。ヘッダーは12 word、regionは113 wordになる。変調付きv7、ストリーム用v8、SF2用v9、保存形式を指定するv10も存在するが、数字だけを最新のv10へ変えない。上流の定義に従ってフィールド、既定値、整数の格納方法、可変領域を構成する。[公式パッカー](https://github.com/Frieve-A/effetune/blob/v2.17.0/js/sfz/asset.js)、[バンク定義](https://github.com/Frieve-A/effetune/blob/v2.17.0/dsp/plugins/others/sfz_note_player/bank.h)、[受入側の検証](https://github.com/Frieve-A/effetune/blob/v2.17.0/dsp/plugins/others/sfz/bank.h)

ネイティブ側の追加メモリは、新しい`et_sample_player_preparation_bytes`で取得する。完全な制御メタデータ、payload全体の長さ、処理レート、最大ブロック長、実際の処理チャンネル数を渡す。Note Playerには`ET_SAMPLE_PLAYER_NOTE`を指定する。必要量はpayload長と取得した追加バイト数の和であり、現在のインデックス計算を足して二重計上しない。

MIDIのテンポ情報を持たない今回の構成では、見積もりとアップロードに同じ`minimum_physical_bpm=0`を使う。これは上流が定義した120 BPMの代替値を選ぶ指定である。これらのAPIは音声処理中に呼び出さず、候補パイプラインを準備する側で処理する。APIの戻り値は64 bitで計算し、1 GiB上限と32 bitフィールドへの変換を確認する。[見積もりとアップロードの契約](https://github.com/Frieve-A/effetune/blob/v2.17.0/dsp/README.md#native-midi-sample-player-hosts)、[APIコメント](https://github.com/Frieve-A/effetune/blob/v2.17.0/dsp/include/effetune/abi.h)

追加メモリの値は、実際に使うバックエンドと処理条件から求める。アーキテクチャ依存の値を、そのまま共通キャッシュの正解として保存しない。デコード中の一時領域、固定のinstance準備領域、転送時のコピー、稼働中と候補の共存は、このAPIの値だけでは表せない。既存のアセット全体のメモリ予算へ、それぞれを含める。

現在のキャッシュ識別子には`sfz-v2/ETA1-table-v2`と旧出典が入っている。今回は形式が実際に変わるため識別子を更新し、旧キャッシュは再利用せず元音源から再生成する。旧形式の変換器は作らない。新しいキャッシュでも、ロード時に実バックエンドの必要量を再確認する。IRとRoom EQの識別子は、EffeTuneの版が上がったという理由だけでは変えない。

### SFZの読込範囲と登録一覧

今回維持する機能は、現行パーサーとテストが扱うinclude・define、ARIAバンク、鍵域・ベロシティ・ランダム・順番によるサンプル選択、音高・音量・pan、再生位置・loop、音量エンベロープ、生成音源である。受け付ける各機能の既定値と音声への効果を2.17.0と比較する。

新たなSFZ機能を古いパーサーで無視すると、上流と異なる音になる可能性がある。対応範囲外のスクリプト、変調、エフェクト、埋込みサンプル、特殊な保存指定などは、対象機能を診断して読込を拒否する。既存の音源機能の意味が変わった場合は、対応するPCMテストから修正する。参考実装が受け付けたというだけで全面対応とは説明しない。[SFZ対応範囲の公式記録](https://github.com/Frieve-A/effetune/blob/v2.17.0/specifications/sfz/README.md)、[上流パーサー](https://github.com/Frieve-A/effetune/blob/v2.17.0/js/sfz/parser.js)

EffeTuneの`sfz-references.json`には、2.17.0からSFZとSF2/SF3が混在する。現行`referenceFromRegistry()`は、選択していない項目でも拡張子が`.sfz`以外なら一覧全体を拒否する。有効なSF2/SF3登録が混ざっていても、選択したSFZは読み込めるようにする。選択した音源がSF2/SF3なら、未対応形式として候補の適用を拒否し、稼働中の音声を保つ。登録一覧のIDとパスの検証は維持し、EffeTuneの登録情報を書き換えない。[登録一覧の公式実装](https://github.com/Frieve-A/effetune/blob/v2.17.0/electron/sfz-library-ipc.js)

### その他のDSPと参照音声

RhythmのドラムモデルとNote Spectrogramの音色判定モデルが更新されている。SFZ Eventが使う共有解析への影響も含め、上流fixtureと製品PCMを確認する。表示専用DSPの除外は維持し、Rhythmのクリック音は引き続き実行する。[Rhythmモデル](https://github.com/Frieve-A/effetune/blob/v2.17.0/dsp/plugins/analyzer/rhythm_analyzer/drum_weights.provenance.json)、[音色判定モデル](https://github.com/Frieve-A/effetune/blob/v2.17.0/dsp/plugins/analyzer/note_spectrogram/timbre_weights.provenance.json)

公式goldenのPCM差分はSFZ Note Playerの`case-002`、`case-007`、`case-008`である。新しい公式PCMを比較基準に使い、他の10件も含む13件を検証する。PipeTuneの結果を正解として参照音声を作り直したり、許容誤差を一括で広げたりしない。[公式SFZ参照音声](https://github.com/Frieve-A/effetune/tree/v2.17.0/dsp/plugins/others/sfz_note_player/golden)

Chorus、FDN Reverb、平滑化、FIR・畳込み処理にも変更がある。長い減衰、極小値、有限PCM、遅延、resetを回帰確認する。x86では、DSP呼出し後に浮動小数点制御レジスタMXCSRを復元するようになった。正常終了とエラー終了の両方で、呼出し元の設定を保持することを実共有バックエンドで確認する。[エンジン変更](https://github.com/Frieve-A/effetune/blob/v2.17.0/dsp/core/engine.cpp)、[DSP差分](https://github.com/Frieve-A/effetune/compare/v2.16.0...v2.17.0)

Room EQ設計にはタグ間差分がない。測定データ保存の変更は設定保存先の置換が中心である。IRライブラリにはキャッシュ操作の変更があるが、PipeTuneのキャッシュを上流アプリの保存実装へ置き換える必要はない。公開アセットAPIへの移行と畳込み処理の影響を、既存のRoom・IR・Crosstalkの音声試験で確認する。[測定データの実装](https://github.com/Frieve-A/effetune/blob/v2.17.0/features/measurement/dataStorage.js)、[上流IRキャッシュ](https://github.com/Frieve-A/effetune/blob/v2.17.0/js/ir-library/ir-pcm-cache.js)

## 段階的な実施計画

各工程でビルドと対象テストを行い、音声ドライバーまたは製品で結果を観測できる状態を作る。コード変更は問題を再現するテストでREDを確認し、修正後にGREENを確認してコミットする。工程1は非SFZの音声処理、工程2はSFZ再生、工程3は製品の音源読込までを成果物とする。更新全体の完了は工程5で判定する。

削除する機能や統合処理には、削除を確認する一時テストを先に追加する。RED、削除後のGREEN、削除コミットの順に進め、その後で一時テスト自体を削除して別コミットにする。音声などの恒久的な動作テストは残す。

計画にない問題が見つかった場合は、原因と計画への影響を整理してから工程を修正する。試行中の不要な修正は取り除く。仕様とテストの要求が矛盾する場合は、どちらへ合わせるか確認する。依存ソースとvendorは編集せず、mainへのマージも行わない。

### 工程0 現行版の基準を記録する

基準コミットの2.16.0でDebugの全体テストを実行する。Releaseでも比較用バイナリを作り、コンパイラ、実行可能なSIMD、Podman、メモリ・ディスクを記録する。管理者権限が必要な不足パッケージは、名称と導入方法を案内する。

同じSFZ音源を使い、Classic/Eventの発音、遅延、初期化時間、処理時間、最大RSSを保存する。Rhythmの表示方式とクリックの有無、ChorusとFDN Reverbの減衰も比較用に記録する。性能測定はReleaseで48/96/384 kHzを基本とし、CPU、ブロック長、入力、処理フレーム数を固定する。ビルドや他の負荷試験とは重ねない。

既存SFZキャッシュを隔離して保存する。更新後は形式変更を検出し、元音源から再生成できることを確認する。登録情報と元音源のハッシュも記録する。

成果物は基準テスト結果、比較用PCM、性能条件、旧キャッシュである。完了条件は、既存の失敗と更新後の失敗を区別でき、同じ入力と環境で比較を再現できること。記録は次の実装コミットと対応付ける。

### 工程1 2.17.0の共有DSPと公開アセットAPIを使えるようにする

現行スモークを`effetune_217_test.cpp`へ更新し、版、113カーネル、新しいパラメータ契約を要求して2.16.0でREDを確認する。公式コミットへ参照を更新し、生成カタログ、追加ソース、コンパイル設定を揃える。`sp0`の省略、上下限、不正値を上流のパッカーと比較する。

公開アセットAPIだけでIRを転送し、インパルス応答が得られるテストを用意する。コピー用ブリッジを公開APIへ移し、必要な見積もりAPIをローダーへ追加する。旧内部フックへの依存をなくす一時テストでRED/GREENを記録し、パッチ、ヘルパー、ABI加工工程、不要なローダー項目を削除する。削除のコミット後に一時テストを除去する。

CLI・GTKの版表示、Matrix、Rhythm、IR、Room EQなどの対象試験を実行する。Scalarと実行可能なSIMDで共有ライブラリをロードし、i686・armv7lも先行ビルドする。上流のSFZ・SoundFontを含むnativeテストは実行対象に残す。これはDSPライブラリの検証であり、MIDI機能を製品へ追加する工程ではない。

成果物は、非SFZの音声処理と公開APIによるアセット転送を実行できる2.17.0ビルドである。完了条件は、版と契約が一致し、対象PCMが成功し、内部アクセス用パッチが不要となること。新しい警告にも対処する。SFZの旧バンク形式による未完了項目は工程2へ明示して引き継ぐ。

コミットは`feat: integrate EffeTune 2.17 native backends`、削除は`refactor: use public EffeTune asset APIs`、一時テストの除去は`chore: remove temporary asset bridge removal checks`に分ける。

### 工程2 既存SFZ音源を新しいNote Playerで再生する

小さな登録SFZを新しい表示名のプリセットで読み込み、Classic/EventのPCMを検証するテストを先に追加する。旧表示名による分岐と旧バンク形式の問題をREDで確認する。型名での判定、v6のパッキング、実バックエンドでのメモリ見積もりを実装し、同じ音源が再生できるところまで進める。

パッカーはC++の独立アセット準備として維持する。実行時にNode.jsを追加しない。可搬なバンク内容は公式JSと比較し、ネイティブに依存する必要メモリは公開APIで別に確認する。バイト列の比較だけで完了とせず、既存のSFZ再生ドライバーで発音、dry/wet、loop、resetを確認する。

21 floatの新契約、1 GiB上限、実際の処理チャンネル数、見積もりの失敗と整数境界を確認する。2.15対応で導入したEventの初回発音用初期化は、2.17.0での音声テストを根拠に扱う。不要と判断して削除する場合は、前述の削除手順に従う。

成果物は、新名称のプリセットから既存SFZを読み込んで音を出せる製品と独立ドライバーである。完了条件は、Scalarと実行可能なSIMDでClassic/Eventが発音し、遅延とresetが成立すること。公式SFZ参照PCM13件も検証する。

コミットは`feat: adapt SFZ preparation to EffeTune 2.17`とする。

### 工程3 音源読込とキャッシュを製品の契約へ揃える

現行SFZの機能ごとに、音源選択と音声への効果を2.17.0と比較する。未対応宣言を含む音源は診断して拒否する。既存機能の意味が変わった箇所は、再現テストから必要な修正を行う。SFZ全機能の移植へは拡大しない。

容量不足時に代表レイヤーへ縮小する機能は、利用者の決定に従って削除する。削除を確認する一時テストでREDを示し、全領域と元ファイルの容量を確認して超過時に全体を拒否する実装へ変更する。新形式の容量見積もり、実バックエンドの追加必要量、上限ちょうどと1 byte不足、稼働中音声の保持を恒久テストで確認する。GREENと削除のコミット後に一時テストを除去する。削減結果の型、診断、旧形式の容量計算、削減音源の共有を前提としたfixtureも整理する。

SFZ・SF2・SF3が混ざる登録一覧から、選択したSFZだけを読み込むケースを追加する。SF2/SF3を直接選択した場合、未登録ID、欠損・変更された音源、読込取消も検証する。SoundFont Playerは未対応のMIDIノードとして警告し、他のDSPを処理できることを確認する。

キャッシュ識別子を更新し、旧バンクを読まずに再生成する。新形式の冷キャッシュと温キャッシュで同じPCMが得られることを確認する。バックエンド、レート、最大ブロック長、チャンネル数を変更した場合も必要量を再計算する。準備中の一時領域と候補の共存を含め、設定済みのメモリ予算を超えて確保しない。

容量超過、破損、取消で候補の一部を公開しないことを、稼働中のPCMと状態で検証する。有効な音源へ戻した後の回復、依存ファイル監視、キャッシュの破損・書込失敗も既存テストで確認する。元の音源とEffeTuneの登録一覧を変更しない。

成果物は、混在した登録一覧と新形式キャッシュを扱える音源ローダーである。完了条件は、対応SFZが再生でき、未対応形式を区別でき、失敗や取消でも稼働中のパイプラインを保てること。メモリ境界と回復のテストが成功し、実行時にNode.jsやffmpegコマンドを必要としないことも確認する。

コミットは`fix: handle EffeTune 2.17 instrument references`と`fix: account for native SFZ preparation and cache changes`に分ける。

### 工程4 製品の音声動作と性能を検証する

Note Playerは44.1/48/96/192/384 kHz、部分ブロック、選択ペア、16チャンネルを代表条件と境界条件に分けて試験する。初回入力、同音の再発音、プレビュー、無音休止と再開、プリセット再読込、レートとバックエンドの再構築を含める。dry/wetの位置と製品の報告遅延を実PCMで照合する。

Rhythmは`dm=band/drums`とクリック有効・無効を確認する。クリック無効時の入力保持と、有効時の選択チャンネルへの出力を検証する。新しいドラム・音色判定モデルは上流テストで検証し、Note Spectrogramの表示専用としての除外も維持する。

Chorus、FDN Reverb、FIR、Room EQ、IR、Crosstalkの音声と遅延を確認する。MXCSR復元は正常・エラーの両経路で検証する。長い減衰と極小値の処理を含め、有限PCMと処理負荷を確認する。

製品経路では、Matrix reset、Processed/Direct、単一・複数出力、音量、遅延補償を確認する。既存の実時間・freewheel試験、出力再接続、音声レートの不一致からの回復、通常のWirePlumberポリシーとの共存を全体回帰へ残す。時間待ちを成功条件にせず、接続状態、処理済みフレーム、PCMの連続性から判定する。

既存の入力再開時の古いPCM、複数出力起動時のバッファ差、原因未判明の時間整列と出力プロファイル復元を対象に、追跡記録と再現テストで原因を特定する。原因が独立していれば修正コミットを分ける。EffeTune更新と切り離せない場合は統合して修正する。反復の一時的な成功や許容誤差の拡大を解消の根拠にせず、根本原因を抑える検証を加える。

工程0と同じRelease条件で準備時間、平均・上位分位・最大処理時間、RSS、遅延、共有ライブラリのサイズを比較する。必要な処理周期を超える、音切れが出る、メモリが継続的に増える場合は原因を調べる。上流の音声仕様による差とPipeTune側の退行を区別し、検証の許容誤差やメモリ上限を便宜的に広げない。QEMUの結果を実機の性能保証には使わない。

成果物は、製品の音声動作の回帰テストと更新前後の性能比較である。完了条件は、対象プリセットで音声・遅延・休止再開・出力経路が成立し、観測した負荷とメモリの差を説明できること。問題があれば原因を再現するテストと最小修正を揃える。

コミットは`feat: verify EffeTune 2.17 audio behavior`とし、独立した不具合修正は`fix:`で分ける。

### 工程5 全体回帰と配布を完了する

最終コードでDebug/Releaseの全体テストを実行する。コア単独ビルド、独立アセット準備、GTK E2E、導入レイアウト、公式参照PCMを含める。最後のコード変更後に全体テストを実行し、個別成功で代用しない。タイムアウト時は処理量と進行条件を調べ、単なる待機時間の追加で解消したことにしない。

既存の9配布構成を生成する。対象はDebian trixieのx86_64・i686・arm64・armv7l・riscv64と、Ubuntu 24.04/26.04のx86_64・arm64である。導入先の実共有ライブラリで版、113カーネル、新しい契約、Note PlayerのClassic/Event、IR、MatrixのPCMを検証する。実行できないSIMDはCPUの制約とともに記録し、ビルド成功と実行成功を区別する。

READMEと関連文書を日本語原本から更新し、英語へ反映する。EffeTune 2.17.0、SoundFont Note Playerの名称、今回扱うSFZの範囲、MIDIとSF2/SF3の未対応を利用者の視点で説明する。移植元の版とURLは実際に対応したコードに合わせる。日本語は`nanaism/yomiyasu`で確認し、一般文書から計画書へリンクしない。

成果物は、検証済みの配布物、利用者向け文書、最終検証記録である。完了条件は、次の最終完了条件をすべて満たし、配布物が検証した最終ソースから作られていること。新たな警告は原因と対処を記録する。

コミットは`doc: describe EffeTune 2.17 support`とし、必要な配布設定の変更は`fix:`または`chore:`で分ける。

## 主な変更・検証箇所

| 箇所 | 作業 |
| --- | --- |
| `deps/effetune` | 公式2.17.0コミットへ参照更新 |
| `pipetune/CMakeLists.txt`、`pipetune/cmake/EffeTuneNativeBackends.cmake` | 追加ソース、API、全バックエンド、モデル、生成依存、テスト登録 |
| `pipetune/src/effetune_backend_asset_bridge.cpp`、`dsp_backend.cpp`、`dsp_backend_loader.h`、`effetune_backend_exports.map` | 公開APIへの移行と必要メモリの取得 |
| `pipetune/patches/effetune/abi-engine-access.patch`、`pipetune/src/effetune_backend_engine_access.*` | 不要になる内部フックの削除 |
| `pipetune/src/dsp_pipeline.cpp`、`pipetune/tools/generate-dsp-catalog.mjs` | 新名称、型名による判定、113カーネル、未対応MIDIノードの扱い |
| `pipetune/asset-preparation/sfz/` | 現行対応範囲のSFZ意味論、v6バンク、独立した準備処理 |
| `pipetune/src/sfz_asset_loader.cpp`、`asset_cache.cpp`、アセット予算関連 | 登録一覧、キャッシュ無効化、実バックエンドの必要量と候補の共存 |
| `pipetune/test/effetune_216_test.cpp`、`effetune_backend_artifact_test.cpp`、`parameter-packer-parity.mjs` | 2.17契約、公開API、実共有バックエンドと公式参照音声 |
| `pipetune/asset-preparation/test/`、SFZ・プリセット・アセット・PipeWire関連テスト | 発音、読込失敗、取消、回復、チャンネルと遅延、製品回帰 |
| `README_ja.md`、`README.md`、`docs/ja/`、`docs/en/`の該当文書 | 対応版、名称、SFZの範囲、移植元情報 |

## 検証コマンドと記録

以下は実装時に使用する既存の検証経路である。計画作成時には製品ビルドとテストを実行しない。

```sh
GDK_BACKEND=x11 make test TEST_BUILD_DIR=build/effetune-217-debug
cmake -S . -B build/effetune-217-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build/effetune-217-release --parallel
GDK_BACKEND=x11 ctest --test-dir build/effetune-217-release --output-on-failure
make -C pipetune/asset-preparation test BUILD_DIR=../../build/effetune-217-assets
DESTDIR="$PWD/artifacts/verification/effetune-2.17/final-install" cmake --install build/effetune-217-release --prefix /usr
./build_package_all.sh
```

個別テスト名は構成後に`ctest --test-dir build/effetune-217-debug -N`で確認する。タグ更新後とコミット後にはCMakeを再構成し、版情報を取り直す。新規の補助スクリプトはNode.js、ビルド時のテキスト処理は既存のfuncityを使う。新たな外部APIは公式文書と宣言コメントの両方を確認する。

コミット、コマンド、RED/GREEN、PCM誤差、性能条件と結果、メモリ、警告への対処、配布物のハッシュを`artifacts/verification/effetune-2.17/`へ記録する。各工程の終了時に完了条件と結果を照合する。エミュレーションと実機の記録、未確認と成功の記録を分ける。

## 最終完了条件

- [x] 公式2.17.0コミットを参照し、依存ソースとvendorへの直接変更がない。
- [x] CLI・GTKの版、113カーネル、Note Playerの21 floatと新ハッシュが一致する。
- [x] 公開APIでアセットを転送し、旧内部フックと不要なビルド処理を削除している。
- [ ] 新名称のNote Playerから、今回の対応範囲のSFZをClassic/Eventで再生できる。
- [x] SF2/SF3を含む登録一覧からSFZを選択でき、対象外の形式・機能を診断できる。
- [ ] 新バンク形式、旧キャッシュの再生成、必要メモリの取得、予算超過と取消後の回復を検証している。
- [ ] 公式参照PCM、Rhythm、他の既存DSP、休止再開、単一・複数出力、レート変更に退行がない。
- [ ] 最終コードのDebug/Release全体テスト、単独構成、GTK E2E、インストール、9配布構成を検証している。
- [ ] 性能・メモリ差と警告への対処を説明でき、文書と配布物が実際の対応範囲に一致する。

## 計画作成時の完了確認

現行コードと公式タグ差分を照合し、単純な参照更新では不足する箇所を特定した。表示名、SFZバンク形式、ネイティブAPI、メモリ、登録一覧、参照音声、各配布構成への影響を工程へ反映した。MIDI演奏とSF2/SF3音源は別計画とする利用者の回答も反映している。

各工程に実行可能な成果物、検証内容、完了条件、コミットの区切りを設けた。これを計画作成の完了条件とし、本書で満たしている。計画作成時点では、上記の実装完了条件はすべて未実施だった。計画書の保存はビルドに影響しないため、この時点では製品ビルドと全体テストを省略した。

## 実施記録

### 工程0の結果

`build/plan2-debug`で2.16.0のDebug全280テストを実行した。最初の44件は逐次実行し、残る236件は4並列で実行した。278件が成功し、`pipetune_product-sfz-classic-realtime`と`pipetune_product-sfz-event-realtime`が失敗した。失敗した2件は単独でも再現した。PCMのフレーム順序の不一致を検出しており、並列実行ではClassicのPCM受信タイムアウトも発生した。更新前からの問題として工程4へ引き継ぎ、最終検証の成功条件から除外しない。

Releaseは`build/release`を使い、ビルド警告なしで比較用バイナリを作成した。Scalar、CPU 8への固定、2チャンネル、256フレームで測定した。48/96/384 kHzについてSFZのClassic/Event、Rhythmのband/drumsとクリックの有無、Chorus、FDN Reverbを各3回測定した。1秒以上の予備処理後、512ブロックのPCMと処理時間を保存している。準備時間、平均・95パーセンタイル・最大処理時間、最大RSS、遅延も同じドライバーで取得した。

記録は`artifacts/verification/effetune-2.17/`に置いた。環境情報は`environment.json`、比較結果は`performance-2.16/results.json`、入力のハッシュは`baseline-source-hashes.json`、隔離した旧SFZキャッシュは`old-cache/`にある。全体テストと単独再現のログ、2.17契約と旧内部フックのREDログも保存した。これにより、更新前の失敗を区別し、同じ入力で比較を再現するという工程0の完了条件を満たした。

### 工程1の実装上の調整

公式2.17.0への参照更新後、旧`abi-engine-access.patch`が適用できずビルドが停止した。公開APIだけでリンクする一時テストでは、旧ブリッジが内部Engineを必要とすることも確認した。計画した公開APIへの移行で両方を解消する。

参照更新だけではビルドを成立させられないため、工程1の統合と公開APIへの移行を1つの実装コミットにまとめる。一時テストは削除のGREEN確認とコミットまで残し、その後で別コミットとして除去する。対象機能と工程の完了条件は変更しない。

公開APIのbeginは対象instanceの既存アセットを置き換え、abortはそのslotを消去する。稼働中の音声の保持は、既存どおり別の候補パイプラインを準備してから切り替えることで実現する。実共有ライブラリの試験でも、候補の不正なアセットを拒否・解放した後に既存instanceのPCMを保持できた。[上流のアセット処理](https://github.com/Frieve-A/effetune/blob/v2.17.0/dsp/core/engine.cpp)

i686ではサンプルキャッシュの静的検査が`24 == 20`で失敗した。上流スキーマが要求する8バイト境界に合わせ、GCCの`-malign-double`をDSPの全C++翻訳単位と、それにリンクする上流nativeテストへ適用した。1つのカーネルだけに適用すると共有内部型の配置が食い違うため、翻訳単位間の設定を揃えている。PipeTuneとの境界には、影響する構造体を渡さない。通常のi686設定でコンパイルした呼出し側から、Scalar・SIMD baseline・x86-64-v3の公開API、SFZのPCM、不正な候補の破棄を確認した。[GCCの配置とABIの説明](https://gcc.gnu.org/onlinedocs/gcc/x86-Options.html#index-malign-double)、[上流のサイズ検査](https://github.com/Frieve-A/effetune/blob/v2.17.0/dsp/plugins/others/sfz/sample_cache.h)

工程1のホストビルドは成功し、CLIとGTKがEffeTune DSP 2.17.0を表示した。契約・公開API・非SFZ DSP・上流Note/SoundFont Playerの13テストと、IR・Room EQの177テストが成功した。i686の3バックエンドでも、通常のABIでコンパイルした呼出し側からPCMを確認した。armv7lのScalar・SIMDもビルドでき、QEMU上で公開APIとSFZのPCMを確認した。x86-64-v4は生成できたが、このホストのCPUでは実行対象にしていない。

製品側のSFZは工程2へ引き継ぐ。新表示名では音源読込の分岐へ入らず、Classic/Eventとも有効DSP数が0になることを確認した。既存パッカーと再生ドライバーの13テスト中7件も、公式v6形式との差と旧表示名の参照で失敗している。これらのREDを工程2の開始条件として保存した。

### 工程2の検証状況

型名によるNote Playerの判定とv6バンクへの更新を実装した。独立パッカーと再生の13テストが成功し、公式SFZ参照PCM13件もScalar・SIMD baseline・x86-64-v3で一致した。追加メモリは実バックエンドへ問い合わせ、共通キャッシュのfootprintには可搬なpayload長だけを保存する。

EventのNote Hold試験では、従来の100 msの空白を2.17.0の音符検出が同じ音符としてつなぐため、空白の前後に設定差が現れなかった。最後の音符の終了には設定差があり、Note Hold自体は動作していた。空白を200 msにすると、終了・再発音とNote Holdの差を観測できたため、入力条件を修正した。PCMの許容誤差や成功しきい値は変更していない。[上流のEvent検出とNote Hold](https://github.com/Frieve-A/effetune/blob/v2.17.0/dsp/plugins/others/sfz/engine.h)

製品SFZランタイム試験は100秒で成功した。44.1/48/96/192/384 kHz、1・2・16チャンネル、選択ペア、部分ブロック、Classic/Event、dry/wet、休止再開、reset、再発音、プレビュー、バックエンド再構築を検証した。Eventの初期化処理は維持する。新名称の発音、遅延、resetと公式PCMを確認し、工程2の完了条件を満たした。キャッシュ識別子、固定準備メモリ、旧形式の容量境界テストは工程3で更新する。

### 工程3の音源対応

SF2/SF3の未選択項目を含む登録一覧から、SFZを発音できることを確認した。SF2/SF3を選ぶと未対応形式を明示する。無効IDとルート外パスの検証も維持した。MIDI専用SoundFont Playerは警告して除外し、同じプリセットのVolumeが音量を変えることをPCMで確認した。無効ノードと無効Sectionは警告しない。

SFZのスクリプト、変調、フィルター、エフェクト、埋込みサンプル、ストリーム指定の15ケースは、include内でも読込全体を拒否する。基本領域の値は2.17.0と一致した。初期CCと鍵切替による静的選択は従来の製品動作として別に検証し、新しいMIDI実行時状態の移植とは区別した。

### 工程3のメモリとキャッシュ

旧v2キャッシュが見積もりで拒否されるREDを確認し、SFZ専用の識別子を公式2.17.0コミットとv6形式へ変更した。隔離した旧キャッシュからの再生成と再利用でPCMが一致し、元音源と登録一覧のハッシュも不変だった。IR・Roomの識別子は変更していない。

公開APIが含めない固定準備領域には、instance作成前に保守的な予約を追加した。解析器・再サンプリング表・voice・準備用一時領域へ64 MiBを割り当て、ブロック長とレート・チャンネル数に依存する領域を別に加算する。アセットのpayloadと公開APIの追加必要量も、稼働中と候補の共存期間を通じて保持する。音源未指定時の過小予算、候補の拒否、既存PCMの処理継続、解放後の回復を確認した。[固定準備の出典](https://github.com/Frieve-A/effetune/blob/v2.17.0/dsp/plugins/others/sfz/engine.h)

登録音源・ARIA・混在アセットの対象試験が成功した。境界値はネイティブの見積もりから取得し、上限ちょうどの成功と1 byte不足の拒否を確認した。容量超過時のレイヤー削減については上流2.17.0の廃止と既存機能維持の計画が異なるため、利用者へ確認中である。この回答に依存する選択処理とテストは保留し、工程4の独立した回帰調査を進める。

### 工程3に追加する生成音源の対応

全体試験で、`*noise`と`*silence`の生成データが2.17.0と異なることを検出した。上流は両者の保存用PCMを2フレームへ縮小し、ノイズをDSP内で連続生成する。この指定はv6の通常サンプル再生では表せない。既存の生成音源を維持するため、生成音源を含むバンクに限りv7の音源指定を追加する。通常のファイル音源だけのバンクにはv6を使う。[生成データとコメント](https://github.com/Frieve-A/effetune/blob/v2.17.0/js/sfz/generators.js)、[連続ノイズの公式仕様](https://github.com/Frieve-A/effetune/blob/v2.17.0/specifications/sfz/README.md)

v7では、既存の音量・pan・音高・音量エンベロープも専用領域へ格納する。移植するのは既存機能を表す固定値と生成音源の種類だけとし、追加の変調命令、MIDI操作、ストリーミングには対応しない。公式パッカーのバンクと実DSPのPCMを比較し、無音、発音、混在音源、連続ノイズを検証する。キャッシュ識別子にも生成方式の変更を含める。これを工程3の完了条件へ加える。[公式コンパイラー](https://github.com/Frieve-A/effetune/blob/v2.17.0/js/sfz/modulation.js)、[受入検証](https://github.com/Frieve-A/effetune/blob/v2.17.0/dsp/plugins/others/sfz/modulation_bank.h)

生成音源では公式の周期波形準備も必要だった。選択区間を1周期とし、半分ずつ短くした波形を用意する。公式の準備処理とパッカーを通した比較音声を使い、7種類の生成音源と区間指定のPCMが許容誤差1e-6以内で一致した。音量、pan、微調整、エンベロープも同時に検証した。混在バンクは公式バイト列と一致した。[周期波形の公式準備処理](https://github.com/Frieve-A/effetune/blob/v2.17.0/js/sfz/oscillator-source.js)

生成音源・バンク・再生・登録元の容量計算を合わせた33テストが成功した。生成データの容量不足を不正領域として省略すると鍵域が失われる問題もREDで再現し、容量エラーとしてバンク全体を拒否するよう修正した。登録音源のcold/warm再生は一致した。登録試験内の旧固定容量とレイヤー削減は、引き続き容量方針への回答待ちである。独立ドライバーの旧v2サイズ期待値もv6の548 byteへ更新し、単独スモークが成功した。

### 工程4のDebug実行負荷

未最適化のDebug DSPでは、256フレーム・48 kHzの周期5.333 msに対し、SFZの処理が5.89 msに達した。出力分配に届く前のPCMは連続していたが、PipeWireのcombine入力では遅れて届いたブロックが捨てられた。周期を1024へ広げる試行でも欠落が残ったため、試験条件の変更は取り消した。

共有DSPと専用PFFFTのDebugに`-Og`を適用した。デバッグ情報、割当監視、警告のエラー扱いは維持する。同じ256フレームの追跡では処理時間が概ね1.5〜2.4 msに下がった。`-Og`はデバッグ向けに最適化するGCCの設定である。[GCCの説明](https://gcc.gnu.org/onlinedocs/gcc/Optimize-Options.html)

別の失敗として、起動時から片方のcombine出力に1周期多くバッファが残るケースがある。このとき、DSPと分配の出力PCMは正しくても、デバイス間の時間差が申告値63フレームから256フレームずれる。Debug最適化だけでは解消しない。初期既定デバイスの確定待ち、全ストリームのクロックグループ統一、試験デバイスの常時処理を試したが、反復試験で解消せず、いずれも採用していない。

### 工程4のDSP回帰と性能比較

x86の共有バックエンドで、4種類の丸め設定、FTZ/DAZの4組、正常・エラー終了、instance・pipelineの両経路を確認した。MXCSRの復元は2.16.0でRED、2.17.0でGREENだった。Chorusの公式PCM10件とFDN Reverbの9件を実共有バックエンドの比較に追加した。12秒の減衰、極小入力、有限PCM、遅延、reset後の再現も検証し、Scalar・SIMD baseline・x86-64-v3で成功した。

性能は工程0と同じi9-12900KS、CPU 8、Scalar、2チャンネル、256フレームで測定した。ビルドや他の負荷試験とは重ねていない。各値は3回の平均で、準備時間にはcold/warm両条件を含む。

| 条件 | 処理時間 ns/frame（2.16→2.17） | 準備時間 ms（2.16→2.17） | 最大RSS MiB（2.16→2.17） |
| --- | --- | --- | --- |
| Classic 48 kHz | 894→915 | 14.8→26.1 | 27.9→41.1 |
| Event 48 kHz | 900→916 | 14.4→26.1 | 27.9→41.0 |
| Classic 96 kHz | 533→560 | 21.7→32.9 | 28.8→41.9 |
| Event 96 kHz | 531→558 | 22.0→33.3 | 28.8→42.0 |
| Classic 384 kHz | 253→291 | 65.1→74.6 | 33.6→47.2 |
| Event 384 kHz | 255→291 | 66.1→75.8 | 33.7→47.2 |

全72回の測定で、最大ブロック処理時間は各レートの周期内だった。RhythmとChorusの負荷はほぼ同等で、FDNの48 kHzは約5%増えた。遅延は不変で、48 kHzのClassicは4320、Eventは11040フレームだった。比較用SFZのPCM差は48 kHzで0、96/384 kHzで最大2.69e-7未満だった。Rhythmの4条件、Chorus、FDNは比較用入力の全PCMが一致した。

共有ライブラリはScalarが5,547,840から6,623,944 byteへ増え、他の3構成も約1.07〜1.08 MB増えた。SFZのRSS増加は約13 MiBで、新しい再サンプリング表とvoice・解析状態の準備が関係すると考えられる。384 kHz・16チャンネル・8192フレームの境界測定では、予約123,835,636 byteに対して実測ヒープ増加59,043,712 byte、最大RSS約71.4 MiBだった。最大処理2.93 msも周期21.33 ms以内だった。これは測定条件での結果であり、QEMUの性能値ではない。[上流の準備領域](https://github.com/Frieve-A/effetune/blob/v2.17.0/dsp/plugins/others/sfz/engine.h)

### 工程4の未解決項目

生成音源修正前の全体試験は301件中296件が成功した。失敗はSFZ容量境界、SFZアセット契約、独立ドライバーの旧サイズ期待値、出力プロファイル復元、音声入力の再開である。サイズ期待値と生成音源は修正済みで、容量の選択処理は確認待ちとした。再開時に古い入力音が出る失敗は、保存した2.16.0 Releaseでも反復試験の3回目に再現した。

既存の音声経路の問題も今回解消するか、更新とは別計画にするかを利用者へ確認している。回答までは最終完了条件を緩めず、追跡用に処理時間、キュー内バッファ数、最初のPCM不一致位置を記録できるようにした。出力プロファイル復元の失敗も未解決として残す。最終配布物の生成と完了宣言は、これらの扱いとSFZ容量方針が確定してから行う。

生成音源修正後のDebug全301テストは298件が成功した。容量方針に依存する2件に加え、コア単独ビルド試験の版照合が失敗した。コミット後にCMakeを再構成していなかったため、実行ファイルは2.16.26、screw-upの算出版は2.16.28だった。単独構成そのもののビルドと音声検証には失敗がなく、次の検証前に版情報を再生成する。今回の音声入力再開と出力経路試験は成功したが、反復時に再現した問題が解消したとは扱わない。

### 工程5の中間検証

日本語の利用者向け文書を2.17.0の名称、SFZの対応範囲、混在登録一覧、未対応機能、キャッシュ再生成へ更新し、英語へ反映した。技術文書には公開API、v6/v7の用途、固定領域の予約、i686の配置と浮動小数点設定、Debugの最適化を反映した。容量超過時の説明は従来の削減方針を記載しており、利用者の回答に合わせて確定する。

Release全300テストは298件が成功した。残る2件は`pipetune_asset_lifecycle_tests`と`pipetune_asset_preparation_contracts`で、容量方針への回答待ちである。後者の317ケース中17件が、旧容量または上流との削減方針の違いで失敗した。コア単独ビルド、GTK E2E、導入レイアウト、公式参照PCM、音声入力再開と出力経路はこの実行で成功した。Debugの割当監視1件がReleaseにはないため、全体の件数が1件異なる。

版情報を更新したDebug全301テストは298件が成功した。容量関連2件に加え、`pipetune_product-time-alignment`が15秒でタイムアウトした。起動時のリンク確立失敗が記録され、713,984フレームの入力に対してPCMの整列検証が始まらなかった。更新前の2.16.0 Releaseでは同じ試験が3回とも成功したが、原因が新旧どちらにあるかは断定できない。既存問題として除外せず、未解決の回帰項目として残す。今回のコア単独ビルド、Classic/Eventの実時間試験、入力再開、出力プロファイル復元は成功した。

GTK E2Eの画像を目視し、拡大後のウィンドウが仮想画面からはみ出して下部が欠ける問題を確認した。AGENTS.mdの画像確認指示に従い、工程5の検証手順を修正した。ウィンドウ全体が画面内に収まることを確認するテストはREDになり、拡大後に画面内へ移動するとGREENになった。修正後の画像で下部のボタンまで表示されることを目視した。動画で検証するメーターの動きと消去も成功した。[使用した移動APIの公式説明](https://github.com/kekyo/gestament/blob/1.4.0/README.md)、[API宣言](https://github.com/kekyo/gestament/blob/1.4.0/src/types.ts)

ホストのReleaseを作業ディレクトリ内の検証用導入先へ配置した。CLIとGTKがEffeTune DSP 2.17.0を表示し、導入されたScalar・SIMD baseline・x86-64-v3で113カーネルと新契約、Note PlayerのClassic/Event、IR、MatrixとresetのPCMを確認した。x86-64-v4はホストCPUの命令対応により実行対象外とした。これは検証用導入であり、最終の9配布物は未生成である。

i686の3バックエンドでも同じPCMを確認し、MXCSRの正常・エラー終了時の復元が成功した。ARMv7のScalar・SIMDもQEMU上で同じPCMに成功した。これらは工程1で生成した共有ライブラリを使った追加検証であり、最終配布物の検証には数えない。元のSFZ、音声ファイル、登録一覧のハッシュは工程0と一致し、サブモジュール内の直接変更もない。

### 工程3の容量方針の確定と修正

2026-11-10の利用者の回答に従い、容量不足時のレイヤー削減を削除した。上流2.17.0と同じく全領域と全サンプルを検証し、元ファイルまたは準備後の必要量が予算を超えれば音源全体を拒否する。v6の12ワードヘッダーと113フィールド、キー索引の見積もりも合わせた。生成音源のv7と実バックエンドの追加領域は、従来どおり準備段階で再検証する。

削除確認の一時テストは旧実装でREDになり、変更後にGREENになった。恒久テスト20件では、原本と共有PCMの容量、境界の1 byte差、未知の長さ、未使用サンプルの容量、整数の桁あふれ、全レイヤーの値と順序を上流と照合した。生成音源3種もネイティブの必要量ちょうどで発音し、1 byte不足と混在音源の超過を拒否した。予算を戻すと元のPCMへ回復した。

共有・取消・依存ファイル監視のfixtureを全レイヤー保持へ更新し、ライフサイクル試験が成功した。アセット契約の全体試験も成功した。削減済みキャッシュを再利用しないよう識別子を更新し、利用者向けの容量説明を日本語と英語へ反映した。証跡は`stage6-reduction-removal-red`、`stage6-budget-red`、`stage6-capacity-green`、`stage6-lifecycle-green`、`stage6-assets-full`に保存した。

### 工程4の配信キュー修正方針

入力再開の追跡で、DSPと配信入力が新しい無音を出した後も、出力1だけが停止前の音を出すことを確認した。配信出力のキューは出力0が1、出力1が2バッファだった。起動時のPCM整列失敗でも同じ差が残っている。上流のcombineモジュールは停止時に遅延線を消すが、個々の出力ストリームのキューを破棄していない。対照的に公式loopbackは停止時に両ストリームをflushしている。[combineの実装](https://github.com/PipeWire/pipewire/blob/1.0.5/src/modules/module-combine-stream.c)、[loopbackの実装](https://github.com/PipeWire/pipewire/blob/1.0.5/src/modules/module-loopback.c)

処理フラグとresamplerのprefillだけを変える実験では解消せず、実験用の変更を撤回した。停止時に配信出力をflushする実験では、入力再開5回とEvent実時間12回が成功した。修正は、PipeTuneが所有する配信ストリームに限って、PAUSEDへの遷移で公開APIの`pw_stream_flush`を呼ぶ処理とする。

combineモジュールにはストリーム取得用の公開APIがないため、Linuxの共有モジュールから呼ばれる公開`pw_stream_new`の境界で生成を監視する。通常の生成はlibpipewireへ委譲し、PipeTuneの配信先を示すプロパティがあるストリームだけに状態リスナーを付ける。リスナーはdestroy通知で解除する。外部コードや非公開の構造体には変更・アクセスしない。音声処理を持つ実行ファイルで当該シンボルを公開する必要があるため、各配布構成でもリンクと実行を確認する。GTKはCLI側の音声処理を操作し、GTK自体にはストリーム生成がない。[公開API宣言](https://github.com/PipeWire/pipewire/blob/1.0.5/src/pipewire/stream.h)、[RTLD_NEXTの公式説明](https://man7.org/linux/man-pages/man3/dlsym.3.html)

旧PCMを出力キューへ1ブロック追加する再現fixtureを先に実装する。停止・再開後の無音と新しい音のPCMでRED/GREENを確認し、既存の単一出力、複数出力、Classic/Event、時間整列、再接続も検証する。EffeTune更新とは独立した`fix:`コミットに分ける。

再現fixtureは修正前に停止前の音を検出してREDとなり、停止時のflush後はGREENになった。関連する21テストも成功した。追加の反復ではEvent、時間整列、キューを残す入力再開が各5回成功した。旧配信処理だけを戻したドライバーでは時間整列のタイムアウトが2回目に再現し、出力1の余分なキューによる256フレームのずれを追跡できた。

Classicの反復では、別の不一致が4回目に発生した。両デバイスのPCMは連続し、配信時の遅延差も正しいが、捕捉した2ブロックに同じ周期時刻が付いていた。時刻だけで異なるブロックを比較していないか、検証方法を追加調査する。停止時の古い音の修正と分けて扱い、全体の完了条件はまだ満たしていない。記録は`stage6-queued-resume-*`、`stage6-distribution-regression`、`stage6-distribution-repeats`、`stage6-classic-fixed-*`、`stage6-time-before-*`に保存した。

### 工程4の捕捉時刻による誤判定

Classicの追跡では、片方のデバイスの処理が次の周期へ遅れ、連続する2ブロックが同じ捕捉時刻を持っていた。PipeWireは入力バッファを受け取ったときの共有クロックから時刻を付けるため、時刻だけでは1ブロックを一意に選べない。旧検証は相手側の次のブロックと比較し、PCMが連続していても256フレームのずれと判定していた。[捕捉時刻の実装](https://github.com/PipeWire/pipewire/blob/1.0.5/src/pipewire/stream.c)

各時刻のグループが閉じてから最後のブロック同士を比較するよう、検証側を修正した。各ブロックのチャンネル識別と全フレームの連続性は、そのまま検査する。重複時刻を再現した単体検証は修正前にRED、修正後にGREENだった。1フレームの欠落を拒否し、片方に余分な1ブロックが残るケースも整列成功としないことを同時に確認した。

遅延・再接続・freewheelを含む関連試験が成功し、Classic/Eventの実時間試験は各12回成功した。記録は`stage6-observer-red`、`stage6-observer-green`、`stage6-observer-regression`、`stage6-observer-repeats`にある。製品DSPと許容誤差は変更していない。この検証修正も独立したコミットとする。

### 工程4のデバイス情報取得の競合

出力経路の失敗ログを見直し、`stage4-preflight-debug`で要求回数の上限に達した試験は、`product-output-change-route-policy`だったと特定した。プロファイル上書き後の復元試験とは区別して追跡する。WirePlumber 0.4.17が古い入力経路と新しい入力経路を交互に復元していた。

CPUを1個に制限して通知の重なりを調べると、製品が同じプロファイルを重複して公開する問題も見つかった。情報更新のたびに再取得していたが、先行する取得要求の遅い応答を判別せず、新しい一覧へ追加していた。古い応答を後から渡すテストはREDとなり、公開一覧、選択中のプロファイル、入力経路と音量に古い値が混ざった。

NodeとDeviceの列挙APIが返す要求番号を、パラメーターごとに保持して応答と照合する。最新の要求だけを受け入れる修正後は、再現テストと実セッションの一覧・プロファイル試験が成功した。単一世代の複数Propsを合成する既存動作も維持する。[列挙APIの説明](https://docs.pipewire.org/structpw__device__methods.html)、[応答のコメント](https://github.com/PipeWire/pipewire/blob/1.0.5/src/pipewire/device.h)、[native要求番号の実装](https://github.com/PipeWire/pipewire/blob/1.0.5/src/modules/module-protocol-native/protocol-native.c)

この修正で一覧の重複は消え、通常の4並列条件では出力切替・上書き復元・入力変更・利用不能の4種類が各5回成功した。CPUを1個に制限した条件では、通常ポリシーによるプロファイル変更との競合が残った。情報取得の修正だけで、経路の収束問題まで解消したとは判断しない。証跡は`stage6-inventory-*`、`stage6-change-onecpu-*`、`stage6-change-generation-*`に保存する。

追加追跡では、再接続時の入力復元要求の直後に古いポリシー要求が到着し、復元済み状態を製品が観測できないケースを確認した。製品は一度要求した印を残して待ち続け、既存の確定前再確認へ進めずタイムアウトしていた。初期のポリシー処理と入力復元を重ねないよう既存の300 msの再確認段階を使い、応答の確認前に限って入力復元を最大2回補う計画へ修正する。応答を観測した後の外部変更は、従来どおり競合として取り消す。

最初の復元結果が観測される前に上書きされるfixtureを先に追加し、RED/GREENと、初期入力の保持、既存の通知保留・観測後の外部変更・ロールバックを確認する。古いポリシーと新しい要求の競合を、無制限な再要求や許容回数の引き上げで隠さない。単一CPUの追跡で副次的に見つかった、非freewheel fixtureがSIGTERMを処理せず終了できない問題も、異常終了経路として別に検証する。

### 工程3の生成音源の作業用メモリ

同じ生成波形でも、offsetとendが異なる領域は別の周期波形へ展開する。登録ローダーが予約した作業用メモリより大きな上限をパッカーへ渡していたため、元のPCMが小さくても展開後に予算を超えられた。1024個の異なる区間で、約14.2 MBのバンクを過小予算のまま受け入れるREDを確認した。

パッカーの上限を実際に予約したPCM・周期波形・完成バンクの割当枠へ合わせた。修正後は過小予算で音源全体を拒否し、十分な予算では全領域を準備できた。拒否後の回復と再読込の一致も確認し、ライフサイクル試験が成功した。`098299c`へ分けてコミットし、`stage6-generator-working-red-final`と`stage6-generator-working-green`へ記録した。

入力復元の再現試験は、外部プロファイル選択を止めて最初の入力復元だけを上書きすると、旧処理でタイムアウトした。修正後は再確認で復元を完了し、関連11テストも成功した。単一CPUでも入力復元は完了したが、後続の接続失敗を注入する場面で制御接続が閉じた。`stage6-profile-input-isolated-red`、`stage6-profile-input-green`、`stage6-profile-input-onecpu-0`に区別して記録した。後続の終了原因は追加調査する。

### 工程4の検証プロセスの終了処理

非freewheelの音声fixtureは、開始時にSIGTERMをブロックしたまま受信処理を登録していなかった。このため他の検証が失敗すると、後処理でSIGKILLが必要になった。デバイス準備完了後に取消し、通常の終了コードと取消理由を確認するテストはREDになった。全音声fixtureへ受信処理を登録するとGREENになり、既存のfreewheel取消・生産側終了・切断の3テストも成功した。記録は`stage6-cancel-red`と`stage6-cancel-green`にある。製品コードの修正とは別にコミットする。
