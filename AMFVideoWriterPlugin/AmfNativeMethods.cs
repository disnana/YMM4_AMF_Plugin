using System.Runtime.InteropServices;

namespace AMFVideoWriterPlugin;

internal static class AmfNativeMethods
{
    [DllImport("AmfNative.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr AmfCreate(
        IntPtr device,
        int width,
        int height,
        int fps,
        int bitrateKbps,
        int codec,
        int quality,
        int rateControlMode,
        int maxBitrateKbps,
        int surfaceFormat,
        int texturePoolSize,
        int enableDebugLog,
        string outputPath);

    [DllImport("AmfNative.dll")]
    public static extern int AmfEncode(IntPtr handle, IntPtr texture);

    [DllImport("AmfNative.dll")]
    public static extern int AmfWriteAudio(IntPtr handle, float[] samples, int sampleCount, int sampleRate, int channels);

    [DllImport("AmfNative.dll")]
    public static extern int AmfFinalize(IntPtr handle);

    [DllImport("AmfNative.dll")]
    public static extern void AmfDestroy(IntPtr handle);

    [DllImport("AmfNative.dll")]
    public static extern IntPtr AmfGetLastError(IntPtr handle);

    [DllImport("AmfNative.dll")]
    public static extern int AmfEnableProfiling(IntPtr handle);

    [DllImport("AmfNative.dll")]
    public static extern int AmfGetProfile(IntPtr handle, out NativeProfileSnapshot result, uint resultSize);
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeProfileMetric
{
    public ulong Count;
    public ulong TotalNanoseconds;
    public ulong MaxNanoseconds;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeProfileSnapshot
{
    public uint Version;
    public uint MetricCount;
    public ulong AcceptedFrames;
    public ulong CompletedFrames;
    public ulong InputRetries;
    [MarshalAs(UnmanagedType.ByValArray, SizeConst = 13)]
    public NativeProfileMetric[] Metrics;

    internal static readonly string[] StageNames =
    [
        "slot_wait", "copy_resource_cpu", "submit_input", "input_retry_wait", "query_output",
        "output_poll_wait", "output_mux_wait", "bitstream_mux", "audio_write", "writer_sample",
        "finalize", "mp4_finalize", "slot_residence",
    ];

    public object ToReport()
    {
        if (Version != 1 || MetricCount != StageNames.Length || Metrics is null || Metrics.Length != StageNames.Length)
            throw new InvalidOperationException("AMFプロファイリングのDLLバージョンが一致しません。");
        var stages = new Dictionary<string, object>();
        for (var i = 0; i < StageNames.Length; i++)
        {
            var metric = Metrics[i];
            stages.Add(StageNames[i], new
            {
                count = metric.Count,
                total_ms = metric.TotalNanoseconds / 1_000_000d,
                mean_ms = metric.Count == 0 ? 0 : metric.TotalNanoseconds / 1_000_000d / metric.Count,
                max_ms = metric.MaxNanoseconds / 1_000_000d,
            });
        }
        return new { version = Version, accepted_frames = AcceptedFrames, completed_frames = CompletedFrames,
            input_retries = InputRetries, stages };
    }
}
