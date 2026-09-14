# AMF版アーキテクチャ（0.1.0）

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

借用textureは`AmfEncode`から戻るまでしか参照しない。各slotは`free -> copying -> submitted -> output-observed -> free`の順で進む。入力surfaceへslot indexをpropertyとして設定し、AMFが出力dataへ伝播した値を確認してからslotを解放する。propertyが欠落した場合は推測でslotを再利用せず、出力を失敗させる。

この方式はAMF内部で入力surfaceが早く解放される場合にも安全側となる。実際の入力解放より遅い「対応する出力を観測した時点」を再利用境界にするためである。B-frameは0に固定しているが、slotの解放自体は出力順ではなくpropertyで対応付ける。

## D3D11同期

ImmediateContextを使う`CopyResource`はYMM4の`WriteVideo`呼び出しthread上だけで行う。background workerはImmediateContextを操作しない。ホストのmultithread protection設定を変更せず、ホストが所有するtextureをworkerへ渡さない。

この契約はYMM4実機E2Eで確認が必要である。対象版が`WriteVideo`と同じtextureを並行更新する契約なら、別の同期方式が必要になる。

## AMF処理

`amfrt64.dll`は`LOAD_LIBRARY_SEARCH_SYSTEM32`で読み込む。AMF contextはYMM4由来のD3D11 deviceで初期化する。H.264 / HEVCともTranscoding usage、選択quality、CBRまたはpeak-constrained VBR、明示fps/framesize/bitrateを設定する。

出力threadは`QueryOutput`を継続し、`AMF_REPEAT`等では1ms待つ。正常終了は入力停止、`Drain`受理、`AMF_EOF`、worker join、accepted/completed照合、AAC flush、MP4 trailer、closeの順となる。60秒以内にDrainが完了しない場合は成功扱いにしない。

## 現在の計測境界

`RadeonBench`のwall timeは最初のfixture生成・uploadからMP4 closeまでを含む。出力JSONの`completed_fps`はaccepted frame数と正常Drainを前提にするが、decode検証は別プロセスで行う。YMM4描画時間、品質比較、P2経路との競合比較は含まれない。

## 次の実装順

1. YMM4配置を指定したmanaged buildと短い実プロジェクトE2E。
2. P2（VideoProcessor -> owned NV12 pool）とP1の同条件比較。
3. ffprobe / full decode結果をrun JSONへ取り込むvalidator。
4. frame marker、色、音声同期の自動検証。
5. 長時間反復、キャンセル、device removal、release packaging。
