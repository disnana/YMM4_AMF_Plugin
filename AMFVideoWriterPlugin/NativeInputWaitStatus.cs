using System.Runtime.InteropServices;

namespace AMFVideoWriterPlugin;

[StructLayout(LayoutKind.Sequential)]
internal struct NativeInputWaitStatus
{
    public uint Version, Requested, Mode, FallbackTimeoutMs;
    public ulong Waits, SignaledWaits, TimedOutWaits;
    public ulong InputReleaseNotifications, OutputNotifications;
    public NativeProfileMetric Wait;

    public object ToReport()
    {
        if (Version != 1 || Requested > 1 || Mode > 1
            || (Requested == 0 && (Mode != 0 || FallbackTimeoutMs != 0 || Waits != 0))
            || (Requested == 1 && (Mode != 1 || FallbackTimeoutMs != 10))
            || SignaledWaits + TimedOutWaits > Waits || Wait.Count != Waits)
            throw new InvalidOperationException("AMF入力満杯待機のDLLバージョンまたは状態が不正です。");
        return new
        {
            requested = Requested != 0,
            effective_mode = Requested != 0 ? "encoded_output_notification_with_10ms_fallback" : "sleep_1ms_legacy",
            fallback_timeout_ms = FallbackTimeoutMs,
            waits = Waits, signaled_waits = SignaledWaits, timed_out_waits = TimedOutWaits,
            input_release_notifications = InputReleaseNotifications,
            output_notifications = OutputNotifications,
            wait = new
            {
                count = Wait.Count,
                total_ms = Wait.TotalNanoseconds / 1_000_000d,
                mean_ms = Wait.Count == 0 ? 0 : Wait.TotalNanoseconds / 1_000_000d / Wait.Count,
                max_ms = Wait.MaxNanoseconds / 1_000_000d,
            },
        };
    }
}
