# 破棄モードとプロファイリング

目的は、最適化を加える前に「YMM4からフレームが届くまで」と「プラグイン内の待ち・処理」を切り分けることです。通常出力のアルゴリズムや画質設定を変える機能ではありません。両オプションは既定でオフです。

診断機能はv0.2.0で追加しました。開発時の自動テストと計測負荷の結果は[検証記録](benchmarks/2026-09-16-profiling.md)を参照してください。v0.1.1には含まれません。

## YMM4で比較する手順

1. 同じビルドの`AMFPlugin.dll`と`AmfNative.dll`を配置してYMM4を起動します。制作プロジェクトは先に保存してください。
2. 同じプロジェクト・出力範囲・解像度・fps・コーデック・ビットレート・品質・プール枚数を使います。他アプリのGPU負荷やドライバーも揃えてください。
3. 「プロファイリング結果を書き出す」をオン、「デバッグログを書き出す」と「計測専用：映像・音声を破棄」をオフにし、通常出力を新しい名前（例：`通常1.mp4`）で実行します。MP4の再生・映像枚数・音声を確認してください。
4. 次に破棄のチェックもオンにし、別の新しい名前（例：`破棄1.mp4`）で実行します。映像・音声を捨てるため、MP4は生成しません。
5. それぞれの`<出力名>.amf_profile.json`を比較します。通常→破棄、破棄→通常と順番を交互にし、各3〜5回とウォームアップを取ってください。両方で受け取りフレーム数と音声サンプル数が同じこと、YMM4で選択した範囲の処理が完了したことを確認します。
6. 計測自体の負荷は、通常出力のプロファイリングをオフ・オンにして別に比較します。調査後は両オプションをオフへ戻してください。

既存のMP4は破棄モードでは更新されません。古い動画を新しい出力と誤認しないよう、必ず新しい出力名を使ってください。YMM4自身による完了通知・保存先処理はプラグイン外の動作です。破棄モードのYMM4実操作E2Eは別途確認が必要です。

## デバッグログとの違い

### GPU直渡しの比較（実験機能）

動画出力のAMF設定にある「GPU直渡し（実験・IVideoFileWriter3）」で切り替えます。別のツールやHarmonyは不要です。設定はWriter生成時に固定され、チェックの変更は次回の出力にだけ影響します。OFFは従来どおりYMM4のCpuReadビットマップ経由、ONは描画済みGPUビットマップ経由です。どちらも既存のAMF所有テクスチャへGPUコピーしてからエンコードします。完全Zero CopyやCPUエンコードの切り替えではありません。

`configuration.gpu_direct_input_enabled`は開始時の設定、`input_delivery_path`はbitmapコールバックで選択経路を記録するフィールドです。`gpu_direct_IVideoFileWriter3`または`cpu_read_bitmap_IVideoFileWriter2`になり、bitmap呼び出しがなければ`not_observed`です。実GPU時間の測定値ではありません。

GPU ON/OFF × 通常/破棄を別々の新規出力名で比較してください。同じ条件でAB/BA順に繰り返し、実速度は外部計測ツール・デバッグ・プロファイリングをすべてOFFにした比較も取ります。破棄でもGPU直渡しOFFでは、プラグインに届く前のYMM4側中間コピーが残ります。従来の破棄測定を「中間コピーも取り除いた理論上限」とは扱いません。

独立実GPUテストと未検証項目は[v0.2.0リリースノート](release-notes/v0.2.0.md)にまとめています。GPU timestamp、フレーム別時系列、正確なp50/p95/p99、GPU使用率、GC pauseはこの版では記録しません。

### 計測ファイル

- デバッグログ（`.amf_log.txt`）：処理やエラーを追う文字列ログ。オンにすると実行中にファイルへ追記します。
- プロファイル（`.amf_profile.json`）：回数・合計・平均・最大時間の集計。フレームごとの一覧を保持せず、終了時にまとめて書き出します。同名JSONは置き換えます。
- 破棄モード：AMF、D3Dテクスチャの取得・コピー、AAC、MP4処理を実行しません。デバッグログも生成しません。プロファイリングは別のチェックで有効にできます。

プロファイリングの計測中にGPU待機や`Flush`を追加していません。プロファイリングがオフなら時計の読み取り・集計・JSON出力を行いませんが、オプション分岐等の追加負荷が完全にゼロとは保証しません。

## JSONの読み方

`configuration`には版・ビルド識別子（assembly MVID）・解像度などの設定が入ります。GPU名・ドライバー・プロジェクト・出力範囲は別途記録してください。絶対パスは設定へ収録しませんが、エラーメッセージに含まれる場合があるため、共有前に確認してください。

