# EffeTune 2.15.0対応計画

## 目的と調査時点

PipeTuneが参照するEffeTuneを公式アプリタグ`v2.14.0`から`v2.15.0`へ更新し、2.15.0で保存したプリセットを利用できるようにする。SFZ Note Playerの新しい検出方式とMatrixのリセット修正を取り込み、音声・遅延補償・休止再開・配布物まで検証する。

調査日は2026-10-24。基準は`develop`の`9715d05ad237d06757ca0d0fc7596ec069a278b9`で、調査開始時の作業ツリーはクリーンだった。

| 項目 | 現行 | 更新先 |
| --- | --- | --- |
| 公式アプリタグ | `v2.14.0` | `v2.15.0` |
| コミット | `01c96f00f8e93595aefab96de4bc4d210e61c53b` | `45d2f69b63483c4a2e98758bd8a5715cb13e3266` |
| 同梱DSPバインディングの版 | 0.14.0 | 0.15.0 |
| 登録カーネル数 | 112 | 112 |

公式リモートの`v2.15.0`とローカルタグは同じコミットへ解決された。追従先はこのアプリタグに固定する。計画作成時には実装とサブモジュール参照を変更しない。[公式リリース](https://github.com/Frieve-A/effetune/releases/tag/v2.15.0)、[タグ間差分](https://github.com/Frieve-A/effetune/compare/v2.14.0...v2.15.0)

## 対応範囲と調査結果

### SFZ Note Playerの検出方式

SFZ Note Playerは入力音声から音符を検出し、登録されたSFZ音源を鳴らすDSPである。2.15.0は従来の`Classic`に加え、解析器が確定した音符の開始・終了を使う`Event`を追加する。プリセットの`dt`で選択し、省略時は`Classic`となる。

| 契約 | 2.14.0 | 2.15.0 |
| --- | --- | --- |
| SFZパラメータ数 | 19 float | 20 float |
| SFZパラメータハッシュ | `0x378e7f2b` | `0xdfb4bbc5` |
| 追加パラメータ | なし | `detection` / `dt`、`Classic=0`・`Event=1` |
| SFZアセットslot 0容量 | 1 GiB | 1 GiB |
| 公式参照PCMのケース数 | 11 | 13 |

`Event`では確定した開始イベントが発音条件となり、存在確率が回復しただけでは再発音しない。確定した音符の継続中はClassic用のしきい値を下回っても継続する。終了イベントとNote Holdの関係、同じ音高での再発音、モード変更・reset時の状態消去が検証対象となる。上流UIではEvent時にThresholdとRetriggerを無効にしているが、PipeTuneには設定UIを追加せず、保存されたパラメータを受け入れる。[パラメータ仕様](https://github.com/Frieve-A/effetune/blob/v2.15.0/dsp/plugins/others/sfz_note_player/params.json)、[発音規則](https://github.com/Frieve-A/effetune/blob/v2.15.0/dsp/plugins/others/sfz_note_player/onset_rule.h)、[アプリ側の保存パラメータ](https://github.com/Frieve-A/effetune/blob/v2.15.0/plugins/others/sfz_note_player.js)

解析を更新するフレーム間隔をhopと呼ぶ。Eventの解析遅延はClassicより7 hop増え、48 kHzでは6720フレーム、約140 msの差になる。SFZの報告遅延は選択方式の解析遅延、hopの半分、負の`tm`に対応する追加遅延の合計である。既存のdry遅延バッファもEventに対応する容量へ拡大されるため、Classicを含めてメモリ使用量を比較する。[SFZカーネル](https://github.com/Frieve-A/effetune/blob/v2.15.0/dsp/plugins/others/sfz_note_player/kernel.cpp)、[解析と遅延の定義](https://github.com/Frieve-A/effetune/blob/v2.15.0/dsp/plugins/analyzer/note_spectrogram/spectral_analysis.h)

上流のSFZ nativeテストはClassic/Event、音符の保持、再発音、モード変更、共有解析との一致、dry/wetの発音時刻を検証する。PipeTune側では実共有バックエンドと登録音源を通るテストを追加する。解析専用DSPの除外方針は維持し、SFZ自身の解析でEventを実行する。公開ヘッダーで`App-internal`と明記された`et_instance_set_analysis_source`は新たに使用しない。[上流nativeテスト](https://github.com/Frieve-A/effetune/blob/v2.15.0/dsp/plugins/others/sfz_note_player/native_test.cpp)、[C ABI宣言とコメント](https://github.com/Frieve-A/effetune/blob/v2.15.0/dsp/include/effetune/abi.h)

### Matrixのリセットと既存回避処理

Matrixはチャンネル間のルーティングを設定するDSPである。2.15.0では適用済みのルートと次回処理待ちのルートを、instance・engineのreset後も保持する。新規instanceだけが既定のステレオルートから始まる。[公式のreset契約](https://github.com/Frieve-A/effetune/blob/v2.15.0/dsp/README.md#structured-parameters)、[Matrixカーネル](https://github.com/Frieve-A/effetune/blob/v2.15.0/dsp/plugins/basics/matrix/kernel.cpp)、[上流の回帰テスト](https://github.com/Frieve-A/effetune/blob/v2.15.0/dsp/plugins/basics/topology_native_test.cpp)

現行`pipetune/src/dsp_pipeline.cpp`は、Matrixがreset時にルートを失うことへの対処として、構造化パラメータを`RetainedParameterBytes`へ保存し、`DspPipeline::reset()`で再送している。構造化パラメータを使う現行DSPはMatrixだけである。上流修正を実共有バックエンドで確認した後、保持構造体、メンバー、構築時の保持処理、reset時の再送処理を削除する。不要になる説明コメントも、この回避処理の削除に合わせて整理する。初回のパラメータ送信とMatrixのパッカーは維持する。

製品のPCMだけを確認すると、回避処理が残っていても成功する。このため、再送がなくなったことを実行時に確認する一時テストと、reset前後のPCMを確認する恒久テストを分ける。一時テストはテスト専用のバックエンドラッパーを使い、初回設定後に再送を拒否する条件でもresetと再生が成功することを検証する。ソース文字列や構造体名の有無では判定しない。

### ビルド、モデル、アセット準備

タグ間では`dsp/registry.inc`、公開C ABI、`dsp/core/`、上流CMake、DSP vendor、モデル生成ツールに変更がない。カーネル追加もない。Note SpectrogramとSFZが共用する解析処理にはイベント情報と状態管理の追加があるため、解析DSPを製品で除外していても、上流nativeテストとモデルfixtureテストを実行する。

`pipetune/cmake/EffeTuneNativeBackends.cmake`の共有バックエンド生成、`-ffp-contract=off`、i686向け条件、ARMモデル変換を継続する。ABIパッチは2.15.0の`core/abi.cpp`を一時コピーし、`patch --dry-run --fuzz=0`で適用できた。実装時には通常のビルドでも確認する。上流のEmscripten指定は6.0.11から6.0.13へ更新されるが、PipeTuneのネイティブ製品にWASMビルドを追加する必要はない。[上流CMake](https://github.com/Frieve-A/effetune/blob/v2.15.0/dsp/CMakeLists.txt)、[上流ビルド要件](https://github.com/Frieve-A/effetune/blob/v2.15.0/dsp/README.md)

SFZの文字列解析・波形生成・サンプル選択・バンク生成を行う`js/sfz/`には差分がない。独立C++準備処理、ARIA bank・文字コード対応、ETA1/table-v2形式は継続する。SFZキャッシュの識別子にある2.14.0コミットは準備処理の出典として保持し、DSP版の更新だけを理由にキャッシュを無効化しない。2.14.0で準備したキャッシュを2.15.0から読む検証と、空キャッシュからの検証を行う。IR・Room EQの準備処理と出典情報も、差分のない部分は維持する。[SFZ準備処理](https://github.com/Frieve-A/effetune/tree/v2.15.0/js/sfz)

### 対象外

上流アプリの内蔵ブラウザ、動画再生、VisualizerのVideo/Counter、macOS向けALAC再生、配布ストア対応は今回の移植対象に含めない。PipeTuneの目的に必要なDSPとプリセットの追従を行う。旧DSPとの互換モード、新しい解析リンクAPI、PipeTune自身の版変更は追加しない。外部サブモジュール・vendorソースは編集せず、公式コミットへの参照更新とPipeTune側の変更で対応する。

## 主な変更・検証箇所

| 箇所 | 作業内容 |
| --- | --- |
| `deps/effetune` | 公式2.15.0コミットへ参照更新 |
| `pipetune/CMakeLists.txt`、`pipetune/cmake/EffeTuneNativeBackends.cmake` | 版取得・生成依存・テスト登録・全バックエンドを確認。必要な登録変更だけを行う |
| `pipetune/tools/generate-dsp-catalog.mjs` | 既存の生成経路が20 floatと新ハッシュを取り込むことを確認。手書きパッカーは追加しない |
| `pipetune/test/effetune_214_test.cpp` | 現行版スモークとして`effetune_215_test.cpp`へ改名し、契約を更新。既存DSPの検証は残す |
| `pipetune/test/effetune_backend_artifact_test.cpp` | SFZ契約とパラメータ配列を更新。Scalar/SIMD双方で13件のSFZ参照PCMとMatrix resetを検証 |
| `pipetune/test/parameter-packer-parity.mjs` | `dt`の既定値、Classic/Event、無効値を上流JSと比較 |
| `pipetune/src/dsp_pipeline.cpp` | Matrix用の保持・再送処理を削除。Event SFZ instanceの構築時・reset時に1 hopの無音で解析を初期化 |
| `pipetune/test/sfz_runtime_test.cpp` | Eventの登録再生・遅延・再構築・休止再開・複数instanceを追加 |
| `pipetune/test/preset_pipeline_test.cpp`、`dsp_pipeline_slot_test.cpp`、`product_output_test.cpp`、`multi-device-output.mjs` | Matrixのreset・休止再開と、SFZ両方式の複数出力PCM整列を検証 |
| `pipetune/asset-preparation/test/`、アセット関連テスト | 準備処理の上流比較、キャッシュ、監視、異常時の状態保持を回帰確認 |
| `README_ja.md`、`README.md`、`docs/ja/details.md`、`docs/en/details.md` | 対応版、Eventの選択元と遅延を日本語原本から反映 |
| `pipetune/README.md`、`pipetune/docs/dsp-backends.md`、`pipetune/docs/asset-preparation.md` | 対応版、SFZの13参照PCM、アセット準備の出典との関係を更新 |

## 段階的な実施計画

実装開始時の`develop`を基準に作業ブランチを作る。各工程でビルドと対象テストを実行し、実共有ライブラリ、CLI、既存ドライバーで成果を観測できる状態をコミットする。mainへのマージは行わない。

コード修正は挙動を示すテストのREDを確認してから実装し、GREENを確認する。参照更新だけで成立する検証には、人為的な失敗を作らない。上流由来の変更で期待値を変える場合は、仕様と参照データを照合する。新たな外部APIを使う場合は公式文書とAPIコメントの両方を確認し、内部用APIは使わない。

### 0 現行の基準を記録する

現行2.14.0でDebug全体テストを実行する。実行可能なCPU命令セット、ビルド依存、Podman環境、ディスク・メモリを確認する。管理者権限が必要な不足パッケージは、必要なものと導入方法を案内する。

ReleaseでSFZ Classicの初期化時間、処理時間、報告遅延、最大RSS、共有バックエンドのサイズを記録する。48/96/384 kHz、同じ登録音源・入力PCM・ブロックサイズ・反復回数で比較し、ビルドや別試験と同時に性能測定しない。2.14.0で作った温キャッシュを更新後の検証用に残す。

完了条件は現行の失敗と更新による失敗を区別でき、同一条件で性能・メモリ・キャッシュを比較できること。記録は次の実装コミットへ含める。

### 1 2.15.0の最小実行を成立させる

2.15.0の版・20 float・新ハッシュ・Eventの受け入れを実共有バックエンドで検証するテストを追加し、2.14.0でREDを確認する。Matrixの適用済み・処理待ちルートをinstance/engine reset後に処理するテストも、PipeTuneの回避処理を経由しない共有バックエンドでREDを確認する。

サブモジュール参照を更新し、固定値と現行版テスト名・CMake登録を更新する。`dt`省略・Classic・Event・無効値のパッカー比較を行い、生成物と実カーネルの契約を揃える。公式SFZ参照PCMは既存11件を維持して、新規の`event-sine-bank`と`event-dry-negative-timing`を加えた13件へ更新する。既存11件のPCM本体にはタグ間差分がなく、JSONの`dt`と参照ハッシュの更新を音声変更と混同しない。

CLI・GTKの版表示、単純な登録SFZのClassic/Event再生、Scalarとホストで実行可能なSIMDを確認する。上流SFZ・Note Spectrogram・topology nativeテストも実行する。i686・armv7lのビルドとロードを早期に確認し、モデル・リンク・浮動小数点条件の問題を切り分ける。

完了条件は、CLI・GTKが2.15.0を表示し、112カーネルの契約が一致し、両モードで有限PCMを得られ、SFZの13件とMatrixのreset検証が成功すること。新しい警告は原因を分析して対処する。

コミット: `feat: update EffeTune integration to 2.15.0`

### 2 Matrixの回避処理を削除する

工程1の上流修正を前提に、初回設定後の構造化パラメータ再送を拒否する一時テストを追加する。現行の再送処理が残る状態でresetが失敗するREDを確認してから、保持・再送処理を削除しGREENを確認する。

恒久テストでは非既定ルート、空ルート、重複加算、位相反転、16チャンネル、処理前と処理後のresetを確認する。製品経路でもMatrixを含むプリセットの初回準備、繰返しreset、無音休止からの再開、複数出力のPCMを確認する。

完了条件は、再送を必要とせず上流がルートを保持し、製品出力がreset前後で同じ意味を保つこと。不要な保持コードが残らないこともレビューする。

削除とGREEN確認を`refactor: remove Matrix reset parameter restaging`でコミットする。その後、一時テストとテスト専用のバックエンドラッパーを削除し、恒久テストを実行して`chore: remove temporary Matrix restaging removal test`を作る。通常のPCM回帰テストは残す。

### 3 SFZ Eventの製品動作を検証する

登録音源を使い、実入力からの発音・終音・Note Hold・同音高の再発音・reset後の再現性を検証する。モード変更はPipeTuneの通常のプリセット再読込で確認し、稼働中instanceの直接切替は共有バックエンドの検証と上流nativeテストで確認する。

遅延は両モードについて上流の`instanceLatency`とPipeTuneの報告値・実dryインパルス位置を照合する。44.1/48/96/192/384 kHz、`tm=-100/0/100`、部分ブロック、選択ペア・未選択チャンネル、別バス、最終複数出力の整列を確認する。全条件を無制限に掛け合わせず、基本条件と各境界を分けて網羅する。Eventの増加量は7 hopから求め、全レートを固定6720フレームとしない。

レート・バックエンド・プリセット変更時の再構築、無音休止オン/オフ、アセット変更・取消・欠損復旧・読込失敗時の旧パイプライン保持を確認する。2.14.0の温キャッシュと空キャッシュの双方で再生し、モードの違いだけでは準備済みバンクの再生成を要しないことを確かめる。Node.jsとffmpegコマンドのない実行環境でも検証する。

製品テストで欠落が見つかった場合は、その挙動をREDで再現したうえでPipeTune側の根本原因を修正する。音声コールバックへファイル読込、アセット準備、不要な割当・解放を加えない。単なる時間待ちではなく、処理フレーム数や状態遷移を条件に判定する。

先行検証で、Eventはリセット直後から音があると最初の発音を抑止することが分かった。無音休止からの復帰でも同じ条件になる。上流の解析器は最初の解析フレームで音符の開始通知を抑止するため、EventのSFZ instanceに限り、構築時とreset時に1 hop分の無音を処理して初期化する。公開`instanceProcess`と既存のarenaを使い、他のDSPの状態は進めない。プレビューを指定している場合は初期化中だけ無効にし、音源の先頭を保つ。初回発音、休止再開、reset後のPCM再現性、プレビューの先頭、dry遅延が変わらないことを検証し、Releaseでresetの追加費用も測る。[開始通知の抑止](https://github.com/Frieve-A/effetune/blob/v2.15.0/dsp/plugins/analyzer/note_spectrogram/spectral_analysis.h)、[公開処理API](https://github.com/Frieve-A/effetune/blob/v2.15.0/dsp/include/effetune/abi.h)

完了条件は、Eventの音声・遅延・再構築・休止再開が製品経路で成立し、Classicと既存アセット処理に退行がないこと。

コミット: `feat: validate SFZ Event detection in PipeTune`。独立した不具合修正が必要なら、この工程内で再現テストとともに`fix:`へ分ける。

### 4 全体回帰、配布、利用者向け文書を完成させる

日本語READMEを原本として、対応版とSFZ Eventの選択元・追加遅延を利用者向けに説明し、英語へ反映する。準備処理の出典版は機械的に更新しない。日本語は`nanaism/yomiyasu`で確認する。一般文書から本計画へのリンクは追加しない。

最終ソースでDebug/Releaseの全体テスト、コア単独ビルド、独立アセット準備、GTK E2E、インストールを検証する。解析9種類の除外、Rhythm Analyzerのクリック出力、既存DSP参照PCM、IR Reverb・Room EQ・Crosstalk Cancellation・FIR生成、レート切替、複数デバイス出力も既存テストで回帰確認する。参照PCMの再生成や許容誤差の緩和で失敗を解消しない。

既存の9配布構成をビルドし、導入後の版表示・共有バックエンドのロード・SFZ両モード・MatrixのPCMを検証する。対象はDebian trixieのx86_64・i686・arm64・armv7l・riscv64、Ubuntu 24.04/26.04のx86_64・arm64。CPU上で実行できない命令セットは、ビルド成功と実行未確認を分けて記録する。エミュレーション時間を実機性能と混同せず、遅延理由を調べて必要なタイムアウトを確保する。

同条件のRelease測定でClassicの更新前後とEventの追加負荷・メモリを比較する。明確な退行は原因を調査し、上流由来でPipeTune側の対処が必要なら、根拠と対応を計画へ反映してから修正する。

完了条件は最終完了条件を全て満たし、検証ログ・性能差・配布物ハッシュと未実施事項を明示できること。ソース変更後は該当検証をやり直し、最後のコード変更後に全体テストを必ず実行する。

コミット: `doc: describe EffeTune 2.15 support and verification`

## 検証の実行と記録

全体検証には既存のMakefileとCMake/CTest経路を使う。GTKは過去の検証と同じく`GDK_BACKEND=x11`を指定し、Waylandセッションの継承による差を避ける。

```sh
GDK_BACKEND=x11 make test TEST_BUILD_DIR=build/effetune-215-debug
cmake -S . -B build/effetune-215-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build/effetune-215-release --parallel
GDK_BACKEND=x11 ctest --test-dir build/effetune-215-release --output-on-failure
make -C pipetune/asset-preparation test BUILD_DIR=../../build/effetune-215-assets
DESTDIR="$PWD/artifacts/verification/effetune-2.15/final-install" cmake --install build/effetune-215-release --prefix /usr
./build_package_all.sh
```

工程1〜3の個別検証には`ctest --test-dir build/effetune-215-debug -N`で登録名を確認してから、`pipetune_effetune_215_tests`、`pipetune_effetune_backend_artifact_tests`、`pipetune_parameter_packer_parity`、`pipetune_sfz_runtime_tests`、Matrix/出力系と上流nativeテストを選ぶ。個別検証の成功を全体成功として扱わない。

バージョンはCMake構成時に取得するため、サブモジュール更新後とコミット後には再構成する。コア単独ビルドと親ビルドの版が揃ってから比較する。新たなスクリプトはNode.js、ビルド時のテキスト処理は既存のfuncityを使う。ブラウザを介したUI試行・E2EにはPlaywright MCPを使う。

コマンド、対象コミット、環境、RED/GREEN、PCM誤差、性能・メモリ、パッケージの検証結果は`artifacts/verification/effetune-2.15/`へ保存し、工程の実績を本書へ追記する。計画外の問題は前提を見直してから対応し、計画とテストの要求が矛盾する場合は確認してから進める。

## 実装の最終完了条件

- [ ] 公式`v2.15.0`の指定コミットを参照し、外部ソース・vendorへの直接変更がない。
- [ ] CLI・GTKが2.15.0を表示し、112カーネルとSFZの20 float・新ハッシュ・アセット容量が一致する。
- [ ] SFZのClassic/Eventと省略時の既定動作が一致し、公式13件の参照PCMがScalarと実行可能なSIMDで成功する。
- [ ] Matrixが適用済み・処理待ちルートをreset後も保持し、PipeTune側の保持・再送処理と削除確認用の一時テストを除去している。
- [ ] Eventの発音・終音・遅延・チャンネル/バス整列・再構築・休止再開を製品経路で確認した。
- [ ] SFZの冷/温キャッシュ、既存アセットと監視・取消・失敗時の旧状態保持に退行がない。
- [ ] 最終コードでDebug/Release全体テスト、単独ビルド、GTK E2E、インストール、9配布構成を検証した。
- [ ] 日本語原本と英語文書、性能・メモリ比較、警告への対処、工程ごとのコミットと検証記録が揃っている。

## 計画作成時の確認結果

現行コード、2.14.0の対応計画と実績、公式2.15.0タグと差分、公式文書とAPIコメントを照合した。更新先コミット、SFZのパラメータ変更と13件の参照PCM、Matrixの保持契約とPipeTuneの回避処理、アセット準備処理に差分がないことを確認した。ABIパッチのfuzzなし適用も一時コピーで確認した。

計画作成時点では、上記の実装完了条件は全て未実施だった。文書追加はビルドへ影響しないため、計画作成時の製品ビルド・全体テストは実行していない。

## 実施記録

### 工程0の基準

作業ブランチは`feature/effetune-2.15.0`。計画コミット`5343f41`とEffeTune 2.14.0でDebug/Releaseをビルドし、Debug全238テストが成功した（506.42秒）。この全体テストにはコア単独ビルドとGTK E2Eも含む。ビルドのコンパイラ警告はなかった。

ホストはIntel Core i9-12900KS、論理24 CPU、メモリ62 GiB、GCC 13.3、CMake 3.28.3。Scalar、baseline SIMD、x86-64-v3を実行でき、x86-64-v4はCPU非対応となる。9配布構成のPodmanイメージも利用できた。環境の詳細は`environment.json`、ビルドと全体テストは`baseline-*.log`に保存した。以下、記録ファイルは`artifacts/verification/effetune-2.15/`を基準とする。

Releaseの性能測定はCPU 8へ固定し、他のビルド・試験と重ねずに行った。入力は1 kHzの8倍音を含むステレオPCM、ブロックは256フレーム、測定前に1秒分を処理し、その後256ブロックを測定した。音源は48 kHzのモノラルfloat32 WAV、4410フレーム、key 83である。各条件を3回測定した中央値を示す。RSSはプロセス全体の最大値であり、SFZだけの使用量ではない。

| レート | キャッシュ | 準備 ms | 処理 ns/frame | 最大RSS KiB | 遅延 frames |
| --- | --- | ---: | ---: | ---: | ---: |
| 48 kHz | 冷 | 15.374 | 888.803 | 27640 | 4320 |
| 48 kHz | 温 | 13.942 | 887.968 | 26640 | 4320 |
| 96 kHz | 冷 | 22.471 | 525.603 | 28524 | 8640 |
| 96 kHz | 温 | 21.195 | 523.519 | 27500 | 8640 |
| 384 kHz | 冷 | 65.862 | 244.751 | 33384 | 34560 |
| 384 kHz | 温 | 64.478 | 247.373 | 32336 | 34560 |

全測定でPCMは有限かつ非無音だった。冷キャッシュのヒット数は0、温キャッシュは1。更新後の読込確認用に、基準測定で作成したキャッシュを保持した。生データと条件は`sfz-performance-baseline-214.json`、共有バックエンドのサイズは`baseline-backend-sizes.json`に保存した。

### 工程1の最小更新

2.15.0の契約を要求するスモークを2.14.0の共有バックエンドで実行し、バージョン、SFZの20 floatとEvent指定、Matrixの適用済み・処理待ち・空ルートのreset保持でREDを確認した。ログは`step1-red/result.log`。その後、公式コミット`45d2f69b63483c4a2e98758bd8a5715cb13e3266`へ参照を更新した。

現行バージョンのスモークを`effetune_215_test.cpp`へ改名し、カタログの契約とパッカー比較を更新した。ホストでは8件の対象テストが成功した（144.76秒）。SFZの公式13参照PCM、Matrixのinstance/engine reset、上流SFZ・Note Spectrogram・モデルfixture・topologyを含む。CLI・GTKはともに`EffeTune DSP 2.15.0`を表示し、コンパイラ警告はなかった。ログは`step1-targeted.log`、表示は`step1-versions.json`、参照PCMの誤差は`step1-golden-metrics.log`に保存した。

Debian trixieのi686・armv7lを先行ビルドし、導入済み共有ライブラリでPCMを確認した。i686はScalar・baseline SSE2・v3、armv7lはScalar・baseline NEONの計5種類を実行でき、非対応CPUによる除外はなかった。全てで112カーネル、SFZの20 float・新ハッシュ・容量、Matrix reset、登録SFZのClassic/Event発音とdry遅延を確認した。`tm=-100`、48 kHzでの遅延はClassicが9120、Eventが15840フレームだった。ビルド・導入・音声試験は全て成功し、警告・エラーはなかった。ARMのGCC 7.1以降の引数ABI変更に関する出力は情報通知であり、各構成を同一コンパイラでビルドしている。

先行配布物のPipeTuneバージョンは既存の生成規則による2.14.2で、EffeTune DSPのバージョンは2.15.0。対象ソースの未コミット差分、配布物のハッシュ、ログは`early32-*`、音声結果は`package-pcm-early.json`に保存した。これらは工程2・3の修正前の成果物であり、最終工程では9構成を再生成する。

工程1の完了条件を満たした。製品経路のEvent初回発音・休止再開について見つかった問題は、工程3に追記した方針で修正する。

### 工程2のMatrix整理

初回設定後の構造化パラメータ送信を拒否する一時テストを追加し、既存の再送処理によってresetが失敗するREDを確認した（`step2-red.log`）。公開されたGNU ldの`--wrap`でローダーの`dlsym`だけを置き換え、実際の共有バックエンドで処理した。製品コードにはテスト用の分岐を加えていない。[リンカーのラップ機能](https://sourceware.org/binutils/docs/ld/Options.html)

保持構造体、保持メンバー、構築時の保存、reset時の再送を削除し、初回送信は残した。一時テストと恒久テスト7件が成功した（41.60秒）。この状態を`8b37904`でコミットした。恒久テストでは非既定・空・重複加算・位相反転・16チャンネルのルート、処理前後のreset、無音休止からの再開をPCMで検証する。既存のプリセットと複数出力のMatrix試験も成功した。

その後、一時テストとCMake登録を削除し、恒久テスト6件が成功した（41.27秒）。ログは`step2-green.log`と`step2-clean.log`、詳細は対応するXMLに保存した。ビルドのコンパイラ警告はなく、工程2の完了条件を満たした。

### 工程3のSFZ Event製品動作

登録SFZ音源を使う製品試験をClassicとEventの両方式に拡張した。44.1/48/96/192/384 kHzで上流の`instanceLatency`と製品の報告遅延を照合し、Eventの追加7 hop、負の`tm`、dryインパルス位置、wet/dry混合、部分ブロック、選択ペア・未選択チャンネル、16チャンネルを確認した。通常のプリセット再読込による方式変更、別バス、レート・バックエンド再構築、無音休止からの再開、終音とNote Hold、同じ音高の再発音、プレビュー先頭とreset後のPCM再現性も試験した。4チャンネルに2つのEvent SFZ instanceを配置し、プレビューあり・なしの両ペアが発音してreset後にPCMが一致することを確認した。

最初の対象試験ではClassicの再生ケースが完了した後、次のEvent 44.1 kHzの登録音源再生でREDとなった。失敗した方式は`step3-red.log`の出力順と`sfz_runtime_test.cpp`の呼出順から特定した。無音休止後の発音も失われ、先頭から有音の入力では最初の音符開始が抑止されることを診断した（`step3-draft/idle-first.log`など）。上流の解析器は最初の解析フレームで開始通知を抑止する。Event SFZ instanceだけに公開`instanceProcess`で1 hopの無音を構築時とreset時に通し、既存arenaを再利用した。プレビューは初期化中だけ無効化して音源先頭を保つ。他のDSP instanceの状態は進めない。この修正と製品試験を`1ea37d5`でコミットした。修正後、SFZ runtime、共有バックエンド参照PCM、プリセット、アセットキャッシュ・ライフサイクル、現行版の6対象テストが成功した（`step3-green.log`）。Node.jsとffmpegコマンドのない環境でもClassic/Eventの各レートと16チャンネルの試験が成功した（`step3-runtime-no-tools.log`）。ビルドに新しいコンパイラ警告はなかった。

2.14時に作成した温キャッシュをコピーし、元のWAV、SFZ、登録情報、Classicプリセットを変更せず、48/96/384 kHzのClassic/Eventで再生した。6条件すべてで`cachedAssetCount==1`、有限で有音のPCM、Eventの追加7 hopを確認した。元のアセットとキャッシュのSHA-256・サイズ・更新時刻は試験前後で一致した。これは機能互換の確認であり、Debugの処理時間をRelease性能比較には使わない。結果とコマンドは`sfz-214-cache-reuse.json`、実行ログは`sfz-214-cache-reuse-run.log`に保存した。

複数出力には、端末間に63フレームの遅延差を設けたSFZ Classic/Eventの2ケースを追加した。試験fixtureで異なる処理周期の端末バッファを時刻差からPCM位置へ換算すると、連続したPCMでも補償量を誤判定することが分かった（`step3-output-diagnostic.log`）。実時間driverでの新ケースには低頻度のPCM連続性失敗もあった。Debug単体で観測した256フレーム処理時間の最大はClassicが1.956 ms、Eventが1.954 msで、48 kHzの実時間周期5.333 msを下回った。失敗の原因をSFZ処理の期限超過とは断定できない。処理速度に依存せず機能を確認するため、新しいSFZ 2ケースにはPipeWireのfreewheel driverを使用し、同一周期のPCM補償62〜63フレーム、連続した32768検証済みフレーム、両出力経路のactive状態とserialを確認した。この環境のfreewheel試験では`reportedLatency`が未設定だったため、従来の遅延・遅延単位・再接続・Time Alignmentの4ケースは実時間driverを維持し、報告遅延も照合した。6ケースを各10回反復し、全60実行が成功した（51.59秒、`step3-output-final.log`とXML）。製品の複数出力処理には変更を加えていない。試験ビルドに新しいコンパイラ警告はなかった。[PipeWireのdriverとgroup](https://docs.pipewire.org/devel/page_running.html)、[freewheelの定義](https://docs.pipewire.org/group__spa__node.html)、[キャプチャバッファの時刻](https://pipewire.pages.freedesktop.org/pipewire/structpw__buffer.html)

以上により、Eventの音声・遅延・再構築・休止再開が製品経路で成立し、Classicと既存アセット処理に退行がないという工程3の完了条件を満たした。全体テスト、Release性能、9配布構成は後続工程で判定する。
