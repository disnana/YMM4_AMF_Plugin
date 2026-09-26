using System.Diagnostics;
using System.IO;
using System.Security.Cryptography;
using Vortice.Direct2D1;
using YukkuriMovieMaker.Plugin.FileWriter;

// Quality-reference generation only. Never use its CPU readback timings as encoder throughput.
internal sealed class ReferenceWriter : IVideoFileWriter3
{
    private readonly Process process;
    private readonly Task<string> error;
    private readonly IncrementalHash hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
    private readonly Options options;
    private bool disposed;
    public string? RawHash { get; private set; }
    public bool IsGpuFrameSupported => false;
    public VideoFileWriterSupportedStreams SupportedStreams => VideoFileWriterSupportedStreams.Video;
    public ReferenceWriter(Options o)
    {
        options = o;
        var info = new ProcessStartInfo("ffmpeg.exe") { UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardInput = true, RedirectStandardError = true };
        foreach (string arg in new[] { "-hide_banner", "-nostdin", "-v", "error", "-n", "-f", "rawvideo", "-pixel_format", "bgra",
            "-video_size", $"{o.Width}x{o.Height}", "-framerate", o.Fps.ToString(), "-i", "pipe:0", "-an",
            "-c:v", "ffv1", "-level", "3", "-threads", "4", "-pix_fmt", "bgra", "-color_range", "pc",
            "-colorspace", "rgb", "-color_trc", "iec61966-2-1", "-color_primaries", "bt709", o.Output }) info.ArgumentList.Add(arg);
        process = Process.Start(info)!;
        error = process.StandardError.ReadToEndAsync();
    }
    public void WriteAudio(float[] samples) => throw new NotSupportedException("Reference is video-only.");
    public unsafe void WriteVideo(ID2D1Bitmap1 frame)
    {
        var mapped = frame.Map(MapOptions.Read);
        try
        {
            int rowBytes = checked(options.Width * 4);
            if (mapped.Pitch == rowBytes)
                Write(new ReadOnlySpan<byte>((void*)mapped.Bits, checked(rowBytes * options.Height)));
            else
                for (int row = 0; row < options.Height; row++)
                    Write(new ReadOnlySpan<byte>((byte*)mapped.Bits + row * mapped.Pitch, rowBytes));
        }
        finally { frame.Unmap(); }
    }
    private void Write(ReadOnlySpan<byte> data)
    {
        hash.AppendData(data);
        process.StandardInput.BaseStream.Write(data);
    }
    public void Dispose()
    {
        if (disposed) return;
        disposed = true;
        try
        {
            process.StandardInput.Close();
            if (!process.WaitForExit(30000)) throw new TimeoutException("FFV1 reference finalization timed out.");
            string stderr = error.GetAwaiter().GetResult();
            if (process.ExitCode != 0) throw new InvalidOperationException(stderr);
            RawHash = Convert.ToHexString(hash.GetHashAndReset());
        }
        finally
        {
            if (!process.HasExited) { process.Kill(true); process.WaitForExit(); }
            process.Dispose(); hash.Dispose();
        }
    }
}

// Optional common real-video fixture. This is a re-encode of supplied media, not a live YMM4 project.
internal sealed class DecodedFixture : IDisposable
{
    private readonly Process process;
    private readonly Task<string> error;
    private readonly byte[] buffer;
    private readonly int stride;
    public DecodedFixture(Options o)
    {
        stride = checked(o.Width * 4);
        buffer = new byte[checked(stride * o.Height)];
        var info = new ProcessStartInfo("ffmpeg.exe") { UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardOutput = true, RedirectStandardError = true };
        foreach (string arg in new[] { "-hide_banner", "-nostdin", "-v", "error", "-threads", "2", "-ss", o.SourceStart.ToString(),
            "-i", o.SourceFile!, "-an", "-sn", "-dn", "-map", "0:v:0", "-frames:v", o.Frames.ToString(),
            "-vf", $"scale={o.Width}:{o.Height}:flags=bicubic:out_range=pc:in_color_matrix=bt709,format=bgra",
            "-fps_mode", "passthrough", "-f", "rawvideo", "pipe:1" }) info.ArgumentList.Add(arg);
        process = Process.Start(info)!;
        error = process.StandardError.ReadToEndAsync();
    }
    public unsafe void Upload(ID2D1Bitmap1 bitmap)
    {
        process.StandardOutput.BaseStream.ReadExactly(buffer);
        fixed (byte* data = buffer) bitmap.CopyFromMemory((IntPtr)data, stride).CheckError();
    }
    public void Dispose()
    {
        try
        {
            if (!process.WaitForExit(10000)) throw new TimeoutException("Fixture decoder did not exit.");
            string stderr = error.GetAwaiter().GetResult();
            if (process.ExitCode != 0) throw new InvalidOperationException(stderr);
        }
        finally
        {
            if (!process.HasExited) { process.Kill(true); process.WaitForExit(); }
            process.Dispose();
        }
    }
}
