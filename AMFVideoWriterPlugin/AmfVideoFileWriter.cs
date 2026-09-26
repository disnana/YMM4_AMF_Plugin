using System.Collections.Generic;
using System.Runtime.InteropServices;
using Vortice.Direct2D1;
using Vortice.Direct3D11;
using Vortice.DXGI;
using YukkuriMovieMaker.Plugin.FileWriter;
using YukkuriMovieMaker.Project;

namespace AMFVideoWriterPlugin;

internal sealed class AmfVideoFileWriter : IVideoFileWriter3, IDisposable
{
    private readonly string _outputPath;
    private readonly VideoInfo _videoInfo;
    private readonly AmfSettings _settings;
    private readonly int _audioChannels;
    private IntPtr _encoderHandle = IntPtr.Zero;
    private bool _disposed;
    private readonly object _encodeLock = new();
    private readonly ExportProfiler? _profile;
    // YMM4 queries this capability to select the bitmap delivery path. Keep the
    // choice constant for this export even if the next export's settings change.
    private readonly bool _gpuDirectInput;
    private readonly bool _optimizeOutputWait;
    private readonly bool _recycleInput;
    private readonly bool _asyncSubmission;
    private readonly bool _dedicatedDevice;
    private readonly bool _adaptiveInputWait;
    private readonly bool _mfStyleNv12;

    public AmfVideoFileWriter(string outputPath, VideoInfo videoInfo, AmfSettings settings)
    {
        _outputPath = outputPath;
        _videoInfo = videoInfo;
        _settings = settings;
        _gpuDirectInput = settings.EnableGpuDirectInput;
        _optimizeOutputWait = settings.OptimizeOutputWait;
        _recycleInput = settings.RecycleInputAfterRelease;
        _asyncSubmission = settings.AsyncSubmission;
        _dedicatedDevice = settings.DedicatedEncoderDevice;
        _adaptiveInputWait = settings.AdaptiveInputWait;
        _mfStyleNv12 = settings.MfStyleNv12;
        _audioChannels = ResolveAudioChannels(videoInfo);
        if (settings.EnableProfiling)
        {
            _profile = new ExportProfiler(settings.DiscardOutput, new
            {
                plugin_version = typeof(AmfVideoFileWriter).Assembly.GetName().Version?.ToString(),
                assembly_mvid = typeof(AmfVideoFileWriter).Assembly.ManifestModule.ModuleVersionId,
                os = Environment.OSVersion.VersionString,
                dotnet = RuntimeInformation.FrameworkDescription,
                width = videoInfo.Width, height = videoInfo.Height, fps = videoInfo.FPS,
                sample_rate = videoInfo.Hz, audio_channels = _audioChannels,
                codec = settings.Codec.ToString(), quality = settings.Quality.ToString(),
                rate_control = settings.RateControl.ToString(), target_bitrate_kbps = GetTargetBitrateKbps(),
                requested_pool_size = settings.TexturePoolSize,
                pool_size = TexturePoolPolicy.Resolve(settings.TexturePoolSize, videoInfo.Width, videoInfo.Height),
                debug_log_enabled = settings.EnableDebugLog,
                gpu_direct_input_enabled = _gpuDirectInput,
                optimize_output_wait = _optimizeOutputWait,
                recycle_input_after_release = _recycleInput,
                async_submission = _asyncSubmission,
                dedicated_encoder_device = _dedicatedDevice,
                adaptive_input_wait = _adaptiveInputWait,
                mf_style_nv12 = _mfStyleNv12,
            });
        }
    }

    public VideoFileWriterSupportedStreams SupportedStreams => VideoFileWriterSupportedStreams.Audio | VideoFileWriterSupportedStreams.Video;

    public bool IsGpuFrameSupported => _gpuDirectInput;

    private readonly List<float> _pendingAudio = new();

