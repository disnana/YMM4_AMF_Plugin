using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Security.Cryptography;
using System.Text.Json;
using Vortice.Direct2D1;
using Vortice.DCommon;
using Vortice.Direct3D11;
using Vortice.DXGI;
using Vortice.Mathematics;
using Vortice.MediaFoundation;
using YukkuriMovieMaker.Commons;
using YukkuriMovieMaker.Plugin.FileWriter;
using VideoInfo = YukkuriMovieMaker.Project.VideoInfo;
using AlphaMode = Vortice.DCommon.AlphaMode;

internal static class Program
{
    internal const BindingFlags Fields = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
    [STAThread]
    private static int Main(string[] args)
    {
        // Run the apphost, not dotnet.exe: AppDirectories keys its paths off ProcessPath.
        if (Path.GetFileNameWithoutExtension(Environment.ProcessPath) != "MfAmfPathBench")
            throw new InvalidOperationException("Launch MfAmfPathBench.exe, not dotnet, to isolate YMM4 settings paths.");
        var options = Options.Parse(args);
        Directory.CreateDirectory(Path.Combine(AppContext.BaseDirectory, "user")); // Prevent YMM4's settings migration.
        Directory.CreateDirectory(options.RunDirectory);
        AssemblyLoadContext.Default.Resolving += (_, name) =>
        {
            string file = Path.Combine(options.YmmDirectory, name.Name + ".dll");
            return File.Exists(file) ? AssemblyLoadContext.Default.LoadFromAssemblyPath(file) : null;
        };
        return Run(options);
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int Run(Options o)
    {
        var report = new Dictionary<string, object?>
        {
            ["schema_version"] = 1, ["status"] = "started", ["requested"] = o,
            ["scope"] = o.Mode == "reference" ? "Lossless BGRA reference generation, not an encoder benchmark"
                : o.SourceFile is null ? "Actual writer DLLs with identical synthetic D2D frames; not YMM4 project end-to-end"
                : "Real-video re-encode fixture including FFmpeg decode/upload; quality test, not isolated encoder timing or project E2E",
            ["mf_dll_sha256"] = Hash(Path.Combine(o.YmmDirectory, "YukkuriMovieMaker.Plugin.FileSource.MediaFoundation.dll")),
            ["amf_native_sha256"] = Hash(o.NativeDll), ["amf_managed_sha256"] = Hash(o.AmfDll),
            ["bench_sha256"] = Hash(typeof(Program).Assembly.Location),
            ["fixture_source_sha256"] = o.SourceFile is null ? null : Hash(o.SourceFile),
        };
        IVideoFileWriter3? writer = null;
        bool started = false;
        var whole = Stopwatch.StartNew();
        try
        {
            if (!AppDirectories.SettingDirectory.StartsWith(Path.Combine(AppContext.BaseDirectory, "user") + Path.DirectorySeparatorChar,
                    StringComparison.OrdinalIgnoreCase)) throw new InvalidOperationException("YMM4 settings directory was not isolated.");
            report["isolated_settings_directory"] = AppDirectories.SettingDirectory;
            MediaFactory.MFStartup().CheckError(); started = true;
            using var graphics = new GraphicsDevices(); // Same published device flags / multithread protection as the host.
            using var dxgi = graphics.D3D.Device.QueryInterface<IDXGIDevice>();
            using var adapter = dxgi.GetAdapter();
            report["gpu"] = adapter.Description.Description;
            using var d2dDevice = D2D1.D2D1CreateDevice(dxgi);
            using var context = d2dDevice.CreateDeviceContext();
            using var texture = graphics.D3D.Device.CreateTexture2D(new Texture2DDescription
            {
                Width = o.Width, Height = o.Height, MipLevels = 1, ArraySize = 1,
                Format = Format.B8G8R8A8_UNorm, SampleDescription = new SampleDescription(1, 0),
                Usage = ResourceUsage.Default, BindFlags = BindFlags.RenderTarget | BindFlags.ShaderResource,
            });
            using var surface = texture.QueryInterface<IDXGISurface>();
            using var bitmap = context.CreateBitmapFromDxgiSurface(surface,
                new BitmapProperties1(new PixelFormat(Format.B8G8R8A8_UNorm, AlphaMode.Ignore), 96, 96, BitmapOptions.Target));
            using var readable = context.CreateBitmap(new SizeI(o.Width, o.Height), IntPtr.Zero, 0,
                new BitmapProperties1(new PixelFormat(Format.B8G8R8A8_UNorm, AlphaMode.Ignore), 96, 96,
                    BitmapOptions.CpuRead | BitmapOptions.CannotDraw));
            using var brush = context.CreateSolidColorBrush(new Color4(1, 1, 1, 1));
            using var source = o.SourceFile is null ? null : new DecodedFixture(o);
            context.Target = bitmap;
            long initialize = Stopwatch.GetTimestamp();
            writer = CreateWriter(o);
            report["writer_create_ms"] = Milliseconds(initialize);
            report["before"] = Snapshot(writer);
            using var memoryProbe = o.Profile ? new GpuMemoryProbe(adapter) : null;
            memoryProbe?.Sample(0, "before_frames");
            if (o.Mode.StartsWith("mf-")) report["mft_before"] = MfDiagnostics.Read(writer);
            var stages = new Dictionary<string, List<double>>
            {
                ["render_cpu"] = [], ["host_bitmap_copy"] = [], ["write_audio"] = [], ["write_video"] = [],
            };
            var gcBefore = Enumerable.Range(0, 3).Select(GC.CollectionCount).ToArray();
            long allocatedBefore = GC.GetTotalAllocatedBytes(true);
            long cpuBefore = Process.GetCurrentProcess().TotalProcessorTime.Ticks;
            var samples = new float[(48000 / o.Fps) * 2];
            long exportStart = Stopwatch.GetTimestamp();
            for (int frame = 0; frame < o.Frames; frame++)
            {
                long tick = Stopwatch.GetTimestamp();
                if (source is not null) source.Upload(bitmap);
                Draw(context, brush, frame, o.Width, o.Height, source is null, o.RenderRepeats);
                stages["render_cpu"].Add(Milliseconds(tick));
                if (o.Audio)
                {
                    for (int sample = 0; sample < samples.Length / 2; sample++)
                    {
                        float value = (float)(Math.Sin((frame * (samples.Length / 2L) + sample) * 2 * Math.PI * 440 / 48000) * 0.1);
                        samples[sample * 2] = samples[sample * 2 + 1] = value;
                    }
                    tick = Stopwatch.GetTimestamp(); writer.WriteAudio(samples);
                    stages["write_audio"].Add(Milliseconds(tick));
                }
                bool gpu = writer.IsGpuFrameSupported;
                tick = Stopwatch.GetTimestamp();
                if (!gpu) readable.CopyFromBitmap(bitmap).CheckError();
                stages["host_bitmap_copy"].Add(gpu ? 0 : Milliseconds(tick));
                tick = Stopwatch.GetTimestamp(); writer.WriteVideo(gpu ? bitmap : readable);
                stages["write_video"].Add(Milliseconds(tick));
                if (frame == 0 || (frame + 1) % 60 == 0) memoryProbe?.Sample(frame + 1, "delivering");
            }
            double framesMs = Milliseconds(exportStart);
            report["after_frames"] = Snapshot(writer);
            // No MFT inspection here: even an excluded pause would let the encoder drain.
            long finalize = Stopwatch.GetTimestamp();
            writer.Dispose();
            if (writer is ReferenceWriter reference) report["reference_bgra_sha256"] = reference.RawHash;
            writer = null;
            double finalizeMs = Milliseconds(finalize);
            double exportMs = Milliseconds(exportStart);
            memoryProbe?.Sample(o.Frames, "after_dispose");
            if (memoryProbe is not null) report["gpu_memory"] = memoryProbe.Report();
            report["frame_delivery_ms"] = framesMs;
            report["finalize_ms"] = finalizeMs;
            report["export_ms"] = exportMs;
            report["writer_create_plus_export_ms"] = (double)report["writer_create_ms"]! + exportMs;
            report["completed_fps"] = o.Frames * 1000 / exportMs;
            report["gc_collections"] = Enumerable.Range(0, 3).Select(i => GC.CollectionCount(i) - gcBefore[i]).ToArray();
            report["allocated_bytes"] = GC.GetTotalAllocatedBytes(true) - allocatedBefore;
            report["process_cpu_ms"] = (Process.GetCurrentProcess().TotalProcessorTime.Ticks - cpuBefore) / 10000d;
            report["stages"] = stages.ToDictionary(k => k.Key, v => Statistics(v.Value));
            report["output_bytes"] = new FileInfo(o.Output).Length;
            report["status"] = "writer_returned_validation_required";
            report["validation"] = new { decode = "not_run", frame_order = "not_run", color = "not_run", audio_sync = "not_run" };
            context.Target = null;
            if (o.InspectCleanup && memoryProbe is not null)
            {
                // Isolated diagnostic only, after all export/CPU timing has
                // ended. Never add per-frame Flush or mutate the host device.
                Thread.Sleep(200);
                memoryProbe.Sample(o.Frames, "after_dispose_idle_200ms");
                graphics.D3D.Device.ImmediateContext.Flush();
                Thread.Sleep(200);
                memoryProbe.Sample(o.Frames, "after_post_export_flush_200ms");
                report["gpu_memory"] = memoryProbe.Report();
            }
        }
        catch (Exception e)
        {
            report["status"] = "failed"; report["error"] = e.ToString();
            Console.Error.WriteLine(e);
        }
        finally
        {
            try { writer?.Dispose(); } catch (Exception e) { report["dispose_error"] = e.ToString(); }
            if (started) MediaFactory.MFShutdown();
            report["process_scope_wall_ms"] = whole.Elapsed.TotalMilliseconds;
            File.WriteAllText(Path.Combine(o.RunDirectory, "run.json"), JsonSerializer.Serialize(report, new JsonSerializerOptions { WriteIndented = true }));
        }
        Console.WriteLine($"{o.Mode}: {report["status"]}; {o.RunDirectory}");
        return Equals(report["status"], "writer_returned_validation_required") ? 0 : 1;
    }

    private static IVideoFileWriter3 CreateWriter(Options o)
    {
        if (o.Mode == "reference") return new ReferenceWriter(o);
        if (o.Mode.StartsWith("mf-"))
        {
            var assembly = AssemblyLoadContext.Default.LoadFromAssemblyPath(Path.Combine(o.YmmDirectory, "YukkuriMovieMaker.Plugin.FileSource.MediaFoundation.dll"));
            var settingsType = assembly.GetType("YukkuriMovieMaker.Plugin.FileWriter.MediaFoundation.MFVideoFileWriterSettings", true)!;
            object settings = Activator.CreateInstance(settingsType, true)!;
            Set(settings, "Width", o.Width); Set(settings, "Height", o.Height); Set(settings, "FPS", o.Fps);
            Set(settings, "Hz", 48000); Set(settings, "Length", o.Frames);
            Set(settings, "VideoBitRateMode", "Custom"); Set(settings, "VideoBitRateControlMode", o.RateMode == "quality" ? "Quality" : o.RateMode == "cbr" ? "CBR" : "UnconstrainedVBR");
            Set(settings, "VideoQuality", 100); Set(settings, "VideoBitRate", o.Bitrate);
            Set(settings, "EncodeSpeed", o.MfSpeed); Set(settings, "NumberOfThreads", 0); Set(settings, "GOPSize", 0);
            Set(settings, "BFrameCount", o.MfBFrames); Set(settings, "MinQP", 0); Set(settings, "MaxQP", 50);
            Set(settings, "IsHardwareAcceleration", true); Set(settings, "IsCABACEnabled", true);
            Set(settings, "IsFFmpegAACEncoderEnabled", o.MfFfmpegAudio); Set(settings, "AudioBitRate", "kb192");
            var type = assembly.GetType("YukkuriMovieMaker.Plugin.FileWriter.MediaFoundation.MFVideoFileWriter", true)!;
            var writer = (IVideoFileWriter3)Activator.CreateInstance(type, Fields, null, [o.Output, settings], null)!;
            if (o.Mode == "mf-cpu") type.GetField("gpuPathDisabled", Fields)!.SetValue(writer, true);
            return writer;
        }
        var amfAssembly = AssemblyLoadContext.Default.LoadFromAssemblyPath(o.AmfDll);
        NativeLibrary.SetDllImportResolver(amfAssembly, (name, _, _) => name == "AmfNative.dll" ? NativeLibrary.Load(o.NativeDll) : IntPtr.Zero);
        object amfSettings = Activator.CreateInstance(amfAssembly.GetType("AMFVideoWriterPlugin.AmfSettings", true)!, true)!;
        Set(amfSettings, "EnableGpuDirectInput", o.Mode == "amf-gpu");
        Set(amfSettings, "Quality", o.AmfQuality); Set(amfSettings, "Codec", "H264");
        Set(amfSettings, "RateControl", o.RateMode == "cbr" ? "Fixed" : "Variable"); Set(amfSettings, "BitrateKbps", o.Bitrate);
        Set(amfSettings, "TexturePoolSize", o.Pool); Set(amfSettings, "EnableProfiling", o.Profile);
        Set(amfSettings, "RecycleInputAfterRelease", o.RecycleInput);
        Set(amfSettings, "OptimizeOutputWait", o.OutputWait);
        Set(amfSettings, "AsyncSubmission", o.AsyncSubmission);
        Set(amfSettings, "DedicatedEncoderDevice", o.DedicatedDevice);
        Set(amfSettings, "AdaptiveInputWait", o.AdaptiveInputWait);
        var amfType = amfAssembly.GetType("AMFVideoWriterPlugin.AmfVideoFileWriter", true)!;
        return (IVideoFileWriter3)Activator.CreateInstance(amfType, Fields, null,
            [o.Output, new VideoInfo { Width = o.Width, Height = o.Height, FPS = o.Fps, Hz = 48000 }, amfSettings], null)!;
    }

    private static void Set(object instance, string propertyName, object value)
    {
        var property = instance.GetType().GetProperty(propertyName, Fields) ?? throw new MissingMemberException(propertyName);
        property.SetValue(instance, property.PropertyType.IsEnum ? Enum.Parse(property.PropertyType, value.ToString()!) : value);
    }

    internal static object? Field(object instance, string name) => instance.GetType().GetField(name, Fields)?.GetValue(instance);
    private static object Snapshot(IVideoFileWriter3 writer)
    {
        var data = new Dictionary<string, object?> { ["gpu_capability"] = writer.IsGpuFrameSupported };
        foreach (string name in new[] { "gpuDirectFailed", "videoFrameIndex", "videoTransformDescription", "audioBufferPosition", "totalSamples" })
            data[name] = Field(writer, name);
        if (Field(writer, "gpuBridge") is object bridge)
            foreach (var p in bridge.GetType().GetProperties(Fields).Where(p => p.PropertyType == typeof(int))) data["bridge_" + p.Name] = p.GetValue(bridge);
        return data;
    }

    private static void Draw(ID2D1DeviceContext context, ID2D1SolidColorBrush brush, int frame, int width, int height, bool synthetic, int repeats)
    {
        context.BeginDraw();
        if (synthetic) context.Clear(new Color4(0.12f, 0.25f, 0.5f, 1));
        for (int repeat = 0; repeat < repeats; repeat++)
        for (int i = 0; synthetic && i < 128; i++)
        {
            brush.Color = new Color4((i * 37 % 255) / 255f, (i * 73 % 255) / 255f, (i * 109 % 255) / 255f, 1);
            int x = (i * 137 + frame * (i % 7 + 1)) % Math.Max(1, width - 40);
            int y = (i * 71 + frame * (i % 5 + 1)) % Math.Max(1, height - 40);
            context.FillRectangle(new Rect(x, y, 40, 40), brush);
        }
        // Match Validate-Output's persistent 16-bit frame-order markers.
        int markerSize = Math.Max(4, Math.Min(width, height) / 32);
        for (int bit = 0; bit < 16; bit++)
        {
            brush.Color = (frame & (1 << bit)) == 0 ? new Color4(0, 0, 0, 1) : new Color4(1, 1, 1, 1);
            context.FillRectangle(new Rect((bit % 8) * markerSize, (bit / 8) * markerSize, markerSize, markerSize), brush);
        }
        context.EndDraw().CheckError();
    }

    private static double Milliseconds(long start) => Stopwatch.GetElapsedTime(start).TotalMilliseconds;
    private static object Statistics(List<double> values)
    {
        double[] sorted = values.Order().ToArray();
        double P(double q) => sorted.Length == 0 ? 0 : sorted[Math.Clamp((int)Math.Ceiling(q * sorted.Length) - 1, 0, sorted.Length - 1)];
        return new { count = values.Count, total_ms = values.Sum(), mean_ms = values.Count == 0 ? 0 : values.Average(),
            p50_ms = P(.5), p95_ms = P(.95), p99_ms = P(.99), max_ms = P(1), per_frame_ms = values };
    }
    private static string Hash(string path) => Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path)));
}

