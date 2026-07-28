# PipeTune

Linuxのデスクトップセッションの音声に、EffeTuneで構築したDSPプリセットを適用

![PipeTune](./images/pipetune-120.png)

----

[(English language is here)](./README.md)

## これは何?

PipeTuneは、Linuxのデスクトップセッションの音声に
[EffeTune](https://github.com/Frieve-A/effetune) で構築したDSPプリセットを適用します。

選択した音声出力デバイスの手前に、仮想PipeWireシンクを挿入して実現します。
また、デスクトップのシステムトレイに常駐するGTK 3コントロールアプリケーションを提供します。

### 機能

- `.effetune_preset`拡張子の標準形式および旧形式のEffeTuneプリセットファイルを
  読み込みます。
- 有効なEffeTuneネイティブDSPパイプラインをデスクトップ音声へ適用します。
- 既定の物理出力を追跡し、既定デバイスの変更やデバイスの着脱に追従します。
- デーモンを再起動せずにプリセットを変更できます。
- プリセットが未選択の場合は、安全なパススルーモードで起動します。
- CLIコマンド1つで、ユーザーごとの設定または解除を行えます。
- GTKアプリケーションに実行状態と音声エラーカウンターを表示します。
- PipeTuneの停止時に、既定の出力を物理出力へ戻します。

既定のストリーム形式は48 kHzステレオです。異なる形式を使用するアプリケーションの
ストリームはPipeWireが変換します。

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
そのディレクトリに対象のPipeTuneパッケージだけがあることを確認して、
次のコマンドでインストールします。

```sh
sudo apt install ./pipetune-*.deb
```

`apt`は、パッケージと必要なランタイム依存関係をインストールします。
パッケージには、PipeTuneデーモン、GTKアプリケーション、systemdユーザーサービス、
デスクトップエントリー、システムトレイの自動起動エントリー、アイコン、
設定例が含まれています。

インストールを確認します。

```sh
pipetune --version
pipetune-gtk --version
```

---

## 初期設定

`sudo`を付けず、デスクトップを使用する一般ユーザーとしてsetupを実行します。

```sh
pipetune setup
```

プリセットの指定は必須ではありません。新規インストール時はバイパスモードで起動し、
仮想シンクから物理出力へ音声を流しますが、DSP処理は行いません。
既に設定がある状態で`--preset`を省略してsetupを再実行した場合は、現在の起動設定を
保持します。

setupと同時にプリセットを選択する場合は、パスを指定します。

```sh
pipetune setup --preset /absolute/path/to/example.effetune_preset
```

明示的に指定したプリセットは、サービス設定を変更する前に検証されます。
setupはsystemdユーザーサービスを再読み込み、有効化、再起動し、active状態を確認して
からPipeTune GTKを非表示で起動します。互換性のあるシステムトレイがない場合は、
操作不能なバックグラウンドプロセスにならないようGTKウィンドウを表示します。

GTKアプリケーションでプリセットを選択すると、デーモンへの接続中はすぐに適用され、
以降のサービス起動で使用するために保存されます。互換性のあるシステムトレイが
利用可能な場合、ウィンドウを閉じると非表示になります。インストールされた
XDG自動起動エントリーにより、次回以降のデスクトップログイン時は非表示の状態で
起動します。

## ログと復旧

デーモンのログは次のコマンドで確認できます。

```sh
journalctl --user -u pipetune.service
```

手動で起動したPipeTuneプロセスが、既定の出力を物理出力へ戻さないまま終了した場合は、
次のコマンドで復旧します。

```sh
pipetune --restore-default
```

## 更新と削除

PipeTuneを更新するには、
[GitHub Releases](https://github.com/kekyo/pipetune/releases)から新しい対応パッケージを
ダウンロードし、同じ`sudo apt install ./pipetune-*.deb`コマンドでインストールします。
その後、デスクトップを使用する一般ユーザーとして`pipetune setup`を実行します。
`--preset`を省略すると、現在の選択を保持したまま更新後のサービスを再読み込みして
再起動します。

パッケージを削除する前に、デスクトップを使用する一般ユーザーとしてユーザーごとの
設定を解除します。

```sh
pipetune unsetup
sudo apt remove pipetune
```

`unsetup`はGTKアプリケーションを終了し、ユーザーサービスを無効化・停止し、
既定の出力を物理出力へ戻します。また、GTKアプリケーションが自動起動しないよう
ユーザー用の自動起動マスクを作成します。起動時のプリセット選択は保持されます。
PipeTuneのアプリケーション設定も削除する場合は、`pipetune unsetup --purge`を
使用します。

独自のユーザー用自動起動エントリーをマスクする必要がある場合、`unsetup`は
PipeTune管理のバックアップとして保存します。後で`pipetune setup`を実行すると、
そのバックアップを上書きせずに復元します。パッケージの削除だけでは、
ユーザーごとの設定や自動起動オーバーライドは削除されません。

## 関連情報

- [デーモンの操作方法と開発者向けドキュメント (英語)](pipetune/README.md)
- [GTKアプリケーションの動作 (英語)](pipetune-gtk/README.md)

## ライセンス

MIT
