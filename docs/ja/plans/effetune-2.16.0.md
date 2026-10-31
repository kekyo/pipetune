# EffeTune 2.16.0対応計画

## 目的と基準

PipeTuneのEffeTune参照を2.15.0から2.16.0へ更新する。今回の実装対象はDSP対応と、その検証・配布・文書更新とする。従来と同じく、生成カタログ、実共有バックエンド、製品の音声経路、配布物の順に検証する。

ネットワークプレーヤーは、課題と実現案を本書へ記録するに留める。PipeTuneを再生端末として使うための実装は、次のステップへ残す。ネットワーク機能の実装・接続設定・相互運用試験は、今回のDSP更新の工程と完了条件に含めない。

後続の検討では、EffeTuneなどから選曲し、PipeTune側で音源取得、デコード、DSP、音声出力を行う構成を対象とする。後述する案A〜Dは比較資料として残す。案Dは処理済み音声を別端末へ送る候補であり、PipeTuneを再生端末にする機能とは用途が異なる。採用する構成と対応クライアントは、次のステップで確定する。

調査日は2026-10-30。PipeTuneの基準は`develop`の`da84d375d11245f19dbc94b31a4355f6aee06b69`で、調査開始時の作業ツリーはクリーンだった。

| 項目 | 現行 | 更新先 |
| --- | --- | --- |
| 公式アプリタグ | `v2.15.0` | `v2.16.0` |
| コミット | `45d2f69b63483c4a2e98758bd8a5715cb13e3266` | `3769b9aefebf2dce2de3169690d4a95e1716da4c` |
| DSPのCMakeプロジェクト版 | 1.0.0 | 1.16.0 |
| DSPのJSバインディング版 | 0.15.0 | 0.16.0 |
| 登録カーネル数 | 112 | 112 |

