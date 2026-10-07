using System.Diagnostics;
using System.IO.Compression;
using System.Net.Http;
using System.Reflection;
using System.Security.Cryptography;
using System.Text.Json;

namespace BotTrainingStudio;

internal sealed record RuntimeProbe(bool Ready, string Detail, string Python = "", string Torch = "", bool Cuda = false);
internal sealed record RuntimeProgress(string Stage, string Name = "", double Fraction = 0, long Bytes = 0);
internal sealed record RuntimeArchive(string Name, string Url, string Sha256);
internal sealed record BundledLibraries(string Directory, string Backend, string PythonMinor);

/// <summary>Installs only pinned CPython; model/application libraries must already ship in the package.
/// Only a fully probed interpreter becomes active. Downloads, extraction and probes run off the UI thread.</summary>
internal sealed class RuntimeSetup
{
    internal static readonly string[] Backends = ["cpu", "cuda"];
    internal static BundledLibraries? Libraries
    {
        get
        {
            try
            {
                string directory = Path.Combine(AppContext.BaseDirectory, "libraries");
                using var doc = JsonDocument.Parse(File.ReadAllText(Path.Combine(directory, "studio-libraries.json")));
                string backend = doc.RootElement.GetProperty("backend").GetString()!;
                string minor = doc.RootElement.GetProperty("python_minor").GetString()!;
                if (!Backends.Contains(backend) || minor != "3.13") return null;
                if (!File.Exists(Path.Combine(directory, "torch", "__init__.py"))) return null;
                return new(directory, backend, minor);
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException or KeyNotFoundException) { return null; }
        }
    }
    private static readonly HashSet<string> Hosts = ["www.python.org", "files.pythonhosted.org", "download.pytorch.org", "download-r2.pytorch.org"];
    private const long MaxDownload = 4L * 1024 * 1024 * 1024;
    private const long MaxExtracted = 12L * 1024 * 1024 * 1024;
    internal static Stream Resource(string name) => Assembly.GetExecutingAssembly().GetManifestResourceStream("BotTrainingStudio." + name)
        ?? throw new FileNotFoundException("Missing application resource: " + name);

    internal static string Manifest(string backend)
    {
        if (!Backends.Contains(backend)) throw new ArgumentException("Unsupported runtime backend");
        using var reader = new StreamReader(Resource($"runtime-{backend}-win-x64.lock.json"));
        return reader.ReadToEnd();
    }

    internal static RuntimeArchive[] Archives(string manifest)
    {
        using var doc = JsonDocument.Parse(manifest);
        var root = doc.RootElement;
        var result = new List<RuntimeArchive> { new("CPython", root.GetProperty("python_url").GetString()!, root.GetProperty("python_sha256").GetString()!) };
        foreach (var p in root.GetProperty("packages").EnumerateArray())
        {
            var source = p.GetProperty("source");
            result.Add(new(p.GetProperty("name").GetString()!, source.GetProperty("url").GetString()!,
                source.GetProperty("archive_info").GetProperty("hashes").GetProperty("sha256").GetString()!));
        }
        foreach (var a in result)
        {
            CheckUrl(new Uri(a.Url));
            if (a.Sha256.Length != 64 || !a.Sha256.All(Uri.IsHexDigit)) throw new InvalidDataException("Missing archive digest");
        }
        return result.ToArray();
    }

    internal static void CheckUrl(Uri uri)
    {
        if (uri.Scheme != "https" || !Hosts.Contains(uri.Host) || !uri.IsDefaultPort || uri.UserInfo.Length > 0)
            throw new InvalidDataException("Unexpected runtime download source");
    }

