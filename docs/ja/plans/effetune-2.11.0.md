# EffeTune 2.11.0対応計画

## 目的と調査時点

EffeTuneの参照をv2.10.0 (`abca7ff96f48f9cacace4d3c6aef5c4d3407bd8f`) から公式v2.11.0 (`e200e5151b919bcb2481626b2aa3a0295baa247f`) へ更新し、追加・変更されたDSPをPipeTuneのプリセット実行環境で扱えるようにする。アプリ版は2.11.0、上流DSPライブラリ版は0.11.0である。

2026-09-30、PipeTune HEAD `f239936`、上流の両タグ間のソース差分、公式リリース、タグ固定の文書とAPI・カーネルのコメントを調査した。公式リモートのv2.11.0とローカルタグのコミット一致も確認した。本書は実装前の計画であり、2.11.0のビルド・実行検証はまだ行っていない。

対応方針は、Attack Tonal BalanceとBass Extenderを通常の音声加工DSPとして実行し、Bass ManagementはIIRとLinearの両方を対応対象にすること。Chroma Spiralは既存の解析専用DSPと同様に、カタログには含め、プリセット実行から無警告で除外する。最も大きな統合作業はBass Managementの条件付きアセット処理とLinear用FIR生成である。

## 上流差分と対応範囲

| 対象 | 2.10.0からの変更 | PipeTuneでの対応 |
| --- | --- | --- |
| Attack Tonal Balance | 新規。時間周波数解析によるAttackとTonalの調整 | 通常経路で実行。長い遅延、残差成分、浮動小数点条件を検証 |
| Bass Extender | 新規。低域から約1オクターブ下の成分を生成 | monoまたは選択ペアで実行。対応レートと処理幅を明示 |
| Bass Management | 新規。メイン低域・LFEを指定サブ出力へ分配 | IIRはアセットなし、LinearはプリセットからFIR生成。配列・経路検証を追加 |
| Chroma Spiral | 新規。音声を変更しないHQスペクトル解析 | 可視化専用として除外 |
| Saturation、Dynamic Saturation、Exciter、Hard Clipping、Harmonic Distortion、Multiband Saturation | `os`追加、共通OversampledShaper導入 | パラメータ契約と音声・遅延を更新 |
| Brickwall Limiter | オーバーサンプリングFIR、再構成、出力上限処理と遅延を変更 | 新golden、ピーク上限、実遅延を検証 |
| Time Alignment | `dl`の最大値を100 msから500 msへ拡大 | 新上限のパック・実音声・必要メモリを検証 |
| MP3 Codec Simulator | 心理音響モデル、アタック判定、量子化の処理を変更。直接のPFFFT利用を除去 | 長時間処理、状態切替、更新goldenと負荷を検証 |
| Tube Simulator | DC状態が変化しなくなった場合の初期化ループ終了を追加 | 既存の音声とruntime event契約、初期化を回帰確認 |
| FM Radio Simulator、TV Audio Simulator | スペクトルテレメトリを共通化 | 音声加工DSPとして維持。既存音声とテレメトリ無効時の処理を確認 |
| MultiChannel Panel | ピーク計測をブロック数基準からフレーム時間基準へ変更 | 音声加工を維持し、gain・mute・delayの回帰確認 |
| Oscilloscope、Pitch Meter、Spectrum Analyzer | 解析・テレメトリ処理を変更 | 引き続き実行から除外。上流nativeテストは実行 |
| 共通AR補間 | 既知成分から右辺を構築する処理を変更 | 利用元のClick Remover / Clip Restorerを含む既存回帰を実施 |
| エンジン | 各DSPへ渡す選択チャンネルの入力遅延を整合 | 分岐・混合・別バスsendを実PCMで確認 |
| ビルド | 新規4種類のnativeテスト、Attack Tonal Balanceの`-ffp-contract=off`を追加 | PipeTune独自共有バックエンドにもソース別条件を反映 |

登録カーネルは103から107、実行を除外する解析専用DSPは7から8になる。登録数はアセット不要で利用できる音声加工DSP数ではない。Sub Synthの`params.json`変更は整形のみで、フィールドの意味・数に変更はない。大量のgolden JSON更新にも参照ハッシュだけの変更が含まれるため、PCM変更と区別する。

公開C ABIヘッダー、`core/abi.cpp`、`kernel.h`、vendor、`scripts/gen-dsp-params.mjs`はタグ間に変更がない。一方、内部エンジンの遅延補償構造は変わっているため、既存ABIブリッジのパッチ適用と全バックエンドの実ロードは確認する。

Visualizerレイアウト、プレイヤー速度変更、ブラウザ拡張、バックアップ、解析結果表示、DSPパラメータ編集UI、Room EQ / IR Reverbの新規アセット対応、動的なPipeWireチャンネル数変更は対象外。旧版のDSP実装や音声を選択する互換モードは追加しない。

