# EffeTune 2.18.1対応計画

PipeTuneの音声処理に使うEffeTuneのDSPを2.18.0から2.18.1へ更新する。共有ライブラリに登録される115個のDSPカーネルを照合する。入力音から音符を検出するSoundFont Note Playerでは、登録済みのSFZ音源を更新後も同じ音と遅延で再生できることを確かめる。

## 更新元と更新先

基準はPipeTuneの`develop`の`b8f40a166262937be300179a65d07d8800b97f4d`、作業ブランチは`feature/effetune-2.18.1`とする。公式リモートと手元のタグを照合した。上流の差分は1コミットで、アプリの`v2.18.1`とDSPの`dsp-v0.18.1`は同じコミットを指す。[公式リリース][release]、[上流の変更差分][change]

| 確認項目 | 2.18.0 | 2.18.1 |
| --- | --- | --- |
| `deps/effetune`のコミット | `738a3eab7f2e4ff02086805d04ae00a9dcaf5065` | `7de52ecc9158888dd437411aed438894d2ab2d1a` |
| CLIとGTKに表示するEffeTuneの版 | 2.18.0 | 2.18.1 |
| DSPのバイナリ互換性を示すABI番号／登録カーネル数 | 1／115 | 1／115 |
| 上流のDSPバインディング版／DSPのCMakeプロジェクト版 | 0.18.0／1.16.0 | 0.18.1／1.16.0 |

PipeTuneはEffeTuneアプリの`package.json`から表示用の版を読む。DSPはC++ソースから、互換性を重視するScalar版とCPUのベクトル命令を使うSIMD版の共有ライブラリにビルドする。両タグでC APIと115件のカーネル定義は一致した。公式メタデータで変わった項目はソースのハッシュとWebAssemblyバイナリのサイズである。現行ローダーはABI、型名、パラメータのハッシュ、データ容量を照合するため、この2版の識別にはビルド元のコミットと配布物の記録を使う。[ビルド定義][build]、[ローダー][loader]、[公式メタデータ][metadata]

## PipeTuneに関係する変更

| 上流で変わった動作 | PipeTuneで確かめること |
| --- | --- |
| SFZ再生エンジンで、一時停止中の演奏状態を復元するときに、発音までの待ち時間をフレーム数で検査する。上限内の値だけを復元先のサンプルレートに換算する。 | 共通のC++エンジンを取り込む。演奏状態の復元は上流のネイティブ試験で確認する。PipeTuneの共有ライブラリが公開するAPIにMIDIの状態復元は含まれない。[復元処理][engine]、[公開シンボル][exports] |
| MIDIトラックを切り替えた直後、前の曲の読み取り位置が残っているときは、新しい曲のイベント列の先頭から先読みに必要なSFZサンプルを調べる。 | この修正はEffeTuneアプリの音声処理を担うAudioWorkletにある。PipeTuneで試すのは登録済みSFZをメモリに読み込んで鳴らすNote Playerと、MIDI用のSoundFont Playerを含むプリセットの警告・音声出力である。[上流の先読み試験][forecast]、[SFZの読み込み][sfz-loader]、[プリセット処理][pipeline] |
| デスクトップ版のChromecast出力で、受信機の応答が遅れても推定遅延を保持し、新しい推定値が安定してから同期を調整する。 | PipeTuneではLive Audio Send／Receiveを含むプリセットを読み、有効なマーカーに警告を表示して前後のエフェクトをPipeWireの音声に適用する。[上流の同期試験][cast]、[プリセット処理][pipeline] |

この範囲は前回の対応計画を引き継ぐ。MIDI演奏とSF2/SF3音源の読み込み、ネットワーク機器との送受信には別の計画を設ける方針である。[2.18.0対応計画][previous]

## 工程1　2.18.1を組み込み、版表示とプリセット再生を確認する

更新前に2.18.0で比較用の音声サンプル列（PCM）を採る。既存のアセット試験と同じように、検証用ディレクトリに700 Hzのサンプルを使うSFZ音源を1個登録する。`pipetune_asset_pipeline_driver`でScalarを選び、48 kHzのステレオで1 kHzの音を入力する。登録音源の700 Hzが出ることを確かめ、出力PCM、報告遅延、準備済み音源のディスクキャッシュを保存する。工程2まで登録先の絶対パスと音源を保つ。[ドライバー][driver]、[登録済み音源の試験][registered]

既存の`pipetune_effetune_218_tests`の期待版を2.18.1に変え、2.18.0では版の検査が失敗することを確認する。次に`deps/effetune`の参照を更新先のコミットに進め、新しいビルドディレクトリでCLI、GTK、共有DSPをビルドする。PipeTune自身の版には既存の`screw-up`の値を使う。[版とカーネルの試験][version-test]、[ビルド定義][build]

完了条件は、同じ試験が成功し、`pipetune --version`と`pipetune-gtk --version`がともに`EffeTune DSP 2.18.1`を表示すること。ScalarとCPUで実行できる各SIMD版で115件の定義が一致し、試験用プリセットの左右から期待する音量のPCMが得られたら、`chore: update the EffeTune DSP to 2.18.1`でコミットする。

## 工程2　SFZの発音、演奏状態の復元、ローカル音声を検証する

上流の参照音声はSoundFont Note Playerが13件、MIDI用SoundFont Playerのプレビューが2件ある。全15件で変更されたのは参照元を示す`referenceHash`だけで、PCMファイル、入力条件、許容誤差は同一だった。`pipetune_effetune_backend_artifact_tests`で各共有ライブラリの出力を公式PCMと照合する。状態復元は上流の`effetune_dsp_soundfont_player_tests`で、48 kHzで一時停止した演奏を44.1、48、96 kHzで復元した後の発音まで確かめる。[Note Playerの参照音声][note-goldens]、[プレビューの参照音声][main-goldens]、[共有ライブラリの試験][artifact-test]、[上流の復元試験][checkpoint-test]

