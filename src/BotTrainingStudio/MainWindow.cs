using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform.Storage;
using Avalonia.Threading;
using Avalonia.Styling;
using System.Text.Json;

namespace BotTrainingStudio;

public sealed class MainWindow : Window
{
    private readonly JobRunner _runner = new();
    private readonly ContentControl _content = new();
    private readonly TextBlock _status = new() { TextWrapping = TextWrapping.Wrap };
    private readonly ProgressBar _progress = new() { Minimum = 0, Maximum = 100, Height = 5 };
    private readonly TextBox _log = new() { IsReadOnly = true, AcceptsReturn = true, TextWrapping = TextWrapping.Wrap, MinHeight = 180, MaxHeight = 270 };
    private readonly List<Button> _jobButtons = [];
    private string _page = "home";
    private string _hardware = "";
    private string _result = "";
    private bool _allowClose;
    private StudioSettings S => App.Settings;
    private string L(string ru, string en) => S.Language == "ru" ? ru : en;
    private static readonly IBrush Accent = new SolidColorBrush(Color.Parse("#8b83ff"));
    private bool Light => ActualThemeVariant == ThemeVariant.Light;

    public MainWindow()
    {
        Title = "Bot Training Studio by ly";
        Width = 1180; Height = 820; MinWidth = 980; MinHeight = 700;
        WindowStartupLocation = WindowStartupLocation.CenterScreen;
        _runner.Received += e => Dispatcher.UIThread.Post(() => OnEvent(e));
        _runner.Diagnostic += line => Dispatcher.UIThread.Post(() => AppendLog(line));
        Closing += (_, e) =>
        {
            if (_runner.IsRunning)
            {
                e.Cancel = true;
                _runner.Cancel();
                _allowClose = true;
                _status.Text = L("Останавливаем задание. Предыдущая модель сохранится.", "Stopping the job. The previous model will be preserved.");
            }
        };
        BuildShell();
    }

