using System.Runtime.InteropServices;

namespace AMFVideoWriterPlugin;

internal static class AmfNativeMethods
{
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
}
