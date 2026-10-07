using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Styling;
using FluentAvalonia.Styling;

namespace BotTrainingStudio;

public sealed class App : Application
{
    public static StudioSettings Settings { get; private set; } = new();
    public override void Initialize()
    {
        Styles.Add(new FluentAvaloniaTheme());
        Settings = StudioSettings.Load();
        ApplyTheme();
    }
    public static void ApplyTheme() => Current!.RequestedThemeVariant = Settings.Theme switch
    {
        "light" => ThemeVariant.Light, "dark" => ThemeVariant.Dark, _ => ThemeVariant.Default
    };
    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            var args = Environment.GetCommandLineArgs();
            if (args.Contains("--ui-test"))
            {
                int at = Array.IndexOf(args, "--ui-test");
                try { MainWindow.RenderTests(args[at + 1]); Environment.Exit(0); }
                catch (Exception ex) { Console.Error.WriteLine(ex); Environment.Exit(1); }
            }
            desktop.MainWindow = new MainWindow();
        }
        base.OnFrameworkInitializationCompleted();
    }
}
