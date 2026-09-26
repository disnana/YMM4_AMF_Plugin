using System.Runtime.InteropServices;

namespace AMFVideoWriterPlugin;

[StructLayout(LayoutKind.Sequential)]
internal struct NativeTexturePoolStatus
{
    public uint Version, RequestedSize, EffectiveSize, InputsInUse;
    public uint PeakInputsInUse, PendingOutputLimit, Reserved0, Reserved1;
    public ulong NominalBytes, ExpandedPayloadLimitBytes, InputExhaustionCount, OutputBudgetExhaustionCount;

    public object ToReport()
    {
        if (Version != 1 || RequestedSize is < 4 or > 128 || EffectiveSize is < 4 or > 128
            || EffectiveSize > RequestedSize || InputsInUse > EffectiveSize || PeakInputsInUse > EffectiveSize
            || (PendingOutputLimit != EffectiveSize && PendingOutputLimit != EffectiveSize * 2)
            || ExpandedPayloadLimitBytes != TexturePoolPolicy.ExpandedPayloadLimit || Reserved0 != 0 || Reserved1 != 0)
            throw new InvalidOperationException("AMFテクスチャプールのDLLバージョンまたは状態が不正です。");
        return new
        {
            requested_size = RequestedSize, effective_size = EffectiveSize,
            inputs_in_use = InputsInUse, peak_inputs_in_use = PeakInputsInUse,
            pending_output_limit = PendingOutputLimit,
            nominal_texture_bytes = NominalBytes, expanded_payload_limit_bytes = ExpandedPayloadLimitBytes,
            memory_measurement = "estimated_pixel_payload_not_total_vram",
            input_exhaustion_count = InputExhaustionCount, output_budget_exhaustion_count = OutputBudgetExhaustionCount,
        };
    }
}
