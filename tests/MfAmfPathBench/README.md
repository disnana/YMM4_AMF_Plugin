# MF / AMF 実 DLL の比較ハーネス

標準 MF の再実装ではなく、ローカル YMM4 の `MFVideoFileWriter` を独立プロセスで実行する検証用ツールです。AMF も `artifacts/bin` の既存 DLL を読み込みます。プラグインのインストール、YMM4 設定の保存、Harmony パッチ、本体出力コードの変更は行いません。

検証対象: YMM4 4.56.1.1、Windows x64 / .NET 10。MF の内部型・フィールドへの Reflection は **この隔離テストに限ります**。将来の YMM4 で型名・契約が変われば失敗する可能性があります。YMM4 DLL や逆コンパイルコードは配布しません。

## ビルドと通常比較

PowerShell 7、.NET 10 SDK、ローカル YMM4、`artifacts/bin/AMFPlugin.dll` / `AmfNative.dll`、PATH 上の `ffmpeg` / `ffprobe` が必要です。リポジトリのルートから実行します。

```powershell
dotnet build tests/MfAmfPathBench/MfAmfPathBench.csproj -c Release '-p:YMM4DirPath=D:\path\to\YMM4\'

tests/MfAmfPathBench/Run.ps1 -YmmDirectory 'D:\path\to\YMM4' `
  -RunDirectory artifacts/runs/mf-test-01 -Mode mf-gpu -Frames 900
tests/MfAmfPathBench/Run.ps1 -YmmDirectory 'D:\path\to\YMM4' `
  -RunDirectory artifacts/runs/amf-test-01 -Mode amf-gpu -AmfQuality Balanced -Frames 900
```

各出力先は未作成の専用ディレクトリを指定します。大量の並列実行はせず、AB/BA 等で順番を反転して反復してください。標準 MF の FFmpeg AAC が有効な場合、YMM4 の FFmpeg 実行依存ファイルだけを、このツールの生成済み `bin/Resources` 以下にコピーします。自動ダウンロードはしません。

`dotnet MfAmfPathBench.dll` での起動は禁止しています。YMM4 の `AppDirectories` が実行プロセスのパスに依存するため、必ず専用 apphost `.exe` を起動します。起動前にその直下の `user` を作り、既存 YMM4 のユーザーデータ移行を防止し、設定ディレクトリの隔離を確認します。

## 主な切り替え

| オプション | 内容 |
| --- | --- |
| `-Mode mf-gpu` | 標準 MF の通常 GPU 入力。GPU 成否・選択 MFT を記録 |
| `-Mode mf-cpu` | 隔離した writer の `gpuPathDisabled` を強制し CPU Map 経路を試験。ハードウェアエンコードは ON。自然に発生したフォールバックではない |
| `-Mode amf-gpu` | AMF の `IVideoFileWriter3` GPU 直渡し |
| `-Mode amf-cpu` | AMF の直渡し OFF 相当。CPU-readable bitmap への中間コピーはあるが、AMF が CPU Map / memcpy する意味ではない |
| `-AmfQuality Quality / Balanced / Speed` | 既存の AMF 高品質 / 標準 / 高速プリセット |
| `-MfSpeed 0..100` | MF のエンコード速度。`QualityVsSpeed = 100 - MfSpeed`。映像品質は 100 のまま |
| `-Rate quality / cbr / vbr` | `quality` は MF 品質100、AMF は既存 VBR。この二つは同じ RC ではない |
| `-Bitrate` | MF は値×1024、AMF は値×1000 bps。CBR 比較は MF 11719 / AMF 12000 等、実 readback も確認する |
| `-Pool 4..128` | AMF の所有テクスチャ要求数。増量時は画素量目安1 GiBで制限。MF のプールを変更するオプションではない |
| `-RecycleInput $true` | 既存 AMF の入力解放通知による回収オプション |
| `-OutputWait $true` | 既存 AMF の出力待機改善オプション |
| `-Profile $true` | AMF 内部の既存プロファイルも出力。測定 OFF 条件と混ぜない |
| `-MfFfmpegAudio $false` | MF 内蔵 AAC 経路。既定はスクリーンショットのノイズ対策 ON 相当の FFmpeg AAC |
| `-SourceFile ... -SourceStart 30` | 実動画の一部を共通 BGRA 入力として再エンコード。復号・upload が律速し得るため、このモードの FPS はエンコーダー単体速度として扱わない |

