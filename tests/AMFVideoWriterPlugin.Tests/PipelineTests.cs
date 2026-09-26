using System.IO;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Windows.Controls;
using AMFVideoWriterPlugin;
using Vortice.Direct2D1;
using YukkuriMovieMaker.Project;

internal static partial class Program
{
    private static void RunPipelineTests(string root)
    {
        var defaults = new AmfSettings();
        Check(!defaults.AsyncSubmission && !defaults.DedicatedEncoderDevice && !defaults.AdaptiveInputWait,
            "Pipeline experiments default OFF");
        Check(defaults.Quality == AmfQuality.Balanced, "Quality guidance preserves Balanced default");
        foreach (var quality in Enum.GetValues<AmfQuality>())
        {
            var settings = new AmfSettings { Quality = quality, BitrateKbps = 27000, TexturePoolSize = 16,
                RecycleInputAfterRelease = true, OptimizeOutputWait = true };
            var panel = (StackPanel)new AmfConfigView(settings).Content;
            var asyncBox = panel.Children.OfType<CheckBox>().Single(x => x.Content.ToString()!.StartsWith("AMF投入"));
            var deviceBox = panel.Children.OfType<CheckBox>().Single(x => x.Content.ToString()!.StartsWith("エンコーダー専用"));
            var inputWaitBox = panel.Children.OfType<CheckBox>().Single(x => x.Content.ToString()!.StartsWith("入力満杯"));
            foreach (bool async in new[] { false, true }) foreach (bool device in new[] { false, true })
            {
                asyncBox.IsChecked = async; deviceBox.IsChecked = device;
                Check(settings.AsyncSubmission == async && settings.DedicatedEncoderDevice == device,
                    "Independent pipeline toggles");
                Check(settings.Quality == quality && settings.BitrateKbps == 27000 && settings.TexturePoolSize == 16
                    && settings.RecycleInputAfterRelease && settings.OptimizeOutputWait, "Pipeline toggles preserve encode/pool settings");
            }
            inputWaitBox.IsChecked = true;
            Check(settings.AdaptiveInputWait && settings.Quality == quality && settings.BitrateKbps == 27000
                && settings.TexturePoolSize == 16 && settings.RecycleInputAfterRelease && settings.OptimizeOutputWait,
                "Input-full wait toggle preserves encode/pool settings");
            inputWaitBox.IsChecked = false;
            Check(panel.Children.OfType<TextBlock>().Any(t => t.Text.Contains("MFの品質100と同義ではありません")),
                "UI explains high-quality/MF distinction without removing the preset");
        }
        Check(Marshal.SizeOf<NativePipelineStatus>() == 88, "Pipeline ABI size");
        Check(Marshal.OffsetOf<NativePipelineStatus>(nameof(NativePipelineStatus.QueuedFrames)).ToInt32() == 32, "Pipeline ABI alignment");
        var status = new NativePipelineStatus { Version = 1, Initialized = 1, AsyncSubmission = 1, DedicatedDevice = 1,
            QueueCapacity = 16, PeakQueueDepth = 8, GpuCopiesPerFrame = 2, QueuedFrames = 100, SubmittedFrames = 100 };
        Check(Json(status.ToReport()).GetProperty("gpu_copies_per_frame").GetInt32() == 2, "Dedicated path reports two copies");
        status.GpuCopiesPerFrame = 1;
        try { status.ToReport(); throw new Exception("Contradictory pipeline accepted"); } catch (InvalidOperationException) { _checks++; }
        status.GpuCopiesPerFrame = 2; status.QueueDepth = 17;
        try { status.ToReport(); throw new Exception("Unbounded pipeline accepted"); } catch (InvalidOperationException) { _checks++; }
        Check(Marshal.SizeOf<NativeInputWaitStatus>() == 80, "Input-full wait ABI size");
        Check(Marshal.OffsetOf<NativeInputWaitStatus>(nameof(NativeInputWaitStatus.Waits)).ToInt32() == 16,
            "Input-full wait ABI alignment");
        var waitStatus = new NativeInputWaitStatus { Version = 1, Requested = 1, Mode = 1,
            FallbackTimeoutMs = 10, Waits = 3, SignaledWaits = 2, TimedOutWaits = 1,
            Wait = new() { Count = 3, TotalNanoseconds = 6_000_000, MaxNanoseconds = 3_000_000 } };
        Check(Json(waitStatus.ToReport()).GetProperty("wait").GetProperty("mean_ms").GetDouble() == 2,
            "Input-full wait report units");
        waitStatus.TimedOutWaits = 2;
        try { waitStatus.ToReport(); throw new Exception("Invalid input-full wait status accepted"); }
        catch (InvalidOperationException) { _checks++; }

        var plugin = new AmfVideoFileWriterPlugin();
        var info = new VideoInfo();
        var boxes = ((StackPanel)((AmfConfigView)plugin.GetVideoConfigView("pipeline", info, 1)).Content).Children.OfType<CheckBox>().ToArray();
        foreach (var prefix in new[] { "AMF投入", "エンコーダー専用", "入力満杯", "プロファイリング", "計測専用" })
            boxes.Single(x => x.Content.ToString()!.StartsWith(prefix)).IsChecked = true;
        var path = Path.Combine(root, "pipeline-discard.mp4");
        using (var writer = (AmfVideoFileWriter)plugin.CreateVideoFileWriter(path, info))
        {
            boxes.Single(x => x.Content.ToString()!.StartsWith("AMF投入")).IsChecked = false;
            boxes.Single(x => x.Content.ToString()!.StartsWith("エンコーダー専用")).IsChecked = false;
            writer.WriteVideo((ID2D1Bitmap1)null!); // No native access, despite both flags ON in snapshot.
        }
        using var saved = JsonDocument.Parse(File.ReadAllText(path + ".amf_profile.json"));
        var config = saved.RootElement.GetProperty("configuration");
        Check(config.GetProperty("async_submission").GetBoolean() && config.GetProperty("dedicated_encoder_device").GetBoolean(),
            "Factory captures pipeline configuration for active export");
        Check(config.GetProperty("adaptive_input_wait").GetBoolean(),
            "Factory captures input-full wait configuration for active export");
        Check(saved.RootElement.GetProperty("pipeline").GetProperty("status").GetString() == "bypassed", "Discard bypasses pipeline");
        Check(!File.Exists(path), "Discard creates no MP4");
    }
}
