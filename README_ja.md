# PipeTune

----

[(English language is here)](https://github.com/kekyo/scheme-cd-ripper)

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
- GTKアプリケーションに実行状態と音声エラーカウンターを表示します。
- StatusNotifierItemまたは互換性のための`GtkStatusIcon`フォールバックを使用し、
  `pipetune-gtk`をシステムトレイで実行します。
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

ユーザーごとに必要なサービス設定を作成します。

```sh
install -d -m 700 "$HOME/.config/pipetune"
install -m 600 \
  /usr/share/doc/pipetune/environment.example \
  "$HOME/.config/pipetune/environment"
```

`~/.config/pipetune/environment`を編集し、EffeTuneプリセットの絶対パスを
設定します。

```text
PIPETUNE_PRESET=/home/user/presets/example.effetune_preset
```

パスに空白が含まれる場合は、値を引用符で囲みます。

```text
PIPETUNE_PRESET="/home/user/My Presets/example.effetune_preset"
```

現在のユーザー用にPipeTuneを有効化して起動します。

```sh
systemctl --user daemon-reload
systemctl --user enable --now pipetune.service
systemctl --user status pipetune.service
```

デスクトップのアプリケーションメニューまたはターミナルからPipeTune GTKを
起動します。

```sh
pipetune-gtk
```

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

PipeTuneを削除するには、次のコマンドを実行します。

```sh
systemctl --user disable --now pipetune.service
sudo apt remove pipetune
systemctl --user daemon-reload
```

パッケージを削除しても、`~/.config/pipetune`内の設定ファイルは削除されません。

## 関連情報

- [デーモンの操作方法と開発者向けドキュメント](pipetune/README.md)
- [GTKアプリケーションの動作](pipetune-gtk/README.md)

## ライセンス

MIT
