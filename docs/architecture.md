# AMF版アーキテクチャ（0.2.0）

## データ経路

```text
YMM4 render thread
  ID2D1Bitmap1
    -> IDXGISurface
    -> borrowed ID3D11Texture2D
    -> CopyResource (呼び出し中のみ)
owned BGRA/RGBA texture pool (4/6/8)
    -> AMFSurface wrapper
    -> AMF EFC
    -> H.264 / HEVC hardware encoder
    -> QueryOutput worker
    -> Annex-B parser / MP4 sample writer
    -> AAC + MP4 trailer
```

## 所有権

AMF Writerは公開`IVideoFileWriter3`を実装する。出力設定の「GPU直渡し」がOFFなら`IsGpuFrameSupported=false`として従来のCpuRead bitmap経由、ONなら描画済みGPU bitmapを受け取る。選択はWriter生成時に固定する。どちらの経路でも以下の所有プールへのコピーは維持し、完全Zero Copyにはしない。別ツールやHarmonyへの依存はない。

借用textureは`AmfEncode`から戻るまでしか参照しない。各slotは`free -> copying -> submitted -> output-observed -> free`の順で進む。入力surfaceへslot indexをpropertyとして設定し、AMFが出力dataへ伝播した値を確認してからslotを解放する。propertyが欠落した場合は推測でslotを再利用せず、出力を失敗させる。

この方式はAMF内部で入力surfaceが早く解放される場合にも安全側となる。実際の入力解放より遅い「対応する出力を観測した時点」を再利用境界にするためである。B-frameは0に固定しているが、slotの解放自体は出力順ではなくpropertyで対応付ける。

## D3D11同期

ImmediateContextを使う`CopyResource`はYMM4の`WriteVideo`呼び出しthread上だけで行う。background workerはImmediateContextを操作しない。ホストのmultithread protection設定を変更せず、ホストが所有するtextureをworkerへ渡さない。

この契約はYMM4実機E2Eで確認が必要である。対象版が`WriteVideo`と同じtextureを並行更新する契約なら、別の同期方式が必要になる。

## AMF処理

`amfrt64.dll`は`LOAD_LIBRARY_SEARCH_SYSTEM32`で読み込む。AMF contextはYMM4由来のD3D11 deviceで初期化する。H.264 / HEVCともTranscoding usage、選択quality、CBRまたはpeak-constrained VBR、明示fps/framesize/bitrateを設定する。

出力threadは`QueryOutput`を継続し、`AMF_REPEAT`等では1ms待つ。正常終了は入力停止、`Drain`受理、`AMF_EOF`、worker join、accepted/completed照合、AAC flush、MP4 trailer、closeの順となる。60秒以内にDrainが完了しない場合は成功扱いにしない。

## 現在の計測境界

`RadeonBench`のwall timeは最初のfixture生成・uploadからMP4 closeまでを含む。出力JSONの`completed_fps`はaccepted frame数と正常Drainを前提にするが、decode検証は別プロセスで行う。YMM4描画時間は測定対象外。他アプリのGPU負荷などの外乱は測定値に影響するため、比較時に揃える必要がある。

## 検証状況と今後の調査候補

1. YMM4配置を指定したmanaged buildとH.264の短い実プロジェクトE2Eは完了。HEVCのYMM4 E2Eは未検証。
2. ffprobe / full decode、frame marker順序、色メタデータ、音声duration差をrun JSONへ取り込むvalidatorは実装済み。
3. VideoProcessor経路はRX 6800 XTのYMM4実測で既存経路より約7%低速だったため、実装と設定UIから廃止した。比較値はベンチ記録にのみ残す。
4. 0.1.1の所有プール・エンコード手順は維持する。0.2.0では既定OFFのGPU入力切替と区間計測を追加し、通常出力と破棄モードで比較できるようにしている。CPU側の計測だけではAMFのハードウェア上限やGPU内の処理時間は判定できない。
5. 色差、VMAF、音声marker相互相関、HEVCのYMM4 E2E、長時間反復、キャンセル、device removalは未検証。

## 診断オプション

既存のコピー・エンコード手順を維持したまま、既定オフの破棄モードとプロファイリングを追加しています。破棄モードは`WriteVideo`でSurfaceを参照する前、`WriteAudio`でバッファーを保持する前に戻ります。AMFの初期化も行いません。通常出力の設定から作成したスナップショットで動作し、実行中のチェック変更には影響されません。

プロファイリングは固定サイズのCPU壁時計時間の集計です。管理側の受け取り・ロック・初期化・終了時間と、ネイティブ側の待機・投入・回収・mux・ファイル書き込みを別々に保存します。既存のAMF ABIは変更せず、計測開始とスナップショット取得の関数だけを追加しています。詳細と解釈上の制限は[プロファイリングガイド](profiling.md)を参照してください。
