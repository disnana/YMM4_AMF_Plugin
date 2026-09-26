using System.IO;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Windows.Controls;
using AMFVideoWriterPlugin;
using Vortice.Direct2D1;
using YukkuriMovieMaker.Project;

internal static partial class Program
{
    private static void RunInputRecyclingTests(string root)
    {
        Check(!new AmfSettings().RecycleInputAfterRelease, "MF-style input recycling defaults off");
        Check(Marshal.SizeOf<NativeInputRecycleStatus>() == 96, "Input-recycling ABI size");
        Check(Marshal.OffsetOf<NativeInputRecycleStatus>(nameof(NativeInputRecycleStatus.InputResidence)).ToInt32() == 72,
            "Input-recycling metric alignment");
        var status = new NativeInputRecycleStatus { Version = 1, Requested = 1, PoolSize = 6, MaxPendingOutputs = 12 };
        Check(Json(status.ToReport()).GetProperty("effective_mode").GetString() == "amf_surface_release", "Input mode report");
        status.PeakPendingOutputs = 13;
        try { status.ToReport(); throw new Exception("Out-of-bounds input status accepted"); }
        catch (InvalidOperationException) { _checks++; }
        foreach (var codec in Enum.GetValues<AmfCodec>())
        foreach (var quality in Enum.GetValues<AmfQuality>())
        foreach (var rate in Enum.GetValues<AmfRateControl>())
        {
            var settings = new AmfSettings { Codec = codec, Quality = quality, RateControl = rate, BitrateKbps = 27000,
                TexturePoolSize = 8, EnableGpuDirectInput = true, OptimizeOutputWait = true };
            string OtherOptions() => JsonSerializer.Serialize(new { settings.Codec, settings.Quality, settings.RateControl,
                settings.BitrateKbps, settings.TexturePoolSize, settings.EnableGpuDirectInput, settings.OptimizeOutputWait });
            var expected = OtherOptions();
            var panel = (StackPanel)new AmfConfigView(settings).Content;
            var box = panel.Children.OfType<CheckBox>().Single(x => x.Content.ToString()!.StartsWith("MF方式：入力解放"));
            box.IsChecked = true;
            Check(settings.RecycleInputAfterRelease && OtherOptions() == expected, "MF input option preserves quality and wait settings");
            box.IsChecked = false;
            Check(!settings.RecycleInputAfterRelease && OtherOptions() == expected, "MF input OFF preserves quality");
        }
        var plugin = new AmfVideoFileWriterPlugin();
        var info = new VideoInfo();
        var config = (StackPanel)((AmfConfigView)plugin.GetVideoConfigView("recycle", info, 1)).Content;
        var boxes = config.Children.OfType<CheckBox>().ToArray();
        boxes.Single(x => x.Content.ToString()!.StartsWith("計測専用")).IsChecked = true;
        boxes.Single(x => x.Content.ToString()!.StartsWith("プロファイリング")).IsChecked = true;
        var toggle = boxes.Single(x => x.Content.ToString()!.StartsWith("MF方式：入力解放"));
        toggle.IsChecked = true;
        var path = Path.Combine(root, "input-recycling-snapshot.mp4");
        using (var writer = (AmfVideoFileWriter)plugin.CreateVideoFileWriter(path, info))
        {
            toggle.IsChecked = false;
            writer.WriteVideo((ID2D1Bitmap1)null!); // Discard must not enter the experimental native path.
        }
        using var saved = JsonDocument.Parse(File.ReadAllText(path + ".amf_profile.json"));
        Check(saved.RootElement.GetProperty("configuration").GetProperty("recycle_input_after_release").GetBoolean(), "Factory snapshots input recycling");
        Check(saved.RootElement.GetProperty("input_recycling").GetProperty("status").GetString() == "bypassed", "Discard reports bypassed input recycling");
    }
}
