# 2026-09-17: YMM4出力経路調査とGPU入力オプション

## 結論

YMM4標準Media Foundation出力だけの特権ではなく、公開`IVideoFileWriter3.IsGpuFrameSupported`がGPU入力の分岐点だった。AMFも同じ公開APIを実装し、動画出力設定の「GPU直渡し（実験・IVideoFileWriter3）」で選択できるようにした。既定OFF、Writer生成時に固定。Harmony・ExportProbe・writerラッパーは不要。

今回省くのはYMM4側のCpuReadビットマップへの中間コピー。既存C++のAMF初期化・所有テクスチャプール・GPUコピー・SubmitInput・回収・Mux・Audio処理は変えていない。完全Zero Copy、Rust移植、先読みキャッシュは実装していない。その後提供された実プロジェクト1往復のログでは、経路の切替と両方の最終化を確認したが、高速化は確認できなかった。

## 調査対象と確認経路

ローカルのYMM4 `4.56.1.1+4b3cdaa22974d33188f874898334f03549b29fc6`をIL解析した。YMM4 DLL・逆コンパイル全文は配布物に含めない。版を固定して拒否する実装は入れず、AMFビルド/実行には`IVideoFileWriter3`を含むホストAPIが必要。

| DLL | SHA-256 |
| --- | --- |
| YukkuriMovieMaker.dll | `407176E632418BA6DA5E671D6FE8C5138814E39FDA7460451B4E00E2EA4A8102` |
| YukkuriMovieMaker.Plugin.dll | `93E04CEB211BCD3BB8BF8BE7E25EC09B46D6DA01583F9339F49C358035008249` |
| YukkuriMovieMaker.Plugin.FileSource.MediaFoundation.dll | `669F9FD2490EEC545CD0E7E8EBC2B2090DD6DAACDB419EB7D2412DD3CA51C23F` |

`YukkuriMovieMaker.VideoFileWriter.VideoFileWriter.CreateFileAsync`の共通出力ループは、共通Rendererで描画した後に以下のように分岐する。

| Writerの契約 | YMM4から渡すもの |
| --- | --- |
| `IVideoFileWriter3`かつ`IsGpuFrameSupported=true` | 描画済みtargetBitmapをそのまま渡す |
| v3でfalse、または`IVideoFileWriter2` | `CpuRead | CannotDraw`ビットマップへ`CopyFromBitmap`して渡す |
| 旧v1 | 上記コピー後にbyte配列化して渡す |

従来AMFはv2であり、プラグインへ届く前に中間コピーがある。AMF自身はそのbitmapのD3D11 textureを取り出して既存プールにGPUコピーし、CPUへMapして画素を読む処理はない。したがって「GPU直渡しOFF＝CPUエンコード」「旧AMFは必ずCPUへ画素を読み戻して再upload」は誤り。

標準MFの`MFVideoFileWriter`はv3を実装し、`EncoderFrameBridge`で描画デバイスからshared keyed-mutex textureを経由し、encoder側textureへGPUコピーする。`MFCreateDXGISurfaceBuffer`、`IMFTrackedSample.SetAllocator`の回収通知で再利用する構成だった。これも完全Zero Copyではない。GPU経路失敗時はCPU経路へ切り替わる。入力モードやエンコーダー設定は実行ログで確認する必要がある。

AMFは入力と同じD3D11デバイスでencoderを初期化して既存プールへコピーする。YMM4は描画targetを次フレームで再利用するため、寿命の保証なしにAMFへ非同期で借用させてコピーを消してはいけない。

## 提供された比較動画から分かること

`AMF_Comparison_260917.mp4`の表示を読んだ結果。GPU表示はRX 6600であり、この開発機のRX 6800 XT測定ではない。録画ファイル自体の1920×1080/60fpsは、書き出した動画の設定の証明には使わない。