    public void WriteAudio(float[] samples)
    {
        EnsureNotDisposed();
        using var callback = _profile?.Callback(ExportStage.AudioCallback, samples?.Length ?? 0) ?? default;
        try
        {
            // Diagnostic sink: no buffering, conversion, native call, or file creation.
            if (_settings.DiscardOutput || samples == null || samples.Length == 0) return;
            var wait = _profile?.Measure(ExportStage.AudioLockWait) ?? default;
            lock (_encodeLock)
            {
                wait.Dispose();
                if (_encoderHandle == IntPtr.Zero)
                {
                    _pendingAudio.AddRange(samples);
                    return;
                }
                WriteAudioInternal(samples);
            }
        }
        catch (Exception exception) { _profile?.Failed(exception); throw; }
    }

    public void WriteVideo(byte[] frame)
    {
        // Not used for normal IVideoFileWriter2 output. The diagnostic sink also accepts this overload.
        EnsureNotDisposed();
        using var callback = _profile?.Callback(ExportStage.CpuVideoCallback) ?? default;
    }

    public void WriteVideo(ID2D1Bitmap1 frame)
    {
        EnsureNotDisposed();
        if (_profile is not null) _profile.InputDeliveryPath = IsGpuFrameSupported ? "gpu_direct_IVideoFileWriter3" : "cpu_read_bitmap_IVideoFileWriter2";
        using var callback = _profile?.Callback(ExportStage.VideoCallback) ?? default;
        try
        {
            // Return before even querying Surface: do not retain or copy the host's texture.
            if (_settings.DiscardOutput) return;
            if (_videoInfo.HasErrors || _videoInfo.Width <= 0 || _videoInfo.Height <= 0) return;

            var textureAccess = _profile?.Measure(ExportStage.TextureAccess) ?? default;
            using var surface = frame.Surface;
            using var texture = surface.QueryInterface<ID3D11Texture2D>();
            textureAccess.Dispose();
            if (texture is null)
                throw new InvalidOperationException("D3D11 テクスチャを取得できませんでした。");

            var wait = _profile?.Measure(ExportStage.VideoLockWait) ?? default;
            lock (_encodeLock)
            {
                wait.Dispose();
                if (_encoderHandle == IntPtr.Zero)
                {
                    using var initialize = _profile?.Measure(ExportStage.EncoderInitialize) ?? default;
                    InitializeEncoder(texture);
                }

                using var native = _profile?.Measure(ExportStage.NativeVideo) ?? default;
                var result = AmfNativeMethods.AmfEncode(_encoderHandle, texture.NativePointer);
                if (result == 0) throw new InvalidOperationException(GetNativeError());
                _profile?.SubmittedVideo();
            }
        }
        catch (Exception exception) { _profile?.Failed(exception); throw; }
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        var disposeScope = _profile?.Measure(ExportStage.Dispose) ?? default;
        try
        {
            lock (_encodeLock)
            {
                if (_encoderHandle != IntPtr.Zero)
                {
                    var finalizeResult = 0;
                    var finalizeError = string.Empty;
                    try
                    {
                        using var finalize = _profile?.Measure(ExportStage.NativeFinalize) ?? default;
                        finalizeResult = AmfNativeMethods.AmfFinalize(_encoderHandle);
                        finalizeError = finalizeResult == 0 ? GetNativeError() : string.Empty;
                        if (_profile is not null) _profile.OutputFinalized = finalizeResult != 0;
                    }
                    finally
                    {
                        CaptureNativeProfile();
                        using var destroy = _profile?.Measure(ExportStage.NativeDestroy) ?? default;
                        AmfNativeMethods.AmfDestroy(_encoderHandle);
                        _encoderHandle = IntPtr.Zero;
                    }
                    if (finalizeResult == 0)
                        throw new InvalidOperationException(string.IsNullOrWhiteSpace(finalizeError)
                            ? "AMF 出力の終了処理に失敗しました。" : finalizeError);
                }
            }
        }
        catch (Exception exception) { _profile?.Failed(exception); throw; }
        finally
        {
            disposeScope.Dispose();
            _profile?.Finish();
            _profile?.Save(_outputPath);
        }
    }

