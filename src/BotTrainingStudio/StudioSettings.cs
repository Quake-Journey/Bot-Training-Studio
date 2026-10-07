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
    public string Project { get; set; } = "";
    public string Bsp { get; set; } = "";
    public string[] Inputs { get; set; } = [];
    public string[] Tricks { get; set; } = [];
    public string[] Projects { get; set; } = [];
    public string SequenceDataset { get; set; } = "";
    public string TemporalStore { get; set; } = "";
    public string DecisionDataset { get; set; } = "";
    public string DecisionStore { get; set; } = "";
    public string Knowledge { get; set; } = "";
    public string Package { get; set; } = "";
    public string Donor { get; set; } = "";
    public int Context { get; set; } = 16;
    public int SampleLimit { get; set; } = 6000;
    public bool IncludeModel { get; set; }
    public bool IncludeChat { get; set; }
    public int BatchSize { get; set; } = 64;
    public int DecisionBatchSize { get; set; } = 64;
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
        if (result.TemporalStore.Length == 0) result.TemporalStore = Path.Combine(Home, "models", "sequences");
        if (result.DecisionStore.Length == 0) result.DecisionStore = Path.Combine(Home, "models", "decisions");
        if (result.Project.Length == 0) result.Project = Path.Combine(Home, "projects", "new-map");
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
