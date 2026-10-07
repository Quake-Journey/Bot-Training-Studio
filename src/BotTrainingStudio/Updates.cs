using System.Diagnostics;
using System.IO.Compression;
using System.Net;
using System.Security.Cryptography;
using System.Text.Json;

namespace BotTrainingStudio;

internal sealed record UpdatePart(string Name, long Size, string Sha256);
internal sealed record UpdateManifest(int Schema, string Version, string LibrarySha256, UpdatePart[] App, UpdatePart[] Libraries);
internal sealed record UpdateAsset(string Name, string Url, long Size);
internal sealed record AvailableUpdate(string Version, string Page, UpdateAsset[] Assets);
internal sealed record PackageInventory(string Version, string LibrarySha256, Dictionary<string, string> AppFiles, Dictionary<string, string> LibraryFiles);
internal sealed record UpdateTicket(string Target, string Stage, string ExpectedExeSha256, string InventorySha256, bool ReuseLibraries, int ParentPid, long ParentStartTicks);
internal sealed record UpdateResult(bool Success, string Version, string? Error, int HelperPid);

internal sealed class Updates(HttpClient? client = null) : IDisposable
{
    internal const string Repository = "Quake-Journey/Bot-Training-Studio";
    internal const string ManifestName = "BotTrainingStudio-update-win-x64.json";
    internal static readonly JsonSerializerOptions Json = new() { PropertyNameCaseInsensitive = true, PropertyNamingPolicy = JsonNamingPolicy.CamelCase, WriteIndented = true };
    private readonly HttpClient _http = client ?? new(new HttpClientHandler { AllowAutoRedirect = false }) { Timeout = TimeSpan.FromMinutes(30) };
    public void Dispose() { if (client == null) _http.Dispose(); }