実動画の FPS は事前に `ffprobe` で確認し `-Fps` と揃えてください。指定解像度への変換は全経路で同一です。素材本来の音声ではなく共通の 440 Hz テスト音声を渡します。

## 出力と計時範囲

- `run.json`: DLL SHA-256、要求設定、実 MFT / メディアタイプ / ICodecAPI readback、フレーム単位の CPU 時間、p50 / p95 / p99 / 最大、割り当て、GC 回数。
- `validation.json`: 全デコード、全フレームの 16-bit マーカー順序、解像度・FPS・フレーム数、AAC / 音声長差。画素同等性・主観画質・リップシンクの保証ではありません。
- `output.mp4.amf_profile.json`: `-Profile` 時の AMF 内部計測。

`export_ms` は描画開始から最終 `Dispose` の完了までです。MF の constructor 初期化は別記、AMF の遅延初期化は最初の WriteVideo に含まれるので、`writer_create_plus_export_ms` も確認してください。MFT 診断はフレーム供給前だけに実施し、供給終了と Finalize の間に診断のための空白を挟みません。

すべて CPU wall-clock です。GPU 実処理時間ではありません。Render / native stages の非同期区間は重複するので合算できません。`process_cpu_ms` には別プロセスの FFmpeg の CPU 時間は含まれません。MF の API 成功だけで出力成功とはせず、ファイルを復号して検証します。

## 画質の共通参照

```powershell
tests/MfAmfPathBench/Run.ps1 -YmmDirectory 'D:\path\to\YMM4' `
  -RunDirectory artifacts/runs/ref-01 -Mode reference -Audio $false -Frames 900
tests/MfAmfPathBench/PrepareReference.ps1 -RunDirectory artifacts/runs/ref-01

scripts/Compare-ExportQuality.ps1 `
  -Reference artifacts/runs/ref-01/reference-420-tagged.mkv `
  -MfOutput artifacts/runs/mf-test-01/output.mp4 `
  -AmfOutput artifacts/runs/amf-test-01/output.mp4
```

参照は描画済み BGRA を FFV1 に無劣化保存し、復号 SHA-256 と取り込んだ生フレームの SHA-256 を照合します。比較用には BT.709 行列、limited 8-bit 4:2:0、bilinear chroma、伝達関数の数値変換なし、という明示的な規則で正規化します。正規化後の画素は QP0 H.264 へ無劣化保存し、ここも生サンプル SHA-256 を照合します。4:2:0 への変換自体は BGRA に対して不可逆です。

この FFmpeg では encoder の色オプションだけでは入力の unknown 属性が残ったため、`setparams` でフレーム属性も指定しています。比較スクリプトの色契約を緩めて通すことはしません。

PSNR / SSIM には GPU 色変換・chroma rounding と圧縮の双方の差が含まれます。MF 出力を正解参照にしていません。同一 writer のプリセット差と、異なる writer 間の画質差を区別してください。一つの短い素材・指標だけで「同等以上の画質」「過剰品質」を一般化できません。

実動画を使う場合は、参照生成と両エンコーダーで `-SourceFile` / `-SourceStart` / `-Frames` / サイズ / FPS を完全に揃えます。これは既存動画の再エンコードであって、元 YMM4 プロジェクトの初回出力との比較ではありません。

## 集計

投入スレッドと専用D3Dデバイスは `Run-PipelineSweep.ps1` で4条件（base / worker / device / both）を順序反転で比較し、`Summarize-PipelineSweep.ps1` で集計します。品質・pool・回収・出力待機は固定。`-RenderRepeats 16` は同フレーム内の矩形描画を16回行う合成負荷であり、実YMM4の再現ではありません。詳細は[パイプライン実験](../../docs/pipeline-experiments.md)を参照してください。

