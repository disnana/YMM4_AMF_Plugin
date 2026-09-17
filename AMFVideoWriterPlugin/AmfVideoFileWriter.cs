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

    public AmfVideoFileWriter(string outputPath, VideoInfo videoInfo, AmfSettings settings)
    {
        _outputPath = outputPath;
        _videoInfo = videoInfo;
        _settings = settings;
        _gpuDirectInput = settings.EnableGpuDirectInput;
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
                pool_size = settings.TexturePoolSize, debug_log_enabled = settings.EnableDebugLog,
                gpu_direct_input_enabled = _gpuDirectInput,
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

        _encoderHandle = AmfNativeMethods.AmfCreate(
            device.NativePointer,
            _videoInfo.Width,
            _videoInfo.Height,
            fps,
            bitrate,
            codec,
            quality,
            rateControl,
            maxBitrate,
            surfaceFormat,
            Math.Clamp(_settings.TexturePoolSize, 4, 8),
            _settings.EnableDebugLog ? 1 : 0,
            _outputPath);

        if (_encoderHandle == IntPtr.Zero)
        {
            throw new InvalidOperationException("AMF 初期化に失敗しました。AMD RadeonドライバーとGPUの対応状況を確認してください。");
        }

        var error = GetNativeError();
        if (!string.IsNullOrWhiteSpace(error))
        {
            AmfNativeMethods.AmfDestroy(_encoderHandle);
            _encoderHandle = IntPtr.Zero;
            throw new InvalidOperationException(error);
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