| 表示値 | AMF | 標準MF |
| --- | --- | --- |
| 完了時間 | 42.75秒 | 25.83秒 |
| 総フレーム数 | 3,601 | 3,601 |
| 総フレーム/完了時間 | 約84.23fps | 約139.41fps |
| ファイル容量表示 | 175MB | 435MB |
| 確認できる主な設定 | H.264、自動YouTube、高品質、pool 6 | 固定品質100 |

この条件ではAMFの時間はMFの約1.655倍。ただしビットレート方式・品質設定・容量が一致していない。MFの「品質100」は速度設定ではなく、内部には別のEncodeSpeedもある。この動画だけから入力中間コピーが差の全原因、AMFハードウェアの限界、または画質優劣とは結論できない。録画当時にMFのGPU経路が実際に選ばれたことも未確認。

## 11:47:39のExportProbe実ログ

対象は`20260917-114739-6dfaf53a1160410e8f0a2d5274efb9ac.exportprobe.json`、旧schema v2。今回のv0.1.3出力Hookの実績とは区別する。

- 4ソース、4,369更新。3本のMP4は実際にはFFmpeg reader、WebPはWIC。HighSpeedのfactory試行と実際のreader採用を区別した。
- 主な長尺MP4は3,442更新、FFmpeg Updateが3,277.19ms、その内側のCrossDevice Transferが2,092.73ms（約63.86%）。これはCPU側の待ち・同期・resource準備を含み、純GPUコピー時間ではない。
- factory openは合計4回、FFmpeg seekは3回、計0.4953ms。現ログでは大量のOpen/Seek反復が主因という証拠はない。
- WebPは生成経路の試行が大半で、WIC Update 111回の合計は1.8442ms。毎フレームのWebP decodeが重いとは言えない。
- MFのWebP形式探索中に内部例外3回を観測したが、外へ伝播した動画更新失敗とは別。観測失敗なしと内部例外ゼロを混同しない。

| 段階 | 回数 | inclusive合計 | 平均 | p50上限 | p95上限 | p99上限 | 最大 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| EffectedItemSource.UpdateEffects | 24,703 | 8,363.63ms | 0.3386ms | 0.5ms | 2ms | 2ms | 34.8798ms |
| FFmpeg Update | 4,258 | 3,734.24ms | 0.8770ms | 1ms | 2ms | 2ms | 53.4664ms |
| CrossDevice Transfer | 3,847 | 2,311.53ms | 0.6009ms | 1ms | 1ms | 2ms | 5.0776ms |

時間は入れ子・並列で重複する。足して全体の内訳にはできない。`rootBusyUnionMilliseconds=9630.96`も書き出し全時間ではなく、FPSへ換算しない。p50/p95/p99はヒストグラム上限であり、正確な分位点ではない。reader側CrossDeviceFrameBridgeとwriter側EncoderFrameBridgeは別の転送経路。

## 今回の実装・独立検証

- AMF動画出力画面にGPU直渡しチェック。設定既定OFF、factory snapshotとwriterのreadonly値で進行中の変更を防止。
- AMF集計JSONに`configuration.gpu_direct_input_enabled`と`input_delivery_path`を追加。
- ExportProbe v0.1.3に設定/ログタブと、動画使用区間・内部詳細・出力経路の独立選択。GPU入力制御は含めない。
- 出力専用private Render、AMF/MF WriteVideo/WriteAudio、MF CPU/GPU経路・bridgeをオプション観測。選択項目はschema v3の`options`へ記録。output-onlyではinventoryを取得しない。

| 検証 | 結果と範囲 |
| --- | --- |
| Releaseビルド | 警告0・エラー0。AMF DLLを`artifacts/bin`へ自動配置 |
| 管理側単体テスト | 43チェック成功。UI→設定→Writer固定、既定OFF、破棄時native不使用、profile入力経路を含む |
| 管理側実GPU試験 | H.264/HEVC × GPU直渡しOFF/ON × profile OFF/ONの8条件、64チェック成功 |
| 上記8出力の検証 | 各640×360/60fps/120フレーム。全フレームdecode、順序マーカー、BT.709/limitedメタデータ、音声長の整合が成功 |
| 既存native単体 | H.264/HEVC × profile OFF/ONの4条件も同じ出力検証成功 |
| ExportProbe | Core 24/24、HostContract 36/36成功。追加出力Hook 14件を実DLLに独立プロセス内でattach/detach確認 |

