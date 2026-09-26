using Vortice.DXGI;

internal sealed class GpuMemoryProbe : IDisposable
{
    private readonly IDXGIAdapter3? adapter;
    private readonly List<object> samples = [];
    private string? error;
    private ulong localPeak, nonLocalPeak;

    public GpuMemoryProbe(IDXGIAdapter source)
    {
        try { adapter = source.QueryInterface<IDXGIAdapter3>(); }
        catch (Exception e) { error = e.Message; }
    }

    // Diagnostic runs only. Sparse process-local DXGI accounting is not a
    // continuous VRAM peak, a GPU timestamp or the whole adapter's usage.
    public void Sample(int deliveredFrames, string phase)
    {
        if (adapter is null || error is not null) return;
        try
        {
            var local = adapter.QueryVideoMemoryInfo(0, MemorySegmentGroup.Local);
            var nonLocal = adapter.QueryVideoMemoryInfo(0, MemorySegmentGroup.NonLocal);
            localPeak = Math.Max(localPeak, local.CurrentUsage);
            nonLocalPeak = Math.Max(nonLocalPeak, nonLocal.CurrentUsage);
            samples.Add(new { delivered_frames = deliveredFrames, phase,
                local_usage_bytes = local.CurrentUsage, local_budget_bytes = local.Budget,
                nonlocal_usage_bytes = nonLocal.CurrentUsage, nonlocal_budget_bytes = nonLocal.Budget });
        }
        catch (Exception e) { error = e.Message; }
    }
    public object Report() => new
    {
        scope = "IDXGIAdapter3 node 0 process usage; sampled every 60 frames in diagnostic runs, not continuous peak or all GPU memory",
        error, sampled_peak_local_bytes = localPeak, sampled_peak_nonlocal_bytes = nonLocalPeak, samples,
    };
    public void Dispose() => adapter?.Dispose();
}
