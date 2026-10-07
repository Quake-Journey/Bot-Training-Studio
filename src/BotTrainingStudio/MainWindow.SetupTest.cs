using Avalonia;
using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.Media.Imaging;
using Avalonia.Threading;
using Avalonia.VisualTree;
using FluentAvalonia.UI.Controls;
using System.Diagnostics;
using System.Text.Json;

namespace BotTrainingStudio;

public sealed partial class MainWindow
{
    internal async Task<int> TestSetupExit(string folder)
    {
        Directory.CreateDirectory(folder);
        var checks = new List<string>();
        bool closed = false; Closed += (_, _) => closed = true;
        void Check(bool value, string name) { if (!value) throw new Exception(name); checks.Add(name); }
        async Task Until(Func<bool> done)
        {
            var time = Stopwatch.StartNew();
            while (!done()) { if (time.Elapsed.TotalSeconds > 45) throw new TimeoutException("Setup exit test timed out"); await Task.Delay(30); }
        }
        try
        {
            string previous = S.Python;
            Check(File.Exists(previous), "Previous ready runtime exists");
            Environment.SetEnvironmentVariable("BTS_HOME", Path.Combine(StudioSettings.Home, "exit-attempt"));
            S.Language = "en"; S.RuntimeBackend = "cuda"; S.Save(); App.ApplyLanguage();
            var installation = InstallRuntimeAsync();
            await Until(() => _setupProgress is { Stage: "checking", Fraction: > .9 } || installation.IsCompleted);
            Check(!installation.IsCompleted, "Replacement runtime is being compute-checked");
            Close();
            await Until(() => _exitDialog?.GetVisualDescendants().OfType<Button>().Any(b => b.Content?.ToString() == "Stop and quit") == true);
            Check(!closed && _setupCancellation?.IsCancellationRequested == false, "Close dialog itself does not abort setup");
            var button = _exitDialog!.GetVisualDescendants().OfType<Button>().Single(b => b.Content?.ToString() == "Stop and quit");
            button.RaiseEvent(new RoutedEventArgs(Avalonia.Controls.Button.ClickEvent));
            await installation; await Until(() => closed);
            Check(!JobActive && _setupCancellation == null, "Stop and quit awaits setup completion");
            Check(S.Python == previous && StudioSettings.Load().Python == previous, "Previous runtime still selected after cancelled replacement");
            Check(!Directory.EnumerateDirectories(Path.Combine(StudioSettings.Home, "runtimes"), ".setup-*").Any(), "Setup staging removed before exit");
            Check(RuntimeSetup.FindManaged(StudioSettings.Home, "cuda") == null, "Cancelled replacement never activated");
            await File.WriteAllTextAsync(Path.Combine(folder, "setup-exit-test.json"), JsonSerializer.Serialize(new { pass = true, checks }, new JsonSerializerOptions { WriteIndented = true }));
            return 0;
        }
        catch (Exception ex)
        {
            await RequestStopAsync();
            await File.WriteAllTextAsync(Path.Combine(folder, "setup-exit-test.json"), JsonSerializer.Serialize(new { pass = false, checks, error = ex.ToString(), log = _log.Text }));
            return 1;
        }
    }

