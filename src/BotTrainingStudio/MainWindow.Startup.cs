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
            PrimaryButtonText = L("Открыть настройки", "Open Settings"), IsPrimaryButtonEnabled = false,
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
                : L("Совместимый Python не найден или не прошёл проверку. В настройках можно выбрать установленный Python или установить только Python. Библиотеки уже входят в комплект.",
                    "Compatible Python was not found or did not pass the check. In Settings you can select installed Python or install Python alone. Libraries are already included.");
            dialog.IsPrimaryButtonEnabled = true;
            dialog.CloseButtonText = L("Продолжить без обучения", "Continue without training");
            if (await shown == FAContentDialogResult.Primary && !_lifetime.IsCancellationRequested) Navigate("settings");
        }
        finally { checking = false; dialog.Hide(); _startupDialog = null; }
    }
}
