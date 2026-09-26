# 出力回収待機の実験: 2026-09-17

これは待機最適化単独の測定記録です。本文中の「最終ビルド」はこの実験時点の版を指します。後から追加した[MF方式の入力再利用](2026-09-18-mf-input-recycling.md)の効果とは分けてください。

## 結論

画質・圧縮設定を変えずに改善できる待ち時間を特定し、既定OFFの「出力回収の待機を最適化（実験）」を追加しました。640×360の合成素材では約4倍の改善を反復確認しました。一方、1920×1080では小幅な改善と逆転の両方があり、一貫した全体速度の向上は確認できていません。YMM4実プロジェクトの高速化率ではありません。

MFの歴史的な高速化は、圧縮形式を新しくしたというより、CPU読み戻しを含む受け渡しからGPU上の受け渡しへ移し、描画用とエンコーダー所有のテクスチャの寿命を分離したことが主要な差分です。新しい標準MFもGPUコピー2回を含み、完全なゼロコピーではありません。現在のAMFにも所有プールと非同期回収があるため、この説明だけで現在のMF対AMFの速度差を特定したことにはなりません。

MF品質固定100とAMFの同等画質比較は未完了です。[比較条件・ツール](../export-quality-comparison.md)を別途用意しています。

## 変更内容

- OFFでは従来の`QueryOutput`と、空の場合の`std::this_thread::sleep_for(1ms)`を維持。
- ONでは対応能力を確認し、初期化前にAMFの`QueryTimeout=10ms`を設定・読み戻し。これは毎フレーム10msを追加するsleepではない。
- それでも1ms未満で空の結果が返る場合に、1msの高精度waitable timerを使用。タイマーを使えない場合は従来sleepへ戻る。
- 画質、ビットレート、GOP/B-frame、色変換、GPUコピー回数、プール枚数は変更しない。
- `timeBeginPeriod`、ビジーループ、毎フレームFlushは追加しない。詳細は[機能仕様](../output-wait-optimization.md)。

この環境では、従来の1ms sleepの実測平均が約14〜15msでした。出力を回収するまで所有スロットが戻らないため、小さく軽い映像ではプール枯渇待ちになっていました。QueryTimeoutを設定するだけではこの条件の即時復帰を解消できず、高精度タイマーの補助も必要でした。

## 環境と計測範囲

- AMD Radeon RX 6800 XT、AMF runtime 1.4.37.0、使用SDK 1.5.2.0。
- Windows build 26200、ローカルのYMM4 4.56.1.1参照でビルド。
- RadeonBenchの合成BGRAフレーム、60fps、AAC音声あり、所有プール6枚。
- 特記しない限りBalanced、VBR 12,000kbps、peak 14,400kbps、デバッグログOFF。
- 同じバイナリで別ウォームアップ後にOFF→ON / ON→OFFを交互に反復。
- 計時は合成フレーム生成・GPU upload・AMF・AAC・MP4終了処理を含み、エンコーダー初期化は含まない。YMM4の素材読み込み・描画は含まない。
- 各出力の全デコード、フレーム数・順序マーカー、BT.709 limitedメタデータ、音声長を計時の外で検証。

## 反復結果

| 条件 | コーデック | 反復ペア | OFF中央値 | ON中央値 | OFF / ON |
| --- | --- | ---: | ---: | ---: | ---: |
| 640×360・1200枚・profile ON | H.264 | 4 | 3132.25ms | 797.62ms | 3.927倍 |
| 同上 | HEVC | 4 | 3120.89ms | 779.35ms | 4.004倍 |
| 1920×1080・900枚・profile ON | H.264 | 2 | 3313.65ms | 3185.82ms | 1.040倍 |
| 同上 | HEVC | 2 | 3145.84ms | 3235.90ms | 0.972倍（低下） |
| 1920×1080・900枚・profile OFF・先行ビルド | H.264 | 4 | 3359.97ms | 3238.10ms | 1.038倍 |
| 同上 | HEVC | 4 | 3332.16ms | 3204.01ms | 1.040倍 |
| 1920×1080・900枚・高品質・profile ON | H.264 | 2 | 5347.16ms | 5341.85ms | 1.001倍（ほぼ同じ） |
| 同上 | HEVC | 2 | 3218.82ms | 4003.07ms | 0.804倍（低下） |

640×360ではH.264の全体fps中央値が383.11→1504.51、HEVCが384.51→1539.76でした。この合成・低解像度の倍率を、YMM4の1080pプロジェクトへ外挿しないでください。

1080p BalancedではOFF/ONの分布が重なり、ペア単位でも逆転しました。profile OFFの先行ビルドは最終版と待機ループが同じですが、その後に初期化時の診断とカウンター読み取り順を修正したため、最終バイナリの測定とは分けています。4%程度の差を安定した改善率としては採用しません。

高品質HEVCのONは4896.75ms / 3109.38msとばらつきました。遅い回は`mp4_finalize`に1724.40ms（速い回0.72ms）を記録しています。回収待ちとは異なる終了処理で止まったと切り分けられますが、その内部原因は未特定です。都合の悪い回を除外せず、終了処理込みの中央値を載せています。高品質H.264は待機改善後もほぼ同速でした。

