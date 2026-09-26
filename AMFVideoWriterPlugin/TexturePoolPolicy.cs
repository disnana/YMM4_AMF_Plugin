namespace AMFVideoWriterPlugin;

internal static class TexturePoolPolicy
{
    public const int Minimum = 4, LegacyMaximum = 8, Maximum = 128;
    public const ulong ExpandedPayloadLimit = 1024UL * 1024 * 1024;
    public static readonly int[] Choices = [4, 6, 8, 16, 32, 64, 128];

    // Keep in sync with AmfTexturePoolPolicy.h. This is a payload estimate,
    // not a guarantee on total resident VRAM or the encoder's internal memory.
    public static int Resolve(int requested, int width, int height)
    {
        requested = Math.Clamp(requested, Minimum, Maximum);
        if (requested <= LegacyMaximum) return requested;
        if (width <= 0 || height <= 0) return LegacyMaximum;
        ulong frameBytes = (ulong)width * (ulong)height * 4;
        int fitting = (int)Math.Min((ulong)Maximum, ExpandedPayloadLimit / frameBytes);
        return Math.Min(requested, Math.Max(LegacyMaximum, fitting));
    }
}
