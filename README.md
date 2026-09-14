# YMM4 Radeon (AMF) 動画出力プラグイン

[![Build and release](https://github.com/disnana/YMM4_AMF_Plugin/actions/workflows/ci-release.yml/badge.svg)](https://github.com/disnana/YMM4_AMF_Plugin/actions/workflows/ci-release.yml)

YukkuriMovieMaker 4（YMM4）のD3D11描画結果を、AMD Advanced Media Framework（AMF）へ渡してH.264 / HEVCのMP4を書き出すWindows向けプラグインです。

**Disnana Project / Developer: [tp-li-dev](https://github.com/tp-li-dev)**

現在は **開発版** です。現在のバージョンは[`VERSION`](VERSION)を参照してください。AMFネイティブコア、所有テクスチャプール、非同期出力取得、AAC音声、MP4 mux、スタンドアロン実機ベンチまで実装されています。AMD Radeon RX 6800 XT環境では、YMM4によるプラグイン認識、設定UI、H.264の実書き出しと再生まで確認済みです。

## 実装済み

- YMM4の`ID2D1Bitmap1`から`ID3D11Texture2D`を取得する`IVideoFileWriter2`接続
- 借用テクスチャを保持せず、4 / 6 / 8枚の所有BGRA/RGBAテクスチャへ`CopyResource`する安全なP1経路
- 同じD3D11 deviceを使うAMF H.264 / HEVC encoder（AMF EFCによるRGB入力変換）
- `SubmitInput`と専用`QueryOutput`スレッドによる非同期処理
- 入力surfaceのスロットIDが出力へ戻ったことを確認してから再利用する所有権管理
- Media Foundation AACとMP4 mux、正常Drain、全出力フレーム数の整合確認
- `RadeonBench`によるprobe / 実機スモーク / JSON結果
- 明示実行のビルド、GPUスモーク、ベンチ、YMM4 CLI adapterスクリプト
- `amfrt64.dll`をSystem32からだけ読み込むDLL検索制限

## 未実装・未検証

- D3D11 VideoProcessorで所有NV12へ変換するP2経路
- P1/P2の比較によるAuto選択
- 画像内frame markerのdecode後自動照合、色差、VMAF、音声marker相互相関
- BT.709 / limited rangeの明示設定とcontainer metadataの自動検証
- YMM4実プロジェクトでのL3比較と、標準出力に対する高速化率
- キャンセルAPI、device removalの実機試験、長時間反復・VRAM plateau試験
- コード署名、複数GPU・複数ドライバーでの互換性試験

未実測の高速化率は主張しません。スタンドアロンの`completed_fps`はYMM4全体の書き出し速度ではありません。

## インストール

1. [Releases](https://github.com/disnana/YMM4_AMF_Plugin/releases)から最新の`YMM4-Radeon-AMF-v*.ymme`をダウンロードする
2. YMM4を終了する
3. `.ymme`をダブルクリックし、YMM4のインストーラーに従う
4. YMM4を起動し、動画出力で「Radeon (AMF) プラグイン出力」を選ぶ

インストーラーが開かない場合は、YMM4の「ヘルプ」→「YMM4用拡張子の関連付け」から関連付けを登録してください。

## 必要環境

- Windows 11 x64
- AMF対応AMD Radeon GPUと対応Radeonドライバー
- Visual Studio 2022（Desktop development with C++）
- Windows 11 SDK 10.0.26100.0
- .NET 10 SDK（YMM4プラグインをビルドする場合）
- YMM4本体のDLL（リポジトリへは含めません）
- ffmpeg / ffprobe（完成出力の全decode検証。エンコード自体には不要）

AMF SDKはsubmoduleとしてv1.5.2のcommit `eadd00804d5f7e5cd8c85d540073198312870776`に固定しています。

```powershell
git clone --recurse-submodules https://github.com/disnana/YMM4_AMF_Plugin.git
```

既にclone済みの場合:

```powershell
git submodule update --init --recursive
```

## ビルドと検証

通常のビルドはYMM4のpluginフォルダーを変更しません。

```powershell
.\scripts\Build.ps1 -Configuration Release
.\scripts\Run-Tests.ps1 -Suite Unit
.\scripts\Run-Tests.ps1 -Suite GpuSmoke
```

`GpuSmoke`はH.264 / HEVCを各120フレーム、AAC音声付きで作成します。ffmpeg / ffprobeがPATHにある場合は、映像フレーム数と全decodeも検証します。GPUや外部ツールがない状態を成功へ読み替えません。

固定configの反復ベンチ:

```powershell
.\scripts\Run-Benchmarks.ps1 -Config .\bench\configs\comparison.json
```

結果は`artifacts/runs/`へJSON / CSVで保存されます。

## 配布パッケージと自動リリース

YMM4公式サンプルの配布方式に合わせ、プラグイン用サブフォルダをZIP化して拡張子を`.ymme`にしています。YMM4本体のDLLはパッケージへ含めません。

ローカルで配布物を作成する場合:

```powershell
.\scripts\Package-Plugin.ps1 -Configuration Release -Ymm4Directory 'C:\path\to\YukkuriMovieMaker_v4'
```

次の2ファイルが`artifacts/release/`へ生成されます。

- `YMM4-Radeon-AMF-v<version>.ymme`
- `YMM4-Radeon-AMF-v<version>.ymme.sha256`

GitHub Actionsはpushとpull requestのたびに、公式配布元のYMM4 Liteをビルド参照として一時取得し、ネイティブDLL、管理DLL、`.ymme`の生成を検証します。`master`へのpushで`VERSION`が以前より大きいSemVerへ更新されていた場合に限り、`v<version>`タグとGitHub Releaseを自動作成して`.ymme`とSHA-256を添付します。`VERSION`が未変更ならReleaseは作らず、同値・巻き戻し・既存タグとの衝突はエラーにします。

## YMM4プラグインのビルドと配置

YMM4の配置を明示してビルドします。

```powershell
.\scripts\Build.ps1 -Configuration Release -IncludePlugin -Ymm4Directory 'C:\path\to\YukkuriMovieMaker_v4'
```

YMM4へ配置する操作は別スクリプトです。対象を確認するには最初に`-WhatIf`を利用できます。

```powershell
.\scripts\Deploy-Plugin.ps1 -Ymm4Directory 'C:\path\to\YukkuriMovieMaker_v4' -WhatIf
.\scripts\Deploy-Plugin.ps1 -Ymm4Directory 'C:\path\to\YukkuriMovieMaker_v4'
```

YMM4のCLI引数は推測しません。対象版で確認した引数配列をローカルprofile JSONへ保存し、次のadapterへ渡します。

```powershell
.\scripts\Run-YmmBench.ps1 -Profile .\bench\configs\ymm-local.json
```

## 設計上の安全境界

YMM4から受け取るテクスチャは次フレームで再利用される可能性があるため、AMFへ直接保持させません。`WriteVideo`の呼び出し中に同じdeviceのImmediateContextで所有textureへコピーし、以降はその所有textureだけを非同期処理します。AMF出力に伝播したslot IDを取得するまで、そのslotは再利用しません。

詳細は[docs/architecture.md](docs/architecture.md)を参照してください。

## 2026-09-14の開発環境スモーク

AMD Radeon RX 6800 XTで次を確認しました。

- H.264、640x360、60fps、120フレーム: MP4 close成功、ffprobeで120フレーム、ffmpeg全decodeエラーなし
- H.264 + AAC stereo 48kHz: 映像120フレーム、AAC stream、ffmpeg全decodeエラーなし
- HEVC、640x360、60fps、120フレーム: MP4 close成功、ffprobeで120フレーム、ffmpeg全decodeエラーなし
- YMM4で「Radeon (AMF) プラグイン出力」を認識し、H.264 MP4の実書き出しと再生に成功

これは短いstandaloneスモークと基本的なYMM4 E2Eです。画質同等性、長時間安定性、標準出力に対する速度優位を証明する結果ではありません。

## ライセンスと由来

本リポジトリはMIT Licenseです。元になった[YMM4_NVEncPlugin](https://github.com/tarutaru247/YMM4_NVEncPlugin)のMITコード（固定点`c7cf16114be09faec877e74e12df36715e1d5881`）から、YMM4接続、AAC、MP4 muxの構造を継承しています。AMF SDKもMIT Licenseですが、H.264 / HEVC / AAC等の標準に関する権利をAMDが付与するものではありません。詳細は`LICENSE`と`THIRD_PARTY_NOTICES.txt`を確認してください。

本AMF版はDisnana Projectとして[tp-li-dev](https://github.com/tp-li-dev)が開発・保守しています。原実装の著作権表示とMITライセンスは維持しています。