## 現行PipeTuneの統合方式と必要な変更

- `pipetune/tools/generate-dsp-catalog.mjs`がregistry・manifest・パラメータ仕様からカタログを生成する。配列、enum、boolは既存機能で扱える。Chroma Spiralを`visualizationOnlyTypes`へ追加する。
- `pipetune/src/dsp_pipeline.cpp`はプリセットをパックし、インスタンス作成・アセット転送・パイプライン構成を行う。`requiresExternalAssets`かつ未対応のDSPはモードに関係なく除外するため、Bass ManagementはIIRでも現状のままでは実行されない。
- `pipetune/src/generated_fir_asset.cpp`にはFIR Crossoverなどの設計、FFT呼び出し、ETA1アセット構築、容量見積もりがある。Bass Managementはこの経路を拡張する。ただし既存FIR Crossoverの公開処理は2ch通過・偶数chへの帯域展開を前提にするため、そのまま呼び出して代用しない。
- 現行パイプラインは、生成アセットがあると一律に`lt` / `fd`を書き換える。Bass Managementにこれらのパラメータは存在しないため、DSPごとに必要な補正だけを適用するよう分離する。バックエンドABIの拡張は現時点では不要と見込む。
- `pipetune/cmake/EffeTuneNativeBackends.cmake`は`kernel.cpp`を自動収集するが、上流のソース単位のコンパイル条件は自動継承しない。Attack Tonal Balanceを既存の`-ffp-contract=off`対象へ追加する。
- `pipetune/test/parameter-packer-parity.mjs`は全DSPをJSパッカーと比較する。新規配列・マスクと`os`の境界を追加する。パッカー一致とDSPとして妥当な設定であることは別に検証する。
- `pipetune/test/effetune_backend_artifact_test.cpp`は刺激とパラメータイベントを扱えるが、goldenの外部アセット投入は未対応。Bass ManagementのLinear golden用にアセット生成・転送を追加する。

## 新規DSPの対応設計

### Attack Tonal Balance

`at`、`tn`、`ae`、`te`の4 floatを既存経路で渡す。外部アセットは不要。上流の分離処理をそのまま使い、DSPアルゴリズムをPipeTune側で実装し直さない。

