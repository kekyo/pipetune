# EffeTune 2.18.0対応計画

## 目的と対象範囲

PipeTuneの音声処理に使うEffeTuneのDSPエンジンを2.17.0から2.18.0へ更新する。2.18.0のプリセットを読み込み、LinuxのPipeWireから入る音声を処理できることを確認する。利用者に関係する主な変更は、ネットワーク送受信の位置を示すマーカー、音響補正に使える測定データの条件、フィルターをリセットした直後の音である。

2026年11月19日の利用者回答に基づき、Live Audio Send／Receiveはプリセットの読み込みとローカル音声処理までを対象とする。ネットワーク機器との実際の送受信は別計画とする。音源については2.17.0の対応範囲を引き継ぎ、入力音から音符を検出して登録済みのSFZ音源を鳴らすSoundFont Note Playerを検証する。MIDI演奏とSF2/SF3の読み込みは前回の決定どおり別計画とする。[2.17.0対応計画](effetune-2.17.0.md)

上流のWindowsシステム音声入力はWindows版アプリ、ReplayGainは曲ごとの音量を調整する音楽プレーヤーの機能である。上流の音響測定画面で追加された測定方法について、PipeTuneが扱うのは、保存された測定結果をプリセットから参照する部分である。[公式の変更履歴](https://github.com/Frieve-A/effetune/blob/v2.18.0/docs/version-history.md)、[エフェクトを通した測定](https://github.com/Frieve-A/effetune/blob/v2.18.0/docs/measurement.md#measuring-with-the-effect-pipeline)

## 調査の基準

PipeTuneの基準は`develop`の`7139e2c25103326ccd1cb1e80e733bd06dc5be48`。作業ブランチは`feature/effetune-2.18.0`とする。次の2タグについて、手元のGitオブジェクトと公式リモートのコミットが一致することを確認した。[公式タグ](https://github.com/Frieve-A/effetune/releases/tag/v2.18.0)、[タグ間差分](https://github.com/Frieve-A/effetune/compare/v2.17.0...v2.18.0)

| 項目 | 現行 | 更新先 |
| --- | --- | --- |
| EffeTuneアプリタグ | `v2.17.0` | `v2.18.0` |
| `deps/effetune`のコミット | `d026b1664ec9444a66297efd36e251bc8aae3d05` | `738a3eab7f2e4ff02086805d04ae00a9dcaf5065` |
| ネイティブDSPの登録数 | 113 | 115 |
| DSPのABI番号 | 1 | 1 |
| DSPのCMakeプロジェクト版 | 1.16.0 | 1.16.0 |
| DSPのJavaScriptバインディング版 | 0.17.0 | 0.18.0 |

CLIとGTKに表示するEffeTuneの版はアプリの`package.json`から取得する。PipeTune自身の版は既存の`screw-up`で決まる。公式のC API宣言、エクスポート一覧、DSPコアにはタグ間差分がない。実際にロードした共有ライブラリと、ビルド時に生成したカタログの照合が版違いを検出する。[アプリ版](https://github.com/Frieve-A/effetune/blob/v2.18.0/package.json)、[DSPビルド定義](https://github.com/Frieve-A/effetune/blob/v2.18.0/dsp/CMakeLists.txt)、[API宣言](https://github.com/Frieve-A/effetune/blob/v2.18.0/dsp/include/effetune/abi.h)、[PipeTuneのローダー](../../../pipetune/src/dsp_backend.cpp)

実装では各工程の完了条件を先に試験にする。現行コードで失敗を再現してから修正し、同じ試験で成功を確認してコミットする。上流から取り込む変更は公式タグへの参照更新とし、製品への組み込みはPipeTune側で実装する。

## 工程0　2.17.0で比較用の音声とキャッシュを用意する

実装開始時に基準コミットでDebugの全体テストを実行する。Releaseでは同じプリセット、音源、サンプルレート、ブロック長を使い、SoundFont Note PlayerのClassic／Eventの出力波形と処理時間、最大常駐メモリを記録する。畳み込みフィルターにはリセット直後にインパルスを入力し、先頭ブロックを含む出力波形を保存する。

測定ファイルには、同じ周波数応答に対して`effectPipeline`があるものとないものを用意する。このキーは測定信号がエフェクトを通ったことを表す。2.17.0でそれぞれのRoom EQキャッシュを生成し、元の測定ファイルとともに隔離する。SFZも通常のサンプルと生成波形を使う音源で現行キャッシュを作る。完了条件は、同じ入力で版を比較でき、旧版のキャッシュを使った更新試験を再現できること。

## 工程1　115個のDSPを実際の共有ライブラリから読み込む

カタログの各項目には、パラメータを渡す浮動小数点値の個数と、その並びを識別するハッシュがある。タグ間で項目が増えたのはLive Audioの2個で、既存項目のパラメータハッシュが変わったのはSoundFont Playerだけである。[登録一覧](https://github.com/Frieve-A/effetune/blob/v2.18.0/dsp/registry.inc)、[公式メタデータ](https://github.com/Frieve-A/effetune/blob/v2.18.0/plugins/dsp/effetune-dsp.meta.json)

| DSP名 | 型名 | 2.17.0の個数・ハッシュ | 2.18.0の個数・ハッシュ |
| --- | --- | --- | --- |
| Live Audio Receive | `NetworkReceivePlugin` | 未登録 | 0・`0x811c9dc5` |
| Live Audio Send | `NetworkSendPlugin` | 未登録 | 0・`0x811c9dc5` |
| SoundFont Player | `SoundFontPlayerPlugin` | 4・`0x844a0998` | 8・`0xf271de51` |
| SoundFont Note Player | `SFZNotePlayerPlugin` | 21・`0x3785c6ed` | 21・`0x3785c6ed` |

版とパラメータを検査する`effetune_217_test.cpp`と、そのCTest登録を2.18.0用に更新する。旧版で版表示、登録数、新規2種、SoundFont Playerのパラメータが一致しないことを確認してから、サブモジュール参照を`738a3eab`へ更新する。既存の生成器がプラグイン一覧とパラメータ定義を読み、CMakeが新規カーネルを収集する。SoundFont Playerの最大同時発音数`pl`の既定値は64から256に変わる。新設の`af`、`pm`、`pc`、`sc`の既定値は順に`true`、`-1`、`0`、`0`である。公式パラメータパッカーとの比較で省略時、上下限、不正値を検査する。[SoundFont Playerの定義](https://github.com/Frieve-A/effetune/blob/v2.18.0/dsp/plugins/others/soundfont_player/params.json)、[カタログ生成器](../../../pipetune/tools/generate-dsp-catalog.mjs)、[パラメータ比較試験](../../../pipetune/test/parameter-packer-parity.mjs)

PipeTuneの共有ライブラリは上流の静的ライブラリと別のCMakeディレクトリでコンパイルする。上流が追加した`-ffp-contract=off`を共有側のVinyl SimulatorとTube Simulatorにも適用する。上流のTube SimulatorとAR補間器のテストでは、上流側の設定が有効かを確認する。i686固有の型配置・浮動小数点設定とARMのモデル生成も先行ビルドで確かめる。[上流のコンパイル設定](https://github.com/Frieve-A/effetune/blob/v2.18.0/dsp/CMakeLists.txt)、[共有バックエンドのビルド定義](../../../pipetune/cmake/EffeTuneNativeBackends.cmake)

完了条件は、CLIとGTKが`EffeTune DSP 2.18.0`を表示し、Scalarとホストで実行できるSIMDの各ライブラリが115個の定義と一致すること。2.17.0のライブラリを指定した場合の不一致も確認する。更新した製品で既存プリセットの音声サンプル列（PCM）を処理できたら、`feat: integrate EffeTune 2.18 native backends`でコミットする。

## 工程2　Live Audioマーカーを含むプリセットを再生する

EffeTuneではSendがその位置の音を送信し、Receiveが受信音の処理開始位置を指定する。通常のローカル入力は先頭から処理される。2個のネイティブカーネルの`process`は空で、送受信はアプリ側が行う。PipeTuneでは有効なマーカーを「無視」と表示し、ネットワーク送受信が未対応であることを警告する。マーカー自体または所属するSectionが無効なら「オフ」とし、そのマーカーの警告は出さない。[公式のマーカー説明](https://github.com/Frieve-A/effetune/blob/v2.18.0/docs/i18n/ja/plugins/control.md)、[送信カーネル](https://github.com/Frieve-A/effetune/blob/v2.18.0/dsp/plugins/control/network_send/kernel.cpp)、[受信カーネル](https://github.com/Frieve-A/effetune/blob/v2.18.0/dsp/plugins/control/network_receive/kernel.cpp)

前後に異なるVolume設定を置き、マーカーを取り除いたプリセットとPCMを比較する。マーカー単独、同種の重複、Sectionの切り替え、高いチャンネル番号と別バスを指定した場合を含める。登録だけ更新した状態で表示またはチャンネル数の誤りを再現する。`inferPresetChannelLayout`とノード構築の両方で同じ型名を判定し、ローカル入力を先頭から末尾まで処理する。[プリセット読み込み](../../../pipetune/src/dsp_pipeline.cpp)、[既存の未対応MIDIノードの試験](../../../pipetune/test/preset_pipeline_test.cpp)

完了条件は、通常のDSPの順番、処理・出力チャンネル数、バス、遅延、PCMが比較用プリセットと一致すること。有効なマーカーを実行DSP数に含めず、CLI・GTKで参照するプリセット状態に警告理由が届くことも確認する。コミットは`feat: recognize EffeTune live audio markers in presets`とする。

## 工程3　エフェクトを通した測定から補正を二重に作らない

2.18.0は測定JSONのルートに`effectPipeline: { pluginNames: [...] }`を保存する。上流で補正に使えるのは、このキーが存在しない測定だけである。PipeTuneの`readMeasurementSessionAsync`は現在、測定IDを確認してJSONを返す。Room EQとCrosstalk Cancellationのどちらもこの入口を通る。IR Reverbが参照するのは独立した`ir-library`内の音声ファイルである。[公式の測定データ判定](https://github.com/Frieve-A/effetune/blob/v2.18.0/js/measurement-store/client.js)、[測定データの読み込み](../../../pipetune/src/measurement_store.cpp)、[IRライブラリの読み込み](../../../pipetune/src/ir_asset_loader.cpp)

通常の測定IDと仮想チャンネル付きID（`id::ch=left`など）で、補正済み測定が使われる問題を先に再現する。次に共通の読み込み箇所でキーの有無を調べる。`pluginNames`が空の場合や値が`null`の場合も補正には使えない。キーがない測定は現在と同じように利用できる。Room EQで選択済みの測定が使えない場合は候補プリセットの適用を拒否し、稼働中の音声を保つ。Crosstalk Cancellationでは該当ノードを警告付きで除外し、残りのDSPを実行する。

Room EQは測定JSONを解析する前に、メモリ内の共有データとディスク上のキャッシュを検索する。旧版で補正済み測定から作ったフィルターもここで見つかるため、Room EQのキャッシュ識別子に新しい判定の版を含める。新識別子で保存するのは検査に通った測定だけとし、元ファイルの内容ハッシュを引き続き照合する。工程0の旧キャッシュから正常な測定は一度再生成され、補正済み測定は拒否されることを検証する。[Room EQのキャッシュと設計処理](../../../pipetune/src/room_asset_loader.cpp)、[キャッシュの製品試験](../../../pipetune/asset-preparation/test/tests/room-cache.test.ts)

完了条件は、通常・仮想チャンネルの両方で二重補正を防ぎ、正常な測定では従来の補正音が得られること。ファイルを書き換える試験で、拒否時の音声、測定を修復した後の再適用、以後のキャッシュ再利用まで確認する。コミットは`fix: exclude pipeline-processed measurements from correction`とする。

## 工程4　登録済みSFZ音源の発音とメモリを検証する

PipeTuneは通常のSFZを表形式v6で渡し、生成波形に変調情報が必要な場合はv7を使う。上流ではパッカーと再生エンジンが更新された。通常サンプルと生成波形の両方を公式パッカーと比較し、ループ、順番による選択、Classic／Eventの発音と遅延を製品のPCMで確認する。上流の65,536サンプルという上限はページ単位で読み込む音源のキャッシュに適用される。PipeTuneが登録ファイルやincludeの探索に設けている10,000件の制限は別の数量である。[公式パッカー](https://github.com/Frieve-A/effetune/blob/v2.18.0/js/sfz/asset.js)、[サンプルページの上限](https://github.com/Frieve-A/effetune/blob/v2.18.0/dsp/plugins/others/sfz/sample_cache.h)、[PipeTuneのSFZ読み込み](../../../pipetune/src/sfz_asset_loader.cpp)

工程0のSFZキャッシュを2.18.0で開き、原本から作り直した場合と波形を比較する。対応済みのバンク内容や読み込み条件に差が出たときは、再現テストを作り、該当するパッカーとキャッシュの版を更新する。どちらの読み込み経路でも、バンクのバイト数に公開APIが返す準備用メモリを足して容量を判定する。容量超過時には候補の適用を拒否し、稼働中の音源で演奏を続ける。製品が固定で予約する64 MiBに加え、レート・チャンネル数・ブロック長に応じた分も、変更後の再生エンジンに対して確認する。[公開APIの説明](https://github.com/Frieve-A/effetune/blob/v2.18.0/dsp/README.md#native-midi-sample-player-hosts)、[API宣言と見積もり対象](https://github.com/Frieve-A/effetune/blob/v2.18.0/dsp/include/effetune/abi.h)、[製品のメモリ計算](../../../pipetune/src/dsp_pipeline.cpp)

SoundFont Note Playerの公式参照PCMは13件とも同じで、更新は参照元ハッシュに限られる。MIDI用のSoundFont Playerにはプレビュー発音の参照PCMが2件追加された。共有バックエンドの試験でこの2件を実行する際には、現行ドライバーが固定している`ET_SAMPLE_PLAYER_NOTE`を音源の型から選ぶようにする。SoundFont Playerは`ET_SAMPLE_PLAYER_MAIN`、製品で使うNote Playerは`ET_SAMPLE_PLAYER_NOTE`で見積もる。[上流の参照ドライバー](https://github.com/Frieve-A/effetune/blob/v2.18.0/dsp/test/parity_runner.cpp)、[プレビューの公式2ケース](https://github.com/Frieve-A/effetune/blob/v2.18.0/dsp/plugins/others/soundfont_player/cases.json)、[PipeTuneの共有バックエンド試験](../../../pipetune/test/effetune_backend_artifact_test.cpp)

完了条件は、公式PCMとの比較、稼働中と候補の音源が共存する場合のメモリ試験、SF2/SF3が混在する一覧からのSFZ再生が成功すること。Releaseで現行版と同じ条件を使い、48・96・384 kHzの処理時間と最大常駐メモリを比較する。機能試験とメモリ試験は`chore: verify EffeTune 2.18 sample player behavior`、再現した製品上の不具合は原因ごとに`fix:`でコミットする。

## 工程5　リセット直後のフィルターと解析DSPの音を確認する

2.18.0では、準備済みのフィルターをリセットすると、最初のブロックから処理済みの音が出る。フィルターが有効で、交換時に原音を通すモード以外の場合に適用される。FIR Crossover、5Band FIR PEQ、Group Delay EQ、Group Delay PEQ、Room EQ、Crosstalk Cancellationの6種が対象になる。公式PCMでは先頭5種の`case-001`とCrosstalk Cancellationの`case-006`が更新された。Room EQの参照計算は`native-room-eq-direct-double-v3`になった。[Room EQのリセット処理](https://github.com/Frieve-A/effetune/blob/v2.18.0/dsp/plugins/eq/room_eq/kernel.cpp)、[公式参照計算](https://github.com/Frieve-A/effetune/blob/v2.18.0/dsp/test/parity_runner.cpp)

無音でフィルターの準備を終えた後にリセットし、先頭ブロックから有効な応答が出ることを測る。通常時と交換時の原音経路、報告する遅延、出力チャンネルを検査する。さらにPipeWireの休止・再開でも最初の出力を捕捉する。長い無音を先に流す試験では先頭の違いを見落とすため、工程0の出力と公式PCMを使って比較する。

Tonal Balance EQとRhythm Analyzerは三角関数などの計算を共通化した。Tonal Balance EQの`case-001/002/003/006`と、Rhythmのクリック有無を検証する。Hum RemoverではAutoが50 Hzから60 Hz系のハムを捕捉し、近い音程の持続低音では誤って切り替わらないことを、上流と同じ信号条件で確かめる。入力するフレーム数で試験を進める。[共通の数値計算](https://github.com/Frieve-A/effetune/blob/v2.18.0/dsp/include/effetune/dsp/portable_math.h)、[Tonal Balance EQの公式PCM](https://github.com/Frieve-A/effetune/tree/v2.18.0/dsp/plugins/eq/tonal_balance_eq/golden)、[Hum Removerの公式動作試験](https://github.com/Frieve-A/effetune/blob/v2.18.0/dsp/plugins/restoration/hum_remover/native_test.cpp)

完了条件は、更新された既存の参照PCM全10件が各ケースの公式許容誤差に収まり、製品でもリセット直後の音と遅延が一致すること。上流native試験と、配布用の共有バックエンドを直接ロードする試験をどちらも実行する。コミットは`chore: verify EffeTune 2.18 filter and analyzer output`とする。

## 工程6　全体テストと配布物で完了を判定する

日本語のREADMEと詳細説明に、対応エンジン版、マーカーの扱い、補正に使える測定の条件を記載し、英語版とバックエンドの説明にも反映する。更新内容を利用者が確認できたら`doc: describe EffeTune 2.18 support`でコミットする。

最終ソースを再構成し、DebugとReleaseで全体テストを実行する。上流のnative DSP、独立したアセット準備、コア単独ビルド、PipeWireの単一・複数出力、GTK E2E、インストールを含める。次のコマンドは実装時に使う。3分を超える試験は開始時にログを保存し、タイマーで終了後に結果を確認する。警告や失敗が出た場合は、原因と影響を確認して該当工程に戻る。

```sh
GDK_BACKEND=x11 make test TEST_BUILD_DIR=build/effetune-218-debug
cmake -S . -B build/effetune-218-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build/effetune-218-release --parallel
GDK_BACKEND=x11 ctest --test-dir build/effetune-218-release --output-on-failure
make -C pipetune/asset-preparation test BUILD_DIR=../../build/effetune-218-assets
./build_package_all.sh
```

既存の9配布構成は、Debian trixieのx86_64・i686・arm64・armv7l・riscv64と、Ubuntu 24.04／26.04の各x86_64・arm64である。各パッケージを対応OSに導入し、CLI・GTKの版と実際に配置された共有ライブラリで、115項目の照合、マーカー前後の音量、登録済みSFZの発音、Room EQの応答を確認する。CPUが実行できないSIMDについては対象名とビルド結果を記録する。[配布構成](../../../build_package.sh)

実装完了は、各工程の音声・診断・キャッシュの条件を満たし、最終ソースの全体テストと9配布構成の導入試験が成功した時点とする。使用コミット、再現時と修正後の結果、PCM誤差、性能差、警告への対処、配布物のSHA-256を`artifacts/verification/effetune-2.18/`に記録する。今回の計画作成で確認したのは公式タグの差分と現行コードの読み込み経路であり、上記の実装と実行試験はこれから行う。
