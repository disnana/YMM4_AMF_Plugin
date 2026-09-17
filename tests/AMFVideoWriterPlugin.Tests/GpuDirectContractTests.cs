using AMFVideoWriterPlugin;
using System.IO;
using System.Text.Json;
using Vortice.Direct2D1;
using YukkuriMovieMaker.Plugin.FileWriter;
using YukkuriMovieMaker.Project;
using System.Windows.Controls;

internal static partial class Program
{
    private static void RunGpuDirectContractTests(string root)
    {
        AmfVideoFileWriter CreateWriter(string name, AmfSettings settings) => new(
            Path.Combine(root, name + ".mp4"), new VideoInfo(), settings);

        var defaults = CreateWriter("gpu-direct-default", new() { DiscardOutput = true });
        try
        {
            Check(defaults is IVideoFileWriter3, "Writer must expose the public IVideoFileWriter3 contract");
            Check(!((IVideoFileWriter3)defaults).IsGpuFrameSupported, "GPU-direct input must default to disabled");
        }
        finally
        {
            defaults.Dispose();
        }

        using (var normalDefault = CreateWriter("gpu-direct-normal-default", new()))
        {
            Check(!((IVideoFileWriter3)normalDefault).IsGpuFrameSupported, "Normal output must default to CPU input delivery");
        }

        var directSettings = new AmfSettings { DiscardOutput = true, EnableGpuDirectInput = true };
        var direct = CreateWriter("gpu-direct-settings-snapshot", directSettings);
        try
        {
            Check(((IVideoFileWriter3)direct).IsGpuFrameSupported, "GPU-direct setting was not exposed to the host");
            directSettings.EnableGpuDirectInput = false;
            Check(((IVideoFileWriter3)direct).IsGpuFrameSupported, "Writer retained a mutable settings reference instead of a construction snapshot");
        }
        finally
        {
            direct.Dispose();
        }

        var cpu = CreateWriter("gpu-direct-cpu-isolation", new() { DiscardOutput = true });
        try
        {
            Check(!((IVideoFileWriter3)cpu).IsGpuFrameSupported, "GPU-direct setting leaked to a separate CPU writer");
            cpu.WriteVideo((ID2D1Bitmap1)null!); // The Program resolver turns any unexpected native call into a test failure.
        }
        finally
        {
            cpu.Dispose();
        }

        var plugin = new AmfVideoFileWriterPlugin();
        var info = new VideoInfo();
        var view = (AmfConfigView)plugin.GetVideoConfigView("gpu-direct-test", info, 1);
        var panel = (StackPanel)view.Content;
        var gpuBox = panel.Children.OfType<CheckBox>().Single(box => box.Content.ToString()!.StartsWith("GPU直渡し"));
        var discardBox = panel.Children.OfType<CheckBox>().Single(box => box.Content.ToString()!.StartsWith("計測専用"));
        gpuBox.IsChecked = true;
        discardBox.IsChecked = true;
        var factoryDirect = (AmfVideoFileWriter)plugin.CreateVideoFileWriter(Path.Combine(root, "gpu-direct-factory-on.mp4"), info);
        try
        {
            gpuBox.IsChecked = false;
            Check(((IVideoFileWriter3)factoryDirect).IsGpuFrameSupported, "Factory writer did not retain its GPU-direct settings snapshot");
            using var factoryCpu = (AmfVideoFileWriter)plugin.CreateVideoFileWriter(Path.Combine(root, "gpu-direct-factory-off.mp4"), info);
            Check(!((IVideoFileWriter3)factoryCpu).IsGpuFrameSupported, "Factory GPU setting did not apply only to future writers");
        }
        finally
        {
            factoryDirect.Dispose();
        }

        string ProfilePath(bool gpuDirect) => Path.Combine(root, gpuDirect ? "gpu-direct-profile.mp4" : "gpu-cpu-profile.mp4");
        foreach (var gpuDirect in new[] { false, true })
        {
            var path = ProfilePath(gpuDirect);
            using (var profiled = CreateWriter(Path.GetFileNameWithoutExtension(path), new()
            {
                DiscardOutput = true, EnableProfiling = true, EnableGpuDirectInput = gpuDirect,
            }))
            {
                profiled.WriteVideo((ID2D1Bitmap1)null!);
            }
            using var report = JsonDocument.Parse(File.ReadAllText(path + ".amf_profile.json"));
            var expected = gpuDirect ? "gpu_direct_IVideoFileWriter3" : "cpu_read_bitmap_IVideoFileWriter2";
            Check(report.RootElement.GetProperty("input_delivery_path").GetString() == expected,
                "Profile input delivery path did not match the writer GPU-direct snapshot");
        }
    }
}