テストはYMM4の描画targetの反復再利用と、v2のCpuReadコピー/v3の直接入力を再現するが、YMM4 GUIを起動した実プロジェクト試験ではない。音声同期の自動確認はdurationのみ（差5.333ms）であり、発話と映像の意味的な同期確認ではない。色はメタデータ検証であり画素値による色精度・HDR・画質比較ではない。GPU試験は短時間で、長時間のVRAM増加やキャンセルは未検証。

ローカル証跡は`artifacts/runs/gpu-smoke/managed-83dc79ca26194acebae07d45e15680b1`、`artifacts/runs/diagnostic-tests-8a5b71a72bbd47cbb0eeb6d9f43d914b`、`ExportProbe/artifacts/runs/host-contract/b0b43692201f45b6ab0ea4ff04fe4fa0`。生成物はGit管理しない。

## 追加: 利用者によるGPU直渡しON/OFF実プロジェクトログ

受領した`Jetson Nano レビュー_RadeonPluginテスト用Copy_shorts.mp4.amf_profile - コピー.json`がGPU ON（21:37:18更新）、通常名の`.amf_profile.json`がGPU OFF（21:38:23更新）。ファイル名から推測せず、`input_delivery_path`と`configuration.gpu_direct_input_enabled`で判別した。順番はON→OFF。

両方とも同一assembly MVID `dfb39249-5750-41e5-880a-64a596032ff3`、1920×1080/60fps、H.264 Balanced、自動12Mbps、pool 6。受信/投入/完了すべて7,259フレーム、音声サンプル値11,614,400、`output_finalized=true`、errorなし、input retries=0。ログ上の完了は確認できるが、実MP4の再生・画素・音ズレ検証は別。

| 指標 | GPU OFF（通常名） | GPU ON（コピー） |
| --- | ---: | ---: |
| Writer生成〜Dispose | 39.8768秒 | 41.3047秒 |
| 上記時間に対する受信速度 | 182.04fps | 175.74fps |
| 最初〜最後のcallback区間 | 36.6166秒 | 37.3081秒 |
| callback区間だけの受信速度 | 198.24fps | 194.57fps |
| callback union | 4.5671秒 | 4.5641秒 |
| callback外（上記区間内） | 32.0495秒 | 32.7440秒 |
| 映像callback平均 | 0.4240ms/frame | 0.4193ms/frame |
| Pool待ち平均 | 0.1360ms/frame | 0.1267ms/frame |
| CopyResourceのCPU呼出平均 | 0.00569ms/frame | 0.00600ms/frame |
| SubmitInput平均 | 0.2494ms/frame | 0.2529ms/frame |
| slot滞留平均 | 14.4746ms | 14.1885ms |
| QueryOutput再試行間の待機平均 | 15.0870ms | 15.0933ms |

この1往復ではGPU ONのWriter全時間が1.428秒（3.58%）長く、callback配信区間は0.691秒（1.89%）長い。callback union差は約-3msしかなく、配信区間の差はほぼcallback外の差（+0.694秒）。さらに最初のcallback前/最後のcallback後を合わせた区間に+0.736秒の差がある。AMF映像API本体が大幅に遅くなったというデータではない。

callback外は配信区間の約88%だが、そこでは上流描画だけでなくGPUの非同期encode・別スレッド処理も重なり得る。純粋なYMM4 Render時間や、最適化可能な待ち時間の割合とはしない。各1回・順序固定・両方debug log ONのため、GPU直渡しが恒常的に遅いとも断定しない。

