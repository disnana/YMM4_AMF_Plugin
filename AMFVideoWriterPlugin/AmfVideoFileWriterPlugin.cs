using YukkuriMovieMaker.Commons;
using YukkuriMovieMaker.Plugin;
using YukkuriMovieMaker.Plugin.FileWriter;
using YukkuriMovieMaker.Project;
using YukkuriMovieMaker.Plugin.Update;

namespace AMFVideoWriterPlugin;

public sealed class AmfVideoFileWriterPlugin : IVideoFileWriterPlugin
{
    private readonly AmfSettings _settings = new();
    private readonly PluginDetailsAttribute _details = new()
    {
        AuthorName = "Disnana / tp-li-dev",
        ContentId = "AMFVideoFileWriterPlugin",
    };

    public string Name => "Radeon (AMF) プラグイン出力";

    public PluginDetailsAttribute Details => _details;

    public IPluginUpdater? Updater => null;

    public VideoFileWriterOutputPath OutputPathMode => VideoFileWriterOutputPath.File;

    public IVideoFileWriter CreateVideoFileWriter(string path, VideoInfo videoInfo)
    {
        var snapshot = new AmfSettings
        {
            Codec = _settings.Codec,
            BitrateKbps = _settings.BitrateKbps,
            Quality = _settings.Quality,
            RateControl = _settings.RateControl,
            TexturePoolSize = _settings.TexturePoolSize,
            EnableGpuDirectInput = _settings.EnableGpuDirectInput,
            EnableDebugLog = _settings.EnableDebugLog,
            DiscardOutput = _settings.DiscardOutput,
            EnableProfiling = _settings.EnableProfiling,
        };
        return new AmfVideoFileWriter(path, videoInfo, snapshot);
    }

    public string GetFileExtention()
    {
        return ".mp4";
    }

    public System.Windows.UIElement GetVideoConfigView(string projectName, VideoInfo videoInfo, int length)
    {
        return new AmfConfigView(_settings);
    }

    public bool NeedDownloadResources()
    {
        return false;
    }

    public Task DownloadResources(ProgressMessage progress, CancellationToken token)
    {
        return Task.CompletedTask;
    }
}