    private void CaptureNativeProfile()
    {
        if (_profile is null || _encoderHandle == IntPtr.Zero) return;
        CaptureOutputWaitStatus();
        CaptureInputRecycleStatus();
        CaptureTexturePoolStatus();
        CapturePipelineStatus();
        CaptureInputWaitStatus();
        CaptureMfStyleNv12Status();
        try
        {
            if (AmfNativeMethods.AmfGetProfile(_encoderHandle, out var snapshot, (uint)Marshal.SizeOf<NativeProfileSnapshot>()) == 0)
                throw new InvalidOperationException("Native profiling snapshot unavailable.");
            _profile.NativeReport = snapshot.ToReport();
            _profile.NativeStatus = "captured_before_destroy";
        }
        catch (Exception exception) when (exception is EntryPointNotFoundException or InvalidOperationException)
        {
            // Diagnostics must not prevent native cleanup or hide the original error.
            _profile.NativeStatus = "unavailable: " + exception.Message;
        }
    }

    private void CaptureOutputWaitStatus()
    {
        if (_profile is null || _encoderHandle == IntPtr.Zero) return;
        try
        {
            if (AmfNativeMethods.AmfGetOutputWaitStatus(_encoderHandle, out var status, (uint)Marshal.SizeOf<NativeOutputWaitStatus>()) == 0)
                throw new InvalidOperationException("Native output-wait status unavailable.");
            _profile.OutputWaitReport = status.ToReport();
        }
        catch (Exception exception) when (exception is EntryPointNotFoundException or InvalidOperationException)
        {
            _profile.OutputWaitReport = new { status = "unavailable", reason = exception.Message };
        }
    }

    private void CaptureInputRecycleStatus()
    {
        if (_profile is null || _encoderHandle == IntPtr.Zero) return;
        try
        {
            if (AmfNativeMethods.AmfGetInputRecycleStatus(_encoderHandle, out var status, (uint)Marshal.SizeOf<NativeInputRecycleStatus>()) == 0)
                throw new InvalidOperationException("Native input-recycling status unavailable.");
            _profile.InputRecycleReport = status.ToReport();
        }
        catch (Exception exception) when (exception is EntryPointNotFoundException or InvalidOperationException)
        {
            _profile.InputRecycleReport = new { status = "unavailable", reason = exception.Message };
        }
    }

    private void CaptureTexturePoolStatus(bool requireExpandedPool = false)
    {
        if (_encoderHandle == IntPtr.Zero || (_profile is null && !requireExpandedPool)) return;
        try
        {
            if (AmfNativeMethods.AmfGetTexturePoolStatus(_encoderHandle, out var status, (uint)Marshal.SizeOf<NativeTexturePoolStatus>()) == 0)
                throw new InvalidOperationException("Native texture-pool status unavailable.");
            var report = status.ToReport();
            if (status.EffectiveSize != TexturePoolPolicy.Resolve(_settings.TexturePoolSize, _videoInfo.Width, _videoInfo.Height))
                throw new InvalidOperationException("AMFテクスチャプールの実効枚数が要求と一致しません。");
            if (_profile is not null) _profile.TexturePoolReport = report;
        }
        catch (Exception exception) when (exception is EntryPointNotFoundException or InvalidOperationException)
        {
            if (requireExpandedPool)
                throw new InvalidOperationException("テクスチャ増量には同じビルドのAMFPlugin.dllとAmfNative.dllが必要です。", exception);
            if (_profile is not null) _profile.TexturePoolReport = new { status = "unavailable", reason = exception.Message };
        }
    }

    private void CapturePipelineStatus(bool required = false)
    {
        if (_encoderHandle == IntPtr.Zero || (_profile is null && !required)) return;
        try
        {
            if (AmfNativeMethods.AmfGetPipelineStatus(_encoderHandle, out var status, (uint)Marshal.SizeOf<NativePipelineStatus>()) == 0)
                throw new InvalidOperationException("Native pipeline status unavailable.");
            var report = status.ToReport();
            if (status.Initialized != 1 || (status.AsyncSubmission != 0) != _asyncSubmission
                || (status.DedicatedDevice != 0) != _dedicatedDevice)
                throw new InvalidOperationException("AMFパイプラインの要求と実効設定が一致しません。");
            if (_profile is not null) _profile.PipelineReport = report;
        }
        catch (Exception exception) when (exception is EntryPointNotFoundException or InvalidOperationException)
        {
            if (required) throw new InvalidOperationException("パイプライン実験には同じビルドのAMFPlugin.dllとAmfNative.dllが必要です。", exception);
            if (_profile is not null) _profile.PipelineReport = new { status = "unavailable", reason = exception.Message };
        }
    }

