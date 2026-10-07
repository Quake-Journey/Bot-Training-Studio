using Avalonia.Controls;
using Avalonia.Layout;
using FluentAvalonia.UI.Controls;
using System.Diagnostics;

namespace BotTrainingStudio;

public sealed partial class MainWindow
{
    private bool _versionStarted, _checkingUpdates, _changesOpen;
    private CancellationTokenSource? _updateCancellation;
    private string? _updateProgress;
    private FAContentDialog? _changesDialog;
    private AvailableUpdate? _availableUpdate;

    private async Task FirstVersionStartAsync()
    {
        if (_versionStarted) return;
        _versionStarted = true;
        // Cleanup waits for the previous updater to exit; it never blocks the dispatcher.
        _ = ReportPreviousUpdateAsync();
        if (S.LastSeenVersion != AppVersion.Current) await ShowChangesAsync(firstStart: true);
        if (!_lifetime.IsCancellationRequested && S.AutoUpdateCheck) await CheckUpdatesAsync(false);
    }

    private async Task ReportPreviousUpdateAsync()
    {
        try
        {
            await Task.Delay(3000, _lifetime.Token);
            string? failure = await Task.Run(() => UpdateInstaller.CleanupFinishedAsync(AppContext.BaseDirectory, StudioSettings.Home));
            if (failure != null && !_lifetime.IsCancellationRequested)
                _status.Text = L("Предыдущее обновление не установлено: ", "The previous update was not installed: ") + failure.Split('\n')[0];
        }
        catch (OperationCanceledException) { }
        catch (Exception error) { if (!_lifetime.IsCancellationRequested) AppendLog(error.Message); }
    }

    private async Task ShowChangesAsync(bool firstStart = false)
    {
        if (_changesOpen || _startupDialog != null || _exitPromptOpen || _updateCancellation != null) return;
        _changesOpen = true;
        try
        {
            var content = Stack(12); content.MaxWidth = 650;
            if (firstStart && S.LastSeenVersion.Length > 0)
                content.Children.Add(Text(L("Предыдущая просмотренная версия: ", "Previously viewed version: ") + S.LastSeenVersion, 13));
            string history = AppVersion.HistoryText(S.EffectiveLanguage);
            var historyBody = Text(history,14); historyBody.Margin = new Avalonia.Thickness(0,0,18,0);
            content.Children.Add(new ScrollViewer { Name = "VersionHistory", MaxHeight = 420,
                VerticalScrollBarVisibility = Avalonia.Controls.Primitives.ScrollBarVisibility.Visible,
                HorizontalScrollBarVisibility = Avalonia.Controls.Primitives.ScrollBarVisibility.Disabled,
                Content = historyBody });
            _changesDialog = new FAContentDialog { Title = L("История версий · ", "Version history · ") + AppVersion.Current, Content = content,
                CloseButtonText = L("Продолжить", "Continue"), DefaultButton = FAContentDialogButton.Close };
            await _changesDialog.ShowAsync(this);
            if (!_lifetime.IsCancellationRequested) { S.LastSeenVersion = AppVersion.Current; S.Save(); }
        }
        finally { _changesOpen = false; _changesDialog = null; }
    }

    private Border UpdateCard()
    {
        var panel = Stack(10);
        panel.Children.Add(Text(L("Версия и обновления", "Version and updates"), 19, true));
        panel.Children.Add(Text("Bot Training Studio " + AppVersion.Current + " by ly"));
        var auto = new CheckBox { Name = "AutoUpdateCheck", Content = L("Автоматически проверять обновления при запуске", "Automatically check for updates at startup"), IsChecked = S.AutoUpdateCheck };
        auto.IsCheckedChanged += (_, _) => { S.AutoUpdateCheck = auto.IsChecked == true; S.Save(); };
        panel.Children.Add(auto);
        panel.Children.Add(Text(L("Новая версия предлагается перед установкой. Работающее обучение и установка Python не прерываются. Проекты, модели и настройки сохраняются; неизменившиеся библиотеки повторно не скачиваются.",
            "A new version is offered before installation. Running training or Python setup is not interrupted. Projects, models and settings are preserved; unchanged libraries are not downloaded again."), 13));
        var buttons = new WrapPanel { Orientation = Orientation.Horizontal };
        buttons.Children.Add(Button(L("Что нового", "What's new"), async () => await ShowChangesAsync()));
        buttons.Children.Add(Button(_availableUpdate == null ? L("Проверить обновления", "Check for updates") : L("Обновить до ", "Update to ") + _availableUpdate.Version,
            async () => await CheckUpdatesAsync(true)));
        panel.Children.Add(buttons);
        return Card(panel);
    }

