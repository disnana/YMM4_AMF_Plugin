# v0.2.1-preview.1 採用設定と配布前検証（2026-09-18）

ユーザーの実機比較用に、既定を所有プール8枚・出力待機最適化ONへ変更した。品質はBalanced、GPU直渡しと他の実験機能はOFF。6枚・待機OFFへ切り替える比較ボタンも用意した。両ボタンは圧縮・診断設定を保持し、進行中のWriterではなく次に作成されるWriterに作用する。

入力解放通知回収、専用スレッド、専用D3Dデバイス、入力満杯の通知待ち、外部NV12変換は、これまでの反復で追加の速度利益を示せなかったため既定に採用しない。既存の所有GPUコピーと非同期出力回収は維持した。

## 同梱対象DLL

- AMFPlugin.dll: `C4621054C5950AF61492D9FB61C6B908742B4566E2F60064C4144FD9CA4D6427`
- AmfNative.dll: `4F6C74AE1C8A815D153CA2257F991CF327F535B27D7B6CB91882FF0A7F1F19FD`

以下はこの組で実行。VERSIONは`0.2.1-preview.1`。ローカルのテストパッケージであり、commit / push / GitHub Releaseは行っていない。

## 既定入力経路での速度

RX 6800 XT、YMM4 4.56.1.1の公開描画デバイスを使う隔離ハーネス。YMM4アプリやプロジェクトは起動しない。H.264 Balanced、1920×1080、60 fps、900フレーム、VBR 12,000 kbps、48 kHz stereo音声、プロファイルOFF。GPU直渡しOFFのため、ホスト相当のCpuRead Bitmapへのコピーを含む。入力解放回収等の他の実験はすべてOFF。

同じDLLで6枚・待機OFFと8枚・待機ONを各4回。実行順はAB / BA / AB / BA。全出力と最終化が完了するまでを計測した。デコード検証は8回の速度測定の後で実施した。

| 設定 | 中央fps | 最小–最大fps | 中央出力時間 |
| --- | ---: | ---: | ---: |
| 従来: 6枚・待機OFF | 358.20 | 354.49–366.48 | 2512.57 ms |
| 推奨: 8枚・待機ON | 406.16 | 401.85–407.20 | 2215.88 ms |

fps +13.39%、時間約11.81%短縮。これは今回の組み合わせの比較で、待機と枚数それぞれの寄与を分離した数値ではない。旧v0.1.1 DLLそのものの再測定でも、MFとの再比較でもない。YMM4実プロジェクトで同じ改善率を保証しない。

全8出力が13,250,824 bytesで、900フレームの全デコード・順序・色・音声検査に合格。MP4全体のハッシュにはメタデータ等の差があるため画質同一性の根拠には使わず、第1ペアをYUV420pおよびPCM s16leへ復号してSHA-256を照合した。

- 映像（両設定共通）: `aadc1646a4e5c4bebe8aabd4b02101c00ad13551ac826ecbb4ba8657ce1028d6`
- 音声（両設定共通）: `6cf104af8762e56c2d1f9ba8f51736812b4b02600dd4340b2d34eba70c131f7e`

他の素材・GPUでの一致や、MF品質100との同等性を証明するものではない。

## 動作・設定・終了

- Unit: managed429チェック、画質比較契約18件、nativeのプロファイル・出力待機・入力寿命・キュー・停止・ABI試験が合格。プリセットが既存の圧縮設定を変えず、生成済みWriterのスナップショットを変えないことも確認。
- 新規の既定設定を上書きせず生成したmanaged Writerで、H.264/HEVC × profile OFF/ONの4出力、各120フレーム。GPU54チェックと全デコード・順序・色・音声検査が合格。
- profileで8枚の実効プール、GPU直渡しOFF、`output_wait.effective_mode=amf_query_timeout`、`reason=enabled`を確認。入力面は終了時0枚。
- H.264/HEVC × 0/1/8入力のFinalize前Destroy: 全6条件が約2〜18msで完了。GUI上での取消とは区別する。
- 長時間耐久、GUIプロジェクト、他GPU/ドライバーの検証は今回未実施。

## 再現

```powershell
./scripts/Run-Tests.ps1 -Suite Unit -Ymm4Directory 'D:/YukkuriMovieMaker4/YMM4修正版'
dotnet ./tests/AMFVideoWriterPlugin.Tests/bin/x64/Release/net10.0-windows10.0.19041.0/AMFVideoWriterPlugin.Tests.dll --gpu-defaults <新しい結果フォルダ> ./artifacts/bin/AmfNative.dll
./tests/MfAmfPathBench/Run.ps1 -YmmDirectory 'D:/YukkuriMovieMaker4/YMM4修正版' -RunDirectory <新しい結果フォルダ> -Mode amf-cpu -Rate vbr -Frames 900 -AmfQuality Balanced -Pool 8 -OutputWait $true
```

従来側は`-Pool 6 -OutputWait $false`。各4回の計時を済ませてから`Validate.ps1`で検証する場合は、Runに`-SkipValidation`を指定する。

生データ: `artifacts/runs/optimized-preview-20260918/`の`defaults/`、`paired/`、`lifecycle/`。これらはプラグイン配布物に含めない。
