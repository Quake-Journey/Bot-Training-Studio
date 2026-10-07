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
    private readonly object _gate = new();
    private RunState? _current;
    private sealed class RunState
    {
        public readonly TaskCompletionSource<string?> Ready = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public Task<bool>? Cancellation;
        public bool Finished;
    }
    public bool IsRunning => Volatile.Read(ref _busy) != 0;
    public string? JobFolder => _folder;
    public event Action<JsonElement>? Received;
    public event Action<string>? Diagnostic;

    public void Cancel() => _ = CancelAsync();

    public Task<bool> CancelAsync()
    {
        lock (_gate)
        {
            if (_current is not { } run) return Task.FromResult(false);
            // Bound to this run even if another job starts while the request is waiting for disk I/O.
            return run.Cancellation ??= Task.Run(async () =>
            {
                try
                {
                    string? folder = await run.Ready.Task.ConfigureAwait(false);
                    lock (_gate) { if (run.Finished || folder == null) return false; }
                    await File.WriteAllTextAsync(Path.Combine(folder, "cancel.request"), "cancel").ConfigureAwait(false);
                    return true;
                }
                catch (Exception ex)
                {
                    Diagnostic?.Invoke("Cannot request stop: " + ex.Message);
                    return false;
                }
            });
        }
    }

    public Task<JsonElement> RunAsync(StudioSettings settings, Dictionary<string, object?> request)
    {
        RunState run;
        lock (_gate)
        {
            if (_current != null) throw new InvalidOperationException("A job is already running");
            run = _current = new RunState();
            _folder = null;
            Volatile.Write(ref _busy, 1);
        }
        // Capture paths now: browsing settings during training cannot redirect the running job.
        string python = settings.Python, worker = settings.WorkerDirectory;
        var snapshot = new Dictionary<string, object?>(request);
        return Task.Run(() => RunCoreAsync(python, worker, snapshot, run));
    }

    private async Task<JsonElement> RunCoreAsync(string python, string worker, Dictionary<string, object?> request, RunState run)
    {
        try
        {
            if (!File.Exists(python)) throw new FileNotFoundException("Choose the Python runtime in Settings", python);
            if (!File.Exists(Path.Combine(worker, "opentdm_x_trainer", "studio.py")))
                throw new DirectoryNotFoundException("Choose the installed worker folder in Settings");
            string id = Guid.NewGuid().ToString("N");
            _folder = Path.Combine(StudioSettings.Home, "jobs", id);
            Directory.CreateDirectory(_folder);
            run.Ready.TrySetResult(_folder);
            request["protocol"] = 1;
            request["job_id"] = id;
            var path = Path.Combine(_folder, "request.json");
            await File.WriteAllTextAsync(path, JsonSerializer.Serialize(request));
            Task<bool>? cancellation;
            lock (_gate) cancellation = run.Cancellation;
            if (cancellation != null) await cancellation;
            var info = new ProcessStartInfo(python)
            {
                UseShellExecute = false, CreateNoWindow = true,
                RedirectStandardOutput = true, RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8,
                WorkingDirectory = worker
            };
            info.ArgumentList.Add("-I");
            if (RuntimeSetup.Libraries != null) info.ArgumentList.Add("-S");
            info.ArgumentList.Add("-X");
            info.ArgumentList.Add("utf8");
            info.ArgumentList.Add("-u");
            info.ArgumentList.Add("-c");
            info.ArgumentList.Add("import runpy,sys; worker=sys.argv.pop(1); library=sys.argv.pop(1); sys.path.insert(0,library) if library else None; sys.path.insert(0,worker); runpy.run_module('opentdm_x_trainer.studio',run_name='__main__')");
            info.ArgumentList.Add(Path.GetFullPath(worker));
            info.ArgumentList.Add(RuntimeSetup.Libraries?.Directory ?? "");
            info.ArgumentList.Add("--request");
            info.ArgumentList.Add(path);
            using var stderr = new StreamWriter(Path.Combine(_folder, "stderr.txt"));
            using var process = Process.Start(info) ?? throw new InvalidOperationException("Cannot start worker");
            Volatile.Write(ref _workerPid, process.Id);
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
        finally
        {
            Task<bool>? cancellation;
            lock (_gate)
            {
                run.Finished = true;
                run.Ready.TrySetResult(null);
                cancellation = run.Cancellation;
            }
            if (cancellation != null) await cancellation;
            lock (_gate) { _current = null; Volatile.Write(ref _workerPid, 0); Volatile.Write(ref _busy, 0); }
        }
    }
}
