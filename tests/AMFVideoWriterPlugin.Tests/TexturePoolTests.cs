using System.Runtime.InteropServices;
using System.Text.Json;
using System.Windows.Controls;
using AMFVideoWriterPlugin;

internal static partial class Program
{
    private static void RunTexturePoolTests()
    {
        Check(new AmfSettings().TexturePoolSize == 8, "Optimized preview uses eight owned textures");
        foreach (int size in TexturePoolPolicy.Choices)
            Check(TexturePoolPolicy.Resolve(size, 1920, 1080) == size, "1080p pool choice preserved");
        Check(TexturePoolPolicy.Resolve(128, 3840, 2160) == 32, "4K expanded pool budget");
        Check(TexturePoolPolicy.Resolve(128, 7680, 4320) == 8, "8K expanded pool budget");
        Check(TexturePoolPolicy.Resolve(6, 16384, 16384) == 6, "Legacy bounds unaffected by new budget");
        Check(TexturePoolPolicy.Resolve(128, int.MaxValue, int.MaxValue) == 8, "Dimensions cannot overflow budget math");
        Check(TexturePoolPolicy.Resolve(128, 0, 0) == 8, "Unknown resolution cannot expand pool");
        Check(TexturePoolPolicy.Resolve(int.MaxValue, 1920, 1080) == 128, "Upper count bound");
        Check(TexturePoolPolicy.Resolve(int.MinValue, 1920, 1080) == 4, "Lower count bound");
        Check(Marshal.SizeOf<NativeTexturePoolStatus>() == 64, "Texture pool ABI size");
        Check(Marshal.OffsetOf<NativeTexturePoolStatus>(nameof(NativeTexturePoolStatus.NominalBytes)).ToInt32() == 32, "Pool ABI alignment");
        var status = new NativeTexturePoolStatus { Version = 1, RequestedSize = 128, EffectiveSize = 32,
            PeakInputsInUse = 32, PendingOutputLimit = 64, ExpandedPayloadLimitBytes = TexturePoolPolicy.ExpandedPayloadLimit };
        Check(Json(status.ToReport()).GetProperty("effective_size").GetUInt32() == 32, "Actual pool count reported");
        status.PeakInputsInUse = 33;
        try { status.ToReport(); throw new Exception("Invalid peak accepted"); }
        catch (InvalidOperationException) { _checks++; }
        var recycled = new NativeInputRecycleStatus { Version = 1, Requested = 1, PoolSize = 128, MaxPendingOutputs = 256 };
        recycled.ToReport(); _checks++;
        foreach (int size in TexturePoolPolicy.Choices)
        {
            var settings = new AmfSettings { TexturePoolSize = size, Quality = AmfQuality.Quality, BitrateKbps = 27000,
                RecycleInputAfterRelease = true, OptimizeOutputWait = true, EnableGpuDirectInput = true };
            string Others() => JsonSerializer.Serialize(new { settings.Quality, settings.Codec, settings.RateControl,
                settings.BitrateKbps, settings.RecycleInputAfterRelease, settings.OptimizeOutputWait, settings.EnableGpuDirectInput });
            string expected = Others();
            var panel = (StackPanel)new AmfConfigView(settings).Content;
            var combo = panel.Children.OfType<ComboBox>().Single(x => x.Items.Cast<string>().Any(i => i.Contains("増量")));
            Check(TexturePoolPolicy.Choices[combo.SelectedIndex] == size, "Pool selection restored");
            for (int i = 0; i < TexturePoolPolicy.Choices.Length; i++)
            {
                combo.SelectedIndex = i;
                Check(settings.TexturePoolSize == TexturePoolPolicy.Choices[i] && Others() == expected,
                    "Pool option does not change compression, recycling or waits");
            }
        }
    }
}
