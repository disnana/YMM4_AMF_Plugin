using System.Runtime.InteropServices;

namespace AMFVideoWriterPlugin;

[StructLayout(LayoutKind.Sequential)]
internal struct NativePipelineStatus
{
    public uint Version, AsyncSubmission, DedicatedDevice, Initialized;
    public uint QueueCapacity, QueueDepth, PeakQueueDepth, GpuCopiesPerFrame;
    public ulong QueuedFrames, SubmittedFrames, ExtraSharedTextureBytes, AdapterLuid;
    public NativeProfileMetric QueueResidence;

    public object ToReport()
    {
        if (Version != 1 || AsyncSubmission > 1 || DedicatedDevice > 1 || Initialized > 1
            || GpuCopiesPerFrame != (DedicatedDevice == 1 ? 2u : 1u)
            || QueueCapacity > 128 || QueueDepth > QueueCapacity || PeakQueueDepth > QueueCapacity
            || (AsyncSubmission == 0 && (QueueCapacity != 0 || QueuedFrames != 0))
            || (Initialized == 1 && AsyncSubmission == 1 && QueueCapacity < 4))
            throw new InvalidOperationException("AMFパイプライン設定のDLLバージョンまたは状態が不正です。");
        return new
        {
            initialized = Initialized != 0, async_submission = AsyncSubmission != 0,
            dedicated_device = DedicatedDevice != 0, gpu_copies_per_frame = GpuCopiesPerFrame,
            queue_capacity = QueueCapacity, queue_depth = QueueDepth, peak_queue_depth = PeakQueueDepth,
            queued_frames = QueuedFrames, submitted_frames = SubmittedFrames,
            extra_shared_texture_bytes = ExtraSharedTextureBytes, adapter_luid = AdapterLuid,
            queue_residence_cpu = new { count = QueueResidence.Count,
                total_ms = QueueResidence.TotalNanoseconds / 1_000_000d, max_ms = QueueResidence.MaxNanoseconds / 1_000_000d },
        };
    }
}
