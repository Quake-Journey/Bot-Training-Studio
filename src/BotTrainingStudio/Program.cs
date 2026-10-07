using Avalonia;

namespace BotTrainingStudio;

internal static class Program
{
    [STAThread]
    public static int Main(string[] args)
    {
        if (args.Contains("--bridge-test")) return BridgeTest.Run(args).GetAwaiter().GetResult();
        BuildApp().StartWithClassicDesktopLifetime(args);
        return 0;
    }
    public static AppBuilder BuildApp() => AppBuilder.Configure<App>()
        .UsePlatformDetect().WithInterFont().LogToTrace();
}