    private void CaptureInputWaitStatus(bool required = false)
    {
        if (_encoderHandle == IntPtr.Zero || (_profile is null && !required)) return;
        try
        {
            if (AmfNativeMethods.AmfGetInputWaitStatus(_encoderHandle, out var status,
                (uint)Marshal.SizeOf<NativeInputWaitStatus>()) == 0)
                throw new InvalidOperationException("Native input-full wait status unavailable.");
            var report = status.ToReport();
            if ((status.Requested != 0) != _adaptiveInputWait)
                throw new InvalidOperationException("AMF入力満杯待機の要求と実効設定が一致しません。");
            if (_profile is not null) _profile.InputWaitReport = report;
        }
        catch (Exception exception) when (exception is EntryPointNotFoundException or InvalidOperationException)
        {
            if (required)
                throw new InvalidOperationException("入力満杯待機の実験には同じビルドのAMFPlugin.dllとAmfNative.dllが必要です。", exception);
            if (_profile is not null) _profile.InputWaitReport = new { status = "unavailable", reason = exception.Message };
        }
    }

    private void CaptureMfStyleNv12Status(bool required = false)
    {
        if (_encoderHandle == IntPtr.Zero || (_profile is null && !required)) return;
        try
        {
            if (AmfNativeMethods.AmfGetMfStyleNv12Status(_encoderHandle, out var status,
                (uint)Marshal.SizeOf<NativeMfStyleNv12Status>()) == 0)
                throw new InvalidOperationException("Native MF方式NV12状態を取得できませんでした。");
            var report = status.ToReport();
            if ((status.Requested != 0) != _mfStyleNv12 || (status.Active != 0) != _mfStyleNv12)
                throw new InvalidOperationException("MF方式NV12変換の要求と実効設定が一致しません。");
            if (_profile is not null) _profile.MfStyleNv12Report = report;
        }
        catch (Exception exception) when (exception is EntryPointNotFoundException or InvalidOperationException)
        {
            if (required)
                throw new InvalidOperationException("MF方式NV12実験には同じビルドのAMFPlugin.dllとAmfNative.dllが必要です。", exception);
            if (_profile is not null) _profile.MfStyleNv12Report = new { status = "unavailable", reason = exception.Message };
        }
    }

