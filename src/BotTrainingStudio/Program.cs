using Avalonia;

namespace BotTrainingStudio;

internal static class Program
{
    [System.Runtime.InteropServices.DllImport("kernel32.dll")]
    private static extern uint SetErrorMode(uint mode);
    [STAThread]
    public static int Main(string[] args)
    {
        // Setup errors belong in our UI/log, never a modal Windows loader/drive error box.
        if (OperatingSystem.IsWindows()) SetErrorMode(0x8003);
        if (args.Length == 2 && args[0] == "--apply-update") return UpdateInstaller.ApplyAsync(args[1]).GetAwaiter().GetResult();
        if (args.Length == 2 && args[0] == "--update-test") return UpdateTest.Run(args[1]).GetAwaiter().GetResult();
        if (args.Contains("--runtime-test")) return RuntimeTest.Run(args).GetAwaiter().GetResult();
        if (args.Contains("--language-test")) return LanguageTest.Run(args);
        if (args.Contains("--resource-load")) return ResourceTest.Load();
        if (args.Contains("--resource-test")) return ResourceTest.Run(args).GetAwaiter().GetResult();
        if (args.Contains("--bridge-test")) return BridgeTest.Run(args).GetAwaiter().GetResult();
        return BuildApp().StartWithClassicDesktopLifetime(args);
    }
    public static AppBuilder BuildApp() => AppBuilder.Configure<App>()
        .UsePlatformDetect().WithInterFont().LogToTrace();
}
