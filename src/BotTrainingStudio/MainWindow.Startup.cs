using Avalonia.Controls;
using FluentAvalonia.UI.Controls;

namespace BotTrainingStudio;

public sealed partial class MainWindow
{
    private FAContentDialog? _startupDialog;

    private async Task ShowStartupCheckAsync(Func<Task> check)
    {
        if (_startupDialog != null || _lifetime.IsCancellationRequested) return;
        bool checking = true;
        var progress = new ProgressBar { IsIndeterminate = true, Height = 5 };
        var detail = Text(L("Проверяем установленный Python и библиотеки из комплекта программы.",
            "Checking installed Python and the libraries included with the application."));
        var content = Stack(14); content.MaxWidth = 530;
        content.Children.Add(detail); content.Children.Add(progress);
        var dialog = new FAContentDialog
        {
            Title = L("Подготовка студии", "Preparing the Studio"), Content = content,
            PrimaryButtonText = L("Установить Python", "Install Python"), IsPrimaryButtonEnabled = false,
            SecondaryButtonText = L("Открыть настройки", "Open Settings"), IsSecondaryButtonEnabled = false,
            CloseButtonText = L("Выйти", "Quit"), DefaultButton = FAContentDialogButton.Close
        };
        _startupDialog = dialog;
        dialog.CloseButtonClick += (_, _) => { if (checking) Close(); };
        try
        {
            var shown = dialog.ShowAsync(this);
            try { await check(); }
            catch (OperationCanceledException) when (_lifetime.IsCancellationRequested) { }
            catch (Exception error) { _runtimeProbe = new(false, error.Message); }
            checking = false;
            if (_lifetime.IsCancellationRequested || _runtimeProbe?.Ready == true)
            {
                dialog.Hide(); await shown; return;
            }
            // Keep an actionable result in the same dialog, without changing the Home layout.
            progress.IsVisible = false;
            dialog.Title = L("Нужна настройка Python", "Python setup needed");
            detail.Text = RuntimeSetup.Libraries == null
                ? L("Не найдены библиотеки программы. Распакуй полный комплект студии. Просмотр проектов доступен без обучения.",
                    "The application libraries are missing. Extract the complete Studio package. You can still browse projects without training.")
                : _runtimeProbe?.Python is { Length: > 0 } version
                    ? L("Найден Python ", "Found Python ") + version + L(". Эта среда не прошла проверку нужных библиотек. Python 3.13 x64 входит в комплект студии: установи его без интернета или выбери другую готовую среду в настройках.",
                        ". This environment did not pass the required library check. Python 3.13 x64 is included: install it offline or choose another ready environment in Settings.")
                    : L("Подходящая среда Python не найдена. Python 3.13 x64 входит в комплект студии: установи его без интернета или выбери готовую среду в настройках.",
                        "No ready Python environment was found. Python 3.13 x64 is included: install it offline or choose a ready environment in Settings.");
            bool canInstall = RuntimeSetup.Libraries != null && File.Exists(RuntimeSetup.BundledPythonArchive);
            dialog.IsPrimaryButtonEnabled = canInstall;
            dialog.IsSecondaryButtonEnabled = true;
            dialog.CloseButtonText = L("Без обучения", "Skip training");
            var result = await shown;
            dialog.Hide(); _startupDialog = null;
            if (!_lifetime.IsCancellationRequested)
            {
                if (canInstall && result == FAContentDialogResult.Primary) await InstallRuntimeAsync();
                else if (result is FAContentDialogResult.Primary or FAContentDialogResult.Secondary) Navigate("settings");
            }
        }
        finally { checking = false; dialog.Hide(); _startupDialog = null; }
    }
}
