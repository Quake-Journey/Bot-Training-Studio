using System.Text.Json;
using System.Text.Json.Serialization;
using System.Globalization;
using System.Runtime.InteropServices;

namespace BotTrainingStudio;

public sealed class StudioSettings
{
    public string Language { get; set; } = "system";
    private static readonly string SystemUiLanguage = ReadSystemUiLanguage();
    [JsonIgnore] public string EffectiveLanguage => ResolveLanguage(Language, SystemUiLanguage);
    public static string ResolveLanguage(string? choice, string systemUiLanguage) => choice switch
    {
        "ru" => "ru", "en" => "en",
        _ => systemUiLanguage.Split('-', '_')[0].Equals("ru", StringComparison.OrdinalIgnoreCase) ? "ru" : "en"
    };
    private static string ReadSystemUiLanguage()
    {
        if (OperatingSystem.IsWindows())
        {
            try
            {
                uint size = 0;
                if (GetUserPreferredUILanguages(8, out _, null, ref size) && size is > 0 and < 4096)
                {
                    var languages = new char[size];
                    if (GetUserPreferredUILanguages(8, out _, languages, ref size))
                    {
                        string first = new string(languages).Split('\0')[0];
                        if (first.Length > 0) return first;
                    }
                }
                return CultureInfo.GetCultureInfo(GetUserDefaultUILanguage()).Name;
            }
            catch (Exception) { }
        }
        return CultureInfo.CurrentUICulture.Name;
    }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern bool GetUserPreferredUILanguages(uint flags, out uint count, [Out] char[]? buffer, ref uint length);
    [DllImport("kernel32.dll")] private static extern ushort GetUserDefaultUILanguage();
    public string Theme { get; set; } = "dark";
    public bool AutoUpdateCheck { get; set; } = true;
    public string LastSeenVersion { get; set; } = "";
    public string Python { get; set; } = "";
    public string WorkerDirectory { get; set; } = "";
    public string Backend { get; set; } = "auto";
    public string RuntimeBackend { get; set; } = "cpu";
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
        if (result.Language is not ("system" or "ru" or "en")) result.Language = "system";
        if (!RuntimeSetup.Backends.Contains(result.RuntimeBackend)) result.RuntimeBackend = "cpu";
        if (RuntimeSetup.Libraries is { } libraries) result.RuntimeBackend = libraries.Backend;
        if (result.WorkerDirectory.Length == 0)
        {
            var bundled = Path.Combine(AppContext.BaseDirectory, "worker");
            if (Directory.Exists(bundled)) result.WorkerDirectory = bundled;
        }
        if (result.Python.Length == 0)
        {
            var bundled = Path.Combine(AppContext.BaseDirectory, "runtime", "python.exe");
            result.Python = RuntimeSetup.FindManaged(Home, result.RuntimeBackend) ?? (File.Exists(bundled) ? bundled : "");
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
