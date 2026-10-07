using System.Runtime.InteropServices;
using System.Text.RegularExpressions;

namespace BotTrainingStudio;

internal sealed record GpuLoad(string Id, string Name, double? Percent, long? Used, long? Total, long? Shared);

/// <summary>WDDM counters, with DXGI identity/capacity matched by adapter LUID, not by vendor or largest value.</summary>
internal sealed class GpuCounters : IDisposable
{
    private IntPtr _query, _engines, _memory, _shared;
    private readonly List<GpuLoad> _adapters = [];
    private static readonly Regex AdapterId = new(@"luid_(0x[0-9a-f]+_0x[0-9a-f]+)_phys_\d+", RegexOptions.IgnoreCase);
    private static readonly Regex EngineId = new(@"_eng_(\d+)_", RegexOptions.IgnoreCase);

    public GpuCounters()
    {
        if (!OperatingSystem.IsWindows()) return;
        try { _adapters.AddRange(EnumerateAdapters()); } catch (Exception) { /* Counter-only fallback. */ }
        if (PdhOpenQueryW(null, IntPtr.Zero, out _query) != 0) return;
        Add(@"\GPU Engine(*)\Utilization Percentage", out _engines);
        Add(@"\GPU Adapter Memory(*)\Dedicated Usage", out _memory);
        Add(@"\GPU Adapter Memory(*)\Shared Usage", out _shared);
        PdhCollectQueryData(_query); // Utilization needs two samples.
    }

    private void Add(string path, out IntPtr counter)
    {
        if (PdhAddEnglishCounterW(_query, path, IntPtr.Zero, out counter) != 0) counter = IntPtr.Zero;
    }

    public IReadOnlyList<GpuLoad> Sample()
    {
        if (_query == IntPtr.Zero || PdhCollectQueryData(_query) != 0)
            return _adapters.Select(a => a with { Percent = null, Used = null, Shared = null }).ToArray();
        return Combine(_adapters, Read(_engines), Read(_memory), Read(_shared));
    }

    internal static string? Luid(string instance)
    {
        var match = AdapterId.Match(instance);
        return match.Success ? match.Groups[1].Value.ToLowerInvariant() : null;
    }

    internal static IReadOnlyList<GpuLoad> Combine(IEnumerable<GpuLoad> adapters,
        IEnumerable<(string name, double value)> engines, IEnumerable<(string name, double value)> memory,
        IEnumerable<(string name, double value)> shared)
    {
        var cards = adapters.ToDictionary(a => a.Id, a => a);
        var busy = new Dictionary<(string card, string engine), double>();
        var dedicated = new Dictionary<string, double>();
        var system = new Dictionary<string, double>();
        void Seen(string id)
        {
            if (!cards.ContainsKey(id)) cards[id] = new GpuLoad(id, "GPU " + id, null, null, null, null);
        }
        foreach (var (name, value) in engines)
        {
            string? id = Luid(name);
            var engine = EngineId.Match(name);
            if (id == null || !engine.Success || !double.IsFinite(value) || value < 0) continue;
            Seen(id);
            // Sum processes on one physical engine; independent engines can work concurrently.
            // Taking the busiest engine avoids adding 3D/compute/copy into an invented >100% load.
            string physical = name[(name.IndexOf("_phys_", StringComparison.OrdinalIgnoreCase) + 6)..];
            physical = physical[..physical.IndexOf('_')];
            var key = (id, physical + ":" + engine.Groups[1].Value);
            busy[key] = busy.GetValueOrDefault(key) + value;
        }
        void Memory(IEnumerable<(string name, double value)> rows, Dictionary<string, double> target)
        {
            foreach (var (name, value) in rows)
                if (Luid(name) is { } id && double.IsFinite(value) && value >= 0)
                { Seen(id); target[id] = target.GetValueOrDefault(id) + value; }
        }
        Memory(memory, dedicated); Memory(shared, system);
        return cards.Values.Select(a => a with
        {
            Percent = busy.Where(p => p.Key.card == a.Id).Select(p => (double?)Math.Clamp(p.Value, 0, 100)).Max(),
            Used = dedicated.TryGetValue(a.Id, out var used) ? (long)used : null,
            Shared = system.TryGetValue(a.Id, out var usedShared) ? (long)usedShared : null
        }).ToArray();
    }

