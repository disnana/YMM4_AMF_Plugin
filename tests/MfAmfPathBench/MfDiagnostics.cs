using System.Runtime.InteropServices;
using Vortice.MediaFoundation;
using YukkuriMovieMaker.Plugin.FileWriter;

internal static class MfDiagnostics
{
    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int GetCodecValue(IntPtr self, in Guid property, IntPtr variant);
    [DllImport("oleaut32.dll")] private static extern int VariantClear(IntPtr variant);
    private static readonly Guid CodecInterface = new("901db4c7-31ce-41a2-85dc-8fa0bf41b8da");
    private static readonly Dictionary<string, Guid> CodecProperties = new()
    {
        ["rate_control"] = new(470157545, 14092, 18192, 138, 88, 203, 97, 129, 196, 36, 35),
        ["quality"] = new(4240398243u, 32421, 19212, 150, 68, 105, 180, 12, 57, 195, 145),
        ["quality_vs_speed"] = new(2553490936u, 973, 18283, 137, 250, 63, 158, 68, 45, 236, 159),
        ["mean_bitrate"] = new(4146209652u, 8516, 18453, 181, 80, 163, 127, 142, 18, 238, 82),
        ["gop"] = new(2515737382u, 38308, 16810, 147, 3, 36, 106, 127, 198, 238, 241),
        ["b_frames"] = new(2369325740u, 56412, 16896, 181, 127, 129, 77, 4, 186, 186, 178),
        ["min_qp"] = new(249703530, 41852, 17768, 181, 241, 157, 76, 43, 58, 184, 134),
        ["max_qp"] = new(1034907494u, 42663, 17888, 168, 229, 242, 116, 63, 70, 163, 162),
        ["cabac"] = new(4000099682u, 54021, 16968, 165, 14, 225, 178, 85, 247, 202, 248),
    };
    public static object Read(IVideoFileWriter3 writer)
    {
        try
        {
            var sink = (IMFSinkWriter)Program.Field(writer, "writer")!;
            int stream = (int)Program.Field(writer, "videoIndex")!;
            using var extended = sink.QueryInterface<IMFSinkWriterEx>();
            var transforms = new List<object>();
            for (int i = 0; i < 8; i++)
            {
                IMFTransform transform;
                Guid category;
                try { extended.GetTransformForStream(stream, i, out category, out transform); }
                catch { break; }
                using (transform)
                {
                    var attributes = new Dictionary<string, object?>();
                    try
                    {
                        using var raw = transform.Attributes;
                        for (int j = 0; j < raw.Count; j++)
                        {
                            object value = raw.GetByIndex(j, out var key);
                            attributes[key.ToString()] = value is string or int or uint or long or ulong or bool or Guid ? value : value?.GetType().Name;
                            (value as IDisposable)?.Dispose();
                        }
                    }
                    catch (Exception e) { attributes["read_error"] = e.Message; }
                    transforms.Add(new { category, attributes, input_type = ReadType(transform, true),
                        output_type = ReadType(transform, false), codec_api = ReadCodec(transform.NativePointer) });
                }
            }
            return new { status = "captured", transforms };
        }
        catch (Exception e) { return new { status = "unavailable", error = e.ToString() }; }
    }

    private static object ReadType(IMFTransform transform, bool input)
    {
        try
        {
            using var type = input ? transform.GetInputCurrentType(0) : transform.GetOutputCurrentType(0);
            var attributes = new Dictionary<string, object?>();
            for (int i = 0; i < type.Count; i++)
            {
                object value = type.GetByIndex(i, out var key);
                attributes[key.ToString()] = value is string or int or uint or long or ulong or bool or Guid ? value : value?.GetType().Name;
                (value as IDisposable)?.Dispose();
            }
            return attributes;
        }
        catch (Exception e) { return new { error = e.Message }; }
    }

    private static object ReadCodec(IntPtr transform)
    {
        var iid = CodecInterface;
        int hr = Marshal.QueryInterface(transform, in iid, out var codec);
        if (hr < 0) return new { query_interface_hresult = hr };
        try
        {
            // Windows SDK strmif.h: IUnknown 0..2, five property/capability methods, GetValue at slot 8.
            var get = Marshal.GetDelegateForFunctionPointer<GetCodecValue>(Marshal.ReadIntPtr(Marshal.ReadIntPtr(codec), 8 * IntPtr.Size));
            var values = new Dictionary<string, object?>();
            foreach (var pair in CodecProperties)
            {
                IntPtr variant = Marshal.AllocCoTaskMem(24);
                try
                {
                    Marshal.WriteInt64(variant, 0, 0); Marshal.WriteInt64(variant, 8, 0); Marshal.WriteInt64(variant, 16, 0);
                    Guid property = pair.Value;
                    int result = get(codec, in property, variant);
                    int type = Marshal.ReadInt16(variant);
                    object? value = result >= 0 && type is 0 or 2 or 3 or 4 or 5 or 8 or 11 or 17 or 18 or 19 or 20 or 21
                        ? Marshal.GetObjectForNativeVariant(variant) : null;
                    values[pair.Key] = new { hresult = result, variant_type = type, value };
                }
                finally { VariantClear(variant); Marshal.FreeCoTaskMem(variant); }
            }
            return values;
        }
        finally { Marshal.Release(codec); }
    }
}
