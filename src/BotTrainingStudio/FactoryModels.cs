using System.Security.Cryptography;
using System.Text.Json;

namespace BotTrainingStudio;

// Older public updaters accept worker/ payloads. The new app materializes its
// verified factory files under Models; this never targets the user's stores.
internal static class FactoryModels
{
    public static string DirectoryPath => Path.Combine(AppContext.BaseDirectory, "Models");
    internal static void Ensure(string? application = null)
    {
        application ??= AppContext.BaseDirectory;
        string source = Path.Combine(application, "worker", "factory-models"), target = Path.Combine(application, "Models");
        if (!File.Exists(Path.Combine(source, "catalog.json"))) return;
        if (new DirectoryInfo(target).Exists && new DirectoryInfo(target).Attributes.HasFlag(FileAttributes.ReparsePoint))
            throw new IOException("Models must be a regular application folder");
        using var document = JsonDocument.Parse(File.ReadAllText(Path.Combine(source, "catalog.json")));
        if (document.RootElement.GetProperty("schema").GetString() != "bts-factory-models-v1") throw new InvalidDataException("Invalid factory catalog");
        var files = new List<(string Name, string Hash)>();
        foreach (var model in document.RootElement.GetProperty("models").EnumerateArray())
        {
            string id = model.GetProperty("id").GetString()!;
            if (!System.Text.RegularExpressions.Regex.IsMatch(id, "^[a-z0-9-]{1,80}$")) throw new InvalidDataException("Invalid factory ID");
            files.Add((id + "/model.json", model.GetProperty("metadata_sha256").GetString()!));
            files.Add((id + "/weights.safetensors", model.GetProperty("weights_sha256").GetString()!));
        }
        if (files.Count is < 2 or > 32) throw new InvalidDataException("Invalid factory catalog size");
        string Hash(string file) { using var stream = File.OpenRead(file); return Convert.ToHexString(SHA256.HashData(stream)).ToLowerInvariant(); }
        foreach (var (name, hash) in files)
        {
            string input = Path.Combine(source, name), output = Path.Combine(target, name);
            if (Hash(input) != hash) throw new InvalidDataException("Factory package integrity failure");
            string parent = Path.GetDirectoryName(output)!;
            if (Directory.Exists(parent) && new DirectoryInfo(parent).Attributes.HasFlag(FileAttributes.ReparsePoint)) throw new IOException("Factory folder cannot be a link");
            if (File.Exists(output))
            {
                if (Hash(output) != hash) throw new InvalidDataException("Factory model was modified; keep user models in a separate folder");
                continue;
            }
            Directory.CreateDirectory(parent);
            File.Copy(input, output + ".pending", true);
            if (Hash(output + ".pending") != hash) throw new InvalidDataException("Factory copy integrity failure");
            File.Move(output + ".pending", output);
        }
        Directory.CreateDirectory(target);
        if (File.Exists(Path.Combine(target,"catalog.json")) && Hash(Path.Combine(target,"catalog.json")) == Hash(Path.Combine(source,"catalog.json"))) return;
        File.Copy(Path.Combine(source, "catalog.json"), Path.Combine(target, "catalog.json.pending"), true);
        File.Move(Path.Combine(target, "catalog.json.pending"), Path.Combine(target, "catalog.json"), true);
    }

    public static string Summary(string language)
    {
        try
        {
            using var doc = JsonDocument.Parse(File.ReadAllText(Path.Combine(DirectoryPath, "catalog.json")));
            return string.Join("\n", doc.RootElement.GetProperty("models").EnumerateArray().Select(m =>
                m.GetProperty("profile").GetString() + " · " + string.Join(", ",m.GetProperty("maps").EnumerateArray().Select(x=>x.GetString()))));
        }
        catch { return language == "ru" ? "Заводские веса отсутствуют. Распакуй полный комплект программы." : "Factory weights are missing. Extract the complete application package."; }
    }
    public static bool IsApplicationPath(string path)
    {
        string relative = Path.GetRelativePath(AppContext.BaseDirectory, Path.GetFullPath(path));
        return relative == "." || !relative.StartsWith(".." + Path.DirectorySeparatorChar) && !Path.IsPathRooted(relative);
    }
}
