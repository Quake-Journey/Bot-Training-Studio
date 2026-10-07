using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Platform.Storage;

namespace BotTrainingStudio;

public sealed partial class MainWindow
{
    private readonly bool _checkRuntime;
    private readonly CancellationTokenSource _lifetime = new();
    private CancellationTokenSource? _setupCancellation;
    private RuntimeProgress? _setupProgress;
    private RuntimeProbe? _runtimeProbe;
    private string? _runtimeProbePath;
    private bool _runtimeChecking;
    private Task? _runtimeCheck;

    private string RuntimeStatus() => _runtimeChecking ? L("Проверяем Python и библиотеки…", "Checking Python and libraries…")
        : _runtimeProbe?.Ready == true && _runtimeProbePath == S.Python
        ? "✓ Python " + _runtimeProbe.Python + " · PyTorch " + _runtimeProbe.Torch
            + (_runtimeProbe.UsesBundledLibraries ? L(" · библиотеки студии", " · Studio libraries")
                : L(" · готовая среда Python", " · existing Python environment"))
        : L("Среда обучения не готова. Выбери или установи Python в настройках.", "The training runtime is not ready. Select or install Python in Settings.");

    private Border RuntimeCard()
    {
        var panel = Stack(12);
        panel.Children.Add(Text(L("Python для запуска", "Python interpreter"), 19, true));
        panel.Children.Add(Text(RuntimeStatus()));
        panel.Children.Add(Text(L("Python 3.13 x64 и библиотеки входят в комплект. Можно использовать совместимый установленный Python или готовую среду Python 3.12 и новее с нужными библиотеками. Если подходящей среды нет, установи Python из комплекта кнопкой ниже.",
            "Python 3.13 x64 and libraries are included. You can reuse compatible installed Python or a complete Python 3.12+ environment with the required libraries. Otherwise, install Python from the package below.")));
        panel.Children.Add(Text(RuntimeSetup.Libraries is { } libraries
            ? L("Библиотеки в комплекте: ", "Included libraries: ") + (libraries.Backend == "cuda" ? "NVIDIA / CPU" : "CPU")
            : L("Не найдена папка libraries. Распакуй полный комплект программы.", "The libraries folder is missing. Extract the complete application package.")));
        panel.Children.Add(Text(L("Установка из комплекта работает без интернета; требуется около 100 МиБ свободного места с запасом. Права администратора не нужны. Для GPU требуется драйвер; AMD/Intel пока отдельно не проверены.",
            "Setup from the package works offline and needs about 100 MiB of free space including headroom. Administrator rights are not required. GPU acceleration needs a driver; AMD/Intel are not yet separately qualified."), 13));
        var buttons = new WrapPanel { Orientation = Orientation.Horizontal };
        var install = Button(L("Установить Python из комплекта", "Install bundled Python"), async () => await InstallRuntimeAsync(), true, true);
        install.IsEnabled &= RuntimeSetup.Libraries != null && File.Exists(RuntimeSetup.BundledPythonArchive);
        install.Name = "InstallRuntime"; buttons.Children.Add(install);
        buttons.Children.Add(Button(L("Указать установленный Python…", "Select installed Python…"), async () => await ChooseRuntimeAsync(), job: true));
        panel.Children.Add(buttons);
        if (_setupCancellation != null) panel.Children.Add(Button(L("Отменить установку", "Cancel installation"), async () => await RequestStopAsync()));
        if (!string.IsNullOrWhiteSpace(S.Python)) panel.Children.Add(Text(S.Python, 12));
        if (_runtimeProbe is { Ready: false, Detail.Length: > 0 }) panel.Children.Add(Text(_runtimeProbe.Detail, 12));
        return Card(panel);
    }

    private Task RefreshRuntimeAsync()
    {
        if (_runtimeCheck is { IsCompleted: false }) return _runtimeCheck;
        return _runtimeCheck = CheckRuntimeCore();
    }

    private async Task CheckRuntimeCore()
    {
        _runtimeChecking = true;
        if (_page is "home" or "settings") ShowPage();
        string candidate = S.Python;
        try
        {
            // No Store aliases are executed during discovery. Ready external environments can supply their own libraries.
            var probe = await RuntimeSetup.ProbeAsync(candidate, _lifetime.Token);
            if (!probe.Ready)
            {
                foreach (var path in RuntimeSetup.Candidates(StudioSettings.Home, S.RuntimeBackend))
                {
                    _lifetime.Token.ThrowIfCancellationRequested();
                    var found = await RuntimeSetup.ProbeAsync(path, _lifetime.Token);
                    if (found.Ready) { candidate = path; probe = found; break; }
                }
            }
            if (!_lifetime.IsCancellationRequested)
            {
                _runtimeProbe = probe; _runtimeProbePath = candidate;
                if (probe.Ready)
                {
                    S.Python = candidate;
                    S.PythonUsesBundledLibraries = probe.UsesBundledLibraries;
                    S.Save();
                }
            }
        }
        catch (OperationCanceledException) { }
        catch (Exception ex) { _runtimeProbe = new(false, ex.Message); }
        finally { _runtimeChecking = false; if (!_lifetime.IsCancellationRequested && _page is "home" or "settings") ShowPage(); }
    }

