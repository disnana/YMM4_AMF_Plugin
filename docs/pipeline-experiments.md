# AMF投入スレッド・専用D3D11デバイスの実験

出力設定に独立したチェックボックスを追加。両方とも初期値OFF。既存の画質、bitrate、回収方式、出力待機、所有プールの設定は変えない。実際の適用値は、デバッグログとプロファイルの `pipeline` に記録する。新機能を要求したのに新しいDLLが見つからない場合は、黙って旧経路に戻さずエラーにする。

[2026-09-18の実装後検証](benchmarks/2026-09-18-pipeline-isolation.md)では、投入スレッド化だけではほぼ同速、専用デバイス化は短い試行で遅くなった。画素・終了経路の検査は通過したが、通常の出力に推奨する段階ではない。実YMM4での反復・長時間試験は別途必要。

`AMF_INPUT_FULL`後の固定sleepを出力完了通知へ置き換える実験も独立したチェックとして追加した。しかし[RX 6800 XTでのAB/BA実測](benchmarks/2026-09-18-input-full-wait.md)では中央値の差が-0.46%〜+0.12%（投入スレッド併用は+0.05%〜+0.06%）で、pool 32の再試行回数だけが約6.4倍へ増えた。既定OFFとし、他の実験機能へ自動連動させない。

## 所有権と実行順序

| 設定 | WriteVideo呼出元で行う処理 | AMFへの投入 | GPUコピー数 |
| --- | --- | --- | ---: |
| 両方OFF | 所有プールへコピー | 呼出元 | 1 |
| 投入スレッドのみON | 所有プールへコピー、有界キューへ追加 | 専用スレッド | 1 |
| 専用デバイスのみON | 共有面へコピー、専用デバイスの所有プールへコピー | 呼出元 | 2 |
| 両方ON | 上記2コピー、有界キューへ追加 | 専用スレッド | 2 |

どの経路も、借用したYMM4描画テクスチャをキューに残さない。コピー命令を発行してから `WriteVideo` を返す。GPU処理完了まで毎回CPUで待つことを意味しない。専用デバイスの橋渡しだけはkeyed mutexで同期する。コピー数はこのプラグインの受け渡し部分のみで、AMF内部の色変換・コピーやYMM4側のv2中間コピーは含めない。

キューは新規テクスチャを確保せず、既存の所有プールで予約した面とそのAMFSurfaceを保持する。上限は実効プール枚数。キュー深度は「まだSubmitしていないジョブ数」であり、AMF内部のqueue depthではない。入力解放通知ON時は従来どおり未回収出力数も2×poolに制限する。

従来の出力回収方式ではAMFSurfaceを再利用するため、`SubmitInput` が復帰したことと出力通知の両方を確認してから空き面に戻す。AMFコンポーネントのSubmit/Queryは複数スレッドで使用できるが、個々のAMFSurfaceのプロパティを同時に変更してよいという意味ではない。入力解放通知ON時はフレームごとの新しいSurfaceとticketを使う。[AMF API reference](https://github.com/GPUOpen-LibrariesAndSDKs/AMF/blob/master/amf/doc/AMF_API_Reference.md#26-components)

終了は `queue.Close → 全ジョブSubmit → worker.join → AMF.Drain → 全出力回収 → MP4確定`。直接Destroyする取消経路は、キュー停止と未投入lease解放、worker終了を先に行い、その後AMFのFlush/Terminateを行う。入力の最初のエラーを保持し、終了時の二次的なcodec headerエラーで上書きしない。C++側のドライバー呼出そのものは強制中断できないため、ドライバーハング全般を防ぐ保証ではない。

## 専用デバイスの境界

元デバイスから実際のDXGI adapterを取得し、同じadapter上にD3D11 BGRA/VideoSupportデバイスを作る。別GPUを推測選択しない。共有用テクスチャは1枚だけで、NT handleは初期化時に開いて閉じ、フレームごとに作り直さない。

`AcquireSync` は有限5秒、戻り値は `S_OK` との完全一致で判定する。`WAIT_TIMEOUT` / `WAIT_ABANDONED` は正の値であり、`SUCCEEDED` 判定だけでは失敗を見逃す。[Microsoft: AcquireSync](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgikeyedmutex-acquiresync)、[OpenSharedResource1](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11device1-opensharedresource1)

借用したYMM4デバイスに `ClearState` / `Flush` やスレッド保護設定変更は行わない。投入スレッドON・専用デバイスOFFの場合、元デバイスが既にmultithread protectedであることを検査する。専用デバイスは自身で保護し、AMFと所有面の破棄後に自身のcontextのみClearState/Flushする。プールの1 GiB目安に加え、共有面1枚分（1080p約7.91 MiB、4K約31.64 MiB）を確保する。総VRAMにはドライバー等の別費用もある。

## 計測と再現

```powershell
./scripts/Run-Tests.ps1 -Suite Unit -Ymm4Directory 'D:\YukkuriMovieMaker4\YMM4修正版'
dotnet build tests/MfAmfPathBench/MfAmfPathBench.csproj -c Release '-p:YMM4DirPath=D:\YukkuriMovieMaker4\YMM4修正版\'
./tests/MfAmfPathBench/Test-PipelineNative.ps1 -RunDirectory artifacts/runs/new-native-pipeline
./tests/MfAmfPathBench/Run-PipelineSweep.ps1 -YmmDirectory 'D:\YukkuriMovieMaker4\YMM4修正版' -RunDirectory artifacts/runs/new-pipeline -Pools 16,32
./tests/MfAmfPathBench/Run-PipelineSweep.ps1 -YmmDirectory 'D:\YukkuriMovieMaker4\YMM4修正版' -RunDirectory artifacts/runs/new-pipeline -ValidateOnly
./tests/MfAmfPathBench/Summarize-PipelineSweep.ps1 -RunDirectory artifacts/runs/new-pipeline
./tests/MfAmfPathBench/Run-InputWaitBench.ps1 -YmmDirectory 'D:\YukkuriMovieMaker4\YMM4修正版' -RunDirectory artifacts/runs/new-input-wait -Pools 16,32 -Rounds 4 -Frames 900
./tests/MfAmfPathBench/Summarize-InputWaitBench.ps1 -RunDirectory artifacts/runs/new-input-wait
./tests/MfAmfPathBench/Hash-InputWaitCases.ps1 -RunDirectory artifacts/runs/new-input-wait
```

ffmpeg / ffprobeをPATHへ追加してから実行。タイミング試行は順序反転で反復し、デコード検証を同時に走らせない。`-Quality Quality` で高品質、`-RenderRepeats 16` で矩形描画量を増やした合成負荷を比較できる。描画量の増加は実YMM4プロジェクトやPSDの再現ではない。プロファイルは別試行にし、p50/p95/p99/maxはハーネスのフレーム別WriteVideo CPU時間から取得する。GPU timestampやCPU呼出内の真のGPU実行時間を測ったと解釈しない。

## 高品質プリセットの案内

高品質は削除しない。通常は標準を推奨する理由をUIに明記した。既存の2素材の実測では標準→高品質のPSNR改善は約0.007 / 0.029 dBと小さく、合成映像の処理時間は約2.2倍だった。これは全素材での画質同等性や、MFとの品質一致を意味しない。[画質比較と条件](benchmarks/2026-09-18-mf-actual-writer.md#画質-共通参照から実測)
