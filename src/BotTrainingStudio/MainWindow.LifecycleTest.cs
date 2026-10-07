using Avalonia;
using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Threading;
using Avalonia.VisualTree;
using FluentAvalonia.UI.Controls;
using System.Diagnostics;
using System.Text.Json;

namespace BotTrainingStudio;

public sealed partial class MainWindow
{
    // In-process UI integration check, driven by real isolated worker messages and real dialog buttons.
    internal async Task<int> TestLifecycle(string python, string folder)
    {
        Directory.CreateDirectory(folder);
        var checks = new List<string>();
        void Check(bool ok, string name) { if (!ok) throw new Exception(name); checks.Add(name); }
        async Task Until(Func<bool> done)
        {
            var time = Stopwatch.StartNew();
            while (!done()) { if (time.Elapsed.TotalSeconds > 15) throw new Exception("UI condition timed out"); await Task.Delay(20); }
        }
        var worker = Path.Combine(folder, "fixture", "opentdm_x_trainer");
        Directory.CreateDirectory(worker);
        await File.WriteAllTextAsync(Path.Combine(worker, "__init__.py"), "");
        await File.WriteAllTextAsync(Path.Combine(worker, "studio.py"), """
            import json,sys,time
            from pathlib import Path
            req=Path(sys.argv[sys.argv.index('--request')+1]); data=json.loads(req.read_text()); seq=0
            def emit(kind,**more):
                global seq
                seq+=1
                print(json.dumps(dict(protocol=1,job_id=data['job_id'],seq=seq,type=kind,**more)),flush=True)
            emit('started')
            for i in range(4000): emit('progress',progress=.1,epoch=i,epochs=10000)
            started=time.monotonic()
            while time.monotonic()-started<20:
                if req.with_name('cancel.request').exists():
                    time.sleep(.8)
                    emit('cancelled',message='Stopped cooperatively'); sys.exit(2)
                if req.with_name('finish.request').exists():
                    emit('completed',result=dict(devices=[dict(name='Fixture')],suggested_profile='compact')); sys.exit(0)
                emit('progress',progress=.5);time.sleep(.03)
            emit('failed',message='Fixture timeout');sys.exit(1)
            """);
        S.Python = python; S.WorkerDirectory = Path.GetDirectoryName(worker)!;
        int ticks = 0; double maxGap = 0;
        long last = Stopwatch.GetTimestamp();
        var beat = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(20) };
        beat.Tick += (_, _) =>
        {
            long now = Stopwatch.GetTimestamp();
            maxGap = Math.Max(maxGap, (now - last) * 1000.0 / Stopwatch.Frequency);
            last = now; ticks++;
        };
        bool closed = false; Closed += (_, _) => closed = true;
        bool DialogReady() => _exitDialog?.GetVisualDescendants().OfType<Button>()
            .Any(b => b.Content?.ToString() == _exitDialog.PrimaryButtonText) == true;
        void ClickDialog(string text)
        {
            var button = _exitDialog!.GetVisualDescendants().OfType<Button>().Single(b => b.Content?.ToString() == text);
            button.RaiseEvent(new RoutedEventArgs(Avalonia.Controls.Button.ClickEvent));
        }
        void Capture(string name)
        {
            UpdateLayout();
            using var bitmap = new RenderTargetBitmap(new PixelSize((int)Bounds.Width, (int)Bounds.Height), new Vector(96, 96));
            bitmap.Render(this);
            using var output = File.Create(Path.Combine(folder, name + ".png"));
            bitmap.Save(output, new PngBitmapEncoderOptions());
        }
        try
        {
            beat.Start();
            Start("hardware");
            await Until(() => _runner.WorkerPid > 0 && _progress.Value > 0);
            foreach (var page in new[] { "jobs", "models", "train", "settings", "home" }) { Navigate(page); await Task.Delay(60); Check(_page == page, "Navigation during worker: " + page); }
            int before = ticks;
            await Task.Delay(350);
            Check(ticks - before >= 5, "UI timer keeps ticking under worker event flood");
            Check((_log.Text?.Length ?? 0) < 250000, "UI log stays bounded");
            Close();
            await Until(DialogReady);
            Check(_exitDialog!.DefaultButton == FAContentDialogButton.Close, "Default exit choice keeps working");
            var firstDialog = _exitDialog;
            Close();
            Check(ReferenceEquals(firstDialog, _exitDialog), "Repeated window close does not stack dialogs");
            Check(!File.Exists(Path.Combine(_runner.JobFolder!, "cancel.request")), "Opening exit dialog does not cancel work");
            await Task.Delay(300); Capture("ru-dark-exit");
            ClickDialog(L("Продолжить работу", "Keep working"));
            await Until(() => !_exitPromptOpen);
            Check(!closed && _runner.IsRunning, "Keep working leaves both window and worker alive");

            // A natural completion while the question is open must not deadlock or disable new jobs forever.
            S.Language = "en"; S.Theme = "light"; App.ApplyTheme(); BuildShell();
            Close(); await Until(DialogReady);
            await Task.Delay(300); Capture("en-light-exit");
            await File.WriteAllTextAsync(Path.Combine(_runner.JobFolder!, "finish.request"), "finish");
            await Until(() => !JobActive);
            ClickDialog("Keep working"); await Until(() => !_exitPromptOpen);
            Check(!closed && _jobButtons.All(b => b.IsEnabled), "Natural completion during prompt keeps UI usable");

            // Back out of automatic exit while the worker is cooperatively stopping.
            Start("hardware"); await Until(() => _runner.WorkerPid > 0);
            Close(); await Until(DialogReady);
            ClickDialog("Stop and quit"); await Until(() => _stay.IsVisible);
            _stay.RaiseEvent(new RoutedEventArgs(Avalonia.Controls.Button.ClickEvent));
            await Until(() => !JobActive);
            Check(!closed && !_exitAfterJob, "Stay after stop prevents automatic exit without resuming cancelled work");

            Start("hardware"); await Until(() => _runner.WorkerPid > 0);
            int pid = _runner.WorkerPid;
            Close(); await Until(DialogReady);
            ClickDialog("Stop and quit"); await Until(() => _exitAfterJob);
            Check(!closed, "Stop and quit waits for worker instead of killing it");
            Navigate("jobs"); before = ticks;
            await Task.Delay(250);
            Check(ticks > before, "UI stays responsive during cooperative shutdown");
            Capture("en-light-stopping");
            await Until(() => closed);
            Check(!_runner.IsRunning && _runner.WorkerPid == 0, "Window closes after the worker is reaped");
            bool exists;
            try { using var p = Process.GetProcessById(pid); exists = !p.HasExited; } catch (ArgumentException) { exists = false; }
            Check(!exists, "No owned worker remains after close");
            Check(maxGap < 1000, "No UI stall of one second under event flood");
            File.WriteAllText(Path.Combine(folder, "lifecycle-test.json"), JsonSerializer.Serialize(new { pass = true, checks, ticks, maximum_ui_gap_ms = maxGap }, new JsonSerializerOptions { WriteIndented = true }));
            return 0;
        }
        catch (Exception ex)
        {
            _exitAfterJob = false;
            await _runner.CancelAsync();
            var wait = Stopwatch.StartNew();
            while (_runner.IsRunning && wait.Elapsed.TotalSeconds < 10) await Task.Delay(50);
            File.WriteAllText(Path.Combine(folder, "lifecycle-test.json"), JsonSerializer.Serialize(new { pass = false, checks, error = ex.ToString(), ticks, maximum_ui_gap_ms = maxGap }, new JsonSerializerOptions { WriteIndented = true }));
            return 1;
        }
        finally { beat.Stop(); }
    }
}
