# 出力回収の待機を最適化（実験）

v0.2.1-preview.1では既定ONに採用したテスト用機能です。公開済みv0.2.0には含まれません。導入と従来設定への切り替えは[テスト版の手順](release-notes/v0.2.1-preview.1.md)を参照してください。

AMF回収ループの待機を変える実験です。MFの仕組みを取り込む[入力寿命管理の実験](mf-input-recycling.md)とは別のオプションです。MFの品質固定100と同等以上の画質を保証するプリセットではありません。[実測結果](benchmarks/2026-09-17-output-wait.md)と[MF対AMFの画質比較手順](export-quality-comparison.md)を参照してください。

## 使い方

1. YMM4を終了し、同じ開発ビルドの`AMFPlugin.dll`と`AmfNative.dll`を配置します。ビルドすると両DLLは`artifacts/bin`へ自動配置されます。YMM4本体へのインストールは自動では行いません。
2. 動画出力で「Radeon (AMF) プラグイン出力」を選び、「出力回収の待機を最適化」がONであることを確認します。ツールやHarmonyは不要です。
3. 同じプロジェクト・範囲・解像度・fps・品質・ビットレート・プール枚数・GPU直渡し設定を使い、待機最適化だけをOFF/ONで比較します。各3〜5回、OFF→ON、ON→OFFと順番を交互にしてください。
4. 出力は別名にし、フレーム数・映像・音声・終了処理を確認します。設定は次回の書き出しから適用され、進行中の書き出しは変わりません。

仕組みの確認時だけプロファイリングをONにし、最終的な速度はプロファイリング・デバッグログともOFFで比較します。YMM4を再起動すると設定は初期値に戻ります。

## 変更する部分・しない部分

- OFF: 従来の`QueryOutput`と、出力がない場合の`std::this_thread::sleep_for(1ms)`。
- ON: `QueryTimeoutSupport`が利用可能なAMFに、初期化前に10msの期限付き`QueryOutput`待機を設定し、値を読み戻します。10msは毎フレーム必ず追加するsleepではありません。
- 対応していても短時間で空の結果が返る場合があります。その呼び出しが1ms未満なら、高精度waitable timerで1ms待ってから再試行します。忙しいループでCPUを回し続けず、粗いsleepの過剰待機を避けるためです。タイマーを利用できない／待機に失敗した場合は従来sleepへ戻ります。
- `timeBeginPeriod`などによるWindows全体のタイマー分解能変更、毎フレームのGPU Flush、スピン待機は追加しません。
- 未対応／能力取得失敗なら従来方式のままです。設定の拒否や読み戻し失敗では0msへ戻したことを確認し、戻せなければ初期化を失敗させます。未確認の無期限待機で続行しません。
- このチェック単体では画質・レート制御・色変換・コピー回数・プール枚数・Mux・GPU入力経路は変更しません。テスト版の推奨ボタンは別途プールも8枚へ設定します。
- 最適化時のDestroyは、回収スレッドを停止・joinしてからFlushします。既存の`AmfCreate` ABIは維持し、追加の`AmfCreateWithOutputWait`をON時だけ使います。

AMFの機能定義は[AMDのH.264ヘッダー](https://github.com/GPUOpen-LibrariesAndSDKs/AMF/blob/master/amf/public/include/components/VideoEncoderVCE.h)、設定時期はSDKの`EncoderParamsAVC.cpp` / `EncoderParamsHEVC.cpp`および`EncoderLatency.cpp`を参照しています。高精度タイマーは[Microsoftの公開API](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw)を使用します。MFの内部処理をコピーしたものではなく、描画とエンコードを並行させる既存AMFパイプラインの回収遅延を改善するものです。

## 適用されたか調べる

プロファイルの`output_wait`またはRadeonBenchの同名項目を確認します。

| 項目 | 意味 |
| --- | --- |
| `requested` | チェックの指定値。trueだけでは適用されたとは限りません。 |
| `effective_mode` | `amf_query_timeout`なら10ms設定を確認済み。`poll_sleep_1ms`なら従来方式。 |
| `reason` | `enabled` / `disabled` / `unsupported` / `capability_unavailable` / `property_rejected` / `readback_failed` / `readback_mismatch` / `reset_failed` / `initialization_changed`。最後の二つは安全な設定を確定できず初期化失敗となります。 |
| `capability_result` / `property_result` | AMF_RESULT。0は成功、-1は未実行。 |
| `early_poll_waits` | 有効設定中でも1ms未満で空の結果が返り、追加待機に入った回数。 |
| `high_resolution_poll_waits` | 上記のうち高精度タイマー待機に成功した回数。差分は従来sleepに戻った回数です。 |

`enabled`はプロパティ確認済みを表し、すべての呼び出しが10ms待機した／速くなったという意味ではありません。破棄モードは`status: bypassed`、フレームが一度も届かなければ`not_initialized`です。古いDLLの混在時はONを黙って無視せず、DLL更新が必要なエラーを表示します。

## 開発者向け検証

```powershell
./scripts/Run-Tests.ps1 -Suite All -Ymm4Directory 'D:/YukkuriMovieMaker4/YMM4修正版'
./scripts/Run-OutputWaitBench.ps1 -Pairs 4 -Frames 900
./scripts/Run-OutputWaitBench.ps1 -Pairs 2 -Frames 900 -Profile
./scripts/Run-OutputWaitBench.ps1 -Pairs 2 -Frames 900 -Quality quality -Profile
```

後者はビルド済みの`artifacts/bin`を使う独立ベンチです。ウォームアップを別に行い、同一バイナリでAB/BA順に反復し、全動画をffmpegでデコードしてフレーム順・色メタデータ・音声長を検証します。出力フォルダーは毎回別に作成し、結果はその中の`summary.json`に保存します。`-PoolSizes 4,6,8`でも比較できます。

計測範囲は合成フレーム生成・upload・AMF・AAC・MP4までです。YMM4の読み込み・描画を含まないため、そのfpsをYMM4の速度としては扱いません。音声長の一致は聴感上の同期検証の代わりにはなりません。取消テストは独立ハーネスのDestroyであり、YMM4 GUIのキャンセルE2E・長時間出力・device lostは別途確認が必要です。
