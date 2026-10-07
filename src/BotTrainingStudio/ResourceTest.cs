using System.Diagnostics;
using System.Text.Json;

namespace BotTrainingStudio;

internal static class ResourceTest
{
    public static int Load()
    {
        var bytes = new byte[96 * 1024 * 1024];
        for (int i = 0; i < bytes.Length; i += 4096) bytes[i] = 17;
        Parallel.For(0, 2, _ =>
        {
            var watch = Stopwatch.StartNew();
            double value = 1;
            while (watch.Elapsed < TimeSpan.FromSeconds(5)) value = Math.Sqrt(value + 1.2345);
            GC.KeepAlive(value);
        });
        GC.KeepAlive(bytes);
        return 0;
    }

    public static async Task<int> Run(string[] args)
    {
        int at = Array.IndexOf(args, "--resource-test");
        string receipt = Path.GetFullPath(args[at + 1]);
        Directory.CreateDirectory(Path.GetDirectoryName(receipt)!);
        var checks = new List<string>();
        var samples = new List<object>();
        void Check(bool pass, string name) { if (!pass) throw new Exception(name); checks.Add(name); }
        try
        {
            const string a = "0x00000000_0x00000001", b = "0x00000000_0x00000002";
            var cards = new[] { new GpuLoad(a, "Card A", null, null, 8L << 30, null), new GpuLoad(b, "Card B", null, null, 32L << 30, null) };
            var combined = GpuCounters.Combine(cards,
                [("pid_1_luid_" + a + "_phys_0_eng_0_engtype_Compute", 40),
                 ("pid_2_luid_" + a + "_phys_0_eng_0_engtype_Compute", 30),
                 ("pid_1_luid_" + a + "_phys_0_eng_1_engtype_Compute", 60),
                 ("pid_3_luid_" + b + "_phys_0_eng_0_engtype_3D", 12),
                 ("pid_1_luid_" + a + "_phys_0_eng_0_engtype_Compute", double.NaN)],
                [("luid_" + a + "_phys_0", 2L << 30), ("luid_" + b + "_phys_0", 16L << 30)], []);
            Check(combined[0].Percent == 70 && combined[1].Percent == 12, "GPU busiest physical engine, summed across processes, isolated by adapter");
            Check(combined[0].Used == 2L << 30 && combined[1].Total == 32L << 30, "GPU usage and capacity retain adapter identity");
            var missing = GpuCounters.Combine(cards, [], [], []);
            Check(missing.All(g => g.Percent == null && g.Used == null && g.Shared == null), "Unavailable counters remain unknown, not zero");
            Check(GpuCounters.Combine([], [], [], []).Count == 0, "No GPU degrades without fabricated adapter");
            Check(GpuCounters.HardwareAdapter(0x231b) && !GpuCounters.HardwareAdapter(0x342) && !GpuCounters.HardwareAdapter(0x105), "Physical GPU retained; indirect display and software renderer excluded");
            Check(GpuCounters.HardwareAdapter(2048), "Headless compute hardware remains eligible");
            Check(GpuCounters.Combine([], [("pid_1_luid_"+a+"_phys_0_eng_0_engtype_3D", 25)], [("luid_"+a+"_phys_0", 100)], []).Count==0,
                "Unmapped performance counters never invent a GPU menu entry");
            Check(GpuCounters.Combine([cards[0], cards[1] with { Name = cards[0].Name }], [], [], []).Count==2,
                "Two genuine adapters with identical names remain separate");

            int pid = 0;
            var runner = new JobRunner();
            using var sampler = new ResourceSampler(() => Volatile.Read(ref pid) is var child && child != 0 ? child : runner.WorkerPid);
            await Task.Delay(2300);
            var idle = sampler.Latest;
            samples.Add(new { phase = "idle", sample = idle });
            Check(idle.Cpu is >= 0 and <= 100 && idle.RamUsed > 0 && idle.RamTotal >= idle.RamUsed, "Live Windows CPU/RAM available");
            var start = new ProcessStartInfo(Environment.ProcessPath!) { UseShellExecute = false, CreateNoWindow = true };
            start.ArgumentList.Add("--resource-load");
            using var load = Process.Start(start)!;
            Volatile.Write(ref pid, load.Id);
            var busy = new List<ResourceSnapshot>();
            while (!load.HasExited)
            {
                await Task.Delay(650);
                busy.Add(sampler.Latest); samples.Add(new { phase = "cpu-load", sample = sampler.Latest });
            }
            Volatile.Write(ref pid, 0);
            Check(load.ExitCode == 0 && busy.Any(s => s.JobCpu > 0 && s.JobRam > 80L * 1024 * 1024), "Real CPU work and touched resident RAM appear as job usage");
            await Task.Delay(1400);
            Check(!sampler.Latest.JobRunning && sampler.Latest.JobCpu == null && sampler.Latest.JobRam == null, "Finished job clears CPU/RAM");
            if (args.Length > at + 3)
            {
                var settings = new StudioSettings { Python = args[at + 2], WorkerDirectory = args[at + 3] };
                for (int run = 0; run < 2; run++)
                {
                    var task = runner.RunAsync(settings, new() { ["action"] = "calibrate_decisions", ["backend"] = "cuda", ["profile"] = "compact", ["context"] = 16 });
                    while (!task.IsCompleted)
                    { await Task.Delay(500); samples.Add(new { phase = "cuda-optimizer", sample = sampler.Latest }); }
                    var result = await task;
                    Check(result.GetProperty("type").GetString() == "completed", "Real CUDA optimizer calibration " + (run + 1));
                    Check(runner.WorkerPid == 0, "Worker PID clears after calibration " + (run + 1));
                }
                await Task.Delay(1500);
                samples.Add(new { phase = "after-cuda", sample = sampler.Latest });
                Check(!sampler.Latest.JobRunning, "Sampler clears completed CUDA job");
                // A sustained real optimizer workload crosses several one-second counter intervals.
                // This checks telemetry, not prediction quality, and saves no training artifacts.
                var stress = new ProcessStartInfo(settings.Python) { UseShellExecute = false, CreateNoWindow = true };
                stress.ArgumentList.Add("-I"); stress.ArgumentList.Add("-c");
                stress.ArgumentList.Add("""
                    import sys,time,torch
                    sys.path.insert(0,sys.argv[1])
                    from opentdm_x_trainer.models import create
                    torch.set_num_threads(4)
                    model=create('compact',351,35).cuda()
                    opt=torch.optim.AdamW(model.parameters(),foreach=False)
                    x=torch.randn(1024,16,351,device='cuda')
                    started=time.monotonic()
                    while time.monotonic()-started<10:
                        opt.zero_grad(set_to_none=True)
                        with torch.autocast('cuda',dtype=torch.bfloat16): loss=model(x).square().mean()
                        loss.backward(); opt.step(); torch.cuda.synchronize()
                    """);
                stress.ArgumentList.Add(settings.WorkerDirectory);
                using var gpuWork = Process.Start(stress)!;
                Volatile.Write(ref pid, gpuWork.Id);
                var gpuSamples = new List<ResourceSnapshot>();
                while (!gpuWork.HasExited)
                { await Task.Delay(500); gpuSamples.Add(sampler.Latest); samples.Add(new { phase = "sustained-cuda", sample = sampler.Latest }); }
                Volatile.Write(ref pid, 0);
                Check(gpuWork.ExitCode == 0, "Sustained CUDA optimizer completed");
                Check(gpuSamples.Any(s => s.Gpus.Any(g => g.Percent > 5 && g.Used > 0)), "WDDM GPU utilization and VRAM present during sustained CUDA work");
                Check(gpuSamples.Any(s => s.JobCpu > 0 && s.JobRam > 100L * 1024 * 1024), "Sustained CUDA job CPU/RAM sampled");
            }
            sampler.Dispose();
            await sampler.Completion.WaitAsync(TimeSpan.FromSeconds(5));
            Check(sampler.Completion.IsCompletedSuccessfully, "Sampler stops and releases native queries");
            File.WriteAllText(receipt, JsonSerializer.Serialize(new { pass = true, checks, samples }, new JsonSerializerOptions { WriteIndented = true }));
            return 0;
        }
        catch (Exception ex)
        {
            File.WriteAllText(receipt, JsonSerializer.Serialize(new { pass = false, checks, error = ex.ToString(), samples }, new JsonSerializerOptions { WriteIndented = true }));
            return 1;
        }
    }
}
