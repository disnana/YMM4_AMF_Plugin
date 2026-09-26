# MF方式の入力再利用: 2026-09-18

## 結論

PR #79と標準MFの「入力sampleの解放通知でtextureをプールへ戻す」仕組みを、AMFの公開surface observerでC++側へ取り込みました。出力設定の「MF方式：入力解放通知でテクスチャを再利用（実験）」で切り替えられます。[コード経路・PRとの対応・使い方](../mf-input-recycling.md)を参照してください。

**この変更単独では、一貫した速度向上を確認できませんでした。常用はOFFのままを推奨します。** H.264高品質はほぼ同速。HEVCは速い回と遅い回があり、profile ONの4往復では中央値が低下しました。入力解放通知が圧縮済み出力の回収に近く、先行再利用で削れる余地が小さいことを実測しました。MFと同等以上の品質・速度を達成したという結果ではありません。

別途測定した[AMF出力待機の最適化](2026-09-17-output-wait.md)とは異なる実験です。そちらの低解像度での約4倍という結果を、今回のMF方式の効果として扱いません。

## 条件

- RX 6800 XT、AMF runtime 1.4.37.0 / SDK 1.5.2.0、Windows build 26200。
- 1920×1080、60fps、900フレームの同じ合成BGRA映像とAAC音声。
- 両側ともQuality preset、VBR 12,000kbps、peak 14,400kbps。
- GPU上の所有textureへの1コピー、GOP/B-frame、色変換、mux、音声の設定を維持。
- 待機最適化は両側でOFF。入力再利用だけOFF/ONを切り替える。
- 別ウォームアップ後、OFF→ON / ON→OFFを交互に実行。同時に別のGPUベンチやビルドは行わない。
- 初期化を除いたフレーム生成・upload・AMF・AAC・MP4終了処理を計時。各出力の全デコード・順序・色メタデータ・音声長は計時外で検証。
- **YMM4の素材読み込み・描画は含まない**。標準MFの実出力との直接比較でもない。

## 全体時間

profile ON、プール6枚、4往復の中央値:

| コーデック | OFF | ON | OFF / ON | 判断 |
| --- | ---: | ---: | ---: | --- |
| H.264 | 5371.34ms | 5406.39ms | 0.994倍 | ほぼ同速、全4ペアでONがわずかに遅い |
| HEVC | 3288.57ms | 3523.88ms | 0.933倍 | 中央値で低下 |

H.264の範囲はOFF 5348.07〜5390.24ms、ON 5356.35〜5446.35ms。HEVCはOFF 3211.92〜3330.54ms、ON 3322.63〜3636.25msでした。

profile OFF、プール4/6/8枚、各2往復の中央値:

| コーデック | プール | OFF | ON | OFF / ON |
| --- | ---: | ---: | ---: | ---: |
| H.264 | 4 | 5371.61ms | 5354.02ms | 1.003倍 |
| H.264 | 6 | 5347.46ms | 5342.32ms | 1.001倍 |
| H.264 | 8 | 5353.06ms | 5347.53ms | 1.001倍 |
| HEVC | 4 | 3859.31ms | 3797.33ms | 1.016倍 |
| HEVC | 6 | 3527.65ms | 3251.86ms | 1.085倍 |
| HEVC | 8 | 3409.63ms | 3246.44ms | 1.050倍 |

HEVCの6枚では1ペア目3772.87→3200.51ms、逆順の2ペア目は3282.43→3303.20msで逆転しました。8枚でも1ペア目3612.52→3274.17ms、逆順は3206.74→3218.72msです。少数反復・順序による差を含むため、表の5〜8%を再現性のある改善率とは採用しません。profile ON/OFFも別セッションであり、二つの表の差を計測負荷だけに帰属できません。

4枚より6/8枚のHEVCが速い傾向はありますが、プール枚数の組自体はランダム化していません。従来の既定6枚を変更する根拠にはしていません。2/3枚は今回のUI・native APIの対応範囲外で、比較していません。

## 入力解放と出力回収はどの程度離れたか

profile ONの各900フレームで:

| 指標 | H.264 | HEVC |
| --- | ---: | ---: |
| 取得→入力解放の平均 | 31.80〜32.22ms | 11.58〜12.12ms |
| 取得→出力回収の平均 | 31.91〜32.30ms | 11.63〜12.18ms |
| 両者の平均の差 | 0.084〜0.105ms | 0.053〜0.059ms |
| 出力回収前の入力解放通知 | 各900回 | 各900回 |
| 前の出力回収前に実際にtextureを再利用 | 316〜319回 | 0〜2回 |
| 同時使用textureの最大値 | 6枚 | 6枚 |
| 未回収フレームの最大値 | 7枚 / 上限12 | 6〜7枚 / 上限12 |

全入力が先に返っていても、その差は1フレーム分の大幅な先読み余裕ではありません。**通知が来たことと、高速化に有効な早期解放ができたことは別**です。また、この入力滞在時間はcopy・投入待ちを含み、純GPUエンコード時間ではありません。未回収数もAMF内部キュー深度ではありません。

### 呼び出し元で止まる部分

同じprofile ON試験で、H.264従来方式の900フレーム合計は、プール待ち1867〜2097ms、`SubmitInput` 980〜1088ms、`CopyResource`のCPU呼び出し1.61〜2.15msでした。新方式もプール待ち1588〜2091ms、`SubmitInput` 969〜1252msです。CPU側のcopy命令発行をさらに短くするだけで全体を大幅改善する、という根拠はありません。

HEVC従来方式のプール待ちは合計0.30〜0.48ms、`SubmitInput`は940〜980msでした。この条件ではプール返却が主な待ちではありません。

これらは平均・合計・最大の集計です。p50/p95/p99やframe IDごとのRender/Copy/Encodeの対応、GPU timestampによる純copy/convert時間は今回追加していません。非同期の区間を単純加算して1フレームの全体時間やGPU使用率とは解釈できません。

## 正しさと停止の検証

- native 16条件とmanaged Writer 32条件、計48出力が成功。H.264/HEVC、profile ON/OFF、待機ON/OFF、入力再利用ON/OFF、managed GPU直渡しON/OFFを含みます。各120フレームで全デコード・フレーム順序・BT.709 limitedメタデータ・音声長を確認。
- 上のベンチ40出力と別ウォームアップ16出力も、全デコード・順序・メタデータ・音声長の検証に成功。
- profile ON/OFFの各プール6枚・pair0をH.264/HEVCで比較（4ペア）。**各900フレームのデコード後の画素ハッシュが全て一致**し、音声のデコード後SHA-256も一致。
- 共通の音声hash: `c80582806ee0edf758e3782d1dcc326d2b143c838185669747b76e617a527dd0`。映像は`ffmpeg -map 0:v:0 -f framemd5 -`でフレームごとのhashを比較しました。
- 全ての比較対象は同じ合成素材です。MF品質固定100との品質比較や、任意の実素材での同等画質の保証ではありません。
- Destroyによる中断を0/1/8フレーム × 2コーデック × 入力再利用ON/OFFの12条件で確認。待機最適化ONで約2.0〜19.4ms。中断ファイルは完成動画の検証対象ではありません。
- managed CPU 135チェック、既存native profiling試験、出力待機の機能判定9シナリオ、入力寿命試験が成功。
- 入力寿命試験は古いticketの遅着、不正なticket 0の拒否、出力が先に完了する順序、予算上限、停止後callback、重複callback、4096件の独立した入力/出力スレッド、待ちのStop解除を含みます。最後にticket 0の拒否を補強したビルドでもAllを再実行し、48出力と12停止条件が成功しました。
- ローカルCI相当の`Run-Tests.ps1`へ接続済み。GitHub Actionsはpushしていないため未実行です。

YMM4 GUIでの新オプションの出力・キャンセル、長時間VRAM推移、device lost、別GPU/ドライバーは未検証です。音声長の一致だけで実素材の聴感上の同期を検証したとは扱いません。ピークRAM値は取得できずnullで、VRAM・CPU/GPUエンジン使用率・GC pauseの詳細測定も未実施です。

