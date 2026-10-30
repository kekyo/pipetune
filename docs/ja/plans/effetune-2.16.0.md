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

### DSP更新

- [ ] 指定の公式2.16.0コミットを参照し、外部ソース・vendorへの直接変更がない。
- [ ] CLI・GTKの版、112カーネル、RhythmとNote Spectrogramの新契約が一致する。
- [ ] 新しいプリセットと公式参照PCMがScalarと実行可能なSIMDで成功する。
- [ ] 表示専用DSPの除外、Rhythmのクリック、SFZ両方式、Matrix reset、休止再開、Processed/Direct、複数出力に退行がない。
- [ ] アセット準備、キャッシュ、2.16.0の測定バックアップ読込に退行がなく、RhythmとSFZの性能・メモリ差を説明できる。
- [ ] 最終コードでDebug/Release全体テスト、単独ビルド、GTK E2E、インストール、9配布構成を検証した。
- [ ] 日本語原本と英語文書、警告への対処、工程ごとのコミット、検証記録が揃っている。

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

ビルドにコンパイラ警告はなかった。npmは既存の開発用依存について、アセット試験で3件、GTK試験で5件の監査指摘を報告した。Vite、Vitestとその関連依存が対象で、結果は`baseline/npm-audit-assets.json`と`baseline/npm-audit-gtk.json`に保存した。指摘は更新前からある開発用依存に限られ、製品の実行時依存には含まれない。ローカルの一回限りの試験として実行し、依存更新は本DSP更新へ混在させない。esbuildとtesseractのインストールスクリプトに関する通知も確認し、ビルドと該当試験が成功した。

### 工程1の最小更新

2.16.0を要求するスモークとパッカー試験を先に追加した。2.15.0で、版表示、カタログの新契約、実共有ライブラリへの4 float設定が失敗することを確認した。Scalar、baseline SIMD、x86-64-v3で新ハッシュの拒否を観測した。記録は`baseline/contracts-red.log`と`baseline/packer-red.log`。

公式コミット`3769b9aefebf2dce2de3169690d4a95e1716da4c`へ参照を更新した。生成カタログとパッカーは既存の生成処理で対応でき、専用処理や外部コードの変更は不要だった。ホストの12対象試験が成功した（179.61秒）。新契約、Matrix reset、Rhythmの6参照PCM、SFZの13参照PCMと製品再生、Note Spectrogram、解析モデル、木モデル埋込みを含む。CLI・GTKは`EffeTune DSP 2.16.0`を表示した。記録は`step1/tests.log`、`step1/tests.xml`、`step1/versions.log`。

Debian trixieのi686とarmv7lを先行ビルドし、パッケージをコンテナーへ導入して確認した。i686はScalar・baseline・x86-64-v3、armv7lはScalar・baselineの計5バックエンドで成功した。読込先が`/usr/lib/pipetune`であること、新しいRhythmとNoteの設定、MatrixのPCM、登録SFZのClassic/Event発音を確認した。48 kHzの報告遅延はClassicが4320、Eventが11040フレームだった。

先行配布物のPipeTune版は既存の生成規則による2.15.2である。ビルド・導入ログは`step1/early32-build.log`、音声試験は`step1/i686-pcm/result.json`と`step1/armv7l-pcm/result.json`、配布物の控えは`step1/packages/`に保存した。コンパイラ警告はなく、ARMで表示されたGCC 7.1以降の引数ABI変更の注記は、同じコンパイラで構成したライブラリ間の不整合を示すものではなかった。既存ABIパッチと32 bit設定が成立し、工程1の完了条件を満たした。

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

最終配布確認では、各構成の既存CMake・FFmpegビルドキャッシュを再利用する。9構成すべてで既存の`scripts/build_linux_dist_container.sh`による最終版の再構成・ビルドを実施し、ステージ領域と導入用コンテナーは作り直す。配布物生成、標準の導入検証、導入済みバックエンドでのPCM確認までを`package-matrix.mjs`と`package-check.mjs`で記録する。検証対象や配布内容は通常の9構成と同じであり、未変更のコンパイル結果を再利用して検証時間を短縮する。