    private static List<(string name, double value)> Read(IntPtr counter)
    {
        var rows = new List<(string, double)>();
        if (counter == IntPtr.Zero) return rows;
        uint size = 0;
        const uint MoreData = 0x800007D2, Double = 0x200;
        if (PdhGetFormattedCounterArrayW(counter, Double, ref size, out _, IntPtr.Zero) != MoreData) return rows;
        // Instances can appear between sizing and reading; retry boundedly.
        for (int attempt = 0; attempt < 3 && size > 0 && size < 32 * 1024 * 1024; attempt++)
        {
            var buffer = Marshal.AllocHGlobal((int)size);
            try
            {
                uint result = PdhGetFormattedCounterArrayW(counter, Double, ref size, out var count, buffer);
                if (result == MoreData) continue;
                if (result != 0) break;
                int stride = Marshal.SizeOf<CounterValue>();
                if ((long)count * stride > size) break;
                for (int i = 0; i < count; i++)
                {
                    var item = Marshal.PtrToStructure<CounterValue>(buffer + i * stride);
                    if (item.Status is 0 or 1) rows.Add((Marshal.PtrToStringUni(item.Name) ?? "", item.Value));
                }
                break;
            }
            finally { Marshal.FreeHGlobal(buffer); }
        }
        return rows;
    }

    private static IEnumerable<GpuLoad> EnumerateAdapters()
    {
        var iid = new Guid("770aae78-f26f-4dba-a829-253c83d1b387"); // IDXGIFactory1
        if (CreateDXGIFactory1(ref iid, out var factory) != 0) yield break;
        try
        {
            var enumerate = Method<EnumAdapters>(factory, 12);
            for (uint i = 0; i < 32 && enumerate(factory, i, out var adapter) == 0; i++)
            {
                try
                {
                    if (Method<GetDesc>(adapter, 10)(adapter, out var desc) != 0 || (desc.Flags & 2) != 0) continue;
                    string luid = $"0x{desc.LuidHigh:x8}_0x{desc.LuidLow:x8}";
                    yield return new GpuLoad(luid, desc.Description, null, null, (long)desc.DedicatedVideoMemory, null);
                }
                finally { Marshal.Release(adapter); }
            }
        }
        finally { Marshal.Release(factory); }
    }

    private static T Method<T>(IntPtr obj, int slot) where T : Delegate =>
        Marshal.GetDelegateForFunctionPointer<T>(Marshal.ReadIntPtr(Marshal.ReadIntPtr(obj), slot * IntPtr.Size));

    public void Dispose()
    {
        if (_query != IntPtr.Zero) PdhCloseQuery(_query);
        _query = _engines = _memory = _shared = IntPtr.Zero;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct CounterValue { public IntPtr Name; public uint Status; public double Value; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct AdapterDesc
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string Description;
        public uint VendorId, DeviceId, SubSysId, Revision;
        public nuint DedicatedVideoMemory, DedicatedSystemMemory, SharedSystemMemory;
        public uint LuidLow, LuidHigh, Flags;
    }
    [UnmanagedFunctionPointer(CallingConvention.StdCall)] private delegate int EnumAdapters(IntPtr self, uint index, out IntPtr adapter);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)] private delegate int GetDesc(IntPtr self, out AdapterDesc desc);
    [DllImport("dxgi.dll")] private static extern int CreateDXGIFactory1(ref Guid iid, out IntPtr factory);
    [DllImport("pdh.dll", CharSet = CharSet.Unicode)] private static extern uint PdhOpenQueryW(string? source, IntPtr user, out IntPtr query);
    [DllImport("pdh.dll", CharSet = CharSet.Unicode)] private static extern uint PdhAddEnglishCounterW(IntPtr query, string path, IntPtr user, out IntPtr counter);
    [DllImport("pdh.dll")] private static extern uint PdhCollectQueryData(IntPtr query);
    [DllImport("pdh.dll", CharSet = CharSet.Unicode)] private static extern uint PdhGetFormattedCounterArrayW(IntPtr counter, uint format, ref uint size, out uint count, IntPtr buffer);
    [DllImport("pdh.dll")] private static extern uint PdhCloseQuery(IntPtr query);
}
