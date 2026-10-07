using System.Diagnostics;
using System.Runtime.InteropServices;

namespace BotTrainingStudio;

internal sealed record ResourceSnapshot(DateTimeOffset At, double? Cpu, long? RamUsed, long? RamTotal,
    bool JobRunning, double? JobCpu, long? JobRam, IReadOnlyList<GpuLoad> Gpus)
{
    public static ResourceSnapshot Empty => new(DateTimeOffset.UtcNow, null, null, null, false, null, null, []);
}

/// <summary>One background sampler per window. No Torch import, command shell, subprocess polling or UI-thread I/O.</summary>
internal sealed class ResourceSampler : IDisposable
{
    private readonly CancellationTokenSource _stop = new();
    private readonly Task _loop;
    private ResourceSnapshot _latest = ResourceSnapshot.Empty;
    public ResourceSnapshot Latest => Volatile.Read(ref _latest);
    public Task Completion => _loop;

    public ResourceSampler(Func<int> workerPid)
    {
        _loop = Task.Run(async () =>
        {
            try
            {
                using var gpu = new GpuCounters();
                var machine = new MachineCounters();
                var worker = new WorkerCounters();
                while (!_stop.IsCancellationRequested)
                {
                    var (cpu, used, total) = machine.Sample();
                    int pid = workerPid();
                    var (jobCpu, jobRam) = worker.Sample(pid);
                    var snapshot = new ResourceSnapshot(DateTimeOffset.UtcNow, cpu, used, total,
                        pid > 0, jobCpu, jobRam, gpu.Sample());
                    Volatile.Write(ref _latest, snapshot);
                    await Task.Delay(1000, _stop.Token).ConfigureAwait(false);
                }
            }
            catch (OperationCanceledException) { }
            catch (Exception) { Volatile.Write(ref _latest, ResourceSnapshot.Empty); }
        });
    }
    public void Dispose() { _stop.Cancel(); } // Never wait on a driver/counter from the UI thread.
}

internal sealed class MachineCounters
{
    private (ulong idle, ulong kernel, ulong user)? _previous;
    public (double? cpu, long? used, long? total) Sample()
    {
        if (!OperatingSystem.IsWindows()) return (null, null, null);
        double? cpu = null;
        if (GetSystemTimes(out var idle, out var kernel, out var user))
        {
            if (_previous is { } old && kernel >= old.kernel && user >= old.user && idle >= old.idle)
            {
                ulong all = kernel - old.kernel + user - old.user, free = idle - old.idle;
                if (all > 0 && free <= all) cpu = 100.0 * (all - free) / all;
            }
            _previous = (idle, kernel, user);
        }
        else _previous = null;
        var memory = new MemoryStatus { Length = (uint)Marshal.SizeOf<MemoryStatus>() };
        return GlobalMemoryStatusEx(ref memory) && memory.Total > 0
            ? (cpu, (long)(memory.Total - memory.Available), (long)memory.Total) : (cpu, null, null);
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct MemoryStatus
    {
        public uint Length, Load;
        public ulong Total, Available, TotalPage, AvailablePage, TotalVirtual, AvailableVirtual, Extended;
    }
    [DllImport("kernel32.dll")] private static extern bool GetSystemTimes(out ulong idle, out ulong kernel, out ulong user);
    [DllImport("kernel32.dll")] private static extern bool GlobalMemoryStatusEx(ref MemoryStatus memory);
}

/// <summary>Sample the Python worker and live descendants, including the offline decoder. UI process is excluded.</summary>
internal sealed class WorkerCounters
{
    private Dictionary<(int pid, long start), long> _previous = [];
    private int _root;
    private long _at;
    public (double? cpu, long? ram) Sample(int root)
    {
        long now = Stopwatch.GetTimestamp();
        if (root <= 0) { _previous.Clear(); _root = 0; _at = now; return (null, null); }
        if (_root != root) { _previous.Clear(); _at = now; _root = root; }
        var current = new Dictionary<(int, long), long>();
        long memory = 0, delta = 0;
        bool comparable = false;
        foreach (int pid in ProcessTree(root))
        {
            try
            {
                using var process = Process.GetProcessById(pid);
                if (process.HasExited) continue;
                var key = (pid, process.StartTime.ToUniversalTime().Ticks);
                long ticks = process.TotalProcessorTime.Ticks;
                current[key] = ticks;
                memory += process.WorkingSet64;
                if (_previous.TryGetValue(key, out var old) && ticks >= old) { delta += ticks - old; comparable = true; }
            }
            catch (Exception ex) when (ex is ArgumentException or InvalidOperationException or System.ComponentModel.Win32Exception) { }
        }
        double seconds = (now - _at) / (double)Stopwatch.Frequency;
        double? cpu = comparable && seconds > 0
            ? Math.Clamp(delta / (double)TimeSpan.TicksPerSecond / seconds / Environment.ProcessorCount * 100, 0, 100) : null;
        _previous = current; _at = now;
        return (cpu, current.Count > 0 ? memory : null);
    }

    private static IEnumerable<int> ProcessTree(int root)
    {
        var ids = new HashSet<int> { root };
        if (!OperatingSystem.IsWindows()) return ids;
        var snapshot = CreateToolhelp32Snapshot(2, 0);
        if (snapshot == new IntPtr(-1)) return ids;
        try
        {
            var parents = new List<(int pid, int parent)>();
            var entry = new ProcessEntry { Size = (uint)Marshal.SizeOf<ProcessEntry>() };
            if (Process32FirstW(snapshot, ref entry))
                do { parents.Add(((int)entry.ProcessId, (int)entry.ParentId)); } while (Process32NextW(snapshot, ref entry));
            bool changed;
            do
            {
                changed = false;
                foreach (var (pid, parent) in parents) if (ids.Contains(parent)) changed |= ids.Add(pid);
            } while (changed);
        }
        finally { CloseHandle(snapshot); }
        return ids;
    }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct ProcessEntry
    {
        public uint Size, Usage, ProcessId;
        public nuint DefaultHeap;
        public uint ModuleId, Threads, ParentId;
        public int Priority;
        public uint Flags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string Exe;
    }
    [DllImport("kernel32.dll")] private static extern IntPtr CreateToolhelp32Snapshot(uint flags, uint id);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] private static extern bool Process32FirstW(IntPtr snapshot, ref ProcessEntry entry);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] private static extern bool Process32NextW(IntPtr snapshot, ref ProcessEntry entry);
    [DllImport("kernel32.dll")] private static extern bool CloseHandle(IntPtr handle);
}
