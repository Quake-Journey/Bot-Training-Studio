using System.Text.Json;

namespace BotTrainingStudio;

public sealed class StudioSettings
{
    public string Language { get; set; } = "ru";
    public string Theme { get; set; } = "dark";
    public string Python { get; set; } = "";
    public string WorkerDirectory { get; set; } = "";
    public string Backend { get; set; } = "auto";
    public string Profile { get; set; } = "reference";
    public string Dataset { get; set; } = "";
    public string Store { get; set; } = "";
    public static string Home => Environment.GetEnvironmentVariable("BTS_HOME") ??
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "QuakeJourney", "BotTrainingStudio");
    public static StudioSettings Load()
    {
        StudioSettings result;
        try { result = JsonSerializer.Deserialize<StudioSettings>(File.ReadAllText(Path.Combine(Home, "settings.json"))) ?? new(); }
        catch { result = new(); }
        if (result.WorkerDirectory.Length == 0)
        {
            var bundled = Path.Combine(AppContext.BaseDirectory, "worker");
            if (Directory.Exists(bundled)) result.WorkerDirectory = bundled;
        }
        if (result.Python.Length == 0)
        {
            var bundled = Path.Combine(AppContext.BaseDirectory, "runtime", "python.exe");
            if (File.Exists(bundled)) result.Python = bundled;
        }
        if (result.Store.Length == 0) result.Store = Path.Combine(Home, "models", "motion");
        return result;
    }
    public void Save()
    {
        Directory.CreateDirectory(Home);
        var path = Path.Combine(Home, "settings.json");
        File.WriteAllText(path + ".tmp", JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true }));
        File.Move(path + ".tmp", path, true);
    }
}
