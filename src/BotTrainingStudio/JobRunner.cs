using System.Diagnostics;
using System.Text;
using System.Text.Json;

namespace BotTrainingStudio;

public sealed class JobRunner
{
    private int _busy;
    private int _workerPid;
    public int WorkerPid => Volatile.Read(ref _workerPid);
    private string? _folder;
    public bool IsRunning => Volatile.Read(ref _busy) != 0;
    public string? JobFolder => _folder;
    public event Action<JsonElement>? Received;
    public event Action<string>? Diagnostic;

    public void Cancel()
    {
        if (IsRunning && _folder != null) File.WriteAllText(Path.Combine(_folder, "cancel.request"), "cancel");
    }

    public async Task<JsonElement> RunAsync(StudioSettings settings, Dictionary<string, object?> request)
    {
        if (Interlocked.CompareExchange(ref _busy, 1, 0) != 0) throw new InvalidOperationException("A job is already running");
        try
        {
            if (!File.Exists(settings.Python)) throw new FileNotFoundException("Choose the Python runtime in Settings", settings.Python);
            if (!File.Exists(Path.Combine(settings.WorkerDirectory, "opentdm_x_trainer", "studio.py")))
                throw new DirectoryNotFoundException("Choose the installed worker folder in Settings");
            string id = Guid.NewGuid().ToString("N");
            _folder = Path.Combine(StudioSettings.Home, "jobs", id);
            Directory.CreateDirectory(_folder);
            request["protocol"] = 1;
            request["job_id"] = id;
            var path = Path.Combine(_folder, "request.json");
            await File.WriteAllTextAsync(path, JsonSerializer.Serialize(request));
            var info = new ProcessStartInfo(settings.Python)
            {
                UseShellExecute = false, CreateNoWindow = true,
                RedirectStandardOutput = true, RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8,
                WorkingDirectory = settings.WorkerDirectory
            };
            info.ArgumentList.Add("-I");
            info.ArgumentList.Add("-X");
            info.ArgumentList.Add("utf8");
            info.ArgumentList.Add("-u");
            info.ArgumentList.Add("-c");
            info.ArgumentList.Add("import runpy,sys; sys.path.insert(0,sys.argv.pop(1)); runpy.run_module('opentdm_x_trainer.studio',run_name='__main__')");
            info.ArgumentList.Add(Path.GetFullPath(settings.WorkerDirectory));
            info.ArgumentList.Add("--request");
            info.ArgumentList.Add(path);
            using var process = Process.Start(info) ?? throw new InvalidOperationException("Cannot start worker");
            Volatile.Write(ref _workerPid, process.Id);
            using var stderr = new StreamWriter(Path.Combine(_folder, "stderr.txt"));
            var errorTask = Task.Run(async () =>
            {
                int count = 0;
                while (await process.StandardError.ReadLineAsync() is { } line)
                {
                    if (count++ < 500) { await stderr.WriteLineAsync(line); Diagnostic?.Invoke(line); }
                }
            });
            JsonElement terminal = default;
            int sequence = 0;
            try
            {
                while (await process.StandardOutput.ReadLineAsync() is { } line)
                {
                    if (line.Length > 1_000_000) throw new InvalidDataException("Oversized worker event");
                    using var doc = JsonDocument.Parse(line);
                    var message = doc.RootElement.Clone();
                    if (message.GetProperty("protocol").GetInt32() != 1 || message.GetProperty("job_id").GetString() != id
                        || message.GetProperty("seq").GetInt32() != sequence + 1)
                        throw new InvalidDataException("Worker protocol identity/sequence mismatch");
                    sequence++;
                    var type = message.GetProperty("type").GetString();
                    if (terminal.ValueKind != JsonValueKind.Undefined) throw new InvalidDataException("Event after terminal state");
                    if (type is "completed" or "failed" or "cancelled") terminal = message;
                    Received?.Invoke(message);
                }
                await process.WaitForExitAsync();
                await errorTask;
                if (terminal.ValueKind == JsonValueKind.Undefined) throw new InvalidDataException($"Worker exited without a result ({process.ExitCode})");
                string? terminalType = terminal.GetProperty("type").GetString();
                int expected = terminalType == "completed" ? 0 : terminalType == "cancelled" ? 2 : 1;
                if (process.ExitCode != expected) throw new InvalidDataException("Worker result/exit-code mismatch");
                return terminal;
            }
            catch
            {
                if (!process.HasExited) process.Kill(entireProcessTree: true);
                await process.WaitForExitAsync();
                await errorTask;
                throw;
            }
        }
        finally { Volatile.Write(ref _workerPid, 0); Volatile.Write(ref _busy, 0); }
    }
}