    private TextBlock Text(string value, double size = 14, bool bold = false) => new()
    {
        Text = value, FontSize = size, FontWeight = bold ? FontWeight.SemiBold : FontWeight.Normal,
        TextWrapping = TextWrapping.Wrap
    };
    private StackPanel Stack(double spacing = 14) => new() { Spacing = spacing };
    private Border Card(Control child) => new()
    {
        Padding = new Thickness(22), CornerRadius = new CornerRadius(12), BorderThickness = new Thickness(1),
        BorderBrush = new SolidColorBrush(Color.Parse(Light ? "#ddddE8" : "#353443")),
        Background = new SolidColorBrush(Color.Parse(Light ? "#ffffff" : "#25242e")), Child = child
    };
    private Button Button(string text, Action action, bool primary = false, bool job = false)
    {
        var b = new Button { Content = text, Padding = new Thickness(18, 10), CornerRadius = new CornerRadius(6) };
        if (primary) { b.Background = new SolidColorBrush(Color.Parse("#6356d9")); b.Foreground = Brushes.White; }
        ToolTip.SetTip(b, text);
        b.Click += (_, _) => action();
        if (job) { b.IsEnabled = !_runner.IsRunning; _jobButtons.Add(b); }
        return b;
    }
    private void BuildShell()
    {
        foreach (var control in new Control[] { _content, _status, _progress })
            if (control.Parent is Panel parent) parent.Children.Remove(control);
        Content = null;
        _jobButtons.Clear();
        var shell = new Grid { ColumnDefinitions = new ColumnDefinitions("220,*"),
            Background = new SolidColorBrush(Color.Parse(Light ? "#f5f5fa" : "#1b1a22")) };
        var sidebar = new StackPanel { Spacing = 8, Margin = new Thickness(16, 26) };
        sidebar.Children.Add(Text("BOT TRAINING\nSTUDIO", 18, true));
        sidebar.Children.Add(new Border { Height = 12 });
        var pages = new[] { ("home", L("Главная", "Home")), ("train", L("Обучение", "Training")),
            ("models", L("Модели и GPU", "Models & GPU")), ("library", L("Библиотека", "Library")),
            ("jobs", L("Задания", "Jobs")), ("settings", L("Настройки", "Settings")) };
        foreach (var (id, label) in pages)
        {
            var item = Button(label, () => Navigate(id));
            item.HorizontalAlignment = HorizontalAlignment.Stretch;
            item.HorizontalContentAlignment = HorizontalAlignment.Left;
            item.Background = id == _page ? new SolidColorBrush(Color.Parse(Light ? "#e3defa" : "#3d365b")) : Brushes.Transparent;
            item.Foreground = Light ? Brushes.Black : Brushes.White;
            sidebar.Children.Add(item);
        }
        sidebar.Children.Add(new Border { Height = 24 });
        sidebar.Children.Add(Text("0.1 · development preview", 11));
        shell.Children.Add(new Border { Background = new SolidColorBrush(Color.Parse(Light ? "#ebebf3" : "#211f2b")), Child = sidebar });
        var outer = new DockPanel();
        var bottom = new StackPanel { Spacing = 8, Margin = new Thickness(32, 8, 32, 18) };
        bottom.Children.Add(_status); bottom.Children.Add(_progress);
        DockPanel.SetDock(bottom, Dock.Bottom); outer.Children.Add(bottom); outer.Children.Add(_content);
        Grid.SetColumn(outer, 1); shell.Children.Add(outer);
        Content = shell;
        ShowPage();
    }
    private void Navigate(string page) { _page = page; BuildShell(); }
    private void ShowPage()
    {
        _jobButtons.Clear();
        var panel = Stack(18); panel.Margin = new Thickness(34, 26, 34, 20);
        switch (_page)
        {
            case "train": Training(panel); break;
            case "models": Models(panel); break;
            case "library": Library(panel); break;
            case "jobs": Jobs(panel); break;
            case "settings": Settings(panel); break;
            default: Home(panel); break;
        }
        _content.Content = new ScrollViewer { Content = panel, HorizontalScrollBarVisibility = Avalonia.Controls.Primitives.ScrollBarVisibility.Disabled };
    }
    private void Header(StackPanel p, string title, string subtitle)
    {
        p.Children.Add(Text(title, 29, true)); p.Children.Add(Text(subtitle, 15));
    }
    private void Home(StackPanel p)
    {
        Header(p, L("Опыт игроков. Новые возможности ботов.", "Player experience. New bot capabilities."),
            L("Локальная студия обучения для OpenTDM-X", "Local learning studio for OpenTDM-X"));
        var banner = Stack(10);
        var tag = Text("DEVELOPMENT PREVIEW  ·  0.1", 12, true); tag.Foreground = Accent; banner.Children.Add(tag);
        banner.Children.Add(Text(L("Первый рабочий этап — обучение движения", "First working stage — motion learning"), 20, true));
        banner.Children.Add(Text(L("Можно проверить оборудование, обучить модель на подготовленных наблюдениях и сравнить результат. Автономное обучение карты по демкам и установка знаний в мод ещё разрабатываются.",
            "Probe hardware, train on prepared observations and compare results. Autonomous map learning from demos and game package installation are still in development.")));
        banner.Children.Add(Button(L("Открыть обучение", "Open training"), () => Navigate("train"), true)); p.Children.Add(Card(banner));
        var ready = Stack(10); ready.Children.Add(Text(L("Готовность к работе", "Readiness"), 19, true));
        ready.Children.Add(Text((File.Exists(S.Python) ? "✓  " : "○  ") + L("Среда обучения", "Training runtime")));
        ready.Children.Add(Text((File.Exists(Path.Combine(S.WorkerDirectory, "opentdm_x_trainer", "studio.py")) ? "✓  " : "○  ") + L("Модуль анализа и обучения", "Analysis and learning worker")));
        ready.Children.Add(Text(_hardware.Length > 0 ? _hardware : L("Оборудование ещё не проверено", "Hardware has not been checked yet")));
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
        buttons.Children.Add(Button(L("Проверить оборудование", "Check hardware"), () => Start("hardware"), job: true));
        buttons.Children.Add(Button(L("Настройки", "Settings"), () => Navigate("settings")));
        ready.Children.Add(buttons); p.Children.Add(Card(ready));
        p.Children.Add(Text(L("Обучение выполняется на твоём компьютере. Данные не отправляются в облако.", "Training runs on your computer. Your data is not uploaded.")));
    }
    private ComboBox Choice(string[] values, string selected, Action<string> update)
    {
        var box = new ComboBox { ItemsSource = values, SelectedItem = selected, MinWidth = 190 };
        box.SelectionChanged += (_, _) => { if (box.SelectedItem is string v) { update(v); S.Save(); } };
        return box;
    }
    private void Training(StackPanel p)
    {
        Header(p, L("Обучение модели", "Train a model"), L("Новый опыт проверяется отдельно от данных, на которых модель училась.", "New learning is evaluated on data held out from training."));
        var card = Stack(12);
        card.Children.Add(Text(L("Подготовленные наблюдения", "Prepared observations"), 18, true));
        card.Children.Add(Text(L("Выбери папку с manifest.json и samples.npz. Прямой импорт DM2 / MVD2 появится на следующем этапе.", "Select a folder containing manifest.json and samples.npz. Direct DM2 / MVD2 ingestion is the next stage.")));
        card.Children.Add(PathRow(S.Dataset, value => { S.Dataset = value; S.Save(); }, false));
        card.Children.Add(Text(L("Папка модели и истории обучения", "Model and training history folder")));
        card.Children.Add(PathRow(S.Store, value => { S.Store = value; S.Save(); }, false));
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 14 };
        row.Children.Add(Choice(["auto", "cuda", "rocm", "xpu", "cpu"], S.Backend, v => S.Backend = v));
        row.Children.Add(Choice(["reference", "compact", "balanced", "large", "xl"], S.Profile, v => S.Profile = v));
        card.Children.Add(row);
        card.Children.Add(Text(L("Reference — проверочный эталон. Остальные модели — экспериментальные временные модели; большая модель не гарантирует лучший результат.",
            "Reference is the baseline. Other profiles are experimental temporal models; larger does not guarantee better.")));
        var commands = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
        commands.Children.Add(Button(L("Обучить новую", "Train new"), () => Start("train", "fresh"), true, true));
        commands.Children.Add(Button(L("Дообучить", "Continue learning"), () => Start("train", "update"), job: true));
        commands.Children.Add(Button(L("Отменить задание", "Cancel job"), () => _runner.Cancel()));
        card.Children.Add(commands); p.Children.Add(Card(card));
        p.Children.Add(Text(L("Прежнее поколение сохраняется. Результат этого этапа ещё нельзя устанавливать в игровой мод.", "Previous generations are preserved. This stage does not produce installable game knowledge.")));
    }
    private void Models(StackPanel p)
    {
        Header(p, L("Модели и оборудование", "Models and hardware"), L("Масштабирование по доступным ресурсам, без общего ограничения в 4 ГБ.", "Scale to available resources, without a universal 4 GB ceiling."));
        var c = Stack(12);
        c.Children.Add(Text(_hardware.Length > 0 ? _hardware : L("Нажми «Проверить оборудование» для определения GPU.", "Check hardware to detect your GPU.")));
        c.Children.Add(Button(L("Проверить оборудование", "Check hardware"), () => Start("hardware"), job: true));
        foreach (var (name, desc) in new[] { ("XL", L("32 ГБ: кандидат для расширенного контекста", "32 GB: extended-context candidate")),
            ("Large", L("24 ГБ: старший профиль", "24 GB: large profile")),
            ("Balanced", L("12–16 ГБ: средний профиль", "12–16 GB: medium profile")),
            ("Compact", L("Мало VRAM или CPU: компактный профиль", "Lower VRAM or CPU: compact profile")) })
            c.Children.Add(Text(name + "  ·  " + desc));
        c.Children.Add(Text(L("Это целевые классы, а не измеренные требования или обещание качества. Проверка ниже реально выполняет обучение выбранной модели на синтетическом примере.",
            "These are target classes, not measured requirements or quality guarantees. The probe actually trains the selected model on a synthetic example.")));
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
        row.Children.Add(Choice(["auto", "cuda", "rocm", "xpu", "cpu"], S.Backend, v => S.Backend = v));
        row.Children.Add(Choice(["reference", "compact", "balanced", "large", "xl"], S.Profile, v => S.Profile = v));
        row.Children.Add(Button(L("Проверить модель", "Probe model"), () => Start("probe"), true, true));
        c.Children.Add(row); p.Children.Add(Card(c));
    }
    private void Library(StackPanel p)
    {
        Header(p, L("Библиотека обучения", "Learning library"), L("Модель, история и проверенные результаты хранятся отдельно от игровых файлов.", "Models, history and verification results are separate from game files."));
        var c = Stack(12); c.Children.Add(Text(L("Текущая модель", "Current model"), 19, true)); c.Children.Add(Text(S.Store));
        c.Children.Add(Button(L("Проверить и открыть результат", "Verify and view result"), () => Start("status"), true, true));
        c.Children.Add(Text(L("Библиотека обученных карт и профилей игроков появится вместе с проверенным форматом данных для мода.", "The map and player-profile library will become available with the validated game-data format.")));
        p.Children.Add(Card(c));
    }
    private void Jobs(StackPanel p)
    {
        Header(p, L("Задания и результаты", "Jobs and results"), L("Ход работы, причины отказа и результаты проверок.", "Progress, rejection reasons and evaluation results."));
        p.Children.Add(Button(L("Отменить текущее задание", "Cancel current job"), () => _runner.Cancel()));
        if (_log.Parent is Panel previous) previous.Children.Remove(_log);
        p.Children.Add(_log);
        if (_result.Length > 0) p.Children.Add(Card(Text(_result)));
        p.Children.Add(Text(_runner.JobFolder ?? L("Пока нет заданий", "No jobs yet")));
    }
    private Control PathRow(string value, Action<string> update, bool file)
    {
        var grid = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto"), ColumnSpacing = 10 };
        var field = new TextBox { Text = value, MinWidth = 100 };
        field.TextChanged += (_, _) => update(field.Text ?? "");
        var browse = Button(L("Выбрать…", "Browse…"), async () =>
        {
            if (file)
            {
                var items = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions { AllowMultiple = false });
                if (items.Count > 0 && items[0].TryGetLocalPath() is { } path) field.Text = path;
            }
            else
            {
                var items = await StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions { AllowMultiple = false });
                if (items.Count > 0 && items[0].TryGetLocalPath() is { } path) field.Text = path;
            }
        });
        Grid.SetColumn(browse, 1); grid.Children.Add(field); grid.Children.Add(browse); return grid;
    }
    private void Settings(StackPanel p)
    {
        Header(p, L("Настройки", "Settings"), L("Внешний вид и локальная среда обучения", "Appearance and local training runtime"));
        var c = Stack(12);
        c.Children.Add(Text(L("Язык", "Language")));
        c.Children.Add(Choice(["ru", "en"], S.Language, v => { S.Language = v; Dispatcher.UIThread.Post(BuildShell); }));
        c.Children.Add(Text(L("Тема", "Theme")));
        c.Children.Add(Choice(["dark", "light", "system"], S.Theme, v => { S.Theme = v; App.ApplyTheme(); Dispatcher.UIThread.Post(BuildShell); }));
        c.Children.Add(Text(L("Python среды обучения", "Training environment Python")));
        c.Children.Add(PathRow(S.Python, v => { S.Python = v; S.Save(); }, true));
        c.Children.Add(Text(L("Папка worker", "Worker folder")));
        c.Children.Add(PathRow(S.WorkerDirectory, v => { S.WorkerDirectory = v; S.Save(); }, false));
        c.Children.Add(Text(L("В этой сборке среда выбирается вручную. Управляемая установка среды будет добавлена перед пользовательским выпуском.", "This build uses a manually selected environment. Managed setup is required before the end-user release.")));
        p.Children.Add(Card(c));
    }
    private async void Start(string action, string mode = "fresh")
    {
        if (_runner.IsRunning) return;
        _progress.Value = 0; _result = "";
        _status.Text = L("Запускаем задание…", "Starting job…");
        foreach (var b in _jobButtons) b.IsEnabled = false;
        try
        {
            var terminal = await _runner.RunAsync(S, new() { ["action"] = action, ["backend"] = S.Backend,
                ["profile"] = S.Profile, ["dataset"] = S.Dataset, ["store"] = S.Store, ["mode"] = mode, ["epochs"] = 20 });
            if (terminal.GetProperty("type").GetString() == "completed")
            {
                var result = terminal.GetProperty("result");
                _result = Summarize(action, result);
                if (action == "hardware") _hardware = string.Join("  ·  ", result.GetProperty("devices").EnumerateArray().Select(d => d.GetProperty("name").GetString()));
            }
        }
        catch (Exception ex) { _status.Text = L("Не удалось выполнить задание: ", "Job failed: ") + ex.Message; AppendLog(ex.Message); }
        finally
        {
            foreach (var b in _jobButtons) b.IsEnabled = true;
            ShowPage();
            if (_allowClose) Close();
        }
    }
    private void AppendLog(string text)
    {
        var lines = ((_log.Text ?? "") + text + "\n").Split('\n');
        _log.Text = string.Join('\n', lines.TakeLast(120));
    }
    private string Summarize(string action, JsonElement result)
    {
        if (action == "hardware")
            return string.Join("\n", result.GetProperty("devices").EnumerateArray().Select(d => d.GetProperty("name").GetString()))
                + "\n" + L("Предлагаемый профиль: ", "Suggested profile: ") + result.GetProperty("suggested_profile");
        if (action == "probe")
            return L("Модель: ", "Model: ") + result.GetProperty("profile") + "\n"
                + L("Параметров: ", "Parameters: ") + result.GetProperty("parameters").GetInt64().ToString("N0") + "\n"
                + L("Обновление весов выполнено: ", "Weights updated: ") + result.GetProperty("weights_changed") + "\n"
                + (result.TryGetProperty("peak_allocated_mib", out var memory) ? L("Память тензоров в этой проверке, МиБ: ", "Tensor memory in this probe, MiB: ") + memory.GetDouble().ToString("F1") + "\n" : "")
                + L("Это проверка вычислений. Качество игры здесь не измерялось.", "This is a computation check. Gameplay quality was not measured.");
        var manifest = action == "status" ? result.GetProperty("manifest") : result;
        var lines = new List<string> { L("Поколение: ", "Generation: ") + result.GetProperty("generation") };
        if (manifest.TryGetProperty("validation", out var validation))
            foreach (var entry in validation.EnumerateObject())
                lines.Add(entry.Name + " · " + L("ошибка движения: ", "motion error: ") + entry.Value.GetProperty("model").GetProperty("rmse").GetDouble().ToString("F2")
                    + L(" ед./с; продолжение текущего движения: ", " units/s; persistence baseline: ") + entry.Value.GetProperty("persistence").GetProperty("rmse").GetDouble().ToString("F2"));
        if (manifest.TryGetProperty("failures", out var failures) && failures.GetArrayLength() > 0)
            lines.Add(L("Причины отклонения: ", "Rejection reasons: ") + string.Join(", ", failures.EnumerateArray().Select(f => f.GetString())));
        lines.Add(L("Подробный результат сохранён в папке задания. Установка в мод пока недоступна.", "The full result is saved in the job folder. Game installation is not available yet."));
        return string.Join("\n", lines);
    }
    private void OnEvent(JsonElement e)
    {
        string? type = e.GetProperty("type").GetString();
        if (type == "progress")
        {
            if (e.TryGetProperty("progress", out var progress)) _progress.Value = progress.GetDouble() * 100;
            _status.Text = e.TryGetProperty("epoch", out var epoch)
                ? L("Обучение · эпоха ", "Training · epoch ") + epoch + " / " + e.GetProperty("epochs")
                : L("Подготовка и проверка…", "Preparing and checking…");
        }
        else if (type == "completed")
        {
            _progress.Value = 100;
            var result = e.GetProperty("result");
            bool rejected = result.TryGetProperty("accepted", out var accepted) && !accepted.GetBoolean();
            _status.Text = rejected ? L("Проверка отклонила модель. Предыдущая сохранена.", "Evaluation rejected the model. Previous model preserved.")
                : L("Задание завершено. Это ещё не подтверждение готовности к игре.", "Job completed. This is not gameplay qualification.");
        }
        else if (type is "failed" or "cancelled") _status.Text = (type == "cancelled" ? L("Отменено: ", "Cancelled: ") : L("Ошибка: ", "Error: ")) + e.GetProperty("message").GetString();
        AppendLog(e.ToString());
    }
    public static void RenderTests(string folder)
    {
        Directory.CreateDirectory(folder);
        foreach (var language in new[] { "ru", "en" })
        foreach (var theme in new[] { "dark", "light" })
        {
            App.Settings.Language = language; App.Settings.Theme = theme; App.ApplyTheme();
            var window = new MainWindow { ShowInTaskbar = false, ShowActivated = false,
                WindowStartupLocation = WindowStartupLocation.Manual, Position = new PixelPoint(-20000, -20000) };
            window.Show();
            foreach (var page in new[] { "home", "train", "models", "library", "jobs", "settings" })
            {
                window._page = page; window.BuildShell();
                var root = (Control)window.Content!;
                Dispatcher.UIThread.RunJobs();
                window.UpdateLayout();
                root.Measure(new Size(1180, 820)); root.Arrange(new Rect(0, 0, 1180, 820));
                using var image = new RenderTargetBitmap(new PixelSize(1180, 820), new Vector(96, 96));
                image.Render(root);
                using var output = File.Create(Path.Combine(folder, $"{language}-{theme}-{page}.png"));
                image.Save(output, new PngBitmapEncoderOptions());
            }
            window.Close();
        }
        File.WriteAllText(Path.Combine(folder, "ui-test.json"), "{\"pass\":true,\"views\":24}");
    }
}
