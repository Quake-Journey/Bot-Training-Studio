using System.IO.Compression;
using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace BotTrainingStudio;

internal static class UpdateTest
{
    private sealed class Handler(Func<HttpRequestMessage, HttpResponseMessage> respond) : HttpMessageHandler
    {
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken token)
        { token.ThrowIfCancellationRequested(); return Task.FromResult(respond(request)); }
    }
    internal static async Task<int> Run(string folder)
    {
        Directory.CreateDirectory(folder); var checks = new List<string>();
        void Check(bool condition, string label) { if (!condition) throw new Exception(label); checks.Add(label); }
        async Task Reject(Func<Task> task, string label)
        { bool rejected = false; try { await task(); } catch (Exception error) when (error is IOException or InvalidDataException or HttpRequestException or OperationCanceledException or InvalidOperationException) { rejected = true; } Check(rejected, label); }
        string hash(byte[] bytes) => Convert.ToHexString(SHA256.HashData(bytes));
        byte[] Zip(Dictionary<string, byte[]> files)
        { using var stream = new MemoryStream(); using (var zip = new ZipArchive(stream, ZipArchiveMode.Create, true)) foreach (var (name, body) in files) { using var output = zip.CreateEntry(name).Open(); output.Write(body); } return stream.ToArray(); }
        string NewFolder() { string path = Path.Combine(folder, Guid.NewGuid().ToString("N")); Directory.CreateDirectory(path); return path; }
        string Files(string root, Dictionary<string, byte[]> contents)
        { foreach (var (name, content) in contents) { string path = Updates.SafePath(root, name); Directory.CreateDirectory(Path.GetDirectoryName(path)!); File.WriteAllBytes(path, content); } return root; }
        try
        {
            Check(AppVersion.History[0].Version == AppVersion.Current, "Assembly and embedded change notes use the same version");
            Check(StudioVersion.Parse("0.3.0-preview.10").CompareTo(StudioVersion.Parse("0.3.0-preview.2")) > 0, "Preview numbers compare numerically");
            Check(StudioVersion.Parse("0.3.0").CompareTo(StudioVersion.Parse("0.3.0-preview.10")) > 0, "Stable version follows its previews");
            Check(!StudioVersion.TryParse("../0.3.0", out _), "Untrusted version is not a path");
            string version = "0.3.1-preview.1", prefix = "https://github.com/" + Updates.Repository + "/releases/download/v" + version + "/";
            var appFiles = new Dictionary<string, byte[]> { ["BotTrainingStudio.exe"] = Encoding.UTF8.GetBytes("new-exe"), ["build.json"] = "{}"u8.ToArray(),
                ["worker/opentdm_x_trainer/studio.py"] = "new worker"u8.ToArray(), ["runtime/python-embed.zip"] = "bundled Python"u8.ToArray(), ["docs/Bot_Training_Studio_User_Guide_RU.docx"] = "ru"u8.ToArray(), ["docs/Bot_Training_Studio_User_Guide_EN.docx"] = "en"u8.ToArray() };
            var libs = new Dictionary<string, byte[]> { ["libraries/studio-libraries.json"] = "model-libraries"u8.ToArray(), ["libraries/test.py"] = "library"u8.ToArray() };
            var inventory = new PackageInventory(version, hash(libs["libraries/studio-libraries.json"]), appFiles.ToDictionary(p => p.Key, p => hash(p.Value)), libs.ToDictionary(p => p.Key, p => hash(p.Value)));
            byte[] inventoryBytes = JsonSerializer.SerializeToUtf8Bytes(inventory, Updates.Json); appFiles["package-files.json"] = inventoryBytes;
            byte[] appZip = Zip(appFiles), libsZip = Zip(libs);
            var manifest = new UpdateManifest(1, version, inventory.LibrarySha256, [new("app.zip", appZip.Length, hash(appZip))],
                [new("libs.zip.001", libsZip.Length / 2, hash(libsZip[..(libsZip.Length / 2)])), new("libs.zip.002", libsZip.Length - libsZip.Length / 2, hash(libsZip[(libsZip.Length / 2)..]))]);
            var payloads = new Dictionary<string, byte[]> { [Updates.ManifestName] = JsonSerializer.SerializeToUtf8Bytes(manifest, Updates.Json), ["app.zip"] = appZip,
                ["libs.zip.001"] = libsZip[..(libsZip.Length / 2)], ["libs.zip.002"] = libsZip[(libsZip.Length / 2)..] };
            var release = new AvailableUpdate(version, prefix, payloads.Select(p => new UpdateAsset(p.Key, prefix + p.Key, p.Value.Length)).ToArray());
            var downloaded = new List<string>();
            using var http = new HttpClient(new Handler(request =>
            {
                string key = request.RequestUri!.Segments.Last(); downloaded.Add(key);
                return new(HttpStatusCode.OK) { Content = new ByteArrayContent(payloads[key]), RequestMessage = request };
            }));
            using var updates = new Updates(http);
            string current = Files(NewFolder(), libs); File.WriteAllText(Path.Combine(current, "BotTrainingStudio.exe"), "old-exe");
            File.WriteAllText(Path.Combine(current, "settings.json"), "user settings");
            Files(current, new() { ["models/mine.bin"] = "user model"u8.ToArray(), ["projects/custom.txt"] = "user project"u8.ToArray(), ["worker/user-note.txt"] = "user note"u8.ToArray() });
            string ticketPath = await updates.PrepareAsync(release, current, _ => { }, default);
            var ticket = JsonSerializer.Deserialize<UpdateTicket>(File.ReadAllText(ticketPath), Updates.Json)!;
            Check(ticket.ReuseLibraries && !downloaded.Any(n => n.StartsWith("libs")), "Unchanged verified libraries are reused without downloads");
            Check(File.ReadAllText(Path.Combine(current, "BotTrainingStudio.exe")) == "old-exe", "Downloading never replaces installed application");
            await Reject(() => Task.Run(() => UpdateInstaller.Install(ticket, count => { if (count == 2) throw new IOException("Injected replacement error"); })), "Installation failure is surfaced");
            Check(File.ReadAllText(Path.Combine(current, "BotTrainingStudio.exe")) == "old-exe" && !File.Exists(Path.Combine(current, "build.json")), "Failed install restores previous files and removes incomplete new ones");
            UpdateInstaller.Install(ticket);
            Check(File.ReadAllText(Path.Combine(current, "BotTrainingStudio.exe")) == "new-exe", "Verified new application is installed");
            Check(File.ReadAllText(Path.Combine(current, "settings.json")) == "user settings" && File.ReadAllText(Path.Combine(current, "models/mine.bin")) == "user model" && File.Exists(Path.Combine(current, "worker/user-note.txt")), "Settings, models, projects and unowned worker files are preserved");
            Check(Directory.GetDirectories(current, ".update-rollback-*").Length == 0, "Completed and rolled-back transactions leave no backup duplicates");
            string missing = NewFolder(); File.WriteAllText(Path.Combine(missing, "BotTrainingStudio.exe"), "old-exe"); downloaded.Clear();
            string freshTicket = await updates.PrepareAsync(release, missing, _ => { }, default);
            var fresh = JsonSerializer.Deserialize<UpdateTicket>(File.ReadAllText(freshTicket), Updates.Json)!;
            Check(!fresh.ReuseLibraries && downloaded.Contains("libs.zip.001") && downloaded.Contains("libs.zip.002"), "Missing libraries download and verify both archive parts");
            UpdateInstaller.Install(fresh);
            Check(Updates.VerifyFiles(missing, inventory.LibraryFiles, default), "Multipart library archive installs intact");
            string changed = Files(NewFolder(), libs); File.WriteAllText(Path.Combine(changed, "BotTrainingStudio.exe"), "old-exe");
            string changedTicket = await updates.PrepareAsync(release, changed, _ => { }, default);
            var changedInfo = JsonSerializer.Deserialize<UpdateTicket>(File.ReadAllText(changedTicket), Updates.Json)!;
            File.WriteAllText(Path.Combine(changed, "BotTrainingStudio.exe"), "independently changed");
            await Reject(() => Task.Run(() => UpdateInstaller.Install(changedInfo)), "Concurrent executable replacement aborts the update");
            var corrupt = release with { Assets = release.Assets.ToArray() };
            byte[] original = payloads["app.zip"]; payloads["app.zip"] = new byte[original.Length];
            string bad = NewFolder(); File.WriteAllText(Path.Combine(bad, "BotTrainingStudio.exe"), "old-exe");
            await Reject(() => updates.PrepareAsync(corrupt, bad, _ => { }, default), "Corrupt download is rejected before installation");
            Check(!Directory.EnumerateDirectories(Path.Combine(bad, ".updates")).Any(), "Failed download staging is cleaned"); payloads["app.zip"] = original;
            using var cancelled = new CancellationTokenSource(); cancelled.Cancel();
            await Reject(() => updates.PrepareAsync(release, bad, _ => { }, cancelled.Token), "Cancellation does not change the installed application");
            foreach (string unsafePath in new[] { "../escape.exe", "/escape.exe", "C:/escape.exe", "worker/../settings.json", "worker/CON.txt", "worker/a:stream", "worker/a./b", "worker\\bad.py" })
                await Reject(() => Task.Run(() => Updates.SafePath(folder, unsafePath)), "Unsafe path rejected: " + unsafePath);
            foreach (string name in new[] { "models/stolen.bin", "settings.json", "../escape.exe" })
            {
                string zip = Path.Combine(NewFolder(), "bad.zip"); File.WriteAllBytes(zip, Zip(new() { [name] = [1] }));
                await Reject(() => Task.Run(() => Updates.Extract(zip, NewFolder(), false, default)), "Archive cannot overwrite user path: " + name);
            }
            using var denied = new HttpClient(new Handler(request => new(HttpStatusCode.ServiceUnavailable) { RequestMessage = request }));
            using var offline = new Updates(denied);
            await Reject(() => offline.CheckAsync(default), "Offline/release-server error is not reported as up-to-date");
            var apiPayload = JsonSerializer.SerializeToUtf8Bytes(new[] {
                new { tag_name="v99.0.0", draft=true, prerelease=false, assets=Array.Empty<object>() },
                new { tag_name="v"+version, draft=false, prerelease=true, assets=release.Assets.Select(a => (object)new { name=a.Name,browser_download_url=a.Url,size=a.Size }).ToArray() }
            });
            using var apiClient = new HttpClient(new Handler(request => new(HttpStatusCode.OK) { RequestMessage=request, Content=new ByteArrayContent(apiPayload) }));
            using var api = new Updates(apiClient); var found = await api.CheckAsync(default);
            Check(found?.Version == version, "Release discovery skips drafts and supports the preview channel");
            using var redirectHttp = new HttpClient(new Handler(request => { var r = new HttpResponseMessage(HttpStatusCode.Redirect); r.Headers.Location = new Uri("http://example.invalid/update"); return r; }));
            using var redirected = new Updates(redirectHttp);
            await Reject(() => redirected.CheckAsync(default), "Unexpected redirect host and protocol are rejected");
            File.WriteAllText(Path.Combine(folder,"update-test.json"),JsonSerializer.Serialize(new { pass=true,checks },Updates.Json));
            Console.WriteLine(JsonSerializer.Serialize(new { pass=true,checks=checks.Count })); return 0;
        }
        catch (Exception error) { File.WriteAllText(Path.Combine(folder,"update-test.json"),JsonSerializer.Serialize(new { pass=false,checks,error=error.ToString() },Updates.Json)); Console.Error.WriteLine(error); return 1; }
    }
}
