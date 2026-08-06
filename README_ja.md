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

WirePlumberが、アプリケーション音声をミックスした後の通常の再生経路へ
PipeTuneを透過フィルタとして挿入します。また、デスクトップのシステムトレイに
常駐するGTK 3コントロールアプリケーションを提供します。

![PipeTune UI](./images/pipetune-ui.png)

### 機能

- `.effetune_preset`拡張子の標準形式および旧形式のEffeTuneプリセットファイルを
  読み込みます。
- 有効なEffeTuneネイティブDSPパイプラインをデスクトップ音声へ適用します。
- 出力デバイスと全体音量の選択は、通常のPipeWireとWirePlumberの管理に保ちます。
- PipeWireグラフとの自動交渉、または44.1、48、96、192、384 kHzの指定周波数で
  DSPを計算します。
- 互換性重視のScalar、SIMD自動選択、CPU検証済みの命令セット別実装を選択できます。
- CLIコマンド1つで、ユーザーごとの設定または解除を行えます。
- GTKアプリケーションに実行状態を表示します。

### 対応システム

PipeTuneには、WirePlumberが管理するPipeWireデスクトップセッションと
systemdユーザーサービスが必要です。WirePlumber 0.4と0.5に対応します。
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

`setup`はsystemdユーザーサービスを再読み込み、有効化、再起動し、アクティブ状態を
確認してからPipeTune GTKアプリケーションをシステムトレイに常駐させます。
同時にWirePlumber 0.4互換ファイルを配置します。WirePlumber 0.5では、PipeTuneが
公開するsmart filterプロパティを直接使用します。

![System tray](./images/system-tray.png)

システムトレイアイコンをダブルクリックするか、あるいはメニューから"Open"を選択することで、PipeTune設定ウインドウを表示できます。

## PipeTune設定ウインドウ

PipeTune設定ウインドウは、左側のセクション分けされたPipeTuneステータスを常に表示し、
右側でProcessing、Rate、DSP、Advancedの設定を
切り替えます。

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

WirePlumberはPipeTuneを通常の再生経路へ挿入します。PipeWireが各アプリケーションの
音声を先にミックスし、PipeTuneがそのストリームを処理した後に、通常の全体音量と
出力デバイス選択が適用されます。

```mermaid
flowchart LR
    apps["アプリケーション<br/>個別ストリーム音量"]
    mix["PipeWireミックス"]
    tune["PipeTuneフィルタ<br/>EffeTune DSPまたはBypass"]
    output["PipeWire出力<br/>全体音量と既定経路"]
    device["選択された音声デバイス"]

    apps --> mix
    mix --> tune
    tune --> output
    output --> device
```

各アプリケーションの音量はミックス前に適用されます。BypassではEffeTuneを呼び出さず、
ミックス済みPCMをそのまま後段へ渡します。フィルタの後では、デスクトップの全体音量と
出力デバイス選択がPipeTuneを使用しない場合と同じように動作します。

## 全体音量と出力デバイス

スピーカー、ヘッドホン、HDMIなどの出力先は、GNOME設定またはデスクトップ環境の
通常のサウンド設定で選択します。同じ設定で、PipeTuneより後段の全体音量を調整します。
PipeTuneは出力先を保存せず、PipeWireの既定デバイスも変更しないため、GTK画面とCLIに
出力デバイス選択機能はありません。

通常の既定デバイスが変更またはホットプラグされると、WirePlumberがフィルタ出力を
再接続します。サウンド設定でPipeTuneデバイスを選ぶ操作は不要です。

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

PipeTune設定ウインドウのDSP sample rateとPipeWire enforcementから設定できます。
Automaticは、PipeWireグラフと交渉された周波数に追従します。固定値では、
44.1、48、96、192、384 kHzのいずれかをフィルタの両ノードとEffeTuneエンジンへ
要求します。状態表示には、実際のDSP周波数とグラフ周波数が表示されます。

Suggestは`node.rate`を提案として設定するため、PipeWireが異なるグラフ周波数を
選ぶ場合があります。Forceはさらに、フィルタ出力が動作している間、その固定周波数を
維持するようPipeWireへ要求します。どちらもPipeWire全体のグローバルクロック設定は
変更しません。
要求値を適用できない場合もPipeTuneは切断せず、実際に交渉されたグラフ周波数で
DSPを継続します。Forceの不成立はRate診断に表示されます。

CLIでは、同じ情報の表示と設定を次のコマンドで行えます。

```sh
pipetune rate list
pipetune rate get
pipetune rate set automatic
pipetune rate set 192000 force
```

`rate list`はAutomaticと5つの固定周波数を表示します。`rate get`は、設定済み
ポリシーと交渉された周波数を表示します。
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
- PCM周波数はAutomatic、PipeWire要求はSuggest
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

`unsetup`はGTKアプリケーションを終了し、ユーザーサービスを無効化・停止し、
WirePlumber 0.4互換ファイルを削除します。また、GTKアプリケーションが自動起動しない
ようユーザー用の自動起動マスクを作成します。起動時のプリセット選択は保持されます。
PipeTuneのアプリケーション設定も削除する場合は、`pipetune unsetup --purge`を
使用します。

独自のユーザー用自動起動エントリーをマスクする必要がある場合、`unsetup`は
PipeTune管理のバックアップとして保存します。後で`pipetune setup`を実行すると、
そのバックアップを上書きせずに復元します。パッケージの削除だけでは、
ユーザーごとの設定や自動起動オーバーライドは削除されません。

## ログ

デーモンのログは次のコマンドで確認できます。

```sh
journalctl --user -u pipetune.service
```

PipeTuneは既定の出力を所有しないため、停止またはクラッシュ後に出力デバイスを
復旧する操作は不要です。通常の出力選択と音量ポリシーはWirePlumberが維持します。

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
