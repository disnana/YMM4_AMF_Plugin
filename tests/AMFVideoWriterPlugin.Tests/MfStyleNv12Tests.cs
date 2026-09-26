using System.IO;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Windows.Controls;
using AMFVideoWriterPlugin;
using Vortice.Direct2D1;
using YukkuriMovieMaker.Project;

internal static partial class Program
{
    private static void RunMfStyleNv12Tests(string root)
    {
        Check(!new AmfSettings().MfStyleNv12, "MF-style NV12 defaults off");
        Check(Marshal.SizeOf<NativeMfStyleNv12Status>() == 72, "MF-style NV12 ABI size");
        Check(Marshal.OffsetOf<NativeMfStyleNv12Status>(nameof(NativeMfStyleNv12Status.ConversionCpu)).ToInt32() == 48,
            "MF-style NV12 metric alignment");
        var status = new NativeMfStyleNv12Status
        {
            Version = 1, Requested = 1, Active = 1, SourceDxgiFormat = 87,
            EncoderSurfaceFormat = 1, GpuOperationsPerFrame = 2, Conversions = 120,
            ConversionCpu = new NativeProfileMetric { Count = 120, TotalNanoseconds = 12_000_000, MaxNanoseconds = 200_000 },
        };
        var report = Json(status.ToReport());
        Check(report.GetProperty("path").GetString()!.Contains("videoprocessor_nv12"), "MF-style NV12 report path");
        Check(report.GetProperty("conversion_cpu").GetProperty("mean_ms").GetDouble() == 0.1, "MF-style NV12 CPU units");
        var off = new NativeMfStyleNv12Status { Version = 1 };
        Check(!Json(off.ToReport()).GetProperty("active").GetBoolean(), "MF-style NV12 OFF status has no GPU operations");
        status.Active = 2;
        try { status.ToReport(); throw new Exception("Invalid MF-style NV12 status accepted"); }
        catch (InvalidOperationException) { _checks++; }

        var plugin = new AmfVideoFileWriterPlugin();
        var info = new VideoInfo();
        var panel = (StackPanel)((AmfConfigView)plugin.GetVideoConfigView("mf-nv12", info, 1)).Content;
        var boxes = panel.Children.OfType<CheckBox>().ToArray();
        boxes.Single(x => x.Content.ToString()!.StartsWith("計測専用")).IsChecked = true;
        boxes.Single(x => x.Content.ToString()!.StartsWith("プロファイリング")).IsChecked = true;
        var toggle = boxes.Single(x => x.Content.ToString()!.StartsWith("MF方式：GPU VideoProcessor"));
        toggle.IsChecked = true;
        var path = Path.Combine(root, "mf-style-nv12-snapshot.mp4");
        using (var writer = (AmfVideoFileWriter)plugin.CreateVideoFileWriter(path, info))
        {
            toggle.IsChecked = false;
            writer.WriteVideo((ID2D1Bitmap1)null!);
        }
        using var saved = JsonDocument.Parse(File.ReadAllText(path + ".amf_profile.json"));
        Check(saved.RootElement.GetProperty("configuration").GetProperty("mf_style_nv12").GetBoolean(),
            "Factory snapshots MF-style NV12");
        Check(saved.RootElement.GetProperty("mf_style_nv12").GetProperty("status").GetString() == "bypassed",
            "Discard reports bypassed MF-style NV12");
    }
}
