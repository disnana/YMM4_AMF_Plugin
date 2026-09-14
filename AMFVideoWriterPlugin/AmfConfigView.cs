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
            ItemsSource = new[] { "4 枚", "6 枚", "8 枚" },
            SelectedIndex = settings.TexturePoolSize switch { 6 => 1, 8 => 2, _ => 0 },
            Margin = new Thickness(0, 0, 0, 12),
        };
        _poolSizeComboBox.SelectionChanged += (_, _) =>
        {
            _settings.TexturePoolSize = _poolSizeComboBox.SelectedIndex switch { 1 => 6, 2 => 8, _ => 4 };
        };
        panel.Children.Add(_poolSizeComboBox);

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
            ItemsSource = new[] { "高速", "標準", "高品質" },
            SelectedIndex = (int)_settings.Quality,
        };
        _qualityComboBox.SelectionChanged += (_, _) =>
        {
            _settings.Quality = (AmfQuality)Math.Clamp(_qualityComboBox.SelectedIndex, 0, 2);
        };
        panel.Children.Add(_qualityComboBox);

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