    private async Task ChooseRuntimeAsync()
    {
        if (JobActive) return;
        try
        {
            var files = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions { Title = L("Выбрать Python среды обучения", "Choose training Python"),
                AllowMultiple = false, FileTypeFilter = [new FilePickerFileType("Python") { Patterns = ["python.exe"] }] });
            if (files.Count == 0 || files[0].TryGetLocalPath() is not { } path) return;
            if (JobActive) return;
            if (_runtimeCheck != null) await _runtimeCheck;
            _status.Text = L("Проверяем выбранную среду…", "Checking selected runtime…");
            var probe = await RuntimeSetup.ProbeAsync(path, _lifetime.Token);
            if (!probe.Ready) { _status.Text = L("Среда не готова: ", "Runtime is not ready: ") + probe.Detail; return; }
            S.Python = path; S.PythonUsesBundledLibraries = probe.UsesBundledLibraries; S.Save(); _runtimeProbe = probe; _runtimeProbePath = path;
            ShowPage(); _status.Text = RuntimeStatus();
        }
        catch (OperationCanceledException) { }
        catch (Exception ex) { _status.Text = L("Не удалось выбрать среду: ", "Could not select runtime: ") + ex.Message; }
    }

    private async Task InstallRuntimeAsync()
    {
        if (JobActive || _exitPromptOpen || _exitAfterJob) return;
        using var cancellation = new CancellationTokenSource();
        _setupCancellation = cancellation;
        _setupProgress = new("checking");
        ShowPage();
        try
        {
            // Finish any startup probe before replacing the selected path, so a stale result cannot restore it.
            if (_runtimeCheck != null) await _runtimeCheck;
            S.RuntimeBackend = RuntimeSetup.Libraries?.Backend ?? S.RuntimeBackend;
            string python = await new RuntimeSetup().InstallAsync(StudioSettings.Home, S.RuntimeBackend,
                progress => Volatile.Write(ref _setupProgress, progress), cancellation.Token);
            S.Python = python; S.PythonUsesBundledLibraries = true; S.Save(); _runtimeProbePath = python;
            _runtimeProbe = await RuntimeSetup.ProbeAsync(python);
            _progress.Value = 100; _status.Text = RuntimeStatus();
            if (S.RuntimeBackend == "cuda" && !_runtimeProbe.Cuda)
                _status.Text += L(" NVIDIA сейчас недоступна: проверь драйвер; CPU остаётся доступен.", " NVIDIA is unavailable: check your driver; CPU remains available.");
        }
        catch (OperationCanceledException) when (cancellation.IsCancellationRequested)
        { _status.Text = L("Установка отменена. Прежняя среда сохранена.", "Installation cancelled. Previous runtime preserved."); }
        catch (Exception ex)
        { _status.Text = L("Не удалось установить среду: ", "Could not install runtime: ") + ex.Message; AppendLog(ex.ToString()); }
        finally
        {
            _setupCancellation = null; _setupProgress = null;
            ShowPage();
            if (_exitAfterJob) { _allowClose = true; Close(); }
        }
    }

    private void ShowSetupProgress(RuntimeProgress progress)
    {
        _progress.Value = progress.Fraction * 100;
        _status.Text = progress.Stage switch
        {
            "downloading" => L("Скачиваем ", "Downloading ") + progress.Name + " · " + (progress.Bytes / 1048576d).ToString("F1") + " MiB",
            "extracting" => L("Распаковываем ", "Extracting ") + progress.Name,
            "verifying" => L("Проверяем Python из комплекта…", "Verifying bundled Python…"),
            "ready" => L("Среда установлена", "Runtime installed"),
            _ => L("Проверяем Python, библиотеки и вычисления…", "Checking Python, libraries and computation…")
        };
    }

    private Task<bool> RequestStopAsync()
    {
        if (_updateCancellation is { } update) { update.Cancel(); return Task.FromResult(true); }
        if (_setupCancellation is { } setup) { setup.Cancel(); return Task.FromResult(true); }
        return _runner.CancelAsync();
    }
}
