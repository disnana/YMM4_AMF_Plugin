using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Windows.Controls;
using AMFVideoWriterPlugin;
using Vortice.Direct2D1;
using YukkuriMovieMaker.Project;

internal static partial class Program
{
    private static int _checks;
    private static void Check(bool condition, string message)
    {
        _checks++;
        if (!condition) throw new InvalidOperationException(message);
    }
    private static JsonElement Json(object value) => JsonSerializer.SerializeToElement(value);

    [STAThread]
    private static int Main(string[] args)
    {
        if (args.Length == 3 && args[0] == "--gpu")
            return RunGpuSmoke(Path.GetFullPath(args[1]), Path.GetFullPath(args[2]));
        var root = Path.GetFullPath(args.Single());
        Directory.CreateDirectory(root);
        // Any attempt to initialize/copy/encode in discard mode must fail this test, even on GPU-less CI.
        NativeLibrary.SetDllImportResolver(typeof(AmfVideoFileWriter).Assembly, (name, assembly, searchPath) =>
            name == "AmfNative.dll" ? throw new InvalidOperationException("Unexpected native call in CPU/sink tests") : IntPtr.Zero);

        var defaults = new AmfSettings();
        RunGpuDirectContractTests(root);
        Check(!defaults.DiscardOutput && !defaults.EnableProfiling && !defaults.EnableDebugLog, "Diagnostics must be opt-in");
        Check(Marshal.SizeOf<NativeProfileSnapshot>() == 344, "Native profile ABI size");
        Check(Marshal.OffsetOf<NativeProfileSnapshot>(nameof(NativeProfileSnapshot.Metrics)).ToInt32() == 32, "Native metric alignment");
        var native = new NativeProfileSnapshot { Version = 1, MetricCount = 13, Metrics = new NativeProfileMetric[13] };
        native.Metrics[0] = new() { Count = 2, TotalNanoseconds = 4_000_000, MaxNanoseconds = 3_000_000 };
        var nativeMetric = Json(native.ToReport()).GetProperty("stages").GetProperty("slot_wait");
        Check(nativeMetric.GetProperty("mean_ms").GetDouble() == 2, "Native units conversion");
        native.Version = 2;
        try { native.ToReport(); throw new Exception("Invalid ABI accepted"); }
        catch (InvalidOperationException) { _checks++; }

        // Deterministic overlapping callbacks: union time must not double-count audio + video.
        long clock = 100;
        var profile = new ExportProfiler(true, new { test = true }, () => clock, 1000);
        clock = 110;
        var video = profile.Callback(ExportStage.VideoCallback);
        clock = 120;
        var audio = profile.Callback(ExportStage.AudioCallback, 100);
        clock = 140;
        video.Dispose();
        clock = 150;
        audio.Dispose();
        clock = 160;
        var secondVideo = profile.Callback(ExportStage.VideoCallback);
        clock = 170;
        secondVideo.Dispose();
        clock = 180;
        profile.Finish();
        var report = Json(profile.CreateReport());
        var metrics = report.GetProperty("metrics");
        Check(metrics.GetProperty("writer_lifetime_ms").GetDouble() == 80, "Lifetime includes initial/final gaps");
        Check(metrics.GetProperty("callback_union_ms").GetDouble() == 50, "Callback union time");
        Check(metrics.GetProperty("delivery_outside_callbacks_ms").GetDouble() == 10, "Delivery gap time");
        Check(metrics.GetProperty("received_video_frames").GetInt32() == 2, "Video count");
        Check(metrics.GetProperty("received_audio_sample_values").GetInt32() == 100, "Audio values, not sample frames");
        Check(metrics.GetProperty("max_concurrent_callbacks").GetInt32() == 2, "Concurrent callback count");
        Check(report.GetProperty("native_status").GetString() == "bypassed", "Discard cannot claim native output");
        Check(!report.GetProperty("output_finalized").GetBoolean(), "Discard cannot claim MP4 output");
        Check(report.GetProperty("host_completion").GetString() == "unknown_no_completion_signal", "No unobservable success/cancellation claim");
        var empty = new ExportProfiler(false, new { }, () => clock, 1000);
        empty.Finish();
        Check(Json(empty.CreateReport()).GetProperty("metrics").GetProperty("received_fps").GetDouble() == 0, "Empty export has finite zero throughput");

        var unicodePath = Path.Combine(root, "日本語.mp4");
        profile.Save(unicodePath);
        Check(!File.Exists(unicodePath), "Report must not create an MP4");
        Check(File.Exists(unicodePath + ".amf_profile.json"), "Unicode report path");
        Check(!Directory.EnumerateFiles(root, "*.tmp").Any(), "No temporary report left behind");
        empty.Failed(new InvalidOperationException("original failure"));
        empty.Failed(new IOException("secondary failure"));
        Check(Json(empty.CreateReport()).GetProperty("error").GetString()!.Contains("original failure"), "First error preserved");
        empty.Save(Path.Combine(root, "nonexistent-directory", "error.mp4"));
        Check(empty.SaveError is not null, "Report failure must not mask an earlier failure");
        try { profile.Save(Path.Combine(root, "nonexistent-directory", "error.mp4")); throw new Exception("Unwritable report accepted"); }
        catch (IOException) { _checks++; }

        var info = new VideoInfo();
        var sinkPath = Path.Combine(root, "existing.mp4");
        File.WriteAllText(sinkPath, "do not overwrite");
        var sink = new AmfVideoFileWriter(sinkPath, info, new() { DiscardOutput = true, EnableProfiling = true, EnableDebugLog = true });
        var samples = new float[4096];
        for (var i = 0; i < 1000; i++)
        {
            sink.WriteVideo((ID2D1Bitmap1)null!); // Must not query the borrowed surface.
            sink.WriteAudio(samples);
        }
        sink.WriteVideo(Array.Empty<byte>());
        var pending = (List<float>)typeof(AmfVideoFileWriter).GetField("_pendingAudio", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(sink)!;
        Check(pending.Count == 0, "Discard must not retain pending audio");
        sink.Dispose();
        var sinkReportText = File.ReadAllText(sinkPath + ".amf_profile.json");
        sink.Dispose();
        Check(File.ReadAllText(sinkPath + ".amf_profile.json") == sinkReportText, "Dispose is idempotent");
        Check(File.ReadAllText(sinkPath) == "do not overwrite", "Discard preserves an existing target");
        Check(!File.Exists(sinkPath + ".amf_log.txt"), "Discard ignores native debug logging");
        using var sinkReport = JsonDocument.Parse(sinkReportText);
        Check(sinkReport.RootElement.GetProperty("metrics").GetProperty("received_video_frames").GetInt32() == 1001, "Both video overloads counted");
        Check(sinkReport.RootElement.GetProperty("metrics").GetProperty("submitted_video_frames").GetInt32() == 0, "No submissions in sink");
        try { sink.WriteAudio(samples); throw new Exception("Write after dispose accepted"); }
        catch (ObjectDisposedException) { _checks++; }

        var noProfilePath = Path.Combine(root, "no-profile.mp4");
        using (var noProfile = new AmfVideoFileWriter(noProfilePath, info, new() { DiscardOutput = true }))
        { noProfile.WriteVideo((ID2D1Bitmap1)null!); noProfile.WriteAudio(samples); }
        Check(!Directory.EnumerateFiles(root, "no-profile*").Any(), "Unprofiled discard produces no files");
        var defaultPath = Path.Combine(root, "default-empty.mp4");
        using (var normal = new AmfVideoFileWriter(defaultPath, info, defaults)) { }
        Check(!File.Exists(defaultPath + ".amf_profile.json"), "Normal output does not enable profiling implicitly");

        // UI toggles and snapshot isolation: changing a checkbox must not change an export already created.
        var plugin = new AmfVideoFileWriterPlugin();
        var view = (AmfConfigView)plugin.GetVideoConfigView("test", info, 1);
        var panel = (StackPanel)view.Content;
        var boxes = panel.Children.OfType<CheckBox>().ToArray();
        var profileBox = boxes.Single(box => box.Content.ToString()!.StartsWith("プロファイリング"));
        var discardBox = boxes.Single(box => box.Content.ToString()!.StartsWith("計測専用"));
        Check(profileBox.IsChecked == false && discardBox.IsChecked == false, "UI defaults are off");
        profileBox.IsChecked = true;
        discardBox.IsChecked = true;
        var snapshotPath = Path.Combine(root, "snapshot.mp4");
        using (var snapshotWriter = (AmfVideoFileWriter)plugin.CreateVideoFileWriter(snapshotPath, info))
        {
            profileBox.IsChecked = false;
            discardBox.IsChecked = false;
            snapshotWriter.WriteVideo((ID2D1Bitmap1)null!);
        }
        Check(File.Exists(snapshotPath + ".amf_profile.json") && !File.Exists(snapshotPath), "Export retains its settings snapshot");
        Check(plugin.GetFileExtention() == ".mp4", "Normal save-dialog extension unchanged");

        Console.WriteLine($"Managed diagnostic checks: {_checks} passed; reports: {root}");
        return 0;
    }
}
