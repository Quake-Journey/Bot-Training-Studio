using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;

namespace BotTrainingStudio;

/// <summary>Mapgen-style value/bar meters, kept visible while the page scrolls or changes.</summary>
internal sealed class LoadMeter : Border
{
    private readonly (TextBlock value, ProgressBar bar)[] _rows;
    private readonly TextBlock _job = new() { FontSize = 12, TextWrapping = TextWrapping.Wrap };
    private readonly ComboBox _gpu = new() { MinWidth = 190, MaxWidth = 400, MinHeight = 28, FontSize = 12,
        HorizontalAlignment = HorizontalAlignment.Right };
    private readonly Func<string, string, string> _l;
    private string[] _ids = [];
    private ResourceSnapshot _snapshot = ResourceSnapshot.Empty;
    public string? SelectedGpu => _gpu.SelectedIndex >= 0 && _gpu.SelectedIndex < _ids.Length ? _ids[_gpu.SelectedIndex] : null;

    public LoadMeter(Func<string, string, string> localize, bool light)
    {
        _l = localize;
        Padding = new Thickness(14, 10); CornerRadius = new CornerRadius(10);
        Background = new SolidColorBrush(Color.Parse(light ? "#ffffff" : "#25242e"));
        BorderBrush = new SolidColorBrush(Color.Parse(light ? "#dddde8" : "#353443"));
        BorderThickness = new Thickness(1);
        var layout = new StackPanel { Spacing = 8 };
        var head = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto") };
        head.Children.Add(new TextBlock { Text = _l("Загрузка компьютера", "Computer load"), FontSize = 13,
            FontWeight = FontWeight.SemiBold, VerticalAlignment = VerticalAlignment.Center });
        Grid.SetColumn(_gpu, 1); head.Children.Add(_gpu);
        ToolTip.SetTip(_gpu, _l("Видеокарта для показаний GPU и видеопамяти. Не меняет устройство обучения.",
            "Adapter for the GPU and video memory readings. Does not change the training device."));
        _gpu.SelectionChanged += (_, _) => UpdateValues();
        layout.Children.Add(head);
        var grid = new Grid { ColumnDefinitions = new ColumnDefinitions("*,*,*,*") };
        string[] labels = ["CPU", "RAM", "GPU", _l("Видеопамять", "Video memory")];
        _rows = new (TextBlock, ProgressBar)[4];
        for (int i = 0; i < 4; i++)
        {
            var column = new StackPanel { Spacing = 3, Margin = new Thickness(i > 0 ? 12 : 0, 0, 0, 0) };
            column.Children.Add(new TextBlock { Text = labels[i], FontSize = 12, Opacity = .8 });
            var value = new TextBlock { Text = "—", FontSize = 13, FontWeight = FontWeight.SemiBold };
            var bar = new ProgressBar { Minimum = 0, Maximum = 100, Height = 6, MinHeight = 6,
                MinWidth = 0, ClipToBounds = true, CornerRadius = new CornerRadius(3) };
            column.Children.Add(value); column.Children.Add(bar);
            _rows[i] = (value, bar); Grid.SetColumn(column, i); grid.Children.Add(column);
        }
        layout.Children.Add(grid); layout.Children.Add(_job); Child = layout;
        ToolTip.SetTip(_job, _l("Процесс задания и его активные дочерние процессы. CPU — доля мощности всех логических ядер. RAM — сумма резидентной памяти; общие страницы могут учитываться повторно.",
            "Worker and its live child processes. CPU is a share of all logical cores. RAM sums resident memory; shared pages can be counted more than once."));
        Show(ResourceSnapshot.Empty);
    }

    public void Show(ResourceSnapshot snapshot, string? preferred = null)
    {
        _snapshot = snapshot;
        string? selected = preferred ?? SelectedGpu;
        string[] ids = snapshot.Gpus.Select(g => g.Id).ToArray();
        if (!_ids.SequenceEqual(ids))
        {
            _ids = ids;
            _gpu.ItemsSource = snapshot.Gpus.Select((g, i) => snapshot.Gpus.Count(other => other.Name == g.Name) > 1
                ? $"GPU {i + 1} — {g.Name}" : g.Name).ToArray();
            int index = selected == null ? -1 : Array.IndexOf(ids, selected);
            // First detection prefers the largest discrete card; subsequent samples preserve explicit selection.
            if (index < 0 && ids.Length > 0)
                index = Array.IndexOf(ids, snapshot.Gpus.MaxBy(g => g.Total ?? 0)!.Id);
            _gpu.SelectedIndex = index;
        }
        else if (preferred != null && Array.IndexOf(ids, preferred) is int index && index >= 0) _gpu.SelectedIndex = index;
        _gpu.IsVisible = ids.Length > 0;
        UpdateValues();
    }

    internal static string Amount(long? bytes) => bytes is { } b ? (b / 1073741824.0).ToString("0.0") : "—";
    private string Memory(long? used, long? total) => used is null ? "—" : total is > 0
        ? $"{Amount(used)} / {Amount(total)} GiB" : $"{Amount(used)} GiB";
    private static double? Share(long? used, long? total) => used is { } u && total is > 0 ? 100.0 * u / total : null;
    private static string Percent(double? value) => value is { } v ? $"{v:0}%" : "—";

    private void UpdateValues()
    {
        var s = _snapshot;
        bool stale = DateTimeOffset.UtcNow - s.At > TimeSpan.FromSeconds(5);
        if (stale) s = ResourceSnapshot.Empty;
        Set(0, s.Cpu, Percent(s.Cpu));
        Set(1, Share(s.RamUsed, s.RamTotal), Memory(s.RamUsed, s.RamTotal));
        var card = s.Gpus.FirstOrDefault(g => g.Id == SelectedGpu);
        Set(2, card?.Percent, Percent(card?.Percent));
        Set(3, Share(card?.Used, card?.Total), Memory(card?.Used, card?.Total));
        ToolTip.SetTip(_rows[3].value, card == null ? _l("Показания недоступны", "Readings unavailable")
            : _l("Выделенная видеопамять. Общая память GPU в RAM: ", "Dedicated video memory. Shared GPU memory in RAM: ")
                + (card.Shared is null ? "—" : Amount(card.Shared) + " GiB"));
        _job.Text = stale ? _l("Обновление показаний недоступно", "Resource readings unavailable")
            : s.JobRunning ? _l("Задание", "Job") + $" · CPU {Percent(s.JobCpu)} · RAM {Memory(s.JobRam, null)}"
            : _l("Задание не запущено", "No active job");
    }
    private void Set(int index, double? percent, string value)
    {
        var row = _rows[index];
        row.value.Text = value; row.bar.Value = Math.Clamp(percent ?? 0, 0, 100);
        row.bar.Opacity = percent is null ? .35 : 1;
        row.bar.Foreground = new SolidColorBrush(Color.Parse(percent > 85 ? "#ef4444" : percent > 60 ? "#f59e0b" : "#22c55e"));
    }
}
