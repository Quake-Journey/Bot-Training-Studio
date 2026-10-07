using System.Text.Json;

namespace BotTrainingStudio;

internal static class BridgeTest
{
    public static async Task<int> Run(string[] args)
    {
        int i = Array.IndexOf(args, "--bridge-test");
        string receipt = args[i + 3];
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(receipt))!);
        try
        {
            var settings = new StudioSettings { Python = args[i + 1], WorkerDirectory = args[i + 2] };
            var runner = new JobRunner();
            int events = 0;
            runner.Received += _ => events++;
            var info = await runner.RunAsync(settings, new() { ["action"] = "hardware" });
            if (info.GetProperty("type").GetString() != "completed") throw new Exception(info.ToString());
            var probe = await runner.RunAsync(settings, new() { ["action"] = "probe", ["backend"] = "cpu", ["profile"] = "compact" });
            if (!probe.GetProperty("result").GetProperty("weights_changed").GetBoolean()) throw new Exception("No optimizer update");
            var failure = await runner.RunAsync(settings, new() { ["action"] = "not-supported" });
            if (failure.GetProperty("type").GetString() != "failed") throw new Exception("Invalid action not rejected");
            JsonElement? training = null, cancellation = null;
            if (args.Length > i + 4)
            {
                string store = Path.Combine(StudioSettings.Home, "bridge-model");
                var request = new Dictionary<string, object?> { ["action"] = "train", ["backend"] = "cpu",
                    ["profile"] = "reference", ["mode"] = "fresh", ["epochs"] = 20, ["dataset"] = args[i + 4], ["store"] = store };
                training = await runner.RunAsync(settings, request);
                if (training.Value.GetProperty("type").GetString() != "completed") throw new Exception(training.ToString());
                var pointer = Path.Combine(store, "active.json");
                var before = File.Exists(pointer) ? File.ReadAllBytes(pointer) : [];
                runner.Received += e => { if (e.GetProperty("type").GetString() == "progress" && e.TryGetProperty("epoch", out _)) runner.Cancel(); };
                cancellation = await runner.RunAsync(settings, request);
                if (cancellation.Value.GetProperty("type").GetString() != "cancelled") throw new Exception("Training was not cancelled");
                var after = File.Exists(pointer) ? File.ReadAllBytes(pointer) : [];
                if (!before.SequenceEqual(after)) throw new Exception("Cancellation changed the active model");
            }
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(receipt))!);
            File.WriteAllText(receipt, JsonSerializer.Serialize(new { pass = true, events, hardware = info, probe, failed_action = failure, training, cancellation }, new JsonSerializerOptions { WriteIndented = true }));
            return 0;
        }
        catch (Exception ex)
        {
            File.WriteAllText(receipt, ex.ToString());
            return 1;
        }
    }
}
