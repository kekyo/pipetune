# EffeTune 2.8.0対応計画

## 目的と完了条件

公式v2.8.0 (`ed3d9666067a166d386f01630ed8b68185fcbaf4`) に更新し、既存機能を維持して、EffeTuneで測定・設定したCrosstalk Cancellationを再現する。

- クリーンReleaseビルド、Debug/Releaseの全体テストが成功する。
- CLI/GTKがEffeTune 2.8.0を表示し、バックエンドに99種類のDSPを登録する。
- 保存済みプリセットから測定JSONを自動解決し、4経路の補正を音声処理へ適用する。
- プリセット、参照する測定JSON、動作サンプルレートの変更を反映する。
- 測定の欠落・不正時は警告して当該DSPをスキップし、復旧後は自動反映する（ユーザー選択）。
- 外部ソースを変更せず、サブモジュール参照とPipeTune側の変更だけで実現する。
- ユーザーが実行中のprereq.shのイメージビルド完了後、既存マトリクスでマルチプラットフォーム検証を実施する。

測定UI、IndexedDB直接アクセス、ポータブル版の新規自動検出、他の外部アセットDSPは範囲外。

## 設計

### ファイル解決

EffeTune設定ディレクトリは既存方式をコア側で共用する。XDG_CONFIG_HOME/effetune、未設定ならHOME/.config/effetune、その配下measurement-backupsを参照する。GTKはeffetune_presets.jsonから選択プリセットを別ファイルへ書き出すため、書き出し先の親から相対解決しない。CLI・サービス・GTKと単独プリセットで同じ測定ストアを使い、追加UIやプリセット形式変更は不要。

ll/lr/rl/rrの参照を測定IDと::ch=チャンネルへ分解する。測定ID.jsonを構築中一度だけ読み、ポイント・IRレコード・チャンネルを対応付ける。Base64のfloat32 little-endianを復号する。単一点、耳ごとの同一セッション、重複禁止、サンプルレート・時間基準・波形の整合性は上流に合わせる。上流のID制約と64 MiB制限を使い、波形なしバックアップは利用不能とする。

### FIRとパイプライン

C++20で上流の時間整列、校正反映、リサンプリング、窓処理、正規化、平滑化、正則化逆補正、ゲイン・帯域制限、FIR生成を実装する。公式ドキュメント・ソース/APIコメントを確認し、JS実装をテストの比較基準にする。実行時Node.js依存は追加しない。

既存アセット転送を使用し、true-stereoのC11,C21,C12,C22順で送る。fdはタップ数/2。遅延はブロック遅延とモデル化遅延の合計。処理対象が2チャンネルでない場合は警告してスキップ。ファイルI/O・設計・アセット準備は音声スレッド外で行い、既存切替経路を使う。

パイプライン読み込みコンテキストに測定ストアを明示する。環境変数は呼出元で解決し、テストは一時ディレクトリを渡す。結果に参照JSON一覧（欠落も含む）を返す。inotify監視を複数ファイルへ拡張し、書込・atomic rename・削除・再作成・ディレクトリの遅延作成を検出する。切替時は監視対象を更新し、既存再読込経路を利用する。制御プロトコルと設定UIの必須項目は増やさない。

## 実装手順

各段階でテスト追加→全体実行RED→実装→全体GREENとし、ビルド可能な単位でコミットする。

1. 基準ビルド・全体テスト。新規5種類の登録・生成検証を追加して参照更新、94→99、3種のffp-contract=off追従、共有ライブラリ出力比較。CLI/GTK/全バックエンドと全体テスト成功で完了。`feat: update EffeTune dependency to 2.8.0`
2. 設定パス共通化・JSON解決・波形復号・上流検証と具体的欠落警告。実形式から4経路取得と異常系の全体テスト成功で完了。`feat: resolve EffeTune measurement backups`
3. C++設計・true-stereo生成・プリセット反映、必要な既存FIR共用化。補正音声、JS比較、経路順、強度・ゲイン・遅延検証成功で完了。`feat: support Crosstalk Cancellation processing`
4. 参照一覧・監視・欠落復旧・測定単独更新・プリセット切替・レート変更。GTK終了中も反映し音声スレッド内I/Oなしで完了。`feat: reload referenced measurement backups`
5. クリーンビルド、一時DESTDIR、パッケージ生成とインストール検証。README英日へ自動参照・前提・警告対処・上流URLを記述。完了条件照合と結果記録。`doc: describe EffeTune measurement integration`

## 検証

テストは常に全体実行。Debug/Release（テスト有効）の双方。JSON 2測定×2ch、ID不正、IR欠落、Base64不正、非有限、測定条件不一致。既知の人工応答をJS/C++双方へ渡し、上流の数値基準と公式DSP許容誤差で検証。測定と再生レート不一致、各レイテンシ、強度0/既定/100%、ゲイン、無効DSP/Section。スカラーと実行可能SIMDを比較し、非対応ISAはビルドと実行可否を区別する。監視テストはイベント同期し固定sleepに依存しない。CLI/GTK/パッケージも全体実行に含む。

## 上流参照

- https://github.com/Frieve-A/effetune/releases/tag/v2.8.0
- https://github.com/Frieve-A/effetune/blob/v2.8.0/docs/plugins/spatial.md
- https://github.com/Frieve-A/effetune/blob/v2.8.0/js/crosstalk-cancellation/design-core.js
- https://github.com/Frieve-A/effetune/blob/v2.8.0/js/measurement-store/client.js
- https://github.com/Frieve-A/effetune/blob/v2.8.0/features/measurement/dataStorage.js
- https://github.com/Frieve-A/effetune/blob/v2.8.0/electron/measurement-backup-ipc.cjs

## 実行記録

実装開始。ホストのPipeWire 1.0.5、samplerate 0.2.2を検出済み。prereq.sh実行中。
