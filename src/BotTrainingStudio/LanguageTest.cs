using System.Text.Json;

namespace BotTrainingStudio;

internal static class LanguageTest
{
    public static int Run(string[] args)
    {
        string receipt = Path.GetFullPath(args[Array.IndexOf(args, "--language-test") + 1]);
        Directory.CreateDirectory(Path.GetDirectoryName(receipt)!);
        string? originalHome = Environment.GetEnvironmentVariable("BTS_HOME");
        string home = Path.Combine(Path.GetDirectoryName(receipt)!, "settings-" + Guid.NewGuid().ToString("N"));
        Environment.SetEnvironmentVariable("BTS_HOME", home);
        var checks = new List<string>();
        void Check(bool ok, string name) { if (!ok) throw new Exception(name); checks.Add(name); }
        try
        {
            Check(new StudioSettings().Language == "system", "New settings use system UI language");
            Check(StudioSettings.Load().Language == "system", "First launch uses system UI language");
            foreach (string culture in new[] { "ru-RU", "ru-BY", "ru", "RU-ru" })
                Check(StudioSettings.ResolveLanguage("system", culture) == "ru", culture + " selects Russian");
            foreach (string culture in new[] { "en-US", "en-GB", "de-DE", "uk-UA", "zh-CN", "" })
                Check(StudioSettings.ResolveLanguage("system", culture) == "en", culture + " selects English");
            Check(StudioSettings.ResolveLanguage("en", "ru-RU") == "en", "English override survives Russian OS");
            Check(StudioSettings.ResolveLanguage("ru", "en-US") == "ru", "Russian override survives English OS");
            foreach (string language in new[] { "ru", "en", "system" })
            {
                var s = new StudioSettings { Language = language }; s.Save();
                Check(StudioSettings.Load().Language == language, language + " persists after reload");
            }
            File.WriteAllText(Path.Combine(home, "settings.json"), "{\"Language\":\"obsolete\"}");
            Check(StudioSettings.Load().Language == "system", "Invalid setting falls back to system");
            File.WriteAllText(Path.Combine(home, "settings.json"), "{\"Language\":null}");
            Check(StudioSettings.Load().Language == "system", "Null setting falls back to system");
            Check(!JsonSerializer.Serialize(new StudioSettings()).Contains("EffectiveLanguage"), "Resolved language is not persisted as explicit choice");
            File.WriteAllText(receipt, JsonSerializer.Serialize(new { pass = true, checks, live_system_language = new StudioSettings().EffectiveLanguage }, new JsonSerializerOptions { WriteIndented = true }));
            return 0;
        }
        catch (Exception ex)
        {
            File.WriteAllText(receipt, JsonSerializer.Serialize(new { pass = false, checks, error = ex.ToString() })); return 1;
        }
        finally { Environment.SetEnvironmentVariable("BTS_HOME", originalHome); }
    }
}
