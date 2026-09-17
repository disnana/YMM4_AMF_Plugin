namespace AMFVideoWriterPlugin;

internal sealed class AmfSettings
{
    public AmfCodec Codec { get; set; } = AmfCodec.H264;
    public int BitrateKbps { get; set; } = 12000;
    public AmfQuality Quality { get; set; } = AmfQuality.Balanced;
    public AmfRateControl RateControl { get; set; } = AmfRateControl.YouTubeRecommended;
    public int TexturePoolSize { get; set; } = 6;
    public bool EnableGpuDirectInput { get; set; }
    public bool EnableDebugLog { get; set; }
    public bool DiscardOutput { get; set; }
    public bool EnableProfiling { get; set; }
}

internal enum AmfCodec
{
    H264,
    H265,
}

internal enum AmfQuality
{
    Speed,
    Balanced,
    Quality,
}

internal enum AmfRateControl
{
    Fixed,
    Variable,
    YouTubeRecommended,
}