## 次に優先する設計上の違い

AMFの中核は既にC++で、今回もC++側を変更しています。C/Rustへの言語置換だけでは、ドライバー内部の待ちや入力/出力の寿命は変わりません。

次の候補は、PRにある**投入専用スレッドと上限付きキュー**です。YMM4から受け取ったtextureの所有コピーは呼び出し元で完了させ、コピー済みスロットだけをworkerへ渡す構成なら、YMM4の次フレーム描画と`SubmitInput`のCPU処理を重ねられる可能性があります。既存の出力回収スレッドとは別の役割です。Drain前の投入完了、キャンセル時の返却、同じD3Dデバイスの並行アクセスを扱う必要があり、**今回は未実装**です。エンコーダー側が飽和していれば、これでも総時間は短縮しません。

その次に標準MFの専用D3Dデバイスとの違いを評価します。デバイス間コピーと同期を追加するため、単純に移せば速いとは限りません。CPUの純データ処理をC++/Rustへ移す判断は、重い処理と割合を測定してから行います。

## 再現と証拠

```powershell
./scripts/Run-Tests.ps1 -Suite All -Ymm4Directory 'D:/YukkuriMovieMaker4/YMM4修正版'
./scripts/Run-OutputWaitBench.ps1 -Experiment InputRecycling -Pairs 4 -Frames 900 -Quality quality -Profile
./scripts/Run-OutputWaitBench.ps1 -Experiment InputRecycling -Pairs 2 -Frames 900 -Quality quality -PoolSizes 4,6,8
```

ローカル記録（Git管理対象外）:

- `artifacts/runs/input-recycling-20260918-001219-917ecd46/summary.json`: profile ON・4往復。
- `artifacts/runs/input-recycling-20260918-001647-fde15dc5/summary.json`: profile OFF・4/6/8枚・各2往復。
- `artifacts/runs/gpu-smoke/managed-29e888ac3bff4a788be34ad2814ca888`: managed 32出力。
- `artifacts/runs/diagnostic-tests-c3c8e776dbfc43848ae3d566b2ad9e00`: All実行時のCPU試験。
- `artifacts/runs/diagnostic-tests-f34b29c2ed2f4a6e8f774940e4bcfff4`: 並行寿命試験追加後のUnit再実行。
- `artifacts/runs/diagnostic-tests-081264d375104b2985e12d20828c544a` / `artifacts/runs/gpu-smoke/managed-0c9b0e0b8a2c418aaa17c11d458d0a5e`: 最終のticket拒否補強後のAll再実行（CPU・managed GPU）。

現在の`artifacts/bin`のSHA-256:

```text
AMFPlugin.dll   E76FEE982D4172806DAA3E049EAB16CAD29F6842877E251A5656FE396AD2A82A
AmfNative.dll   9581748349006072A95CBBFADE21FB6A75DDB5E366CDE1E79D5CF5A2F78A443B
RadeonBench.exe 3AA7BF50A1772444EFE32E8E07C59DDFC3FD94A3CE7DF745B0DEA4BFCF9589C7
```

二つの速度比較で使用したnative DLLのSHA-256は共に`1EDE89B115F243E9B784372B8376CDE740A2E6F00CF8964DB7A892F6B5593ACB`です。profile ON試験後にRadeonBenchへCPU並行テストを追加し、速度比較終了後にnative側へ不正なticket 0を拒否するチェックを追加しました。最後の変更後はAllの回帰試験を再実行しましたが、上の反復速度表を最終バイナリで測り直したわけではありません。各summaryに測定バイナリのhashを残しています。

調査した標準MF DLLのSHA-256は`669F9FD2490EEC545CD0E7E8EBC2B2090DD6DAACDB419EB7D2412DD3CA51C23F`。配置先のYMM4本体は4.56.1.1です。MF DLL自身のFileVersionは1.0.0.0で、本体の版番号とは区別しています。

開発版DLLの配置は`artifacts/bin`までです。YMM4へのインストール、コミット、push、VERSION更新、リリースは行っていません。既存のExportProbeや未コミットの出力経路調査には変更を加えていません。