公式リモートのタグと取得したタグは同じコミットを指している。PipeTuneの表示対象はアプリ版の2.16.0とし、DSPライブラリやバインディングの版と混同しない。計画作成ではサブモジュールのチェックアウトと製品コードを変更していない。[公式リリース](https://github.com/Frieve-A/effetune/releases/tag/v2.16.0)、[タグ間差分](https://github.com/Frieve-A/effetune/compare/v2.15.0...v2.16.0)

## DSPの変更と対応方針

### パラメータと解析処理

カーネルの追加・削除はなく、生成メタデータのパラメータ契約が変わるのはRhythm AnalyzerとNote Spectrogramである。共有解析の影響を受けるSFZ Note Playerも合わせて示す。

| DSP | 2.15.0 | 2.16.0 | PipeTuneでの扱い |
| --- | --- | --- | --- |
| Rhythm Analyzer | 3 float、`0xbae05865` | 4 float、`0xf854c585`。表示方式`dm`を追加。`band`が既定、`drums`でドラム表示 | 従来どおり実行する。表示の有無とクリック音の有無を別々に検証する |
| Note Spectrogram | 3 float、`0xafc5ff07` | 4 float、`0xb8518943`。コード・調の検出を指定する`ck`を追加。既定はfalse | カタログを更新し、表示専用DSPとしての除外を維持する |
| SFZ Note Player | 20 float、`0xdfb4bbc5`、slot 0は1 GiB | パラメータ、アセット容量とも同じ | Classic/Event、初回発音と休止再開を回帰検証する |

`ck`はDSPごとに意味が異なる。Rhythm Analyzerでは既存のメトロノームクリックを表し、Note Spectrogramでは今回追加されたコード・調の検出を表す。[Rhythmのパラメータ](https://github.com/Frieve-A/effetune/blob/v2.16.0/dsp/plugins/analyzer/rhythm_analyzer/params.json)、[Note Spectrogramのパラメータ](https://github.com/Frieve-A/effetune/blob/v2.16.0/dsp/plugins/analyzer/note_spectrogram/params.json)、[生成された契約](https://github.com/Frieve-A/effetune/blob/v2.16.0/dsp/bindings/generated/effects-v1.private.json)

Rhythm Analyzerは音声から拍を検出し、設定によってクリック音を加えるDSPである。PipeTuneは現在もクリック無効時にこのDSPを実行している。2.16.0のカーネルは`dm=band`でもドラム解析器を準備し、入力を渡すため、表示を追加しない場合もCPUとメモリの比較が必要になる。`dm`の値を強制変更したり、クリック無効時にDSPを除外したりして負荷を隠さない。[Rhythmカーネル](https://github.com/Frieve-A/effetune/blob/v2.16.0/dsp/plugins/analyzer/rhythm_analyzer/kernel.cpp)

Note Spectrogramにはコード・調の検出モデルが追加される。SFZ Note Playerも同じ音符解析処理を使うが、コード・調の検出はfalseで構成する。SFZの音声仕様が変わったとは扱わず、共有解析の変更による音声・初期化時間・メモリへの影響を検証する。2.15.0対応で追加したEvent用の無音初期化は維持する。[共用解析](https://github.com/Frieve-A/effetune/blob/v2.16.0/dsp/plugins/analyzer/note_spectrogram/spectral_analysis.h)、[SFZカーネル](https://github.com/Frieve-A/effetune/blob/v2.16.0/dsp/plugins/others/sfz_note_player/kernel.cpp)

### ABI、参照PCM、アセット

上流には`et_instance_stream_boundary`が追加される。ただし公開ヘッダーのコメントは`App-internal`としているため、PipeTuneから新たに呼び出したり、独自バックエンドAPIへ追加したりしない。通常のresetと既存の休止再開経路で検証する。Rhythmの解析結果を受け渡すテレメトリはversion 4の1496 bytesからversion 5の2024 bytesへ変わるが、PipeTuneに解析表示機能は追加しない。上流nativeテストで新形式と境界処理を検証する。[ABI宣言とコメント](https://github.com/Frieve-A/effetune/blob/v2.16.0/dsp/include/effetune/abi.h)、[テレメトリ仕様](https://github.com/Frieve-A/effetune/blob/v2.16.0/dsp/README.md)

既存のABIパッチは2.16.0の一時コピーに対して`patch --dry-run --fuzz=0`で適用できた。実装時には通常ビルドで確認する。外部ソースを直接編集せず、公式タグへの参照更新とPipeTune側の生成・ビルド設定で対応する。

上流のgoldenディレクトリで変わった171ファイルは全てJSONで、PCMファイルに差分はない。Note Spectrogramの5件には`ck`、Rhythmの6件には`dm`と対応する参照ハッシュの更新がある。他の160件は`jsEngineHash`の更新である。SFZの13件はJSON・PCMとも変更がない。PCMの再生成や許容誤差の緩和を更新手順に含めない。[上流DSPテスト資材](https://github.com/Frieve-A/effetune/tree/v2.16.0/dsp/plugins)

`js/sfz/`、DSP vendor、既存の木モデル埋込み処理には差分がない。IRライブラリには遠隔操作の取消・確定に関する変更があるが、DSP更新だけでPipeTuneにライブラリへの書込機能を追加する必要はない。IR・SFZ・Room EQの準備結果、保存形式、出典情報を確認し、出典版やキャッシュ識別子をアプリ版に合わせて機械的に変更しない。[SFZ準備処理](https://github.com/Frieve-A/effetune/tree/v2.16.0/js/sfz)、[IRライブラリの変更](https://github.com/Frieve-A/effetune/blob/v2.16.0/js/ir-library/ir-library-store.js)

音響測定には歪み・共振の解析が追加され、測定バックアップのIRレコードに任意の`harmonicAnalysis`が保存される。Room EQの設計処理と、PipeTuneが移植しているFFT・平滑化・再標本化処理には差分がない。測定画面やVisualizerの新表示は移植対象に含めず、追加情報を持つ2.16.0のバックアップからRoom EQとCrosstalk Cancellationが既存の線形IRを利用できることを検証する。現行`measurement_store.cpp`にはファイル全体で64 MiBの上限があるため、追加データによる容量増加も確認する。[測定データの保存形式](https://github.com/Frieve-A/effetune/blob/v2.16.0/features/measurement/dataStorage.js)、[Room EQ設計](https://github.com/Frieve-A/effetune/blob/v2.16.0/js/room-eq/design-core.js)

## ネットワークプレイの調査記録

### EffeTuneの機能が提供するもの

2.16.0は音楽共有と遠隔エフェクト編集に加え、AirPlayとUPnP AV/DLNAの受信を追加している。OpenHomeも含め、機器の発見、曲の選択・再生、音声データの取得、エフェクト編集は別の役割を持つ。[公式リリース](https://github.com/Frieve-A/effetune/releases/tag/v2.16.0)

UPnP AV/DLNAはLAN上の音楽サーバーや再生機器を操作する方式である。OpenHomeでは再生機器側が曲のキューを保持するため、操作アプリを切断しても曲順を維持できる。AirPlay受信では送信端末から音声を受け取り、再生の開始・停止を送信側で操作する。[上流のネットワーク機能ガイド](https://github.com/Frieve-A/effetune/blob/v2.16.0/docs/i18n/ja/openhome.md)

音楽共有では元の音声を配信し、EffeTuneを再生先に選ぶと再生先のDSPを適用する。送信側のDSP処理済み音声を配信する方式ではない。遠隔編集用の`remote-v1`はWebSocketで操作と状態を送るAPIであり、音声そのものは運ばない。したがって「EffeTuneから選曲してPipeTuneで鳴らす」「PipeTuneのDSPを遠隔編集する」「PipeTuneの処理済み音を別室へ送る」は、それぞれ別の対応になる。[音楽共有の仕様](https://github.com/Frieve-A/effetune/blob/v2.16.0/docs/i18n/ja/openhome.md)、[remote-v1仕様](https://github.com/Frieve-A/effetune/blob/v2.16.0/docs/remote-v1.md)

音源サーバー、再生端末、操作端末は別々に配置できる。音源のあるPCを再生先にも選ぶ場合、操作端末は選曲や設定を送るリモコンとして動作し、そのPCでDSPと音声出力まで行う。音源サーバーと再生端末が別の場合は、再生端末がHTTP/HTTPSで音声を取得する。PCM WAVはWAVのまま、FLACやMP3は圧縮されたまま転送し、再生端末でデコードする。配信にはHTTPのRange要求による部分取得も使える。[配信実装](https://github.com/Frieve-A/effetune/blob/v2.16.0/electron/network-music-server.cjs)、[端末の役割とHTTPS設定](https://github.com/Frieve-A/effetune/blob/v2.16.0/docs/i18n/ja/openhome.md)

EffeTuneのAirPlay受信はUxPlayを別プロセスで起動し、内部のループバックへRTP/UDPでPCMを渡す。形式はL16、44.1 kHz・16 bit・ステレオ・ビッグエンディアンであり、この経路は同じPC内の通信である。上流の配布手順でLinux向けに対応するのはx64であり、PipeTuneの9配布構成へそのまま使えるとは扱わない。AirPlay 2や複数室の同期も、この上流実装の対応範囲に含まれない。[AirPlay実装の説明](https://github.com/Frieve-A/effetune/blob/v2.16.0/native/airplay-receiver/README.md)、[ループバック送信の実装](https://github.com/Frieve-A/effetune/blob/v2.16.0/electron/airplay-receiver.cjs)、[POSIXビルド対象](https://github.com/Frieve-A/effetune/blob/v2.16.0/native/airplay-receiver/BUILD-POSIX.md)、[利用上の制約](https://github.com/Frieve-A/effetune/blob/v2.16.0/docs/i18n/ja/openhome.md)

### 現在のPipeTuneで再利用できる部分

| 現在の構成 | 実現案への影響 | コード |
| --- | --- | --- |
| PipeWireの`Audio/Sink`として音声を受ける | ネットワーク受信アプリを普通の音声入力元として接続できる | `pipetune/src/filter_graph_properties.cpp`、`pipewire_pipeline.cpp` |
| 複数出力モードにProcessedとDirectがある | DSPを通す受信音声はProcessedへ接続する。DirectではDSPを通らない | `pipetune/include/pipetune/pipewire_pipeline.h` |
| ローカル制御は同一ユーザーのUnixソケット | LAN公開には認証と操作範囲を管理する別の入口が必要 | `pipetune/src/control_socket.cpp`、`include/pipetune/control_protocol.h` |
| 制御はプリセットファイル読込、bypass、レート、出力設定など | 曲のキュー、URL再生、シーク、DSPパラメータの遠隔編集APIは追加が必要 | `pipetune/include/pipetune/control_protocol.h` |
| 同梱FFmpegはネットワーク無効。デコーダーは完全なアセットファイルを入力に取る | URLを渡すだけではネットワーク再生できない。連続読込や取消、シークを別に設計する必要がある | `pipetune/cmake/ConfigureFFmpeg.cmake`、`src/asset_audio_decoder.h` |
| 出力一覧はALSAに限定せず`Audio/Sink`を列挙する | PipeWireが公開するネットワークsinkを出力候補にできる可能性がある | `pipetune/src/pipewire_output_inventory.cpp` |

ネットワーク音声を既存入力へ接続する方式では、DSP計算、レート変換、休止再開、チャンネル割当を共用できる。通常のデスクトップ音声との混合も既存のPipeWire経路で行える。ただし、上流EffeTuneの「新しい再生元へ排他的に切り替える」動作と同じにはならない。最初の受信対応では通常のアプリと同じ混合を基本とし、排他再生が必要なら別途仕様を決める。

### 再生端末として次のステップに残す課題

PipeTuneを再生端末にする場合、音声データを取得する経路と、操作を受け付ける経路の両方が必要になる。既存のEffeTuneと接続するために必要なプロトコルを、接続方法ごとに整理する。

| 用途・接続方法 | PipeTune側で必要になる対応 |
| --- | --- |
| 音源サーバーから音声を取得 | HTTP/HTTPSクライアント。対応形式、部分取得、読込取消、通信エラーの扱いを決める |
| EffeTuneデスクトップ版から再生先を発見し、選曲・再生 | OpenHomeまたはUPnP AVの再生機器として動作する。機器発見のSSDPと、HTTP上でXMLの操作要求を受けるSOAPを含む |
| EffeTuneからDSPを遠隔編集 | `remote-v1`のWebSocketサーバー。認証、対応機能の通知、操作と状態の同期を実装する |
| EffeTune PWAへ直接登録して再生操作 | `/pwa/v1/...`のHTTP APIとの互換を検討する。DSP編集のWebSocket接続も別に確認する |

EffeTuneデスクトップ版は、OpenHomeのPlaylistサービスまたはUPnP AVのAVTransportサービスへ音源URLと操作を渡す。HTTPクライアントと`remote-v1`リスナーだけでは、この再生先としての動作を満たさない。再生制御に使うサービスの対応範囲を決め、実クライアントで機器発見、音源指定、状態取得を検証する必要がある。[OpenHome操作の実装](https://github.com/Frieve-A/effetune/blob/v2.16.0/electron/network-music-host.cjs)、[UPnP AV操作の実装](https://github.com/Frieve-A/effetune/blob/v2.16.0/electron/network-music-av-player.cjs)、[機器発見とSOAPの実装](https://github.com/Frieve-A/effetune/blob/v2.16.0/electron/network-music-upnp.cjs)

`remote-v1`には音楽操作用の`music1`拡張もある。ただし、`playMusic`は音声URLを直接受け取らず、`serverId`と`trackId`で曲を指定する。PipeTuneがこのAPIを受け付けるなら、音源サーバーの識別と曲IDからURLへの解決も必要になる。キュー、シーク、再生用音量、操作完了後の状態取得も含めて検討する。[music1仕様](https://github.com/Frieve-A/effetune/blob/v2.16.0/docs/remote-v1.md#network-music-music1)、[音楽操作の実装](https://github.com/Frieve-A/effetune/blob/v2.16.0/electron/remote-music-dispatcher.cjs)

既存PWAの直接接続では、`/pwa/v1/info`で接続先の情報を取得し、`resolve`で音源を解決し、`player`で再生を操作する。エフェクト編集は`/pwa/v1/effects`へのWebSocket接続を使う。`music1`への対応だけでPWAとの互換が成立するとは扱わず、これらのHTTP API、認証、対応機能の通知も確認する。EffeTuneを仲介してOpenHome/UPnP AVの再生先を操作する構成は、PWAからPipeTuneへ直接登録する構成と分けて検討する。[PWAの通信実装](https://github.com/Frieve-A/effetune/blob/v2.16.0/js/network-music/pwa-transport.js)、[PWAの接続方法](https://github.com/Frieve-A/effetune/blob/v2.16.0/docs/i18n/ja/openhome.md#pwa)

再生エンジンも次のステップの課題とする。現在の`decodeAssetAudio()`はメモリ上の完全なファイルを入力に取り、同梱FFmpegのネットワーク機能は無効である。HTTP取得に加えて連続デコード、上限を設けたバッファ、再生・停止・シーク、キュー、切断と取消、既存DSP入力へのPCM供給が必要になる。既存プレーヤーへ任せる案と内製する案を比較し、通信やバッファ待機を音声コールバックから分離する。根拠は`pipetune/src/asset_audio_decoder.h`、`pipetune/cmake/ConfigureFFmpeg.cmake`、`pipetune/include/pipetune/control_protocol.h`とする。

## ネットワークプレイの4案（次のステップの比較資料）

以下はコードと公開仕様に基づく設計案であり、今回の実装対象には含めない。PipeTuneとの接続・実機相互運用は、次のステップで採用する構成を決めてから検証する。規模は相対比較であり、日数の見積りではない。

| 案 | 利用者ができること | 主な追加作業 | 規模 |
| --- | --- | --- | --- |
| A 既存の受信アプリを接続 | AirPlayやOpenHome/DLNAの音声をPipeTuneのDSPで聴く | 接続設定、同一ユーザーでの起動、PCM検証、導入手順 | 小 |
| B 受信サービスをPipeTuneへ統合 | PipeTuneの設定画面から受信を有効化し、状態を確認する | 専用サービス、設定・CLI・GTK、起動停止と障害復旧、配布 | 中 |
| C EffeTuneの遠隔操作に対応 | EffeTuneのデスクトップ/PWAからPipeTuneのDSPを操作する。音楽操作も段階的に追加する | `remote-v1`、ペアリング、状態同期、プリセット/アセット対応。音楽操作には接続方法に応じたAPIと再生機能 | 大 |
| D PipeWireのネットワーク出力を利用 | PipeTuneのDSP処理後のデスクトップ音声を別端末へ送る | RTP/RAOP sinkの設定・識別・再接続・遅延検証 | 小〜中。厳密な同期は別計画 |

### 案A 既存の受信アプリを接続する

AirPlayはUxPlay、OpenHome/UPnP AVはupmpdcliとMPDを候補とする。受信アプリの出力を同じユーザーのPipeWireセッションに接続し、単一出力モードではPipeTuneの処理入力、複数出力モードではProcessedを明示的に選ぶ。

音声経路は「スマートフォンまたは音楽サーバー → 受信アプリ → PipeTune入力 → DSP → 設定済み出力」となる。upmpdcliはUPnP/OpenHomeの操作を受け、MPDが曲を取得・再生する。MPDには接続先を指定できるPipeWire出力があり、UxPlayもGStreamerの音声出力先を指定できる。[UxPlayの出力指定](https://github.com/FDH2/UxPlay#usage)、[upmpdcli](https://www.lesbonscomptes.com/upmpdcli/)、[MPDのPipeWire出力](https://mpd.readthedocs.io/en/stable/plugins.html#pipewire)

最初に1種類の受信で音が出る構成を作り、既知のゲインやMatrixを含むプリセットでDSP適用をPCMから確認する。その後、もう1種類の受信と単一・複数出力を検証する。既定出力への追従だけに頼らず、再起動時にも処理入力へ戻る設定にする。

製品本体への変更が少なく、プロトコルやデコードの保守を既存アプリへ任せられる。反面、初期設定と曲の操作は外部アプリで行い、PipeTune GTKに統一された再生表示は出ない。UPnP/OpenHomeの再生先として選べることと、EffeTuneのエフェクト編集先として選べることも別である。後者は案Cの対象となる。

外部アプリはユーザーセッションへ接続できる形で起動する。ディストリビューション既定のシステムサービスを使う場合、実行ユーザーが異なるため接続方法を確認する。採用する版と配布環境を固定し、必要なパッケージとライセンスの扱いを記録する。例えばMPD 0.24.4にはネットワークMP3のシークに既知の問題があるため、対応版または当該機能の検証結果を採用条件に含める。[upmpdcliによる問題と修正版の案内](https://www.lesbonscomptes.com/upmpdcli/)

### 案B 受信サービスをPipeTuneへ統合する

ネットワーク受信と曲の管理を担う別プロセスを設け、音声は案Aと同じPipeWire入力へ渡す。名前を付ける場合は`pipetune-network`などとし、既存デーモンの音声コールバックへ通信処理を入れない。受信サービスが終了しても、ローカル音声のDSP処理は継続できる構成とする。

初期実装は案Aで検証したUxPlay、upmpdcli、MPDを外部プロセスとして管理する方式を推奨する。受信名、方式ごとの有効・無効、起動失敗、接続状態をCLIとGTKへ公開する。曲情報や再生操作は、採用したプログラムの公開インターフェイスで取得できる範囲から追加する。独自パッチやEffeTuneのElectron内IPCを前提にしない。

受信処理をさらに内製する場合は、UPnP/OpenHomeにohNetなどの公開APIを使い、曲のキューと再生エンジンを新設する。EffeTuneのsidecarは構成の参考にはなるが、そのままPipeTuneの安定APIとは扱わない。この方式ではHTTP/HTTPS取得、連続デコード、バッファ上限、シーク、取消、再接続までが追加範囲となる。既存の`decodeAssetAudio()`を曲全体の読込に流用しない。[ohNet](https://github.com/openhome/ohNet)、[EffeTuneの受信構成](https://github.com/Frieve-A/effetune/tree/v2.16.0/native/openhome-sidecar)

PipeTune自身が行うI/Oは既存のcardioを使う。`from_fd()`はfdの準備完了を非同期に待つ機能であり、HTTPやTLSの実装そのものではない。採用する通信・再生ライブラリは公式文書とAPIコメントを確認してから決める。fdの所有者と取消後の解放を明確にし、待機のためだけのワーカースレッドやポーリングを追加しない。[cardioの説明](https://github.com/kekyo/cardio/blob/31e8149ce52cdfdaaa48838ca9de39288074885c/README_ja.md)、[APIコメント](https://github.com/kekyo/cardio/blob/31e8149ce52cdfdaaa48838ca9de39288074885c/include/cardio.h)

受信の初期値は無効とする。音量操作は受信ストリームに適用し、デスクトップ全体やDirectの音量を変更しない。最初はステレオ受信を対象とし、DSP後のチャンネル展開には既存のプリセットと出力設定を使う。この案では設定と障害表示を統一できるが、追加プロセスの配布・更新・終了処理をPipeTune側で保守する必要がある。

### 案C EffeTuneの遠隔操作に対応する

PipeTune用のゲートウェイが、公開された`remote-v1`を受け付ける。WebSocketで受け取った操作をPipeTuneのプリセット構築・状態通知へ変換する。DSPは現在のネイティブバックエンドで実行する。EffeTuneアプリ全体やElectronの内部モジュールを組み込む前提にはしない。

現在の制御ソケットはプリセットのローカルパスを受けるため、遠隔クライアントから届くパイプラインJSONを直接渡せない。ゲートウェイが管理する領域へプリセットを保存して既存ローダーへ渡すか、検証済みのプリセット内容を受け取る入口を追加する。初期段階はアセット不要のエフェクトとbypassで、読込失敗時に以前のパイプラインを保つことまで確認する。

プリセット切替を中心にする場合は、既存の非同期準備とパイプライン差替えを再利用できる。一方、EffeTuneの画面で値を連続編集する場合は、変更規模が増える。現在の状態通知には編集用の全パラメータがなく、`DspPipeline`にも個別パラメータを更新する公開APIがない。バックエンドの`instanceSetParams`だけでは、編集中の状態管理や音声処理との同期は成立しない。変更のたびに全体を再構築する方式では、DSPの内部状態や残響、アセットの再準備への影響を検証する必要がある。根拠は`pipetune/include/pipetune/control_protocol.h`、`pipetune/include/pipetune/dsp_pipeline.h`、`pipetune/src/dsp_pipeline.cpp`、`pipetune/src/dsp_backend_loader.h`とする。

マスターバイパスの意味も合わせる必要がある。現在のPipeTuneはbypassへの切替時にパイプラインを差し替え、選択中のプリセットを解除する。EffeTuneの遠隔編集では編集対象のチェーンを保持するため、保持と復帰の設計が必要になる。GTK、プリセットの再読込、複数クライアントからの変更と再接続も、同じ状態へ反映できるようにする。CLI・GTK・サービス設定と配布への追加を含め、連続編集は独立した段階で扱う。根拠は`pipetune/src/pipewire_pipeline.cpp`と[remote-v1の状態・バイパス仕様](https://github.com/Frieve-A/effetune/blob/v2.16.0/docs/remote-v1.md)とする。

EffeTuneのエディターとの相互運用には、接続の認証、`hello/state`、操作完了の応答、対応する同期方式、エフェクトの説明情報が必要になる。`sync1`や`describe1`などは実装した機能だけを通知する。既存UnixソケットをLANへ単純に公開せず、許可した操作へ変換する。切断時に操作の適用結果が不明になる場合は、再接続後に状態を取得してから再試行する。[接続・同期・エフェクト説明の仕様](https://github.com/Frieve-A/effetune/blob/v2.16.0/docs/remote-v1.md)

ゲートウェイは初期状態で無効にし、接続用トークンの発行・取消を実装する。TLSと証明書の設定も公開仕様に合わせ、PWAから接続する場合は対象ブラウザでLAN接続条件を確認する。トークンを取り消した操作が、遅れて完了したアセット準備によって適用されないことも検証する。

IR・SFZ・Room EQを含む編集では、参照先のアセットがPipeTune側に存在する必要がある。最初は登録済みアセットに限定し、転送を追加する段階で容量、取消、登録完了前のプリセット適用を検証する。遠隔編集は音声伝送を追加しないため、再生機能には案AまたはBを併用する。

音楽一覧と選曲も統合する場合は、前述した接続方法ごとに追加範囲を決める。`music1`対応クライアントを対象とする場合は、音源を示す`serverId`と再生先を示す`playerId`、曲IDの解決、キュー、シーク、再生用音量を扱う。音楽ライブラリや再生機能がない段階では`music1`を通知しない。PWAの直接接続に必要なHTTP APIと、デスクトップ版からのOpenHome/UPnP AV操作も別々に検証する。EffeTuneから一体的に操作できる利点がある一方、他案より状態同期と相互運用の保守範囲が広い。[music1仕様](https://github.com/Frieve-A/effetune/blob/v2.16.0/docs/remote-v1.md#network-music-music1)、[PWAの通信実装](https://github.com/Frieve-A/effetune/blob/v2.16.0/js/network-music/pwa-transport.js)

### 案D PipeWireのネットワーク出力を利用する

PipeTuneの出力先として、PipeWireのRTP sinkまたはRAOP/AirPlay sinkを使う。音声経路は「デスクトップ音声 → PipeTuneのDSP → ネットワークsink → 別端末」となる。元ファイルを共有するEffeTuneの音楽共有とは用途が異なり、アプリが出す音声全体に適用できる。[PipeWire RTP sink](https://docs.pipewire.org/page_module_rtp_sink.html)、[RAOP Discover](https://docs.pipewire.org/page_module_raop_discover.html)

現在の出力列挙は非ALSAのsinkにも対応している。ただし実際のネットワークsinkで、チャンネル情報、永続的な識別、再発見後の接続が成立するかは未検証である。まず1台のネットワーク出力を検証し、その後にローカル出力との併用を検証する。RAOP sinkはAirPlay受信機へ送る機能であり、スマートフォンからPipeTuneへの受信には案AまたはBが必要となる。

ネットワークの揺らぎと受信機の時計差は、現在の出力遅延補償だけで解決するとは限らない。受信側の実PCMで遅延、継続再生中のずれ、切断・再接続後の欠落を測る。複数室での厳密な同期を求める場合は、専用の同期方式を含む別計画とする。PipeWireの公式ページは調査時点の版を示すため、PipeTuneが対象とする各ディストリビューションの版・モジュールでも成立を確認する。

## DSP更新の段階的な実施計画

実装開始時の`develop`を基準に作業ブランチを作る。各工程でビルドと対象テストを実行し、CLI、GTK、既存の音声ドライバーで成果を観測できる状態をコミットする。mainへのマージは行わない。

コードの変更は挙動を示すテストでREDを確認し、変更後のGREENを確認する。機能を削除する必要が生じた場合は、削除を確認する一時テストでRED/GREENを記録して削除をコミットする。その後、一時テストを除去して別コミットにする。仕様とテストが矛盾する場合は、どちらを直すか確認する。

### 0 現行版の基準を記録する

2.15.0のDebug全体テストを実行する。実行可能なSIMD、ビルド依存、Podman環境、メモリ・ディスクを確認する。管理者権限が必要な不足パッケージは、名前と導入方法を案内する。

ReleaseでRhythm AnalyzerとSFZ Classic/Eventの初期化時間、処理時間、最大RSS、報告遅延を記録する。Rhythmはクリック有効・無効、SFZは同じ登録音源を使い、48/96/384 kHzを基本条件とする。ブロックサイズ、PCM、処理フレーム数、CPU条件を揃え、性能測定を他のビルド・試験と重ねない。共有バックエンドのサイズと、更新後に読み直す準備済みキャッシュも保存する。

完了条件は、更新前の失敗を識別でき、同じ入力と環境で音声・負荷・メモリ・キャッシュを比較できること。基準の記録は次の実装コミットに含める。

### 1 2.16.0の最小実行を成立させる

現行版スモークを`effetune_216_test.cpp`へ移し、2.16.0の版と2種類の4 float契約を要求する。実共有バックエンドへ新しいハッシュ・配列を渡すテストを追加し、2.15.0でREDを確認する。上流の公式コミットへ参照を更新し、生成カタログとCMakeの登録を揃える。

`parameter-packer-parity.mjs`で、Rhythmの`dm`省略・`band`・`drums`・無効値と、Note Spectrogramの`ck`省略・true/false・無効値を上流JSと比較する。生成処理がそのまま対応できる場合、手書きの専用パッカーは追加しない。既存のMatrix resetとSFZ Eventの検証は残す。

CLI・GTKの版表示、Rhythmを含むプリセット、登録SFZの再生を確認する。Scalarとホストで実行可能なSIMDを使い、Rhythmの6件、SFZの13件を含む既存の参照PCMを検証する。上流のRhythm、Note Spectrogram、SFZ、モデルfixture、木モデル埋込みテストも実行する。i686・armv7lのビルドと共有ライブラリのロードはこの段階で先行確認する。

完了条件は、2.16.0を表示し、112カーネルと新しいパラメータ契約が一致して、既存の音声ドライバーで有限PCMを出力できること。既存ABIパッチ、`-ffp-contract=off`、ARM向けモデル変換、32 bit向け設定が成立し、新しい警告に対処していることも確認する。

コミットは`feat: update EffeTune integration to 2.16.0`とする。

### 2 新しいプリセットと製品の音声動作を検証する

Rhythmの`dm=band/drums`とクリック有効・無効を組み合わせる。クリック無効時は入力PCMを保ち、有効時は選択チャンネルにクリックを出すことを検証する。既存の対応レート、1/2/16チャンネル、末尾の選択ペア、部分ブロック、reset、無音からの復帰を含める。全条件の直積は作らず、通常条件と境界条件を分ける。表示方式だけでクリックの意味や製品の報告遅延が変わらないことを確かめる。

Note Spectrogramの`ck=true`を含む2.16.0プリセットを読み込み、表示専用DSPを従来どおり除外し、同じプリセットの音声処理DSPを実行できることを確認する。新しい解析モデルの推論結果は上流nativeテストで検証する。

SFZはClassic/Eventの発音、初回入力、reset、休止再開、プレビュー、dry/wetの遅延、再構築を既存の製品テストで回帰確認する。Matrixのルート保持、Processed/Directの音量、入力再開時に古い音声を出さないこと、複数出力の整列も確認する。IR・SFZ・Room EQの空キャッシュと準備済みキャッシュ、監視・取消・読込失敗時の以前の状態保持を検証する。

2.16.0の測定バックアップを使い、`harmonicAnalysis`の有無で同じ線形IRから同じ補正音声を得られることを確認する。単一・複数チャンネルの参照とファイル容量の境界も検証し、上限超過時は以前のパイプラインを保つ。容量上限の変更が必要なら、実データとメモリ予算を根拠にこの工程の方針を更新する。

同じRelease条件でRhythmとSFZの更新前後を比較する。Rhythmのドラム解析追加と、Note/SFZの共有解析変更による増加を分けて説明する。必要な処理周期を超える、または既存の音声に退行がある場合は根本原因を調べ、対応方針を計画へ反映してから修正する。外部コードの改変やDSPの無断除外では解消しない。

完了条件は、新しいパラメータを持つプリセットを受け入れ、音声・遅延・休止再開・出力経路に退行がなく、負荷とメモリの差を説明できること。

コミットは`feat: validate EffeTune 2.16 presets and runtime behavior`とする。独立した不具合があれば、この工程内で再現テストとともに`fix:`へ分ける。

### 3 全体回帰と配布を完了する

最終コードでDebug/Releaseの全体テスト、コア単独ビルド、独立アセット準備、GTK E2E、インストールを検証する。個別テストの成功を全体成功として扱わない。参照PCMと全体テストに失敗がある場合、許容誤差や時間待ちを緩める前に原因を調べる。

既存の9配布構成をビルドする。対象はDebian trixieのx86_64・i686・arm64・armv7l・riscv64と、Ubuntu 24.04/26.04のx86_64・arm64である。導入済みの実共有ライブラリで版表示、ロード、Rhythmの新パラメータ、SFZの両方式、MatrixのPCMを確認する。CPU非対応で実行できないSIMDは、ビルド成功と実行未確認を分けて記録する。

READMEと利用者向け文書は日本語原本から更新し、英語へ反映する。対応DSP版とプリセットの扱いを説明し、未採用のネットワーク案を実装済みの機能として記載しない。日本語は`nanaism/yomiyasu`で確認し、一般文書から本計画へリンクしない。

完了条件は、DSP更新の最終完了条件を満たし、最終ソースの検証ログ、性能比較、配布物のハッシュ、警告への対処が揃うこと。最後のコード変更後に全体テストを実行する。

コミットは`doc: describe EffeTune 2.16 support and verification`とする。

## 次のステップで検討するネットワーク実装の工程候補

以下は次のステップの検討材料として保存する工程候補であり、今回のDSP更新では実施しない。着手時に、再生端末として対応するクライアントとプロトコル、遠隔編集の範囲、外部プレーヤーの利用または内製を確定する。案Dによる音声送信は別の用途として扱う。決定した構成に応じて工程と完了条件を見直し、実行可能な成果物ごとに実装を進める。

### AからBへ進む場合

最初の工程ではAirPlayまたはOpenHome/DLNAの片方を接続し、CLIで起動できる構成とPCM検証を作る。ネットワークからの音声に既知のDSPを適用し、単一出力で期待したPCMが得られることを完了条件とする。接続設定と検証は`feat: verify network receiver routing through PipeTune`でコミットする。

次の工程では同じ受信方式の起動停止と設定を専用サービスへまとめ、CLIから有効・無効と状態を確認できるようにする。状態遷移とプロセス終了をテストで先に示す。再起動後も処理入力へ接続し、受信サービスの停止・異常終了がローカル音声を止めないことを完了条件とする。コミットは`feat: manage network reception as a user service`とする。

その後、もう片方の受信方式とGTK設定を追加する。音量は受信ストリームだけに作用し、Processedへ接続され、Directと物理出力の音量を変えないことを検証する。GTKを閉じても受信が続き、無効化すると接続と広告が終了することを完了条件とする。コミットは`feat: configure network receivers from GTK`とする。

最後に実機の送信端末とコントローラーで、選曲、停止、シーク、切断、再接続、スリープ復帰を検証する。デスクトップ音声との混合、レート変更、プリセット切替、長時間再生中のバッファとメモリも確認する。端末・版・音声形式を記録し、全体テスト、配布、導入手順を完成させる。対応を表明する配布構成で導入後の受信が成立することを完了条件とする。

### Cを採用する場合

最初の工程ではペアリングと状態取得、bypass、アセット不要のパイプライン適用を実行可能にする。上流2.16.0の実クライアントから操作し、PCMの変化と失敗時の以前の状態保持を確認してコミットする。

次の工程では実クライアントが必要とする同期・エフェクト説明を実装し、複数クライアントの競合と切断後の状態回復を検証する。その後、全体再構築の影響を確認して連続編集を扱い、登録済みアセットの編集、必要ならアセット転送へ進む。音楽操作は、採用する接続方法に応じて`music1`またはPWAのHTTP APIなどを別段階で追加する。各工程の完了条件は、通知した対応機能が実クライアントと製品PCMの両方で成立することとする。

この案の音声受信にはAまたはBを用いる。新しいブラウザUIを作る場合はTypeScript・Vite・Vitest・React/MUIと既定のnpm開発設定を使い、ブラウザでの試行とE2EはPlaywright MCPで行う。通信アダプターを追加するだけの段階では、新しいWeb UIを必須にしない。各工程を`feat:`でコミットし、最後に全体テストと利用者向け文書を完成させる。

### Dを採用する場合

最初の工程ではRTPなど1方式・1台のネットワークsinkを出力へ設定する。受信側PCMでDSP適用、チャンネル順、有限性を確認し、切断時に意図しない別出力へ切り替わらないことを完了条件とする。構成と検証を`feat: validate PipeWire network output`でコミットする。

次にローカル出力との併用、機器の再発見、レート切替、ネットワークの遅延・欠落を検証する。現行の識別や出力設定で不足があれば、挙動を再現するテストのREDから必要な修正だけを行う。受信側で測った遅延と時間経過によるずれを記録し、実証できた範囲で利用者へ説明できることを完了条件とする。最後のコード変更後に全体テストを実行する。

### 次のステップで採用した場合の完了条件

以下はネットワーク実装に着手する際に見直す条件であり、今回のDSP更新の完了判定には使わない。

- [ ] 再生端末、音声送信、遠隔編集の対象と対応クライアントを確定し、採用した工程を完了した。
- [ ] 実送信元または実クライアントから操作し、最終出力PCMでDSP適用を確認した。
- [ ] 切断・取消・再接続・再起動でもローカル音声と設定を保ち、音量の操作範囲が仕様どおりである。
- [ ] 通信とバッファ待機が音声コールバックを妨げず、長時間再生で資源使用量が増え続けない。
- [ ] 対応する配布構成の依存パッケージ、導入、全体テスト、相互運用結果と利用上の制約を記録した。

## 主な変更・検証箇所

| 箇所 | DSP更新での作業 |
| --- | --- |
| `deps/effetune` | 公式2.16.0コミットへ参照更新 |
| `pipetune/CMakeLists.txt`、`pipetune/cmake/EffeTuneNativeBackends.cmake` | 版取得、テスト登録、モデル・ABIパッチ・全バックエンドを確認 |
| `pipetune/tools/generate-dsp-catalog.mjs` | 生成経路が新しいパラメータを取り込むことを確認。必要な変更のみ行う |
| `pipetune/test/effetune_215_test.cpp` | `effetune_216_test.cpp`へ移し、契約を更新 |
| `pipetune/test/effetune_backend_artifact_test.cpp` | RhythmとNote Spectrogramの契約を更新し、実共有バックエンドで検証 |
| `pipetune/test/parameter-packer-parity.mjs`、`rhythm_analyzer_test.cpp`、`preset_pipeline_test.cpp` | 新パラメータの変換と製品PCM、表示専用DSPの除外を検証 |
| `pipetune/test/sfz_runtime_test.cpp`、出力・キャッシュ・ライフサイクル系テスト | 共有解析変更と既存製品経路の回帰確認 |
| `pipetune/src/measurement_store.cpp`、Room EQ・Crosstalk Cancellation関連テスト | 2.16.0の測定バックアップと容量増加を検証 |
| `README_ja.md`、`README.md`、`docs/ja/details.md`、`docs/en/details.md`、`pipetune/`内の関連文書 | 対応版と利用者への説明を更新 |

次のステップでネットワーク案を採用する場合、Aは起動・接続設定と結合テスト、Bは専用サービスと`startup_config`・CLI・GTK、Cはゲートウェイとプリセット適用・状態同期、Dは出力列挙・設定・PCM検証が主な対象となる。必要性を確認する前に既存の音声経路を抽象化し直さない。

## 検証コマンドと記録

実装時の全体検証は既存のMakefileとCMake/CTest経路を使う。GTKは従来の検証と同じく`GDK_BACKEND=x11`を指定する。

```sh
GDK_BACKEND=x11 make test TEST_BUILD_DIR=build/effetune-216-debug
cmake -S . -B build/effetune-216-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build/effetune-216-release --parallel
GDK_BACKEND=x11 ctest --test-dir build/effetune-216-release --output-on-failure
make -C pipetune/asset-preparation test BUILD_DIR=../../build/effetune-216-assets
DESTDIR="$PWD/artifacts/verification/effetune-2.16/final-install" cmake --install build/effetune-216-release --prefix /usr
./build_package_all.sh
```

個別検証の登録名は`ctest --test-dir build/effetune-216-debug -N`で確認する。サブモジュール更新後とコミット後にはCMakeを再構成し、版情報を取り直す。新たなスクリプトはNode.js、ビルド時のテキスト処理はfuncityを使う。

コマンド、コミット、RED/GREEN、PCM誤差、性能・メモリ、配布物の結果を`artifacts/verification/effetune-2.16/`へ保存する。次のステップでネットワークを検証する際は、送信側・受信側・操作アプリの版、音声形式、開始・停止条件、実際のPCMも記録する。単なる時間待ちを成功条件にせず、処理フレーム数、接続完了、プロセス終了など観測可能な条件を使う。

## 今回の最終完了条件

以下は工程0〜3で完了した更新・検証の記録である。低頻度のPCM欠落と、通常のWirePlumber経路ポリシーとの競合には未解決の部分がある。末尾の「検証で見つかった問題の追加分析・修正計画」で別の完了条件を設け、全体試験の成功だけで解決済みとは判定しない。

### DSP更新

- [x] 指定の公式2.16.0コミットを参照し、外部ソース・vendorへの直接変更がない。
- [x] CLI・GTKの版、112カーネル、RhythmとNote Spectrogramの新契約が一致する。
- [x] 新しいプリセットと公式参照PCMがScalarと実行可能なSIMDで成功する。
- [x] 表示専用DSPの除外、Rhythmのクリック、SFZ両方式、Matrix reset、休止再開、Processed/Direct、複数出力に退行がない。
- [x] アセット準備、キャッシュ、2.16.0の測定バックアップ読込に退行がなく、RhythmとSFZの性能・メモリ差を説明できる。
- [x] 最終コードでDebug/Release全体テスト、単独ビルド、GTK E2E、インストール、9配布構成を検証した。
- [x] 日本語原本と英語文書、警告への対処、工程ごとのコミット、検証記録が揃っている。

### ネットワーク課題の記録

- [x] PipeTuneを再生端末にする目的と、音源サーバー・再生端末・操作端末の役割を記録した。
- [x] HTTP/HTTPS、OpenHome/UPnP AV、`remote-v1`・`music1`、PWAのHTTP APIの用途と不足する対応を記録した。
- [x] 再生エンジン、連続デコード、バッファ、シーク、状態管理と、DSP遠隔編集の追加範囲を記録した。
- [x] 案A〜Dと工程候補を後続の比較資料として残し、ネットワーク実装と相互運用試験を今回の対象から外した。

## 計画作成時の確認結果

計画作成の完了条件は、現行コードと公式タグ差分を照合し、DSPの変更点、複数のネットワーク案、それぞれの段階的な成果物と完了条件を記載することとした。本書でこれらを満たしている。

公式タグ、2種類のパラメータ契約、112カーネル、参照PCMの差分、アセット関連の差分、ABIパッチの適用を確認した。ネットワーク案は現在のPipeWire入力、出力列挙、Unixソケット、FFmpeg構成を根拠に整理した。接続や相互運用の成功はまだ実証していない。

追加の議論を反映し、ネットワークプレーヤーは課題の記録までとし、実装を次のステップへ残した。HTTP配信、再生制御、DSP遠隔編集、PWAの直接接続を上流2.16.0の文書とコードで区別し、現在のPipeTuneで不足する機能も記載した。今回のネットワーク課題の記録条件は満たしている。

今回の変更は計画書のみで、ビルドには影響しない。製品ビルド・全体テストは実施しておらず、DSPの実装完了条件は今後の作業で判定する。ネットワーク実装の完了条件は、次のステップで採用する計画に引き継ぐ。

## 実施記録

### 工程0の基準と既存の失敗

作業ブランチは`feature/effetune-2.16.0`。更新前のEffeTune 2.15.0でDebug全252テストを実行し、251件が成功した。所要時間は1826.74秒で、コア単独ビルドとGTK E2Eも成功している。以下の記録は`artifacts/verification/effetune-2.16/`を基準とする。

失敗した`pipetune_pipewire_pipeline_tests`は、入力のない状態で`graphSampleRate == 0`を待っていた。専用のPipeWireとWirePlumberでも同じ失敗を再現した。現行の入力ノードには`node.always-process=true`が設定され、DSPの休止とグラフの停止は別の状態になる。工程2では、この既存テストをDSP休止中の設定変更という目的に合わせる。休止ポリシーを明示し、公開状態の`dspActivity`を観測する。製品の常時動作設定や待機時間は変更しない。REDの記録は`baseline/tests-debug.log`と`baseline/pipewire-isolated.log`に残した。

ホストはIntel Core i9-12900KS、論理24 CPU、メモリ62 GiB、GCC 13.3、CMake 3.28.3。Scalar、baseline SIMD、x86-64-v3を実行でき、x86-64-v4はCPU非対応だった。9配布構成のPodmanイメージと必要なビルド依存は利用できた。詳細は`baseline/environment.json`に記録した。

Releaseの測定はCPU 8に固定し、他のビルド・試験と重ねずに行った。ブロックは256フレームで、2秒分の入力を処理してから1024ブロックを測定した。Rhythmには120 BPMの合成音、SFZには8倍音を含むMIDI 83相当の入力と、48 kHz・4800フレームの登録音源を使った。各条件を3回測定した。Scalarの温キャッシュの中央値を示す。RSSはプロセス全体の最大値である。

| レート | DSP | 準備 ms | 処理 ns/frame | 最大RSS KiB | 遅延 frames |
| --- | --- | ---: | ---: | ---: | ---: |
| 48 kHz | Rhythm、クリック無効 | 3.095 | 374.797 | 23396 | 0 |
| 48 kHz | Rhythm、クリック有効 | 3.000 | 373.501 | 23552 | 0 |
| 48 kHz | SFZ Classic | 13.775 | 888.929 | 31328 | 4320 |
| 48 kHz | SFZ Event | 13.918 | 891.151 | 31336 | 11040 |
| 96 kHz | Rhythm、クリック無効 | 2.970 | 214.383 | 23660 | 0 |
| 96 kHz | Rhythm、クリック有効 | 2.950 | 215.705 | 23452 | 0 |
| 96 kHz | SFZ Classic | 20.941 | 530.046 | 32580 | 8640 |
| 96 kHz | SFZ Event | 21.222 | 531.126 | 32652 | 22080 |
| 384 kHz | Rhythm、クリック無効 | 2.995 | 74.773 | 24612 | 0 |
| 384 kHz | Rhythm、クリック有効 | 2.943 | 74.783 | 24460 | 0 |
| 384 kHz | SFZ Classic | 64.206 | 254.589 | 38816 | 34560 |
| 384 kHz | SFZ Event | 64.575 | 256.178 | 38856 | 88320 |

3バックエンドの全162測定で、PCMは有限かつ非無音だった。SFZの冷キャッシュはヒット数0、温キャッシュは1で、更新後に再読込するため保存した。生データは`baseline/performance.json`、共有バックエンドのサイズは`baseline/backend-sizes.json`、測定用ドライバーと実行条件は`dsp-profile.cpp`と`profile.mjs`に保存した。

製品側のビルドログにコンパイラ警告はなかった。外部FFmpegの別ログについては工程11で再確認した。npmは既存の開発用依存について、アセット試験で3件、GTK試験で5件の監査指摘を報告した。Vite、Vitestとその関連依存が対象で、結果は`baseline/npm-audit-assets.json`と`baseline/npm-audit-gtk.json`に保存した。指摘は更新前からある開発用依存に限られ、製品の実行時依存には含まれない。ローカルの一回限りの試験として実行し、依存更新は本DSP更新へ混在させない。esbuildとtesseractのインストールスクリプトに関する通知も確認し、ビルドと該当試験が成功した。

### 工程1の最小更新

2.16.0を要求するスモークとパッカー試験を先に追加した。2.15.0で、版表示、カタログの新契約、実共有ライブラリへの4 float設定が失敗することを確認した。Scalar、baseline SIMD、x86-64-v3で新ハッシュの拒否を観測した。記録は`baseline/contracts-red.log`と`baseline/packer-red.log`。

公式コミット`3769b9aefebf2dce2de3169690d4a95e1716da4c`へ参照を更新した。生成カタログとパッカーは既存の生成処理で対応でき、専用処理や外部コードの変更は不要だった。ホストの12対象試験が成功した（179.61秒）。新契約、Matrix reset、Rhythmの6参照PCM、SFZの13参照PCMと製品再生、Note Spectrogram、解析モデル、木モデル埋込みを含む。CLI・GTKは`EffeTune DSP 2.16.0`を表示した。記録は`step1/tests.log`、`step1/tests.xml`、`step1/versions.log`。

Debian trixieのi686とarmv7lを先行ビルドし、パッケージをコンテナーへ導入して確認した。i686はScalar・baseline・x86-64-v3、armv7lはScalar・baselineの計5バックエンドで成功した。読込先が`/usr/lib/pipetune`であること、新しいRhythmとNoteの設定、MatrixのPCM、登録SFZのClassic/Event発音を確認した。48 kHzの報告遅延はClassicが4320、Eventが11040フレームだった。

先行配布物のPipeTune版は既存の生成規則による2.15.2である。ビルド・導入ログは`step1/early32-build.log`、音声試験は`step1/i686-pcm/result.json`と`step1/armv7l-pcm/result.json`、配布物の控えは`step1/packages/`に保存した。製品側のログにコンパイラ警告はなく、ARMで表示されたGCC 7.1以降の引数ABI変更の注記は、同じコンパイラで構成したライブラリ間の不整合を示すものではなかった。既存ABIパッチと32 bit設定が成立し、工程1の完了条件を満たした。

### 工程2で判明した容量超過の扱い

Room EQは容量超過時に以前のパイプラインを保持したが、Crosstalkは同じ読込エラーを警告として扱い、自身を除外したパイプラインへ更新していた。稼働中に64 MiBを超えるバックアップへ置き換える試験を追加し、保持条件に対するREDを確認した（`step2/crosstalk-red.log`）。

容量上限は変更しない。ファイル読込層で容量超過を型として区別し、Crosstalkの候補作成を失敗させる。欠落・未割当・不適切な測定参照については、既存の除外規則を維持する。文字列でエラーを判定せず、読込前の容量検査と既存の監視・再試行を使う。Room EQとCrosstalkの保持・復旧を同じ製品経路で確認し、独立した`fix:`としてコミットする。

修正後は、Room EQとCrosstalkの容量超過・自動復旧を含む5対象試験が成功した（9.02秒）。64 MiBちょうどの読込、1 byte超過の拒否、参照PCM、既存のアセット監視・ライフサイクルも確認した。記録は`step2/size-green.log`とXML。製品コードは容量超過の分類と伝達だけを変更し、通常のDSP処理や容量上限には手を加えていない。

### 工程2のプリセット・音声・性能

Rhythmは`band/drums`のクリック有効・無効、48 kHzでの方式間PCM一致、部分ブロックとreset、各対応レートの代表値、1/2/16チャンネル、末尾ペア、無音復帰を確認した。Note Spectrogramは`ck=true`でも表示専用として除外され、同じプリセットの音声DSPが動作する。測定バックアップでは、単一・複数チャンネルの線形IRに`harmonicAnalysis`を加えても、Room EQのPCMが一致し、Crosstalkが同じ参照補正PCMを満たした。

これらとSFZ両方式、キャッシュ、ライフサイクル、複数出力のSFZ再生、入力再開を含む13対象試験が成功した（175.06秒、`step2/tests.log`とXML）。その後の容量超過修正は前節の5試験で確認した。アイドル試験の修正は専用PipeWire/WirePlumberで成功した。通常のデスクトップセッションではDSP休止を確認できなかったため、最終全体試験も専用セッションを指定する。他の製品PCM試験は、それぞれ専用の音声環境を構築する既存手順を維持する。

容量超過修正後のReleaseで、工程0と同じ162条件を再測定した。以下はScalarの温キャッシュの中央値であり、処理時間とRSSは2.15.0から2.16.0への変化を示す。単位はレートがkHz、準備がms、処理がns/frame、RSSがKiB、遅延がframesである。

| レート | DSP | 準備 | 処理 | 最大RSS | 遅延 |
| --- | --- | ---: | ---: | ---: | ---: |
| 48 | Rhythm、クリック無効 | 5.390 | 374.797 → 630.429 | 23396 → 27632 | 0 |
| 48 | Rhythm、クリック有効 | 5.454 | 373.501 → 633.660 | 23552 → 27656 | 0 |
| 48 | SFZ Classic | 13.775 | 888.929 → 857.128 | 31328 → 32152 | 4320 |
| 48 | SFZ Event | 13.775 | 891.151 → 855.975 | 31336 → 32164 | 11040 |
| 96 | Rhythm、クリック無効 | 5.547 | 214.383 → 345.305 | 23660 → 27736 | 0 |
| 96 | Rhythm、クリック有効 | 5.481 | 215.705 → 342.487 | 23452 → 27808 | 0 |
| 96 | SFZ Classic | 20.530 | 530.046 → 508.579 | 32580 → 33276 | 8640 |
| 96 | SFZ Event | 21.036 | 531.126 → 509.360 | 32652 → 33248 | 22080 |
| 384 | Rhythm、クリック無効 | 5.475 | 74.773 → 108.579 | 24612 → 28824 | 0 |
| 384 | Rhythm、クリック有効 | 5.342 | 74.783 → 107.783 | 24460 → 28716 | 0 |
| 384 | SFZ Classic | 62.350 | 254.589 → 247.980 | 38816 → 39508 | 34560 |
| 384 | SFZ Event | 62.994 | 256.178 → 249.178 | 38856 → 39548 | 88320 |

Rhythmはドラム解析の追加により、Scalarで処理が約1.4〜1.7倍、準備が約2.4 ms増えた。RSSの増加は約4 MiBで、クリック有効・無効の差は小さい。SFZは両方式とも処理時間と報告遅延に退行がなく、RSSは約0.7 MiB増えた。コード・調の検出を有効にしないSFZの経路も含め、共用解析とバックエンド更新の影響を確認できた。

全測定の平均処理時間は1コアの処理周期の9.6%以下、観測した256フレーム処理時間の最大値は周期の38.4%以下だった。これは当該ホスト・単一DSP・2チャンネルでの測定であり、任意の端末や複合プリセットの性能保証にはしない。バックエンドのReleaseサイズはScalarが4,189,056から5,547,840 bytes、baselineが4,189,600から5,552,480 bytes、v3が4,143,872から5,502,688 bytes、v4が4,274,848から5,641,864 bytesになった。生データと条件別の比較は`updated/performance.json`、`step2/performance-comparison.json`、`updated/backend-sizes.json`に保存した。

2.15.0のSFZキャッシュを48/96/384 kHz、Classic/Event、3バックエンドの18条件で再利用し、全てヒット数1、有限かつ非無音のPCMだった。アセット原本の内容・サイズ・更新時刻と、キャッシュの内容・サイズは試験前後で一致した。キャッシュの更新時刻は既存の削除順管理のために読込時に更新される。最初の検証ドライバーがその時刻まで不変と仮定して失敗したため、`asset_cache.cpp`の`touchEntry`に合わせて内容と時刻を区別した。製品コードやキャッシュ形式は変更していない。記録は`cache-reuse/unchanged-cache.json`と`step2/cache-reuse.log`。以上で工程2の完了条件を満たした。

### 工程3の実行方法

9構成すべてで既存の`scripts/build_linux_dist_container.sh`による最終版の再構成・ビルドを実施する。配布物生成、標準の導入検証、導入済みバックエンドでのPCM確認までを`package-matrix.mjs`と`package-check.mjs`で記録する。検証対象や配布内容は通常の9構成と同じである。

当初は既存のCMake・FFmpegキャッシュを再利用する手順と記述していたが、工程11で標準スクリプトが作業ディレクトリを削除することを確認した。工程3も実際にはキャッシュを再利用せず、ビルドとステージ領域を作り直していた。導入用コンテナーも毎回新しく起動している。実行方法の記録をこの事実に合わせて訂正した。

### 工程3の初回全体試験と再確認

初回検証のコードは`2d1b502c990babbc0ebc9ec69a4cb4ec775c40f8`。DebugとReleaseを再構成して全ターゲットをビルドした。製品側のログにコンパイラ警告はなく、CLI・GTKの埋込みDSP版は2.16.0だった。

初回のDebug全体試験は4並列で249/252件が成功した（773.42秒）。失敗は出力切替の`pipetune_product-output-change`、入力状態確定の`pipetune_product-output-profile-settle-input`、再接続の`pipetune_multi_device_reconnect`だった。順に、新しい機器fixtureの操作記録が空、出力変更中に入力状態が変化、再接続時の連続PCMタグが不一致という結果である。DSP参照PCM、コア単独ビルド、GTK E2Eは成功した。

同じ3件を逐次実行すると全て成功した（13.07秒）。更新前の逐次全体試験でもこの3件は成功している。Releaseの初回全体試験は2並列で250/251件が成功し（965.34秒）、出力切替で同じ機器fixtureの記録確認が失敗した。ReleaseにはDebug専用の上流メモリ確保ガード試験がないため、総数が1件少ない。初回結果は`final/tests-debug-parallel.*`、`final/tests-release-parallel.*`、3件の再確認は`final/audio-diagnostic.*`に保存した。

出力切替のテストは、機器の再起動直後に同じカード名・プロファイル名で状態を確認し、その後に新プロセスの操作記録を確認している。機器切断の通知が反映される前に以前の状態を読む可能性がある。他の2件も状態遷移とPCM到着の検証であり、並列負荷の影響は考えられるが、根本原因の確定とは扱わない。判定条件、PCM許容誤差、待機時間は緩めず、通常の逐次実行でDebugとReleaseの全件を再確認する。

Releaseの初回実行後、検証用スクリプトの一時ディレクトリ削除も失敗した。専用D-Busの終了後にGVFSのFUSEマウントだけが残っていたため、その一時マウントを解除した。検証用WirePlumberには通常のランタイムディレクトリを使い、音声接続先だけを専用PipeWireの絶対パスで指定する方式へ合わせた。製品コードの変更はない。

### 工程3での試験の同期修正

Debugの逐次再実行でも、Crosstalkの容量超過と出力切替が失敗したため、途中で終了して原因を調べた。容量超過では補正は維持されており、DSP数は2、遅延は640フレームのままだった。一方、64 MiBの置換ファイルを用意している間に直前のファイル作成に対する正常な再読込が完了し、比較元の版番号31が32へ進んでいた。版番号の差は、正常な再読込と試験の比較開始点のずれによるものだった。

置換ファイルを先に書き終え、明示的なプリセット読込の完了応答を比較元としてから、監視対象へ原子的に置き換えるようにした。版番号、プリセット、DSP数、遅延の保持条件は変更していない。診断ログは`final/pipeline-diagnostic.log`、修正後の12回連続成功は`final/pipeline-staging.log`に保存した。

出力切替では、新しい機器fixtureの開始前に、古いカードが製品の出力一覧から消えたことを待つ処理が欠けていた。プロセス終了だけでは一覧の更新は保証されず、以前の光出力プロファイルを再接続後の状態と誤認できた。同じ試験内の別の切断箇所と同様に、出力一覧からの消失を確認してから再接続する。

この修正後、出力切替は5回連続で成功した（`final/output-removal.log`）。容量超過、出力切替、入力状態確定、再接続の4件もまとめて成功した（21.56秒、`final/synchronization-green.*`）。診断中には切替前の音声が残る検出と、WirePlumberによる入力経路の再選択との競合も観測した。記録は残し、これらを無視する判定や待機時間の延長は追加していない。

試験の同期修正を`fix: synchronize live reload and output restoration tests`として分ける。製品ソースには変更がない。修正後にDebug/Releaseを再構成し、全体試験と9配布構成をあらためて完了する。

### 工程3での仮想カード試験の競合解消

同期修正後の全体試験でも、出力切替が入力設定の変化を検出した。仮想カードはプロファイル変更時に入力経路を変え、PipeTuneは元の入力設定を復元する。一方、試験環境のWirePlumber 0.4.17も中間状態の経路を自動選択・復元し、両者の操作が繰り返されていた。製品が不安定な入力状態を拒否すること自体は正しい。[WirePlumber 0.4.17の経路ポリシー](https://github.com/PipeWire/wireplumber/blob/0.4.17/src/scripts/policy-device-routes.lua)と操作記録を照合した。経路の優先度を変える試行では解消しなかったため、その変更は全て戻した。

この仮想カードを使うプロファイル試験に限り、専用WirePlumber 0.4セッションの経路自動復元を無効にする。カード側で指定する入力変更を試験条件とし、WirePlumberの実際のストリーム接続ポリシーは引き続き使う。製品の設定や通常の音声試験は変更しない。外部からプロファイル・入力設定を変更する既存試験も維持し、拒否、再試行、ロールバックを確認する。この分離は、実機でのWirePlumberとの競合を製品側で解決したことを意味しない。

入力の確定待ち試験には、別の順序問題があった。仮想カードの書込みログを見て外部変更を注入すると、PipeTuneが復元値を読み取る前に値が変わり、確定待ちに入る前のタイムアウトになり得た。試験用プロファイルの説明に入力復元の状態を含め、PipeTuneの出力一覧へ反映されたことを確認してから変更を注入する。PCM、保持条件、エラーの判定、待機時間は緩めない。説明の変化をまだ公開しないfixtureでREDを確認し、反映後は10回連続で成功した。

初回の経路分離だけで残った失敗は`final/route-isolation.*`、観測条件のRED/GREENは`final/profile-observation-red.log`と`final/profile-observation-green.log`に記録した。出力切替も5回連続で成功した（`final/output-route-isolation.log`）。これらは試験環境と同期の修正として独立したコミットにまとめ、最終全体試験へ進む。

### 工程3の最終検証条件

最終検証のコードは`509cac335aa5748f3c4f76e88d52642331ade131`。既存の版生成規則によるPipeTune版は2.15.8、埋込みEffeTune DSP版は2.16.0である。Debug/Releaseを再構成し、製品側のログにコンパイラ警告がないことを確認した。Releaseを検証用の`final-install/usr`へ導入し、CLIとGTKの両方で版表示の一致を確認した。文書の最終更新は、検証結果の記録と合わせて別のコミットにする。

全体試験は専用PipeWireセッションで逐次実行する。実時間で音声を流す試験とバックエンド比較の間は、同時進行している配布用コンテナーのコンパイルを一時停止し、終了後に再開する。試験の時間制限やPCM判定は変更しない。実行記録は`final/tests-debug.*`、`final/tests-release.*`、コンテナーの停止・再開時刻は`final/quiet-audio-tests.log`へ保存する。

初回の9配布構成は全て導入・PCM確認に成功した。その版は2.15.6で、結果と配布物を`step3-before-synchronization/`に保存した。最終コードでは9構成とも2.15.8として再生成し、同じ導入検証を実施する。ARMとRISC-VはQEMU上の確認であり、実機の処理性能を保証するものではない。

### 工程3でのSFZ試験の終了順序

上記の最終実行では、Debugは252件全て成功した（2748.63秒）。Releaseは250/251件が成功し、SFZ Eventの1件が45秒でタイムアウトした。診断用に反復すると5回目に再現し、GDBで音声fixtureが`pw_stream_destroy`内で停止していることを確認した。製品のDSPスレッドは待機状態だった。受信側はメインループの終了後にストリームを破棄する構造だった。動作中のfreewheelグラフとの終了順序が停止に関係する可能性がある。ただし、このスタックだけでは相互待機の仕組みや、停止した回のPCM検証が成功していたかまでは確定できない。記録は`final/sfz-diagnostics.json`と`final/sfz-hang-5-*.log`に保存した。[PipeWireのストリームAPI](https://pipewire.pages.freedesktop.org/pipewire/group__pw__stream.html)と[使用版1.0.5の処理通知](https://github.com/PipeWire/pipewire/blob/1.0.5/src/pipewire/stream.c)を確認した。

SFZの2ケースは、32768フレームの連続性と遅延差の検証が完了してもバッファを返し続ける。JS側が製品プロセスの正常終了を確認してから受信側へ終了を通知する。通常の製品終了と同じ5秒の制限を使い、PCMの条件と試験全体の時間制限は変更しない。変更後、Classic/EventをDebugとReleaseで各10回ずつ実行し、40実行全てが成功した（10.93秒と5.75秒）。ログは`final/sfz-shutdown-debug.*`と`final/sfz-shutdown-release.*`。この変更は試験fixtureだけに適用する。

診断用の反復実行では、終了前のPCMに256フレームの欠落を検出した回もある。これは終了時の停止とは別の現象であり、今回の順序修正で解消したとは扱わない。`final/sfz-shutdown-experiment.*`に記録し、連続性の判定は保持する。2.15.0の計画にも実時間fixtureでの低頻度の連続性失敗が記録されているが、今回と同じ原因だとは断定しない。

9配布構成は全て2.15.8として生成・導入・PCM検証を完了した。終了順序の修正は配布物の製品ソースに影響しないため、配布検証の対象コミットは`509cac3`のまま記録する。以後の試験修正との差分が試験と文書だけであることを確認する。全体試験はこの修正後にDebug/Releaseの全件を再実行する。配布コンパイルは完了しており、最終再実行では他のビルドを重ねず2並列とする。失敗時のログを残し、部分成功を全体成功へ置き換えない。

### 配布物と警告の確認

9配布構成では、導入済みの`/usr/lib/pipetune`から計25種類の共有バックエンドをロードした。全てで新しいパラメータ契約とMatrixのPCMを確認し、SFZ Classic/Eventの計50再生ケースで有限・有音のPCMと4320/11040フレームの遅延を確認した。Debian trixie、Ubuntu 24.04、Ubuntu 26.04のx86-64-v4はビルドできたが、ホストCPUが非対応のため実行対象から外した。ARMとRISC-Vの実行はQEMUによる機能確認であり、実機性能の結果には含めない。各配布物のSHA-256は`final/packages.json`、音声結果は各`final/*-pcm/result.json`に保存した。

ホストの試験修正後のコードは`6765c09`、PipeTune版は2.15.9である。配布物の`509cac3`との差分は試験fixtureと本計画書だけだった。ホストの導入確認では`DESTDIR`を指定して専用ディレクトリへ配置し、CLI・GTKともにPipeTune 2.15.9、EffeTune DSP 2.16.0の表示を確認した。配布物のPipeTune版2.15.8と区別し、製品ソースの一致を`final/after-package-changes.json`へ記録した。

ホストと9配布構成の製品側ログにはコンパイラ警告がなかった。FFmpegの別ログを含む調査範囲は工程11で訂正した。Ubuntu 24.04/26.04のARM64では、`dpkg-shlibdeps`がローダーの移設について各2件の警告を出した。`/lib/ld-linux-aarch64.so.1`にはlibc6のdiversionがあり、実体の`/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1`もlibc6の所有だった。生成された依存欄には、24.04で`libc6 (>= 2.38)`、26.04で`libc6 (>= 2.43)`が含まれる。通常の導入時依存検査、CLI・GTKの起動、共有ライブラリの音声試験はいずれも成功した。警告は抑制せず、調査結果を`final/ubuntu-arm64-diversions.json`、警告一覧を`final/build-diagnostics.json`へ残した。[dpkg-shlibdepsの依存解析](https://manpages.debian.org/trixie/dpkg-dev/dpkg-shlibdeps.1.en.html)

### 最終全体試験と完了判定

最後のコード変更`6765c09`の後に、専用PipeWireセッションでDebug/Releaseの全件を2並列で実行した。Debugは252/252件が成功し、所要時間は947.66秒。Releaseは251/251件が成功し、520.55秒だった。CTestのスキップは両方とも0件。ReleaseにはDebug専用の上流メモリ確保ガード試験がないため、総数が1件少ない。

両構成でコア単独ビルド、GTK E2E、導入レイアウト、製品の音声経路が成功した。RhythmとSFZの参照PCMは、各構成57比較で最大絶対誤差0。独立アセット準備の3件も成功した。容量超過からの復旧、出力切替、プロファイル変更、再接続、SFZ Classic/Eventを含む。全体結果を`final/tests-debug.*`と`final/tests-release.*`、独立アセット試験を`final/standalone-assets.*`に保存した。集計用スクリプトは全体の終了コード、件数、失敗・スキップ数、参照PCM、9配布物のハッシュを照合し、`final/verification-summary.json`を作成した。

診断中の低頻度なPCM欠落は、別の診断用ドライバーでも切り分けた。SFZをプリセットから除き、有効DSPがMatrixの1個、DSP遅延が0であることを確認した上で、同じfreewheelグラフと連続性判定を実行した。この構成でも1536フレームを検証した後にタグとして解釈できないPCMが入り、続くタグは2752から3009へ飛んだ。SFZを含まない構成でも発生するため、SFZの検出処理に固有の不具合とは扱わない。freewheelと非同期の試験fixtureを使う条件での未解決事象として、`final/matrix-freewheel-results.json`、`final/matrix-freewheel-1.log`と診断ソースを保存した。試験環境に原因があるという分類は暫定的で、製品の共通音声経路やPipeWire側の問題も除外できない。今回の終了順序修正で解消したとは主張せず、製品の判定条件を変更する根拠にも使わない。

日本語原本と英語の利用者向け文書を更新した。本計画書はyomiyasuの検査で指摘なし。READMEと詳細文書に残る文末などの既存指摘は、今回の追加文に由来しない。外部ソースは直接変更しておらず、EffeTuneの参照だけを指定コミットへ更新した。

以上の検証範囲で、工程0〜3とDSP更新の完了条件を照合した。ネットワークについては、今回の4つの記録条件を満たした。次のステップの実装条件は未着手のまま残す。文書の最終コミットでは製品・試験コードを変更しない。

## 検証で見つかった問題の追加分析・修正計画

2026-10-31時点の`651dbdb`を分析の基準とする。この追記では、上記の検証で見つかった問題の原因候補、調査方法、必要になった場合の修正を計画する。工程4以降の実装は未着手であり、今回の文書追記では製品・試験コードを変更しない。ネットワークプレーヤーは引き続き別のステップに残す。

### 問題の整理と計画の見直し

fixtureは、試験用の音源や仮想カードを提供するプログラムを指す。以下でいうfreewheelは実時間の経過に合わせず音声グラフを進める動作で、quantumは1回に処理するフレーム数である。フレームタグは、PCMへ埋め込んだ連続番号で欠落や重複を検出するために使う。接続の世代は、同じ名前やノードIDが再利用されても、新旧の接続を区別するための識別情報を指す。

共通する背景として、非同期処理の開始と完了を同じ出来事として扱った可能性がある。ファイルを書き終えること、fixtureが操作を記録すること、プロセスが終了することは、PipeTuneが新しい状態を観測したことまで保証しない。また、正常終了の順序を直しても、PCM検証失敗やタイムアウトから同じ順序で終了するとは限らない。

| 問題 | 確認できたこと | 未確定の点と後続工程 |
| --- | --- | --- |
| 低頻度のPCM欠落 | SFZを外したMatrix単独でも発生した。2752の次に3009が現れ、間の256フレームは設定quantumと一致する | 送受信fixtureの供給・回収、定常状態の判定、製品内のバッファと出力分配、PipeWireのどこで最初に不連続になるか。工程4・6で切り分ける |
| ストリーム破棄時の停止 | 受信fixtureが`pw_stream_destroy`内で止まった。正常系では製品を先に終了する修正後、40実行と最終全体試験が成功した | 異常系も同じ終了条件を満たすか、停止中の各ループが何を待つか。工程5で調べる |
| 状態確定前の検証 | 正常な再読込と版番号比較が重なり、カード再接続や入力復元でも古い観測値を使い得た。個別の同期は修正済み | 通知の遅延、順序の入替え、同名の再接続でも新しい状態を確認できるか。工程7で検証する |
| WirePlumberとの経路復元競合 | 仮想カードの入力変更に対し、PipeTuneとWirePlumber 0.4.17が復元操作を繰り返した | 試験の経路ポリシー無効化は条件の分離に留まる。通常のポリシーを有効にした共存条件を工程8で確認する |
| 測定ファイルの容量超過 | Crosstalkが容量超過を通常の読込警告として扱い、補正を外した候補を適用する問題を修正した | 読込中のファイル変更や取消でも、候補の拒否と稼働中の補正保持が成立するか。工程9で検証する |
| Rhythm Analyzerの負荷増加 | 同一ホストのScalar処理で約1.4〜1.7倍、RSSで約4 MiB増えた。上流には常時実行するドラム解析が追加された | 上流の追加処理とPipeTune側の余分な処理を分け、必要な対策を工程10で判断する |

これまでの計画は、DSPの契約・参照PCM・配布物を広く確認する一方、製品の状態確定と試験側の観測開始の関係、異常終了時の後始末を十分に検証していなかった。また、複数の経路管理者が動く場合と、操作主体を限定した試験を分けて評価する条件が不足していた。工程4以降では、まず状態遷移とPCMを同じ実行記録で照合できるようにする。その後、再現する失敗を対象に修正し、全体試験での一時的な成功を根拠に原因を確定しない。

### 共通の実施条件

各工程でドライバーを実行できる状態を保ち、観測記録または修正済みの挙動を成果物とする。コード修正は問題を再現するテストのRED確認後に行い、GREENを確認して工程ごとに`fix:`、`feat:`、`refactor:`、`doc:`の適切な単位でコミットする。既存の修正で満たされる条件はその結果を記録し、同じ目的の修正を重ねない。原因を特定するまでの試行が不要になった場合は戻す。

試験の進行には接続通知、処理フレーム数、状態の版番号、明示的な完了応答を使う。一定時間のsleep、成功するまでの無制限再実行、PCMの許容誤差拡大を解決方法にしない。時間制限は停止検出と後始末のために残し、失敗原因と強制終了の有無を記録する。エミュレーションで所要時間が延びる場合は、処理が進んでいる証拠を確認して制限を見直す。

リアルタイムの音声処理へ観測を加える場合は、動的メモリ確保、ファイル出力、待機するロックを持ち込まない。実行前に確保した容量固定の記録領域へ必要な値だけを保存し、既存の非リアルタイム処理から取り出す。記録領域の不足も検出し、観測が欠けた実行を原因確定に使わない。観測あり・なしの対照実行で処理への影響を確認する。新たなワーカースレッドやポーリング待機を安易に追加しない。

外部APIを使う修正では、使用する版の公式文書とヘッダーコメントを確認する。特に`PW_STREAM_FLAG_RT_PROCESS`の追加は、既存コールバックがリアルタイム処理に適するかの確認が先になる。フラグだけを変える修正は採用しない。PipeWireやEffeTuneの外部コードを直接編集せず、必要なら最小再現と版による差を記録して、公開APIで可能な対処を検討する。[PipeWireのストリームAPI](https://pipewire.pages.freedesktop.org/pipewire/group__pw__stream.html)、[1.0.5の宣言とコメント](https://github.com/PipeWire/pipewire/blob/1.0.5/src/pipewire/stream.h)

追加検証の記録は`artifacts/verification/effetune-2.16/follow-up/`へ分ける。各実行にはコミット、ビルド種別、PipeWire・WirePlumberの版、設定、入力、終了コードを添える。工程0〜3の記録は上書きしない。再現に必要なドライバーと最小設定はリポジトリへ残し、一時ログだけに依存しない形にする。

### 工程4：PCM欠落を位置づける観測と再現

最初に`final/matrix-freewheel-1.log`と保存済みの診断ソースを基に、Matrix単独の再現ドライバーを通常の試験構成から実行できるようにする。有効DSP数1、DSP遅延0を実行時に確認し、SFZを除いたつもりで別の構成を測る誤りを防ぐ。基準条件は48 kHz、256フレーム、2出力、freewheelとし、まず元の失敗条件を保つ。

送信fixture、PipeTuneの入力、DSP出力、最終受信の順に、フレームタグ、フレーム数、グラフ周期、接続世代を照合する。製品内部では`pipewire_pipeline.cpp`と`audio_bridge.cpp`の入出力を調べる。リングバッファの投入・取得数に加え、不足を数える`underrunFrames`、あふれを数える`overrunFrames`、意図した無音挿入や破棄も対応づける。バッファが空だった回数と、キューへ返した結果も送受信別に記録する。出力分配後にだけ差が出る場合は、分配の入口と各受信側の境界を追加観測する。

欠落はタグの連続性で判定し、経過時間からフレーム数を推測しない。現在のログで無効なタグが0と表示されることから、元のPCMが無音だったとは断定しない。最初に異常になった境界の前後を保存し、製品内のカウンターだけでfixtureやPipeWire内の欠落まで説明しない。

基準条件の記録が得られたら、1出力、実時間駆動、SFZ Classic/Eventの順に条件を一つずつ変える。Debug/Releaseも比較し、結果に応じて調査対象を絞る。SFZを有効にする場合、タグ検証には元の信号を保つ既存のdry経路を使い、発音するwet経路は既知の入力と期待PCMで別に検証する。DSPが意図して変えた音をタグ破損と誤認しない。

成果物は、単独で動く再現ドライバー、観測記録、原因候補ごとの確認結果である。工程4の完了条件は、正常な入力の連続性を確認でき、既知の境界で1ブロックの欠落を注入した場合にその境界を特定できることとする。元の自然発生と注入した失敗は区別する。自然発生を再現できなければ原因は未確定として工程6の調査を続ける。独立して進められる終了処理や状態同期の検証は、その間も実施できる。

### 工程5：成功・失敗の両方で終了処理を完結させる

現行のSFZ成功経路は、PCM検証完了後も受信バッファを返し、JS側の`stopRuntime`が製品の正常終了を確認してから受信ループを終了する。一方、`product_output_test.cpp`の`fail`は直ちにループを終了し、その後にストリームを破棄する。PCM不一致やタイムアウトが成功経路と同じ後始末になるかは未確認であり、ここを最初の検証対象にする。

送信開始前、再生中、PCM判定失敗、機器切断、取消、製品の早期終了の各状態で終了を発生させる。イベントで到達点を確認してから失敗を注入し、元のエラーが報告されることと、所有するプロセス・ストリームが解放されることを確認する。時間制限による終了も独立して確認する。停止時は両プロセスの状態と必要なスタックを採取し、GDBで見えた停止箇所と相互待機の原因を分けて記録する。

REDを確認した経路では、最初の失敗を保持しつつ、必要な処理ループを動かしたまま送信側を停止し、その完了後に受信側を解放する順序へ揃える。製品自体が停止した場合や起動途中にも使える終了条件を設ける。正常な停止を待てない場合の強制終了は失敗として報告し、成功扱いにはしない。

成果物は、正常終了と異常終了を単独実行できる試験と、必要な終了処理の修正である。全経路で元の成功・失敗を保って終了し、子プロセスや試験用セッションを残さないことを完了条件とする。既存の40実行成功だけで異常系を確認済みとはしない。

### 工程6：PCM欠落の原因に対応した最小修正

工程4の観測を使い、次の候補を順に判定する。終了に失敗する実行は工程5の修正を反映してから取り直し、PCM欠落と終了停止を混同しない。

| 原因候補 | 確認する事実 | 根拠が得られた場合の修正方向 |
| --- | --- | --- |
| freewheelの進行に送受信fixtureが追いつかない | 製品入力前のタグ欠落、バッファ取得失敗、送受信の回収遅れが同じ周期に起きるか | フレームやバッファの受渡しで試験を進める方式、またはリアルタイム処理に適したコールバックと制御処理の分離を比較する |
| 定常状態を早く宣言している | `captureLatency`は一度整列した組で`latencySettled`にする。経路のactive状態やserialは後で確認するため、観測開始後に接続・遅延が変わっていないか | 同じ接続世代の経路確定とPCM整列を観測開始条件にする。切替の区間と定常区間を区別する |
| 製品のリングバッファや出力分配で失われる | 入力が連続しているのに、DSP出力、取り出したPCM、各出力のいずれかで初めて欠落するか | `audio_bridge.cpp`、`pipewire_pipeline.cpp`、分配設定のうち、最初に不整合を作った箇所だけを修正する |
| PipeWire側の動作や版の差が関係する | 製品境界まで正常な入力を、最小の公開API構成でも失うか。使用版で差が出るか | 最小再現と該当する上流コードを示し、公開APIによる回避が必要か判断する。依存版の変更が必要なら影響と工程を計画へ反映する |

選んだ原因候補について、スケジュールや状態遷移を制御して同じ失敗を再現するテストを作り、修正前のREDと修正後のGREENを示す。自然発生の記録と注入条件が対応することを説明し、単に負荷を下げて通るようになったことを修正根拠にしない。固定時間の待機やバッファ増量だけで終えず、バッファの所有・回収と処理順序を確認する。

定常状態の開始条件を見直す場合も、宣言後に欠落を検出したら失敗させる。異常のたびに計測をやり直す方式にはしない。既存の32768フレームの連続性、チャンネル識別、2出力間の補償62〜63フレームを保持し、接続やレート変更に伴う意図した境界は別の試験で確認する。

成果物は原因を示す記録、回帰テスト、最小修正である。Matrix単独とSFZ Classic/Event、実時間とfreewheelで対象の再現試験が成功し、計測を外した通常の製品経路も成功することを完了条件とする。繰返し試験は補助として実行数と失敗数を残し、再現不能だけでは完了にしない。

### 工程7：試験の進行を製品の観測状態に合わせる

対象は`pipewire_pipeline_test.cpp`、`multi-device-output.mjs`、`device_presentation_fixture.c`とする。修正済みの容量超過試験では、置換ファイルの準備後に明示的な読込完了を比較元とする。再接続では古いカードが出力一覧から消えたこと、入力復元では製品の一覧へ復元後の情報が反映されたことを確認する。この3つの条件を基準に、通知の遅れと順序を制御する試験を追加する。

同名カードの再接続ではserialなどの接続世代を照合し、以前のカードの状態で次段階へ進まないことを検証する。再読込では正常な更新と失敗する更新を区別し、正常な版番号の増加を退行と誤認しない。明示的な休止状態とレート変更中の状態も、既存の公開状態から判定する。

fixtureの標準出力は操作開始の手掛かりに留め、その操作を製品が観測した条件を別に確認する。既存の状態通知で表せる範囲では、試験のためだけの製品APIを追加しない。もし公開状態と実際の動作が食い違うなら、試験側の条件緩和で吸収せず、製品側の問題としてREDから修正する。

成果物は、通知順序を変えても古い状態で成功しない回帰テストと必要な同期修正である。逐次実行と同じ条件の並列実行で成立し、待機が必要な場合はどの状態の確定を待つか説明できることを完了条件とする。

### 工程8：通常のWirePlumberポリシーとの共存を確認する

経路自動復元を無効にした現在の試験は、PipeTune単体の処理を確認する対照条件として残す。その上で、通常の経路ポリシーを有効にした専用セッションを追加する。まず競合を観測したWirePlumber 0.4.17で再現し、配布対象が使う版を調べて0.5系の設定・通知の違いも確認する。実行した版と設定を記録し、同じ設定ファイルを全版に流用できるとは仮定しない。[0.4.17の経路復元処理](https://github.com/PipeWire/wireplumber/blob/0.4.17/src/scripts/policy-device-routes.lua)

仮想カードのProfile・Route変更、WirePlumberの操作、PipeTuneの復元要求を時系列で対応づける。`pipewire_output_inventory.cpp`と`pipewire_pipeline.cpp`を対象に、入力経路、音量、mute、接続世代と要求回数を調べる。自分の要求に対する通知、プロファイル変更に伴う過渡状態、外部からの新しい操作を区別できるかを確認する。実機を使える場合は同じ条件を検証し、仮想カードだけの結果を実機での確認と混同しない。

再現結果を基に、入力復元を誰がどの段階で確定するかを整理する。既存の入力保持、取消、失敗時のロールバックを維持し、外部の新しい操作を古い保存値で上書きし続けない設計にする。両立しない要求を検出した場合は理由を示して取消またはロールバックへ進め、同じ復元要求を無制限に発行しない。期待する利用者向けの動作が既存仕様と矛盾する場合は、実装前に仕様と計画を照合する。

成果物は、通常ポリシーで競合を再現するテスト、要求と応答の記録、必要な製品修正である。通常ポリシーを有効にしたまま、出力切替と入力保持が完了し、外部操作・切断・再接続でも有限の状態遷移で確定することを完了条件とする。失敗を返すべきケースと正常に完了すべきケースを分ける。試験の分離設定を製品や利用者の全体設定へ広げることは、競合の解決に含めない。

### 工程9：容量超過の分類と候補適用の境界を確認する

修正済みの`AssetFileSizeError`、`CrosstalkMeasurements.exceedsByteBudget`、候補拒否を基準にする。64 MiBちょうど、1 byte超過、`harmonicAnalysis`の有無について既存試験を確認し、RoomとCrosstalkで必要な境界を補う。容量超過時には、稼働中のプリセット、版番号、DSP数、遅延と補正後PCMを保持し、有効なファイルへの置換後に回復することを検証する。

追加で、読込開始後のファイル増大、原子的な置換、読込取消、新しい要求による旧候補の失効を調べる。`asset_file.cpp`は読込中の増大を通常の例外として報告する経路も持つため、事前のサイズ検査で検出する超過と候補適用の結果が異ならないかを確認する。これは未確認の経路であり、不具合の存在を断定しない。非同期読込の区切りで変更を注入し、競合が偶然起こることに依存しない試験にする。

既存の欠損・不正参照を警告として扱う方針と、資源上限超過で更新を拒否する方針は区別する。REDが得られた場合だけ、エラー分類または候補の確定処理を修正する。上限の引上げや、全ての読込エラーの一律な扱い変更は行わない。

成果物は境界条件ごとの結果と、不足があれば回帰テスト・最小修正である。失敗・取消された候補の一部が公開されず、稼働中の補正保持と有効な更新への回復を確認できることを完了条件とする。

### 工程10：Rhythmの負荷増加を分けて評価する

上流2.16.0の`rhythm_analyzer/kernel.cpp`は表示方式やクリック設定にかかわらずドラム解析へ入力を渡す。この追加処理は負荷増加の説明になるが、測定された差の全てをそれだけに帰属させない。既存の2.15.0と2.16.0の条件を使い、同じ入力、レート、quantum、CPU条件で、バックエンド単体と製品経路を比較する。[Rhythmカーネル](https://github.com/Frieve-A/effetune/blob/v2.16.0/dsp/plugins/analyzer/rhythm_analyzer/kernel.cpp)

48/96/384 kHzについて、初回準備と準備後の処理を分け、処理時間の中央値・最大値、処理周期に対する比率、RSS、モデルと配布物のサイズを残す。コンパイルや他の性能試験を重ねず、測定条件を揃える。CPUやバックエンドの違いも記録し、QEMUの実行時間を実機性能の根拠にはしない。32 bitや低速端末の実機を確認できない場合は、その範囲を未確認として残す。

PipeTune側の不要なコピーや再初期化が見つかった場合は、その挙動を再現するテストから修正する。上流の処理増加で説明でき、対象の処理周期内に余裕がある場合は、根拠と測定範囲の記録を成果物として実装修正なしで完了できる。処理周期を超える対象がある場合は、対象条件と必要な対策を計画へ反映してから進める。`dm`の強制変更やクリック無効時のDSP除外で仕様を変えて負荷を隠さない。

完了条件は、負荷増加の原因と測定した範囲を説明でき、対策の要否を数値で判断できることとする。SFZも対照として確認し、共用解析の変更とRhythm固有の増分を区別する。

### 工程11：修正後の全体検証と完了判定

各工程の成果物と完了条件を照合した後、最後のコード変更に対してDebug/Releaseの全体テストを実行する。コア単独ビルド、GTK E2E、導入レイアウト、独立アセット準備、公式参照PCMを含め、追加した再現試験も全体から実行できるようにする。低頻度の事象は、元の失敗条件、制御した再現条件、通常の製品経路の結果を揃える。全体試験が成功しても、元の問題の原因と修正効果が説明できなければ未解決として残す。

製品コード、依存版、ビルド設定を変更した場合は9配布構成を再生成し、導入済みバックエンドのPCMまで確認する。試験だけの修正なら、既存の配布検証対象と製品ソースが一致することを記録する。CPU非対応のSIMD、実機未確認の構成、警告の調査結果を明記し、実行できなかった条件を成功件数に含めない。

各原因、採用した修正、戻した試行、RED/GREEN、全体検証のコードと結果を本書へ記録する。利用者に影響する動作や制約を変更した場合は、日本語原本と英語の利用者向け文書を更新する。内部の試験手順だけの変更は利用者文書へ追加しない。

### 追加工程の完了条件

- [x] PCM欠落が最初に発生する境界と原因を記録し、制御した再現試験で修正前のREDと修正後のGREENを確認した。
- [x] 正常・異常終了の各経路で元の結果を保持し、強制終了や資源の残留を成功扱いにせず後始末を完了した。
- [x] 再読込、同名機器の再接続、入力復元の試験が、製品に反映された状態と接続世代を基準に進行する。
- [x] 通常のWirePlumber経路ポリシーで入力保持と出力切替が成立し、外部操作との競合が有限の手順で完結する。
- [x] 容量超過と読込中の変更・取消で候補適用の境界を確認し、必要な修正と有効な更新への回復を検証した。
- [x] Rhythmの負荷増加を測定条件とともに説明し、対策の要否と実機未確認の範囲を記録した。
- [x] 最終コードの全体試験、必要な配布検証、警告への対処、工程ごとのコミットが揃い、未確認事項と解決済み事項を区別した。

追加計画の作成時点では、上記の条件は全て未完了だった。実施結果に応じてチェックを更新し、根拠を以下へ記録する。

## 追加工程の実施記録

### 工程4の観測ドライバーと再現

`product-matrix-freewheel`を通常のドライバーとCTestへ追加した。有効DSP数1、DSP遅延0、32768フレームの連続性と62〜63フレームの出力差を実行時に検証する。既存ドライバーでは補償値0のため失敗するREDを確認した後、Matrix単独でタグを送受信する経路を加えた。通常の成功と、従来の低頻度な欠落の両方を再現している。

`PIPETUNE_PCM_TRACE_DIRECTORY`へ保存先を指定すると、試験実行ファイルだけがPipeWireの公開ストリームAPIを観測する。プロセスIDごとのJSONには、接続ごとの名前・フラグ、フレーム数、タグの先頭・末尾、ブロック内の不連続、無効PCM、バッファ取得失敗と返却エラーを記録する。記録領域は起動前に確保し、停止後にファイルへ出力する。上限超過も明示する。動的ロードする分配モジュールも同じ公開APIで観測し、外部ソースは変更していない。[ストリームAPI](https://pipewire.pages.freedesktop.org/pipewire/group__pw__stream.html)、[dlsymによる関数の観測](https://man7.org/linux/man-pages/man3/dlsym.3.html)

観測自体の試験は、実際のPCM配列を入力し、連続した512フレーム、途中の256フレーム欠落、不正な浮動小数点、記録領域の不足を区別する。未実装時のREDと実装後のGREENを確認した。結果は`follow-up/trace-test-red.log`と`follow-up/trace-test-green.log`に保存した。Releaseの対象ビルドの製品側ログにコンパイラ警告はなかった。

初回の観測では、送信元と製品出力は連続していたが、両方の受信側で同じ256フレームを失った。分配モジュールまで観測を広げると、別の失敗では送信元が連続したタグを供給しているのに、製品入力へ256フレームの無効PCMが入り、その後の境界へ伝わった。後者は出力0で2240の後に無効PCMを検出した回である。記録は`follow-up/trace-first/`、`follow-up/trace-module/`と各`matrix-trace-*.log`に残した。記録領域の不足やバッファ返却エラーはなかった。

この2つの記録から、送信fixtureと受信fixtureの両方について、freewheelとメインループの処理通知の関係を工程6で調べる。現段階では、定常状態の判定だけが原因とは扱わない。製品入力前と製品出力後に最初の不連続が見えたため、DSP内部の追加計測は保留する。内部で初めて不連続になる記録が得られた場合に、リングバッファなどの計測を追加する。工程4の観測条件を満たしたが、PCM欠落の修正条件は未完了である。

### 工程5の異常終了手順

送信開始前の失敗を注入すると、従来のfixtureは製品停止の応答を待たずに終了した。`shutdownAcknowledged=false`を含むREDを`follow-up/shutdown-red.log`に保存した。失敗時も受信バッファを返し続け、製品の終了後に受信側へ終了を通知するようにした。最初のエラーを保持し、後続のPCM不一致やタイムアウトで上書きしない。共通の完了通知により、正常系と異常系を同じ終了順序で扱う。修正後のGREENは`follow-up/shutdown-final-green.log`に保存した。

PCM検証失敗、機器エラー、タイムアウト、取消、製品の早期終了も個別の試験として追加した。再生中の失敗は処理したフレーム数を基準に注入する。タイムアウトは再生の到達点で既存のタイマーを発火させ、その通知から後始末までを確認する。終了時にSIGKILLが必要だった場合は試験を失敗させ、もとのエラーとともに報告する。

6経路の初回実行では、送信開始前の失敗と取消が成功した。他の4経路は、予定した失敗の注入前に既知のPCM欠落を検出したため、期待するエラーとの照合で失敗した。これは終了順序の成功結果に置き換えない。記録は`follow-up/shutdown-paths.log`に残し、工程6のPCM修正後に全経路を再検証する。先に発生したPCMエラーを受け取った際も、後続の注入通知を待ち続けず、製品停止と受信側の解放へ進めるようにした。

### 工程6の原因と最小修正

PipeWire 1.0.5のストリームは、通常のprocess通知をメインループへ非同期に渡す。freewheelではメインループの処理完了を待たずに音声グラフが進むため、送信側の供給や受信側の回収が遅れると、製品境界の前後でPCMが失われる。工程4で確認した2箇所の不連続は、このバッファの受渡しと整合した。[1.0.5のprocess通知](https://github.com/PipeWire/pipewire/blob/1.0.5/src/pipewire/stream.c)

`product-matrix-freewheel-delayed-observer`では、整列後の観測を送信2048フレーム分だけ遅らせる。修正前はタグ1728の次が3777となり、2048フレームが失われた。製品のリングバッファのoverrun、underrun、処理エラーは全て0で、終了応答は受信できた。固定時間の待機を使わず、自然発生時と同じ受信回収の遅れを再現した。REDは`follow-up/observer-delay-red.log`に残した。

固定遅延のPCM試験では、送受信を音声ループで実行する。受信PCMのタグをその場で検査し、バッファを返してから容量固定のメタデータキューで制御側へ通知する。制御側で観測を遅らせても、PipeWireのバッファを保持しない。キューの不足は明示的な失敗にする。コールバック内のログ出力とエラー処理も制御側へ移し、動的確保や待機するロックを持ち込んでいない。公開APIの文書とヘッダーコメントを確認してから`RT_PROCESS`とイベント通知を使った。[ストリームの処理条件](https://pipewire.pages.freedesktop.org/pipewire/group__pw__stream.html)、[ループのイベント通知](https://pipewire.pages.freedesktop.org/pipewire/group__pw__loop.html)

製品のDSP、リングバッファ、定常状態の判定条件、PCMの許容誤差は変更していない。32768フレームの連続性、チャンネル識別、2出力間の62〜63フレーム補償を維持した。Matrixの1出力、MatrixとSFZ両方式の実時間駆動も対照試験として追加した。

Debugの19件、Releaseの14件が成功した。既存の遅延・再接続と6種類の異常終了も確認でき、工程5の終了条件を満たした。強制終了や試験用プロセスの残留はなかった。MatrixとSFZ Classic/Eventは各構成20回ずつ、合計120回で失敗0だった。計測を有効にした3件も成功し、記録領域の不足とバッファ返却エラーは0だった。バッファを全て取り出した後の空取得は正常に区別して記録した。

GREENは`follow-up/rt-fixtures-green.log`、`pcm-variants-release.log`、`pcm-debug.log`、反復結果は`pcm-repeat-debug.log`と`pcm-repeat-release.log`に保存した。観測ありの結果は`pcm-trace-fixed.log`、`trace-fixed/`、`trace-fixed-summary.json`にある。制御した再現の解消と対照条件の成功から工程6を完了した。任意の負荷や端末で欠落しないという性能保証にはしない。

### 工程7の観測状態と接続世代

入力復元試験へ通知の保留を加えた。fixtureが入力を復元した後も、Routeとプロファイル説明の更新通知を保留する。制御APIの応答で、製品が復元中かつ旧説明を保持していることを確認してから通知を解放する。続いて製品の一覧で復元後の説明を確認し、外部の入力変更を注入する。通知の保留が未実装のREDは`follow-up/notification-red.log`、実装後のGREENは`notification-green.log`に残した。製品側の試験専用APIや固定時間の待機は追加していない。

同名カードの再接続では、既存の「旧カードが一覧から消える」という条件に加え、光出力のserialが以前と異なることを確認する。再接続前の一覧の版で設定を要求すると拒否され、保存済み設定が保持されることも検証した。出力切替、入力変更、外部からのプロファイル上書き、利用不能からの復帰の4件は、逐次実行と同条件の4並列実行で全て成功した。並列結果は`follow-up/notification-parallel.log`にある。

容量超過試験は、置換ファイルの作成後に明示的な読込完了を受け取り、その版を比較元とする既存の修正を維持した。正常な再読込と拒否された候補を同じ版として比較しない。休止判定も`dspActivity`と`rateTransitioning`を使う既存条件を維持した。両者は専用音声セッションで`pipetune_pipewire_pipeline_tests`を再実行して確認した（`follow-up/state-pipeline.log`）。工程3のREDと今回の通知順序・世代の検証を併せ、試験の同期条件を満たした。

### 工程8の競合と確定手順

通常の経路ポリシーを有効にする4試験を追加した。WirePlumber 0.4.17では、出力方式を変更した後に、一時的な入力経路をポリシーが復元する現象を再現した。PipeTuneは自身の復元応答を受け取ると直ちに準備完了とし、その後のポリシーによる変更を最終確認で検出して取り消していた。製品の復元要求は既に回数制限があり、両者が無制限に要求しているという当初の推測は修正した。REDと操作記録は`follow-up/route-policy-red.log`と`route-policy-red-events.log`にある。[0.4.17の経路復元処理](https://github.com/PipeWire/wireplumber/blob/0.4.17/src/scripts/policy-device-routes.lua)

入力復元を`save=true`にする試行は、既定音量では10回成功した。しかし、音量0.37・ミュート有効の条件で同じ競合を検出したため採用しなかった。保存状態の変更だけでは、遅れて届くポリシーの操作を防げない。製品の保存指定と説明は元に戻し、`save=false`を維持した。試行記録の`route-save-repeat.json`を最終修正の成功件数には含めない。

仮想カードにも問題があった。出力方式の変更で別の入力経路へ移った際、以前の経路のsaveフラグを引き継いでいた。そのため、ポリシーへ一時的な入力状態の保存を要求してしまう。自動的に経路を変えた場合はフラグを解除し、同じプロファイルのsave指定だけを変えた場合は入力を再初期化しないようにした。実装の根拠は、saveが端子ごとに管理され、端子変更時に古い指定が解除されるACPの処理である。[PipeWire 1.0.5の端子変更](https://github.com/PipeWire/pipewire/blob/1.0.5/spa/plugins/alsa/acp/acp.c)、[Routeのsave出力](https://github.com/PipeWire/pipewire/blob/1.0.5/spa/plugins/alsa/alsa-acp-device.c)

製品では、実際に出力方式を変更した要求に確定前の確認段階を加える。再接続の自動復元で使う既存の300 msと同じ期間を設け、現在のプロファイル、出力ノードの世代と数、入力経路・音量・ミュートを再確認する。入力だけが異なる場合は最大2回まで復元を補い、各応答後に再確認する。出力側の変更、復元拒否、上限までに解消しない競合は旧構成へ戻す。時間の経過だけでは成功にせず、全ての状態が一致した場合に準備完了を返す。出力方式を変更しない要求には、この確認期間を追加しない。

非既定の入力状態は、外部クライアントによる変更がWirePlumberの保存状態と製品の一覧へ反映された後に切替試験を始める。通常の再接続は同じ入力状態を公開するカードで確認する。一方、新しいカードが初期値を公開した後にWirePlumberが異なる保存値を適用すると、自動復元は入力変更の競合として有限回で取り消した。この結果は`route-input-gain-saved-flags.log`に残した。現在の入力を保持する要求と、後から到着する異なる入力設定は区別し、後者を通常の再接続成功に数えない。外部からの入力変更とプロファイル上書きは、既存の取消・復元試験でも確認する。

通常ポリシーの上書き試験では、WirePlumber自身が最後の要求先を製品の観測前に変更する経路もあった。この場合は復元タイムアウト、観測後の上書きなら反復競合として失敗する。どちらも初回と2回の再要求で止まり、保存済み設定と入力を復元することを検証する。対照試験では反復競合の診断を引き続き要求する。記録は`route-rollback-status.log`。さらに、ロールバック後に届いた試験用シグナルが復元済み状態を再変更するREDを`route-policy-final-release.log`で確認した。操作は観測した出力の世代ごとに送り、fixture側でも対象プロファイルが有効な場合だけ適用する。ロールバック用カードの既定プロファイル優先度も、初期状態の入力専用プロファイルと一致させた。

WirePlumber 0.4.17 / PipeWire 1.0.5では通常ポリシーの4件がDebugで成功し、Releaseでは出力切替・入力競合・利用不能の3件を各5回確認した。上書き試験はシグナル修正後に通常ポリシーと対照条件で各10回成功した。0.5.8 / 1.4.2のDebian trixieコンテナでも4件が成功し、シグナル修正後の上書き試験も成功した。ログは`route-policy-final-debug.log`、`route-policy-final-058.log`、`route-signal-final*.log`。既にWirePlumberが音声ポートを接続した場合は、接続要求の終了コードだけでなく実際の接続先を確認する。実機カードでの相互運用は未確認であり、仮想カードの結果と区別する。工程8の完了条件をこの範囲で満たした。

### 工程9の読込境界と候補拒否

事前検査後にファイルが上限を超えると、末尾の追加1 byteを検出した処理が通常の変更エラーを返していた。Crosstalkではこの分類だと該当DSPを除外した候補になり、事前検査での容量超過と結果が異なる。最初の非同期読込直前に増大を注入し、容量エラーにならないREDを確認した（`follow-up/asset-growth-red.log`）。製品の読込方法や外部ライブラリは変更せず、試験実行ファイルのリンク時ラッパーでタイミングを固定した。

追加データを検出した場合は、開いているストリームのサイズを調べて`AssetFileSizeError`へ分類する。読込開始時点で上限ちょうどなら、追加1 byteの観測だけで超過が確定する。パスが小さいファイルへ原子的に置換されても、読込中の旧ファイルの超過を見落とさない。上限内の増大と置換は従来どおり変更エラーであり、欠損や不正な参照の扱いは広げていない。[GIOのストリーム情報取得](https://docs.gtk.org/gio/method.FileInputStream.query_info_async.html)、[cardioのGIOラッパー](https://github.com/kekyo/cardio/blob/main/include/cardio.h)

回帰試験は上限超過、上限内の増大、原子的な置換、旧ファイルの増大と置換の組合せ、読込中の取消、上限ちょうどからの増大を確認する。各失敗後に現在の有効ファイルを再読込できた。64 MiBを読込中に1 byte超過したCrosstalkも、容量超過と依存ファイル2件を返し、文書・スナップショットの一部をキャッシュへ公開しなかった。既存の世代失効・終了時取消の試験も成功した。

64 MiBちょうどと1 byte超過の既存境界に加え、容量超過した候補の拒否前後で、稼働中のCrosstalkのDSP数、遅延、補正後PCMが一致することを検証した。有効な測定ファイルに戻した新候補も同じPCMへ回復した。比較前に初回の係数有効化を終え、同じ入力とリセット条件で比較する。`harmonicAnalysis`の有無は既存のRoom・Crosstalk公式参照比較で扱い、最終全体試験からも実行する。稼働中のプリセットと版番号の保持、Roomの拒否・回復は専用音声セッションの`pipetune_pipewire_pipeline_tests`でも成功した。Releaseの読込・測定・ライフサイクル3件は`asset-growth-green-final.log`、稼働中の検証は`asset-growth-pipeline.xml`に記録した。工程9の完了条件を満たした。

### 工程10の性能分離

2.15.0は`13d2c50`と固定依存から隔離したソースを展開し、現在のGCCでReleaseを再構築した。再構築したScalar・baseline・v3のライブラリサイズは工程0の基準と一致した。2.16.0は`a7189f6`の製品を使った。Core i9-12900KSのCPU 8へ固定し、コンパイルと他の試験を終えてから測定した。両版とも同じ入力、2チャンネル、256フレーム、2秒分の準備処理後の1024ブロックを使い、各条件を3回実行した。

Rhythmは製品の`DspPipeline::process`と、同じバックエンドのネイティブパイプラインへ直接入力する経路を比較した。後者は製品の入出力コピーを含まず、エンジン・パラメータ・直列構成・入力は揃える。これはDSP処理の比較であり、PipeWireの転送・スケジューリング時間は含まない。クリック有無、48/96/384 kHz、Scalar・baseline・v3を確認した。対照のSFZ Classic/Eventは製品経路で初回・キャッシュ再利用を区別した。計432測定で有限かつ非無音のPCMを得て、Rhythmの製品経路と直接入力経路は同一版・同一条件のチェックサムとエネルギーが一致した。

以下はScalar、クリック無効の代表値である。処理時間は各実行のフレーム平均を3回測った中央値。全条件のブロック中央値・最大値、初回準備時間、RSSは`follow-up/performance/performance.json`と`summary.json`に保存した。

| レート | 製品経路 2.15 → 2.16（ns/frame） | 直接入力 2.15 → 2.16（ns/frame） | 製品の準備時間 2.16（ms） | 製品RSS 2.15 → 2.16（KiB） |
| --- | --- | --- | --- | --- |
| 48 kHz | 419.3 → 660.1 | 375.2 → 659.1 | 5.531 | 23,496 → 27,348 |
| 96 kHz | 214.9 → 361.3 | 213.6 → 374.3 | 5.537 | 23,608 → 27,384 |
| 384 kHz | 75.3 → 111.6 | 74.7 → 111.4 | 5.508 | 24,672 → 28,640 |

Rhythmの処理時間比は直接入力で1.47〜1.82、製品経路で1.37〜1.82だった。RSSは3,648〜4,180 KiB、準備時間は1.25〜2.79 ms増えた。一方、SFZの処理時間比は0.94〜1.04、RSS増分は192〜636 KiBで、準備時間差は−0.25〜+0.37 msだった。共通の解析やPipeTuneのコピーだけではRhythm固有の増分を説明できない。`DspPipeline::process`は版間で同じ処理であり、音声ブロックごとの再初期化もない。直接入力と製品の差には測定ばらつきがあるため、その差分をコピーの厳密な所要時間とは扱わない。

上流は表示方式とクリック設定にかかわらず`drums_->push`を呼ぶ。追加されたドラムモデルは16 bit重み566,669個、配列として1,133,338 bytesである。ヘッダーのテキストサイズ5,327,350 bytesとは区別する。既存の3つのRhythmバイナリとTCN重みのサイズは変わらない。Release共有ライブラリはScalarが4,189,056 → 5,547,840、baselineが4,189,600 → 5,552,480、v3が4,143,872 → 5,502,688 bytesになった。ソース上の追加処理と直接入力でも発生する増分から、主な原因を上流のドラム解析追加と判断する。ただし、計測だけで全増分を個別の内部関数へ帰属させてはいない。[Rhythmの処理](https://github.com/Frieve-A/effetune/blob/v2.16.0/dsp/plugins/analyzer/rhythm_analyzer/kernel.cpp)、[ドラム重み](https://github.com/Frieve-A/effetune/blob/v2.16.0/dsp/plugins/analyzer/rhythm_analyzer/drum_weights.h)

測定全条件の平均負荷は処理周期の最大9.95%、観測した最長ブロックは58.20%だった。この実機条件では処理周期を超えず、PipeTune側に不要な処理を削る根拠もないため、性能を理由とした製品変更は行わない。`dm`やクリック設定の意味も維持する。32 bit・ARM・RISC-Vの実機速度、低速CPU、多数のDSPを組み合わせたプリセットは未確認であり、QEMUの配布試験を速度保証には使わない。配布物全体のサイズとハッシュは工程11の9構成で記録する。条件・ドライバー・再構築記録は`follow-up/performance/`にまとめ、工程10の完了条件を満たした。

### 工程11で判明した対照条件の不足

最初の全体試験ではDebugの269件が成功した。その後、Releaseの`product-output-profile-restore-failure`で、期待した反復競合の診断が復元タイムアウトになった。製品は要求回数の上限でロールバックしていたが、試験が3回目の対象プロファイルを観測できていなかった。REDと操作記録は`follow-up/before-profile-control/tests-release.log`に保存した。

工程8の対照条件は経路の自動復元を止めていたが、別に動く既定プロファイルの選択を残していた。この選択が試験の操作に割り込み、通常ポリシー試験と同じ競合を起こしていた。経路ポリシーの分離だけで操作元を限定できるという計画上の前提が不足していた。[0.4.17のプロファイル選択](https://github.com/PipeWire/wireplumber/blob/0.4.17/src/scripts/policy-device-profile.lua)、[0.5.8のプロファイル選択](https://github.com/PipeWire/wireplumber/blob/0.5.8/src/scripts/device/select-profile.lua)

この対照試験の専用セッションに限り、既定プロファイルの選択も無効にした。WirePlumberの版ごとに対応するスクリプトを試験用設定で置き換える。通常ポリシー試験では経路とプロファイルの両方を有効なまま残した。製品コードや利用者の設定は変更せず、対照試験が反復競合の診断を要求する条件も維持する。工程8の世代確認と、遅れた操作がロールバック後に状態を変えないための修正は、通常ポリシー試験でも必要なため残した。

修正後はWirePlumber 0.4.17のRelease対照試験が12回、Debugの対照・通常ポリシーが各3回成功した。0.5.8でも両条件が各5回成功した。GREENは`profile-control-green.log`、`profile-control-debug.log`、`profile-control-058.log`にある。この修正を含む最終コードで全体試験を再実行した。

### 工程11の全体検証

初回Releaseでは、コア単独ビルド試験の版表示確認も失敗し、最終結果は266/268件成功だった。検証中に試験コードを修正・コミットしたことでGitの状態が変わり、自動採番がビルド時の2.15.17と一致しなくなった。未コミットの変更も採番に影響する。これは検証手順の不備であり、DSPや単独ビルドの障害とは区別する。初回結果は`before-profile-control/`、版表示を揃える前に中止した再実行は`version-mismatch-restart/`へ保存した。

最終コードを`9642cc5`へ固定し、作業ツリーに変更がない状態でDebug・Releaseを再構成してビルドした。CLIとGTKの版表示は2.15.18、EffeTune DSPは2.16.0となった。全体試験の終了まではコードと文書を変更せず、各ビルドの試験を2並列で実行した。音声接続先には専用のPipeWireとWirePlumberを使い、CPUは0〜7へ限定した。初回の逐次実行と所要時間を性能比較には使わない。

| 検証 | 成功件数 | 失敗・スキップ | 所要時間（秒） |
| --- | ---: | ---: | ---: |
| Debug全体 | 269/269 | 0・0 | 1082.35 |
| Release全体 | 268/268 | 0・0 | 593.95 |
| アセット準備の独立構成 | 3/3 | 0・0 | 0.04 |

両方の全体試験で、コア単独ビルド、GTK E2E、導入レイアウト、追加したPCM・終了処理・状態同期・容量境界の試験が成功した。RhythmとSFZの公式参照PCMは各ビルド57比較で、全て最大絶対誤差0だった。Debugだけに含まれる上流のメモリ確保ガードも成功した。Releaseとの件数差はこの1件である。記録は`tests-debug.*`、`tests-release.*`、`standalone-assets.*`と`test-exits.json`にある。

配布検証は`de69f53`の製品コードと採番2.15.17を使う。`9642cc5`までの変更は対照試験と本書だけであり、製品ソースとEffeTuneの参照は一致する。Gitの状態が異なるため、ホスト検証物とは自動生成したPipeTuneの版表示が異なる。この対応を`product-source-match.json`に保存した。ステージ領域への導入確認は`follow-up/install/usr`で実施し、CLI・GTKの両方がEffeTune DSP 2.16.0を表示した。

追加工程の対応コミットは次のとおり。

| 工程 | コミット | 成果物 |
| --- | --- | --- |
| 4 | `8480c3a` | PCMの境界観測と再現ドライバー |
| 5 | `b56525f` | 失敗時もバッファを返して終了するfixture |
| 6 | `aa4606f` | 音声処理スレッドでの送受信と通知処理の分離 |
| 7 | `e23a718` | 入力復元の観測状態と接続世代による試験の進行 |
| 8 | `6c19005` | 通常ポリシーとの競合を確認してから出力設定を確定 |
| 9 | `a7189f6` | 読込中の容量超過も候補全体を拒否 |
| 10 | `de69f53` | バックエンドと製品経路の性能比較 |
| 11 | `9642cc5` | 対照試験で既定プロファイルの選択を分離 |

### 工程11の外部ビルド警告

FFmpegのコンパイルはCMakeが別ログへ保存するため、製品側のログだけでは警告を集計できていなかった。工程0〜3の「警告なし」という記述の範囲を訂正し、外部ビルドの標準エラーも収集対象へ追加した。`external-build-diagnostics.json`へ分類を保存し、元のメッセージと前後の注記を`ffmpeg-build-logs/`へ複写した。警告を抑制するフラグや外部コードの修正は追加していない。

固定依存のFFmpegは`n6.1.6`で、参照コミットとビルド設定はEffeTune更新前から変わっていない。工程10で再構築した2.15.0側のFFmpegログにも、未使用関数、配列境界、書式の切詰めなどの警告があった。上流の該当ソース9ファイルを取得し、ローカルの固定依存と内容が一致することを確認した。出自とハッシュは`ffmpeg-warning-sources.json`にある。

| 警告 | コードから確認した背景と扱い |
| --- | --- |
| 未使用関数・到達不能なswitch内の文 | 音声デコードだけを選んだ構成でも、動画・エンコード用の共用ソースがコンパイルされる。MOVの動画用caseは条件付きで除かれ、本文だけが残る。選択した機能を増やして警告を消す変更は行わない。[MOVの条件付き処理](https://github.com/FFmpeg/FFmpeg/blob/n6.1.6/libavformat/mov.c#L4767) |
| C90の宣言位置 | ビルドはC11だが、上流の`-Wdeclaration-after-statement`が途中の変数宣言を報告する。使用言語規格への違反ではない。[該当箇所](https://github.com/FFmpeg/FFmpeg/blob/n6.1.6/libavutil/eval.c#L522) |
| DCT引数の配列長 | 宣言はポインター、定義は64要素配列の型別名になっている。Cの関数引数としては同じポインター型へ変換される記述の差であり、今回のABI変更ではない。[DCT定義](https://github.com/FFmpeg/FFmpeg/blob/n6.1.6/libavcodec/jrevdct.c#L215) |
| 書式の切詰め | Vorbisの章番号・時刻は書込み側で、今回のデコード構成では使わない。フレームスレッドの名前も`snprintf`の容量内へ収める処理である。警告はバッファ超過の実行結果ではなく、表示文字列の切詰め可能性を示す。[Vorbisの書込み](https://github.com/FFmpeg/FFmpeg/blob/n6.1.6/libavformat/vorbiscomment.c#L92)、[スレッド名](https://github.com/FFmpeg/FFmpeg/blob/n6.1.6/libavcodec/pthread_frame.c#L170) |
| AAC SBRの負添字 | 直前に2 bitで読んだ2値の和に1を足し、5を超えれば戻る。警告対象のループでは書込み添字が1〜4、読込み添字が2〜5の範囲で、8要素配列に対する負添字へ到達しない。この警告箇所の範囲解析と、AAC全体の安全性保証は区別する。[SBRの上限検査とループ](https://github.com/FFmpeg/FFmpeg/blob/n6.1.6/libavcodec/aacsbr_template.c#L718) |
| VLCテーブルの配列境界 | 16 bitの値を詰める場合は再帰の上限が3要素となり、深さは0〜2、6 byteの値配列へ書き込む最大添字は5である。警告が示すオフセット8・10には、この再帰条件では到達しない。[再帰の条件](https://github.com/FFmpeg/FFmpeg/blob/n6.1.6/libavcodec/vlc.c#L353) |
| `memset`の長さ | リセット時は有効な子変換の数を0にし、関数配列の先頭1要素だけを消す。残りは次の初期化時に上書きする旨のコメントがあり、実際の初期化も有効数の位置へ関数を設定する。[リセットと再初期化](https://github.com/FFmpeg/FFmpeg/blob/n6.1.6/libavutil/tx.c#L279) |

GNU Makeの並列実行枠を管理するjobserverについても警告が出た。外部ビルドへ明示した`-j4`で枠を作り直す通知と、枠を引き継げない子処理が1並列へ戻る通知である。ビルドの速度・並列数に関する結果として記録し、音声処理の不具合とは扱わない。比較用の旧版ログにも同じ種類の通知がある。外部ビルドを含む並列数の管理を変更する場合は、別のビルド改善として検証する。[GNU Makeの並列実行枠](https://www.gnu.org/software/make/manual/html_node/Job-Slots.html)

i386では上流x86アセンブリ由来のTEXTRELが残る。TEXTRELは、ロード時に実行コード領域へアドレスを書き込む再配置を表す。更新前の2.15.0パッケージと今回の配布物を`readelf`で調べ、`libavcodec`・`libavutil`・`libswresample`の3ライブラリに同じ属性を確認した。両版の比較は`ffmpeg-i386-textrel.json`に保存した。今回の導入・PCM検証は成功したが、この属性を禁止する実行環境への対応を確認した結果ではない。解消にはFFmpeg側のアセンブリ構成や依存版の検討が必要なため、EffeTune更新へ混在させず、既存の配布制約として残す。[上流のx86アセンブリ](https://github.com/FFmpeg/FFmpeg/blob/n6.1.6/libavutil/x86/float_dsp.asm)

### 工程11の配布検証と完了判定

標準の配布スクリプトで9構成をクリーンビルドし、新しいコンテナーへの導入と、導入済みバックエンドのPCM検証まで全て成功した。配布物のPipeTune版は2.15.17、CLI・GTKが示すEffeTune DSP版は2.16.0である。配布物は`artifacts/deb/`、構成別の結果・所要時間・SHA-256は`follow-up/packages.json`に保存した。最後に9ファイルのハッシュを再計算し、記録と一致した。

| 配布構成 | 更新前の配布物（bytes） | 今回の配布物（bytes） | 導入後SFZの検証数 |
| --- | ---: | ---: | ---: |
| Debian trixie / i386 | 17,220,354 | 18,543,678 | 6 |
| Debian trixie / amd64 | 17,785,172 | 19,252,732 | 6 |
| Debian trixie / armhf | 15,186,580 | 16,427,862 | 4 |
| Debian trixie / arm64 | 15,292,328 | 16,792,422 | 6 |
| Debian trixie / riscv64 | 15,773,456 | 17,017,302 | 4 |
| Ubuntu 24.04 / amd64 | 17,799,884 | 19,234,854 | 6 |
| Ubuntu 24.04 / arm64 | 15,310,160 | 16,872,904 | 6 |
| Ubuntu 26.04 / amd64 | 17,943,322 | 19,470,218 | 6 |
| Ubuntu 26.04 / arm64 | 15,436,176 | 16,989,510 | 6 |

更新前はEffeTune 2.15.0を使う配布物であり、圧縮済みの配布物全体では7.7〜10.2%増えた。モデル配列や非圧縮の共有ライブラリ単体の増分とは区別する。旧・新のサイズとハッシュ、今回生成した依存欄は`package-comparison.json`にある。

x86ではScalar・baseline・v3、ARM64ではScalar・baseline・SVE、armhfとRISC-VではScalar・baselineを確認した。導入後SFZの50条件は全て有限かつ非無音で、48 kHzでの遅延はClassicが4320、Eventが11040フレームだった。読込先は`/usr/lib/pipetune/`配下であることを確認した。amd64の3構成に含まれるv4はホストCPU非対応のため実行せず、成功件数に含めていない。ARM・RISC-VはQEMUによる機能検証であり、実機性能の測定ではない。

製品側のビルドログにコンパイラ警告はなかった。ARMでの引数ABI変更の注記はGCC 7.1の変更に由来し、同一のコンパイラで構成したライブラリと導入後のPCMを確認した。FFmpegの別ログの警告は前節のとおり分類し、既存の制約として扱う。[GCC 7のARM ABI変更](https://gcc.gnu.org/gcc-7/changes.html)

Ubuntu ARM64では`dpkg-shlibdeps`の移設警告が各2件、計4件あった。ローダーの移設元・移設先がともにlibc6に属することを再確認した。依存欄には24.04で`libc6 (>= 2.38)`、26.04で`libc6 (>= 2.43)`が含まれ、通常の導入検査と実行も成功した。警告は抑制せず、`build-diagnostics.json`と`ubuntu-arm64-diversions.json`に残した。[依存解析の仕様](https://manpages.debian.org/trixie/dpkg-dev/dpkg-shlibdeps.1.en.html)

全体試験・独立構成・9配布物・参照PCM・導入後PCMをまとめた検証は、`summarize-final.mjs --complete`で成功した。集計は`verification-summary.json`にあり、全体試験の後に試験用音声プロセスと専用セッションの一時ディレクトリが残っていないことも`host-test-cleanup.json`へ記録した。以上で追加工程4〜11の完了条件を満たした。

実機音声カードでの相互運用、低速CPU・ARM・RISC-Vの実機性能、多数のDSPを組み合わせた負荷は未確認として残す。今回修正した問題と、FFmpegの既存制約は区別する。ネットワークプレーヤーは合意どおり検討事項の記録に留め、実装は次の工程とする。
