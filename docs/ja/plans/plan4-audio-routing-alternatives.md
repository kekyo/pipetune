# plan4 システム音声をPipeTuneに接続する方式の比較

出力構成を変更するとユーザーセッションが応答しなくなったり、選んだ機器から音が出なくなったりするとの報告がある。安定性を優先する場合の第一候補は、OSでPipeTuneを出力先に選び、PipeTuneでスピーカーやDACを選ぶ方式である。物理機器をOSから直接選んで再生する経路も残せる。

2026-11-23時点の検討を記録する。調査基準は `develop` の `9af7280`。現行コードでセッション全体に影響し得る箇所を調べた。報告されたハングの原因は未特定で、代替方式の採用は実機での確認後に判断する。

## 現在の音声経路とセッションへの影響

PipeWireはアプリと機器の間で音声を運び、WirePlumberはデバイスの検出や接続先の選択を担う。PipeTuneはすでに独立したユーザープロセスとしてPipeWireからPCM音声を受け取り、DSPで処理して出力している。方式を見直す範囲は主にDSPの前後の接続と音量制御である。[PipeWireのセッション管理](https://docs.pipewire.org/page_session_manager.html)・[PipeTuneのストリーム定義](../../../pipetune/src/filter_graph_properties.cpp)

単一モードでは、利用者がOSで物理出力を選ぶと、その経路にPipeTuneが自動で入る。WirePlumber 0.5系ではフィルターを自動接続する標準機能のsmart filterを使う。0.4系ではPipeTuneが配布するLuaスクリプトで標準の接続処理の一部を置き換えている。[0.4系の互換処理](../../../pipetune/src/wireplumber_04_compat.cpp)・[単一モードの操作](../../../README_ja.md#単一モード)

複数モードでは、OSに2つの仮想出力が現れる。`PipeTune Processed Audio`に送った音声はDSPを通り、`PipeTune Direct Output`に送った音声はDSPを通さずに合流する。PipeTuneは合成後の音声に共通音量を適用し、PipeWireの標準モジュール `combine-stream` で各チャンネルを指定の物理機器へ送る。この分配経路は代替案でも利用できる。[複数モードの操作と音量](../../../README_ja.md#複数モード)・[分配経路の構築](../../../pipetune/src/output_distribution.cpp)

独自のLuaスクリプトは、内部ノードを見られるクライアントを制限する。複数モードでは既定出力を切り替え、選択した物理機器の音量とミュートも変更・復元する。さらに、PipeWireサーバー内で動く独自モジュールが、一般クライアントに対して物理出力端子を一覧から隠す。通常のデスクトップ再生への影響を小さくするには、この表示処理も見直す必要がある。[WirePlumberの権限・音量制御](../../../pipetune/src/wireplumber_visibility.cpp)・[PipeWireサーバー内の表示処理](../../../pipetune/src/pipewire_presentation.c)

`pipetune setup` が管理対象の音声設定ファイルを更新すると、PipeWire、WirePlumber、pipewire-pulseの3サービスをまとめて再起動する。この再起動はセットアップ時の処理であり、通常の出力切り替えとは適用のタイミングが異なる。[セットアップ処理](../../../pipetune/src/user_setup.cpp)

## 案1　PipeWireの仮想出力を選ぶ

音声は「アプリ → PipeTune仮想出力 → DSP → PipeTuneで選んだ物理出力」の順に流れる。デスクトップ標準のWirePlumberに機器の検出と通常の接続を任せ、PipeTune独自のLuaスクリプトとサーバー内の表示モジュールを外す。OSの既定出力にPipeTuneを選ぶと、既定に従うアプリの音声が処理される。物理出力を指定したアプリは、その機器へ直接再生できる。[PipeWireのストリームと接続先](https://docs.pipewire.org/group__pw__stream.html)

物理出力が1台でも、現行の複数モードと同様にProcessedを入口にする。DSPの送り先は選択した物理機器に固定する。これにより、OSの既定出力がPipeTune自身でも音声が循環しない。複数台では既存のチャンネル割り当てと遅延補償を生かせる。USB DACが外れたときは、その機器向けのチャンネルだけを止め、同じ機器が戻ったら元の割り当てで再生する。[機器とチャンネルの割り当て](../../../pipetune/src/output_distribution.cpp)・[PipeWireのCombine Stream](https://docs.pipewire.org/page_module_combine_stream.html)

物理機器の音量とミュートはOSに任せ、PipeTuneの共通音量は自身を通る音声だけに掛ける。現在の複数モードは物理機器のゲインを1に保ち、ミュートを解除している。この動作を残して物理出力を公開すると、アプリが機器を直接選んだ際にPipeTuneの共通音量を通らず、大きな音が出る可能性がある。[現行の音量仕様](../../../README_ja.md#音声出力の選択)

PipeTuneが停止しても、利用者はOSから物理出力を選び直せる。すでに再生中のアプリが自動で物理出力に移るかどうかは、アプリの出力指定とセッションの接続方針による。

## 案2　単一モードにWirePlumber 0.5の標準smart filterを使う

OSでスピーカーとヘッドホンを切り替えるだけでDSPが追従する操作を優先するなら、この案が現行の使い方に最も近い。0.5系の単一モードでは現在のフィルター入出力を公開し、標準のsmart filterに自動接続を任せる。0.4系と複数出力には案1を使う。独自の経路制御と物理出力を隠す処理を外すため、デスクトップによってはOSの出力一覧に仮想出力も表示される。[WirePlumberのSmart Filters](https://pipewire.pages.freedesktop.org/wireplumber/policies/smart_filters.html)

この案でも単一モードの接続はWirePlumberの動作に依存する。既存の試験では、0.4.17の再起動後に再生中のアプリが無音になり、PipeTuneを起動していない環境でも再現した。0.5.8では同じPipeTune再生試験で音声が復帰した。この記録は0.4系の独自対応を縮小する判断材料になる。今回報告されたセッションのハングが0.5系で解消するかは未確認である。[プロジェクトの再現記録](../../../README_ja.md#pipetuneの更新と削除)・[0.4.17の音声アダプター](https://github.com/PipeWire/wireplumber/blob/0.4.17/modules/module-si-audio-adapter.c)・[0.5.8の音声アダプター](https://github.com/PipeWire/wireplumber/blob/0.5.8/modules/module-si-audio-adapter.c)

## 案3　PulseAudio本体に対応する

PulseAudioの `module-null-sink` で仮想出力を作る。PipeTuneは、その出力に届いた音声を読み出せるモニター入力から録音し、DSPで処理して指定の物理出力へ再生する。PulseAudio本体を音声サーバーに使う環境ではWirePlumberを必要としない。PipeWireのPulseAudio互換サービス `pipewire-pulse` に接続する環境では、アプリ側のAPIを `libpulse` に変えても、下層のデバイス管理は従来の構成に従う。[PulseAudioの仮想出力とモニター入力](https://www.freedesktop.org/projects/pulseaudio/Documentation/User/Modules/)・[pipewire-pulseの互換モジュール](https://docs.pipewire.org/page_pulse_module_null_sink.html)

PulseAudio環境を正式にサポートする場合の候補である。現在と同じ機能を提供するには、PipeWireを直接使うアプリの音声を取り込む方法や、複数DACのクロック差とチャンネル割り当てを別途扱う必要がある。3案のうち変更範囲が最も広い。

## 最初の検証と採用条件

まず案1を物理出力1台で試す。既定出力に従うアプリを再生したまま、利用者がOSのサウンド設定を操作でき、次の条件を音声サービスの再起動なしに満たすことを確認する。

| 操作 | 完了条件 |
| --- | --- |
| 再生中のUSB DACを抜き差しする | 切断中はそのチャンネルの音が別の機器から出ず、再接続後は同じDACで再生が戻る |
| PipeTuneを強制終了し、OSから物理出力を選び直す | 通常のアプリから再生でき、物理機器に設定した音量とミュートが有効である |

この2条件を確かめてから、案1を複数出力へ広げるかを判断する。OSで物理機器を選ぶだけでDSPが自動追従する操作も必要なら、0.5系の単一モードに限って案2を組み合わせる。

## 案1の試験実装と確認結果

案1を `pipetune setup --virtual-output` で選べるようにした。物理出力が1台でも `pipetune output select` でその機器を割り当て、OSで `PipeTune Processed Audio` を選ぶ。既存環境からの移行では、PipeTuneが設置した音声設定をディスクから取り除き、現在のセッションでは再生を続ける。すべてのログインセッションが終了し、新しい音声セッションが始まると仮想出力が有効になる。操作手順は[日本語READMEの「仮想出力を使う場合」](../../../README_ja.md#仮想出力を使う場合)に記載した。

次の試験では、デスクトップから独立したPipeWireとWirePlumberを起動した。物理機器の代わりに音声を受け取ってサンプル値を照合できる出力を用意し、音声サービスを再起動せずに操作した。

| 試験 | 確認した動作 |
| --- | --- |
| 割り当てた唯一の機器を再生中に切断し、同じ識別情報で作り直す | 切断中にアプリが32,768フレーム以上を送っても、他の出力には音が漏れなかった。再接続後は元のチャンネルでDSP処理後の音声を受信した |
| 2台のうち1台を切断する | もう1台には割り当てどおりの音声が届いた。戻った機器は新しいPipeWireノードとして元のチャンネルで再生した |
| 物理出力の音量・ミュートを設定し、PipeTuneを強制終了する | 左右の音量と別の機器のミュートは終了前後で一致した。アプリから物理出力を選んで直接再生すると、左右それぞれの音量が実際のサンプル値に反映された |
| OSでProcessedを既定にして、CLIから機器の割り当てを解除・再選択する | Processedは同じノードとして残り、既定出力に従うアプリの音声が選んだ機器に届いた |
| ProcessedとDirectを同時に使い、音量・ミュートとサンプルレートを変更する | 混合後の音量とメーターの値が一致し、割り当てていない機器から音が出なかった |

WirePlumber 0.4.17では、内部の分配先を作り直す間に、Directの再生ストリームとメーターが一時的に別の機器へ接続される事例を隔離試験で再現した。仮想出力方式では、以前の接続を閉じ、新しい分配先がPipeWireに登録されてから、そのノードの識別番号を指定して再接続する。機器の割り当て変更中もProcessedの公開入力は維持する。サンプルレート設定の変更でProcessedを作り直す場合や、チャンネル数の変更でDirectを作り直す場合、その入力を使うアプリの再接続はデスクトップ側の方針に従う。[WirePlumberの接続先選択](https://pipewire.pages.freedesktop.org/wireplumber/policies/linking.html)

隔離試験ではWirePlumber 0.4.17と0.5.13を使った。採用条件のうち、実際のUSB DACを抜き差ししたときの再生と、その間もデスクトップを操作できることは実機確認が残る。それまでは案1を選択式とし、通常のセットアップには従来方式を使う。
