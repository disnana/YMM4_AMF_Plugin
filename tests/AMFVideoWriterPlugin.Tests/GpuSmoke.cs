using System.IO;
using System.Runtime.InteropServices;
using System.Text.Json;
using AMFVideoWriterPlugin;
using Vortice.DCommon;
using Vortice.Direct2D1;
using Vortice.Direct3D;
using Vortice.Direct3D11;
using Vortice.DXGI;
using Vortice.Mathematics;
using YukkuriMovieMaker.Project;
using YukkuriMovieMaker.Plugin.FileWriter;
using AlphaMode = Vortice.DCommon.AlphaMode;
using FeatureLevel = Vortice.Direct3D.FeatureLevel;

internal static partial class Program
{
    // Actual managed writer -> P/Invoke -> AMF, without launching or modifying a YMM4 installation.
    private static int RunGpuSmoke(string root, string nativePath)
    {
        Directory.CreateDirectory(root);
        NativeLibrary.SetDllImportResolver(typeof(AmfVideoFileWriter).Assembly, (name, _, _) =>
            name == "AmfNative.dll" ? NativeLibrary.Load(nativePath) : IntPtr.Zero);
        D3D11.D3D11CreateDevice(IntPtr.Zero, DriverType.Hardware, DeviceCreationFlags.BgraSupport,
            [FeatureLevel.Level_11_0], out var device, out var deviceContext).CheckError();
        using (device)
        using (deviceContext)
        using (var dxgiDevice = device.QueryInterface<IDXGIDevice>())
        using (var d2dDevice = D2D1.D2D1CreateDevice(dxgiDevice))
        using (var context = d2dDevice.CreateDeviceContext())
        using (var texture = device.CreateTexture2D(new Texture2DDescription
        {
            Width = 640, Height = 360, MipLevels = 1, ArraySize = 1,
            Format = Format.B8G8R8A8_UNorm, SampleDescription = new SampleDescription(1, 0),
            Usage = ResourceUsage.Default, BindFlags = BindFlags.RenderTarget | BindFlags.ShaderResource,
        }))
        using (var surface = texture.QueryInterface<IDXGISurface>())
        using (var bitmap = context.CreateBitmapFromDxgiSurface(surface,
            new BitmapProperties1(new PixelFormat(Format.B8G8R8A8_UNorm, AlphaMode.Ignore), 96, 96, BitmapOptions.Target)))
        using (var readableBitmap = context.CreateBitmap(new SizeI(640, 360), IntPtr.Zero, 0,
            new BitmapProperties1(new PixelFormat(Format.B8G8R8A8_UNorm, AlphaMode.Ignore), 96, 96,
                BitmapOptions.CpuRead | BitmapOptions.CannotDraw)))
        using (var white = context.CreateSolidColorBrush(new Color4(1, 1, 1, 1)))
        using (var black = context.CreateSolidColorBrush(new Color4(0, 0, 0, 1)))
        {
            context.Target = bitmap;
            foreach (var codec in new[] { AmfCodec.H264, AmfCodec.H265 })
            foreach (var enabled in new[] { false, true })
            foreach (var gpuDirect in new[] { false, true })
            {
                var caseName = (codec == AmfCodec.H264 ? "h264" : "hevc") + (enabled ? "-profile" : "-off") + (gpuDirect ? "-gpu-direct" : "-cpu-read");
                var caseRoot = Path.Combine(root, caseName);
                Directory.CreateDirectory(caseRoot);
                var path = Path.Combine(caseRoot, "output.mp4");
                var info = new VideoInfo { Width = 640, Height = 360, FPS = 60, Hz = 48000 };
                using (var writer = new AmfVideoFileWriter(path, info,
                    new AmfSettings { Codec = codec, TexturePoolSize = 4, EnableProfiling = enabled, EnableGpuDirectInput = gpuDirect }))
                {
                    Check(((IVideoFileWriter3)writer).IsGpuFrameSupported == gpuDirect, "Host input capability matches this case");
                    var samples = new float[1600]; // 800 stereo sample frames per video frame; silence is intentional.
                    for (var frame = 0; frame < 120; frame++)
                    {
                        context.BeginDraw();
                        context.Clear(new Color4(0.15f, 0.35f, 0.7f, 1));
                        // Same 16-bit ordering markers as RadeonBench / Validate-Output.ps1.
                        for (var bit = 0; bit < 16; bit++)
                            context.FillRectangle(new Rect((bit % 8) * 11, (bit / 8) * 11, 11, 11),
                                (frame & (1 << bit)) == 0 ? black : white);
                        context.EndDraw().CheckError();
                        writer.WriteAudio(samples); // Includes the pre-initialization buffering path on frame 0.
                        // Reproduce YMM4's actual v3/v2 dispatch, including the v2 CpuRead copy.
                        // Reuse the target immediately on the next iteration to exercise texture ownership.
                        if (((IVideoFileWriter3)writer).IsGpuFrameSupported) writer.WriteVideo(bitmap);
                        else
                        {
                            readableBitmap.CopyFromBitmap(bitmap).CheckError();
                            writer.WriteVideo(readableBitmap);
                        }
                    }
                }
                Check(File.Exists(path), "Managed writer produced an MP4");
                Check(!File.Exists(path + ".amf_log.txt"), "Profiling does not require debug logging");
                Check(File.Exists(path + ".amf_profile.json") == enabled, "Profile file respects its independent switch");
                if (enabled)
                {
                    using var json = JsonDocument.Parse(File.ReadAllText(path + ".amf_profile.json"));
                    var report = json.RootElement;
                    Check(report.GetProperty("input_delivery_path").GetString() == (gpuDirect ? "gpu_direct_IVideoFileWriter3" : "cpu_read_bitmap_IVideoFileWriter2"), "Profile records the selected input path");
                    Check(report.GetProperty("status").GetString() == "writer_disposed", "Managed completion without errors");
                    Check(report.GetProperty("output_finalized").GetBoolean(), "Native finalization propagated");
                    Check(report.GetProperty("native_status").GetString() == "captured_before_destroy", "Real native snapshot marshalled");
                    Check(report.GetProperty("metrics").GetProperty("submitted_video_frames").GetInt32() == 120, "Managed submitted count");
                    Check(report.GetProperty("native").GetProperty("accepted_frames").GetInt32() == 120, "Native accepted count");
                    Check(report.GetProperty("native").GetProperty("completed_frames").GetInt32() == 120, "Native completed count");
                    Check(report.GetProperty("native").GetProperty("stages").GetProperty("copy_resource_cpu").GetProperty("count").GetInt32() == 120,
                        "Native metric array marshalling");
                }
                // Common validation manifest consumed by Validate-Output.ps1, separate from the profile itself.
                File.WriteAllText(Path.Combine(caseRoot, "run.json"), JsonSerializer.Serialize(new
                {
                    status = "passed", source = "managed_writer_gpu_smoke",
                    validation = new { decode = "not_run", frame_order = "not_run", color = "not_run", audio_sync = "not_run" },
                }));
                Console.WriteLine($"Managed writer GPU case: {caseName}");
            }
            context.Target = null;
        }
        Console.WriteLine($"Managed GPU diagnostic checks: {_checks} passed; outputs still require decode/order validation: {root}");
        return 0;
    }
}