| 区間・指標 | 意味と注意点 |
| --- | --- |
| `metrics.writer_lifetime_ms` / `received_fps` | Writer作成から終了処理後まで。JSON保存時間は含みません。fpsは受け取った映像枚数から計算した値で、デコード検証済みの出力fpsではありません。 |
| `metrics.delivery_span_ms` | 最初の映像または音声コールバックの開始から、最後のコールバック終了まで。 |
| `metrics.callback_union_ms` | 映像・音声を合わせて、少なくとも1つのコールバック内にいた時間。並行呼び出しは二重加算しません。 |
| `metrics.delivery_outside_callbacks_ms` | 上記delivery区間のうち、プラグインが映像・音声のコールバックを処理していなかった時間。YMM4の純粋な描画時間を直接測るものではありません。 |
| `managed_stages.video_callback` / `audio_callback` | 各コールバック全体。ロック待ち・初期化などを含みます。 |
| `texture_access` / `video_lock_wait` / `audio_lock_wait` | テクスチャのインターフェイス取得、管理側の映像・音声共通ロック取得待ち。 |
| `encoder_initialize` | 初回のAMF作成、所有テクスチャ作成、MP4開始、先行音声の処理。初回は定常状態と分けて見てください。 |
| `native_video` / `native_audio` | ネイティブAPI呼び出しの所要時間。下記ネイティブ区間を含みます。 |
| `native_finalize` / `native_destroy` / `dispose` | Drain・末尾処理、破棄、Writer終了処理全体。 |
| `native.stages.slot_wait` | 空きテクスチャを確保するまで（mutex取得とスロット探索を含む）。AMFだけでなく出力側の混雑でも増えます。 |
| `copy_resource_cpu` | `CopyResource`のCPU呼び出し時間。GPU上でコピーが完了するまでの時間ではありません。 |
| `submit_input` / `input_retry_wait` | AMFへの投入APIと、投入待ち再試行のsleep時間。`input_retries`はINPUT_FULL / NEED_MORE_INPUTによる再試行数です。 |
| `query_output` / `output_poll_wait` | 出力取得APIと、出力がまだないときのpoll待機。待機が長いだけでGPUが限界とは判断できません。 |
| `output_mux_wait` / `bitstream_mux` | 回収スレッドのmuxロック待ちと、映像ビットストリームの解析・書き込みキュー投入。 |
| `audio_write` | 音声API全体（muxロック・PCM処理・AAC処理を含む）。 |
| `writer_sample` | 書き込みスレッドのサンプル書き込みと索引更新。ファイルロック待ちを含み、ストレージへの物理書き込み完了時間そのものではありません。 |
| `slot_residence` | スロット確保から対応出力を処理して解放するまで。キュー、GPU変換・エンコード、回収側処理を含む滞留時間で、GPU単体の処理時間ではありません。 |
| `finalize` / `mp4_finalize` | ネイティブ終了処理全体と、AAC flush・writer join・MP4末尾処理。ネストしています。 |

各stageの`count` / `total_ms` / `mean_ms` / `max_ms`を確認します。区間は入れ子・非同期で重なるため、合計して全体時間や100%の内訳にしないでください。ネイティブ計測はAMF初期化完了後に開始し、Destroy直前にスナップショットを取得します。初期化・Destroy自体は管理側の指標で確認します。

## どこが詰まっているかを考える

- 破棄にしても所要時間があまり変わらず、通常出力のコールバック時間が小さい：YMM4側の素材読み込み・描画・音声生成等を優先して調べる手掛かりになります。ただし、それら個別の内訳はこのプラグインでは測定していません。
- 破棄で大きく短縮し、通常出力の`slot_wait`が長い：出力側の圧力が疑われます。`output_mux_wait`、`bitstream_mux`、`writer_sample`、音声処理も確認し、AMFハードウェアだけを原因と決めないでください。
- `writer_sample`や`mp4_finalize`が目立つ：保存先・他のI/O・書き込みキューや終了処理の調査候補です。
- 初回・終了だけ長い：短い動画では初期化とDrainの割合が大きくなります。長さの違う同条件の出力で比較してください。

これは原因候補を絞る計測です。破棄ではエンコード負荷だけでなく、GPU競合やバックプレッシャーもなくなり、上流の動作が変わります。また、GPU描画の完了を待つ機能でもありません。通常出力との差を「純粋なエンコード時間」「GPU性能の限界」とは扱わないでください。[CopyResourceも非同期のAPIです](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-copyresource)。GPU内の詳細分解には、別途GPUタイムスタンプ等の計測が必要です。

## 失敗・中断した計測

例外を観測した場合は`status: failed`と最初のエラーを記録します。計測ファイルの保存失敗で元のエラーを置き換えないようにしています。出力に成功しても計測JSONだけ保存できなかった場合は、その旨の例外になります。保存先の権限を確認してください。

`status: writer_disposed`はWriterの終了処理を観測した、という意味です。ホストAPIからキャンセル・選択範囲の完走を判別する通知を受け取らないため、`host_completion`は`unknown_no_completion_signal`です。中断結果や異なるフレーム数は速度比較に使わないでください。プロセス強制終了時にはJSONが残らない場合があります。

## ネイティブ単体での実行

ビルド後、リポジトリのルートで実行します。

```powershell
.\artifacts\bin\RadeonBench.exe run --output artifacts\runs\profile\output.mp4 --result artifacts\runs\profile\run.json --width 1920 --height 1080 --fps 60 --frames 900 --bitrate-kbps 12000 --max-bitrate-kbps 14400 --codec h264 --pool-size 6 --audio --profile --no-debug-log
```

`run.json`の`profiling`にネイティブ区間が入ります。通常比較では`--profile`だけを外し、`--no-debug-log`を付けたままにします。ベンチのフレーム生成・upload時間を含むので、YMM4全体の速度としては使えません。単体ベンチにはYMM4の破棄モードはありません。出力のデコード検証は[CONTRIBUTING](../CONTRIBUTING.md)の手順も参照してください。