### 工程3の初回全体試験と再確認

初回検証のコードは`2d1b502c990babbc0ebc9ec69a4cb4ec775c40f8`。DebugとReleaseを再構成して全ターゲットをビルドした。コンパイラ警告はなく、CLI・GTKの埋込みDSP版は2.16.0だった。

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

最終検証のコードは`509cac335aa5748f3c4f76e88d52642331ade131`。既存の版生成規則によるPipeTune版は2.15.8、埋込みEffeTune DSP版は2.16.0である。Debug/Releaseを再構成し、コンパイラ警告なしでビルドした。Releaseを検証用の`final-install/usr`へ導入し、CLIとGTKの両方で版表示の一致を確認した。文書の最終更新は、検証結果の記録と合わせて別のコミットにする。

全体試験は専用PipeWireセッションで逐次実行する。実時間で音声を流す試験とバックエンド比較の間は、同時進行している配布用コンテナーのコンパイルを一時停止し、終了後に再開する。試験の時間制限やPCM判定は変更しない。実行記録は`final/tests-debug.*`、`final/tests-release.*`、コンテナーの停止・再開時刻は`final/quiet-audio-tests.log`へ保存する。

初回の9配布構成は全て導入・PCM確認に成功した。その版は2.15.6で、結果と配布物を`step3-before-synchronization/`に保存した。最終コードでは9構成とも2.15.8として再生成し、同じ導入検証を実施する。ARMとRISC-VはQEMU上の確認であり、実機の処理性能を保証するものではない。

### 工程3でのSFZ試験の終了順序

上記の最終実行では、Debugは252件全て成功した（2748.63秒）。Releaseは250/251件が成功し、SFZ Eventの1件が45秒でタイムアウトした。診断用に反復すると5回目に再現し、GDBで音声fixtureが`pw_stream_destroy`内で停止していることを確認した。製品のDSPスレッドは待機状態だった。PCM検証を終えた受信側が先にメインループを終了し、動作中のfreewheelグラフのストリームを破棄していた。記録は`final/sfz-diagnostics.json`と`final/sfz-hang-5-*.log`に保存した。[PipeWireのストリームAPI](https://pipewire.pages.freedesktop.org/pipewire/group__pw__stream.html)と[使用版1.0.5の処理通知](https://github.com/PipeWire/pipewire/blob/1.0.5/src/pipewire/stream.c)を確認した。

SFZの2ケースは、32768フレームの連続性と遅延差の検証が完了してもバッファを返し続ける。JS側が製品プロセスの正常終了を確認してから受信側へ終了を通知する。通常の製品終了と同じ5秒の制限を使い、PCMの条件と試験全体の時間制限は変更しない。変更後、Classic/EventをDebugとReleaseで各10回ずつ実行し、40実行全てが成功した（10.93秒と5.75秒）。ログは`final/sfz-shutdown-debug.*`と`final/sfz-shutdown-release.*`。この変更は試験fixtureだけに適用する。

診断用の反復実行では、終了前のPCMに256フレームの欠落を検出した回もある。これは終了時の停止とは別の現象であり、今回の順序修正で解消したとは扱わない。`final/sfz-shutdown-experiment.*`に記録し、連続性の判定は保持する。2.15.0の計画にも実時間fixtureでの低頻度の連続性失敗が記録されているが、今回と同じ原因だとは断定しない。

9配布構成は全て2.15.8として生成・導入・PCM検証を完了した。終了順序の修正は配布物の製品ソースに影響しないため、配布検証の対象コミットは`509cac3`のまま記録する。以後の試験修正との差分が試験と文書だけであることを確認する。全体試験はこの修正後にDebug/Releaseの全件を再実行する。配布コンパイルは完了しており、最終再実行では他のビルドを重ねず2並列とする。失敗時のログを残し、部分成功を全体成功へ置き換えない。
