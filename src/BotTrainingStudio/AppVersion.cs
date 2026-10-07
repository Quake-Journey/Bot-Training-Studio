using System.Reflection;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace BotTrainingStudio;

internal sealed record ReleaseNote(string Version, string Date, string[] Ru, string[] En);

internal static class AppVersion
{
    public static string Current => typeof(AppVersion).Assembly
        .GetCustomAttribute<AssemblyInformationalVersionAttribute>()!.InformationalVersion.Split('+')[0];
    public static bool IsPreview => Current.Contains('-');
    public static ReleaseNote[] History { get; } = ReadHistory();
    private static ReleaseNote[] ReadHistory()
    {
        using var stream = RuntimeSetup.Resource("changelog.json");
        return JsonSerializer.Deserialize<ReleaseNote[]>(stream, new JsonSerializerOptions { PropertyNameCaseInsensitive = true })!;
    }
    public static string HistoryText(string language, string? since = null) => string.Join("\n\n",
        History.Where(n => since == null || !StudioVersion.TryParse(since, out var old) || StudioVersion.Parse(n.Version).CompareTo(old) > 0)
        .Select(n => n.Version + " — " + n.Date + "\n" + string.Join("\n", (language == "ru" ? n.Ru : n.En).Select(t => "• " + t))));
}

internal sealed record StudioVersion(Version Number, string? Preview) : IComparable<StudioVersion>
{
    public static bool TryParse(string? value, out StudioVersion version)
    {
        version = new(new Version(0, 0, 0), null);
        if (value == null) return false;
        var match = Regex.Match(value, @"^v?(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-([0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*))?$");
        if (!match.Success || !Version.TryParse(string.Join('.', match.Groups.Cast<Group>().Skip(1).Take(3).Select(g => g.Value)), out var number)) return false;
        version = new(number, match.Groups[4].Success ? match.Groups[4].Value : null);
        return true;
    }
    public static StudioVersion Parse(string value) => TryParse(value, out var version) ? version : throw new InvalidDataException("Invalid application version");
    public int CompareTo(StudioVersion? other)
    {
        if (other == null) return 1;
        int number = Number.CompareTo(other.Number);
        if (number != 0) return number;
        if (Preview == other.Preview) return 0;
        if (Preview == null) return 1;
        if (other.Preview == null) return -1;
        var a = Preview.Split('.'); var b = other.Preview.Split('.');
        for (int i = 0; i < Math.Min(a.Length, b.Length); i++)
        {
            bool an = long.TryParse(a[i], out long av), bn = long.TryParse(b[i], out long bv);
            int part = an && bn ? av.CompareTo(bv) : an != bn ? (an ? -1 : 1) : string.CompareOrdinal(a[i], b[i]);
            if (part != 0) return part;
        }
        return a.Length.CompareTo(b.Length);
    }
}
