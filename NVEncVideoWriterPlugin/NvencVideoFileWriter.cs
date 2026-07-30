using System.Collections.Generic;
using System.Runtime.InteropServices;
using Vortice.Direct2D1;
using Vortice.Direct3D11;
using Vortice.DXGI;
using YukkuriMovieMaker.Plugin.FileWriter;
using YukkuriMovieMaker.Project;

namespace NVEncVideoWriterPlugin;

internal sealed class NvencVideoFileWriter : IVideoFileWriter2, IDisposable
{
    private readonly string _outputPath;
    private readonly VideoInfo _videoInfo;
    private readonly NvencSettings _settings;
    private readonly int _audioChannels;
    private IntPtr _encoderHandle = IntPtr.Zero;
    private bool _disposed;
    private readonly object _encodeLock = new();

    public NvencVideoFileWriter(string outputPath, VideoInfo videoInfo, NvencSettings settings)
    {
        _outputPath = outputPath;
        _videoInfo = videoInfo;
        _settings = settings;
        _audioChannels = ResolveAudioChannels(videoInfo);
    }

    public VideoFileWriterSupportedStreams SupportedStreams => VideoFileWriterSupportedStreams.Audio | VideoFileWriterSupportedStreams.Video;

    private readonly List<float> _pendingAudio = new();

    public void WriteAudio(float[] samples)
    {
        EnsureNotDisposed();
        if (samples == null || samples.Length == 0)
        {
            return;
        }

        lock (_encodeLock)
        {
            if (_encoderHandle == IntPtr.Zero)
            {
                _pendingAudio.AddRange(samples);
                return;
            }

            WriteAudioInternal(samples);
        }
    }

    public void WriteVideo(byte[] frame)
    {
        // Not used in IVideoFileWriter2 mode.
    }

    public void WriteVideo(ID2D1Bitmap1 frame)
    {
        EnsureNotDisposed();

        if (_videoInfo.HasErrors || _videoInfo.Width <= 0 || _videoInfo.Height <= 0)
        {
            return;
        }

        using var surface = frame.Surface;
        using var texture = surface.QueryInterface<ID3D11Texture2D>();
        if (texture is null)
        {
            throw new InvalidOperationException("D3D11 テクスチャを取得できませんでした。");
        }

        lock (_encodeLock)
        {
            if (_encoderHandle == IntPtr.Zero)
            {
                InitializeEncoder(texture);
            }

            var result = NvencNativeMethods.NvencEncode(_encoderHandle, texture.NativePointer);
            if (result == 0)
            {
                throw new InvalidOperationException(GetNativeError());
            }
        }
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;

        lock (_encodeLock)
        {
            if (_encoderHandle != IntPtr.Zero)
            {
                var finalizeResult = NvencNativeMethods.NvencFinalize(_encoderHandle);
                var finalizeError = finalizeResult == 0 ? GetNativeError() : string.Empty;
                NvencNativeMethods.NvencDestroy(_encoderHandle);
                _encoderHandle = IntPtr.Zero;
                if (finalizeResult == 0)
                {
                    throw new InvalidOperationException(string.IsNullOrWhiteSpace(finalizeError)
                        ? "NVENC 出力の終了処理に失敗しました。"
                        : finalizeError);
                }
            }
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
            throw new InvalidOperationException("NVENC は偶数サイズの解像度が必要です。");
        }

        var fps = Math.Max(1, _videoInfo.FPS);
        var bitrate = GetTargetBitrateKbps();
        var codec = _settings.Codec switch
        {
            NvencCodec.H265 => 1,
            NvencCodec.AV1 => 2,
            _ => 0,
        };
        var quality = (int)_settings.Quality;
        var rateControl = _settings.RateControl == NvencRateControl.Variable ? 1 : 0;
        if (_settings.RateControl == NvencRateControl.YouTubeRecommended)
        {
            rateControl = 1;
        }
        var maxBitrate = rateControl == 1
            ? Math.Clamp((int)(bitrate * 1.2), 100, 300000)
            : bitrate;
        var bufferFormat = ResolveBufferFormat(texture);

        _encoderHandle = NvencNativeMethods.NvencCreate(
            device.NativePointer,
            _videoInfo.Width,
            _videoInfo.Height,
            fps,
            bitrate,
            codec,
            quality,
            0,
            rateControl,
            maxBitrate,
            bufferFormat,
            _settings.HevcAsync && _settings.Codec == NvencCodec.H265 ? 1 : 0,
            _settings.EnableDebugLog ? 1 : 0,
            _outputPath);

        if (_encoderHandle == IntPtr.Zero)
        {
            throw new InvalidOperationException("NVENC 初期化に失敗しました。");
        }

        var error = GetNativeError();
        if (!string.IsNullOrWhiteSpace(error))
        {
            NvencNativeMethods.NvencDestroy(_encoderHandle);
            _encoderHandle = IntPtr.Zero;
            throw new InvalidOperationException(error);
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
        var sampleRate = Math.Max(8000, _videoInfo.Hz);
        var result = NvencNativeMethods.NvencWriteAudio(_encoderHandle, samples, samples.Length, sampleRate, _audioChannels);
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
        var ptr = NvencNativeMethods.NvencGetLastError(_encoderHandle);
        return ptr == IntPtr.Zero ? string.Empty : Marshal.PtrToStringUni(ptr) ?? string.Empty;
    }

    private static int ResolveBufferFormat(ID3D11Texture2D texture)
    {
        var format = texture.Description.Format;
        return format switch
        {
            Format.B8G8R8A8_UNorm => NvencBufferFormat.ARGB,
            Format.B8G8R8A8_UNorm_SRgb => NvencBufferFormat.ARGB,
            Format.R8G8B8A8_UNorm => NvencBufferFormat.ABGR,
            Format.R8G8B8A8_UNorm_SRgb => NvencBufferFormat.ABGR,
            _ => NvencBufferFormat.ARGB,
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
        if (_settings.RateControl != NvencRateControl.YouTubeRecommended)
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
            throw new ObjectDisposedException(nameof(NvencVideoFileWriter));
        }
    }

    private static class NvencBufferFormat
    {
        public const int ARGB = 0x01000000;
        public const int ABGR = 0x10000000;
    }

    
}
