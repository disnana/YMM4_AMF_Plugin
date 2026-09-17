using System.Diagnostics;
using System.IO;
using System.Text;
using System.Text.Json;

namespace AMFVideoWriterPlugin;

internal enum ExportStage
{
    VideoCallback, AudioCallback, CpuVideoCallback, TextureAccess,
    VideoLockWait, AudioLockWait, EncoderInitialize, NativeVideo, NativeAudio,
    NativeFinalize, NativeDestroy, Dispose,
}

// One bounded aggregate per stage; never retain a frame, sample buffer, or per-frame event list.
internal sealed class ExportProfiler
{
    private sealed class Metric
    {
        public long Count, Total, Maximum;
        public void Add(long ticks)
        {
            Count++;
            Total += ticks;
            Maximum = Math.Max(Maximum, ticks);
        }
    }

    private readonly object _gate = new();
    private readonly Func<long> _clock;
    private readonly double _frequency;
    private readonly long _created;
    private readonly object _configuration;
    private readonly bool _discard;
    private readonly Metric[] _metrics = Enum.GetValues<ExportStage>().Select(_ => new Metric()).ToArray();
    private long? _firstCallback;
    private long _lastCallback, _busyStart, _busyTicks, _end;
    private int _active, _maxActive;
    private long _videoFrames, _audioValues, _submittedFrames;
    private string? _failure;

    public ExportProfiler(bool discard, object configuration, Func<long>? clock = null, long? frequency = null)
    {
        _discard = discard;
        _configuration = configuration;
        _clock = clock ?? Stopwatch.GetTimestamp;
        _frequency = frequency ?? Stopwatch.Frequency;
        if (_frequency <= 0) throw new ArgumentOutOfRangeException(nameof(frequency));
        _created = _clock();
    }

    public object? NativeReport { get; set; }
    public string NativeStatus { get; set; } = "not_initialized";
    public bool OutputFinalized { get; set; }
    public string InputDeliveryPath { get; set; } = "not_observed";
    public string? SaveError { get; private set; }

    public Scope Measure(ExportStage stage) => new(this, stage, _clock(), false);

    public Scope Callback(ExportStage stage, int audioValues = 0)
    {
        lock (_gate)
        {
            var now = _clock();
            _firstCallback ??= now;
            if (_active++ == 0) _busyStart = now;
            _maxActive = Math.Max(_maxActive, _active);
            if (stage is ExportStage.VideoCallback or ExportStage.CpuVideoCallback) _videoFrames++;
            if (stage == ExportStage.AudioCallback) _audioValues += Math.Max(0, audioValues);
            return new Scope(this, stage, now, true);
        }
    }

    private void End(ExportStage stage, long start, bool callback)
    {
        lock (_gate)
        {
            var now = _clock();
            _metrics[(int)stage].Add(Math.Max(0, now - start));
            if (callback)
            {
                _lastCallback = now;
                if (--_active == 0) _busyTicks += Math.Max(0, now - _busyStart);
            }
        }
    }

    public void SubmittedVideo() { lock (_gate) _submittedFrames++; }
    public void Failed(Exception exception) { lock (_gate) _failure ??= exception.GetType().Name + ": " + exception.Message; }
    public void Finish() { lock (_gate) _end = _clock(); }

    public object CreateReport()
    {
        lock (_gate)
        {
            var now = _end == 0 ? _clock() : _end;
            double Ms(long ticks) => ticks * 1000d / _frequency;
            var lifetime = Ms(Math.Max(0, now - _created));
            var spanTicks = _firstCallback.HasValue ? Math.Max(0, _lastCallback - _firstCallback.Value) : 0;
            var stages = new Dictionary<string, object>();
            foreach (var stage in Enum.GetValues<ExportStage>())
            {
                var metric = _metrics[(int)stage];
                stages.Add(JsonNamingPolicy.SnakeCaseLower.ConvertName(stage.ToString()), new
                {
                    count = metric.Count, total_ms = Ms(metric.Total),
                    mean_ms = metric.Count == 0 ? 0 : Ms(metric.Total) / metric.Count,
                    max_ms = Ms(metric.Maximum),
                });
            }
            return new
            {
                schema_version = 1,
                mode = _discard ? "discard" : "encode",
                status = _failure is not null ? "failed" : "writer_disposed",
                error = _failure,
                // The writer API supplies no cancellation/completion token. Do not claim full YMM4 export success.
                host_completion = "unknown_no_completion_signal",
                output_finalized = OutputFinalized,
                input_delivery_path = InputDeliveryPath,
                configuration = _configuration,
                metrics = new
                {
                    received_video_frames = _videoFrames, submitted_video_frames = _submittedFrames,
                    received_audio_sample_values = _audioValues, writer_lifetime_ms = lifetime,
                    received_fps = lifetime > 0 ? _videoFrames * 1000d / lifetime : 0,
                    delivery_span_ms = Ms(spanTicks), callback_union_ms = Ms(_busyTicks),
                    delivery_outside_callbacks_ms = Ms(Math.Max(0, spanTicks - _busyTicks)),
                    max_concurrent_callbacks = _maxActive, active_callbacks_at_dispose = _active,
                },
                managed_stages = stages,
                native_status = _discard ? "bypassed" : NativeStatus,
                native = NativeReport,
                notes = new[]
                {
                    "CPU wall-time, not GPU timestamp measurements. Nested/asynchronous stage totals overlap; do not sum them.",
                    "Outside-callback time is not pure YMM4 rendering time. Work can overlap, and discarding changes GPU contention/backpressure.",
                    "received_fps is callback throughput including writer setup/finalization, not validated encoded-video fps.",
                    "Report serialization/file I/O is excluded. Compare profiling off/on separately to assess measurement overhead.",
                },
            };
        }
    }

    public void Save(string outputPath)
    {
        var reportPath = outputPath + ".amf_profile.json";
        var temporaryPath = reportPath + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            var json = JsonSerializer.Serialize(CreateReport(), new JsonSerializerOptions { WriteIndented = true });
            File.WriteAllText(temporaryPath, json, new UTF8Encoding(false));
            File.Move(temporaryPath, reportPath, overwrite: true);
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            SaveError = exception.Message;
            // Never replace an earlier encoding error with a diagnostics-file error.
            if (_failure is null)
                throw new IOException("プロファイリング結果を保存できませんでした。動画処理の成否とは別のエラーです: " + reportPath, exception);
            Debug.WriteLine("AMF profile save failed: " + exception.Message);
        }
        finally
        {
            try { if (File.Exists(temporaryPath)) File.Delete(temporaryPath); }
            catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
            { Debug.WriteLine(exception.Message); }
        }
    }

    internal readonly struct Scope : IDisposable
    {
        private readonly ExportProfiler? _owner;
        private readonly ExportStage _stage;
        private readonly long _start;
        private readonly bool _callback;
        internal Scope(ExportProfiler owner, ExportStage stage, long start, bool callback)
        { _owner = owner; _stage = stage; _start = start; _callback = callback; }
        public void Dispose() => _owner?.End(_stage, _start, _callback);
    }
}