    internal async Task<HttpResponseMessage> GetAsync(string url, CancellationToken token)
    {
        for (int redirect = 0; redirect < 6; redirect++)
        {
            ValidateUrl(url);
            using var request = new HttpRequestMessage(HttpMethod.Get, url);
            request.Headers.UserAgent.ParseAdd("BotTrainingStudio/" + AppVersion.Current);
            var response = await _http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, token);
            if ((int)response.StatusCode is >= 300 and < 400 && response.Headers.Location is { } location)
            { url = new Uri(new Uri(url), location).AbsoluteUri; response.Dispose(); continue; }
            try { response.EnsureSuccessStatusCode(); return response; } catch { response.Dispose(); throw; }
        }
        throw new InvalidDataException("Too many download redirects");
    }

    private static void ValidateUrl(string url)
    {
        if (!Uri.TryCreate(url, UriKind.Absolute, out var uri) || uri.Scheme != "https" || !uri.IsDefaultPort || uri.UserInfo.Length != 0 ||
            uri.Host is not ("api.github.com" or "github.com" or "release-assets.githubusercontent.com" or "objects.githubusercontent.com"))
            throw new InvalidDataException("Unexpected update download host");
        if (uri.Host == "api.github.com" && !uri.AbsolutePath.StartsWith("/repos/" + Repository + "/", StringComparison.Ordinal))
            throw new InvalidDataException("Unexpected update repository");
        if (uri.Host == "github.com" && !uri.AbsolutePath.StartsWith("/" + Repository + "/releases/", StringComparison.Ordinal))
            throw new InvalidDataException("Unexpected release path");
    }

    private async Task<byte[]> ReadSmallAsync(string url, CancellationToken token)
    {
        using var response = await GetAsync(url, token);
        await using var source = await response.Content.ReadAsStreamAsync(token);
        using var data = new MemoryStream(); var buffer = new byte[16384]; int count;
        while ((count = await source.ReadAsync(buffer, token)) > 0)
        { if (data.Length + count > 2 * 1024 * 1024) throw new InvalidDataException("Oversized update metadata"); data.Write(buffer, 0, count); }
        return data.ToArray();
    }

    public async Task<AvailableUpdate?> CheckAsync(CancellationToken token)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token); timeout.CancelAfter(TimeSpan.FromSeconds(15));
        using var document = JsonDocument.Parse(await ReadSmallAsync("https://api.github.com/repos/" + Repository + "/releases?per_page=30", timeout.Token));
        var candidates = new List<AvailableUpdate>();
        foreach (var entry in document.RootElement.EnumerateArray())
        {
            if (entry.GetProperty("draft").GetBoolean() || (!AppVersion.IsPreview && entry.GetProperty("prerelease").GetBoolean())) continue;
            string tag = entry.GetProperty("tag_name").GetString() ?? "";
            if (!StudioVersion.TryParse(tag, out var version) || version.CompareTo(StudioVersion.Parse(AppVersion.Current)) <= 0) continue;
            var assets = entry.GetProperty("assets").EnumerateArray().Select(a => new UpdateAsset(a.GetProperty("name").GetString()!, a.GetProperty("browser_download_url").GetString()!, a.GetProperty("size").GetInt64())).ToArray();
            if (assets.Count(a => a.Name == ManifestName) != 1) continue;
            string prefix = "https://github.com/" + Repository + "/releases/download/" + tag + "/";
            if (assets.Any(a => !a.Url.StartsWith(prefix, StringComparison.Ordinal))) continue;
            candidates.Add(new(tag.TrimStart('v'), "https://github.com/" + Repository + "/releases/tag/" + tag, assets));
        }
        return candidates.OrderByDescending(c => StudioVersion.Parse(c.Version)).FirstOrDefault();
    }

    internal static string Hash(string file) { using var stream = File.OpenRead(file); return Convert.ToHexString(SHA256.HashData(stream)); }
    internal static bool EqualHash(string a, string b) => a.Equals(b, StringComparison.OrdinalIgnoreCase);
    internal static void CheckHashText(string hash) { if (hash.Length != 64 || hash.Any(c => !Uri.IsHexDigit(c))) throw new InvalidDataException("Invalid checksum"); }

    internal async Task DownloadArchiveAsync(UpdatePart[] parts, AvailableUpdate release, string archive, Action<string> progress, CancellationToken token)
    {
        if (parts.Length is < 1 or > 16) throw new InvalidDataException("Invalid number of update parts");
        if (parts.Select(p => p.Name).Distinct(StringComparer.OrdinalIgnoreCase).Count() != parts.Length) throw new InvalidDataException("Duplicate update part");
        await using var output = new FileStream(archive, FileMode.CreateNew, FileAccess.Write, FileShare.None, 1024 * 1024, true);
        var buffer = new byte[1024 * 1024];
        foreach (var part in parts)
        {
            token.ThrowIfCancellationRequested(); CheckHashText(part.Sha256);
            if (Path.GetFileName(part.Name) != part.Name || part.Size is <= 0 or >= 2147483648L) throw new InvalidDataException("Invalid release asset");
            var asset = release.Assets.SingleOrDefault(a => a.Name == part.Name && a.Size == part.Size) ?? throw new InvalidDataException("Missing release asset: " + part.Name);
            using var response = await GetAsync(asset.Url, token);
            await using var source = await response.Content.ReadAsStreamAsync(token);
            using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
            long bytes = 0; int count;
            while ((count = await source.ReadAsync(buffer, token)) > 0)
            {
                bytes += count; if (bytes > part.Size) throw new InvalidDataException("Update part exceeds declared size");
                hash.AppendData(buffer, 0, count); await output.WriteAsync(buffer.AsMemory(0, count), token);
                progress(part.Name + " · " + (bytes / 1048576) + " / " + (part.Size / 1048576) + " MiB");
            }
            if (bytes != part.Size || !EqualHash(Convert.ToHexString(hash.GetHashAndReset()), part.Sha256)) throw new InvalidDataException("Update checksum mismatch: " + part.Name);
        }
    }

    internal static string SafePath(string root, string relative)
    {
        if (relative.Length == 0 || relative.Contains('\\') || relative.Contains(':') || Path.IsPathRooted(relative) || relative.Split('/').Any(p => p.Length == 0 || p is "." or ".." || p.EndsWith('.') || p.EndsWith(' ') || p.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 || System.Text.RegularExpressions.Regex.IsMatch(p, @"^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)", System.Text.RegularExpressions.RegexOptions.IgnoreCase)))
            throw new InvalidDataException("Unsafe package path");
        string target = Path.GetFullPath(Path.Combine(root, relative.Replace('/', Path.DirectorySeparatorChar)));
        if (!target.StartsWith(Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("Package path escapes root");
        return target;
    }

    internal static void NoLinks(string path)
    {
        for (string? current = Path.GetFullPath(path); current != null; current = Path.GetDirectoryName(current))
            if ((File.Exists(current) || Directory.Exists(current)) && (File.GetAttributes(current) & FileAttributes.ReparsePoint) != 0) throw new IOException("A package path contains a link: " + current);
    }

    internal static bool OwnedPath(string relative, bool libraries) => libraries ? relative.StartsWith("libraries/", StringComparison.Ordinal) :
        relative.StartsWith("worker/", StringComparison.Ordinal) || relative.StartsWith("docs/", StringComparison.Ordinal) ||
        relative is "BotTrainingStudio.exe" or "libSkiaSharp.dll" or "libHarfBuzzSharp.dll" or "README.md" or "LICENSE" or "build.json" or "package-files.json" or "CHANGELOG.ru.md" or "CHANGELOG.en.md";

    internal static void Extract(string archivePath, string output, bool libraries, CancellationToken token)
    {
        NoLinks(output); Directory.CreateDirectory(output);
        using var archive = ZipFile.OpenRead(archivePath); var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase); long total = 0;
        if (archive.Entries.Count > 40000) throw new InvalidDataException("Too many archive entries");
        foreach (var entry in archive.Entries)
        {
            token.ThrowIfCancellationRequested();
            if (entry.FullName.EndsWith('/')) { SafePath(output, entry.FullName.TrimEnd('/')); continue; }
            string destination = SafePath(output, entry.FullName);
            if (!OwnedPath(entry.FullName, libraries) || !names.Add(entry.FullName) || ((entry.ExternalAttributes >> 16) & 0xf000) == 0xa000 || (entry.ExternalAttributes & 0x400) != 0)
                throw new InvalidDataException("Unexpected archive entry");
            total += entry.Length; if (total > 12L * 1024 * 1024 * 1024) throw new InvalidDataException("Update archive is too large");
            NoLinks(destination); Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
            entry.ExtractToFile(destination, false);
        }
    }

    internal static PackageInventory ReadInventory(string root)
    {
        var inventory = JsonSerializer.Deserialize<PackageInventory>(File.ReadAllText(Path.Combine(root, "package-files.json")), Json) ?? throw new InvalidDataException("Missing package inventory");
        StudioVersion.Parse(inventory.Version); CheckHashText(inventory.LibrarySha256);
        if (inventory.AppFiles.Count is < 5 or > 4000 || inventory.LibraryFiles.Count is < 1 or > 35000) throw new InvalidDataException("Invalid package inventory size");
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var (relative, hash) in inventory.AppFiles.Concat(inventory.LibraryFiles))
        {
            SafePath(root, relative); CheckHashText(hash);
            if (!seen.Add(relative) || relative == "package-files.json" || !OwnedPath(relative, inventory.LibraryFiles.ContainsKey(relative))) throw new InvalidDataException("Invalid package ownership");
        }
        foreach (string required in new[] { "BotTrainingStudio.exe", "build.json", "worker/opentdm_x_trainer/studio.py", "docs/Bot_Training_Studio_User_Guide_RU.docx", "docs/Bot_Training_Studio_User_Guide_EN.docx" })
            if (!inventory.AppFiles.ContainsKey(required)) throw new InvalidDataException("Incomplete application package");
        if (!inventory.LibraryFiles.TryGetValue("libraries/studio-libraries.json", out var manifest) || !EqualHash(manifest, inventory.LibrarySha256)) throw new InvalidDataException("Incomplete model libraries");
        return inventory;
    }

    internal static bool VerifyFiles(string root, Dictionary<string, string> files, CancellationToken token)
    {
        foreach (var (name, hash) in files)
        {
            token.ThrowIfCancellationRequested(); string path = SafePath(root, name); NoLinks(path);
            if (!File.Exists(path) || !EqualHash(Hash(path), hash)) return false;
        }
        return true;
    }

    public async Task<string> PrepareAsync(AvailableUpdate release, string target, Action<string> progress, CancellationToken token)
    {
        NoLinks(target); string currentExe = Path.Combine(target, "BotTrainingStudio.exe");
        string expectedExe = Hash(currentExe);
        var manifestAsset = release.Assets.Single(a => a.Name == ManifestName);
        var manifest = JsonSerializer.Deserialize<UpdateManifest>(await ReadSmallAsync(manifestAsset.Url, token), Json) ?? throw new InvalidDataException("Missing update metadata");
        if (manifest.Schema != 1 || manifest.Version != release.Version || manifest.Libraries.Length > 16) throw new InvalidDataException("Update version mismatch");
        CheckHashText(manifest.LibrarySha256);
        string updateRoot = Path.Combine(target, ".updates"); NoLinks(updateRoot); Directory.CreateDirectory(updateRoot);
        string job = Path.Combine(updateRoot, Guid.NewGuid().ToString("N")); Directory.CreateDirectory(job);
        File.WriteAllText(Path.Combine(job, "owned.json"), "{\"owner\":\"BotTrainingStudio\"}");
        try
        {
            long required = manifest.App.Sum(p => p.Size) * 4 + 256L * 1024 * 1024;
            if (new DriveInfo(Path.GetPathRoot(job)!).AvailableFreeSpace < required) throw new IOException("Insufficient free space for update");
            string appZip = Path.Combine(job, "app.zip"), stage = Path.Combine(job, "new");
            await DownloadArchiveAsync(manifest.App, release, appZip, progress, token);
            await Task.Run(() => Extract(appZip, stage, false, token), token);
            var inventory = ReadInventory(stage);
            if (inventory.Version != manifest.Version || !EqualHash(inventory.LibrarySha256, manifest.LibrarySha256) || !await Task.Run(() => VerifyFiles(stage, inventory.AppFiles, token), token)) throw new InvalidDataException("Application package validation failed");
            progress("verify-libraries");
            bool reuse = File.Exists(Path.Combine(target, "libraries", "studio-libraries.json")) && EqualHash(Hash(Path.Combine(target, "libraries", "studio-libraries.json")), inventory.LibrarySha256)
                && await Task.Run(() => VerifyFiles(target, inventory.LibraryFiles, token), token);
            if (!reuse)
            {
                if (new DriveInfo(Path.GetPathRoot(job)!).AvailableFreeSpace < manifest.Libraries.Sum(p => p.Size) + 12L * 1024 * 1024 * 1024) throw new IOException("Insufficient free space to replace model libraries");
                string libsZip = Path.Combine(job, "libraries.zip");
                await DownloadArchiveAsync(manifest.Libraries, release, libsZip, progress, token);
                await Task.Run(() => Extract(libsZip, stage, true, token), token);
                if (!await Task.Run(() => VerifyFiles(stage, inventory.LibraryFiles, token), token)) throw new InvalidDataException("Model library validation failed");
            }
            token.ThrowIfCancellationRequested();
            using var self = Process.GetCurrentProcess();
            var ticket = new UpdateTicket(Path.GetFullPath(target), stage, expectedExe, Hash(Path.Combine(stage, "package-files.json")), reuse, Environment.ProcessId, self.StartTime.ToUniversalTime().Ticks);
            string path = Path.Combine(job, "ticket.json"); await File.WriteAllTextAsync(path, JsonSerializer.Serialize(ticket, Json), token);
            return path;
        }
        catch { DeleteOwnedJob(job); throw; }
    }

    internal static void DeleteOwnedJob(string job)
    {
        NoLinks(job);
        if (Path.GetFileName(Path.GetDirectoryName(job)) != ".updates" || !Guid.TryParseExact(Path.GetFileName(job), "N", out _) || !File.Exists(Path.Combine(job, "owned.json"))) throw new IOException("Not a Studio update directory");
        // Reject child links before any recursive operation.
        var pending = new Stack<string>(); pending.Push(job);
        while (pending.Count > 0) foreach (string entry in Directory.EnumerateFileSystemEntries(pending.Pop()))
        { NoLinks(entry); if (Directory.Exists(entry)) pending.Push(entry); }
        Directory.Delete(job, true);
    }
}