    private void InitializeEncoder(ID3D11Texture2D texture)
    {
        using var device = texture.Device;
        if (device is null)
        {
            throw new InvalidOperationException("D3D11 デバイスを取得できませんでした。");
        }

        if ((_videoInfo.Width & 1) != 0 || (_videoInfo.Height & 1) != 0)
        {
            throw new InvalidOperationException("AMF H.264/HEVC 出力は偶数サイズの解像度が必要です。");
        }

        var fps = Math.Max(1, _videoInfo.FPS);
        var bitrate = GetTargetBitrateKbps();
        var codec = _settings.Codec switch
        {
            AmfCodec.H265 => 1,
            _ => 0,
        };
        var quality = (int)_settings.Quality;
        var rateControl = _settings.RateControl == AmfRateControl.Variable ? 1 : 0;
        if (_settings.RateControl == AmfRateControl.YouTubeRecommended)
        {
            rateControl = 1;
        }
        var maxBitrate = rateControl == 1
            ? Math.Clamp((int)(bitrate * 1.2), 100, 300000)
            : bitrate;
        var surfaceFormat = ResolveSurfaceFormat(texture);
        var requestedPoolSize = Math.Clamp(_settings.TexturePoolSize, TexturePoolPolicy.Minimum, TexturePoolPolicy.Maximum);

        try
        {
            _encoderHandle = _mfStyleNv12
                ? AmfNativeMethods.AmfCreateWithMfStyleNv12(device.NativePointer,
                    _videoInfo.Width, _videoInfo.Height, fps, bitrate, codec, quality,
                    rateControl, maxBitrate, surfaceFormat, requestedPoolSize,
                    _settings.EnableDebugLog ? 1 : 0, _outputPath, _optimizeOutputWait ? 1 : 0,
                    _recycleInput ? 1 : 0, _asyncSubmission ? 1 : 0, _dedicatedDevice ? 1 : 0,
                    _adaptiveInputWait ? 1 : 0, 1)
                : _adaptiveInputWait
                ? AmfNativeMethods.AmfCreateWithAdaptiveInputWait(device.NativePointer,
                    _videoInfo.Width, _videoInfo.Height, fps, bitrate, codec, quality,
                    rateControl, maxBitrate, surfaceFormat, requestedPoolSize,
                    _settings.EnableDebugLog ? 1 : 0, _outputPath, _optimizeOutputWait ? 1 : 0,
                    _recycleInput ? 1 : 0, _asyncSubmission ? 1 : 0, _dedicatedDevice ? 1 : 0, 1)
                : _asyncSubmission || _dedicatedDevice
                ? AmfNativeMethods.AmfCreateWithPipeline(device.NativePointer,
                    _videoInfo.Width, _videoInfo.Height, fps, bitrate, codec, quality,
                    rateControl, maxBitrate, surfaceFormat, requestedPoolSize,
                    _settings.EnableDebugLog ? 1 : 0, _outputPath, _optimizeOutputWait ? 1 : 0,
                    _recycleInput ? 1 : 0, _asyncSubmission ? 1 : 0, _dedicatedDevice ? 1 : 0)
                : _recycleInput
                ? AmfNativeMethods.AmfCreateWithInputRecycling(device.NativePointer,
                    _videoInfo.Width, _videoInfo.Height, fps, bitrate, codec, quality,
                    rateControl, maxBitrate, surfaceFormat, requestedPoolSize,
                    _settings.EnableDebugLog ? 1 : 0, _outputPath, _optimizeOutputWait ? 1 : 0)
                : _optimizeOutputWait
                ? AmfNativeMethods.AmfCreateWithOutputWait(device.NativePointer,
                    _videoInfo.Width, _videoInfo.Height, fps, bitrate, codec, quality,
                    rateControl, maxBitrate, surfaceFormat, requestedPoolSize,
                    _settings.EnableDebugLog ? 1 : 0, _outputPath, 1)
                : AmfNativeMethods.AmfCreate(device.NativePointer,
                    _videoInfo.Width, _videoInfo.Height, fps, bitrate, codec, quality,
                    rateControl, maxBitrate, surfaceFormat, requestedPoolSize,
                    _settings.EnableDebugLog ? 1 : 0, _outputPath);
        }
        catch (EntryPointNotFoundException exception)
        {
            throw new InvalidOperationException("AmfNative.dllが古いため実験機能を利用できません。AMFPlugin.dllと同じビルドのDLLを配置してください。", exception);
        }

        if (_encoderHandle == IntPtr.Zero)
        {
            throw new InvalidOperationException("AMF 初期化に失敗しました。AMD RadeonドライバーとGPUの対応状況を確認してください。");
        }

        var error = GetNativeError();
        CaptureOutputWaitStatus();
        CaptureInputRecycleStatus();
        if (!string.IsNullOrWhiteSpace(error))
        {
            AmfNativeMethods.AmfDestroy(_encoderHandle);
            _encoderHandle = IntPtr.Zero;
            throw new InvalidOperationException(error);
        }

        try
        {
            CaptureTexturePoolStatus(requireExpandedPool: requestedPoolSize > TexturePoolPolicy.LegacyMaximum);
            CapturePipelineStatus(required: _asyncSubmission || _dedicatedDevice);
            CaptureInputWaitStatus(required: _adaptiveInputWait);
            CaptureMfStyleNv12Status(required: _mfStyleNv12);
        }
        catch
        {
            // No input submitted yet. Preserve this failure, not a later missing-header error.
            AmfNativeMethods.AmfDestroy(_encoderHandle);
            _encoderHandle = IntPtr.Zero;
            throw;
        }

        if (_profile is not null)
        {
            try
            {
                if (AmfNativeMethods.AmfEnableProfiling(_encoderHandle) == 0)
                    throw new InvalidOperationException("AMFプロファイリングを開始できませんでした。");
                _profile.NativeStatus = "enabled";
            }
            catch (Exception exception)
            {
                // No frame was submitted. Clean up now so a later Dispose cannot replace this
                // initialization error with the secondary "Video codec header not found" error.
                AmfNativeMethods.AmfDestroy(_encoderHandle);
                _encoderHandle = IntPtr.Zero;
                _profile.NativeStatus = "initialization_failed";
                if (exception is EntryPointNotFoundException)
                    throw new InvalidOperationException("AmfNative.dllが古いため計測できません。AMFPlugin.dllと同じビルドのDLLを配置してください。", exception);
                throw;
            }
        }

        if (_pendingAudio.Count > 0)
        {
            var buffer = _pendingAudio.ToArray();
            _pendingAudio.Clear();
            WriteAudioInternal(buffer);
        }
    }

