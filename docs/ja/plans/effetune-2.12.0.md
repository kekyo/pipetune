# EffeTune 2.12.0対応計画

## 目的と調査時点

EffeTuneの参照をv2.11.0 (`e200e5151b919bcb2481626b2aa3a0295baa247f`) から公式v2.12.0 (`1d4d33b2e631a840ae178295e3ea2b66b06b1e43`) へ更新する。アプリ版は2.12.0、上流DSPライブラリ版は0.12.0である。当初の依頼にあった1.12.0については、利用者に確認し、2.12.0を対象とすることを確定した。

2026-10-04、PipeTune HEAD `f843632558a374365d3f06f02ebc214efd675e85`を基準に、現行の統合コード、過去の対応計画、上流タグ間差分、公式リリース、対象タグの文書とAPI・カーネルのコメントを調査した。公式リモートのv2.12.0およびdsp-v0.12.0が、ローカルの対象コミットと一致することも確認した。本書は実装計画であり、更新後のビルド・音声・配布物の検証結果ではない。

Tonal Balance EQとRhythm Analyzerを通常のDSPとして実行し、Analog Meterを既存の解析専用DSPと同様に無警告で除外する。Rhythm Analyzerはメトロノーム音を加える機能があるため、名前やカテゴリだけで解析専用に分類しない。モデル埋め込みのビルド変更、長時間の音声処理、既存のプリセット状態表示までを対応範囲に含める。

無音時休止がオンの場合は、既存どおりフェード後にDSPをリセットして休止する。Tonal Balance EQの累積測定やRhythm Analyzerの拍検出もリセット対象とする。これが問題になる利用者は休止をオフにする方針を、利用者との対話で確認した。DSPごとの休止例外や、測定状態の退避・復元は追加しない。

## 上流差分と対応範囲

| 対象 | 2.11.0からの変更 | PipeTuneでの対応 |
| --- | --- | --- |
| Tonal Balance EQ | 新規。長時間のスペクトル測定から音色を自動補正 | 通常経路で実行。補正の成立、パラメータ、リセット、長時間処理を検証 |
| Rhythm Analyzer | 新規。テンポ解析と任意のメトロノーム音出力 | 通常経路で実行。クリックの有無と出力ch、拍検出後の音声を検証 |
| Analog Meter | 新規。音声を変更しないメーター | カタログには含め、プリセット実行から無警告で除外 |
| 解析モデル | Note Spectrogram専用のビルドから共通tree modelsへ移行。既存モデルを圧縮し、Rhythm用5モデルを追加 | リンク先・生成物へのARM対応・関連する名前と説明を更新 |
| 浮動小数点条件 | 新規3DSPとTonal用校正ツールに`-ffp-contract=off`を指定 | PipeTune独自共有バックエンドにも新規3カーネルの条件を反映 |
| Auto Leveler | K-weighting実装を共通ヘッダーへ移動 | 既存golden、レート・6chの挙動を回帰確認 |
| Oscilloscope | トリガーモードに`Off`を追加 | パッカー比較と上流nativeテストで確認。製品では解析除外を維持 |
| エンジン | パイプラインに属さないインスタンスを破棄しても、実行中パイプラインを無効化しなくなる | 共有バックエンド経由でPCMと遅延保持を確認 |
| 表示・操作 | Visualizer、ブラウザ拡張、Controller Mappingなどを拡張 | PipeTuneへの新規移植は対象外 |
| 配布用NOTICE | Rhythm Analyzerが使うfdlibm由来のatan・atan2の表示を追加 | 配布する著作権表示へ反映 |

登録カーネルは107から110、無条件に実行から除外する解析専用DSPは8から9になる。110は登録数であり、外部アセットなしで実行できるDSP数ではない。Rhythm Analyzerは`ck: false`でも通常の有効DSPとして数え、解析処理を実行する。クリック設定に応じて実行分類を切り替える仕組みは追加しない。

公開C ABIヘッダー、`core/abi.cpp`、`kernel.h`、`scripts/gen-dsp-params.mjs`、DSPのvendor、既存Bass Management・FIR Crossover設計にはタグ間の変更がない。既存DSPのgolden PCMファイルにも変更はなく、追加は新規3種類の17ケースである。既存golden JSONの参照ハッシュ更新と、音声データの変更は区別する。

