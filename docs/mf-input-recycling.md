# MFの入力寿命管理をAMFへ取り込む実験

開発版の出力設定に「MF方式：入力解放通知でテクスチャを再利用（実験）」を追加しました。既定OFFで、公開済みv0.2.0には含まれません。エンコーダーはAMFのままです。MFへ切り替える設定ではありません。

今回取り込んだのは、**入力テクスチャの返却を、圧縮済み出力の回収から独立させる仕組み**です。PRと標準MFにある入力解放通知を使ったプール管理を参考に、AMFの公開APIでC++側へ実装しています。画質設定を下げる高速化ではありません。ただし、この仕組みがこのGPUで速いかは別問題です。[反復測定の結果](benchmarks/2026-09-18-mf-input-recycling.md)を参照してください。

## PR・標準MFと、今回の実装の対応

調査対象は[Community PR #79](https://github.com/manju-summoner/YukkuriMovieMaker.Plugin.Community/pull/79)のhead `99cda7487d0422fc0e3ebcf54e7b920ed781cb0f`と、ローカルYMM4 4.56.1.1配置の標準MF DLLです。PRは調査時点でclosed/unmergedであり、標準MFと同一のコードではありません。

| 仕組み | PR #79 | 調査した標準MF | AMF側への反映 |
| --- | --- | --- | --- |
| GPU上でエンコーダー所有の入力を作る | GPU成功経路で所有テクスチャへ1コピー | 共有テクスチャ経由の2コピー | 既存の所有プールへの1コピーを維持。CPU読み戻しは追加しない。 |
| 入力の寿命でプールに返す | `IMFTrackedSample.SetAllocator`のcallback | ticketと`SampleRecycler`で返却 | **今回実装**。`AMFSurfaceObserver`の通知で返却する。 |
| プールの上限 | 画像サイズに応じた8〜32枚とセマフォ | 空なら追加確保。静的解析では上限を確認できない | 既定6枚。4/6/8枚に加え、開発版では16/32/64/128枚を実験選択可能。増量分に画素量目安1 GiBの制限。未回収フレームの上限は実効枚数の2倍。 |
| エンコーダーへの投入を別スレッドへ移す | 専用pipeline threadが`WriteSample` | bridge自体はlock/keyed mutex内で直列 | **未移植**。`SubmitInput`は従来どおり呼び出し元。出力回収・ファイル書き込み用スレッドは既存。 |
| 描画用D3Dデバイスとの分離 | GPU経路は既存デバイスを利用 | encoder専用デバイスとshared NT handle | **未移植**。AMFは既存の描画デバイスを利用する。 |
| 圧縮・品質の設定 | quality / quality-vs-speed等をMFへ指定 | RGB32入力からH.264等へ変換・圧縮 | 変更なし。AMFの既存preset、ビットレート、レート制御、GOP等を維持。 |
| 回収pollの待機 | 本プラグインと同じAMF回収ループではない | 本プラグインと同じAMF回収ループではない | [別オプション](output-wait-optimization.md)。今回の入力再利用とは独立。 |

PRの根拠は[固定commitのWriter](https://github.com/manju-summoner/YukkuriMovieMaker.Plugin.Community/blob/99cda7487d0422fc0e3ebcf54e7b920ed781cb0f/YukkuriMovieMaker.Plugin.Community/FileWriter/Video/MediaFoundationFast/MediaFoundationFastVideoFileWriter.cs)のプール設定（79〜100行）、入力コピーとrecycler（314〜421行）、pipeline（467〜659行）、SinkWriter属性（857〜901行）です。標準MFでは`YukkuriMovieMaker.Plugin.FileSource.MediaFoundation.dll`内の`EncoderFrameBridge.AcquireSample`、`BuildTrackedSample`、`SampleRecycler.Invoke`、`RecycleTicket`を追跡しました。逆コンパイルしたYMM4の実装そのものをプラグインへ転載・同梱していません。

旧MFからの高速化と整合する主要な実装差分は、CPU読み戻しを含む受け渡しをGPU上の受け渡しに変え、描画とエンコードの入力寿命を分離した点です。一方、現在のAMFもGPU入力・所有プールを持っています。この静的解析だけで、現在のMF対AMFの速度差の原因や寄与率まで特定できたとは扱いません。

## 入力と出力を分ける

従来方式では、AMFから圧縮済み出力を回収し、muxへの投入を済ませたときにスロットを空けます。新方式では次の二つを独立に管理します。

1. **入力テクスチャ**: AMFが入力surfaceを解放した通知で返す。texture自体はプール内に残す。
2. **未回収フレームの予算**: 対応する圧縮済み出力を処理したときに戻す。

同じtextureを再利用しても毎回異なるticketを付けます。古い出力が届いても、新しく使用中のtextureを解放しません。callbackと出力の順番が逆でも扱える構造です。

実装は[`AmfInputRecyclePool.h`](../AmfNative/AmfInputRecyclePool.h)と[`AmfNative.cpp`](../AmfNative/AmfNative.cpp)です。記録領域は初期化時に固定数確保します。一方、AMFへ寿命を渡すためのsurface wrapperとobserverは各フレームで生成します。**毎フレームのallocationがゼロになる変更ではありません**。追加コストが利点を上回る可能性も測定対象です。

使用する通知はAMFの[公開APIドキュメント](https://github.com/GPUOpen-LibrariesAndSDKs/AMF/blob/master/amf/doc/AMF_API_Reference.md)の`AMFSurfaceObserver.OnSurfaceDataRelease`です。YMM4の内部API、Reflection、Harmonyには依存しません。既存の`AmfCreate`は維持し、新オプションON時だけ`AmfCreateWithInputRecycling`を使用します。

停止時は新規取得を止め、callbackが遅れても触るtextureと管理情報を保持します。入力取得・投入再試行は30秒、既存のDrainは60秒の期限があります。ただし、ドライバーの全ネイティブ呼び出しに強制停止期限を保証するものではありません。デバイス喪失とYMM4 GUIのキャンセルE2Eは別途検証が必要です。

## 試し方

1. ビルド済みの`artifacts/bin/AMFPlugin.dll`と`artifacts/bin/AmfNative.dll`を同じ組で使います。YMM4を終了してから配置してください。ビルドはYMM4本体へ自動インストールしません。
2. 動画出力設定の「MF方式：入力解放通知でテクスチャを再利用（実験）」をONにします。変更は次に開始する書き出しへ適用されます。
3. 品質、ビットレート、プール枚数、GPU直渡し、待機最適化は固定し、このチェックだけOFF/ONで比較します。最初は待機最適化をOFFにすると影響を分けられます。
4. 出力名を毎回変え、OFF→ON / ON→OFFを反復します。遅い、破損する、終了しない場合はOFFへ戻してください。

プラグイン設定はファイル保存していないため、YMM4再起動後は確認してください。古いnative DLLでONにした場合は、黙って無視せず同一ビルドのDLLへ更新するようエラーを出します。破棄モードではこの実験処理を実行しません。

## ログの読み方

プロファイルの`configuration.recycle_input_after_release`とトップレベル`input_recycling`を確認します。デバッグログにもモードと完了時の回収数を記録します。

| 項目 | 意味 |
| --- | --- |
| `requested` / `effective_mode` | 指定値と適用方式。managed JSONでは`amf_surface_release` / `encoded_output_legacy`。 |
| `pool_size` / `peak_inputs_in_use` | 所有texture数と同時使用数の最大値。 |
| `pending_output_limit` / `peak_pending_outputs` | 未回収フレームの上限と最大値。取得済み・投入準備中も含む**本プラグインの記録**で、AMF内部キュー深度ではない。 |
| `input_releases` / `output_completions` | 入力解放通知数と出力完了数。途中停止では一致しない場合がある。 |
| `input_releases_before_output` | 出力回収前に入力解放を通知された回数。これだけでは速度向上を意味しない。 |
| `reuses_before_output` | 前の出力が未回収のうちに同じtextureを実際に再利用できた回数。 |
| `input_residence` | 取得からAMFの入力解放通知まで。copy、投入待ち等も含む。`total_ms / count`で平均。 |
| `invalid_events` | ticket不整合や重複通知など。正常完了では0。 |

従来の`native.stages.slot_residence`はON時も**取得から出力回収まで**を記録します。新方式での物理textureの占有時間は`input_residence`です。二つの差が短い場合、入力の早期返却で削れる余地も小さいと判断できます。どちらもGPUの純エンコード時間ではありません。

`pending_output_limit`はtexture以外のAMF内部メモリやmuxの全メモリ量の上限ではありません。VRAM全体の増加を防止できた、と読み替えないでください。

## 再現コマンド

```powershell
./scripts/Run-Tests.ps1 -Suite All -Ymm4Directory 'D:/YukkuriMovieMaker4/YMM4修正版'
./scripts/Run-OutputWaitBench.ps1 -Experiment InputRecycling -Pairs 4 -Frames 900 -Quality quality -Profile
./scripts/Run-OutputWaitBench.ps1 -Experiment InputRecycling -Pairs 2 -Frames 900 -Quality quality -PoolSizes 4,6,8
```

スクリプト名は既存の待機比較と共通ですが、`-Experiment InputRecycling`では待機方式を固定して入力再利用だけを切り替えます。待機最適化を両側でONに固定する場合だけ`-EnableOutputWaitForInputComparison`を付けます。

同じ品質設定を維持しても、MF品質固定100以上の画質や速度を証明したことにはなりません。MFとの同条件・同品質比較は未完了です。また、単体ベンチにはYMM4の素材読み込み・描画が含まれません。
