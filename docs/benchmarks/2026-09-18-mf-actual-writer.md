# 標準 MF 実 DLL と AMF: 速度・画質・回収の切り分け

## 結論

**今回の合成入力で再現した約2倍の差は、GPU直渡しの有無だけでなく、圧縮プリセットの違いで大きく説明できた。** AMFを高品質から標準へ変えると 5.351 → 2.413 秒。逆に MF は映像品質100を維持したまま、エンコード速度50から0へ変えると 2.469 → 5.534 秒になった。

ただし **AMF 高品質 = MF 品質100ではない**。共通参照との PSNR / SSIM は合成入力では AMF が上、実動画の短い区間では MF が上だった。現在の高品質プリセットは、この2種類では標準に対する指標改善が小さかったが、すべての素材で「過剰品質」「同等画質」とは結論しない。

回収通知の導入単独ではほぼ同速。既存の出力待機改善は標準プリセットで多くの試行を約8%短縮したが、1回遅い試行も残った。MFのプールは135〜138面まで確保しており、6面に制限したAMFとの比較を「MFの回収だけが優れている」と解釈することもできない。

製品コード・YMM4設定・インストール済みDLLは今回変更していない。[隔離ハーネス](../../tests/MfAmfPathBench/README.md)と本レポートのみ追加。既存の未コミット変更は保持した。

## 対象と方法

- CPU: Ryzen 9 5900X、GPU: RX 6800 XT。GPU名は実デバイスから取得。
- Windows build 26200、.NET 10.0.12、AMD driver 32.0.21045.5002（2026-08-17）。
- YMM4本体: 4.56.1.1。`YukkuriMovieMaker.Plugin.FileSource.MediaFoundation.dll` の実 `MFVideoFileWriter` を独立apphostで生成。
- `YukkuriMovieMaker.Commons.GraphicsDevices` と同じD3Dデバイス生成経路を使用。専用の `user` を事前作成して設定移行を防ぎ、実YMM4のプロジェクトや設定は読み込まない。
- AMFは手元の `artifacts/bin` の実DLL。主測定は既存の待機改善・入力回収オプションOFF、プール6面。
- 合成入力: 1920×1080 / 60 fps、900フレーム、同一の128個の移動矩形と全フレーム順序マーカー。共通440 Hz / 48 kHzステレオ音声。
- MFはスクリーンショット相当の品質100、速度50、HW ON、CABAC ON、B要求2、MinQP0 / MaxQP50、AAC192 kbps / FFmpeg AAC ON。
- AMFはH.264、品質プリセットを比較。VBR target 12 Mbps / peak 14.4 Mbps。
- 各条件4回、順序を反転。MF/AMF/GPU試験を同時実行していない。検証と画質解析は計時外。
- `export_ms`: フレーム生成開始からwriterのDispose完了まで。MFのconstructorは別計測、AMFの遅延初期化は第1フレーム内に入るため、constructorとexportの合計も併記。
- MFT照会を供給終了とFinalizeの間に挟まない。診断中にキューだけが排出される計時バイアスを避けた。

**YMM4プロジェクト全体の書き出しベンチではない。** 素材読込・PSD・エフェクトが重い実プロジェクトで同じ倍率が出るとは限らない。

## 実際に選択された MF 経路

`IMFSinkWriterEx.GetTransformForStream` と `ICodecAPI.GetValue` から以下を取得した。

| 項目 | 観測結果 |
| --- | --- |
| MFT列 | VideoProcessor → AMDh264Encoder |
| vendor | VEN_1002（AMD） |
| 変換器入出力 | RGB32 → NV12 |
| エンコーダー入出力 | NV12 → H.264 |
| capability | D3D11-aware、エンコーダーはAsync |
| GPU経路 | `gpu_capability=true`, `gpuDirectFailed=false` |
| RateControl | 3（Quality） |
| Quality / QualityVsSpeed | 100 / 50。速度0試験では100 / 100 |
| GOP | 30（UI設定0でもwriterがHW用に30を要求） |
| B-frame要求 / MinQP / MaxQP / CABAC | 2 / 0 / 50 / true |