    private void WriteAudioInternal(float[] samples)
    {
        using var native = _profile?.Measure(ExportStage.NativeAudio) ?? default;
        var sampleRate = Math.Max(8000, _videoInfo.Hz);
        var result = AmfNativeMethods.AmfWriteAudio(_encoderHandle, samples, samples.Length, sampleRate, _audioChannels);
        if (result == 0)
        {
            throw new InvalidOperationException(GetNativeError());
        }
    }

    private string GetNativeError()
    {
        if (_encoderHandle == IntPtr.Zero)
        {
            return string.Empty;
        }
        var ptr = AmfNativeMethods.AmfGetLastError(_encoderHandle);
        return ptr == IntPtr.Zero ? string.Empty : Marshal.PtrToStringUni(ptr) ?? string.Empty;
    }

    private static int ResolveSurfaceFormat(ID3D11Texture2D texture)
    {
        var format = texture.Description.Format;
        return format switch
        {
            Format.B8G8R8A8_UNorm => AmfSurfaceFormat.Bgra,
            Format.B8G8R8A8_UNorm_SRgb => AmfSurfaceFormat.Bgra,
            Format.R8G8B8A8_UNorm => AmfSurfaceFormat.Rgba,
            Format.R8G8B8A8_UNorm_SRgb => AmfSurfaceFormat.Rgba,
            _ => throw new NotSupportedException($"AMFで未対応の入力形式です: {format}"),
        };
    }

    private static int ResolveAudioChannels(VideoInfo videoInfo)
    {
        const int fallback = 2;
        var type = videoInfo.GetType();
        var prop = type.GetProperty("Channels")
            ?? type.GetProperty("ChannelCount")
            ?? type.GetProperty("AudioChannels")
            ?? type.GetProperty("AudioChannelCount");
        if (prop?.GetValue(videoInfo) is int value && value > 0)
        {
            return value;
        }
        return fallback;
    }

    private int GetTargetBitrateKbps()
    {
        if (_settings.RateControl != AmfRateControl.YouTubeRecommended)
        {
            return Math.Clamp(_settings.BitrateKbps, 100, 200000);
        }

        var height = Math.Max(1, _videoInfo.Height);
        var highFps = _videoInfo.FPS >= 48;
        return height switch
        {
            >= 2160 => (highFps ? 60 : 40) * 1000,
            >= 1440 => (highFps ? 24 : 16) * 1000,
            >= 1080 => (highFps ? 12 : 8) * 1000,
            >= 720 => highFps ? 7500 : 5000,
            _ => (highFps ? 4 : 3) * 1000,
        };
    }

    private void EnsureNotDisposed()
    {
        if (_disposed)
        {
            throw new ObjectDisposedException(nameof(AmfVideoFileWriter));
        }
    }

    private static class AmfSurfaceFormat
    {
        public const int Bgra = 3;
        public const int Rgba = 5;
    }

}
