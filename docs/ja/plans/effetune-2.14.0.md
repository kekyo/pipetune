# EffeTune 2.14.0対応計画

## 目的と調査時点

PipeTuneが参照するEffeTuneを、公式アプリタグ`v2.13.0`から`v2.14.0`へ更新する。2.14.0で保存したプリセットと登録済みSFZ音源を読み込み、既存の音声処理・外部アセット・複数出力を利用できる状態を目指す。

2026-10-23時点の`develop`、PipeTune HEAD `7a0a4534dfb7edc6db4b65fe03cc7a740f9b9005`を調査基準とした。調査開始時の作業ツリーはクリーンだった。参照元と更新先は次のとおり。

| 項目 | 現行 | 更新先 |
| --- | --- | --- |
| 公式アプリタグ | `v2.13.0` | `v2.14.0` |
| コミット | `6791afdfc6f4e6c913b9f48c8189a44e29a09bde` | `01c96f00f8e93595aefab96de4bc4d210e61c53b` |
| 同梱DSPバインディングの版 | 0.13.0 | 0.14.0 |
| 登録カーネル数 | 112 | 112 |

公式リモートの`v2.14.0`とローカルタグが同じコミットへ解決されることを確認した。追従基準はアプリタグとし、独立したDSPタグや最新版のmainへ置き換えない。[公式リリース](https://github.com/Frieve-A/effetune/releases/tag/v2.14.0)、[タグ間差分](https://github.com/Frieve-A/effetune/compare/v2.13.0...v2.14.0)

計画作成時にはソース・公式文書・APIコメントを調べ、一時コピーに対するABIパッチの適用を確認した。実装開始後の結果は、末尾の工程別実績へ記録する。

## 対応範囲

今回の中心はSFZ Note Playerの追従である。PipeTuneにはSFZの読込・変換をC++で実装した部分があるため、上流DSPの参照更新に加えて準備処理も更新する。音源ファイルから再生用データを作る処理を、本書では「アセット準備」と呼ぶ。

| 対象 | 上流2.14.0の変更 | PipeTuneで行うこと |
| --- | --- | --- |
| SFZの準備 | 生成波形、ARIA bank定義、文字コード、条件付きregion、エンベロープ補正を拡張 | 独立C++準備処理と登録音源ローダーへ反映 |
| SFZの音声処理 | 音符検出・発音開始・サンプル選択・試聴パラメータ・遅延を変更 | 生成カタログ、実音声、遅延補償、再構築を検証 |
| Analog Meter | Customモードと検出器・針応答の設定を追加 | パッカーと共有バックエンドを検証し、製品では解析除外を維持 |
| Note Spectrogram | 音色解析と音符イベントモデルを追加・更新 | モデルをビルドし、SFZからの利用を検証。製品では解析除外を維持 |
| Oscilloscope | 表示時間の上限を0.1秒から10秒へ拡張 | パッカーと上流テストで検証し、製品では解析除外を維持 |
| Engine | SFZバンクを事前準備して差し替えるC++ APIを追加 | 現在の構築済みパイプライン交換方式を継続し、既存アセットAPIを検証 |
| 既存DSP | 一部の公式参照PCMを更新 | 更新された音声データも回帰検証対象に含める |

解析専用DSPの除外は9種類を維持する。Rhythm Analyzerはクリック音を出せるため、引き続き通常のDSPとして実行する。IR Reverb、Room EQ、Crosstalk Cancellation、パラメータから生成するFIRの機能も回帰対象に含める。

EffeTuneアプリのLyrics、VisualizerのEdge Fade、MCP操作、画面ツアー、音源選択・試聴UIは移植対象に含めない。DSPパラメータ編集UI、旧DSP互換モード、PipeTune自身の版変更も追加しない。SFZバンクだけを稼働中Engine内で差し替える新方式は、今回の更新に必要ないため採用しない。

## 現行コードへの影響

### カタログ・パラメータ・共有バックエンド

`pipetune/CMakeLists.txt`は上流`package.json`からEffeTuneの版を取得する。カタログは`pipetune/tools/generate-dsp-catalog.mjs`が登録一覧とパラメータ仕様から生成する。新しいパラメータもこの経路を使い、手書きのパッカーを追加しない。

| 型 | float数の変更 | 2.14.0のハッシュ | 主な追加・変更 |
| --- | --- | --- | --- |
| `SFZNotePlayerPlugin` | 16 → 19 | `0x378e7f2b` | `ss`、`pm`、`pv` |
| `AnalogMeterPlugin` | 4 → 7 | `0xad3d461c` | Custom、`de`、`nr`、`os` |
| `NoteSpectrogramPlugin` | 2 → 3 | `0xafc5ff07` | `te`。既定パック値は`[28,91,0]` |
| `OscilloscopePlugin` | 配置変更なし | 既存値を維持 | `dt`の上限が10秒 |

SFZのslot 0容量は1 GiB、畳み込みアセットは32 MiBのままである。`registry.inc`、公開C ABIヘッダー、`core/abi.cpp`、tree modelsの生成処理、DSP vendorにはタグ間の変更がない。上流CMakeの変更はNote Spectrogramのモデルfixtureテスト追加で、製品コアのソース一覧と既存の浮動小数点コンパイル条件には変更がない。[パラメータ仕様](https://github.com/Frieve-A/effetune/tree/v2.14.0/dsp/plugins)、[上流CMake](https://github.com/Frieve-A/effetune/blob/v2.14.0/dsp/CMakeLists.txt)

`pipetune/cmake/EffeTuneNativeBackends.cmake`の独自共有バックエンドでは、既存の`-ffp-contract=off`、i686向け演算条件、ARMモデル変換を継続する。新しいモデルヘッダーの依存が反映されることと、上流追加テストのCTest登録を確認する。Engineには64bit atomicを使う識別子が追加されているため、32bit配布先でのコンパイル・リンク・ロードも早い段階で実施する。追加リンク依存の要否は実ビルドで判断する。

ABIパッチ`pipetune/patches/effetune/abi-engine-access.patch`は、2.14.0の`core/abi.cpp`を一時ディレクトリーへコピーして`patch --dry-run --fuzz=0`で適用できた。実装時には通常のビルド経路でも確認する。`deps/effetune`の作業ファイルやvendorは編集せず、公式コミットへの参照だけを更新する。

固定値を持つ`pipetune/test/effetune_213_test.cpp`と`effetune_backend_artifact_test.cpp`を2.14.0の契約へ更新する。前者は現行版のスモークテストとして改名し、Adaptiveなど過去に追加した機能の回帰検証は残す。`parameter-packer-parity.mjs`のNote Spectrogram既定値、Analogのモード一覧、SFZの新パラメータ、Oscilloscopeの境界値も更新する。

### SFZの文字コード・bank定義・依存ファイル

現行の`pipetune/src/sfz_asset_loader.cpp`は、登録情報から`id`・`path`・`root`を読み、SFZとincludeをそのまま文字列へ変換する。2.14.0の登録情報は`bankPaths`と`importedAt`を追加する。`bankPaths`は音源ルートを基準とする`.bank.xml`の相対パス一覧である。省略時は空一覧として扱い、件数・拡張子・ルート内参照を検証する。`importedAt`は表示用情報として扱い、音声生成に利用しない。[登録形式](https://github.com/Frieve-A/effetune/blob/v2.14.0/electron/sfz-library-ipc.js)

上流の`js/sfz/source.js`に従い、SFZ・include・bank XMLを厳格なUTF-8として読んだ後、不正な場合はWindows-1252で復号する。現在の`sfz/text.cpp`にあるJavaScriptの文字列長・並び順を合わせる処理と、ファイルバイト列の復号は役割を分ける。

ARIA bankは、SFZと音源の配置や変数を記したXMLファイルである。`AriaBank`、`AriaElement`、`Define`を読み、対象SFZに最も近い候補を上流と同じ順序で選ぶ。同じ階層に候補が一つだけある場合の扱い、パス比較、XML属性の文字参照、`$sample_dir`の`$Banks/`から`Samples`への変換も含める。初期definesをSFZパーサーへ渡し、SFZ内のdefine・include展開へ接続する。一般的なXML処理系へ仕様を拡張しない。[文字コードとbank選択](https://github.com/Frieve-A/effetune/blob/v2.14.0/js/sfz/source.js)、[準備サービス](https://github.com/Frieve-A/effetune/blob/v2.14.0/js/sfz/service.js)

ファイル読込・閉域解決・変更監視はホスト側に残す。メモリ上の文字列からbankを選び、definesを取り出す処理は`pipetune/asset-preparation/sfz/`へ置き、独立ドライバーで検証できるようにする。新たな読込も既存のcardioによる制御側の非同期処理へ接続する。

選択したbankの内容と選択に影響する登録一覧・読込済み候補を、依存ファイルとキャッシュの検証へ含める。選択されていない候補の変更で選択結果が変わる場合も見落とさない。読込中の変更は既存のsnapshot再検証で検出する。パスやシンボリックリンクによる音源ルート外の参照は、既存の`asset_file.cpp`の制約を維持する。

### SFZの生成波形・region・診断

regionは、鍵盤範囲や強さに応じて再生するサンプルと設定の組である。2.14.0では`sample=*sine`など、実ファイルを使わないregionを利用できる。

`*sine`、`*saw`、`*square`、`*triangle`、`*tri`、`*noise`、`*silence`を上流と同じ決定的PCMとして生成する。通常波形は2048フレーム、noiseは8192フレームで48 kHzとなる。生成波形は実ファイルの存在確認やFFmpegデコードへ渡さず、メタデータ取得・容量選別・PCMプール生成へ接続する。上流どおり基準キー60と連続ループを設定し、noiseの乱数列とfloat32への丸めも比較する。[生成波形](https://github.com/Frieve-A/effetune/blob/v2.14.0/js/sfz/generators.js)

パーサーでは`sw_lolast`・`sw_hilast`を初期キースイッチと照合する。`sustain_sw`・`sostenuto_sw`の分類、条件不成立regionの除外診断も上流へ揃える。従来の`unsupported-regions`警告を生成する分岐は不要になるため整理する。

`ampeg_attack`・`ampeg_hold`・`ampeg_decay`・`ampeg_release`は、範囲外なら0〜100秒へ補正し、非有限値なら既定値へ戻す。補正のためにregionを捨てず、`envelope-times-adjusted`の警告と件数を返す。現在の警告説明は未知コードをループ点修復として扱うため、新しいコードを明示的に説明する。準備処理の診断、メモリ計上、CLI・GTKの表示まで確認する。[パーサー](https://github.com/Frieve-A/effetune/blob/v2.14.0/js/sfz/parser.js)、[保存バンクの警告](https://github.com/Frieve-A/effetune/blob/v2.14.0/js/sfz/bank.js)

再生用バンクを作る上流`js/sfz/asset.js`には変更がない。ETA1/table-v2の形式と容量・索引の契約を維持する。SFZキャッシュは新しいアプリコミットと準備処理版へ進め、2.13の準備結果を流用しない。IR・Room EQの準備アルゴリズムと形式には今回の差分がないため、個別の出典版とキャッシュ識別子を機械的に書き換えない。

### SFZの発音・遅延・ライフサイクル

`ss`は-6〜6のサンプル選択オフセットで、既定値は0となる。選ぶサンプルのキーをずらすが、出力の音高は保つ。`pm`は試聴MIDI番号で既定-1、`pv`は試聴の強さで既定100。上流アプリは`ss`を保存し、`pm`・`pv`を音声処理へ一時的に渡す。通常の保存プリセットでは試聴を開始しない。PipeTuneもパラメータ契約は受け入れ、試聴UIや保存時の独自注入は追加しない。[アプリ側パラメータ](https://github.com/Frieve-A/effetune/blob/v2.14.0/plugins/others/sfz_note_player.js)、[DSP仕様](https://github.com/Frieve-A/effetune/blob/v2.14.0/dsp/plugins/others/sfz_note_player/params.json)

新しい発音処理は音の立ち上がり検出と再発音規則を使う。公式11件の参照PCMに加え、連続音・反復音・離鍵・release・resetを実プリセットから検証する。`ss`の検証には、隣接キーで波形の異なる音源を用意する。サンプルが変わることと、出力音高が保たれることを分けて確認する。[発音規則](https://github.com/Frieve-A/effetune/blob/v2.14.0/dsp/plugins/others/sfz_note_player/onset_rule.h)、[カーネル](https://github.com/Frieve-A/effetune/blob/v2.14.0/dsp/plugins/others/sfz_note_player/kernel.cpp)

報告遅延には、解析遅延と負の`tm`による追加遅延に加えて、解析hopの半分が入る。hopは解析を更新するフレーム間隔で、48 kHzでは960フレームとなり、半分は480フレームである。旧版の値に固定せず、上流APIが返す遅延と実際のdryインパルス位置を照合する。`tm=-100/0/100`、選択ペア、未選択ch、別バス、最終出力間の整列を確認する。

`pipetune/src/dsp_pipeline.cpp`は制御側でアセットを転送し、無音処理でACTIVEになるまで準備してresetした後、パイプラインを公開する。2.14でもこの手順を継続する。新C++ APIは任意の拡張であり、公式文書も既存C ABIとアセットAPIの継続を明記している。`effetune_backend_asset_bridge.cpp`のbegin/copy/commit経路を保ち、準備途中のバンクを音声処理へ公開しない。[公式のネイティブSFZ更新契約](https://github.com/Frieve-A/effetune/blob/v2.14.0/dsp/README.md#native-sfz-bank-updates)、[所有権とスレッドのコメント](https://github.com/Frieve-A/effetune/blob/v2.14.0/dsp/core/engine.h)

レート・バックエンド・プリセット変更時はパイプラインを再構築する。原本変更、準備取消、読込失敗では既存の旧パイプライン保持を検証する。無音時休止オンではresetして休止し、オフではreleaseを含む音声処理を継続する。音声コールバックへファイル読込や準備・解放処理を追加しない。

### 解析DSP・既存DSP・モデル

Analog MeterのCustom、Note Spectrogramの`te`、Oscilloscopeの長時間表示を含むプリセットは、既存どおり警告なしの`ignored`となる。加工DSPとの混在、無効Section、別バス、96個を超える解析ノードでも、PCM・遅延・有効DSP数に影響しないことを確認する。

配布する共有バックエンド自体はこれらのDSPを含むため、製品で除外する場合もパラメータと上流nativeテストを検証する。Analog Meterの公式goldenは5件から6件へ更新する。Note Spectrogramのモデルfixtureテストも全体テストへ含める。Note解析はSFZ内でも使われるため、解析除外を理由にモデルの検証を省略しない。

上流参照PCMにはSFZのほか、AM Radio Simulator、Vinyl Artifacts、Phaser、Tube Simulatorの更新がある。共有バックエンドの既存goldenランナーを拡張し、これらの更新ケースを取りこぼさない。JSONの参照ハッシュだけの更新と音声データ自体の更新を区別する。対象タグのPCMと許容誤差をそのまま使い、合格のためのgolden再生成や許容誤差緩和は行わない。

## 段階的な実施計画

実装は基準の`develop`から作業ブランチを作って進める。各工程は、ビルドと対象テストを実行し、CLIまたは既存テストドライバーで成果を観測できる単位でコミットする。mainへのマージは行わない。

コード修正は、対象の挙動を示すテストでREDを確認してから実装し、GREENを確認する。上流参照更新だけで成立する回帰検証には、失敗を作るための変更を加えない。既存テストの期待値変更は、上流仕様の変更と根拠を確認してから行う。工程途中の対象テスト成功を、全体成功として記録しない。

### 0 現行の基準を記録する

現行2.13.0で全体テストを実行する。利用可能なISA、ビルド依存、コンテナ、ディスク・メモリを確認し、SFZの初期化時間・報告遅延・処理負荷・RSSと配布バックエンドのサイズを記録する。測定はReleaseで48/96/384 kHzを代表条件とし、ビルドと同時に測らない。

完了条件は、既存不具合と更新後の失敗を比較できる基準が残ること。環境不足は必要なパッケージと導入方法を案内し、未実施項目を成功扱いしない。記録は次の実装コミットへまとめる。

### 1 2.14.0の最小実行を成立させる

2.14.0のパラメータを実共有ライブラリへ渡すスモークテストを追加し、現行版でREDを確認する。サブモジュールを指定コミットへ進め、版・ハッシュ・float数の固定値とパッカー比較を更新する。共有バックエンド、モデル、ABIパッチを通常のビルド経路で検証する。

既存のAdaptiveプリセットと単純な登録済みSFZ音源を使い、CLIと実共有バックエンドから音声を処理する。Scalarとホストで実行可能なSIMDに加え、armv7l・i686でビルドとロードを確認する。新しいSFZ構文の追従は次工程で行い、この段階では全SFZ適合テストの成功を完了条件にしない。

完了条件は、CLI・GTKの取得する版が2.14.0となり、112カーネルの契約が一致し、最小プリセットから有限PCMを得られること。追加のビルド警告は原因を調べて解消する。

コミットは`feat: update EffeTune integration to 2.14.0`。

### 2 SFZの生成波形とregion解釈を完成させる

既存の`sfz-parser`・`sfz-selection`・`sfz-bank`適合テストに、生成波形、初期キースイッチの範囲、エンベロープ補正を追加する。2.14.0のJS出力と現在のC++出力の違いでREDを確認し、独立C++準備処理とホストの生成波形経路を更新する。

実ファイルを持たない音源から冷キャッシュ・温キャッシュの両方でPCMを出す。通常ファイルと生成波形の混在、容量選別、noiseの再現性、silenceの有限ゼロ出力、新しい警告件数も検証する。

完了条件は、同一入力のパース結果・選択結果・バンク・音声が上流契約に一致し、Node.jsなしの製品経路で生成波形を再生できること。旧`unsupported-regions`生成機能を削除する際は、削除確認の一時テストでRED、削除後GREENとコミット、その後に一時テストを削除する。通常の結果比較テストは維持する。

コミットは`feat: support EffeTune 2.14 SFZ generators and region rules`。一時テストの整理は独立した`chore:`コミットにする。

### 3 SFZのbank定義と文字コードを完成させる

Windows-1252のSFZ・include・XML、複数階層のbank候補、definesの上書き、`$sample_dir`を使う登録音源でREDを確認する。登録情報、メモリ入力の解析、依存ファイル監視を接続する。追加の外部APIを使う場合は、公式文書とAPIコメントを確認する。

空キャッシュから実音源を再生し、bank内容・一覧・include・サンプルを変えた後の再生成を検証する。破損キャッシュ、欠損復旧、準備中の原本変更、取消、音源ルート外の参照も既存のテスト基盤で確認する。

完了条件は、上流と同じbankとdefinesを選び、文字コードを正しく扱い、入力変更を見落とさず再生結果へ反映すること。失敗時には旧パイプラインを保持し、EffeTuneの原本へ書き込まない。

コミットは`feat: support EffeTune SFZ bank definitions and text decoding`。

### 4 SFZの発音と遅延補償を検証する

公式11 goldenを実共有バックエンドで比較し、`ss`、`pm`、`pv`の既定値・境界値と音声への効果を確認する。通常プリセット、音高を保つサンプル選択、反復音、release、dry/wetの混合を検証する。試聴パラメータはバックエンドの試験へ含め、通常プリセットには注入しない。

`sfz_runtime_test.cpp`を拡張し、44.1/48/96/192/384 kHz、1/2/16ch、All・最後のペア・単一ch、端数ブロックを分担して検証する。報告遅延、dryインパルス、別バスと複数出力の整列、reset、レート・バックエンド変更、無音時休止を含める。学習や検出の時間は処理フレーム数で進める。

完了条件は、公式許容誤差内で一致し、登録済み音源の発音・再発音・releaseと全出力の遅延整列が成立すること。Releaseの同条件測定で2.13との差を記録し、増加が大きい場合はモデル・準備・定常処理のどこで増えたか説明する。新たな製品不具合は再現テストと根本修正を`fix:`コミットにまとめる。

コミットは`feat: validate EffeTune 2.14 SFZ playback and latency`。

### 5 解析除外と既存機能を回帰検証する

Analog Customの6件目のgolden、Noteモデルfixture、Oscilloscopeの10秒設定と解析9種類の除外を検証する。更新された既存DSPの参照PCMを共有バックエンドの対象へ追加する。Adaptive、Cassette、Rhythm、IR、Room EQ、Crosstalk、生成FIR、複数出力、再読込時の状態表示も確認する。

DebugとReleaseの全体テストを実行する。初期段階で保留したSFZ準備処理の適合テストを含め、全ての変更を統合した状態で検証する。

完了条件は、既存機能に退行がなく、更新された参照PCMと全体テストが成功すること。初期のREDから残る未解決の失敗がないことも確認する。

コミットは`feat: verify EffeTune 2.14 regression coverage`。

### 6 利用者文書と配布物を仕上げる

`README_ja.md`と`docs/ja/details.md`を原本として対応版とSFZの利用条件を更新し、対応する英語文書へ反映する。`pipetune/README.md`、既存内部文書の移植対応表、著作権表示は、変更した部分の出典を2.14.0へ進める。過去の計画・実績や変更していない移植コードの出典は残す。一般ドキュメントから本計画へのリンクは追加しない。

単独ビルド、ステージング先へのインストール、GTK E2E、現行9配布構成を検証する。配布環境ではNode.jsとffmpegコマンドに依存せず、空キャッシュからSFZ・IR・Room EQを準備して音声処理する。私有LGPL FFmpeg、必要なライブラリ・ライセンス・対応ソースの同梱も既存検証で確認する。

完了条件は、配布物のCLI・GTK・共有バックエンドが2.14.0として動き、対応ISAとアセット準備を確認できること。最終ソースに対する全体テストを完了し、以下の最終完了条件と結果を照合する。ビルドへ影響しない文書だけを最後に直した場合は再ビルド不要。

文書は`doc: document EffeTune 2.14 support`、配布側の修正が必要なら独立した`fix:`または`chore:`コミットにする。

## 検証の入口と記録

既存MakefileとCTestを使う。全体テストには上流nativeテスト、パッカー比較、アセット準備のVitest、共有バックエンドのPCM比較、製品経路、単独ビルド・インストール、GTK E2Eを含める。

```sh
GDK_BACKEND=x11 make test TEST_BUILD_DIR=build/effetune-214-debug
cmake -S . -B build/effetune-214-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build/effetune-214-release --parallel
GDK_BACKEND=x11 ctest --test-dir build/effetune-214-release --output-on-failure
DESTDIR="$PWD/artifacts/verification/effetune-2.14/final-install" cmake --install build/effetune-214-release --prefix /usr
./build_package_all.sh
```

配布対象は`build_package.sh`にあるDebian trixieのx86_64・i686・arm64・armv7l・riscv64、Ubuntu 24.04/26.04のx86_64・arm64の9構成とする。各CPUで実行可能なScalar/SIMDを検証し、実行できないISAを成功扱いしない。エミュレーションの所要時間を実機性能と混同せず、必要なタイムアウトを確保する。

UIをブラウザ経由で試す場合はPlaywright MCPを使う。アニメーションなど時間軸の問題が出た場合は動画で検証し、画像マスターを変える場合は目視で確認する。新たなスクリプトはNode.jsを使い、ビルド時テキスト処理は既存のfuncityへ揃える。

実行コマンド、対象コミット、環境、ログ、PCM誤差、性能・メモリ、配布物ハッシュは`artifacts/verification/effetune-2.14/`へ保存し、工程ごとの結果を本書へ追記する。計画外の問題が見つかったら前提を見直し、場当たり的な修正を重ねない。計画とテストの要求が矛盾する場合は、どちらを採用するか確認してから進める。

## 実装の最終完了条件

- [ ] 指定のアプリコミットを参照し、外部ソース・vendorへの直接変更がない。
- [ ] CLI・GTKが2.14.0を表示し、112カーネルのパラメータ・容量が実共有バックエンドと一致する。
- [ ] SFZの生成波形、bank定義、文字コード、region条件、エンベロープ補正が上流比較と製品再生で一致する。
- [ ] 新しい依存ファイルを監視し、キャッシュ再生成・取消・欠損復旧・失敗時の旧状態保持が成立する。
- [ ] SFZの公式11 golden、サンプル選択、発音・release・reset、レート・ch選択、遅延補償が成功する。
- [ ] 解析9種類の除外、更新モデル、Analogの6 golden、既存DSPの更新PCMと外部アセットに退行がない。
- [ ] Debug/Release全体テスト、単独ビルド、GTK E2E、インストール、9配布構成を最終ソースで検証した。
- [ ] Node.jsとffmpegコマンドなしで、冷キャッシュからSFZ・IR・Room EQを利用できる。
- [ ] 日本語原本と翻訳、出典・著作権表示、性能差、未検証事項を記録し、工程ごとのコミットが揃っている。

## 計画作成時の確認結果

現行コードと過去の2.13.0対応実績、公式2.14.0リリース、タグ間差分を照合した。更新先コミットの一致とABIパッチのfuzzなし適用を確認した。SFZの独立C++準備処理までを対象とし、各工程の観測可能な成果物・検証・完了条件を定義した。

計画作成時には、実装の最終完了条件は全て未実施だった。文書追加はビルドへ影響しないため、製品ビルド・全体テストは計画作成時には実行していない。

## 工程別実績

### 0 基準状態

`b089b70166dfc48aef057fb77adb117a159bc9ba`から`feat/effetune-2.14.0`を作成した。ホストはIntel Core i9-12900KS、GCC 13.3、CMake 3.28、Node.js 24.21。Scalar、x86-64 baseline、v3を実行できる。v4はビルド対象で、ホストではCPU要件を満たさないため実行を除外する。工程5で実ロード結果を再確認し、当初のv4も実行可能という記述を訂正した。メモリは約62 GiB、開始時の空きディスクは約700 GBで、全9構成のPodmanイメージを確認した。

2.13.0のDebug全237テストを`build/effetune-214-debug`で実行した。初回は233件成功、GTKの4件が失敗し、1643.08秒を要した。Waylandセッションの環境を継承したため、Xvfbを使う試験でもGTKがWaylandへ接続していた。`GDK_BACKEND=x11`を指定して失敗した4件を再実行し、全て成功した。以後の全体試験にもこの指定を使う。基準ログは`baseline-debug.log`、環境を揃えた再確認は`baseline-gtk-x11-recheck.log`に保存した。GTK E2Eは初回でも成功している。

Release測定には、44.1 kHz・4410フレームのステレオWAVをキー83でループ再生する合成音源を使った。入力は1 kHz、2ch、256フレーム単位。約1秒の準備運転後、256ブロックの処理呼出しだけを測った。CPU 8へ固定し、レートごとに独立プロセスで冷・温キャッシュを各3回測定した。初期化と処理時間は中央値、RSSはプロセス全体の最大値である。

| レート | キャッシュ | 初期化ms | 処理ns/frame | 最大RSS KiB | 遅延frame |
| --- | --- | --- | --- | --- | --- |
| 48 kHz | 冷 | 13.910 | 835.03 | 19056 | 3840 |
| 48 kHz | 温 | 12.495 | 830.21 | 18096 | 3840 |
| 96 kHz | 冷 | 21.141 | 525.46 | 20000 | 7680 |
| 96 kHz | 温 | 19.744 | 523.16 | 18828 | 7680 |
| 384 kHz | 冷 | 65.086 | 296.78 | 24804 | 30720 |
| 384 kHz | 温 | 63.809 | 300.36 | 23628 | 30720 |

測定時にはDebugのCTestが動いていたため、無負荷時の絶対性能を示すものではない。条件・各回の値・集計を`sfz-performance-baseline-213.conditions.json`、同名の`.json`と`.summary.json`へ保存した。再測定には同じ検証ディレクトリーの`run-sfz-performance.mjs`を使う。

strip前のRelease共有バックエンドは、Scalarが3,669,384 byte、baseline SIMDが3,669,928 byte、v3が3,628,296 byte、v4が3,755,176 byteだった。値は`baseline-backend-sizes.json`に保存した。

### 1 参照更新と最小実行

2.14.0の版・パラメータ契約と、19 floatのSFZパラメータを実共有バックエンドへ渡すテストを追加した。2.13.0では旧版・旧契約・新パラメータの拒否で失敗し、REDを確認した。結果は`step1-red/result.log`に保存した。

独立作業ツリーでサブモジュール参照を公式コミット`01c96f00f8e93595aefab96de4bc4d210e61c53b`へ進めた。外部ソースを編集せず、既存ABIパッチとモデル生成を含むDebugビルドが警告なしで成功した。CLI・GTKともEffeTune DSP 2.14.0を表示した。検証用のリンク作業ツリーではPipeTune自身の版が0.0.1となるため、配布用の版は通常の作業ツリーで確認する。

スモーク、共有バックエンドのgolden、SFZ製品再生、パッカー比較、上流ABI、Analog・Note・SFZ native、追加されたNoteモデルfixtureの9テストが成功した。所要時間は137.19秒で、ログは`step1-targeted.log`に残した。Scalarとホストで利用可能なSIMDを使って112カーネルの読込と有限PCMを確認している。

i686・armv7lの配布パッケージをビルドし、別コンテナへ導入してScalarバックエンドをロードした。ABI 1、112カーネル、SFZの新ハッシュ・19 float・1 GiB容量、既存アセット転送シンボル、ゲイン適用後のPCM、Tube Simulatorの有限かつ有音のPCMを確認した。暫定パッケージ・SHA-256・ソース状態・導入試験は`step1-matrix/`へ保存している。モデル生成と64bit atomicの追加によるビルド・ロード失敗はなかった。

i686では変更のない上流Tube Simulatorのベクトル戻り値に`-Wpsabi`が2件出た。armv7lでは`dpkg-shlibdeps`が`__aeabi_atexit@CXXABI_ARM_1.3.3`の解決警告を1件出した。後者はリンク先libstdc++の同じ版のシンボルを確認し、導入後の実行も成功している。診断の根拠は`step1-matrix/results.md`に残した。浮動小数点条件を変えずに警告へ対処し、最終配布工程で再検証する。[dpkg-shlibdepsの警告仕様](https://manpages.debian.org/trixie/dpkg-dev/dpkg-shlibdeps.1.en.html)

工程0は更新前との比較基準を記録し、工程1は版・カーネル契約・最小PCM・32bitロードの完了条件を満たした。最終配布物、全体回帰、残るSFZ準備処理は後続工程で検証する。

### 2 生成波形とregion解釈

7種類の生成波形、初期キースイッチ範囲、エンベロープ時刻の補正を独立C++準備モジュールへ追加した。生成波形のrate・フレーム数・PCMは上流JSと比較し、繰返し生成の再現性も確認した。通常ファイルと生成波形の混在、容量選別、ETA1/table-v2バンクの一致を含む純粋SFZ試験66件が成功した。独立モジュールのビルドとCTest 3件も成功している。

登録音源ローダーは生成波形のメタデータとPCMを直接作り、ファイル解決・外部デコードへ渡さない。新しい警告の説明とメモリ計上を追加し、SFZキャッシュを2.14.0コミット・準備処理v2へ進めた。PATHから外部コマンドを除いた状態で、sine・noise・silenceの冷／温キャッシュ、容量ちょうどの成功と1 byte不足の拒否、通常ファイルとの混在、レイヤー削減、補正警告の保存が成功した。

変更前のパーサー比較と登録再生でREDを確認した。`unsupported-regions`生成機能は削除確認の一時テストでREDを確かめ、削除後にGREENとした。全アセット試験では旧警告を期待していた既存テスト1件も失敗したため、計画と上流仕様を照合して期待値を更新した。変更後は32ファイル265テストが成功し、所要時間は120.34秒だった。ログは`step2-parser-red.log`、`step2-registered-red.log`、`step2-targeted.log`、`step2-contracts-green.log`に保存した。

通常のDebugビルドは警告なしで成功した。共有バックエンドのgolden、登録SFZの実再生、アセットのライフサイクル、2.14.0スモーク、独立SFZ準備、GTKの状態保持と診断表示も成功した。工程2の上流比較・製品再生の完了条件を満たした。

周期波形の登録再生はDebugのScalarでnoiseより長くかかった。個別計測では初期化が約30ms、0.5秒の周期波形処理が約2.8秒、noise処理が約0.23秒だった。生成周期の原レートは約536 kHzで、キー83・48 kHz出力では上流の大きな変換倍率用フィルターを使う。所要時間の差はDSP処理で生じていた。計測は`step2-generator-timing.log`に残し、Releaseの性能確認にも含める。[上流リサンプリング処理](https://github.com/Frieve-A/effetune/blob/v2.14.0/dsp/plugins/others/sfz_note_player/kernel.cpp)

工程2を`7045f1c`でコミットした後、機能削除の確認に使った一時テストを削除した。通常の上流比較テストは残している。

### 3 bank定義と文字コード

独立C++準備モジュールへ、厳格なUTF-8とWindows-1252の復号、ARIA bankの選択、初期definesを追加した。UTF-8のBOM、全256 byte、Unicodeの大文字小文字・空白、XML属性の文字参照、候補の階層と並び順を上流JSと比較した。初期defineの未対応と、最初のXML解析案で長い属性が失敗することをREDで確認した。XML属性は上流の対象構文を走査する方式に改め、100 KiBの属性も成功している。Windows-1252復号中の余分なコピーも除き、ホストのメモリ予約内で変換する。

候補の並び順とUnicodeの大小文字変換にはICUを使用する。公式文書と公開APIコメントを確認し、独立モジュールのリンク依存と`prereq.sh`へ追加した。ホストのICUは74.2、比較用Node.jsは78.3で、C・C.UTF-8・英語・日本語のロケールで今回の比較ケースが一致した。全9配布イメージにも開発パッケージがあることを確認した。配布版でのリンクと実再生は工程6で検証する。[ICU照合API](https://unicode-org.github.io/icu/userguide/collation/api.html)、[公開C API](https://unicode-org.github.io/icu-docs/apidoc/released/icu4c/ucol_8h.html)

登録情報の`bankPaths`を検証し、選択したbankとSFZ・includeを既存の非同期読込へ接続した。Windows-1252のファイル名、初期defineを使うinclude、`$sample_dir`、冷／温キャッシュ、候補・一覧・include・サンプルの変更を実音声で確認した。候補全体ではなく選択したbankだけを原本容量へ算入し、容量超過を拒否する。欠損からの復旧と、音源ルート外を指すシンボリックリンクの拒否も成功した。

未選択候補と欠損候補をsnapshotと変更監視へ含め、候補の変更・出現で準備結果を無効にする。新しいライフサイクル試験は工程2のローダーでRED、工程3でGREENとなった。準備取消と読込失敗後も既存パイプラインが処理を継続し、失敗した依存ファイルも復旧監視へ残ることを確認した。

Debug全ビルドと独立準備モジュールのビルドは警告なしで成功し、独立CTest 3件、ホストのキャッシュ・原本・ライフサイクル・版契約4件、全アセット準備CTestが成功した。全アセット試験は123.46秒だった。主なログは`step3-registered-red.log`、`step3-source-green.log`、`step3-contracts-green.log`、`step3-host-targeted.log`、`step3-standalone.log`に保存した。旧ローダーとの比較は`step2-host/lifecycle-phase2-red.log`に残した。工程3の完了条件を満たした。

### 4 SFZの発音・遅延・性能

実WAVのキーごとに倍音を変え、`ss=-6/0/+6`で音源が変わっても出力の基音700 Hzを保つことを確認した。`pm`の既定値では無音入力を無音のまま通し、明示した試聴は入力音の検出なしで発音する。`pv`による音量差、reset後の再発音、レート・バックエンド変更時のバンク再利用も成功した。共有バックエンドでは`pm=-1→83→-1→83→-1`を渡し、停止・再開とvelocity両端をPCMで検証した。製品のパラメータ更新APIや試聴UIは追加していない。

連続音、2回の発音、離鍵後のrelease減衰、無音時休止の既存試験を確認した。44.1/48/96/192/384 kHz、1/2/16ch、選択ペア・単一ch・All、端数ブロックを各共有バックエンドで検証している。`tm=0/-100/+100`の上流`instanceLatency`とパイプラインの報告値が一致し、dryインパルスもその位置に現れた。別バス経由のPCMと直結のPCM、選択外を含む16chの整列も一致した。物理的な複数出力への割付・補償は、既存の`product-time-alignment`、`product-slots`、`product-sixteen`などを工程5の全体試験で再確認する。

SFZ公式11 goldenと試聴遷移はDebug・Releaseの共有バックエンドで成功し、上流SFZ native試験も成功した。Debug実再生試験は約29秒で、ログは`step4-runtime/runtime-native-latency.log`へ保存した。共有バックエンドの結果は`step4-preview-artifact.log`と`step4-release-preview.log`、上流試験は`step4-upstream-sfz.log`に残した。

工程0と同じRelease測定をCPU 8で行い、保存した2.13.0の実行ファイルと共有ライブラリも直後に再測定した。測定中はビルドとプロジェクトの試験を止め、デスクトップアプリは稼働していた。下表は冷キャッシュの各3回の中央値で、RSSだけ最大値である。

| レート | 2.13初期化ms | 2.14初期化ms | 2.13処理ns/frame | 2.14処理ns/frame | 2.14最大RSS KiB | 2.14遅延frame |
| --- | --- | --- | --- | --- | --- | --- |
| 48 kHz | 13.920 | 15.978 | 825.84 | 978.00 | 27928 | 4320 |
| 96 kHz | 21.172 | 23.207 | 522.59 | 597.42 | 28532 | 8640 |
| 384 kHz | 65.019 | 67.056 | 296.20 | 318.86 | 33424 | 34560 |

通常音源の初期化は約2ms、定常処理は約18%・14%・8%増えた。RSSは約8〜9 MiB増えている。上流では解析ネットワークが拡張され、音色解析の固定領域も初期化時に確保する。型サイズを同じコンパイラで調べると、`HarmNetInference`は257600から967036 byteへ増え、新しい`TimbreAnalysis`は5666608 byteだった。両者で約6.4 MB増えており、RSS増加の主な根拠となる。定常処理の増加には新しい解析モデルと発音処理が含まれ、ホストのファイル準備処理は含まれない。[解析の初期化](https://github.com/Frieve-A/effetune/blob/v2.14.0/dsp/plugins/analyzer/note_spectrogram/spectral_analysis.h)、[解析ネットワーク](https://github.com/Frieve-A/effetune/blob/v2.14.0/dsp/plugins/analyzer/note_spectrogram/harmnet_inference.h)

生成sineは48/96/384 kHzで7778.06/3891.57/307.15 ns/frame、noiseは943.43/547.29/259.90 ns/frameだった。sineのキー83では前二つのレートで変換倍率が8を超え、上流の係数キャッシュを使わないフィルターへ入る。384 kHzではキャッシュ対象となり負荷が下がる。生成処理自体の初期化は通常WAVと同程度であり、差は再生時のレート変換にある。外部DSPの演算条件や音声品質は変更していない。[上流フィルター](https://github.com/Frieve-A/effetune/blob/v2.14.0/dsp/plugins/others/sfz_note_player/kernel.cpp)

strip前のReleaseバックエンドはScalar 4184960、baseline SIMD 4185504、v3 4139776、v4 4270752 byteとなり、約14%増えた。各回の測定値・温キャッシュ結果・条件は`sfz-performance-updated-214.*`と`sfz-performance-extra.*`、型サイズは`model-size-213.log`と`model-size-214.log`、ライブラリサイズは`updated-backend-sizes.json`に保存した。

Releaseの実再生試験も5.41秒で成功し、`step4-release-runtime.log`へ保存した。最終の対象ビルドは警告なしで、工程4の公式PCM・発音・遅延・性能差の確認を完了した。

### 工程5の実績

共有バックエンドの回帰比較へ、2.14.0で参照PCMが変わったAM Radio Simulatorの5件、Vinyl Artifactsの1件、Phaserの1件、Tube Simulatorの6件を追加した。Analog MeterのCustomを含む6件、SFZの11件、Adaptive、Cassette、IRなどの既存goldenとともに、Scalar・baseline SIMD・x86-64-v3で成功した。参照PCMや許容誤差は変更していない。

解析9種類を100ノード並べたプリセットに、Analog Custom、Note Spectrogramの`te=true`、Oscilloscopeの`dt=10`を含めた。全ノードが警告なしの`ignored`となり、有効DSP数・遅延・PCM・バス転送に影響しないことを確認した。Noteモデルfixture、物理的な複数出力の割付・整列、再読込、IR・Room EQ・Crosstalk・生成FIR、GTK E2Eも成功した。アセット準備の上流比較は292件で成功している。

初回の全体試験はDebug 238件を1637.22秒、Release 237件を902.11秒で実行した。それぞれ単独ビルド試験のworkspace版比較だけが失敗した。工程4のコミット後にCMakeを再構成していなかったため、workspace実行ファイルはPipeTune 2.13.7、git由来の期待値は2.13.8だった。独立ビルド・EffeTune 2.14.0表示・共有バックエンドの音声比較は同じ試験内で成功しており、DSPや製品コードの修正は必要ない。原因と初回ログは`step5-debug-all.log`、`step5-release-all.log`へ残した。

CMakeを明示的に再構成し、警告なしで再ビルドした。失敗した単独ビルド試験はDebug 241.59秒、Release 250.53秒で成功し、`step5-*-component-recheck.log`へ保存した。これで工程5の未解決の失敗はなくなった。工程6の配布修正を統合した後も、コミットを固定して再構成し、最終ソースの全体試験を行う。