internal sealed record Options(string YmmDirectory, string AmfDll, string NativeDll, string RunDirectory,
    string Mode, int Width, int Height, int Fps, int Frames, int Bitrate, int Pool, string RateMode, bool Audio, bool MfFfmpegAudio, bool Profile,
    string AmfQuality, int MfSpeed, int MfBFrames, bool RecycleInput, bool OutputWait, string? SourceFile, int SourceStart, bool InspectCleanup,
    bool AsyncSubmission, bool DedicatedDevice, bool AdaptiveInputWait, int RenderRepeats)
{
    public string Output => Path.Combine(RunDirectory, Mode == "reference" ? "reference-bgra.mkv" : "output.mp4");
    public static Options Parse(string[] args)
    {
        var values = new Dictionary<string, string>();
        if (args.Length % 2 != 0) throw new ArgumentException("Pass --key value pairs.");
        for (int i = 0; i < args.Length; i += 2) values.Add(args[i], args[i + 1]);
        string S(string key, string? fallback = null) => values.TryGetValue("--" + key, out var value) ? value : fallback ?? throw new ArgumentException(key);
        int N(string key, int fallback) => int.Parse(S(key, fallback.ToString()));
        var o = new Options(Path.GetFullPath(S("ymm")), Path.GetFullPath(S("amf")), Path.GetFullPath(S("native")),
            Path.GetFullPath(S("run")), S("mode"), N("width", 1920), N("height", 1080), N("fps", 60), N("frames", 900),
            N("bitrate", 12000), N("pool", 6), S("rate", "quality"), S("audio", "true") == "true", S("mf-ffmpeg", "false") == "true", S("profile", "false") == "true",
            S("amf-quality", "Quality"), N("mf-speed", 50), N("mf-bframes", 2),
            S("recycle", "false") == "true", S("output-wait", "false") == "true",
            values.TryGetValue("--source", out var source) ? Path.GetFullPath(source) : null, N("source-start", 0), S("inspect-cleanup", "false") == "true",
            S("async-submission", "false") == "true", S("dedicated-device", "false") == "true",
            S("adaptive-input-wait", "false") == "true", N("render-repeats", 1));
        if (o.Mode is not ("mf-gpu" or "mf-cpu" or "amf-gpu" or "amf-cpu" or "reference") || o.RateMode is not ("quality" or "cbr" or "vbr")
            || o.Width < 88 || o.Height < 40 || o.Width % 2 != 0 || o.Height % 2 != 0 || o.Frames is < 1 or > 10000
            || o.Fps is < 1 or > 120 || 48000 % o.Fps != 0 || o.Pool is < 4 or > 128
            || o.AmfQuality is not ("Quality" or "Balanced" or "Speed") || o.MfSpeed is < 0 or > 100
            || o.MfBFrames is < 0 or > 2 || o.RenderRepeats is < 1 or > 64) throw new ArgumentException("Invalid benchmark options.");
        if (o.SourceStart < 0 || (o.Mode == "reference" && o.Audio)) throw new ArgumentException("Reference is video-only; source start must be non-negative.");
        if (o.InspectCleanup && !o.Profile) throw new ArgumentException("Cleanup inspection requires a separate profile run.");
        if (Directory.Exists(o.RunDirectory)) throw new IOException("Use a new, non-existing result directory.");
        return o;
    }
}
