# plan3 リモートからのDSP編集とネットワーク再生

PipeTuneは常にシンク、つまり音声を受けて処理し、スピーカーへ出力する側として働く。選曲、再生、DSP編集の指示は手元の端末からPipeTuneを動かすPCに届く。手元でEQを調整すると、そのPCで再生中の曲と、同じDSPを通るデスクトップ音声に変更が反映される。

2026-11-20時点の採用案はコントローラー案2とプレーヤー案1である。初版はEffeTuneのリモートエディターから編集を受け付ける。再生には利用者が同じPCで起動する2つのプログラムを使う。upmpdcliがネットワークから再生指示を受け、Music Player Daemon（MPD）が曲を再生する。後続のプレーヤー案2で、この2プロセスの起動と状態表示をPipeTuneの管理に移せる構成とする。調査基準は`develop`の`84a2ac0`。通信の互換対象はEffeTune 2.16.0、PipeTuneが使うDSPは2.18.1である。[2.16.0対応時の調査](effetune-2.16.0.md#ネットワークプレイの調査記録)

## 操作と音声の経路

| 用途 | 経路 | 誰が状態を持つか |
| --- | --- | --- |
| DSPを編集する | 手元のEffeTuneエディター → 認証付きWebSocket（`remote-v1`）→ PipeTune | PipeTuneデーモンが編集中のエフェクト列とバイパス状態を持つ。接続中のエディターに適用結果を返す |
| 曲を選んで再生する | 手元のEffeTuneなど → OpenHomeまたはUPnP AVの再生指示 → upmpdcli → 同じPCのMPD | OpenHomeのキューは再生端末に置く。UPnP AVでEffeTuneが曲順を管理する場合は、操作側が次曲を指示する |
| 音を鳴らす | 音源サーバー → HTTP(S) → MPDでデコード → PipeWireの処理入力 → PipeTuneのDSP → 設定済み出力 | 音源サーバーが元のファイルを配信し、MPDが再生位置を持つ。PipeTuneはデスクトップに適用中のプリセットで処理する |

例えば、スマートフォンでNAS上のFLACを選ぶと、再生端末のMPDがNASからその曲を取得する。スマートフォンを切断した後もOpenHomeのキューで次曲へ進める。音源は引き続きNASから取得する。DSPへのペアリングと再生機のLAN公開は個別に有効化できる。[EffeTuneのネットワーク音楽ガイド](https://github.com/Frieve-A/effetune/blob/v2.16.0/docs/i18n/ja/openhome.md)・[upmpdcliの受信構成](https://www.lesbonscomptes.com/upmpdcli/pages/upmpdcli-manual.html)

## コントローラー案2 EffeTuneからPipeTuneのDSPを編集する

利用者はPipeTuneで遠隔編集を有効にし、ペアリングリンクを取得する。そのリンクをスマートフォンのブラウザーで開くか、EffeTuneデスクトップ版の「Join another EffeTune」に渡す。どちらもリンク先の画面を開くため、PipeTuneはWebSocketと同じ接続先から上流の`remote.html`と必要な静的ファイルを配信する。配布には既存のEffeTuneサブモジュールの資材を使い、2.16.0のエディターでも接続を確かめる。[ブラウザー版の仕様](https://github.com/Frieve-A/effetune/blob/v2.16.0/docs/remote-v1.md#browser-clients)・[デスクトップ版の接続処理](https://github.com/Frieve-A/effetune/blob/v2.16.0/electron/remote-client-window.cjs)

2.16.0のデスクトップ版では、再生機一覧に表示する条件と、その画面にエフェクト編集ボタンを表示する条件が異なる。後者にはメーカー名による判定がある。PipeTuneは自身の名前で再生機として公開し、編集にはペアリングリンクを使う。PWAの「端末を追加」からの直接登録は別のHTTP APIを使うため、初版の相互運用試験ではリンクから開くエディターを使う。[機器の判定](https://github.com/Frieve-A/effetune/blob/v2.16.0/electron/network-music-host.cjs#L85-L89)・[編集画面の表示条件](https://github.com/Frieve-A/effetune/blob/v2.16.0/js/network-music/view.js#L213-L215)・[PWAの接続方式](https://github.com/Frieve-A/effetune/blob/v2.16.0/js/network-music/pwa-transport.js)

### 接続と編集状態

LAN用の通信処理はデーモンの制御側に置く。待ち受けは初期状態で無効にし、CLIから有効化、リンクの取得、トークンの更新、無効化ができるようにする。ブラウザーの接続元とトークンを検証し、無効化・トークン更新時には既存の接続も閉じる。信頼できるLANでのHTTP/WSと、証明書を指定したHTTPS/WSSを扱う。既存のUnixソケットは同じユーザーのCLIとGTKが使う。各入口から届いた編集をデーモン内の同じ適用処理へ渡し、ネットワークI/Oは音声コールバックの外で行う。[remote-v1の接続仕様](https://github.com/Frieve-A/effetune/blob/v2.16.0/docs/remote-v1.md)・[ローカル制御ソケット](../../../pipetune/src/control_socket.cpp)

現行の`loadPreset`はローカルパスを受け取る。DSPローダーは短縮形式のエフェクト列も解釈できるが、現行の状態通知で得られるのはエフェクト名、適用状態、診断が中心である。編集用には実際に適用した順序、パラメーター、バスとチャンネルの割り当てを保持する。受信したJSONはメモリ上で検証して既存の構築処理へ渡す。ライブ編集のたびに元のプリセットファイルへ書き戻すと、ファイル監視による再読込と競合するため、ライブ編集はデーモン内で扱う。[制御コマンド](../../../pipetune/include/pipetune/control_protocol.h)・[プリセットの読込と再構築](../../../pipetune/src/dsp_pipeline.cpp)・[プリセットの状態表示](../../../pipetune/include/pipetune/preset_entry.h)

初版は`hello`で対応するエフェクトと機能を通知し、`get`で現在のチェーンとマスターバイパス状態を返す。`chain`はチェーン全体、`params`は指定段のパラメーターを変更する。`bypass`の解除時は、バイパス中の編集も含めて元のチェーンで処理を再開する。現行のローカル操作はバイパス時に選択中のプリセットを解除するので、プリセット選択の解除と遠隔のマスターバイパスを区別する。[上流の要求と応答](https://github.com/Frieve-A/effetune/blob/v2.16.0/docs/remote-v1.md#requests-client--app)・[現行のバイパスとプリセット切替](../../../pipetune/src/pipewire_pipeline.cpp)

プリセットの選択には、現在GTKが使っているカタログの読込処理を共用する。デーモン単体で`listPresets`には名前の一覧、`getPreset`には短縮形式のチェーンを返し、エディターから続いて届く`chain`で適用する。[PipeTuneのプリセット一覧](../../../pipetune-gtk/src/preset-catalog.h)・[上流エディターのプリセット操作](https://github.com/Frieve-A/effetune/blob/v2.16.0/js/remote/remote-client.js#L55-L90)

### 再生中の連続編集と状態の同期

上流のエディターは、同期拡張`sync1`を通知しない相手には、つまみ一つの変更でも`chain`を送る。そこで、個別の`params`と、全体を受け取っても構成が一致し値だけが変わった`chain`は、同じ差分更新で扱う。制御側で値を検証し、動作中のDSPに安全に反映できる更新を音声ブロック境界で適用する。例えば残響の後段に置いたVolumeの変更なら、残響の履歴を保ったままゲインが変わる。段の追加、バス構成の変更、IRの再準備などは候補を制御側で組み立て、成功してから既存の差し替え処理に渡す。[2.16.0エディターの送信処理](https://github.com/Frieve-A/effetune/blob/v2.16.0/js/remote/legacy-sync-engine.js)・[現在のDSP公開操作](../../../pipetune/include/pipetune/dsp_pipeline.h)・[処理中のパイプラインの差し替え](../../../pipetune/src/dsp_pipeline_slot.h)

適用結果は要求番号`seq`に対応する`ack`で返す。内容が変わったら、更新番号`rev`を進めて`state`を配信する。CLI・GTKからのプリセット切替と、監視対象ファイルの再読込も同じ状態に反映する。複数端末からの要求は受理順に扱い、古いアセット準備が後から完了しても新しい設定を上書きしない。失敗時は直前の音と設定を保ち、再接続した端末には最後に確定した内容を返す。レートやバックエンドの変更による再構築にも編集中の値を使う。[状態通知の仕様](https://github.com/Frieve-A/effetune/blob/v2.16.0/docs/remote-v1.md#pushes-app--client)・[プリセットの変更監視](../../../pipetune/src/active_preset_file_monitor.cpp)

最初の動作確認にはVolumeなど外部ファイルを必要としないエフェクトを使う。IR・SFZ・測定結果を参照するチェーンは、このPCに登録済みの素材で検証する。編集可能なエフェクトと拡張は実装・検証した範囲で通知する。複数端末の状態確認には基本の`state`を使い、段のIDを使う共同編集、IR転送、音楽操作を表す`sync1`・`irSync`・`music1`はそれぞれ独立した拡張として扱う。[remote-v1の機能と拡張](https://github.com/Frieve-A/effetune/blob/v2.16.0/docs/remote-v1.md)

## プレーヤー案1 外部の受信機をPipeTuneにつなぐ

upmpdcliは再生機としてLANに機器名を公開し、選曲・再生・一時停止・停止・シークを受け付ける。MPDは同じPCで音源を取得し、デコードしたPCMをPipeWireへ出力する。初版ではどちらもPipeTuneと同じユーザーのセッションで起動する。シークは部分取得に対応する音源で検証する。[upmpdcliの再生機能](https://www.lesbonscomptes.com/upmpdcli/)・[上流のキューの扱い](https://github.com/Frieve-A/effetune/blob/v2.16.0/docs/i18n/ja/openhome.md#ライブラリと再生先を選ぶ)

単一出力モードではMPDの音を通常のアプリと同じ処理入力へ通し、OSで選んだ物理出力から聴く。複数出力モードでは、OSの既定がDirectでもMPDはステレオの`PipeTune Processed Audio`へ接続する。製品の処理入力のノード名は`pipetune_sink`である。MPDのPipeWire出力の`target`でこの入力を指定する設定を両モードで試す。再生機の音量・ミュートはMPDの再生ストリームに適用し、他のアプリの音とPipeTuneの共通出力音量は独立して操作できるようにする。[PipeTuneの出力モード](../../../README_ja.md#複数モード)・[製品の入力ノード名](../../../pipetune/src/main.cpp)・[MPDのPipeWire出力](https://mpd.readthedocs.io/en/stable/plugins.html#pipewire)

### プレーヤー案2へ移行できる構成

初版の成果物に、専用インスタンス用のMPDとupmpdcliの設定例と起動手順を含める。機器名、公開するLANインターフェイス、同一PC内のMPD接続先、PipeWireの出力先、設定・状態の保存先を一組として扱う。個人が別用途で使うMPDとの取り違えを防ぐため、接続先と保存先はこの受信機に割り当てる。起動元が変わっても同じ設定を読み込める構造にする。[upmpdcliの設定項目](https://www.lesbonscomptes.com/upmpdcli/pages/upmpdcli-manual.html)・[MPDのユーザー設定](https://mpd.readthedocs.io/en/stable/user.html)

| 責任 | 初版の手動運用 | 後続のPipeTune管理 |
| --- | --- | --- |
| 起動・停止と障害復旧 | 利用者が専用の設定で両プロセスを起動・停止する | 専用のsystemdユーザーサービスが同じ設定で起動・停止し、終了理由と再起動結果を報告する |
| キューと再生位置 | OpenHomeの曲順は受信機が持ち、MPDが現在曲と再生位置を報告する | CLI・GTKは同じ受信機から読み取った曲名・位置・受信状態を表示する。UPnP AVで操作端末が管理する曲順もその端末に委ねる |
| 配線と障害の影響 | MPDの出力を処理入力へつなぐ。受信機が停止しても通常のアプリ音はDSPを通る | 管理サービスは同じ入力への復帰を確認する。DSPデーモンと受信機はそれぞれ単独で起動できる |

移行時には、利用者が手動で起動した専用インスタンスを終了してから、その設定と状態の保存先を管理サービスへ引き継ぐ。二重起動とポートの競合は診断し、既存のシステムサービスや他用途のMPDの停止は利用者の管理に委ねる。初版の検証でも、同じ設定を手動と一時的なユーザーサービスから順に起動し、機器の識別、接続先、DSP後の出力を比較できる。管理用の常設ユニット、GTK・CLIの受信設定画面、再起動制御の製品化はプレーヤー案2の工程とする。現在曲や状態を表示する際は、MPDの公開操作`currentsong`・`status`と変更通知`idle`を利用できる。[既存のDSPユーザーサービス](../../../pipetune/packaging/systemd/pipetune.service.in)・[MPDの状態取得と通知](https://mpd.readthedocs.io/en/stable/protocol.html#querying-mpd-s-status)

## 初版の実装工程と完了条件

各工程では利用者が通る入口からテストし、コードを修正する場合は対象の問題を再現するテストのRED、修正後のGREENを確認してコミットする。音声の判定には、既存の製品出力テストと隔離したPipeWire環境を使う。再生状態や固定時間の経過だけで成功とせず、出力先で受信したPCMを調べる。[製品出力テスト](../../../pipetune/test/product_output_test.cpp)・[複数出力の検証環境](../../../pipetune/test/multi-device-output.mjs)

### 1 リモートで選んだ曲をPipeTuneで鳴らす

専用の設定例で両プロセスを手動起動する。EffeTune 2.16.0からOpenHomeの再生先として選び、独立した音源サーバーの既知のFLACまたはWAVを再生する。Volumeを−6 dBにしたとき、同じ出力音量でのバイパス時に対する振幅比が約0.501になることを測る。UPnP AVの再生先についても音源指定から出力まで確かめる。

完了条件は、単一・複数出力の両方で期待したチャンネルから処理済みPCMが出ること。複数出力ではDirectをOSの既定にした場合も確認する。一時停止・再開・シーク・停止が出力に反映され、OpenHomeでは音源を稼働させて操作端末を閉じた後に2曲目へ進むことを記録する。

### 2 ペアリングしたエディターからチェーンを適用する

LAN用の編集入口とエディターの配信を追加する。手元の端末でリンクを開き、稼働中のチェーンを読み、Volumeを含むチェーンに切り替える。デスクトップ版の別ウィンドウとブラウザーの両方を使う。ブラウザーでの操作はPlaywright MCPでも再現する。

完了条件は、エディターから変更したゲインが工程1の曲に反映され、バイパスを有効にして戻すと編集したチェーンで鳴ること。未知のエフェクトを含むチェーンはエラーになり、直前の音と表示が一致することも確かめる。無効なトークンや取り消した接続では設定を変更できず、新しいリンクで再接続すると現在の状態を取得できることを確認する。

### 3 再生を続けながら個別パラメーターを調整する

構成が同じチェーンの差分更新を実装する。残響の後段のVolumeをエディターで連続操作し、つまみを動かす前に入力した音の残響に新しいゲインが掛かることをPCMで確かめる。`params`を直接送った場合と、上流エディターが`chain`を送った場合を比較する。アセットの準備を伴う変更では失敗と取消も再現する。

完了条件は、連続編集で不要なDSPの初期化や音切れが生じず、2台のエディターに最後に受理した状態が表示されること。片方の切断、ローカルでのプリセット切替・再読込、レート変更を挟んでも、実音と再接続時の表示が一致することを確かめる。

### 4 両機能の併用と管理方式の移行を検証する

ネットワーク再生中に別の端末からDSPを編集する。MPDまたはupmpdcliを停止し、通常のデスクトップ音声が引き続き処理されることを確認する。手動起動から一時的なユーザーサービスへの切り替えでは、同じ設定で再発見・再生でき、二重起動やDirectへの誤接続がないことを調べる。利用手順には、検証したOS、受信機の版と必要な出力プラグインを記載する。

初版の最終完了条件は、実際のEffeTuneから選曲とDSP編集の双方を行い、その効果を同じ出力PCMで確認できること。さらに、起動方法を替えても同じ試験が通り、最後のコード変更後に`make test`で全体テストが成功することとする。実装時に実機と隔離環境の結果を区別して記録する。

## 2026年11月20日の実装・検証結果

PipeTuneはDSP編集と再生指示を受ける側として動作する。EffeTune 2.16.0のデスクトップ版では、再生先に「PipeTune (UPnP AV)」を選ぶと選曲からDSP編集まで利用できた。OpenHomeで同じアプリから選曲する条件には、下記の互換性問題が残った。[利用手順](../../../README_ja.md#ネットワークから曲を再生する)

### 工程ごとの結果

| 工程 | 確認した動作と判定 |
| --- | --- |
| 1 ネットワーク再生 | 専用のMPDがWAVを取得し、処理入力に送った。単一出力と、Directを既定にした複数出力で、左右の振幅がバイパス時の8192から−6 dBで4106に変化した。OpenHomeの受信機は一時停止・再開・シーク・停止に応答し、操作側の探索を閉じてから2曲目へ進んだ。EffeTuneアプリからのOpenHome選曲のみ未達。 |
| 2 遠隔編集 | ブラウザーのペアリングリンクとデスクトップ版の「Join another EffeTune」から接続した。別ウィンドウのVolumeを−12 dBにすると、ブラウザーにも同じ値が表示され、再生中の曲の振幅は2058になった。バイパス中の編集と解除、プリセット取得、無効なトークンと接続元の拒否、トークンの更新、TLS接続も確認した。 |
| 3 連続編集 | 残響の後段のVolumeを連続変更した。インパルス入力後の出力を、編集しないFDN Reverbに新しいゲインを掛けた参照波形と比較し、残響が継続することを確かめた。チェーンの置換と個別パラメーターの変更は同じ編集状態に反映された。ローカルでの選択とファイル再読込、48／96 kHz、DSPバックエンドの切り替えを検証した。登録済みのIR、Room EQの測定データ、SFZ音源の適用も成功した。 |
| 4 併用と移行 | 同じ設定で手動プロセスから一時的なsystemdユーザーサービスへ移行した。再生機の識別子とキューの曲は維持され、MPDと受信機の二重起動は拒否された。受信機のミュート中と停止後のどちらも、別のデスクトップ音声に−6 dBのDSPが掛かり、振幅6144が3079になった。 |

遠隔編集はメモリ上のチェーンを使う。安全に値を変更できるエフェクトでは、音声ブロックの境界で値を切り替える。アセットの準備に失敗した場合と、別の要求で準備を取り消した場合は、最後に受理したチェーンを保つ。出力チャンネル数が変わる場合も、エディターに知らせるのは出力の切り替えが確定したチェーンである。[遠隔操作の回帰試験](../../../pipetune/test/remote-control.mjs)・[残響と出力切り替えの試験](../../../pipetune/test/dsp_pipeline_slot_test.cpp)

### 実際のアプリとOpenHomeの互換性

EffeTune 2.16.0は選曲を確定する前に、OpenHomeの曲順が読み取り中に変化したかを`IdArrayChanged`で問い合わせる。upmpdcli 1.9.17は曲順が同じときに真を返すため、EffeTuneは再読込を求め、曲を登録できない。同じ応答の計算はupmpdcli 1.10.1のソースにもある。実際に選曲と再生を確認した組み合わせはEffeTune 2.16.0とupmpdcli 1.9.17のUPnP AVである。OpenHomeの受信・次曲の検証には、同じ版の探索・通信コードから受信機の公開操作を呼び出した。[EffeTuneのキュー照合](https://github.com/Frieve-A/effetune/blob/v2.16.0/electron/network-music-host.cjs#L291-L293)・[upmpdcli 1.9.17のソース](https://www.lesbonscomptes.com/upmpdcli/downloads/upmpdcli-1.9.17.tar.gz)・[upmpdcli 1.10.1のソース](https://www.lesbonscomptes.com/upmpdcli/downloads/upmpdcli-1.10.1.tar.gz)

### 検証環境と再実行

Ubuntu 24.04.5で、PipeWire 1.0.5、WirePlumber 0.4.17、PipeWire出力付きのMPD 0.23.14、upmpdcli 1.9.17を使った。ネットワークとPipeWireセッションを隔離し、仮想ステレオ出力の左右のPCMを収録した。デスクトップ版は公式のLinux AppImageをXvfbの画面で実行した。ブラウザーとアプリの編集ウィンドウはPlaywright MCPから操作した。製品同梱の2.18.1と、検証用に配置した2.16.0のエディター資材の両方で試験を通した。物理スピーカーでの聴感試験は実施していない。[EffeTune 2.16.0リリース](https://github.com/Frieve-A/effetune/releases/tag/v2.16.0)

最終コード`b962458`で`CTEST_PARALLEL_LEVEL=4 make test TEST_BUILD_DIR=build/effetune-2181-debug`を実行した。全309件が成功し、CTestの実行時間は7分41秒だった。一時ディレクトリへインストールしたデーモンと同梱エディターでも、手動起動からユーザーサービスへ移行する再生・編集試験に成功した。

リポジトリのルートで`make test`を実行すると、デーモンの遠隔編集とDSPの試験が走る。受信機を含む追加試験には、上記の音声ソフトウェア、Node.js 24、rootless Podman、iproute2、D-Bus、利用可能なsystemdユーザーセッションを用意する。第1引数はテスト用のビルド先、第2引数はEffeTune 2.16.0のソースディレクトリである。[隔離環境の起動](../../../pipetune/test/network-player-session.sh)・[ネットワーク再生とPCMの照合](../../../pipetune/test/network-player.mjs)

```sh
pipetune/test/network-player-session.sh build/test /path/to/effetune-2.16.0
```

実際のエディターを含めるときは、展開したAppImage内の実行ファイル、Chromium、Playwright MCPのパスを指定する。ブラウザーでゲインを調整し、デスクトップ版のライブラリから曲を選んだ後、別ウィンドウで同じチェーンを編集する。[エディターの操作試験](../../../pipetune/test/network-player-editor.mjs)

```sh
export PIPETUNE_TEST_EDITOR="$PWD/pipetune/test/network-player-editor.mjs"
export PIPETUNE_TEST_EFFETUNE_DESKTOP=/path/to/squashfs-root/effetune
export PIPETUNE_TEST_CHROMIUM=/path/to/chrome
export PIPETUNE_TEST_PLAYWRIGHT_MCP=/path/to/playwright-mcp
xvfb-run -a pipetune/test/network-player-session.sh build/test /path/to/effetune-2.16.0
```
