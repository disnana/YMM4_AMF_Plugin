# コントリビューター向けガイド

このプロジェクトはDisnana Projectとして[tp-li-dev](https://github.com/tp-li-dev)が開発・保守しています。コードだけでなく、説明書の改善や動作報告も歓迎します。利用手順は[README](README.md)、ライセンスの扱いは[ライセンスの案内](docs/licensing.md)を参照してください。

## Issue・Pull Request

1. [既存のIssue](https://github.com/disnana/YMM4_AMF_Plugin/issues)を確認します。大きな機能追加やエンコード経路の変更は、実装前に目的・変更範囲・検証方法をIssueで相談してください。
2. リポジトリをforkし、作業用ブランチで変更します。PRの宛先は`disnana/YMM4_AMF_Plugin`の`master`です。
3. 関連するテストとドキュメントを更新し、下記の検証を実施します。
4. PRには変更理由、関連Issue、実行したコマンドと結果、実機環境、未検証事項を記載してください。GPUがない場合も貢献できますが、「GPU検証未実施」と明記してください。

変更は目的ごとに小さく分け、無関係な整形・依存更新を混ぜないでください。説明書だけの変更にGPUテストは不要です。設定名・既定値・コマンド・リンクが現行実装と合うことを確認してください。

YMM4本体、私有プロジェクト、有償素材、個人情報入りログ、ビルド成果物をコミットしないでください。再現用素材は配布可能なものに限定します。通常のPRでは`VERSION`を変更せず、リリース時にメンテナーが更新します。

## 開発環境

- Windows x64、PowerShell 7、Git
- Visual Studio 2022またはBuild ToolsのMSBuild、C++デスクトップ開発ワークロード、MSVC v143
- Windows SDK `10.0.26100.0`（ネイティブプロジェクトで指定）
- .NET 10 SDK
- [公式配布のYMM4 / YMM4 Lite](https://manjubox.net/ymm4/)の.NET 10対応版と、その本体フォルダー（v4.56.1.0で確認）
- 実機出力の検証にはAMF対応Radeonとドライバー、`ffmpeg` / `ffprobe`をPATHから実行できる環境

AMF SDKはGit submoduleとしてv1.5.2のcommit `eadd00804d5f7e5cd8c85d540073198312870776`に固定しています。AMF版のビルドにNVIDIA SDKは不要です。YMM4のDLLは手元の配置先を参照し、リポジトリや配布パッケージには含めません。

```powershell
git clone --recurse-submodules https://github.com/disnana/YMM4_AMF_Plugin.git
Set-Location YMM4_AMF_Plugin
```

PRを送る場合は、clone先を自分のforkへ読み替えてください。既にclone済みなら次でSDKを取得します。

```powershell
git submodule update --init --recursive
```

以下のコマンドはリポジトリのルートからPowerShell 7で実行します。YMM4の場所は実際の配置先に置き換えてください。普段の制作環境とは別の検証用YMM4を推奨します。

```powershell
$amfYmm4Directory = 'C:\path\to\YukkuriMovieMaker_v4_Lite'
```

## ビルドと配置

プラグインを含めてビルドします。

```powershell
.\scripts\Build.ps1 -Configuration Release -IncludePlugin -Ymm4Directory $amfYmm4Directory
```

ビルド後は`artifacts\bin`へ`AMFPlugin.dll`、`AmfNative.dll`、`RadeonBench.exe`が自動配置されます。このコマンドはYMM4のpluginフォルダーを変更しません。`-IncludePlugin`を省くと、ネイティブDLLとベンチツールだけをビルドします。

YMM4へ配置して試すときはYMM4を終了し、次を実行します。

```powershell
.\scripts\Deploy-Plugin.ps1 -Ymm4Directory $amfYmm4Directory -WhatIf
.\scripts\Deploy-Plugin.ps1 -Ymm4Directory $amfYmm4Directory
```

配置先は`<YMM4フォルダ>\user\plugin\AMFVideoWriterPlugin`です。既存の同名DLLと同梱説明書を上書きするため、必要なら事前に退避してください。`-WhatIf`でもビルドは実行されますが、YMM4へのコピーは行いません。

開発用の直接配置と一般配布は区別してください。一般配布には後述の`Package-Plugin.ps1`を使い、ライセンス表記を含む`.ymme`を作成します。

## 検証

### CPU側の契約テスト

```powershell
.\scripts\Run-Tests.ps1 -Suite Unit
git diff --check
```

`Unit`はReleaseのネイティブビルド、PowerShell構文、必須引数・不正プール枚数に対するCLIの拒否動作を検査します。すべてのエンコーダー処理を網羅する単体テストではなく、GPUでの出力成功も証明しません。

### GPUスモークテスト

```powershell
.\scripts\Run-Tests.ps1 -Suite GpuSmoke
```

H.264 / HEVCを各640×360・60fps・120フレーム、AAC音声付きで出力します。GPUや`ffmpeg` / `ffprobe`がなければ成功扱いにはしません。結果は`artifacts\runs\gpu-smoke`へ保存されます。

検証する項目は、MP4の正常終了、映像フレーム数、全デコード、画像内の16-bitフレームマーカーの順序、BT.709 / limited rangeメタデータ、音声形式、映像と音声のduration差（50ms以内）です。色の画素値、VMAF、音声マーカーの相互相関は検証対象に含まれていません。

### YMM4実プロジェクトでの確認

エンコード処理や設定UIを変えた場合は、検証用YMM4でプラグイン認識、設定、音声付きMP4出力、再生と終了処理を確認してください。使ったYMM4の版、GPU、ドライバー、解像度、fps、コーデック、出力設定、プロジェクトの条件を記録します。

CI、スタンドアロンGPUテスト、YMM4実プロジェクトの確認は、それぞれ別の検証です。一つの成功を他の成功として報告しないでください。

### 速度の比較

```powershell
.\scripts\Run-Benchmarks.ps1 -Config .\bench\configs\comparison.json
```

この設定はH.264・1920×1080・60fps・900フレームを、プール4 / 6 / 8枚で各5回測定し、別セッションでウォームアップします。結果のJSON / CSVと出力は`artifacts\runs`以下に保存されます。

- 比較は同じPC・GPU・ドライバー・入力・設定で行い、反復数と中央値、ばらつきを残してください。
- 正常Drain、MP4 close、全フレームのデコード確認後の`completed_fps`を使います。単なる入力受付速度は採用判断に使いません。
- YMM4全体の高速化を主張する場合は、同じ実プロジェクトの総出力時間も比較してください。単体ベンチのfpsをYMM4のfpsとして扱いません。
- 速度だけでなく、画質、音声、フレーム欠落、メモリ、失敗時の後始末を確認します。未測定の項目は残してください。

YMM4 CLI向けの`Run-YmmBench.ps1`は、`executable`、`arguments`配列、任意の`working_directory`を持つローカルJSONを実行するアダプターです。対象版で確認した引数だけを使用してください。共通のCLI引数や完成済みプロファイルは提供していません。私有パスを含むプロファイルは`artifacts`以下などのGit管理外に置きます。

過去の結果は[実測記録](docs/benchmarks/2026-09-14-rx6800xt.md)を参照してください。

## エンコード経路を変更する前に

まず[アーキテクチャ](docs/architecture.md)を確認してください。現在は、YMM4から借用したD3D11テクスチャを呼び出し中に所有テクスチャへコピーする経路（P1）のみを採用しています。ホスト所有テクスチャを非同期処理へ渡さず、対応する出力を確認するまでスロットを再利用しないことが前提です。

実験したVideoProcessor経路（P2）は同一YMM4プロジェクトで遅くなったため廃止しました。新しい高速化は、再現できる改善と正しい出力を確認してから採用します。v0.1.1では既存の入力経路を維持しています。

今後の検証候補は、HEVCのYMM4 E2E、画素値の色差・VMAF、音声マーカー照合、標準出力との比較、長時間反復・VRAM使用量、キャンセルAPIとその試験、device removal、複数GPU・ドライバー互換性、コード署名です。現在の測定だけでAMFのハードウェア性能上限に達したとは判断できません。

## 配布用パッケージ

[YMM4公式の配布方式](https://manjubox.net/ymm4/faq/plugin/how_to_make/)に従い、プラグイン用サブフォルダーをZIP化して拡張子を`.ymme`にします。

```powershell
.\scripts\Package-Plugin.ps1 -Configuration Release -Ymm4Directory $amfYmm4Directory
```

`artifacts\release`に`YMM4-Radeon-AMF-v<version>.ymme`と同名の`.ymme.sha256`を生成します。`VERSION`を読み、ビルドと内容検査も行います。パッケージの`AMFVideoWriterPlugin`フォルダーには次の6ファイルだけを含めます。

- `AMFPlugin.dll`、`AmfNative.dll`
- `README.md`、`VERSION`
- `LICENSE`、`THIRD_PARTY_NOTICES.txt`

YMM4・SharpGen・Vorticeの参照DLL、AMDの`amfrt64.dll`、ベンチツール、FFmpegは含めません。一般利用者にビルド出力フォルダー全体を渡さないでください。

スクリプトは`artifacts\ymme-staging`と出力ディレクトリを作り直します。既定の`artifacts\release`や、`-OutputDirectory`に指定するディレクトリへ保管用ファイルを置かないでください。カスタム出力先も`artifacts`の子ディレクトリに限定されます。

## CIとリリース（メンテナー向け）

[GitHub Actions](.github/workflows/ci-release.yml)はPR、`master`へのpush、手動実行で、公式YMM4 Liteの参照DLLを一時取得し、CPU側の契約テスト、管理・ネイティブビルド、`.ymme`の作成を行います。通常のGitHub-hosted runnerではRadeonの実機検証を実施しません。

リリース手順は次のとおりです。

1. 対象変更のテスト結果、既知の制限、ライセンス同梱を確認します。
2. `VERSION`を既存の最新`v*` SemVerタグより大きい版へ更新し、`docs/release-notes/v<version>.md`へ変更点と検証範囲を記録します。
3. パッケージを作成・確認し、変更を`master`へ取り込みます。
4. pushで起動したCIが成功すると、条件を満たす新バージョンのタグとGitHub Releaseを作り、`.ymme`とSHA-256を添付します。
5. 公開された添付ファイルをダウンロードし、SHA-256と内容を確認します。新規インストールや更新を実機で試したかも、別途記録してください。

リリース判定は`master`へのpush時だけです。現在の`VERSION`と同じタグがあれば再リリースしません。同じタグがなく、バージョンが既存の最新SemVerタグ以下ならエラーになります。`VERSION`の差分があるだけでリリースする仕組みではありません。PRや手動実行はビルド検証のみです。

Release本文は現在GitHubの自動生成ノートです。`docs/release-notes`の本文を自動転載する処理はありません。既存の公開タグや添付ファイルは差し替えず、配布物を修正するときは新しい版を使用してください。

## 貢献コードのライセンス

このプロジェクトへ取り込む自作コードは、[MIT License](LICENSE)で提供できるものを送ってください。第三者コードを持ち込む場合は、出典・参照commit・ライセンス・必要な著作権表示をPRに示し、必要に応じて[THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt)を更新します。既存の原著作者の表示は削除しないでください。