最終DLLでの反復、出力検証、画素照合、取消、メモリの結果は[2026-09-18のレポート](../../docs/benchmarks/2026-09-18-pipeline-isolation.md)にまとめています。`Hash-PipelineCases.ps1 -RunDirectory <root>`で第1巡の各画質・描画条件内の復号YUV420p一致を検査できます。比較対象の書き出し条件や品質を混ぜないでください。

入力満杯時の固定sleepと出力完了通知待機は、次の一連のコマンドで比較します。`Run-InputWaitBench.ps1`がタイミングを先に直列実行し、その後にプロファイルとデコード検証を行います。ゲーム、動画再生、別のGPUベンチを同時に動かした結果は使わず、新規ディレクトリで取り直してください。

```powershell
tests/MfAmfPathBench/Run-InputWaitBench.ps1 -YmmDirectory 'D:\path\to\YMM4' `
  -RunDirectory artifacts/runs/input-wait -Pools 16,32 -Rounds 4 -Frames 900
tests/MfAmfPathBench/Summarize-InputWaitBench.ps1 -RunDirectory artifacts/runs/input-wait
tests/MfAmfPathBench/Hash-InputWaitCases.ps1 -RunDirectory artifacts/runs/input-wait
```

同じpoolと品質の範囲で`fixed` / `signal` / `worker-fixed` / `worker-signal`を比較します。第1巡の全復号YUV420pハッシュ一致も要求します。実測結果は[入力満杯待機のレポート](../../docs/benchmarks/2026-09-18-input-full-wait.md)を参照してください。

テクスチャ枚数と回収方式の比較は、次のスクリプトで条件順を反転して繰り返せます。計時中には検証用デコードを並走させません。

```powershell
tests/MfAmfPathBench/Run-PoolSweep.ps1 -YmmDirectory 'D:\path\to\YMM4' `
  -RunDirectory artifacts/runs/pool-check -Rounds 4
tests/MfAmfPathBench/Run-PoolSweep.ps1 -YmmDirectory 'D:\path\to\YMM4' `
  -RunDirectory artifacts/runs/pool-check -ValidateOnly
tests/MfAmfPathBench/Summarize-PoolSweep.ps1 -RunDirectory artifacts/runs/pool-check
```

`legacy`（両方OFF）、`recycle`（入力解放回収のみON）、`wait`（出力待機最適化のみON）、`both`を比較します。回収ON時の未回収上限も実効枚数の2倍に変わるため、枚数とキュー上限を独立に変える実験ではありません。品質・ビットレートは固定です。

`-Profile`を指定した別実験では、DXGIのプロセス別GPUメモリ会計を開始前・初回・60フレームごと・Dispose後に記録します。[QueryVideoMemoryInfo](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiadapter3-queryvideomemoryinfo)のLocal / NonLocal使用量であり、全GPUの使用量、常時計測した真のピーク、純粋な所有textureのサイズではありません。計測負荷を含むため通常の速度結果には混ぜません。

`Run.ps1 -Profile $true -InspectCleanup`は、計時終了後に200 ms待機し、その隔離プロセス自身のImmediateContextに1回だけFlushしてさらに200 ms後のメモリ会計を記録します。遅延解放の調査専用で、製品の終了経路や毎フレーム処理を変えるものではありません。

`Hash-PoolCases.ps1 -RunDirectory <root>`は、検証済みの`balanced/01-*`と`quality/01-*`から復号YUV420p画素のSHA-256を取り、各プリセット内の一致を確認します。MFとの画質等価性検査ではありません。

`Summarize.ps1 -RunRoot artifacts/runs/<root>` は `main-*` / `control-*` / `mf-speed-*` / `profile-*` / `recovery-*` / `real-*` というケース名を集計します。各ケースの検証成功を要求し、プロファイル有無・素材・経路の異なる群は統合しません。生成された動画・DLL・ログは既存の `bin/` / `artifacts/` ignore 配下です。