MFの「映像品質」と「エンコード速度」は別のプロパティ。品質100は圧縮探索の手間を最大にする指定ではない。`QualityVsSpeed` は0が速度寄り、100が品質寄りのトレードオフで、YMM4は速度スライダーを反転して渡す。[Microsoft: QualityVsSpeed](https://learn.microsoft.com/en-us/windows/win32/codecapi/avenccommonqualityvsspeed-property)、[Quality](https://learn.microsoft.com/en-us/windows/win32/codecapi/avenccommonquality-property)

Readbackが成功しても、要求がビットストリームにそのまま反映されるとは限らない。代表900フレームを実際に解析すると、MFは I=30 / P=870 / B=0、AMFは I=4 / P=896 / B=0。MFはHigh profile、AMFはMain profileだった。B=2のreadbackだけから「実際にBフレーム2枚で圧縮している」とは言えない。

標準MFの2回のGPUコピーとtracked-sample回収、PR #79の有界キュー・専用workerは引き続き別の実装として扱う。今回実行したのはPR版ではなく標準MF実DLL。[PR #79](https://github.com/manju-summoner/YukkuriMovieMaker.Plugin.Community/pull/79)、[既存の静的経路調査](2026-09-17-output-path.md)

## 速度: プリセットと経路

900フレーム、各4回。秒数は通常の中央値。FPSは各試行の完了FPSの中央値。

| 条件 | export中央値 | 範囲 | constructor+export中央値 | FPS |
| --- | ---: | ---: | ---: | ---: |
| MF GPU / 品質100 / 速度50 | 2.458秒 | 2.453–2.470 | 2.920秒 | 366.15 |
| MF CPU入力強制 / 同設定 | 3.059秒 | 2.676–3.536 | 3.537秒 | 294.94 |
| AMF GPU / 高品質 | 5.351秒 | 5.340–5.355 | 5.379秒 | 168.21 |
| AMF 直渡しOFF / 高品質 | 5.352秒 | 5.343–5.357 | 5.380秒 | 168.16 |
| AMF GPU / 標準 | 2.413秒 | 2.404–2.431 | 2.441秒 | 373.03 |
| AMF GPU / 高速 | 2.409秒 | 2.405–2.428 | 2.437秒 | 373.60 |
| MF GPU / 品質100 / 速度0 | 5.534秒 | 5.529–5.539 | 6.008秒 | 162.63 |
| 上記と交互測定したMF / 速度50 | 2.469秒 | 2.459–2.492 | 2.963秒 | 364.54 |

MF CPU入力試験は隔離writerの `gpuPathDisabled=true` を強制したもの。ハードウェアエンコードはONで、実環境で自然にフォールバックした結果ではない。CPU入力の分散が大きいため、改善率を最速1回から出さない。

AMF直渡しOFFはYMM4 v2相当のCPU-readable bitmap中間コピーを追加するが、AMF自身はそのD3D textureを使う。MF旧経路のCPU Map / full-frame memcpyと同義ではない。

CBRの要求targetをMF 12,000,256 / AMF 12,000,000 bpsに近づけても、MF 2.472秒 / AMF高品質5.344秒だった。ただし実出力はMF約12.003 Mbps、AMF約6.933 Mbpsであり、**実ビットレートまで同一にした比較ではない**。rate-controlモードの切替だけではこの速度差を消せなかった、という補助結果に留める。

## 1フレームのどこで待つか

主測定4試行×900回を合算した `WriteVideo` のCPU wall-time（ms）。フレームの描画、音声、Finalizeはこの列に含めない。

| 条件 | 平均 | p50 | p95 | p99 | 最大 |
| --- | ---: | ---: | ---: | ---: | ---: |
| MF GPU / 速度50 | 2.175 | 0.244 | 3.262 | 60.528 | 61.637 |
| MF CPU入力 | 3.213 | 3.212 | 3.968 | 4.457 | 11.594 |
| AMF GPU / 高品質 | 5.624 | 0.216 | 15.165 | 15.477 | 46.088 |
| AMF GPU / 標準 | 2.437 | 0.160 | 13.817 | 14.276 | 45.442 |
| MF GPU / 速度0 | 5.109 | 0.243 | 6.439 | 121.422 | 123.003 |

MFは多くの呼出を短時間で返す一方、まとまった待機がある。小さいp50だけを見て「常に1フレーム0.24msで完了する」と解釈してはいけない。Finalize中央値もMF速度50で約364ms、AMF高品質で約43ms残る。

AMF内部は別のprofile ON試験をQuality / Balanced / Balanced / Qualityの順に行った。代表値（ms）:

| 段階 | 高品質 | 標準 | 意味 |
| --- | ---: | ---: | --- |
| 所有texture空き待ち / frame | 約5.37 | 約2.22 | input→output回収までのbackpressure |
| CopyResource呼出 / frame | 約0.008 | 約0.008 | GPU実行時間ではなくコマンド投入時間 |
| SubmitInput呼出 / frame | 約0.203 | 約0.160 | pure GPU encode時間ではない |
| 圧縮bitstreamのmux / frame | 約0.023 | 約0.023 | この条件では支配的ではない |
| 音声Write / callback | 約0.127 | 約0.11 | GPU側の待機とは別 |

高品質では動画callback約5.64msの大半がプール待ち。入力リトライは0、accepted/completedは900/900。出力スレッドの従来sleepは1回約15msになっていた。**プール待ちにはencodeだけでなく出力回収待ちも含まれる**。各非同期・ネスト段階の合計をexport時間へ足してはいけない。

測定OFF時のmanaged allocationはAMF約0.394 MB/900f、MF GPU約4.866 MB/900f。最初の44試験は全てGen0/1/2の増分0。GC pauseの直接測定ではないが、少なくともその測定窓のGC発生を主因にする根拠はない。GPU使用率・Video Codec使用率・GPU timestamp・AMF内部の真のqueue depthは未測定。

## 回収通知と出力待機

AMF標準、プール6、画質/RC不変、各4回。今回の追加試験:

| 入力解放通知 | 出力待機改善 | 中央値 | 範囲 |
| --- | --- | ---: | ---: |
| OFF | OFF | 2.418秒 | 2.408–2.436 |
| ON | OFF | 2.410秒 | 2.403–2.422 |
| OFF | ON | 2.215秒 | 2.208–3.235 |
| ON | ON | 2.211秒 | 2.205–2.222 |

回収単独の差は約0.3%で、ばらつきと比べ小さい。待機改善は中央値では約8.4%短縮だが、OFF/ON群に3.235秒の遅い試行が1回ある。原因を測れていないため除外しない。両ONが常に安全・高速という長時間保証でもない。

第1巡の4条件は、復号した全900フレームのyuv420p SHA-256が完全一致した。この比較での改善は、画素を劣化させて得たものではない。

両ONの別profile試験では、入力解放900回、出力完了900回、無効イベント0、所有input peak6、未回収output peak7、上限12。これは公開API境界の未回収数であり、AMF内部のqueue depthではない。

MF GPU主試験の確保texture数は135/138/136/137、供給終了時in-flightは113〜120。BGRA面だけの単純計算で約1.04〜1.07 GiB（実VRAM量ではない）。AMF6面は約47.5 MiB。MFが多く溜めて並列実行を維持できる構造は確認できたが、**回収callback単独の効率の優劣は未分離**。これを真似てAMFプールを無制限に増やす判断はしない。

## 画質: 共通参照から実測

### 参照の作り方と限界

GPU描画済みBGRAをFFV1保存し、生BGRAと復号結果のSHA-256一致を確認。その共通参照をBT.709 matrix / limited 8-bit 4:2:0 / bilinear chroma / 伝達関数の数値変換なしで正規化し、QP0 H.264の無劣化保存・復号についても生420サンプルSHA-256一致を確認した。MF出力を正解扱いした比較ではない。

初回の参照生成では転送特性・原色メタデータがunknownのまま残り、比較スクリプトが拒否した。フレームにも`setparams`を付けて明示した参照で再試行し、契約検査を通した。失敗した参照・比較ログは成功結果に含めない。

PSNR/SSIMは高いほど共通参照に近い。ただし色変換・chroma sampling/rounding・圧縮の差が混在する。主観画質の絶対点数ではないし、BGRAから4:2:0への変換自体は不可逆。ここでの「無劣化」は保存codecが追加損失を与えないという意味。

### 合成900フレーム

| 条件 | PSNR dB | SSIM平均 | ファイルMB（音声込み） |
| --- | ---: | ---: | ---: |
| MF 品質100 / 速度50 | 44.398949 | 0.997116500 | 11.243 |
| MF 品質100 / 速度0 | 44.401956 | 0.997119064 | 11.179 |
| AMF 高品質 | 46.937374 | 0.998818504 | 13.374 |
| AMF 標準 | 46.930620 | 0.998814430 | 13.251 |

AMF高品質→標準で失うPSNRは約0.0068 dB。この合成入力では約2.22倍の処理速度差に対し、指標差は小さかった。ただしMFよりAMFのファイルが約18〜19%大きく、同じ容量での比較ではない。

### 提供shorts動画の30〜35秒: 300フレーム

元は既存MP4で、同じ復号フレームをBGRAとして両writerへ渡した再エンコード試験。対象はイラスト・字幕中心。元YMM4プロジェクトの初回出力、激しい動き、実写の細かなノイズについて一般化しない。復号/uploadが入るため、この群の実行時間はエンコーダー単体速度として扱わない。

| 条件 | PSNR dB | SSIM平均 | SSIM p05 | ファイルMB（音声込み） |
| --- | ---: | ---: | ---: | ---: |
| MF 品質100 / 速度50 | 53.759963 | 0.999076510 | 0.998738 | 1.972 |
| MF 品質100 / 速度0 | 53.751736 | 0.999076270 | 0.998743 | 1.960 |
| AMF 高品質 | 46.440892 | 0.997790003 | 0.997002 | 1.897 |
| AMF 標準 | 46.411827 | 0.997763117 | 0.996930 | 1.910 |

この区間のAMF高品質→標準のPSNR差は約0.0291 dB。MFとの差は約7.32 dBあるが、これをそのまま「見た目の品質が何倍」とは換算できない。代表フレームを並べて大きな破損は見られなかったが、正式な主観画質判定はしていない。

合成と実動画で順位が逆転している。**「高品質プリセットは高コストだが、この素材では標準との差が小さい」と「AMFがMF以上の画質である」は別の主張**。後者は確認できていない。MF100とAMF Qualityの対応表も作れない。

## 次の改善方針

設定は以下の3軸へ分けるのが妥当。現時点では設定画面の改修はしていない。

1. **画質・容量の目標**: 品質基準とbitrate/RCの選択。品質固定やQPを追加するなら実効値のreadbackと対応検査を付け、MFの100と同じ数値を安易に対応させない。
2. **圧縮処理の手間**: 現在のSpeed/Balanced/Quality preset。「高品質」という表示だけで目標品質と混同させず、速度・圧縮効率のトレードオフであると説明する。
3. **パイプライン**: GPU入力、pool上限、入力回収、出力待機。画質を変える設定と分離し、メモリ上限と遅い試行も記録する。

次の技術的切り分けは、AMF側の実効Profile / GOP / QP / color profile / range / RCをreadbackし、MFと可能な限り揃えること。そのうえで、同一NV12入力による比較を行えば、色変換と圧縮の寄与を分けやすい。現AMFは色profileの明示指定がなくSDKのAUTO/USAGE既定値に依存する部分があるが、実ランタイムの値をまだ取得しておらず、**色変換が間違っていると断定しない**。

高品質で待っている処理の中心は既にC++の先のAMF/ドライバー側。C#/C++/Rustの変更を最初に行うより、上の設定・入力条件の分離を優先する。MFの136面前後をそのまま採用することも優先しない。

## 検証結果と未検証

- 主測定・回収試験・実動画試験の65本 / 56,100フレームで、全デコードとフレーム順序、解像度、FPS、AAC/stereo/48kHz、音声長差50ms以下を確認。音声長差は約18.7msで、リップシンク検証ではない。
- BGRA参照2本と正規化420参照2本は生サンプルSHA-256照合済み。
- 画質比較は合成/実動画各3組。全フレームの時刻・数・色条件を検査してからPSNR/SSIMを算出。
- ハーネスRelease build: warning 0 / error 0。最終ファイルにもapphost隔離・timeout・依存物非配布を維持。
- 画質解析の既存CPU契約テスト18件成功、追加PowerShellの構文検査と新規ソースの空白検査成功、`git diff --check`成功。
- 未検証: 本物のYMM4プロジェクトでのAB/BA、GPU timestamp、GPU/Video Codec使用率、実VRAMピーク、数十分以上の出力、取消/device loss、動きの激しい実写/HDR/HEVC、主観品質、実効AMF encoder property全件。
- MFの内部型に依存するのは隔離ハーネスだけ。製品コードに新しいinternal/Harmony依存は追加していない。

## 再現データ

ローカルのgitignoredデータ:

- `artifacts/runs/mf-actual-20260918/summary.json`: 全条件の中央値・範囲・CPU段階分布。
- 同ディレクトリの `quality-comparisons.json`: 画質比較6組と個別レポートの場所。
- 同ディレクトリの `recovery-frame-hashes.json`: 第1巡の回収/待機4条件で復号画素が一致することの記録。
- `main-*`, `control-*`, `mf-speed-*`, `profile-*`, `recovery-*`, `real-*`: 実出力・要求値・readback・検証結果。
- `reference-*`: 共通参照、生フレーム照合、正規化の記録。
- `views/`: 実動画32.5秒位置の比較用PNG。人物・素材を含むので公開配布しない。

使用DLLのSHA-256:

```text
YukkuriMovieMaker.dll
407176E632418BA6DA5E671D6FE8C5138814E39FDA7460451B4E00E2EA4A8102
YukkuriMovieMaker.Plugin.FileSource.MediaFoundation.dll
669F9FD2490EEC545CD0E7E8EBC2B2090DD6DAACDB419EB7D2412DD3CA51C23F
AMFPlugin.dll
E76FEE982D4172806DAA3E049EAB16CAD29F6842877E251A5656FE396AD2A82A
AmfNative.dll
9581748349006072A95CBBFADE21FB6A75DDB5E366CDE1E79D5CF5A2F78A443B
```

ハーネスは調査中にMFT入出力型・プリセット・参照生成機能を追加したため、各 `run.json` に実行時の `bench_sha256` を保存している。製品DLLのハッシュは全試験で固定。コミット・push・releaseは今回行っていない。