    private async Task CheckUpdatesAsync(bool manual)
    {
        if (_checkingUpdates || _startupDialog != null || _updateCancellation != null || _lifetime.IsCancellationRequested || _changesOpen || _exitPromptOpen) return;
        _checkingUpdates = true;
        try
        {
            if (manual) _status.Text = L("Проверяем GitHub Releases…", "Checking GitHub Releases…");
            using var updates = new Updates();
            _availableUpdate = await updates.CheckAsync(_lifetime.Token);
            if (_availableUpdate == null)
            {
                if (manual) _status.Text = L("Новых опубликованных обновлений нет. Текущая версия: ", "No newer published update. Current version: ") + AppVersion.Current;
                return;
            }
            if (_page == "settings") ShowPage();
            if (!manual && JobActive) { _status.Text = L("Доступна версия ", "Version available: ") + _availableUpdate.Version + L(". Обновить можно после задания.", ". Update after your job finishes."); return; }
            var dialog = new FAContentDialog { Title = L("Доступно обновление ", "Update available: ") + _availableUpdate.Version,
                Content = Text(JobActive ? L("Заверши текущее задание перед установкой обновления. Оно не будет остановлено автоматически.", "Finish the current job before installing the update. It will not be stopped automatically.") :
                    L("Скачать проверенное обновление из GitHub Releases и перезапустить студию? Проекты, модели и настройки будут сохранены.", "Download the verified update from GitHub Releases and restart the Studio? Projects, models and settings will be preserved.")),
                PrimaryButtonText = L("Обновить", "Update"), IsPrimaryButtonEnabled = !JobActive,
                CloseButtonText = L("Позже", "Later"), DefaultButton = FAContentDialogButton.Close };
            if (await dialog.ShowAsync(this) != FAContentDialogResult.Primary || JobActive || _lifetime.IsCancellationRequested) return;
            await InstallUpdateAsync(_availableUpdate);
        }
        catch (OperationCanceledException) when (_lifetime.IsCancellationRequested) { }
        catch (Exception ex) { if (manual) _status.Text = L("Проверка обновлений не удалась: ", "Could not check updates: ") + ex.Message; }
        finally { _checkingUpdates = false; }
    }

    private async Task InstallUpdateAsync(AvailableUpdate release)
    {
        if (JobActive || _exitPromptOpen || _exitAfterJob) return;
        using var cancel = new CancellationTokenSource(); _updateCancellation = cancel;
        string? preparedTicket = null; bool helperStarted = false;
        _updateProgress = L("Готовим обновление…", "Preparing update…"); ShowPage();
        try
        {
            if (_runtimeCheck != null) await _runtimeCheck;
            using var updates = new Updates();
            string ticket = preparedTicket = await Task.Run(() => updates.PrepareAsync(release, AppContext.BaseDirectory, text => Volatile.Write(ref _updateProgress,
                text == "verify-libraries" ? L("Проверяем установленные библиотеки…", "Checking installed libraries…") : text), cancel.Token));
            cancel.Token.ThrowIfCancellationRequested();
            string stage = Path.Combine(Path.GetDirectoryName(ticket)!, "new");
            var start = new ProcessStartInfo(Path.Combine(stage, "BotTrainingStudio.exe")) { UseShellExecute = false, WorkingDirectory = stage, CreateNoWindow = true };
            start.ArgumentList.Add("--apply-update"); start.ArgumentList.Add(ticket);
            using var helper = Process.Start(start) ?? throw new IOException("Could not start update helper");
            helperStarted = true;
            _allowClose = true; Close();
        }
        catch (OperationCanceledException) { _status.Text = L("Обновление отменено. Прежняя версия сохранена.", "Update cancelled. Previous version preserved."); }
        catch (Exception ex) { _status.Text = L("Обновление не установлено: ", "Update not installed: ") + ex.Message; AppendLog(ex.ToString()); }
        finally
        {
            if (!helperStarted && preparedTicket != null)
            {
                try { await Task.Run(() => Updates.DeleteOwnedJob(Path.GetDirectoryName(preparedTicket)!)); }
                catch (Exception error) { AppendLog(error.Message); }
            }
            _updateCancellation = null; _updateProgress = null;
            if (!_allowClose) { ShowPage(); if (_exitAfterJob) { _allowClose = true; Close(); } }
        }
    }
}
