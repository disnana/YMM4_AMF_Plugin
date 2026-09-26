using System.IO;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using AMFVideoWriterPlugin;
using Vortice.Direct2D1;
using YukkuriMovieMaker.Project;

internal static partial class Program
{
    private static void RunOptimizationPresetTests(string root)
    {
        foreach (var codec in Enum.GetValues<AmfCodec>())
        foreach (var quality in Enum.GetValues<AmfQuality>())
        foreach (var rate in Enum.GetValues<AmfRateControl>())
        {
            var settings = new AmfSettings
            {
                Codec = codec, Quality = quality, RateControl = rate, BitrateKbps = 27000,
                TexturePoolSize = 32, OptimizeOutputWait = false, EnableGpuDirectInput = true,
                RecycleInputAfterRelease = true, MfStyleNv12 = true, AsyncSubmission = true,
                DedicatedEncoderDevice = true, AdaptiveInputWait = true,
                EnableDebugLog = true, EnableProfiling = true, DiscardOutput = true,
            };
            string Unrelated() => JsonSerializer.Serialize(new { settings.Codec, settings.Quality,
                settings.RateControl, settings.BitrateKbps, settings.EnableDebugLog,
                settings.EnableProfiling, settings.DiscardOutput });
            string original = Unrelated();
            var panel = (StackPanel)new AmfConfigView(settings).Content;
            foreach (bool optimized in new[] { true, false, true })
            {
                var button = panel.Children.OfType<Button>().Single(b => b.Name ==
                    (optimized ? "OptimizedPresetButton" : "LegacyPresetButton"));
                button.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
                Check(settings.TexturePoolSize == (optimized ? 8 : 6)
                    && settings.OptimizeOutputWait == optimized, "Preset changes pool and output waiting together");
                Check(!settings.EnableGpuDirectInput && !settings.RecycleInputAfterRelease && !settings.MfStyleNv12
                    && !settings.AsyncSubmission && !settings.DedicatedEncoderDevice && !settings.AdaptiveInputWait,
                    "Preset clears unrelated performance experiments");
                Check(Unrelated() == original, "Preset preserves compression quality, rate control and diagnostics");
            }
        }

        var plugin = new AmfVideoFileWriterPlugin();
        var info = new VideoInfo { Width = 1920, Height = 1080, FPS = 60, Hz = 48000 };
        var view = (StackPanel)((AmfConfigView)plugin.GetVideoConfigView("preview", info, 1)).Content;
        view.Children.OfType<CheckBox>().Single(b => b.Content.ToString()!.StartsWith("プロファイリング")).IsChecked = true;
        view.Children.OfType<CheckBox>().Single(b => b.Content.ToString()!.StartsWith("計測専用")).IsChecked = true;
        string path = Path.Combine(root, "optimized-snapshot.mp4");
        using (var writer = (AmfVideoFileWriter)plugin.CreateVideoFileWriter(path, info))
        {
            view.Children.OfType<Button>().Single(b => b.Name == "LegacyPresetButton")
                .RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
            writer.WriteVideo((ID2D1Bitmap1)null!);
        }
        using var first = JsonDocument.Parse(File.ReadAllText(path + ".amf_profile.json"));
        var configuration = first.RootElement.GetProperty("configuration");
        Check(configuration.GetProperty("pool_size").GetInt32() == 8
            && configuration.GetProperty("optimize_output_wait").GetBoolean(),
            "Changing the preset cannot mutate an existing export");
        string next = Path.Combine(root, "legacy-snapshot.mp4");
        using (var writer = (AmfVideoFileWriter)plugin.CreateVideoFileWriter(next, info)) { }
        using var second = JsonDocument.Parse(File.ReadAllText(next + ".amf_profile.json"));
        configuration = second.RootElement.GetProperty("configuration");
        Check(configuration.GetProperty("pool_size").GetInt32() == 6
            && !configuration.GetProperty("optimize_output_wait").GetBoolean(),
            "The comparison preset applies to the next export");
    }
}
