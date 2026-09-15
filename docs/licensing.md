# ライセンスと第三者コンポーネント

利用手順は[README](../README.md)、開発・配布手順は[CONTRIBUTING](../CONTRIBUTING.md)を参照してください。この文書は案内です。正式な許諾条件は[LICENSE](../LICENSE)と各コンポーネントのライセンス原文で確認してください。

## 本プラグインのMIT License

本プラグインのコードはMIT Licenseで公開しています。著作権表示は次のとおりです。

- Copyright (c) 2026 tarutaru247
- Copyright (c) 2026 tp-li-dev / Disnana

MIT Licenseは、利用、改変、複製、配布、販売などを許可します。一方、ソフトウェアの複製または重要な部分を配布するときは、著作権表示と許諾文を含める必要があります。また、ソフトウェアは無保証で提供されます。[このリポジトリのライセンス全文](../LICENSE)と[MIT Licenseの原文](https://opensource.org/license/mit)を参照してください。

### 動画制作で使う場合

本プラグインを使って動画を書き出すこと自体について、本プロジェクトは利用料や動画内・概要欄のクレジット表記を必須にしていません。紹介する場合は「YMM4 Radeon (AMF) 動画出力プラグイン / Disnana・tp-li-dev」と[リポジトリへのリンク](https://github.com/disnana/YMM4_AMF_Plugin)を使えますが、任意です。

これは、YMM4本体、音声合成、立ち絵、画像、楽曲などの条件を免除するものではありません。商用利用・収益化を行う際も、それぞれの提供元の条件を確認してください。YMM4については[公式配布ページの商用利用案内](https://manjubox.net/ymm4/)から確認できます。

### 改変・再配布する場合

本プラグインまたはそのコードを配布するときは、`LICENSE`と必要な第三者のライセンス・著作権表示を添付してください。既存の著作権者を自分の名前へ置き換えないでください。改変版は、利用者が本プロジェクトの配布物と区別できるよう変更内容や配布元を明記することを推奨します。

このリポジトリの配布スクリプトは、`.ymme`へ`LICENSE`と`THIRD_PARTY_NOTICES.txt`を同梱します。ビルドしたDLLだけを抜き出して再配布せず、必要な表記も一緒に渡してください。

## コードの由来と外部依存

| 対象 | 本プロジェクトでの利用 | 配布・ライセンスの扱い |
| --- | --- | --- |
| [YMM4_NVEncPlugin](https://github.com/tarutaru247/YMM4_NVEncPlugin) | commit `c7cf16114be09faec877e74e12df36715e1d5881`を基に、YMM4接続、AAC、MP4処理を継承 | MIT。原実装の著作権表示を`LICENSE`に維持しています。 |
| [AMD AMF SDK](https://github.com/GPUOpen-LibrariesAndSDKs/AMF/tree/eadd00804d5f7e5cd8c85d540073198312870776) | 固定したSDKの公開ヘッダーを使用して`AmfNative.dll`をビルド | MITと規格に関する注意書き。全文を`THIRD_PARTY_NOTICES.txt`へ収録しています。 |
| AMD AMFランタイム（`amfrt64.dll`） | Radeonドライバーに付属するランタイムを読み込み | 本パッケージには含めません。SDKのMIT表記をドライバー全体の許諾として扱わないでください。 |
| YMM4 / SharpGen / Vortice | インストール済みYMM4のDLLをビルド・実行時に参照 | 本パッケージには含めません。各提供元・YMM4同梱の表記を確認してください。 |
| Windows Media Foundation / Visual C++ランタイム | AAC音声処理やネイティブDLLの実行に使用 | 本パッケージには含めません。Microsoftの提供物です。 |
| FFmpeg / ffprobe | 開発時に生成動画を外部コマンドで検証 | 本パッケージには含めず、AMF版のエンコード処理からも呼び出しません。利用するツールの配布元の条件に従ってください。 |
| NVIDIA NVENC SDK | リポジトリに残る元のNVENCプロジェクト向け | AMF版のビルド・実行・配布には使用しません。元のNVENC版をビルドする場合は別途NVIDIAの条件を確認してください。 |

AMF SDKのリポジトリ全体には、サンプルや第三者ファイルも含まれます。`third_party/AMF`以下のすべてを本プラグインのMITだけで再配布できる、という意味ではありません。本パッケージはSDKリポジトリやAMFのFFmpegコンポーネントを丸ごと同梱しません。

## H.264 / HEVC / AACなどの規格について

AMF SDKのライセンスには、AMDがメディア規格に関する特許等の権利を許諾するものではない旨が記載されています。本プラグインのMIT Licenseも、第三者のコーデックに関する権利や費用を一括して解決するものではありません。

具体的な配布・商用利用に必要な権利は、利用形態に応じて確認してください。AMFの注意書きは[THIRD_PARTY_NOTICES.txt](../THIRD_PARTY_NOTICES.txt)および[固定SDKのLICENSE.txt](https://github.com/GPUOpen-LibrariesAndSDKs/AMF/blob/eadd00804d5f7e5cd8c85d540073198312870776/LICENSE.txt)で確認できます。
