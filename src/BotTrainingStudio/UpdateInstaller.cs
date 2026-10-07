using System.Diagnostics;
using System.Text.Json;

namespace BotTrainingStudio;

internal static class UpdateInstaller
{
    internal static void Install(UpdateTicket ticket, Action<int>? afterWrite = null)
    {
        string target = Path.GetFullPath(ticket.Target), stage = Path.GetFullPath(ticket.Stage);
        Updates.NoLinks(target); Updates.NoLinks(stage);
        if (!Updates.EqualHash(Updates.Hash(Path.Combine(target, "BotTrainingStudio.exe")), ticket.ExpectedExeSha256)) throw new IOException("Installed application changed while downloading");
        if (!Updates.EqualHash(Updates.Hash(Path.Combine(stage, "package-files.json")), ticket.InventorySha256)) throw new InvalidDataException("Staged inventory changed");
        var inventory = Updates.ReadInventory(stage);
        if (!Updates.VerifyFiles(stage, inventory.AppFiles, default) || !Updates.VerifyFiles(ticket.ReuseLibraries ? target : stage, inventory.LibraryFiles, default)) throw new InvalidDataException("Staged files changed");
        string lockPath = Path.Combine(target, ".update.lock"); Updates.NoLinks(lockPath);
        using var installLock = new FileStream(lockPath, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
        string transaction = Path.Combine(target, ".update-rollback-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(transaction);
        var copied = new List<(string Relative, bool Existed)>();
        var files = inventory.AppFiles.Keys.Concat(ticket.ReuseLibraries ? [] : inventory.LibraryFiles.Keys).Append("package-files.json")
            .OrderBy(p => p == "BotTrainingStudio.exe" ? 1 : 0).ToArray();
        try
        {
            foreach (string relative in files)
            {
                string src = Updates.SafePath(stage, relative), dst = Updates.SafePath(target, relative), backup = Updates.SafePath(transaction, relative);
                Updates.NoLinks(dst); Updates.NoLinks(src);
                bool existed = File.Exists(dst);
                // Unknown files in the installation directory are never enumerated or removed.
                Directory.CreateDirectory(Path.GetDirectoryName(dst)!);
                if (existed)
                { Directory.CreateDirectory(Path.GetDirectoryName(backup)!); File.Move(dst, backup); }
                copied.Add((relative, existed));
                File.WriteAllText(Path.Combine(transaction, "transaction.json"), JsonSerializer.Serialize(copied.Select(r => new { path = r.Relative, existed = r.Existed }), Updates.Json));
                File.Copy(src, dst, false);
                afterWrite?.Invoke(copied.Count);
            }
            // Only remove tracked files left by a previous Studio package, never arbitrary user data.
            // Obsolete tracked files are retained in this preview; all Python modules ship in the new package.
        }
        catch (Exception original)
        {
            var errors = new List<Exception>();
            foreach (var (relative, existed) in copied.AsEnumerable().Reverse())
            {
                try
                {
                    string dst = Updates.SafePath(target, relative), backup = Updates.SafePath(transaction, relative);
                    Updates.NoLinks(dst); Updates.NoLinks(backup);
                    if (File.Exists(dst)) File.Delete(dst);
                    if (existed) File.Move(backup, dst);
                }
                catch (Exception error) { errors.Add(error); }
            }
            if (errors.Count > 0) throw new AggregateException("Update failed; recovery files are kept in " + transaction, errors.Prepend(original));
            Directory.Delete(transaction, true);
            throw;
        }
        Directory.Delete(transaction, true);
    }

    internal static async Task<int> ApplyAsync(string ticketPath)
    {
        string job = Path.GetDirectoryName(Path.GetFullPath(ticketPath))!;
        UpdateTicket? ticket = null; string version = AppVersion.Current; bool success = false, parentExited = false; string? failure = null;
        try
        {
            Updates.NoLinks(ticketPath);
            if (Path.GetFileName(ticketPath) != "ticket.json" || !File.Exists(Path.Combine(job, "owned.json"))) throw new InvalidDataException("Invalid update ticket");
            ticket = JsonSerializer.Deserialize<UpdateTicket>(File.ReadAllText(ticketPath), Updates.Json)!;
            if (Path.GetFullPath(ticket.Stage) != Path.Combine(job, "new") || !string.Equals(Environment.ProcessPath, Path.Combine(ticket.Stage, "BotTrainingStudio.exe"), StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("Updater must run from the verified staged application");
            version = Updates.ReadInventory(ticket.Stage).Version;
            if (version != AppVersion.Current) throw new InvalidDataException("Staged application version mismatch");
            try
            {
                using var parent = Process.GetProcessById(ticket.ParentPid);
                if (parent.StartTime.ToUniversalTime().Ticks != ticket.ParentStartTicks) throw new IOException("Original process identity changed");
                using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(90));
                await parent.WaitForExitAsync(timeout.Token);
            }
            catch (ArgumentException) { } // original process has already exited
            parentExited = true;
            Install(ticket); success = true;
        }
        catch (Exception error) { failure = error.ToString(); }
        var result = new UpdateResult(success, version, failure, Environment.ProcessId);
        File.WriteAllText(Path.Combine(job, "result.json"), JsonSerializer.Serialize(result, Updates.Json));
        if (ticket != null && parentExited && Environment.GetEnvironmentVariable("BTS_UPDATE_NO_RESTART") != "1")
        {
            string exe = Path.Combine(ticket.Target, "BotTrainingStudio.exe");
            if (File.Exists(exe))
                Process.Start(new ProcessStartInfo(exe) { UseShellExecute = false, WorkingDirectory = ticket.Target });
        }
        return success ? 0 : 1;
    }

    internal static async Task<string?> CleanupFinishedAsync(string target, string home)
    {
        string updates = Path.Combine(target, ".updates"); if (!Directory.Exists(updates)) return null;
        string? failure = null;
        foreach (string job in Directory.GetDirectories(updates))
        {
            string report = Path.Combine(job, "result.json"); if (!File.Exists(report)) continue;
            try
            {
                Updates.NoLinks(job);
                var result = JsonSerializer.Deserialize<UpdateResult>(await File.ReadAllTextAsync(report), Updates.Json)!;
                if (!result.Success) { failure = result.Error; continue; } // retain recovery evidence
                try { using var helper = Process.GetProcessById(result.HelperPid); if (!helper.HasExited) continue; } catch (ArgumentException) { }
                Directory.CreateDirectory(home);
                File.Copy(report, Path.Combine(home, "last-update.json"), true);
                Updates.DeleteOwnedJob(job);
            }
            catch (IOException) { } // Antivirus/another process may release a file later.
            catch (UnauthorizedAccessException) { }
        }
        return failure;
    }
}