`exports.txt`に追加された`et_rhythm_analyzer_warm_up`は、実装が`__EMSCRIPTEN__`で囲まれたWASM用フックである。PipeTuneのネイティブABIに追加しない。既存ABIブリッジは継続し、パッチのfuzzなし適用と実ロードを検証する。[上流差分](https://github.com/Frieve-A/effetune/compare/v2.11.0...v2.12.0)、[Rhythmカーネル](https://github.com/Frieve-A/effetune/blob/v2.12.0/dsp/plugins/analyzer/rhythm_analyzer/kernel.cpp)

DSPパラメータ編集UI、解析グラフ・テレメトリ表示、新規の音声試聴UI、Room EQ / IR Reverbの外部アセット対応、旧DSP実装を選ぶ互換モード、PipeTune自身のリリース番号変更は対象外とする。

## 現行コードに対する統合設計

### モデル埋め込みとバックエンド

`pipetune/cmake/EffeTuneNativeBackends.cmake`は全共有バックエンドを`effetune_note_models`へリンクしている。上流2.12ではこのターゲットがなくなり、`effetune_tree_models`へ置き換わるため、参照の更新だけでは統合が成立しない。

同ファイルの`pipetune_configure_effetune_note_models`、遅延実行用の`pipetune/cmake/EffeTuneNoteModels.cmake`、その呼出元、`pipetune/tools/arm-note-model.fc`を共通モデル用の名前と参照へ揃える。役割を保った改名として扱い、古い名前の互換ラッパーは残さない。上流の生成・検証処理をそのまま使い、生成済みアセンブリへのARM用ディレクティブ変換だけをPipeTune側で行う。

新しい生成器もELF向けに`@progbits`・`@object`を出力する。GNU asが`@`をコメントとして扱う32bit ARMでは、現行の`funcity`による`%`への変換を新ターゲットの全生成アセンブリへ適用する。上流ディレクトリ内でDEFERして生成規則のスコープを揃える現行方式を維持する。[モデルビルド](https://github.com/Frieve-A/effetune/blob/v2.12.0/dsp/plugins/analyzer/tree_models/models.cmake)、[生成器](https://github.com/Frieve-A/effetune/blob/v2.12.0/dsp/plugins/analyzer/tree_models/embed_models.py)

埋め込み対象はNote Spectrogramの3モデルとRhythm Analyzerの5モデル。対象`.bin`の合計は3,494,436 bytesで、旧3モデルの5,779,072 bytesより小さい。ただし、これは入力データの合計であり、共有ライブラリ全体のサイズではない。新しいカーネルや別の生成済みテーブルも増えるため、配布サイズ・初期化時メモリ・実行時メモリは別に測る。モデル再学習やTonalの校正テーブル再生成は行わない。

Python 3.10以上は上流が要求する既存ビルド依存として継続する。独自の調査・検証スクリプトはNode.js、追加のビルド時テキスト処理は`funcity`を使う。実行時にPythonやモデルファイルを別途配置する方式へ変更しない。

新規3カーネルを独自共有バックエンドの`-ffp-contract=off`対象へ追加する。Tonalの`calibrate_tables.cpp`は上流の校正用実行ファイルであり、共有バックエンドの製品ソースへ追加しない。上流nativeテストと独自Scalar / SIMDで同じ音声契約を検証する。[上流CMake](https://github.com/Frieve-A/effetune/blob/v2.12.0/dsp/CMakeLists.txt)

### カタログとプリセット状態

`pipetune/tools/generate-dsp-catalog.mjs`は上流registry・manifest・パラメータ仕様から生成するため、新規型の手書き定義は増やさない。`visualizationOnlyTypes`には`AnalogMeterPlugin`だけを追加する。`TonalBalanceEQPlugin`と`RhythmAnalyzerPlugin`は通常経路でパック・作成する。

`pipetune/src/dsp_pipeline.cpp`は解析除外をパラメータ処理・96ノード制限より前に行い、`PresetEntryState::ignored`として記録する。Analog Meterもこの順序に従う。有効なTonal / Rhythmは`enabled`、無効なノードや無効Section内では`off`となる既存契約を維持する。

現行HEADには、読込済みプリセットの各エントリーを制御プロトコル経由でGTKのProcessingページへ表示する機能がある。新規3種類の順序・名前・状態が、読込、レート変更、バックエンド変更、再読込後にも一致することを検証する。表示のためだけに制御プロトコルを増やさない。

## 新規DSPの対応と検証

### Tonal Balance EQ

36 float、144 bytes、パラメータハッシュ`0x0a5d8c64`。外部アセット不要、報告遅延0。`tg`はAll / Classical / Electronic / Pop / Rock / Tiltの6種類。`am`、`rg`、`sm`、`at`、`lo`、`hi`、`sp`、5組の`ea0`〜`ea4`・`ta0`〜`ta4`・`fa0`〜`fa4`・`ga0`〜`ga4`・`qa0`〜`qa4`、`ts`、`tc`、`mp`を上流のパッカーに従って渡す。Target Adjustは目標曲線の変更であり、常時加算する別のEQとして実装しない。[公式説明](https://github.com/Frieve-A/effetune/blob/v2.12.0/docs/dsp/effects/tonal-balance-eq/index.md)、[パラメータ](https://github.com/Frieve-A/effetune/blob/v2.12.0/dsp/plugins/eq/tonal_balance_eq/params.json)

`at: 100`は100秒窓ではなく、作成またはreset以降の累積測定を意味する。`mp`は測定の一時停止であり、ホスト側のバイパスとは区別する。補正は測定の蓄積に応じて変化するので、初期の短い通過音声だけで対応完了と判定しない。補正後のラウドネス調整はピーク制限ではなく、ピークが上がる場合がある。独自のリミッターは加えない。[カーネルとコメント](https://github.com/Frieve-A/effetune/blob/v2.12.0/dsp/plugins/eq/tonal_balance_eq/kernel.cpp)、[解析の契約](https://github.com/Frieve-A/effetune/blob/v2.12.0/dsp/plugins/eq/tonal_balance_eq/analysis.h)

検証を次の三系統に分ける。

1. `parameter-packer-parity.mjs`で6ターゲット、5組の各キー、bool、範囲端、既定値、範囲外・不正値、`at: 100`、`mp`を上流JSと比較する。型や値の独自補正は足さない。
2. 公式golden全6件を、PipeTuneの実共有バックエンドで比較する。許容絶対誤差は元の`2e-5`を維持する。生成元は`production-native-promoted-v1`であり、独立実装との一致を証明するものとは扱わない。
3. 製品のプリセット経由で、時間変化する帯域別テスト信号を十分なフレーム数だけ処理し、補正による帯域比の変化、左右への同一補正、`am: 0`・`rg: 0`、無音・静かな区間、有限出力、報告遅延0を確認する。累積測定と有限時間平均、測定一時停止、reset後の再現性は上流nativeテストと共有バックエンドの検証を分担させる。

レートは上流nativeテストが判定する44.1 / 48 / 96 / 192 kHzに加え、PipeTuneが固定設定で提供する384 kHzを含める。Automaticの代表値として32 / 88.2 / 176.4 kHzも選ぶ。1 / 2 / 6 / 16ch、All・ペア・単一ch、端数ブロックを代表ケースで分担し、未選択chが変わらないことを確認する。IIR処理の周波数依存の位相変化と、ホストが通知する遅延を混同しない。

上流テストには数十秒相当の入力やRelease専用の統計検証がある。短いgoldenでそれらを置き換えず、DebugとRelease両方で実行する。壁時計のsleepではなく、処理フレーム数で測定時間を進める。[上流nativeテスト](https://github.com/Frieve-A/effetune/blob/v2.12.0/dsp/plugins/eq/tonal_balance_eq/native_test.cpp)、[goldenケース](https://github.com/Frieve-A/effetune/blob/v2.12.0/dsp/plugins/eq/tonal_balance_eq/cases.json)

### Rhythm Analyzer

3 float、12 bytes。`mn`は最低BPM、`mx`は最高BPM、`ck`はメトロノーム音の有効化で既定false。BPMの上下限の整合は上流カーネルに従う。プリセットに保存される表示用の`sp`・`vt`・`vm`・`ve`・`vl`は、既存の未知パラメータ処理に従って音声パラメータへ混入させない。[公式説明](https://github.com/Frieve-A/effetune/blob/v2.12.0/docs/dsp/effects/rhythm-analyzer/index.md)、[パラメータ](https://github.com/Frieve-A/effetune/blob/v2.12.0/dsp/plugins/analyzer/rhythm_analyzer/params.json)

`ck: false`ではPCMを変更しない。trueでは拍検出後、処理範囲の先頭1〜2chへ同じクリック音を加える。Allで多chを渡した場合も3ch目以降へは加えず、解析入力も先頭1〜2chである。選択ペアならそのペアに適用する。全ペアへ自動展開しない。報告音声遅延0と、テンポ検出に必要な時間は別の概念として説明する。[カーネル](https://github.com/Frieve-A/effetune/blob/v2.12.0/dsp/plugins/analyzer/rhythm_analyzer/kernel.cpp)

公式golden全6件を許容絶対誤差`1e-6`で検証する。ただし`click-on`ケースも短いnoise入力なので、golden一致だけではクリックが出た証拠にならない。上流nativeテストの拍入力を参考に、検出に十分な長さの決定的なリズム信号を用意する。テレメトリを無効にした製品経路で、ck=falseとの出力差からクリックの発生、周期、左右一致、未選択chの保持を確認する。[goldenケース](https://github.com/Frieve-A/effetune/blob/v2.12.0/dsp/plugins/analyzer/rhythm_analyzer/cases.json)、[nativeテスト](https://github.com/Frieve-A/effetune/blob/v2.12.0/dsp/plugins/analyzer/rhythm_analyzer/native_test.cpp)

レートの代表値はTonalと共通にし、上流がレートにより解析経路を変えることを考慮する。1 / 2 / 4 / 16ch、All・別ペア・単一ch、端数ブロック、reset、ckを変えたプリセットの再読込を検証する。製品の再読込は新パイプラインの構築なので、上流のライブパラメータ変更が解析状態を保持することとは区別する。ck=falseでも解析負荷があるため、両設定のCPU負荷と初期化コストを測る。

### Analog Meter

4 float、16 bytes。VU / PPM / RMS / Sample Peak / True Peak / Loudnessの6モードを持つが、いずれも音声を加工しない。全モードを既存8種類と同じ除外経路へ通し、メーターUIは実装しない。[公式説明](https://github.com/Frieve-A/effetune/blob/v2.12.0/docs/dsp/effects/analog-meter/index.md)、[カーネル](https://github.com/Frieve-A/effetune/blob/v2.12.0/dsp/plugins/analyzer/analog_meter/kernel.cpp)

単独、加工DSP・Sectionとの混在、異なる入出力バス、96個超の解析ノードを含むプリセットで、PCM・遅延・有効DSP数が解析ノードを除いた構成と一致することを確認する。警告を出さず、バス間コピーも発生させない。GTKの表示状態は、ノードやSectionの有効・無効によらず既存解析DSPと同じ`ignored`とする。

上流nativeテストに加え、共有バックエンド自体の登録・パック・実行を公式golden全5件で検証する。製品で除外することと、配布バックエンドが正しく構築されていることを別々に確認する。

## 再構築と無音時休止

レート・Scalar / SIMD・プリセット切替では、既存どおり制御側で新パイプラインを構築して交換する。新規DSPの測定・拍検出も初期状態から始める。読込失敗では旧パイプライン、プリセットのエントリー一覧、設定リビジョンを保持する。音声コールバック内にモデル読込、係数設計用の独自処理、追加ワーカーやポーリングを導入しない。

`pipetune/src/dsp_pipeline_slot.cpp`は、無音時休止へ移る際に`pipeline->reset()`を呼ぶ。利用者が確認した方針に従い、新規DSPにもこの動作を適用する。

- 休止オン: 指定時間の無音とフェード後にreset・休止し、再開時にはTonalの測定とRhythmの拍検出をやり直す。クリックも既存の音声生成DSPと同様に休止対象となる。
- 休止オフ（Ignore）: 無音入力でも呼出しを継続し、ホスト都合の休止resetを行わない。無音を測定に含めるか、拍追跡をどう扱うかは上流DSPに任せる。

両設定を音声処理と状態遷移で検証する。Tonalの`at: 100`でも休止オンなら測定はresetされること、休止オフにしてもレート変更やプリセット再読込ではresetされることを利用者文書に明記する。

## 段階的な実施計画

各段階は、その段階の成果をCLI・共有バックエンドまたは既存UIから観測できる状態で完了する。製品コードの修正は、先に挙動を再現するテストを追加してREDを確認し、修正後にGREENを確認する。依存更新だけで成立する追加検証は回帰確認として扱い、REDを作るためだけの製品変更は行わない。

### 0 現行の基準を記録する

作業ツリー、参照タグ、ビルド依存、利用可能なISA・配布用コンテナを確認し、現行2.11.0で`make test`を実行する。既存のTonalに近いEQ、Rhythmに近い解析・音声生成DSP、バックエンド、プリセット状態表示、無音時休止のテスト構成を把握する。必要なパッケージが不足していれば、その内容とインストール方法を案内する。

完了条件: 現行テストの結果と環境が記録され、既存不具合と更新による失敗を区別できる。上流とPipeTuneのソースにはまだ変更を加えない。この段階の記録は次の実装コミットへまとめる。

### 1 2.12.0のビルドと最小のプリセット実行を成立させる

新規DSPがプリセットから読めること、Analog Meterが無警告で除外されることを検証するテストを、現行版でREDにする。サブモジュールを指定の公式コミットへ進め、モデルターゲット・ARM変換・コンパイル条件・解析除外をまとめて更新する。既存カタログテストを110種類へ更新し、新規型のパラメータ配置・アセット契約をロード済み共有ライブラリから照合する。

ホストDebugビルド、パッカー比較、カタログ・PCMの対象テスト、モデル埋め込み・Note Spectrogram・tree modelの上流テストを実行する。32bit ARMのモデル移行を後回しにしないため、既存配布構成のうちarmv7lを使って生成・アセンブル・リンク・ロードを確認する。ABIパッチも実際のビルドでfuzzなし適用を確認する。

完了条件: CLI/GTKがEffeTune 2.12.0を表示し、全生成バックエンドの契約が一致する。実行可能ISAとarmv7lでロードでき、Tonal / Rhythmが有効、Analogが除外された最小プリセットを実行できる。モデルや外部ソースへの直接変更がない。

コミット: `feat: update EffeTune integration to 2.12.0`

### 2 Tonal Balance EQを音声と時間経過で検証する

パラメータ境界、公式6 golden、補正が成立する長い入力、チャンネル選択、累積測定・一時停止・resetの検証を追加する。製品側に不足があれば再現テストのREDを確認してから修正する。AllとTilt、Target Adjustを観測できるプリセットを`pipetune/test/presets/effetune-2.12/`へ追加する。

既存の`effetune_backend_artifact_test.cpp`とパッカー比較を拡張し、機能テストは必要な単位で追加する。既存goldenランナーは今回の刺激形式に対応しているため、別の比較基盤は作らない。CLIベンチマークで実行できる成果物を用意し、測定が進んだ区間の出力も記録する。

完了条件: Scalarと実行可能SIMDでgoldenが元の許容誤差内に一致し、実プリセットから音色補正と各設定の違いを観測できる。レート・ch・ブロック・resetの代表ケースが成功する。短い初期区間の一致だけで合格にしていない。

コミット: `feat: validate Tonal Balance EQ preset processing`

### 3 Rhythm Analyzerのクリックと解析除外を検証する

Rhythmの公式6 goldenと、拍検出後のクリックを検証する長い入力を追加する。ck=falseのPCM保持、ck=trueの加算音声、Allでの先頭1〜2ch・選択ペア・未選択chの挙動を確認する。click-off / click-onの観測用プリセットを追加し、音声出力の差をCLIまたはテストドライバーで確認できるようにする。

Analogの公式5 goldenと9種類の解析除外回帰を追加する。加工DSPを解析専用として除外していないことも検証する。Tonal / Rhythm / Analogを混在させたプリセットで、読込済みエントリーのenabled / off / ignoredと有効DSP数を確認する。

完了条件: テレメトリを無効にした製品経路でもクリック出力が成立する。クリックなしはPCMを保持するが有効DSPとして実行される。Analogは警告・バスコピー・遅延・有効ノード数へ影響せず、各状態表示が既存契約に一致する。

コミット: `feat: validate Rhythm Analyzer and Analog Meter integration`

### 4 ライブ動作と既存機能の回帰を確認する

レート・バックエンド切替、ファイル再読込、失敗時の旧状態保持、無音時休止オン・オフを実パイプラインで検証する。GTKの既存モデル・E2Eテストで新規DSPのエントリー表示と再構築後の状態を確認する。表示機能は既存構造を使い、今回のための画面を追加しない。

エンジンの非所属インスタンス破棄について、実共有ライブラリ経由で遅延のあるパイプラインを処理し、無関係のインスタンス破棄後もPCM・遅延が継続すること、所属ノード破棄後は既存どおり無効化されることを確認する。Auto Leveler、Oscilloscopeの`Off`、既存Bass Management IIR / Linear、FIR生成、Crosstalk、入力遅延補償、Matrix resetを回帰対象に含める。

ReleaseでTonal / Rhythmの初期化時間、代表レート・チャンネル数の処理負荷、RSSを測定する。Tonalは測定が進む前後、Rhythmはckの両設定を含める。負荷測定はビルドと競合させず、エミュレーションの時間を実機性能として扱わない。

完了条件: ライブ再構築とGTK表示が整合し、失敗時の旧パイプライン保持が成立する。休止オンでのreset・再測定と、オフでの呼出し継続を確認した。全体Debugテストが成功し、負荷の測定条件と結果が記録されている。

コミット: `feat: verify EffeTune 2.12 runtime integration`。製品不具合が見つかった場合は、再現と根本修正を含む独立した`fix:`コミットに分ける。

### 5 利用者文書と配布物を完成させる

英日README、`docs/en/details.md`、`docs/ja/details.md`、`pipetune/README.md`を更新する。Tonalの累積測定・reset・ピーク、Rhythmのクリックとch選択、解析9種類の除外、休止オフによる測定継続を利用者の視点で説明する。内部のモデル・コンパイル条件・ABIの説明は、既存の`pipetune/docs/dsp-backends.md`と必要な`pipetune/docs/architecture.md`へ記載する。過去の対応計画は履歴として維持する。

Rhythm Analyzerの`g2_math.h`が使うfdlibmの表示を、`packaging/copyright`と必要な配布文書へ反映する。上流のNOTICEとソースコメントを出典にし、外部ファイルは編集しない。[上流NOTICE](https://github.com/Frieve-A/effetune/blob/v2.12.0/plugins/dsp/NOTICE.txt)、[対象ソース](https://github.com/Frieve-A/effetune/blob/v2.12.0/dsp/plugins/analyzer/rhythm_analyzer/g2_math.h)

最終の作業ツリーを固定して、Debug全体、クリーンRelease全体、コンポーネント単独ビルド、GTK E2E、一時DESTDIRへのインストール、既存13構成のパッケージ生成・インストール・起動を検証する。配布された共有ライブラリを使ってTonal / Rhythmを実行し、Analogを含む混在プリセットの除外も確認する。Tonalの初期通過やRhythmの拍検出前だけを実行成功の証拠にしない。

完了条件: 最終完了条件をすべて照合し、利用者文書、著作権表示、検証記録が揃っている。実行できなかったISA、実機性能、物理的な音声接続は未検証として明確に区別する。

コミット: `doc: describe EffeTune 2.12.0 support`。著作権表示の配布変更を独立させる場合は`chore: include fdlibm attribution in packages`とする。

## 検証方法と実施上の条件

基本の入口は既存Makefileとする。最終Releaseは新しいビルドディレクトリを使い、Debugだけで成功と判断しない。

```sh
make test
cmake -S . -B build/effetune-2.12-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build/effetune-2.12-release --parallel
ctest --test-dir build/effetune-2.12-release --output-on-failure
DESTDIR="$PWD/artifacts/verification/effetune-2.12.0/install" cmake --install build/effetune-2.12-release --prefix /usr
./build_package_all.sh
```

- 全体テストには上流native・全DSPパッカー・上流native parity・独自共有バックエンドgolden・コンポーネント単独ビルド・インストール・GTK E2Eを含める。新規nativeテストとモデルテストがCTestへ登録されていることを確認する。
- 前回の検証では、コミットや作業ツリー変更後に再構成しないと、`screw-up format`による算出版が単独ビルドとずれた。全体テスト中はファイルを変更せず、コミット後の検証ではCMakeを再構成する。必要になった版管理も既存の`screw-up format`経路を使う。
- 13配布構成はDebian bookwormのx86_64 / i686 / arm64 / armv7l、trixieの同4種とriscv64、Ubuntu 24.04 / 26.04のx86_64 / arm64。コンパイル、ロード、プリセットPCM、golden比較を別々に記録する。
- ScalarとCPUで実行可能なSIMDを比較し、x86 FMA差、i686、ARMのアセンブリ、RISC-Vを含む配布上の差を確認する。未対応ISAを実行した扱いにしない。
- 長時間測定やエミュレーションは処理フレーム数・実測スループットから必要時間を見積もる。タイムアウトは必要に応じて延長し、全体検証を省略したり許容誤差を緩めたりしない。固定sleepに依存するテストを増やさない。
- 全レート×全ch×全パラメータの直積は作らず、境界と契約を代表ケースで分担する。新規golden全17件と上流のRelease専用検証は省略しない。
- 既存GTK E2E基盤を維持する。ブラウザを介する試行・E2E操作が必要になればPlaywright MCPを使う。画像マスターを変更する場合は目視確認し、UIの時間軸の問題が見つかった場合は動画による挙動検証を追加する。
- 外部APIを新たに使う場合は対象版の公式文書とAPIコメントの両方を確認する。外部ソースとvendorは編集せず、新たな内部API依存を加えない。
- 計画外の問題は原因と設計前提を本書へ反映してから対処し、不要な試行は戻す。計画とテストの契約が矛盾した場合は確認する。機能削除が必要になった場合は、削除を確認するテストのRED、削除後GREENとコミット、最後にその一時テストの削除という手順を守る。

実行コマンド、対象コミット、環境、ログ、golden誤差、負荷、配布物ハッシュは`artifacts/verification/effetune-2.12.0/`へ保存し、実装完了時に本書へ実施結果と条件の照合を追記する。

## 最終完了条件

- [ ] 指定の公式v2.12.0を参照し、外部ソースを変更していない。
- [ ] CLI/GTKのEffeTune版が2.12.0で、各バックエンドの110カーネル・パラメータ・アセット契約が一致する。
- [ ] 共通tree modelsの埋め込みと32bit ARM対応が成立し、単独ビルド・配布物からロードできる。
- [ ] Tonalの音色補正、パラメータ、累積測定・一時停止・reset、レート・ch・端数ブロックを検証した。
- [ ] Rhythmのck=falseのPCM保持、ck=trueのクリック、出力ch、reset・再検出とテレメトリ無効時の音声を検証した。
- [ ] Analogを含む解析9種類が無警告で除外され、PCM・遅延・バス転送・有効ノード数へ影響しない。
- [ ] 新規3種類のプリセット状態が制御プロトコルとGTKへ正しく伝わり、再構築・失敗時の保持も成立する。
- [ ] 無音時休止オンでは既存どおりreset・休止し、オフではDSP呼出しを継続する。利用者文書がこの選択を説明する。
- [ ] 既存DSP・生成FIR・遅延補償・エンジン破棄契約の回帰と、新規17 goldenの比較が成功する。
- [ ] Debug/Release全体テスト、単独ビルド、GTK E2E、インストールと13配布構成の検証結果を記録した。
- [ ] fdlibmの表示を含む配布文書と利用者文書が揃い、負荷と実機未検証範囲を区別して記録した。

## 計画作成の完了確認

- [x] 対象版と無音時休止の方針を利用者に確認した。
- [x] 現行コードと公式タグ間差分を調査し、モデル移行、音声加工2種類、解析除外1種類、既存変更を区別した。
- [x] 各段階の実行可能な成果物、TDD・回帰検証、コミット、完了条件を定義した。
- [x] 音声の時間依存、現行GTK表示、休止reset、ARM対応、著作権表示、配布検証を計画に含めた。
- [x] 調査で確認した事実、PipeTune側の設計方針、未実施の実行検証を区別した。

計画作成では本書だけを追加し、製品実装・サブモジュール参照は変更しない。ビルドステップに影響しない文書のみの変更なので、この段階ではビルド・テストを実行しない。

## 主な参照

- [公式2.12.0リリース](https://github.com/Frieve-A/effetune/releases/tag/v2.12.0)
- [DSP README](https://github.com/Frieve-A/effetune/blob/v2.12.0/dsp/README.md)、[公開C ABI](https://github.com/Frieve-A/effetune/blob/v2.12.0/dsp/include/effetune/abi.h)、[カーネルAPIとコメント](https://github.com/Frieve-A/effetune/blob/v2.12.0/dsp/include/effetune/kernel.h)
- [エンジンの破棄処理](https://github.com/Frieve-A/effetune/blob/v2.12.0/dsp/core/engine.cpp)、[上流ABIテスト](https://github.com/Frieve-A/effetune/blob/v2.12.0/dsp/test/abi_test.cpp)
- [共通K-weighting](https://github.com/Frieve-A/effetune/blob/v2.12.0/dsp/include/effetune/dsp/k_weighting.h)

既存計画の形式と前回の検証上の注意点は[2.11.0対応計画](effetune-2.11.0.md)を参照した。今回の実装基準は本書冒頭の現行HEADとする。

## 実施記録

- 段階0: 計画コミット`e9eaaa7`で`make test`を実行し、Debug全160件が成功した（723.18秒）。単独Releaseビルド、GTK E2Eを含む。`baseline-debug.log`と`environment.json`を検証ディレクトリへ保存。必要なツールと13構成のPodmanイメージは存在し、追加インストールは不要だった。
- 段階1のRED: 新規2加工DSPの無警告実行とAnalog Meter全モードの無警告除外をプリセット経由で検証し、2.11.0では3つの期待どおりの失敗を確認した（`step1-red.log`）。公式2.12.0への参照更新、共通tree modelsへの移行、ARM用生成物変換の改名、3カーネルの浮動小数点条件、Analogの除外後、ホストDebugビルドと対象8テストが成功した（`step1-build.log`、`step1-green.log`）。
- 段階1の設計見直し: armv7lの初回ビルドで、8個の変換規則が同時に`npx`の同一キャッシュへインストールし、`ENOTEMPTY`・`ENOENT`・`funcity: Permission denied`を起こした（`step1-armv7-package.log`）。従来の独立した変換規則を8モデルへ増やすだけでは、空キャッシュでの並列ビルドを保証できない。変換出力間へ生成依存を加え、この変換だけを直列化する設計に修正する。上流のモデル生成と通常のコンパイルは並列のまま維持し、外部コードは変更しない。修正後は新しいコンテナの空キャッシュから同じARM配布ビルドを再実行する。
- 段階1完了: 空キャッシュのarmv7l配布ビルド・インストールが成功した。インストール先のScalar / NEON共有ライブラリで最小混在プリセットを実行し、有効DSP数2、除外警告0、同一チェックサムを確認した。CLI/GTKはともにEffeTune DSP 2.12.0を表示する（`step1-armv7-package-green.log`、`step1-installed-arm.log`）。新規型のハッシュ、float数、構造化バイト領域なし、外部アセット不要の契約もホストで成功した（`step1-catalog.log`）。上流作業ツリーは変更なし。段階1の完了条件を満たした。
- 段階2完了: Tonalの6ターゲット・全36要素の境界・不正値を上流JSと比較し、公式6 goldenも全実行可能ISAで成功した。追加のプリセットテストは8レート、1/2/6/16ch、無補正設定と初期測定停止、32秒の音声・無音・静かな区間、All / Tilt / Target Adjust / 累積測定、選択ペアと未選択ch、resetと異なる端数ブロックを検証した（対象3テスト35.66秒、`step2-tests.log`）。上流のpreview/c7も成功し、蓄積後の測定停止・再開と70秒の累積測定を確認した（`step2-native-time.log`）。追加3プリセットをCLIで32秒相当ウォームアップ後に実行し、Scalar / baseline / x86-64-v3のチェックサム一致と各設定の出力差を記録した（`step2-benchmark.json`）。製品コードの追加修正は不要だった。最終全体検証ではRelease専用の統計検証も実施する。
- 段階3完了: Rhythm 6件・Analog 5件の公式goldenがScalar / baseline / x86-64-v3で成功した（全て最大絶対誤差0）。Rhythmは表示パラメータを無効化した実プリセットで20秒の120 BPM入力から36〜37クリックを生成し、8レート・1/2/4/16ch・All/ペア/単一ch、未選択chの保持、resetと端数ブロック変更、ck=falseへの再読込を検証した（`step3-tests.log`、4件30.49秒）。解析9種類100ノードと加工DSPの混在、および新規型のenabled/off/ignoredとレート再構築も成功した（`step3-mixed.log`）。click-on/offと混在プリセットを追加し、段階3の完了条件を満たした。製品コードの追加修正は不要だった。
- 段階4の対象検証: 実制御ソケットから新規DSPのレート・Scalar/SIMD切替、ファイル再読込、失敗時の状態・リビジョン保持を確認した。GTK E2Eは新規3種類の表示をレート・バックエンド変更・バイパス後も確認する。20秒の測定済み音声から休止オン/オフへ移行するテストは、オンでreset後の参照PCM、オフで連続処理の参照PCMに一致し、再開後の補正/クリックも確認した。共有バックエンドは無関係インスタンス破棄後の5120フレーム遅延と保留PCMを保持し、所属ノード破棄では実行を無効化した。対象5件77.28秒で成功（`step4-targeted.log`）。
- 段階4の性能条件: i9-12900KSホスト、Release、257フレーム、48 kHz/2ch・96 kHz/6ch・384 kHz/16ch、Scalar/x86-64-v3、Tonal AllとRhythm ck両設定の18条件。コンパイル・他のテスト終了後、合成リズムを33秒分処理し、最初と33秒目の処理時間、初期化、プロセス最大RSSを記録した（`performance.json`、再実行用`measure-performance.mjs`と`performance-driver.cpp`）。48 kHz/2chの33秒目は1コア換算Tonal 0.67〜0.71%、Rhythm 1.00〜1.04%。18条件の最大は18.76%。RSSはホスト全体やカーネル単体の使用量ではなく、ライブラリ発見とドライバーの音声バッファも含む。起動順・キャッシュ状態の影響を含む単回測定であり、実時間処理の保証値ではない。
- 段階4完了: 作業ツリーを固定して再構成・ビルド後、Debug全166件が成功した（256.16秒、skipなし、`step4-full-debug.log`）。新規native3種類、共通モデル、Auto Leveler、Oscilloscope Offのパッカー、既存FIR/Crosstalk/遅延/reset、単独Releaseビルド、GTK E2Eを含む。長時間PCM、ライブ状態、休止方針、負荷測定を含めて段階4の完了条件を満たした。
