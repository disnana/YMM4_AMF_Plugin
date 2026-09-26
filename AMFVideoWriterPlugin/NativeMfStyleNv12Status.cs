using System.Runtime.InteropServices;

namespace AMFVideoWriterPlugin;

[StructLayout(LayoutKind.Sequential)]
internal struct NativeMfStyleNv12Status
{
    public uint Version, Requested, Active, SourceDxgiFormat;
    public uint EncoderSurfaceFormat, GpuOperationsPerFrame, Reserved0, Reserved1;
    public ulong Conversions, Failures;
    public NativeProfileMetric ConversionCpu;

    public object ToReport()
    {
        if (Version != 1 || Requested > 1 || Active > 1 || Active > Requested
            || (Active == 1 ? GpuOperationsPerFrame != 2 : GpuOperationsPerFrame != 0)
            || Reserved0 != 0 || Reserved1 != 0)
            throw new InvalidOperationException("MF方式NV12変換のDLLバージョンまたは状態が不正です。");
        return new
        {
            requested = Requested != 0,
            active = Active != 0,
            path = Active != 0 ? "owned_rgb_copy_then_d3d11_videoprocessor_nv12_then_amf" : "amf_bgra_rgba",
            source_dxgi_format = SourceDxgiFormat,
            encoder_surface_format = EncoderSurfaceFormat,
            gpu_operations_per_frame = GpuOperationsPerFrame,
            conversions = Conversions,
            failures = Failures,
            conversion_cpu = new
            {
                count = ConversionCpu.Count,
                total_ms = ConversionCpu.TotalNanoseconds / 1_000_000d,
                mean_ms = ConversionCpu.Count == 0 ? 0 : ConversionCpu.TotalNanoseconds / 1_000_000d / ConversionCpu.Count,
                max_ms = ConversionCpu.MaxNanoseconds / 1_000_000d,
                measurement = "cpu_command_wall_time_not_gpu_timestamp",
            },
        };
    }
}