遅延はFFT長とその1/4の和。48 kHzでは5,120 frames、約106.67 msであり、両ゲイン0 dBでも遅延は残る。FFT長はレートに応じて変わるため、48 kHzの値を固定で加算せず、既存の`pipelineLatency`取得とPipeWire通知を使う。両成分を無効にしても、分類されなかった残差は残るので無音とは限らない。[公式仕様](https://github.com/Frieve-A/effetune/blob/v2.11.0/docs/dsp/effects/attack-tonal-balance/index.md)、[カーネル](https://github.com/Frieve-A/effetune/blob/v2.11.0/dsp/plugins/dynamics/attack_tonal_balance/kernel.cpp)

検証は全9 golden（許容絶対誤差`2e-5`）に加え、0 dBでの遅延付き再現、短いアタックと持続音に対する変化、成分有効化と残差を確認する。goldenは`production-native-promoted-v1`由来なので、golden一致だけで独立した実装との一致と主張しない。

1/2/多ch、All / ペア / 単一ch、固定5レート、端数ブロック、reset、プリセット再読込を代表ケースで分担する。遅延を超える入力と末尾処理を用意し、無音の先頭だけを比較して成功としない。Spatial Mapperなどとの連結、無音時停止と再開も確認する。高レート・16chは負荷計測対象とし、無条件のリアルタイム動作保証はしない。

### Bass Extender

`am`と`og`の2 float、外部アセット不要、報告遅延0。ステレオ時は左右の平均から低域を生成し、同じ生成成分を両chへ加算する。左右を独立に処理するDSPではない。

上流prepareが受理するレートは44.1 / 48 / 88.2 / 96 / 176.4 / 192 kHzのみ。32 / 352.8 / 384 kHzなどではインスタンス生成に失敗する。Automaticで非対応レートになる場合を含め、既存の起動・再構築失敗処理と固定レートによる回復を検証する。[カーネル](https://github.com/Frieve-A/effetune/blob/v2.11.0/dsp/plugins/saturation/bass_extender/kernel.cpp)

上流`process`は1〜2chのみを処理し、3ch以上では全体を変更せずに戻る。PipeTuneでは選択された処理幅が2を超える設定をノード名付き読込エラーにする方針とする。これは加工されない設定を成功として扱わないためのホスト側設計であり、上流カーネルのエラー契約ではない。多chストリームでは対象ペアを選び、必要なら複数ノードを置く。Allを自動的にペア列へ展開しない。1chの検証ではAllまたは単一chを明示する。

全10 golden（`2e-6`）に加え、対象帯域の正弦波で約半分の周波数成分が増えること、逆相ステレオの共通成分、`am: 0`、`og`、無音、ブロック境界、reset後の状態を検証する。全6対応レート、非対応レート、別ペア、処理対象外chの保持を確認する。goldenの出自はAttack Tonal Balanceと同じくnative実装であり、意味的な音声検証を併用する。

### Bass Managementの入力契約とIIR

89 float、356 bytes。`ph` / `tp`、16要素ずつの`ro` / `fc` / `sl` / `rt` / `ri`、`su` / `lf` / `ls` / `lo` / `bg` / `lg` / `hg`を既存パッカーで扱う。`ro`は0=Full Range、1=Managed、2=LFE、3=Unused。`rt`と`ri`は入力chごとの16bit出力マスクで、`ri`は経路別の極性反転、`su`はサブ出力マスクである。

上流アプリが保存した完成済みパラメータを入力とする。UIでは新規chをManagedにするが、ネイティブスキーマの既定`ro`は全0である。PipeTuneがUIの初期化を推測して配列を書き換えることはしない。[パラメータ](https://github.com/Frieve-A/effetune/blob/v2.11.0/dsp/plugins/basics/bass_management/params.json)、[上流アプリ](https://github.com/Frieve-A/effetune/blob/v2.11.0/plugins/basics/bass_management.js)

対応するルーティングは上流アプリと同様にAll（`A`または`All`）とする。選択範囲内の相対ch番号で別ペアへルートが移る混乱を避けるため、指定なし・ペア・単一chはノード名付き読込エラーにする。PipeTuneは必要な`--channels`で起動し、プリセットからストリーム幅を変更しない。

パック済みの値と実処理幅を使い、以下を構築時に検証する。配列の生値を別規則で再解釈し、カーネルへ渡す値とFIR設計値が食い違う構造にしない。

- サブ出力が処理幅内にあり、範囲外chにManaged / LFEや有効ルートがない。
- サブ出力chがFull Range / Managedを兼ねない。LFE入力と同じ番号をサブ出力にする構成は許す。
- `su != 0`ではManaged / LFEに宛先があり、`rt`は`su`の部分集合、`ri`は`rt`の部分集合である。
- スロープは24 / 48 / 96 dB/oct。スキーマが数値範囲として受理する中間値でも、カーネル契約外の値は拒否する。
- `su == 0`は未設定の通過状態として扱う。上流が行う役割・ルートの無効化を踏まえて検証し、不要なFIRを生成しない。ゲイン・遅延まで常にバイパス相当とは扱わない。

不正ルートはホスト側で読込エラーにする。上流カーネルには設定不正時のdry fallbackがあり、`setParams`成功だけでは設定が有効な証拠にならないためである。ライブ変更失敗時は既存の実行パイプラインを保持する。[カーネルの検証・fallback](https://github.com/Frieve-A/effetune/blob/v2.11.0/dsp/plugins/basics/bass_management/kernel.cpp)

IIRはアセット不要、報告遅延0。Managedの高域を同じchへ残し、低域とLFEを指定サブへ分配する。複数サブへの分配係数は`1 / 宛先数`であり、等電力配分ではない。LFEの任意ローパス、Bass / LFE gain、Headroom、極性反転はカーネルに任せる。Full Range、Unused、LFEとサブの番号重複、全出力のHeadroom、ホストバイパス時の元の配線への復帰をPCMで確認する。

4chのステレオ＋2サブ、6chサラウンド、16chのマスク上端を検証用プリセットで観測する。PipeWireの既存チャンネル配置を使用し、特に9ch以上のAUX接続は利用者による明示的な配線を前提とする。DSP上の番号と実スピーカー配置を区別する。

### Bass ManagementのLinear

プリセットにIRファイルを追加する方式にはせず、上流`js/bass-management/design-core.js`と同じ設計で、読込・再構築時にFIRを生成する。Managedごと、およびLFE Low-passが有効なLFEごとに、指定周波数・スロープの低域FIRを作る。高域の補数とサブへのミックスはカーネルが担当する。上流は線形位相・2帯域のFIR Crossover設計から低域を取り出しているため、既存C++実装の必要な係数設計部分だけを再利用する。[設計実装](https://github.com/Frieve-A/effetune/blob/v2.11.0/js/bass-management/design-core.js)

転送は既存`instanceAssetCopy`ブリッジを利用する。slot 0、`ET_ASSET_F32_MULTICH`、ETA1、topology 4、head block 128、rate divider 1。input countとprocessing channelsは実処理幅、IR本数とpath countはローパスを必要とする入力数。各pathは入力chから同じchへの対角経路で、入力ch昇順にIR番号を割り当てる。FIR Crossoverの左右2入力・帯域出力用マッピングは流用しない。

タップ数は8192 / 16384 / 32768。報告遅延は`taps / 2 + 128`で、それぞれ4,224 / 8,320 / 16,512 frames。48 kHzでは88 / 約173.33 / 344 msとなる。Full RangeとLFEを含む実出力の整合、後段との遅延合算、レート変更時の通知を確認する。Linearでも`su == 0`や必要ローパス数0ならアセットを作らず、上流どおりの通過・経路・遅延を保つ。[アセット契約を検証するカーネル](https://github.com/Frieve-A/effetune/blob/v2.11.0/dsp/plugins/basics/bass_management/kernel.cpp)

容量はIR本体だけでなく、畳み込み領域・作業領域を含む32 MiB制限で検証する。既存容量見積もりと[上流の見積もり](https://github.com/Frieve-A/effetune/blob/v2.11.0/js/ir-library/ir-plugin-contract.js)を照合し、16ch・最大タップの受理可能範囲を実測する。受理できない構成は説明付き読込エラーにし、自動的なタップ削減やIIRへの変更はしない。不足パッケージがあればインストールを案内する。

係数生成・アセットの確保と転送は制御側で完了させる。新しいワーカースレッドや音声コールバック内の設計処理は追加しない。上流の段階的なconvolver準備・warmup・128フレームのfadeは維持し、投入成功と処理ACTIVEを区別して音声を検証する。準備中は通常メインの遅延付きdry、サブ無音等の状態があるため、起動直後だけを見て対応済みと判定しない。プリセット再読込・レート変更・Scalar/SIMD切替は既存の新パイプライン構築で係数も再生成する。

検証は次の二系統とする。

1. 上流JS設計をNode.jsで呼ぶ独立した比較と、PipeTuneが生成した係数を用いた実PCM・周波数応答・高低域再合成の確認。対象スロープ3種、タップ3種、周波数上下端、代表レート、1〜16chの代表構成を分担する。
2. 上流golden全3ケース（`2e-4`）。Linearのcase-003は設計済みクロスオーバーではなく、`sparse-decay-v1`の合成IRを指定している。そのIR・path・seed・準備手順を正確に再現して共有ライブラリへ渡す。これだけで製品のFIR設計まで検証したとはしない。

アセットを扱うテストは既存ブリッジのコピー契約を使い、64bitネイティブポインターを`uint32_t`へ変換する経路を増やさない。外部API変更時には公式文書とAPIコメントの両方を確認し、新しい内部API依存は追加しない。

### Chroma Spiral

音声を変更せず、DSPパラメータは0個。色・表示オクターブ・グラフ試聴などのUI機能は実装しない。既存7種類と合わせて、パラメータパック・インスタンス作成・有効DSP数・96ノード制限の計数より前に除外する。[公式契約](https://github.com/Frieve-A/effetune/blob/v2.11.0/docs/dsp/effects/chroma-spiral/index.md)

単独、加工DSP・Sectionとの混在、異なるバス指定、96個超の解析ノードを検証する。解析ノードを除いたプリセットとPCM・遅延・警告が一致し、除外ノードのバスコピーが発生しないことを確認する。FM / TV / MultiChannel Panelは音声を加工するため、この分類へ入れない。

## 既存DSPとエンジンの変更への対応

### オーバーサンプリングと遅延

歪み系6種類の`os`は既定1。通常は1 / 2 / 4 / 8、Hard Clippingのみ16も有効。スキーマ自体は整数範囲を受理するが、例えば3などをカーネルは1倍として扱う。既存パッカーと上流の挙動を維持し、独自の丸めやenum制約を追加しない。1倍では追加遅延0、2倍以上では倍率にかかわらず64 framesとなる。dry混合も整合される。[共通実装](https://github.com/Frieve-A/effetune/blob/v2.11.0/dsp/include/effetune/dsp/oversampled_shaper.h)

6種類すべてで既定・有効倍率・範囲内の無効倍率、dry/wet、実遅延、更新golden、レート・バックエンド切替を確認する。OSありの高周波歪み成分と折り返しの代表ケースも確認する。倍率を変えたプリセットの再読込で遅延が更新されることを検証し、製品に新しいリアルタイムパラメータ操作APIは追加しない。

Brickwall Limiterは既存のOSパラメータのまま実装が変わり、OS有効時の遅延はlookahead＋64 framesになる。旧版の`ceil(62 / 倍率)`相当の期待値を残さない。新PCM golden、しきい値変更、dry相当の入力、出力上限を意味的に確認する。[カーネル](https://github.com/Frieve-A/effetune/blob/v2.11.0/dsp/plugins/dynamics/brickwall_limiter/kernel.cpp)

### 入力遅延整合

上流2.11.0は、DSPへ渡す選択chを、その中の最大入力遅延へ揃えてから処理する。別バスsendは作業用コピーだけを整合し、送信元バスを変更しない。混合先と最終出力にも必要な補償が加わる。[公式説明](https://github.com/Frieve-A/effetune/blob/v2.11.0/dsp/README.md)、[実装](https://github.com/Frieve-A/effetune/blob/v2.11.0/dsp/core/engine.cpp)

片chまたは一部chだけに遅延のあるDSPを置き、その後にMatrix、Spatial Mapper、Bass Managementなどのch混合を置くプリセットを検証する。Allとペア、同一バスと別バス、send元を使う別経路、加算merge、reset後の履歴を含める。最終出力の遅延値だけではなく、混合前の信号が揃い、不要な二重遅延がないことをインパルスで確認する。

Time Alignmentの意図的な配置調整とエンジンが報告する処理遅延は区別する。`dl`を自動的にホスト補償へ加算する変更はしない。最大500 msを超える長さのPCMで実際の遅延を測り、高レート・16chでのメモリとresetを確認する。[Time Alignment](https://github.com/Frieve-A/effetune/blob/v2.11.0/dsp/plugins/delay/time_alignment/kernel.cpp)

### その他の回帰

MP3は更新された心理音響・量子化処理と長い内部履歴を持つため、変更されたgoldenだけでなく、既存nativeテストと複数フレームにまたがる音声、モード・レート変更後の復帰を確認する。Tubeの初期化短縮、AR補間変更、FM / TV / MultiChannel Panelのテレメトリ変更も既存テストと共有バックエンド経由の音声で確認する。音声仕様の変更と性能最適化を混同しない。

上流のgolden許容誤差はケースのメタデータを使用し、SIMD差を隠すために緩めない。新しい刺激種別が必要なら、上流と同じ生成方法を追加する。固定sleepではなく処理フレーム数、アセット状態、実際の出力で検証する。

## インクリメンタルな実装手順

各段階はCLIとPCM処理で結果を観測でき、ビルド・全体テストが可能な単位とする。製品修正前に不具合を再現するテストを追加してREDを確認し、修正後にGREENを確認する。既存実装で最初から成功する追加検証は回帰テストとして扱い、人為的なREDや不要な製品変更を作らない。段階ごとに全体テストを実行してコミットする。

### 1 参照更新とアセット不要DSPの基本対応

2.10.0で全体テストを実行して基準を記録する。107カーネル、Attack Tonal Balance / Bass Extenderの実処理、Chroma Spiralの無警告除外を旧版で検証してREDを確認する。未取得のgoldenファイルがないだけの失敗にはしない。

公式タグへサブモジュール参照を更新し、カタログ再生成、解析分類、Attack Tonal Balanceのコンパイル条件、Bass Extenderの処理幅エラーを実装する。上流で変わったパラメータ数・遅延の既存テストは契約差を説明して更新する。Bass Managementはこの段階では既存の外部アセット未対応警告が残る中間成果物とする。

完了条件: 全体GREEN、CLI/GTKのEffeTune表示2.11.0、全バックエンドの107カーネル契約成立、解析8種類の除外、新規アセット不要DSPの48 kHz実行。`deps/`内の外部ソースは編集しない。

コミット: `feat: update EffeTune dependency to 2.11.0`

### 2 Attack Tonal BalanceとBass Extenderの利用条件確定

新規19 goldenを既存artifactテストへ追加し、上記の意味的検証、処理幅、対応レート、無音時停止、再読込とバックエンド切替を実施する。検証用プリセットを用意してCLIベンチマークから観測する。製品修正が必要な場合は、その再現テストから着手する。

完了条件: 全体GREEN、golden許容誤差内、Attackの実遅延と残差、Bass Extenderの生成低域・レート制限・選択chが確認できる。代表構成の処理時間・メモリを記録する。

コミット: `feat: validate Attack Tonal Balance and Bass Extender support`

### 3 Bass ManagementのIIR対応

現行ではアセット警告で除外されるIIRプリセットの再現テストを追加してREDを確認する。Bass Management固有の設定検証と、アセット不要モードを実行できる分岐を実装する。LinearでFIRが不要な構成も扱い、FIRが必要なLinearはこの段階では明示的な未対応エラーにする。

4chのステレオ＋2サブなどのプリセットを用意し、役割・宛先・極性・分配係数、LFE番号重複、不正設定、`su == 0`、All制約を検証する。IIRのgolden 2件を共有バックエンドで比較する。

完了条件: 全体GREEN、IIRは外部アセット警告なしで実行可能、期待するchへ期待する帯域・レベルが出る。不正設定と未対応Linearが説明付きで失敗し、ライブ失敗時に旧パイプラインを保持する。

コミット: `feat: support Bass Management IIR presets`

### 4 Bass ManagementのLinear対応

Linearの期待PCMとアセット転送を検証するテストでREDを確認する。係数設計の必要部分を再利用し、89 floatの設定との整合、対角path、固定head block、容量見積もり、DSPごとのパラメータ補正を実装する。JS設計比較とgolden用の合成IR転送も追加する。

同じ多chプリセットをIIR / Linearで実行可能にし、全3タップ、スロープ3種、レート・幅の代表ケース、準備からACTIVEへの移行、実遅延、失敗時の保持、無音時停止からの復帰を確認する。既存FIR Crossover / PEQ / Group Delay / Crosstalkも回帰確認する。

完了条件: 全体GREEN、必要なLinear FIRが正しく生成・転送され、設計比較・実PCM・上流3 goldenが成功する。IIR / Linear切替、再読込、レート・バックエンド再構築が成立し、容量限界と高負荷構成の結果が記録される。

コミット: `feat: support Bass Management linear phase presets`

### 5 既存DSPと遅延補償の回帰を確定

歪み系6種類、Brickwall Limiter、Time Alignment、MP3と共通遅延補償について、上記の音声・境界・再構築テストを補完する。旧版とPCMが変わるケースは2.11.0の公式仕様・goldenに合わせる。上流変更だけで成立するケースに独自修正を加えない。

完了条件: 全体GREEN、更新goldenと意味的検証が成功し、OS・500 ms遅延・混合前整合・send元保持をPipeTuneプリセット経由で観測できる。既存TV / Spatial Mapperと生成FIR機能も回帰しない。

コミット: `feat: validate EffeTune 2.11 DSP and latency changes`。独立した製品不具合は再現と修正を含む`fix:`コミットに分ける。

### 6 利用者文書と配布検証

英日README、`pipetune/README.md`、`pipetune/docs/dsp-backends.md`、`pipetune/docs/architecture.md`と必要な公開APIコメントを更新する。版・登録数、解析除外、Bass Extenderのレートと処理幅、Bass ManagementのAll・出力接続・Linear遅延、Attackの遅延を説明する。SIMD説明のPFFFT利用先も今回確認した実装に合わせる。過去の対応計画は履歴として維持する。

クリーンRelease、Debug/Release全体テスト、コンポーネント単独ビルド、一時DESTDIRへのインストール、GTK既存E2E、既存13構成のパッケージ生成・インストール・起動を検証する。配布ライブラリを使って新規加工3DSPを実行し、Bass ManagementはIIRとLinearの両方を含める。

完了条件: 最終完了条件を証拠と照合し、未検証のISA・実機性能・物理スピーカー接続を区別して記録する。

コミット: `doc: describe EffeTune 2.11.0 support`

## 検証の実施条件

- 基本は`make test`によるDebug全体実行。必要なら既存CMake経路で並列ビルド・CTest全件を実行する。最終段階では空のReleaseディレクトリを`BUILD_TESTING=ON`で構成し、全件を実行する。
- 上流追加4種類のnativeテストも取り込む。直接カーネルのテスト、PipeTune共有ライブラリのgolden、プリセットのルーティング検証をそれぞれ実施する。カタログ登録やファイル存在だけを成功条件にしない。
- 既存のJSパッカー比較、生成FIR、Crosstalk、解析除外、レート変更、Scalar/SIMD切替、起動失敗・復帰、GTK E2Eを全体実行に含める。
- 比較対象のScalarとCPUで実行可能なSIMDを明記する。x86 FMA差、32bit、ARM、RISC-V、既存のNote Spectrogramモデル移植条件を確認する。
- 13配布構成はDebian bookwormのx86_64/i686/arm64/armv7l、Debian trixieの同4種＋riscv64、Ubuntu 24.04/26.04のx86_64/arm64。パッケージ起動成功、DSP実行成功、golden比較成功を区別する。
- 長い遅延・FIR準備・エミュレーションを踏まえてテスト時間を確保する。時間切れを理由に範囲を縮小せず、処理フレーム数と必要資源を基にタイムアウトを設定する。
- 全レート×全ch×全パラメータの無制限な直積は作らず、各境界を代表ケースで分担する。Bass Extenderの対応6レートとLinearの3タップは全て含める。
- 上流のソースは直接編集しない。統合で障害が見つかった場合は原因と設計前提を本書へ反映してから修正し、不要になった試行は戻す。計画とテストの契約が矛盾する場合は確認する。
- 独自スクリプトはNode.js、追加のビルド時テキスト処理は`funcity`を使用する。上流の既存モデル生成依存は維持する。新しいツール基盤への移行は本計画に含めない。
- PipeTuneのリリース番号はこの計画では変更しない。必要になった場合は既存の`screw-up format`経路を使用する。

検証対象コミット、実行コマンド、ログ、golden比較、ベンチマーク、配布物ハッシュは`artifacts/verification/effetune-2.11.0/`へまとめ、実装完了時に本書へ結果を追記する。

## 最終完了条件

- [ ] 指定の公式v2.11.0を参照し、外部ソースを変更していない。
- [ ] CLI/GTKがEffeTune 2.11.0を表示し、全バックエンドの107カーネル・パラメータ・アセット容量契約が一致する。
- [ ] Attack Tonal Balanceの音声、長い遅延、成分制御、再構築を検証した。
- [ ] Bass Extenderの生成低域、1〜2ch処理、対応レートと失敗・回復を検証した。
- [ ] Bass ManagementのIIR / Linear、設定検証、ch経路、極性、設計比較、アセット準備、容量・遅延を検証した。
- [ ] Chroma Spiralを含む解析8種類がPCM・遅延・有効ノード数に影響せず無警告で除外される。
- [ ] 歪み系6種類、Limiter、Time Alignment、MP3などの変更と入力遅延補償が公式契約に従う。
- [ ] 既存DSP・生成FIR・Crosstalk・無音時停止・ライブ再構築の全体回帰に成功する。
- [ ] Debug/Release全体テスト、単独ビルド、インストール、GTK E2Eと13配布構成の結果を記録した。
- [ ] 利用者文書が利用条件・制限を説明し、実機未検証範囲と最終条件の照合を記録した。

## 参照と主な変更対象

公式情報はすべて対象タグへ固定する。

- [公式2.11.0リリース](https://github.com/Frieve-A/effetune/releases/tag/v2.11.0)、[2.10.0からの差分](https://github.com/Frieve-A/effetune/compare/v2.10.0...v2.11.0)
- [DSP README](https://github.com/Frieve-A/effetune/blob/v2.11.0/dsp/README.md)、[公開C ABIとコメント](https://github.com/Frieve-A/effetune/blob/v2.11.0/dsp/include/effetune/abi.h)、[nativeビルド](https://github.com/Frieve-A/effetune/blob/v2.11.0/dsp/CMakeLists.txt)
- [Bass Managementのnativeテスト](https://github.com/Frieve-A/effetune/blob/v2.11.0/dsp/plugins/basics/bass_management/native_test.cpp)、[goldenのケースと合成IR指定](https://github.com/Frieve-A/effetune/blob/v2.11.0/dsp/plugins/basics/bass_management/cases.json)、[公式利用説明](https://github.com/Frieve-A/effetune/blob/v2.11.0/docs/dsp/effects/bass-management/index.md)
- [MP3心理音響モデル](https://github.com/Frieve-A/effetune/blob/v2.11.0/dsp/plugins/lofi/mp3_codec_simulator/psycho_model.h)、[MP3カーネル](https://github.com/Frieve-A/effetune/blob/v2.11.0/dsp/plugins/lofi/mp3_codec_simulator/kernel.cpp)

PipeTune側の主な対象は`pipetune/tools/generate-dsp-catalog.mjs`、`pipetune/src/dsp_pipeline.cpp`、`pipetune/src/generated_fir_asset.cpp`とヘッダー、`pipetune/cmake/EffeTuneNativeBackends.cmake`、`pipetune/CMakeLists.txt`、`pipetune/test/preset_pipeline_test.cpp`、`pipetune/test/parameter-packer-parity.mjs`、`pipetune/test/effetune_backend_artifact_test.cpp`、既存のpipeline slot / backend / sample rateテスト、上記利用者文書である。JS設計比較用の小さなNode.jsランナーとC++ランナーは既存の比較テスト方式に合わせて追加する。

## 計画作成の完了確認

- [x] 現行PipeTuneと公式タグ間差分を基に、新規4種類と既存変更を整理した。
- [x] 通常DSP、解析除外、Bass Managementの条件付きアセットを区別し、処理幅・レート・遅延・失敗処理を定義した。
- [x] 各段階の実行可能な成果物、TDD、検証、コミットと完了条件を定義した。
- [x] 調査済みの事実、ホスト側の設計方針、未実施の実行検証を区別した。

計画作成時点では製品実装・サブモジュール参照は変更していない。本書だけの変更でビルドステップに影響しないため、計画作成時のビルド・テストは実施しない。

## 実施記録

- 段階1の基準: `make test`で2.10.0の151件を実行。150件が成功し、単独コンポーネントの版比較のみ失敗した。検証中にテストを追加したため、`screw-up format`の算出版がクリーン時の2.10.1から変更あり時の2.10.2へ変わったことが原因。変更を一時退避して作業ツリーを固定し、`ctest --test-dir build/test --output-on-failure -R '^pipetune_component_build$'`を再実行して成功した（131.92秒）。初回全体ログは`baseline-debug.log`、再確認は`baseline-component-recheck.log`。以降の全体検証中は作業ツリーを固定する。
- 段階1のRED: 旧版共有ライブラリを使う別のテスト実行ファイルで、107カーネルと新規4登録、新規2加工DSPの無警告実行、Bass Extenderの処理幅拒否、解析8種類の無警告除外が失敗することを確認した。goldenファイル不足による失敗ではない。ログは`step1-red-artifact.log`と`step1-red-preset.log`。
- 段階1の実装: 公式リモートのタグと一致する`e200e5151b919bcb2481626b2aa3a0295baa247f`へ参照を更新。Chroma Spiral分類、Attack Tonal Balanceの`-ffp-contract=off`、Bass Extenderの3ch以上の処理幅に対するノード名付きエラーを追加した。外部ソースの編集はない。
- 段階1のGREEN: `cmake -S . -B build/test -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON`、`cmake --build build/test --parallel 12`、`ctest --test-dir build/test --output-on-failure --parallel 6`で全155件成功（132.65秒）。ログは`step1-build.log`と`step1-debug.log`。上流追加4nativeテスト、JSパッカー、共有バックエンド契約、単独Releaseビルド、GTK E2Eを含む。Scalar / baseline / x86-64-v3で共有ライブラリを実ロードした。x86-64-v4はビルドのみで、CPU非対応のため実行対象外。
- CLI/GTKの`--version`はともに`EffeTune DSP 2.11.0`を表示。48 kHzのAttackは5,120 frames後のPCM再現、Bass Extenderは有限・非ゼロの加工結果を確認した。解析ノード100個を含むプリセットは無警告で除外され、PCM・遅延とバス動作が加工ノードのみの場合と一致する。段階1の完了条件を満たした。Bass Managementは計画どおり、この時点では外部アセット未対応警告で除外される。
- 段階2: Attack 9件、Bass Extender 10件の全公式goldenを、Scalar / baseline / x86-64-v3のDebug共有ライブラリと単独Release共有ライブラリで比較した。許容絶対誤差はメタデータの`2e-5` / `2e-6`を維持。各ケースの実測最大誤差は`step2-goldens.log`に保存した。テストランナーへ無音刺激と10件以上の連番読み込みを追加し、製品コードの追加修正は不要だった。
- 段階2の意味的検証: Attackの固定5レート、1/2/6/16ch、All・ペア・単一ch、端数ブロック、reset後の異なるブロック分割、レート/Scalar/SIMD再構築、Spatial Mapperとの遅延合算をPCMで確認。短いアタックと持続音のboost/cut/無効化、残差の非ゼロ出力、成分の再合成を確認した。Bass Extenderは全6対応レートで100 Hzから50 Hzの成分が生成されること、16chの上端ペア、対象外ch保持、1/2ch、逆相・非対称ステレオ、amount 0、出力gain、無音とreset再現を確認。32/352.8/384 kHzの拒否、Automatic起動失敗と固定48 kHzでの回復、ライブSIMD切替と失敗時保持も成功した。
- 段階2の無音停止: 新規2DSPを実処理してから、入力フレーム数でtimeout＋fade後のsleepとDSP呼び出し停止を検証。復帰後は遅延を超えるPCMを新規パイプラインと比較し、残留履歴が消え、非ゼロ音声へ戻ることを確認した。固定sleepは使用していない。
- 段階2の全体結果: Debug全156件成功（132.11秒、`step2-debug.log`）。`cmake --build build/test --parallel 12`後に`ctest --test-dir build/test --output-on-failure --parallel 6`を実行。追加テストのペア表記と設定ファイル共用による失敗をテスト側で修正した上で全件成功した。GTK E2E、単独Releaseビルド、既存DSP/FIR/Crosstalk回帰を含む。
- 段階2の観測用プリセット: `pipetune/test/presets/effetune-2.11/`へAttackとBass Extenderを追加。`build/effetune-2.11-benchmark`をRelease / `BUILD_TESTING=OFF`で構成してCLIをビルドし、全体テスト終了後に`node artifacts/verification/effetune-2.11.0/benchmark-step2.mjs`で計測した。256 frames/block、128 warmup blocks、256 measure blocks。Attackは48/192/384 kHz×2/16ch、Bassは48/192 kHz×2/16ch（16chストリームでは先頭ペアを処理）の計10構成、利用可能3バックエンドを合わせて30実行。全構成で有効DSP数1、除外0、有限・非ゼロの出力を確認した。
- 段階2の性能記録: AttackのScalarは476〜1,445 ns/frame、プロセス最大RSSは16,912〜30,752 KiB。Bass ExtenderのScalarは25〜135 ns/frame、最大RSSは15,516〜15,968 KiB。個別JSONと`*.memory.txt`、集計`step2-benchmarks.json`に保存した。RSSはCLIと複数バックエンド全体で、実機一般のリアルタイム性能保証ではない。段階2の完了条件を満たした。
