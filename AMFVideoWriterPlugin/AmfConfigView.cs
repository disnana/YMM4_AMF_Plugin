using System.Windows;
using System.Windows.Controls;

namespace AMFVideoWriterPlugin;

internal sealed class AmfConfigView : UserControl
{
    private readonly ComboBox _codecComboBox;
    private readonly ComboBox _rateControlComboBox;
    private readonly TextBox _bitrateTextBox;
    private readonly ComboBox _qualityComboBox;
    private readonly ComboBox _poolSizeComboBox;
    private readonly CheckBox _debugLogCheckBox;
    private readonly AmfSettings _settings;

    public AmfConfigView(AmfSettings settings)
    {
        _settings = settings;
        var panel = new StackPanel
        {
            Margin = new Thickness(8),
        };

        panel.Children.Add(new TextBlock
        {
            Text = "コーデック",
            Margin = new Thickness(0, 0, 0, 4),
        });

        _codecComboBox = new ComboBox
        {
            Margin = new Thickness(0, 0, 0, 12),
            ItemsSource = new[] { "H.264", "H.265 (HEVC)" },
            SelectedIndex = _settings.Codec switch
            {
                AmfCodec.H265 => 1,
                _ => 0,
            },
        };
        panel.Children.Add(_codecComboBox);

        panel.Children.Add(new TextBlock
        {
            Text = "所有テクスチャプール",
            Margin = new Thickness(0, 0, 0, 4),
        });

        _poolSizeComboBox = new ComboBox
        {
            ItemsSource = TexturePoolPolicy.Choices.Select(size => size <= 8 ? $"{size} 枚" : $"{size} 枚（増量・実験）").ToArray(),
            SelectedIndex = Math.Max(0, Array.IndexOf(TexturePoolPolicy.Choices, settings.TexturePoolSize)),
            ToolTip = "このテスト版の既定は8枚です。16枚以上は実験設定です。増量時は所有BGRA/RGBAテクスチャの画素量を目安1 GiBまでに制限（1080p:128枚、4K:32枚）。これは総VRAMの上限ではありません。画質や回収方式は変更せず、実効枚数はプロファイルに記録します。",
            Margin = new Thickness(0, 0, 0, 12),
        };
        _poolSizeComboBox.SelectionChanged += (_, _) =>
        {
            if (_poolSizeComboBox.SelectedIndex >= 0)
                _settings.TexturePoolSize = TexturePoolPolicy.Choices[_poolSizeComboBox.SelectedIndex];
        };
        panel.Children.Add(_poolSizeComboBox);

        var gpuDirectCheckBox = new CheckBox
        {
            Content = "GPU直渡し（実験・IVideoFileWriter3）",
            IsChecked = _settings.EnableGpuDirectInput,
            ToolTip = "ON: YMM4の描画済みGPUフレームを直接受け取り、CPU読み取り用ビットマップへのコピーを省きます。AMFの所有テクスチャプールへのGPUコピーは維持します。設定は次に開始する出力に適用されます。",
            Margin = new Thickness(0, 0, 0, 12),
        };
        gpuDirectCheckBox.Checked += (_, _) => _settings.EnableGpuDirectInput = true;
        gpuDirectCheckBox.Unchecked += (_, _) => _settings.EnableGpuDirectInput = false;
        panel.Children.Add(gpuDirectCheckBox);

        var recycleInputCheckBox = new CheckBox
        {
            Content = "MF方式：入力解放通知でテクスチャを再利用（実験）",
            IsChecked = _settings.RecycleInputAfterRelease,
            ToolTip = "MFのTrackedSample方式を参考に、AMFが入力を使い終えた通知で所有テクスチャを返却します。圧縮済み出力の回収とは寿命を分離し、出力待ち上限はプール枚数の2倍です。画質・ビットレート・コピー回数・待機最適化は変更しません。MFと同じ速度や画質を保証する設定ではありません。",
            Margin = new Thickness(0, 0, 0, 12),
        };
        recycleInputCheckBox.Checked += (_, _) => _settings.RecycleInputAfterRelease = true;
        recycleInputCheckBox.Unchecked += (_, _) => _settings.RecycleInputAfterRelease = false;
        panel.Children.Add(recycleInputCheckBox);

        var mfStyleNv12CheckBox = new CheckBox
        {
            Content = "MF方式：GPU VideoProcessorでNV12化してAMFへ投入（実験）",
            IsChecked = _settings.MfStyleNv12,
            ToolTip = "YMM4のBGRA/RGBAフレームを所有RGBテクスチャへGPUコピーし、D3D11 VideoProcessorでBT.709 limited-range NV12へ変換してAMFへ投入します。CPU読み戻しは行いません。従来のBGRA経路と圧縮設定は維持し、速度・色差・安定性を比較するための既定OFF機能です。専用D3D11デバイス実験とは併用できません。",
            Margin = new Thickness(0, 0, 0, 12),
        };
        mfStyleNv12CheckBox.Checked += (_, _) => _settings.MfStyleNv12 = true;
        mfStyleNv12CheckBox.Unchecked += (_, _) => _settings.MfStyleNv12 = false;
        panel.Children.Add(mfStyleNv12CheckBox);

        var outputWaitCheckBox = new CheckBox
        {
            Content = "出力回収の待機を最適化（このテスト版では推奨・既定ON）",
            IsChecked = _settings.OptimizeOutputWait,
            ToolTip = "対応するAMFでは期限付きQueryOutput待機と、即時に戻る場合の高精度タイマー待機を使います。非対応なら従来方式を使用します。画質・GPU直渡し・プール枚数は変更しません。適用結果はデバッグログ／プロファイルへ記録し、設定は次の出力から反映します。",
            Margin = new Thickness(0, 0, 0, 12),
        };
        outputWaitCheckBox.Checked += (_, _) => _settings.OptimizeOutputWait = true;
        outputWaitCheckBox.Unchecked += (_, _) => _settings.OptimizeOutputWait = false;
        panel.Children.Add(outputWaitCheckBox);

        var inputWaitCheckBox = new CheckBox
        {
            Content = "入力満杯時を進捗通知で待機（実験）",
            IsChecked = _settings.AdaptiveInputWait,
            ToolTip = "AMF_INPUT_FULL時の固定1ms sleepを、圧縮済み出力の完了通知待ちに置き換えます。入力解放は診断にだけ記録し、通知欠落やドライバー差への保険として10msで再確認します。画質・ビットレート・プール枚数・コピー回数は変更しません。",
            Margin = new Thickness(0, 0, 0, 12),
        };
        inputWaitCheckBox.Checked += (_, _) => _settings.AdaptiveInputWait = true;
        inputWaitCheckBox.Unchecked += (_, _) => _settings.AdaptiveInputWait = false;
        panel.Children.Add(inputWaitCheckBox);

        var asyncCheckBox = new CheckBox
        {
            Content = "AMF投入を専用スレッドへ分離（実験）",
            IsChecked = _settings.AsyncSubmission,
            ToolTip = "所有テクスチャへコピーした後、AMFへの投入を有界キュー経由で別スレッドへ渡します。キューは既存プールの枠内で、画質・プール枚数・回収方式は変えません。終了時は全キューを投入してからDrainします。高速化は環境・素材依存です。",
            Margin = new Thickness(0, 0, 0, 12),
        };
        asyncCheckBox.Checked += (_, _) => _settings.AsyncSubmission = true;
        asyncCheckBox.Unchecked += (_, _) => _settings.AsyncSubmission = false;
        panel.Children.Add(asyncCheckBox);

        var deviceCheckBox = new CheckBox
        {
            Content = "エンコーダー専用D3D11デバイス（実験）",
            IsChecked = _settings.DedicatedEncoderDevice,
            ToolTip = "同じGPUに専用デバイスを作成し、共有テクスチャ経由で2回GPUコピーします（通常は1回）。共有用BGRA/RGBAテクスチャを1枚追加します。AMF投入スレッドとは独立した設定です。CPU読み戻しは追加せず、非対応時は明示エラーとします。",
            Margin = new Thickness(0, 0, 0, 12),
        };
        deviceCheckBox.Checked += (_, _) => _settings.DedicatedEncoderDevice = true;
        deviceCheckBox.Unchecked += (_, _) => _settings.DedicatedEncoderDevice = false;
        panel.Children.Add(deviceCheckBox);

        // Apply through the controls so the visible settings and export snapshot stay in sync.
        void ApplyPerformancePreset(bool optimized)
        {
            _poolSizeComboBox.SelectedIndex = Array.IndexOf(TexturePoolPolicy.Choices, optimized ? 8 : 6);
            outputWaitCheckBox.IsChecked = optimized;
            gpuDirectCheckBox.IsChecked = false;
            recycleInputCheckBox.IsChecked = false;
            mfStyleNv12CheckBox.IsChecked = false;
            inputWaitCheckBox.IsChecked = false;
            asyncCheckBox.IsChecked = false;
            deviceCheckBox.IsChecked = false;
        }
        var optimizedButton = new Button
        {
            Name = "OptimizedPresetButton",
            Content = "推奨設定：8枚・出力待機最適化ON",
            Margin = new Thickness(0, 0, 0, 4),
        };
        optimizedButton.Click += (_, _) => ApplyPerformancePreset(true);
        var legacyButton = new Button
        {
            Name = "LegacyPresetButton",
            Content = "比較用の従来設定：6枚・出力待機最適化OFF",
            Margin = new Thickness(0, 0, 0, 4),
        };
        legacyButton.Click += (_, _) => ApplyPerformancePreset(false);
        panel.Children.Insert(0, optimizedButton);
        panel.Children.Insert(1, legacyButton);
        panel.Children.Insert(2, new TextBlock
        {
            Text = "高速化テスト版：推奨設定で開始します。両ボタンは画質・ビットレート・診断設定を維持し、他の実験機能をOFFにします。次の書き出しから適用されます。",
            TextWrapping = TextWrapping.Wrap,
            Margin = new Thickness(0, 0, 0, 12),
        });

        _codecComboBox.SelectionChanged += (_, _) =>
        {
            _settings.Codec = _codecComboBox.SelectedIndex switch
            {
                1 => AmfCodec.H265,
                _ => AmfCodec.H264,
            };
        };

        _debugLogCheckBox = new CheckBox
        {
            Content = "デバッグログを書き出す",
            IsChecked = _settings.EnableDebugLog,
            Margin = new Thickness(0, 0, 0, 12),
        };
        _debugLogCheckBox.Checked += (_, _) => _settings.EnableDebugLog = true;
        _debugLogCheckBox.Unchecked += (_, _) => _settings.EnableDebugLog = false;
        panel.Children.Add(_debugLogCheckBox);

        var profilingCheckBox = new CheckBox
        {
            Content = "プロファイリング結果を書き出す（集計JSON）",
            IsChecked = _settings.EnableProfiling,
            ToolTip = "デバッグログとは独立した時間計測です。終了時に、出力名.amf_profile.jsonへ保存します。",
            Margin = new Thickness(0, 0, 0, 8),
        };
        profilingCheckBox.Checked += (_, _) => _settings.EnableProfiling = true;
        profilingCheckBox.Unchecked += (_, _) => _settings.EnableProfiling = false;
        panel.Children.Add(profilingCheckBox);

        var discardCheckBox = new CheckBox
        {
            Content = "計測専用：映像・音声を破棄（MP4を作成しない）",
            IsChecked = _settings.DiscardOutput,
            Margin = new Thickness(0, 0, 0, 4),
        };
        discardCheckBox.Checked += (_, _) => _settings.DiscardOutput = true;
        discardCheckBox.Unchecked += (_, _) => _settings.DiscardOutput = false;
        panel.Children.Add(discardCheckBox);
        panel.Children.Add(new TextBlock
        {
            Text = "破棄モードではプラグイン内のコピー・エンコード・動画保存を行いません。GPU直渡しOFFではYMM4側の入力前コピーが残ります。\n必ず新しい出力名を使い、集計JSONが必要ならプロファイリングもオンにしてください。",
            TextWrapping = TextWrapping.Wrap,
            Margin = new Thickness(0, 0, 0, 12),
        });

        panel.Children.Add(new TextBlock
        {
            Text = "ビットレート方式",
            Margin = new Thickness(0, 0, 0, 4),
        });

        _rateControlComboBox = new ComboBox
        {
            Margin = new Thickness(0, 0, 0, 12),
            ItemsSource = new[] { "固定 (CBR)", "可変 (VBR)", "自動 (YouTube 推奨)" },
            SelectedIndex = _settings.RateControl switch
            {
                AmfRateControl.Variable => 1,
                AmfRateControl.YouTubeRecommended => 2,
                _ => 0,
            },
        };
        panel.Children.Add(_rateControlComboBox);

        panel.Children.Add(new TextBlock
        {
            Text = "出力品質",
            Margin = new Thickness(0, 0, 0, 4),
        });

        _qualityComboBox = new ComboBox
        {
            Margin = new Thickness(0, 0, 0, 12),
            ItemsSource = new[] { "高速", "標準（通常はこちらを推奨）", "高品質（低速・画質差を要確認）" },
            SelectedIndex = (int)_settings.Quality,
        };
        _qualityComboBox.SelectionChanged += (_, _) =>
        {
            _settings.Quality = (AmfQuality)Math.Clamp(_qualityComboBox.SelectedIndex, 0, 2);
        };
        panel.Children.Add(_qualityComboBox);
        panel.Children.Add(new TextBlock
        {
            Text = "通常は標準を推奨します。高品質は圧縮処理の手間を増やす設定で、MFの品質100と同義ではありません。\n検証した2素材では標準との差が小さく、合成映像では処理時間が約2.2倍でした。素材によって効果が変わるため、高品質は差を確認して選択してください。",
            TextWrapping = TextWrapping.Wrap,
            Margin = new Thickness(0, 0, 0, 12),
        });

        panel.Children.Add(new TextBlock
        {
            Text = "ビットレート（kbps）",
            Margin = new Thickness(0, 0, 0, 4),
        });

        _bitrateTextBox = new TextBox
        {
            Text = _settings.BitrateKbps.ToString(),
            Margin = new Thickness(0, 0, 0, 8),
            IsEnabled = _settings.RateControl != AmfRateControl.YouTubeRecommended,
        };
        _rateControlComboBox.SelectionChanged += (_, _) =>
        {
            _settings.RateControl = _rateControlComboBox.SelectedIndex switch
            {
                1 => AmfRateControl.Variable,
                2 => AmfRateControl.YouTubeRecommended,
                _ => AmfRateControl.Fixed,
            };
            if (_settings.RateControl == AmfRateControl.YouTubeRecommended)
            {
                _bitrateTextBox.IsEnabled = false;
                return;
            }

            _bitrateTextBox.IsEnabled = true;
            _bitrateTextBox.Text = _settings.BitrateKbps.ToString();
        };
        _bitrateTextBox.TextChanged += (_, _) =>
        {
            if (int.TryParse(_bitrateTextBox.Text, out var value))
            {
                _settings.BitrateKbps = Math.Clamp(value, 100, 200000);
            }
        };
        panel.Children.Add(_bitrateTextBox);

        Content = panel;
    }
}
