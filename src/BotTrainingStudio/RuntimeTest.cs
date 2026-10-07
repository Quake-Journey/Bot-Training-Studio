using System.IO.Compression;
using System.Net;
using System.Net.Http;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace BotTrainingStudio;

internal static class RuntimeTest
{
    private sealed class ResponseHandler(byte[] data, HttpStatusCode status = HttpStatusCode.OK, string? redirect = null) : HttpMessageHandler
    {
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken) =>
            Task.FromResult(new HttpResponseMessage(status) { RequestMessage = redirect == null ? request : new HttpRequestMessage(HttpMethod.Get, redirect), Content = new ByteArrayContent(data) });
    }
    [DllImport("shell32.dll", CharSet = CharSet.Unicode)] private static extern uint ExtractIconEx(string file, int index, IntPtr[]? large, IntPtr[]? small, uint count);

    internal static async Task<int> Run(string[] args)
    {
        string folder = Path.GetFullPath(args[Array.IndexOf(args, "--runtime-test") + 1]);
        Directory.CreateDirectory(folder);
        var checks = new List<string>();
        void Check(bool condition, string name) { if (!condition) throw new Exception(name); checks.Add(name); }
        async Task Reject(Func<Task> action, string name)
        {
            bool rejected = false;
            try { await action(); } catch (Exception ex) when (ex is InvalidDataException or HttpRequestException or OperationCanceledException) { rejected = true; }
            Check(rejected, name);
        }
        string Zip(params (string name, string content, int attrs)[] entries)
        {
            string path = Path.Combine(folder, Guid.NewGuid().ToString("N") + ".zip");
            using var archive = ZipFile.Open(path, ZipArchiveMode.Create);
            foreach (var (name, content, attrs) in entries) { var entry = archive.CreateEntry(name); entry.ExternalAttributes = attrs; using var writer = new StreamWriter(entry.Open()); writer.Write(content); }
            return path;
        }
        try
        {
            foreach (string backend in RuntimeSetup.Backends)
            {
                var archives = RuntimeSetup.Archives(RuntimeSetup.Manifest(backend));
                Check(archives.Length >= 15 && archives.Any(a => a.Name == "torch"), "Embedded pinned manifest: " + backend);
            }
            Check(ExtractIconEx(Environment.ProcessPath!, -1, null, null, 0) > 0, "Explorer can extract a real icon from the EXE");
            using (var icon = RuntimeSetup.Resource("studio.ico"))
            { var header = new byte[6]; icon.ReadExactly(header); Check(header[2] == 1 && BitConverter.ToUInt16(header, 4) >= 8, "Window icon contains small and large resolutions"); }
            Check(!(await RuntimeSetup.ProbeAsync(Path.Combine(folder, "no-python.exe"))).Ready, "Missing Python is reported");
            string empty = Path.Combine(folder, "not-python.exe"); await File.WriteAllTextAsync(empty, "not an executable");
            Check(!(await RuntimeSetup.ProbeAsync(empty)).Ready, "Broken Python is not accepted by existence alone");
            byte[] body = Encoding.UTF8.GetBytes("verified download");
            string hash = Convert.ToHexString(SHA256.HashData(body));
            var good = new RuntimeArchive("fixture", "https://www.python.org/runtime-fixture.zip", hash);
            string output = Path.Combine(folder, "download-good.zip");
            using (var http = new HttpClient(new ResponseHandler(body))) await RuntimeSetup.DownloadAsync(http, good, output, _ => { }, default);
            Check(File.ReadAllBytes(output).SequenceEqual(body), "Verified archive downloaded intact");
            using (var http = new HttpClient(new ResponseHandler([1, 2, 3])))
                await Reject(() => RuntimeSetup.DownloadAsync(http, good, Path.Combine(folder, "bad-hash.zip"), _ => { }, default), "Corrupt download rejected by SHA256");
            using (var http = new HttpClient(new ResponseHandler(body, HttpStatusCode.ServiceUnavailable)))
                await Reject(() => RuntimeSetup.DownloadAsync(http, good, Path.Combine(folder, "offline.zip"), _ => { }, default), "HTTP failure does not look like success");
            using (var http = new HttpClient(new ResponseHandler(body, redirect: "https://example.invalid/runtime.zip")))
                await Reject(() => RuntimeSetup.DownloadAsync(http, good, Path.Combine(folder, "redirect.zip"), _ => { }, default), "Unexpected redirect host rejected");
            using (var http = new HttpClient(new ResponseHandler(body)))
                await Reject(() => RuntimeSetup.DownloadAsync(http, good with { Url = "http://www.python.org/runtime.zip" }, output, _ => { }, default), "Non-HTTPS source rejected");
            string extract = Path.Combine(folder, "good-wheel", "Lib", "site-packages");
            await RuntimeSetup.ExtractAsync(Zip(("pkg/a.py", "ok", 0), ("pkg.data/", "", 0), ("pkg.data/purelib/b.py", "pure", 0), ("pkg.data/platlib/c.pyd", "native", 0), ("pkg.data/data/share/test.txt", "data", 0)), extract, true, default);
            Check(File.ReadAllText(Path.Combine(extract, "b.py")) == "pure" && File.Exists(Path.Combine(extract, "c.pyd")), "Wheel purelib and platlib correctly relocated");
            Check(File.Exists(Path.Combine(folder, "good-wheel", "share", "test.txt")), "Wheel data installed under private runtime");
            foreach (string unsafeName in new[] { "../escape.py", "/absolute.py", "C:/escape.py", "a\\b.py", "directory./file.py", "pkg.data/unknown/file.py" })
                await Reject(() => RuntimeSetup.ExtractAsync(Zip((unsafeName, "bad", 0)), Path.Combine(folder, Guid.NewGuid().ToString("N")), true, default), "Unsafe wheel rejected: " + unsafeName);
            await Reject(() => RuntimeSetup.ExtractAsync(Zip(("link", "target", unchecked((int)0xa1ff0000))), Path.Combine(folder, "links"), false, default), "Archive symlink rejected");
            using var cancelled = new CancellationTokenSource(); cancelled.Cancel();
            await Reject(() => new RuntimeSetup().InstallAsync(folder, "cpu", _ => { }, cancelled.Token), "Cancelled setup does not install");
            Check(RuntimeSetup.FindManaged(folder, "cpu") == null, "Cancelled setup has no active runtime");
            string runtimes = Path.Combine(folder, "runtimes"); Directory.CreateDirectory(runtimes);
            await File.WriteAllTextAsync(Path.Combine(runtimes, "active-cpu.txt"), "../external");
            Check(RuntimeSetup.FindManaged(folder, "cpu") == null, "Managed runtime pointer cannot escape its root");
            await Reject(() => { RuntimeSetup.DeleteStage(runtimes, folder); return Task.CompletedTask; }, "Cleanup refuses unrelated directory");
            await File.WriteAllTextAsync(Path.Combine(folder, "runtime-test.json"), JsonSerializer.Serialize(new { pass = true, checks }, new JsonSerializerOptions { WriteIndented = true }));
            return 0;
        }
        catch (Exception ex)
        {
            await File.WriteAllTextAsync(Path.Combine(folder, "runtime-test.json"), JsonSerializer.Serialize(new { pass = false, checks, error = ex.ToString() }, new JsonSerializerOptions { WriteIndented = true }));
            return 1;
        }
    }
}
