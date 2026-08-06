# PipeTune

Linuxのデスクトップセッションの音声に、EffeTuneで構築したDSPプリセットを適用

![PipeTune](./images/pipetune-120.png)

[![Project Status: Active – The project has reached a stable, usable state and is being actively developed.](https://www.repostatus.org/badges/latest/active.svg)](https://www.repostatus.org/#active)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

----

[(English language is here)](./README.md)

## これは何?

PipeTuneは、Linuxのデスクトップセッションの音声に
[EffeTune](https://github.com/Frieve-A/effetune) で構築したDSPプリセットを適用します。

通常のデスクトップ音声設定で選択した物理出力の直前へ、WirePlumberが内部
DSPフィルターを挿入して実現します。また、デスクトップのシステムトレイに常駐する
GTK 3コントロールアプリケーションを提供します。

![PipeTune UI](./images/pipetune-ui.png)

### 機能

- `.effetune_preset`拡張子の標準形式および旧形式のEffeTuneプリセットファイルを
  読み込みます。
- 有効なEffeTuneネイティブDSPパイプラインをデスクトップ音声へ適用します。
- 物理出力の選択と音量調整には従来どおりOSの音声設定を使います。
  WirePlumber 0.4では、内部PipeTuneノードも一覧に表示される場合があります。
- 出力ごとの最大対応周波数、または44.1、48、96、192、384 kHzの指定周波数で
  独立してDSPを計算します。
- 単一パッケージ・単一PipeTuneバイナリのまま、実行時にWirePlumber 0.4/0.5
  用ポリシーが選択されます。
- 互換性重視のScalar、SIMD自動選択、CPU検証済みの命令セット別実装を選択できます。
- CLIコマンド1つで、ユーザーごとの設定または解除を行えます。
- GTKアプリケーションに実行状態を表示します。

### 対応システム

PipeTuneには、PipeWireデスクトップセッションとsystemdユーザーサービスが必要です。
単独のPulseAudioセッションには対応していません。

ビルド済みDebianパッケージは、次の環境向けに公開しています。

| ディストリビューション | リリース | アーキテクチャ |
| --- | --- | --- |
| Debian | bookworm | amd64, i386, arm64, armhf |
| Debian | trixie | amd64, i386, arm64, armhf, riscv64 |
| Ubuntu | 24.04 | amd64, arm64 |
| Ubuntu | 26.04 | amd64, arm64 |

ダウンロードするパッケージのアーキテクチャが分からない場合は、次のコマンドで
確認できます。

```sh
dpkg --print-architecture
```

---

## ダウンロードとインストール

[PipeTuneのGitHub Releasesページ](https://github.com/kekyo/pipetune/releases/) から、
使用するディストリビューション、リリース、アーキテクチャに合う`.deb`をダウンロードしてください。

ダウンロードしたパッケージがあるディレクトリでターミナルを開きます。
そのディレクトリに対象のPipeTuneパッケージだけがあることを確認して、次のコマンドでインストールします。

```sh
sudo apt install ./pipetune-*.deb
```

`apt`は、パッケージと必要なランタイム依存関係をインストールします。

インストール出来たかどうかは、以下のコマンドで確認出来ます:

```sh
pipetune --version
```

## 初期設定

PipeTuneはユーザーレベルデーモンとして動作するため、パッケージのインストール後にユーザー毎に個別の追加セットアップが必要です。

`sudo` を付けずに、デスクトップを使用する一般ユーザーとしてsetupを実行します。

```sh
pipetune setup
```

`setup`は動作中のWirePlumberへインストール済みポリシーを再読込させ、PipeTuneの
systemdユーザーサービスを有効化・再起動し、アクティブ状態を確認してからPipeTune GTK
アプリケーションをシステムトレイに常駐させます。

![System tray](./images/system-tray.png)

システムトレイアイコンをダブルクリックするか、あるいはメニューから"Open"を選択することで、PipeTune設定ウインドウを表示できます。

## PipeTune設定ウインドウ

PipeTune設定ウインドウは、左側のセクション分けされたPipeTuneステータスを常に表示し、
右側で**Processing**、**Rate**、**DSP**、**Advanced**の設定を切り替えます。

![PipeTune UI Window](./images/pipetune-ui-window.png)

設定を変更すると、動作中のデーモンへ直ちにプレビュー反映されます。
単一の**Apply**ボタンは、デーモンが確認したすべての選択を一つの起動設定として
原子的に保存します。**Cancel**、Escape、またはタイトルバーの閉じるボタンは、
以前のライブ状態へ戻してからウィンドウを隠します。

常時表示されるペインでは、DSPの**Load**を接続状態の直下に配置し、ステータス
アイコンを避けて接続ラベルの左端に揃えます。現在のパーセント値を内包する
横方向に伸縮するメーターで表示し、グラフの塗りつぶしは100%を上限としますが、
100%を超えた実測値もテキストにはそのまま表示します。

下部の**Action Log**ドロワーでは、接続、プレビュー、保存、失敗の履歴を参照できます。
PipeTune切断中は設定が読み取り専用になり、再接続後に処理を再開します。

## 音声ストリームとPipeTune

アプリケーションとデスクトップは、従来どおり通常の物理出力を指定します。
WirePlumberが、その出力に対応する準備済みPipeTuneフィルターを暗黙に挿入します。
アプリケーション音量はフィルター前、物理出力の通常音量は最終段で一度だけ適用されます。

```mermaid
flowchart LR
    app["1. アプリケーション<br/>ブラウザー・プレーヤー・ゲーム"]
    os["2. OSの音声設定<br/>（通常の物理出力）"]
    mix["3. PipeWireミックス<br/>アプリケーション音量"]
    tune["4. 内部PipeTuneフィルター<br/>(EffeTune DSP)"]
    device["5. 物理出力<br/>通常のデバイス音量"]

    app -->|"① 音声を送る"| os
    os -->|"② 選択した出力へルーティング"| mix
    mix -->|"③ ミックス済み音声"| tune
    tune -->|"④ 処理済み音声"| device
```

WirePlumber 0.5では、フィルターノードを通常のクライアントから参照できないようにします。
WirePlumber 0.4では、安定版Lua APIが非表示ノードを安全にリンクするための権限を
設定できないため、参照可能な状態を維持します。クライアントの一覧には内部
`pipetune.filter.*`ノードも表示される場合がありますが、通常の物理デバイスを
選択してください。既定出力、アプリごとのルーティング、ミュート、音量は、その
物理出力を引き続き使用します。

PipeTuneは、対象となるローカル物理出力ごとに独立したフィルターを管理します。
出力の追加や再接続時には自動的にフィルターを作ります。フィルターの準備中、非対応、
または障害時には、WirePlumberがその出力を直接経路のままにして、DSP障害で音声全体が
失われないようにします。ステータスには各出力の有効・待機・直接・エラー状態を表示します。

## EffeTune DSPプリセット選択

EffeTune DSPプリセットをPipeTuneにロードする場合、以下のファイルを選択できます:

- EffeTune標準のDSPプリセット群
- EffeTune (Linux AppImage版)で保存されたユーザープリセット群
- 個別の `*.effetune_preset` ファイル

このうち、Linux AppImageで保存されるユーザープリセットファイルは `$XDG_CONFIG_HOME/effetune/effetune_presets.json`
（あるいは `~/.config/effetune/effetune_presets.json`）に存在します。

リストからプリセットを選択するかファイルを指定すると、DSPへ直ちに
ライブプレビューされます。ダイアログ全体の設定を保存するには**Apply**を使用します。
**Enable DSP processing**をオフにするとバイパスをプレビューし、Cancelで以前の
ライブ処理へ戻せます。

CLIでは、プリセットファイルのパスを指定するか、またはバイパスモードを指定することが出来ます。

```sh
pipetune setup --preset /absolute/path/to/example.effetune_preset
pipetune bypass
```

## PCM周波数の選択

PipeTune設定ウインドウの**DSP rate**と**PipeWire request**ドロップダウンから設定できます。
**Max**は、各出力が対応する44.1、48、96、192、384 kHzのうち最高の周波数を
出力ごとに使用します。各指定周波数の行には、すべての出力が対応するか、いずれかで
リサンプリングが必要かを表示します。**Effective rates**には各出力について次を表示します。

- 入力とEffeTune DSPの周波数
- その出力で解決したPipeWireグラフ周波数
- 動作中の物理デバイス周波数（アイドル時は`idle`）
- PipeWireのリサンプリングが必要かどうか

指定周波数は各フィルターのDSP計算周波数となります。出力が対応していない場合は、
その周波数以下で対応する最大周波数を出力側に選び、それもなければ
デバイスの最小対応周波数を選びます。この間の変換はPipeWireが行います。

**Suggest**は`node.rate`を提案として設定するため、PipeWireが異なるグラフ周波数を
選ぶ場合があります。**Force**はさらに、PipeTuneの再生ノードが動作している間、
その周波数を維持するようPipeWireへ要求します。どちらもPipeWire全体の
グローバルクロック設定は変更しません。

CLIでは、同じ情報の表示と設定を次のコマンドで行えます。

```sh
pipetune rate list
pipetune rate get
pipetune rate set max suggest
pipetune rate set 192000 force
```

`rate list`は、利用可能な各出力について5つの選択可能周波数への対応状況を表示します。
`rate get`は、設定済みポリシーと最終周波数を表示します。
デーモン接続中の`rate set`は直ちに切り替え、デーモンが成功を確定した後でのみ
設定を保存します。デーモンが利用できない場合は、次回起動用として保存します。
切り替え中はDSPとPipeWireストリームを再構築するため、短い無音区間が発生する
場合があります。

## ネイティブDSPバックエンドの選択

PipeTune設定ウインドウのDSPページにある**Native backend**から
選択できます。**Scalar**は互換性重視の既定値です。**SIMD (Auto)**は、
CPUが対応し検証に通った最上位の実装を自動選択します。同じドロップダウンから、
対象アーキテクチャで利用できるbaseline、x86-64-v3、x86-64-v4、
Arm64 SVEを固定することもできます。

CLIでは、同じ情報の表示と設定を次のコマンドで行えます。

```sh
pipetune dsp list
pipetune dsp get
pipetune dsp set scalar
pipetune dsp set simd
pipetune dsp set simd --variant x86-64-v3
```

デーモン接続中の`dsp set`は、現在のプリセットパイプラインを新しい
バックエンドで再構築し、デーモンが成功を確定した後でのみ設定を保存します。
DSP内部状態はリセットされるため、切り替え時の不連続や短い無音は許容されます。
デーモンが利用できない場合は、ローカル検証に通った選択を次回起動用に保存します。
起動時にSIMDのCPU要件またはライブラリ検証を満たせない場合は、利用可能な
下位SIMDまたはScalarへフォールバックし、実際のバリアントと理由を状態表示に
残します。

効果はプリセットとそこで使われるDSP演算の種類に大きく依存します。

## PipeTune設定のリセット

PipeTune設定ウインドウのAdvancedページにある**Restore Defaults**は、次の状態を
ライブプレビューします。この時点では保存しません。

- DSPは**Bypass**
- 物理出力は**System default**
- PCM周波数は**Max**、PipeWire要求は**Suggest**
- ネイティブDSPバックエンドは**Scalar**、SIMD設定は**Auto**

**Apply**で既定値を保存し、**Cancel**で以前のライブ設定へ戻せます。このGTK操作は
サービスを再起動しません。設定を直ちに置き換えてサービスを再起動するCLI操作も
引き続き利用できます。

```sh
pipetune config reset
pipetune config reset --yes
```

`--yes`（または`-y`）を付けない場合は確認を求めます。設定ファイルは
バックアップせずに原子的に置き換えられるため、非対応の旧形式設定からの
復旧にも使用できます。実行中のユーザーサービスは直ちに再起動し、
停止中のサービスは停止したままです。

## PipeTuneの更新と削除

PipeTuneを更新するには、
[GitHub Releases](https://github.com/kekyo/pipetune/releases/) から新しい対応パッケージを
ダウンロードし、同じ`sudo apt install ./pipetune-*.deb`コマンドでインストールします。
その後、デスクトップを使用する一般ユーザーとして`pipetune setup`を実行します。

パッケージを削除する前に、デスクトップを使用する一般ユーザーとしてユーザーごとの
設定を解除します。

```sh
pipetune unsetup
sudo apt remove pipetune
```

`unsetup`はGTKアプリケーションを終了し、ユーザーサービスを無効化・停止します。
物理出力の選択はPipeTuneが所有しないため、復元処理は不要です。また、GTKアプリケーションが
自動起動しないようユーザー用の自動起動マスクを作成します。起動時のプリセット選択は保持されます。
PipeTuneのアプリケーション設定も削除する場合は、`pipetune unsetup --purge`を
使用します。

独自のユーザー用自動起動エントリーをマスクする必要がある場合、`unsetup`は
PipeTune管理のバックアップとして保存します。後で`pipetune setup`を実行すると、
そのバックアップを上書きせずに復元します。パッケージの削除だけでは、
ユーザーごとの設定や自動起動オーバーライドは削除されません。

## ログと復旧

デーモンのログは次のコマンドで確認できます。

```sh
journalctl --user -u pipetune.service
```

PipeTuneが異常終了した場合も、内部フィルターノードが消えるとWirePlumberが
アプリケーションを直接経路へ戻します。物理出力とその音量は変更されません。

---

## 関連情報

- [デーモンの操作方法と開発者向けドキュメント (英語)](pipetune/README.md)
- [GTKアプリケーションの動作 (英語)](pipetune-gtk/README.md)
- [ネイティブDSPバックエンドとベンチマーク (英語)](pipetune/docs/dsp-backends.md)

## 制約

現在のバージョンでは、Room EQとIR Reverbは使用できません。これらに必要なアセットは
EffeTuneのIndexedDB内に保存され、`.effetune_preset`ファイルには含まれないため、
PipeTuneから読み込むことができません。

## ライセンス

Under MIT.