    internal async Task<int> TestSetup(string folder, string backend = "cpu")
    {
        Directory.CreateDirectory(folder);
        var checks = new List<string>();
        long last = Stopwatch.GetTimestamp();
        void Check(bool value, string name) { if (!value) throw new Exception(name); checks.Add(name); }
        async Task Until(Func<bool> done, int seconds = 30)
        {
            var time = Stopwatch.StartNew();
            while (!done()) { if (time.Elapsed.TotalSeconds > seconds) throw new TimeoutException("Setup UI condition timed out"); await Task.Delay(50); }
        }
        void Capture(string name)
        {
            UpdateLayout();
            using var image = new RenderTargetBitmap(new PixelSize((int)Bounds.Width, (int)Bounds.Height), new Vector(96, 96));
            image.Render(this); using var output = File.Create(Path.Combine(folder, name + ".png")); image.Save(output, new PngBitmapEncoderOptions());
            // Synchronous bitmap encoding is test instrumentation, not runtime installation work.
            last = Stopwatch.GetTimestamp();
        }
        int ticks = 0; double maxGap = 0;
        var beat = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(25) };
        beat.Tick += (_, _) => { long now = Stopwatch.GetTimestamp(); maxGap = Math.Max(maxGap, (now-last)*1000d/Stopwatch.Frequency); last=now;ticks++; };
        try
        {
            Check(Icon != null, "Window has explicit application icon");
            S.Python = ""; S.RuntimeBackend = backend; S.Language = "ru"; S.Save();
            // No Python on PATH. Install the packaged interpreter through the startup dialog.
            Environment.SetEnvironmentVariable("PATH", Environment.GetFolderPath(Environment.SpecialFolder.System));
            _runtimeProbe = new(false, ""); _runtimeProbePath = ""; Navigate("settings");
            beat.Start();
            Capture("ru-setup-missing");
            var install = ShowStartupCheckAsync(() => { _runtimeProbe = new(false, "Test missing Python"); return Task.CompletedTask; });
            await Until(() => _startupDialog?.GetVisualDescendants().OfType<Button>().Any(b => b.Content?.ToString() == "Установить Python") == true);
            _startupDialog!.GetVisualDescendants().OfType<Button>().Single(b => b.Content?.ToString() == "Установить Python")
                .RaiseEvent(new RoutedEventArgs(Avalonia.Controls.Button.ClickEvent));
            await Until(() => _setupProgress is { Stage: "checking", Fraction: > .9 } || install.IsCompleted);
            Check(!install.IsCompleted, "Offline setup starts without system Python");
            foreach (var page in new[] { "train", "models", "jobs", "home", "settings" }) { Navigate(page); await Task.Delay(60); Check(_page == page, "Setup allows navigation: " + page); }
            int before = ticks; await Task.Delay(300); Check(ticks-before >= 5, "UI responds during offline setup");
            Start("hardware"); Check(!_runner.IsRunning, "Training cannot race installation");
            Capture("ru-setup-extracting");
            Close();
            await Until(() => _exitDialog?.GetVisualDescendants().OfType<Button>().Any(b => b.Content?.ToString() == _exitDialog.CloseButtonText) == true);
            Check(_exitDialog!.DefaultButton == FAContentDialogButton.Close, "Closing setup defaults to keep working");
            Capture("ru-setup-close");
            var close = _exitDialog.GetVisualDescendants().OfType<Button>().Single(b => b.Content?.ToString() == _exitDialog.CloseButtonText);
            close.RaiseEvent(new RoutedEventArgs(Avalonia.Controls.Button.ClickEvent));
            await Until(() => !_exitPromptOpen);
            Check(_setupCancellation != null && !_setupCancellation.IsCancellationRequested, "Keep working leaves installation running");
            await RequestStopAsync(); await install;
            Check(!JobActive && string.IsNullOrEmpty(S.Python), "Cancellation preserves selected runtime");
            Check(!Directory.EnumerateDirectories(Path.Combine(StudioSettings.Home, "runtimes"), ".setup-*").Any(), "Cancelled offline setup staging cleaned up");
            Check(RuntimeSetup.FindManaged(StudioSettings.Home, backend) == null, "Cancelled offline setup never becomes active");
            S.Language = "en"; App.ApplyLanguage(); Navigate("settings"); Capture("en-setup-retry");
            install = InstallRuntimeAsync();
            await Until(() => install.IsCompleted, 1800); await install;
            Check(_runtimeProbe?.Ready == true, "Real " + backend + " runtime installed and compute-probed");
            Check(S.Python.StartsWith(Path.Combine(StudioSettings.Home, "runtimes"), StringComparison.OrdinalIgnoreCase), "Interpreter belongs to app-managed home");
            Check(StudioSettings.Load().Python == S.Python, "Installed Python remembered after reload");
            Check(_runtimeProbe!.Torch == (backend == "cuda" ? "2.10.0+cu128" : "2.10.0+cpu"), "Exact pinned PyTorch imported");
            Check(RuntimeSetup.Libraries != null, "Model libraries are supplied in the application package");
            Check(File.Exists(RuntimeSetup.BundledPythonArchive), "Python archive is supplied in the application package");
            Check(!Directory.Exists(Path.Combine(Path.GetDirectoryName(S.Python)!, "Lib", "site-packages", "torch")), "Setup installs only Python without downloading or copying model libraries");
            if (backend == "cuda") Check(_runtimeProbe.Cuda, "CUDA runtime detects the test GPU");
            string original = S.Python;
            await InstallRuntimeAsync();
            Check(S.Python == original, "Repeated setup reuses verified runtime");
            string originalHome = StudioSettings.Home;
            Environment.SetEnvironmentVariable("BTS_HOME", Path.Combine(originalHome, "reuse-existing"));
            Environment.SetEnvironmentVariable("PATH", Path.GetDirectoryName(original) + Path.PathSeparator + Environment.GetFolderPath(Environment.SpecialFolder.System));
            S.Python = "";
            await RefreshRuntimeAsync();
            Check(S.Python == original && _runtimeProbe?.Ready == true, "Existing bare Python is discovered and uses packaged libraries without installation");
            Check(!Directory.Exists(Path.Combine(StudioSettings.Home, "runtimes")), "Reusing installed Python creates no duplicate interpreter");
            Environment.SetEnvironmentVariable("BTS_HOME", originalHome);
            S.Save();
            Navigate("home"); Capture("en-setup-complete");
            var result = await _runner.RunAsync(S, new() { ["action"] = "hardware" });
            Check(result.GetProperty("type").GetString() == "completed", "Actual Studio worker runs with newly installed Python");
            result = await _runner.RunAsync(S, new() { ["action"] = "probe", ["backend"] = backend, ["profile"] = "compact" });
            Check(result.GetProperty("type").GetString() == "completed", "Packaged libraries execute a real model forward/backward on " + backend);
            Check(maxGap < 1500, "No long UI stall during offline extraction and checks");
            await File.WriteAllTextAsync(Path.Combine(folder, "setup-ui-test.json"), JsonSerializer.Serialize(new { pass = true, checks, ticks, max_ui_gap_ms = maxGap, python = S.Python }, new JsonSerializerOptions { WriteIndented = true }));
            return 0;
        }
        catch (Exception ex)
        {
            await RequestStopAsync(); await Until(() => !JobActive, 120);
            await File.WriteAllTextAsync(Path.Combine(folder, "setup-ui-test.json"), JsonSerializer.Serialize(new { pass = false, checks, error = ex.ToString(), status = _status.Text, log = _log.Text, ticks, max_ui_gap_ms = maxGap }, new JsonSerializerOptions { WriteIndented = true }));
            return 1;
        }
        finally { beat.Stop(); }
    }
}