    internal static string? FindManaged(string home, string backend)
    {
        if (!Backends.Contains(backend)) return null;
        try
        {
            string root = Path.Combine(home, "runtimes");
            string generation = File.ReadAllText(Path.Combine(root, "active-" + backend + ".txt")).Trim();
            if (!generation.StartsWith(backend + "-", StringComparison.Ordinal) || generation != Path.GetFileName(generation)
                || generation.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0) return null;
            string path = Path.Combine(root, generation, "python.exe");
            return File.Exists(path) ? path : null;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { return null; }
    }

    internal static IEnumerable<string> Candidates(string home, string backend)
    {
        var candidates = new List<string>();
        foreach (var choice in new[] { backend }.Concat(Backends).Distinct())
            if (FindManaged(home, choice) is { } managed) candidates.Add(managed);
        candidates.Add(Path.Combine(AppContext.BaseDirectory, "runtime", "python.exe"));
        foreach (string part in (Environment.GetEnvironmentVariable("PATH") ?? "").Split(Path.PathSeparator))
        {
            string directory = part.Trim().Trim('"');
            if (Path.IsPathFullyQualified(directory) && !directory.Contains("WindowsApps", StringComparison.OrdinalIgnoreCase))
                candidates.Add(Path.Combine(directory, "python.exe"));
        }
        if (OperatingSystem.IsWindows())
        {
            foreach (var hive in new[] { Microsoft.Win32.RegistryHive.CurrentUser, Microsoft.Win32.RegistryHive.LocalMachine })
            {
                using var key = Microsoft.Win32.RegistryKey.OpenBaseKey(hive, Microsoft.Win32.RegistryView.Registry64)
                    .OpenSubKey(@"Software\Python\PythonCore");
                if (key == null) continue;
                foreach (string version in key.GetSubKeyNames())
                {
                    using var install = key.OpenSubKey(version + @"\InstallPath");
                    if (install?.GetValue("ExecutablePath") is string exe) candidates.Add(exe);
                    else if (install?.GetValue("") is string directory) candidates.Add(Path.Combine(directory, "python.exe"));
                }
            }
        }
        return candidates.Where(File.Exists).Distinct(StringComparer.OrdinalIgnoreCase);
    }

    public Task<string> InstallAsync(string home, string backend, Action<RuntimeProgress> progress, CancellationToken token) =>
        Task.Run(() => InstallCore(home, backend, progress, token), token);

    private async Task<string> InstallCore(string home, string backend, Action<RuntimeProgress> progress, CancellationToken token)
    {
        if (!OperatingSystem.IsWindows() || !Environment.Is64BitProcess) throw new PlatformNotSupportedException("Windows x64 is required");
        var libraries = Libraries ?? throw new InvalidDataException("The package is missing its libraries folder. Extract the complete application package.");
        backend = libraries.Backend;
        string manifest = Manifest(backend);
        var archives = Archives(manifest).Take(1).ToArray(); // Libraries ship with the application; download only CPython.
        string root = Path.GetFullPath(Path.Combine(home, "runtimes"));
        Directory.CreateDirectory(root);
        // OS handle prevents two application instances installing concurrently. It survives a stale lock file.
        using var exclusive = new FileStream(Path.Combine(root, "setup.lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
        if (FindManaged(home, backend) is { } existing)
        {
            progress(new("checking"));
            if ((await ProbeAsync(existing, token)).Ready) return existing;
        }
        string id = Guid.NewGuid().ToString("N");
        string stage = Path.Combine(root, ".setup-" + id);
        string generation = backend + "-" + id;
        string destination = Path.Combine(root, generation);
        Directory.CreateDirectory(stage);
        try
        {
            // Only the small interpreter is downloaded. Packaged model libraries stay in place.
            long space = new DriveInfo(Path.GetPathRoot(root)!).AvailableFreeSpace;
            long required = 100L * 1024 * 1024;
            if (space < required) throw new IOException("Insufficient disk space: 100 MiB required for Python");
            using var http = new HttpClient { Timeout = Timeout.InfiniteTimeSpan };
            http.DefaultRequestHeaders.UserAgent.ParseAdd("BotTrainingStudio/0.2");
            for (int i = 0; i < archives.Length; i++)
            {
                token.ThrowIfCancellationRequested();
                var archive = archives[i];
                string downloaded = Path.Combine(stage, ".download.zip");
                await DownloadAsync(http, archive, downloaded,
                    bytes => progress(new("downloading", archive.Name, (double)i / archives.Length, bytes)), token);
                progress(new("extracting", archive.Name, (double)i / archives.Length));
                await ExtractAsync(downloaded, i == 0 ? stage : Path.Combine(stage, "Lib", "site-packages"), i != 0, token);
                File.Delete(downloaded);
            }
            await File.WriteAllTextAsync(Path.Combine(stage, "python313._pth"), "python313.zip\n.\nLib/site-packages\nimport site\n", token);
            progress(new("checking", Fraction: .97));
            var probe = await ProbeAsync(Path.Combine(stage, "python.exe"), token);
            if (!probe.Ready) throw new InvalidDataException(probe.Detail);
            using var parsed = JsonDocument.Parse(manifest);
            string expectedTorch = parsed.RootElement.GetProperty("packages").EnumerateArray()
                .Single(p => p.GetProperty("name").GetString() == "torch").GetProperty("version").GetString()!;
            if (probe.Torch != expectedTorch) throw new InvalidDataException("Installed PyTorch version mismatch");
            await File.WriteAllTextAsync(Path.Combine(stage, "runtime.json"), manifest, token);
            token.ThrowIfCancellationRequested();
            // Commit only complete generations. Cancellation before this point leaves the old pointer intact.
            // Windows scanners can briefly hold a just-verified DLL/EXE after the probe exits.
            // Retry the atomic rename; never merge over or delete an existing generation.
            for (int retry = 0; ; retry++)
            {
                token.ThrowIfCancellationRequested();
                try { Directory.Move(stage, destination); break; }
                catch (Exception ex) when (retry < 6 && ex is IOException or UnauthorizedAccessException)
                { await Task.Delay(100 * (retry + 1), token); }
            }
            string pointer = Path.Combine(root, "active-" + backend + ".txt");
            string pointerTemp = Path.Combine(root, "active-" + id + ".tmp");
            try { await File.WriteAllTextAsync(pointerTemp, generation); File.Move(pointerTemp, pointer, true); }
            finally { if (File.Exists(pointerTemp)) File.Delete(pointerTemp); }
            progress(new("ready", Fraction: 1));
            return Path.Combine(destination, "python.exe");
        }
        finally
        {
            // Exact owned staging directory only, never an existing or selected runtime.
            for (int retry = 0; ; retry++)
            {
                try { DeleteStage(root, stage); break; }
                catch (Exception ex) when (retry < 8 && ex is IOException or UnauthorizedAccessException)
                { await Task.Delay(150 * (retry + 1)); }
            }
        }
    }

    internal static void DeleteStage(string root, string stage)
    {
        string full = Path.GetFullPath(stage);
        if (Path.GetDirectoryName(full) != Path.GetFullPath(root) || !Path.GetFileName(full).StartsWith(".setup-", StringComparison.Ordinal))
            throw new InvalidDataException("Refusing cleanup outside runtime staging");
        if (Directory.Exists(full))
        {
            if ((File.GetAttributes(full) & FileAttributes.ReparsePoint) != 0) throw new IOException("Unexpected staging link");
            Directory.Delete(full, true);
        }
    }

    internal static async Task DownloadAsync(HttpClient http, RuntimeArchive archive, string path, Action<long> progress, CancellationToken token)
    {
        CheckUrl(new Uri(archive.Url));
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token);
        timeout.CancelAfter(TimeSpan.FromMinutes(30));
        var ct = timeout.Token;
        using var response = await http.GetAsync(archive.Url, HttpCompletionOption.ResponseHeadersRead, ct);
        response.EnsureSuccessStatusCode();
        CheckUrl(response.RequestMessage!.RequestUri!);
        if (response.Content.Headers.ContentLength > MaxDownload) throw new InvalidDataException("Archive exceeds size limit");
        await using var input = await response.Content.ReadAsStreamAsync(ct);
        await using var output = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None, 131072, true);
        using var digest = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        var buffer = new byte[131072];
        long total = 0;
        var watch = Stopwatch.StartNew();
        int read;
        while ((read = await input.ReadAsync(buffer, ct)) != 0)
        {
            total += read;
            if (total > MaxDownload) throw new InvalidDataException("Archive exceeds size limit");
            digest.AppendData(buffer, 0, read);
            await output.WriteAsync(buffer.AsMemory(0, read), ct);
            if (watch.ElapsedMilliseconds > 200) { progress(total); watch.Restart(); }
        }
        progress(total);
        if (!Convert.ToHexString(digest.GetHashAndReset()).Equals(archive.Sha256, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("Archive SHA256 mismatch: " + archive.Name);
    }

    internal static async Task ExtractAsync(string path, string folder, bool wheel, CancellationToken token)
    {
        string root = Path.GetFullPath(folder) + Path.DirectorySeparatorChar;
        Directory.CreateDirectory(root);
        using var archive = ZipFile.OpenRead(path);
        long expanded = 0;
        foreach (var entry in archive.Entries)
        {
            token.ThrowIfCancellationRequested();
            string name = entry.FullName;
            // ZIP paths are portable forward-slash paths. Refuse aliases, links and path escapes.
            var parts = name.Split('/');
            if (name.Contains('\\') || name.Contains(':') || name.StartsWith('/') || parts.Any(p => p is "." or ".." || p.EndsWith(' ') || p.EndsWith('.'))
                || ((entry.ExternalAttributes >> 16) & 0xf000) == 0xa000)
                throw new InvalidDataException("Unsafe archive path");
            string entryRoot = root;
            if (wheel && parts[0].EndsWith(".data", StringComparison.Ordinal))
            {
                if (name.EndsWith('/') && parts.Length <= 3) continue; // scheme directory, not a file
                if (parts.Length < 3) throw new InvalidDataException("Unsupported wheel installation layout: " + name);
                string runtimeRoot = Path.GetFullPath(Path.Combine(folder, "..", ".."));
                entryRoot = parts[1] switch
                {
                    "purelib" or "platlib" => root,
                    "scripts" => Path.Combine(runtimeRoot, "Scripts") + Path.DirectorySeparatorChar,
                    "headers" => Path.Combine(runtimeRoot, "Include") + Path.DirectorySeparatorChar,
                    "data" => runtimeRoot + Path.DirectorySeparatorChar,
                    _ => throw new InvalidDataException("Unsupported wheel installation layout: " + name)
                };
                name = string.Join('/', parts.Skip(2));
            }
            string target = Path.GetFullPath(Path.Combine(entryRoot, name));
            if (!target.StartsWith(entryRoot, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("Archive escapes destination");
            expanded += entry.Length;
            if (expanded > MaxExtracted) throw new InvalidDataException("Expanded archive exceeds size limit");
            if (name.EndsWith('/')) { Directory.CreateDirectory(target); continue; }
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            await using var source = entry.Open();
            await using var output = new FileStream(target, FileMode.CreateNew, FileAccess.Write, FileShare.None, 131072, true);
            await source.CopyToAsync(output, token);
        }
    }

    public static Task<RuntimeProbe> ProbeAsync(string python, CancellationToken token = default) => Task.Run(async () =>
    {
        if (!File.Exists(python)) return new RuntimeProbe(false, "Python executable not found");
        if (OperatingSystem.IsWindows() && !IsWindowsX64Executable(python))
            return new RuntimeProbe(false, "The selected file is not a Windows x64 executable");
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token);
        timeout.CancelAfter(TimeSpan.FromSeconds(90));
        const string code = "import sys,struct,json; library=sys.argv[1]; assert (sys.version_info[:2]==(3,13) if library else sys.version_info >= (3,12)) and struct.calcsize('P') == 8, 'This package requires Python 3.13 x64'; sys.path.insert(0,library) if library else None; import torch,numpy,pyarrow,safetensors,sklearn,packaging,rarfile; from sklearn.tree import DecisionTreeClassifier; DecisionTreeClassifier().fit([[0],[1]],[0,1]); a=torch.tensor([2.0],requires_grad=True); (a*a).sum().backward(); assert a.grad.item()==4; print(json.dumps(dict(python=sys.version.split()[0],torch=torch.__version__,cuda=torch.cuda.is_available())))";
        var info = new ProcessStartInfo(python) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
        if (Libraries != null) info.ArgumentList.Add("-S");
        foreach (string arg in new[] { "-I", "-X", "utf8", "-c", code }) info.ArgumentList.Add(arg);
        info.ArgumentList.Add(Libraries?.Directory ?? "");
        try
        {
            using var process = Process.Start(info) ?? throw new IOException("Could not start Python");
            var stdout = process.StandardOutput.ReadToEndAsync();
            var stderr = process.StandardError.ReadToEndAsync();
            try { await process.WaitForExitAsync(timeout.Token); }
            catch (OperationCanceledException)
            {
                if (!process.HasExited) process.Kill(entireProcessTree: true); // Only this short-lived probe, never a learning job.
                await process.WaitForExitAsync();
                await Task.WhenAll(stdout, stderr);
                token.ThrowIfCancellationRequested();
                return new RuntimeProbe(false, "Python runtime check timed out");
            }
            string output = await stdout, error = await stderr;
            if (process.ExitCode != 0) return new RuntimeProbe(false, (error.Length > 1800 ? error[^1800..] : error).Trim());
            using var parsed = JsonDocument.Parse(output);
            var r = parsed.RootElement;
            return new RuntimeProbe(true, "", r.GetProperty("python").GetString()!, r.GetProperty("torch").GetString()!, r.GetProperty("cuda").GetBoolean());
        }
        catch (Exception ex) when (ex is IOException or System.ComponentModel.Win32Exception or JsonException) { return new RuntimeProbe(false, ex.Message); }
    }, token);

    internal static bool IsWindowsX64Executable(string path)
    {
        try
        {
            using var file = File.OpenRead(path);
            using var reader = new BinaryReader(file);
            if (file.Length < 64 || reader.ReadUInt16() != 0x5a4d) return false;
            file.Position = 0x3c;
            int pe = reader.ReadInt32();
            if (pe < 64 || pe > file.Length - 26) return false;
            file.Position = pe;
            if (reader.ReadUInt32() != 0x4550 || reader.ReadUInt16() != 0x8664) return false;
            file.Position = pe + 24;
            return reader.ReadUInt16() == 0x20b;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { return false; }
    }
}