### どこで止まっていたか

640×360 H.264・1200フレームの各反復で:

- OFF: プール待ち合計2314〜2362ms、1回の空出力待機平均14.30〜14.43ms。
- ON: プール待ち合計58〜109ms、補助タイマー待機平均1.24〜1.30ms。
- 平均スロット滞在時間は14.31〜14.42msから3.15〜3.34msへ短縮。

1080p Balanced H.264・900フレームでは、OFFのプール待ち合計が既に0.36〜0.51msしかありませんでした。平均スロット滞在時間は約11〜12msから約4.6〜4.8msに減りましたが、プール枯渇が全体の律速ではなく、大きな速度向上に結び付いていません。

`QueryOutput`のCPU呼び出し時間は、ONではAMF内の出力待ちを含みます。GPUの純粋なエンコード時間ではありません。並列に動く区間の時間を単純加算して1フレームの総時間とは解釈しないでください。今回、GPU timestampによるcopy/convertの純GPU時間やMF内部キューを新たに測定したわけではありません。

## 品質・正しさ・停止

- 最終ビルドでnative 8条件 + managed 16条件の計24出力を検証。H.264/HEVC、profile ON/OFF、managed GPU直渡しON/OFF、待機最適化ON/OFFを含む。
- 最終Balancedの1080p/360p各コーデックの最初のOFF/ONペア（計4ペア）を追加比較。デコードした映像フレームは全て一致（900枚または1200枚）、デコード後の音声SHA-256も一致。
- これは比較した4ペアでの同一性であり、元映像に対する可逆圧縮や、MF以上の画質の証明ではない。
- 最適化ONのDestroyを0/1/8フレーム投入 × H.264/HEVCの6条件で確認。約2〜18msで停止。完了前Destroyのテスト出力は正常完了した動画としては扱わない。
- CPU側92チェック、AMF機能判定・設定拒否・rollback等9シナリオ、既存のprofilingテストが成功。画質比較ツールはCPU契約18チェック、ffmpeg統合込み24チェックが成功。
- YMM4 GUIでの新オプションの出力・キャンセルE2E、長時間VRAM推移、device lost、別GPU/ドライバーは未検証。
- ベンチの`process_peak_working_set_bytes`は取得できずnull。RAM/VRAM改善の根拠にはしない。GPU使用率やGC pauseもこの独立試験では計測していない。

## 再現とローカル証拠

```powershell
./scripts/Run-Tests.ps1 -Suite All -Ymm4Directory 'D:/YukkuriMovieMaker4/YMM4修正版'
./scripts/Run-OutputWaitBench.ps1 -Pairs 4 -Frames 900
./scripts/Run-OutputWaitBench.ps1 -Pairs 2 -Frames 900 -Profile
./scripts/Run-OutputWaitBench.ps1 -Width 640 -Height 360 -Pairs 4 -Frames 1200 -Profile
./scripts/Run-OutputWaitBench.ps1 -Pairs 2 -Frames 900 -Quality quality -Profile
```

待機単独の測定時ビルドのSHA-256（現在の開発版とは異なります）:

```text
AMFPlugin.dll   EF6173D18AE7609172768887DD716E3CF13D86275ED61824CDA4CE62EB74A9B6
AmfNative.dll   6EA5855F4DF911FB2024A398E6C75EC0C653380D1AD3D637FAFDACCE32B6CAE9
RadeonBench.exe 0B26761ACE69BAF904FEB8054D1E4A9E9D8C174BDCF2593C402A5FE343A16B6E
```

ローカルの詳細JSON（`artifacts`はGit管理対象外）:

- `artifacts/runs/output-wait-20260917-232401-7bb414bb/summary.json`: 最終360p、4ペア。
- `artifacts/runs/output-wait-20260917-232220-8b9c7f58/summary.json`: 最終1080p・profile ON、2ペア。
- `artifacts/runs/output-wait-20260917-231522-f01c75f3/summary.json`: 先行1080p・profile OFF、4ペア。native SHA `B50B5A2B01F231CF41BA041B4455A5A38D574EE96DD36F452735C5ED6395EFE0`。
- `artifacts/runs/output-wait-20260917-234211-89e08721/summary.json`: 最終1080p・高品質・profile ON、2ペア。
- `artifacts/runs/diagnostic-tests-3184185f28804606b1381cc1b54820c9`: 最新CPU契約テスト。
- `artifacts/runs/gpu-smoke/managed-14c5166728e94b1f99c53d53eb4c9a92`: managed実GPU試験。
- `artifacts/runs/quality-tests-4593b334710a43998eed8f0dc03bc061`: 画質比較ツール用fixture（実MF/AMF比較ではない）。

開発版DLLは`artifacts/bin`へ配置済み。YMM4へのインストール、コミット、push、リリースは行っていません。公開v0.2.0とは分けて試してください。
