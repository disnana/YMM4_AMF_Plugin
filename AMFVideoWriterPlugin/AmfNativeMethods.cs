using System.Runtime.InteropServices;

namespace AMFVideoWriterPlugin;

internal static class AmfNativeMethods
{
    [DllImport("AmfNative.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr AmfCreateWithPipeline(
        IntPtr device, int width, int height, int fps, int bitrateKbps, int codec,
        int quality, int rateControlMode, int maxBitrateKbps, int surfaceFormat,
        int texturePoolSize, int enableDebugLog, string outputPath, int optimizeOutputWait,
        int recycleInput, int asyncSubmission, int dedicatedDevice);

    [DllImport("AmfNative.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr AmfCreateWithAdaptiveInputWait(
        IntPtr device, int width, int height, int fps, int bitrateKbps, int codec,
        int quality, int rateControlMode, int maxBitrateKbps, int surfaceFormat,
        int texturePoolSize, int enableDebugLog, string outputPath, int optimizeOutputWait,
        int recycleInput, int asyncSubmission, int dedicatedDevice, int adaptiveInputWait);

    [DllImport("AmfNative.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr AmfCreateWithMfStyleNv12(
        IntPtr device, int width, int height, int fps, int bitrateKbps, int codec,
        int quality, int rateControlMode, int maxBitrateKbps, int surfaceFormat,
        int texturePoolSize, int enableDebugLog, string outputPath, int optimizeOutputWait,
        int recycleInput, int asyncSubmission, int dedicatedDevice, int adaptiveInputWait,
        int mfStyleNv12);

    [DllImport("AmfNative.dll")]
    public static extern int AmfGetMfStyleNv12Status(IntPtr handle, out NativeMfStyleNv12Status result, uint resultSize);

    [DllImport("AmfNative.dll")]
    public static extern int AmfGetInputWaitStatus(IntPtr handle, out NativeInputWaitStatus result, uint resultSize);

    [DllImport("AmfNative.dll")]
    public static extern int AmfGetPipelineStatus(IntPtr handle, out NativePipelineStatus result, uint resultSize);
    [DllImport("AmfNative.dll")]
    public static extern int AmfGetTexturePoolStatus(IntPtr handle, out NativeTexturePoolStatus result, uint resultSize);
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

    [DllImport("AmfNative.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr AmfCreateWithOutputWait(
        IntPtr device, int width, int height, int fps, int bitrateKbps, int codec,
        int quality, int rateControlMode, int maxBitrateKbps, int surfaceFormat,
        int texturePoolSize, int enableDebugLog, string outputPath, int optimizeOutputWait);

    [DllImport("AmfNative.dll")]
    public static extern int AmfGetOutputWaitStatus(IntPtr handle, out NativeOutputWaitStatus result, uint resultSize);

    [DllImport("AmfNative.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr AmfCreateWithInputRecycling(
        IntPtr device, int width, int height, int fps, int bitrateKbps, int codec,
        int quality, int rateControlMode, int maxBitrateKbps, int surfaceFormat,
        int texturePoolSize, int enableDebugLog, string outputPath, int optimizeOutputWait);

    [DllImport("AmfNative.dll")]
    public static extern int AmfGetInputRecycleStatus(IntPtr handle, out NativeInputRecycleStatus result, uint resultSize);

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
internal struct NativeInputRecycleStatus
{
    public uint Version, Requested, PoolSize, MaxPendingOutputs;
    public uint InputsInUse, PeakInputsInUse, PendingOutputs, PeakPendingOutputs;
    public ulong InputReleases, OutputCompletions, ReusesBeforeOutput, InputReleasesBeforeOutput, InvalidEvents;
    public NativeProfileMetric InputResidence;

    public object ToReport()
    {
        if (Version != 1 || Requested > 1 || (Requested == 1 && (PoolSize is < 4 or > 128 || MaxPendingOutputs != PoolSize * 2))
            || InputsInUse > PoolSize || PeakInputsInUse > PoolSize || PendingOutputs > MaxPendingOutputs || PeakPendingOutputs > MaxPendingOutputs)
            throw new InvalidOperationException("AMF入力再利用のDLLバージョンまたは状態が不正です。");
        return new
        {
            requested = Requested != 0,
            effective_mode = Requested != 0 ? "amf_surface_release" : "encoded_output_legacy",
            pool_size = PoolSize, pending_output_limit = MaxPendingOutputs,
            inputs_in_use = InputsInUse, peak_inputs_in_use = PeakInputsInUse,
            pending_outputs = PendingOutputs, peak_pending_outputs = PeakPendingOutputs,
            input_releases = InputReleases, output_completions = OutputCompletions,
            reuses_before_output = ReusesBeforeOutput, input_releases_before_output = InputReleasesBeforeOutput,
            invalid_events = InvalidEvents,
            input_residence = new { count = InputResidence.Count, total_ms = InputResidence.TotalNanoseconds / 1_000_000d,
                max_ms = InputResidence.MaxNanoseconds / 1_000_000d },
        };
    }
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeOutputWaitStatus
{
    public uint Version, Requested, TimeoutMs, Reason;
    public int CapabilityResult, PropertyResult;
    public ulong EarlyPollWaits, HighResolutionPollWaits;

    public object ToReport()
    {
        if (Version != 1 || Requested > 1 || Reason > 8 || TimeoutMs is not (0 or 10)
            || (Reason == 1) != (TimeoutMs == 10) || (Reason == 1 && Requested == 0))
            throw new InvalidOperationException("AMF出力待機設定のDLLバージョンまたは状態が不正です。");
        string[] reasons = ["disabled", "enabled", "capability_unavailable", "unsupported",
            "property_rejected", "readback_failed", "readback_mismatch", "reset_failed", "initialization_changed"];
        return new
        {
            requested = Requested != 0,
            effective_mode = Reason >= 7 ? "initialization_failed" : TimeoutMs == 0 ? "poll_sleep_1ms" : "amf_query_timeout",
            query_timeout_ms = TimeoutMs, reason = reasons[Reason],
            capability_result = CapabilityResult, property_result = PropertyResult,
            early_poll_waits = EarlyPollWaits,
            high_resolution_poll_waits = HighResolutionPollWaits,
        };
    }
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
