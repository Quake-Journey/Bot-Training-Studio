using Avalonia;

namespace BotTrainingStudio;

internal static class Program
{
    [STAThread]
    public static int Main(string[] args)
    {
        if (args.Contains("--language-test")) return LanguageTest.Run(args);
        if (args.Contains("--resource-load")) return ResourceTest.Load();
        if (args.Contains("--resource-test")) return ResourceTest.Run(args).GetAwaiter().GetResult();
        if (args.Contains("--bridge-test")) return BridgeTest.Run(args).GetAwaiter().GetResult();
        return BuildApp().StartWithClassicDesktopLifetime(args);
    }
    public static AppBuilder BuildApp() => AppBuilder.Configure<App>()
        .UsePlatformDetect().WithInterFont().LogToTrace();
}