`AmfNative.cpp`のQueryOutputがまだ出力を返さない分岐は`std::this_thread::sleep_for(1ms)`を指定しているが、実測`output_poll_wait`平均は約15msだった。設定値と実待機の差は具体的な調査候補。pool再利用を遅らせる可能性はあるが、バックグラウンド回収スレッドの待機総量35〜36秒を全体から差し引けるわけではない。pool待ち合計はOFF約0.987秒、ON約0.919秒。今は待機方式やタイマー設定を変更していない。

注意: `.amf_log.txt`は追記式でON→OFFの2セッションを含む。「コピー」のtextログにもOFFの途中まで含まれており、厳密な1ファイル1セッションではない。性能比較は各JSONを正として扱った。次回は`gpu-on-01.mp4` / `gpu-off-01.mp4`等の別出力名を使い、debug OFF、同条件でAB/BA反復する。

受領JSONのSHA-256:

- OFF: `B6AF2199CA05413A03916D5CE9C981EC8DCB5E2556C65258EF51F0F59B951CFB`
- ON: `62E8C86135D056BF0B7CF5639DB2750A69823EE53CB95075B085388037DC395D`

## 要求された詳細プロファイルの残作業

今回で「1フレームがどこで何ms止まるか」の完全な時系列レポートが完成したわけではない。

| 対象 | 現在分かること | 次に必要なこと |
| --- | --- | --- |
| YMM4取得/Render | Render・reader・effectのCPU wall集計、最大frame | フレームIDに結び付けた取得前後のtimestamp、ホスト中間コピーの境界 |
| GPU同期/コピー | CPU呼出時間、親処理の待ち込み時間 | D3D11 timestamp/disjoint query。計測のための毎フレームFlush/全GPU待機は避ける |
| Pool/Submit/Query | slot wait/residence、Submit/Query、retry/pollの集計 | フレーム別滞留、pool占有率/枯渇、投入済み未完了数。AMF内部queue depthとは区別 |
| 色変換 | 外側のmanaged処理・AMF内包の経過時間 | AMF内部の変換/encodeを分離できる計測手段の確認 |
| Mux/Audio/並列性 | native stage集計、managed callback union | 共通clockとframe/PTS IDでRender/Copy/Encode/Mux/audioを対応付ける |
| p50/p95/p99 | ExportProbeにバケット上限 | AMF managed/nativeへのbounded histogram追加、フレーム追跡と整合 |
| CPU/GPU/Video Codec/VRAM/GC | 今回追加なし | 低頻度samplerとETW等。プロセス帰属・GC pause/alloc量と負荷評価 |
| pool 2/3/4/6/8 | 現在4/6/8。今回変更なし | 2/3の安全な許容範囲確認と同条件反復測定 |
| 完全Zero Copy | 未実装 | host描画targetの寿命と再利用防止が必要。コピー除去を先に行わない |

## 次の比較手順

1. 同じビルドの`AMFPlugin.dll`と`AmfNative.dll`を利用者が更新してYMM4を再起動する。インストール・設定はビルドでは変更しない。
2. 同じ短い出力範囲、品質、bitrate、codec、poolでGPU直渡しOFF/ONを別名出力し、再生・フレーム数・音声を確認する。
3. ExportProbe OFFでAB/BA順に各3〜5回。最速1回ではなく中央値・範囲を比較する。通常/破棄はGPU設定を揃えて別に測る。
4. 出力経路の内訳を調べる回だけExportProbeの必要項目をON。MFのGPU/CPU経路を実際のhook callsで確認する。AMF profileも別に取得する。
5. GPU timestampとフレームIDの観測を先に拡張し、そこで律速した処理のみ改善する。C#/C++/Rustへの配置判断は測定後に行う。

標準MFとの性能比較には設定条件と出力品質の確認が必要。GPU直渡しだけで比較動画の時間差が解消する保証はない。
