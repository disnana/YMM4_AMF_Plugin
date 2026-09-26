using System.IO;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Windows.Controls;
using AMFVideoWriterPlugin;
using Vortice.Direct2D1;
using YukkuriMovieMaker.Project;

internal static partial class Program
{
    private static void RunOutputWaitTests(string root)
    {
        Check(new AmfSettings().OptimizeOutputWait, "Preview enables the validated output-wait optimization");
        // The speed experiment must not silently lower any encoding-quality setting.
        foreach (var codec in Enum.GetValues<AmfCodec>())
        foreach (var quality in Enum.GetValues<AmfQuality>())
        foreach (var rateControl in Enum.GetValues<AmfRateControl>())
        {
            var settings = new AmfSettings { Codec = codec, Quality = quality, RateControl = rateControl,
                BitrateKbps = 27000, TexturePoolSize = 8, EnableGpuDirectInput = true };
            string UnrelatedSettings() => JsonSerializer.Serialize(new { settings.Codec, settings.Quality,
                settings.RateControl, settings.BitrateKbps, settings.TexturePoolSize, settings.EnableGpuDirectInput,
                settings.EnableDebugLog, settings.EnableProfiling, settings.DiscardOutput });
            var expected = UnrelatedSettings();
            var view = new AmfConfigView(settings);
            var toggle = ((StackPanel)view.Content).Children.OfType<CheckBox>()
                .Single(box => box.Content.ToString()!.StartsWith("出力回収"));
            toggle.IsChecked = true;
            Check(settings.OptimizeOutputWait && UnrelatedSettings() == expected, "Wait ON preserves quality and other options");
            toggle.IsChecked = false;
            Check(!settings.OptimizeOutputWait && UnrelatedSettings() == expected, "Wait OFF preserves quality and other options");
        }
        Check(Marshal.SizeOf<NativeOutputWaitStatus>() == 40, "Output-wait ABI size");
        Check(Marshal.OffsetOf<NativeOutputWaitStatus>(nameof(NativeOutputWaitStatus.EarlyPollWaits)).ToInt32() == 24,
            "Output-wait ABI counter alignment");
        var native = new NativeOutputWaitStatus { Version = 1, Requested = 1, TimeoutMs = 10, Reason = 1,
            EarlyPollWaits = 3, HighResolutionPollWaits = 2 };
        var report = Json(native.ToReport());
        Check(report.GetProperty("effective_mode").GetString() == "amf_query_timeout", "Effective wait mode");
        Check(report.GetProperty("high_resolution_poll_waits").GetInt32() == 2, "High-resolution wait counter");
        native.TimeoutMs = 0; native.Reason = 3;
        Check(Json(native.ToReport()).GetProperty("reason").GetString() == "unsupported", "Unsupported fallback is explicit");
        native.Version = 99;
        try { native.ToReport(); throw new Exception("Invalid wait ABI accepted"); }
        catch (InvalidOperationException) { _checks++; }
        native.Version = 1; native.Reason = 1;
        try { native.ToReport(); throw new Exception("Contradictory wait status accepted"); }
        catch (InvalidOperationException) { _checks++; }
        native.Reason = 8;
        Check(Json(native.ToReport()).GetProperty("effective_mode").GetString() == "initialization_failed",
            "Init-changed wait must not claim an active output mode");

        var plugin = new AmfVideoFileWriterPlugin();
        var info = new VideoInfo();
        var panel = (StackPanel)((AmfConfigView)plugin.GetVideoConfigView("wait", info, 1)).Content;
        var boxes = panel.Children.OfType<CheckBox>().ToArray();
        var waitBox = boxes.Single(box => box.Content.ToString()!.StartsWith("出力回収"));
        Check(waitBox.IsChecked == true, "Output-wait UI reflects the optimized preview default");
        boxes.Single(box => box.Content.ToString()!.StartsWith("プロファイリング")).IsChecked = true;
        boxes.Single(box => box.Content.ToString()!.StartsWith("計測専用")).IsChecked = true;
        waitBox.IsChecked = true;
        var path = Path.Combine(root, "wait-snapshot.mp4");
        using (var writer = (AmfVideoFileWriter)plugin.CreateVideoFileWriter(path, info))
        {
            waitBox.IsChecked = false;
            writer.WriteVideo((ID2D1Bitmap1)null!); // Must not call even the new native entry point in discard mode.
        }
        using var saved = JsonDocument.Parse(File.ReadAllText(path + ".amf_profile.json"));
        Check(saved.RootElement.GetProperty("configuration").GetProperty("optimize_output_wait").GetBoolean(),
            "Factory captures output-wait setting at export start");
        Check(saved.RootElement.GetProperty("output_wait").GetProperty("status").GetString() == "bypassed",
            "Discard cannot claim that wait optimization ran");
        var next = Path.Combine(root, "wait-next.mp4");
        using (var writer = (AmfVideoFileWriter)plugin.CreateVideoFileWriter(next, info)) { }
        using var nextSaved = JsonDocument.Parse(File.ReadAllText(next + ".amf_profile.json"));
        Check(!nextSaved.RootElement.GetProperty("configuration").GetProperty("optimize_output_wait").GetBoolean(),
            "Checkbox change applies to the next export");
    }
}