工程1のキャッシュを2.18.1のドライバーから読み込む。同じ音源を空のキャッシュから準備した場合とも比較する。キャッシュを使った実行では`cacheHits=1`、新規生成では`cacheHits=0`となり、どちらでも発音と報告遅延が2.18.0の記録と一致することを確認する。SFZのバンク形式と、準備済み音源を識別するための入力にタグ間差分はない。[キャッシュ識別子][sfz-loader]、[登録済み音源の試験][registered]

製品経路では`pipetune_sfz_runtime_tests`とPipeWireの`product-sfz-classic`、`product-sfz-event`で2種類の音符検出方式を試す。`pipetune_preset_pipeline_tests`ではMIDI用のPlayerとLive Audioマーカーについて、有効時の警告、無効時の表示、前後のエフェクトを通した音量とチャンネル数を照合する。[発音と遅延の試験][runtime-test]、[プリセットの試験][preset-test]

完了条件は、15件のPCMが各ケースの公式許容誤差に収まり、上流の復元試験と製品経路の試験がすべて通ること。差が出た場合は、該当する製品経路で失敗を再現する。原因を修正して同じ試験の成功を確認し、`fix:`で個別にコミットする。検証結果はこの計画書に記載し、`doc: record EffeTune 2.18.1 playback verification`でコミットする。

## 工程3　全体テストと配布パッケージで確認する

日本語READMEを原本として英語READMEにも対応版を反映する。コアのREADME、アーキテクチャ、DSPバックエンド、アセット準備の各文書では、現在の組み込み版と参照コミットを更新する。測定方法の導入やキャッシュ移行を説明する箇所には、その変更が入った版を記す。文書は`doc: describe EffeTune 2.18.1 support`でコミットする。

最終ソースからDebugとReleaseの全体テスト、独立したアセット準備試験、全配布構成のビルドを実行する。長い試験はログを保存し、タイマーで終了と結果を確認する。[テストの入口][make]、[アセット準備の試験][asset-make]、[配布構成][packages]

```sh
GDK_BACKEND=x11 make test TEST_BUILD_DIR=build/effetune-2181-debug
cmake -S . -B build/effetune-2181-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build/effetune-2181-release --parallel
GDK_BACKEND=x11 ctest --test-dir build/effetune-2181-release --output-on-failure
make -C pipetune/asset-preparation test BUILD_DIR=../../build/effetune-2181-assets
./build_package_all.sh
```

配布対象はDebian trixieのamd64、i386、arm64、armhf、riscv64と、Ubuntu 24.04／26.04の各amd64、arm64の計9構成。各パッケージを該当OSに導入し、CLIとGTKの版を確認する。共有DSPはパッケージ内とインストール先のSHA-256を照合し、導入したライブラリでSFZの発音を試す。CPUで実行できるSIMDは実行結果を、命令セットが足りないSIMDは対象名とビルド結果を記録する。

実装の完了条件は、工程1と2の版・音声・遅延の条件、DebugとReleaseの全体テスト、独立試験、9構成の導入試験がすべて成立すること。PipeTuneとEffeTuneのコミット、テスト結果、ビルド警告への対処、パッケージと同梱DSPのSHA-256を`artifacts/verification/effetune-2.18.1/`に記録する。

[release]: https://github.com/Frieve-A/effetune/releases/tag/v2.18.1
[change]: https://github.com/Frieve-A/effetune/commit/7de52ecc9158888dd437411aed438894d2ab2d1a
[metadata]: https://github.com/Frieve-A/effetune/blob/v2.18.1/plugins/dsp/effetune-dsp.meta.json
[engine]: https://github.com/Frieve-A/effetune/blob/v2.18.1/dsp/plugins/others/sfz/engine.h#L2176-L2200
[forecast]: https://github.com/Frieve-A/effetune/blob/v2.18.1/tests/esm/sfz-stream-readiness.test.mjs
[cast]: https://github.com/Frieve-A/effetune/blob/v2.18.1/tests/cjs/network-output-host.test.cjs
[note-goldens]: https://github.com/Frieve-A/effetune/tree/v2.18.1/dsp/plugins/others/sfz_note_player/golden
[main-goldens]: https://github.com/Frieve-A/effetune/tree/v2.18.1/dsp/plugins/others/soundfont_player/golden
[checkpoint-test]: https://github.com/Frieve-A/effetune/blob/v2.18.1/dsp/plugins/others/soundfont_player/native_test.cpp#L1479-L1528
[previous]: effetune-2.18.0.md
[build]: ../../../pipetune/CMakeLists.txt
[loader]: ../../../pipetune/src/dsp_backend.cpp
[exports]: ../../../pipetune/src/effetune_backend_exports.map
[pipeline]: ../../../pipetune/src/dsp_pipeline.cpp
[sfz-loader]: ../../../pipetune/src/sfz_asset_loader.cpp
[driver]: ../../../pipetune/test/asset_pipeline_driver.cpp
[registered]: ../../../pipetune/asset-preparation/test/tests/sfz-registered.test.ts
[version-test]: ../../../pipetune/test/effetune_218_test.cpp
[artifact-test]: ../../../pipetune/test/effetune_backend_artifact_test.cpp
[runtime-test]: ../../../pipetune/test/sfz_runtime_test.cpp
[preset-test]: ../../../pipetune/test/preset_pipeline_test.cpp
[make]: ../../../Makefile
[asset-make]: ../../../pipetune/asset-preparation/Makefile
[packages]: ../../../build_package.sh
