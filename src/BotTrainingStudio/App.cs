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
        ApplyLanguage();
        ApplyTheme();
    }
    public static void ApplyLanguage()
    {
        var culture = System.Globalization.CultureInfo.GetCultureInfo(Settings.EffectiveLanguage == "ru" ? "ru-RU" : "en-US");
        System.Globalization.CultureInfo.CurrentUICulture = culture;
        System.Globalization.CultureInfo.DefaultThreadCurrentUICulture = culture;
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
            if (args.Contains("--lifecycle-test"))
            {
                int at = Array.IndexOf(args, "--lifecycle-test");
                desktop.ShutdownMode = Avalonia.Controls.ShutdownMode.OnExplicitShutdown;
                var test = new MainWindow { ShowInTaskbar = false, ShowActivated = false,
                    WindowStartupLocation = Avalonia.Controls.WindowStartupLocation.Manual,
                    Position = new PixelPoint(-20000, -20000) };
                desktop.MainWindow = test;
                test.Opened += async (_, _) => desktop.Shutdown(await test.TestLifecycle(args[at + 1], args[at + 2]));
                base.OnFrameworkInitializationCompleted();
                return;
            }
            if (args.Contains("--ui-test"))
            {
                // The capture suite opens/closes several windows; keep the dispatcher alive between them.
                desktop.ShutdownMode = Avalonia.Controls.ShutdownMode.OnExplicitShutdown;
                int at = Array.IndexOf(args, "--ui-test");
                try { MainWindow.RenderTests(args[at + 1]); Environment.Exit(0); }
                catch (Exception ex) { Console.Error.WriteLine(ex); Environment.Exit(1); }
            }
            desktop.MainWindow = new MainWindow();
        }
        base.OnFrameworkInitializationCompleted();
    }
}
