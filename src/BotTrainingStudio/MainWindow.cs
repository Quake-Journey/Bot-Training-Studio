using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform.Storage;
using Avalonia.Threading;
using Avalonia.Styling;
using System.Text.Json;
using System.Diagnostics;
using Avalonia.VisualTree;
using FluentAvalonia.UI.Controls;

namespace BotTrainingStudio;

public sealed partial class MainWindow : Window
{
    private readonly JobRunner _runner = new();
    private readonly ResourceSampler _resources;
    private readonly DispatcherTimer _resourceTimer;
    private readonly DispatcherTimer _eventTimer;
    private readonly JobEventBuffer _events = new();
    private LoadMeter? _meter;
    private readonly ContentControl _content = new();
    private readonly TextBlock _status = new() { TextWrapping = TextWrapping.Wrap };
    private readonly ProgressBar _progress = new() { Minimum = 0, Maximum = 100, Height = 5 };
    private readonly TextBox _log = new() { IsReadOnly = true, AcceptsReturn = true, TextWrapping = TextWrapping.Wrap, MinHeight = 180, MaxHeight = 270 };
    private readonly List<Button> _jobButtons = [];
    private string _page = "home";
    private string _hardware = "";
    private string _result = "";
    private bool _allowClose;
    private bool _exitPromptOpen;
    private bool _exitAfterJob;
    private bool _jobUiActive;
    private FAContentDialog? _exitDialog;
    private readonly Button _stay = new() { IsVisible = false };
    private bool JobActive => _runner.IsRunning || _jobUiActive || _setupCancellation != null || _updateCancellation != null;
    private StudioSettings S => App.Settings;
    private string L(string ru, string en) => S.EffectiveLanguage == "ru" ? ru : en;
    private static readonly IBrush Accent = new SolidColorBrush(Color.Parse("#8b83ff"));
    private bool Light => ActualThemeVariant == ThemeVariant.Light;

    public MainWindow(bool checkRuntime = true)
    {
        Title = "Bot Training Studio " + AppVersion.Current + " by ly";
        using (var icon = RuntimeSetup.Resource("studio.ico")) Icon = new WindowIcon(icon);
        _checkRuntime = checkRuntime;
        _resources = new ResourceSampler(() => _runner.WorkerPid);
        _resourceTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
        _resourceTimer.Tick += (_, _) => _meter?.Show(_resources.Latest);
        _eventTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(100) };
        _eventTimer.Tick += (_, _) => FlushEvents();
        _stay.Click += (_, _) => { _exitAfterJob = false; _stay.IsVisible = false; };
        Opened += async (_, _) =>
        {
            _resourceTimer.Start(); _eventTimer.Start();
            if (_checkRuntime)
            {
                try { await Task.Run(() => FactoryModels.Ensure()); }
                catch (Exception error) { AppendLog(L("Не удалось подготовить заводские модели: ", "Could not prepare factory models: ") + error.Message); }
                await ShowStartupCheckAsync(RefreshRuntimeAsync);
                if (!_lifetime.IsCancellationRequested) await FirstVersionStartAsync();
            }
        };
        Closed += (_, _) => { _lifetime.Cancel(); _startupDialog?.Hide(); _changesDialog?.Hide(); _resourceTimer.Stop(); _eventTimer.Stop(); _resources.Dispose(); };
        Width = 1180; Height = 820; MinWidth = 980; MinHeight = 700;
        WindowStartupLocation = WindowStartupLocation.CenterScreen;
        _runner.Received += _events.Receive;
        _runner.Diagnostic += _events.Log;
        Closing += OnClosing;
        BuildShell();
    }

    private async void OnClosing(object? sender, WindowClosingEventArgs e)
    {
        if (_allowClose || !JobActive && !_exitPromptOpen) return;
        e.Cancel = true;
        if (_exitPromptOpen || _exitAfterJob) return;
        _exitPromptOpen = true;
        try
        {
            _exitDialog = new FAContentDialog
            {
                Title = _updateCancellation != null ? L("Обновление ещё загружается", "Update download is in progress") : _setupCancellation != null ? L("Среда ещё устанавливается", "Runtime installation is in progress") : L("Задание ещё выполняется", "A job is still running"),
                Content = _updateCancellation != null ? L("Отменить загрузку и выйти? Установленная программа останется прежней.", "Cancel the download and quit? Your installed application will stay unchanged.") : _setupCancellation != null ? L("Остановить установку и выйти? Незавершённые файлы установки будут удалены. Прежняя рабочая среда сохранится.", "Stop installation and quit? Incomplete setup files will be removed. Your previous working runtime will be kept.") : L("Остановить задание и выйти? Программа дождётся безопасной остановки. Прежняя модель и завершённые контрольные точки сохранятся; незавершённая часть текущего шага может быть потеряна.",
                    "Stop the job and quit? The application will wait for a safe stop. The previous model and completed checkpoints will be kept; unfinished work in the current step may be lost."),
                PrimaryButtonText = L("Остановить и выйти", "Stop and quit"),
                CloseButtonText = L("Продолжить работу", "Keep working"),
                DefaultButton = FAContentDialogButton.Close
            };
            if (await _exitDialog.ShowAsync(this) != FAContentDialogResult.Primary) return;
            _exitAfterJob = true;
            _stay.IsVisible = true;
            _status.Text = L("Останавливаем задание… Окно закроется после остановки. Интерфейс доступен.",
                "Stopping the job… The window will close once it stops. You can still use the interface.");
            bool requested = await RequestStopAsync();
            if (!requested && _runner.IsRunning)
            {
                _exitAfterJob = false; _stay.IsVisible = false;
                _status.Text = L("Не удалось запросить остановку. Задание продолжает работать; подробности в журнале.",
                    "Could not request a stop. The job is still running; see the log for details.");
            }
            if (_exitAfterJob && !JobActive) { _allowClose = true; Close(); }
        }
        catch (Exception ex)
        {
            _exitAfterJob = false; _stay.IsVisible = false;
            _status.Text = L("Не удалось закрыть программу: ", "Could not close the application: ") + ex.Message;
        }
        finally
        {
            _exitPromptOpen = false; _exitDialog = null;
            foreach (var b in _jobButtons) b.IsEnabled = !JobActive && !_exitAfterJob;
        }
    }

    private void FlushEvents()
    {
        foreach (var choice in this.GetVisualDescendants().OfType<ComboBox>().Where(c => c.Name == "ComputeChoice"))
            choice.IsEnabled = !JobActive && !_exitAfterJob;
        if (_setupCancellation != null && Volatile.Read(ref _setupProgress) is { } progress) ShowSetupProgress(progress);
        if (_updateCancellation != null && Volatile.Read(ref _updateProgress) is { } update) { _status.Text = update; }
        var (message, lines) = _events.Drain();
        if (message is { } item) OnEvent(item);
        if (lines.Length > 0) AppendLog(string.Join('\n', lines));
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
        Tip(b, Help(text));
        b.Click += (_, _) => action();
        if (job) { b.IsEnabled = !JobActive && !_exitPromptOpen && !_exitAfterJob; _jobButtons.Add(b); }
        return b;
    }
    private void BuildShell()
    {
        foreach (var control in new Control[] { _content, _status, _progress, _stay })
            if (control.Parent is Panel parent) parent.Children.Remove(control);
        Content = null;
        _jobButtons.Clear();
        var shell = new Grid { ColumnDefinitions = new ColumnDefinitions("220,*"),
            Background = new SolidColorBrush(Color.Parse(Light ? "#f5f5fa" : "#1b1a22")) };
        var sidebar = new StackPanel { Spacing = 8, Margin = new Thickness(16, 26) };
        sidebar.Children.Add(Text("BOT TRAINING\nSTUDIO", 18, true));
        sidebar.Children.Add(new Border { Height = 12 });
        var pages = new[] { ("home", L("Главная", "Home")), ("project", L("Карта и демки", "Map & demos")), ("train", L("Обучение", "Training")),
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
        sidebar.Children.Add(Text(AppVersion.Current + L(" · предварительная версия", " · development preview"), 11));
        sidebar.Children.Add(Button(L("Что нового", "What's new"), async () => await ShowChangesAsync()));
        sidebar.Children.Add(Button(L("Обновить", "Update"), async () => await CheckUpdatesAsync(true)));
        shell.Children.Add(new Border { Background = new SolidColorBrush(Color.Parse(Light ? "#ebebf3" : "#211f2b")), Child = sidebar });
        var outer = new DockPanel();
        var bottom = new StackPanel { Spacing = 8, Margin = new Thickness(32, 8, 32, 18) };
        bottom.Children.Add(_status); bottom.Children.Add(_progress);
        _stay.Content = L("Остаться в программе после остановки", "Stay in the application after stopping");
        Tip(_log,L("Журнал текущего задания: последние сообщения и причины ошибок. Полный журнал хранится в папке задания.",
            "Current job log: recent messages and failure details. The complete log is stored in the job folder."));
        bottom.Children.Add(_stay);
        string? selectedGpu = _meter?.SelectedGpu;
        _meter = new LoadMeter(L, Light);
        _meter.Show(_resources.Latest, selectedGpu);
        bottom.Children.Add(_meter);
        DockPanel.SetDock(bottom, Dock.Bottom); outer.Children.Add(bottom); outer.Children.Add(_content);
        Grid.SetColumn(outer, 1); shell.Children.Add(outer);
        Content = shell;
        ShowPage();
        ApplyHelp(shell);
    }
    private void Navigate(string page) { _page = page; BuildShell(); }
    private void ShowPage()
    {
        _jobButtons.Clear();
        var panel = Stack(18); panel.Margin = new Thickness(34, 26, 34, 20);
        switch (_page)
        {
            case "train": Training(panel); break;
            case "project": Project(panel); break;
            case "models": Models(panel); break;
            case "library": Library(panel); break;
            case "jobs": Jobs(panel); break;
            case "settings": Settings(panel); break;
            default: Home(panel); break;
        }
        _content.Content = new ScrollViewer { Content = panel, HorizontalScrollBarVisibility = Avalonia.Controls.Primitives.ScrollBarVisibility.Disabled };
        ApplyHelp(panel);
    }
    private void Header(StackPanel p, string title, string subtitle)
    {
        p.Children.Add(Text(title, 29, true)); p.Children.Add(Text(subtitle, 15));
    }
    private void Home(StackPanel p)
    {
        Header(p, L("Опыт игроков. Новые возможности ботов.", "Player experience. New bot capabilities."),
            L("Локальная студия обучения для OpenTDM-X", "Local learning studio for OpenTDM-X"));
        p.Children.Add(ComputeCard());
        var banner = Stack(10);
        var tag = Text(L("ПРЕДВАРИТЕЛЬНАЯ ВЕРСИЯ  ·  ", "DEVELOPMENT PREVIEW  ·  ") + AppVersion.Current, 12, true); tag.Foreground = Accent; banner.Children.Add(tag);
        banner.Children.Add(Text(L("От записей игры к проверяемому опыту", "From recordings to verifiable experience"), 20, true));
        banner.Children.Add(Text(L("Выбери BSP и демки, изучи маршруты и стиль игрока, обучи модель по истории матчей. Студия сохраняет поколения и проверяет новый опыт. Пакеты пока предназначены для офлайн-проверки; качество игры в моде ещё не подтверждено.",
            "Select a BSP and recordings, analyze routes and player style, and train on match histories. The Studio preserves generations and evaluates new learning. Packages are for offline review; mod gameplay is not yet qualified.")));
        banner.Children.Add(Button(L("Открыть обучение", "Open training"), () => Navigate("train"), true)); p.Children.Add(Card(banner));
        var ready = Stack(10); ready.Children.Add(Text(L("Готовность к работе", "Readiness"), 19, true));
        ready.Children.Add(Text(RuntimeStatus()));
        ready.Children.Add(Button(L("Настройка Python", "Python setup"), () => Navigate("settings")));
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
        if (values.Contains("compact")) Tip(box,L("Размер архитектуры модели: compact, balanced, large или xl. Это разные совместимые семейства весов, а не предел используемой VRAM. Заводские веса доступны для compact и balanced.",
            "Model architecture: compact, balanced, large or xl. These are separate weight families, not a VRAM ceiling. Factory weights are available for compact and balanced."));
        box.SelectionChanged += (_, _) => { if (box.SelectedItem is string v && v != selected) { selected = v; update(v); S.Save(); } };
        return box;
    }
    private static void WhenEdited(TextBox field, Action<string> update)
    {
        string previous = field.Text ?? "";
        field.TextChanged += (_, _) =>
        {
            string value = field.Text ?? "";
            if (value == previous) return;
            previous = value; update(value);
        };
    }
    private void Training(StackPanel p)
    {
        Header(p, L("Обучение по истории игры", "Learn from gameplay history"),
            L("Движение, оружие, изменения стека и исходы эпизодов. Проверка на отдельных матчах.", "Movement, weapons, resource changes and episode outcomes. Independent match validation."));
        var sequence = Stack(12);
        sequence.Children.Add(Text(L("Проекты карт — по одному пути в строке", "Map projects — one folder per line")));
        var projects = new TextBox { AcceptsReturn = true, MinHeight = 70, Text = string.Join("\n", S.Projects) };
        WhenEdited(projects, value => { S.Projects = value.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries); S.Save(); });
        sequence.Children.Add(projects);
        sequence.Children.Add(Button(L("Добавить текущую карту", "Add current map"), () => { S.Projects = S.Projects.Append(S.Project).Distinct().ToArray(); S.Save(); ShowPage(); }));
        sequence.Children.Add(Text(L("История наблюдений (кадров)", "Observed history (frames)")));
        sequence.Children.Add(Choice(["4", "8", "16", "32", "64", "128"], S.Context.ToString(), v => S.Context = int.Parse(v)));
        sequence.Children.Add(Text(L("Максимум примеров в каждой выборке", "Maximum examples per dataset partition")));
        sequence.Children.Add(Choice(["2000", "6000", "12000", "24000"], S.SampleLimit.ToString(), v => S.SampleLimit = int.Parse(v)));
        sequence.Children.Add(Button(L("Подготовить данные проектов", "Prepare project data"), () => Start("prepare_sequences"), true, true));
        sequence.Children.Add(Text(L("Подготовленный набор", "Prepared dataset")));
        sequence.Children.Add(PathRow(S.SequenceDataset, v => { S.SequenceDataset = v; S.Save(); }, false));
        sequence.Children.Add(Text(L("Модель и контрольные точки", "Model and checkpoints")));
        sequence.Children.Add(PathRow(S.TemporalStore, v => { S.TemporalStore = v; S.Save(); }, false));
        var modelrow = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 12 };
        modelrow.Children.Add(ComputeChoice());
        modelrow.Children.Add(Choice(["compact", "balanced", "large", "xl"], S.Profile == "reference" ? "compact" : S.Profile, v => S.Profile = v));
        sequence.Children.Add(modelrow);
        sequence.Children.Add(Button(L("Подобрать размер пакета по памяти и скорости", "Measure batch size for memory and throughput"), () => Start("calibrate"), job: true));
        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        actions.Children.Add(Button(L("Обучить новое поколение", "Train new generation"), () => Start("train_sequences", "fresh"), true, true));
        sequence.Children.Add(Button(L("Начать от заводской модели", "Start from factory model"), () => Start("train_sequences", "factory"), job: true));
        actions.Children.Add(Button(L("Дообучить", "Update"), () => Start("train_sequences", "update"), job: true));
        actions.Children.Add(Button(L("Возобновить", "Resume"), () => Start("train_sequences", "resume"), job: true));
        sequence.Children.Add(actions);
        sequence.Children.Add(Text(L("Отмена сохраняет завершённую эпоху. Новое поколение не стирает предыдущее. Подготовка требует независимых матчей для обучения и проверки.",
            "Cancellation preserves the completed epoch. New generations preserve previous ones. Preparation requires independent training and evaluation matches.")));
        sequence.Children.Add(Text(L("Качество каждого прогноза проверяется отдельно. Для проектов прежней версии повтори импорт и подготовку данных; старую модель сохрани в отдельной папке.",
            "Each prediction is evaluated separately. Re-import older projects and prepare their data again; preserve the old model in a separate folder.")));
        p.Children.Add(Card(sequence));
        var decisions = Stack(12);
        decisions.Children.Add(Text(L("Выстрелы и подборы предметов", "Shots and item pickups"), 20, true));
        decisions.Children.Add(Text(L("Отдельное обучение по фактическим выстрелам и сообщениям о подборе. Использует проекты, историю и профиль выше. Старые проекты сначала импортируй повторно.",
            "Separate learning from observed shots and pickup messages. Uses the projects, history and profile above. Re-import older projects first.")));
        decisions.Children.Add(Button(L("Подготовить события игры", "Prepare gameplay events"), () => Start("prepare_decisions"), true, true));
        decisions.Children.Add(Text(L("Набор событий", "Event dataset")));
        decisions.Children.Add(PathRow(S.DecisionDataset, v => { S.DecisionDataset = v; S.Save(); }, false));
        decisions.Children.Add(Text(L("Отдельная папка модели решений", "Separate decision model folder")));
        decisions.Children.Add(PathRow(S.DecisionStore, v => { S.DecisionStore = v; S.Save(); }, false));
        decisions.Children.Add(Button(L("Подобрать пакет для модели решений", "Measure decision model batch size"), () => Start("calibrate_decisions"), job: true));
        var decisionActions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        decisionActions.Children.Add(Button(L("Обучить", "Train"), () => Start("train_decisions", "fresh"), true, true));
        decisionActions.Children.Add(Button(L("Дообучить", "Learn new data"), () => Start("train_decisions", "update"), job: true));
        decisionActions.Children.Add(Button(L("Возобновить", "Resume"), () => Start("train_decisions", "resume"), job: true));
        decisions.Children.Add(decisionActions);
        decisions.Children.Add(Text(L("Эксперимент: предсказывает действия из записей. Дообучение повторяет прежний опыт и отдельно проверяет его сохранение. Результат пока не устанавливается в мод. Для обучения с нуля выбери новую папку модели.",
            "Experiment: predicts recorded actions. Learning new data replays prior experience and checks retention separately. Results cannot yet be installed in the mod. Choose a new model folder to train from scratch.")));
        p.Children.Add(Card(decisions));
    }
    private void LegacyTraining(StackPanel p)
    {
        Header(p, L("Обучение модели", "Train a model"), L("Новый опыт проверяется отдельно от данных, на которых модель училась.", "New learning is evaluated on data held out from training."));
        var card = Stack(12);
        card.Children.Add(Text(L("Подготовленные наблюдения", "Prepared observations"), 18, true));
        card.Children.Add(Text(L("Выбери папку с manifest.json и samples.npz. Прямой импорт DM2 / MVD2 появится на следующем этапе.", "Select a folder containing manifest.json and samples.npz. Direct DM2 / MVD2 ingestion is the next stage.")));
        card.Children.Add(PathRow(S.Dataset, value => { S.Dataset = value; S.Save(); }, false));
        card.Children.Add(Text(L("Папка модели и истории обучения", "Model and training history folder")));
        card.Children.Add(PathRow(S.Store, value => { S.Store = value; S.Save(); }, false));
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 14 };
        row.Children.Add(ComputeChoice());
        row.Children.Add(Choice(["reference", "compact", "balanced", "large", "xl"], S.Profile, v => S.Profile = v));
        card.Children.Add(row);
        card.Children.Add(Text(L("Reference — проверочный эталон. Остальные модели — экспериментальные временные модели; большая модель не гарантирует лучший результат.",
            "Reference is the baseline. Other profiles are experimental temporal models; larger does not guarantee better.")));
        var commands = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
        commands.Children.Add(Button(L("Обучить новую", "Train new"), () => Start("train", "fresh"), true, true));
        commands.Children.Add(Button(L("Дообучить", "Continue learning"), () => Start("train", "update"), job: true));
        commands.Children.Add(Button(L("Отменить задание", "Cancel job"), async () => await RequestStopAsync()));
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
        row.Children.Add(ComputeChoice());
        row.Children.Add(Choice(["reference", "compact", "balanced", "large", "xl"], S.Profile, v => S.Profile = v));
        row.Children.Add(Button(L("Проверить модель", "Probe model"), () => Start("probe"), true, true));
        c.Children.Add(row); p.Children.Add(Card(c));
    }
    private void Library(StackPanel p)
    {
        Header(p, L("Библиотека обучения", "Learning library"), L("Модель, история и проверенные результаты хранятся отдельно от игровых файлов.", "Models, history and verification results are separate from game files."));
        var factory = Stack(12);
        factory.Children.Add(Text(L("Заводские модели", "Factory models"),19,true));
        factory.Children.Add(Text(FactoryModels.DirectoryPath));
        factory.Children.Add(Text(FactoryModels.Summary(S.EffectiveLanguage)));
        factory.Children.Add(Button(L("Проверить заводские модели", "Verify factory models"), () => Start("factory_status"), job:true));
        factory.Children.Add(Text(L("Комплектные веса для наблюдательных экспериментов на q2duel5 и ztn2dm3. Полноценная игровая тактика ещё не готова. Обучение не меняет эту основу.",
            "Bundled weights for observation experiments on q2duel5 and ztn2dm3. Full gameplay tactics are not ready. Training does not modify this base.")));
        p.Children.Add(Card(factory));
        var c = Stack(12); c.Children.Add(Text(L("Пользовательская модель и дополнения", "User model and overlays"), 19, true)); c.Children.Add(Text(S.TemporalStore));
        c.Children.Add(Text(File.Exists(Path.Combine(S.TemporalStore,"active.json"))
            ? L("Есть сохранённое поколение. Проверь его целостность и результаты кнопкой ниже.","A saved generation exists. Verify its integrity and results below.")
            : L("Принятого пользовательского поколения в этой папке пока нет.","No accepted user generation exists in this folder yet.")));
        var viewResult = Button(L("Проверить и открыть результат", "Verify and view result"), () => Start("sequence_status"), true, true);
        viewResult.IsEnabled &= File.Exists(Path.Combine(S.TemporalStore,"active.json")); c.Children.Add(viewResult);
        c.Children.Add(Text(L("Экспериментальные поколения моделей и пакеты карты сохраняются отдельно от файлов мода.", "Experimental model generations and map packages are separate from mod files.")));
        p.Children.Add(Card(c));
        var pack = Stack(12);
        pack.Children.Add(Text(L("Пакет данных карты", "Map data package"), 19, true));
        pack.Children.Add(Text(L("Результат анализа", "Analysis result")));
        pack.Children.Add(PathRow(S.Knowledge, v => { S.Knowledge = v; S.Save(); }, true));
        var includeModel = new CheckBox { Content = L("Добавить обученную модель (карта и донор должны совпадать)", "Include the learned model (map and donor must match)"), IsChecked = S.IncludeModel };
        includeModel.IsCheckedChanged += (_, _) => { S.IncludeModel = includeModel.IsChecked == true; S.Save(); };
        pack.Children.Add(includeModel);
        var includeChat = new CheckBox { Content = L("Включить фразы игрока в пакет", "Include player phrases in the package"), IsChecked = S.IncludeChat };
        includeChat.IsCheckedChanged += (_, _) => { S.IncludeChat = includeChat.IsChecked == true; S.Save(); };
        pack.Children.Add(includeChat);
        pack.Children.Add(Button(L("Собрать пакет карты", "Compile map package"), () => Start("compile_package"), true, true));
        pack.Children.Add(Text(L("Файл пакета", "Package file")));
        pack.Children.Add(PathRow(S.Package, v => { S.Package = v; S.Save(); }, true));
        pack.Children.Add(Button(L("Проверить пакет", "Verify package"), () => Start("verify_package"), job: true));
        pack.Children.Add(Button(L("Добавить в библиотеку студии", "Add to Studio library"), () => Start("install_offline"), job: true));
        pack.Children.Add(Text(L("Пока это офлайн-пакет для проверки. Установка на сервер станет доступна после согласования загрузчика мода.",
            "This is an offline candidate package. Server installation requires the coordinated mod loader.")));
        p.Children.Add(Card(pack));
    }
    private void Project(StackPanel p)
    {
        Header(p, L("Карта и записи игр", "Map and recordings"), L("Исходные файлы остаются на месте. Анализ и история хранятся в проекте.", "Original files stay in place. Analysis and history belong to the project."));
        var card = Stack(12);
        card.Children.Add(Text(L("Карта BSP", "BSP map")));
        card.Children.Add(PathRow(S.Bsp, v => { S.Bsp = v; S.Save(); }, true));
        card.Children.Add(Text(L("Папка проекта", "Project folder")));
        card.Children.Add(PathRow(S.Project, v => { S.Project = v; S.Save(); }, false));
        card.Children.Add(Text(L("DM2 / MVD2 или ZIP / RAR", "DM2 / MVD2 or ZIP / RAR")));
        var inputs = new TextBox { AcceptsReturn = true, MinHeight = 80, Text = string.Join("\n", S.Inputs) };
        WhenEdited(inputs, value => { S.Inputs = value.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries); S.Save(); });
        card.Children.Add(inputs);
        card.Children.Add(Text(L("Отдельные записи триксов (по одному пути в строке)", "Separate trick recordings (one path per line)")));
        var tricks = new TextBox { AcceptsReturn = true, MinHeight = 55, Text = string.Join("\n", S.Tricks) };
        WhenEdited(tricks, value => { S.Tricks = value.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries); S.Save(); });
        card.Children.Add(tricks);
        card.Children.Add(Button(L("Добавить записи…", "Add recordings…"), async () =>
        {
            var files = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions { AllowMultiple = true,
                FileTypeFilter = [new FilePickerFileType("Quake recordings") { Patterns = ["*.dm2", "*.mvd2", "*.zip", "*.rar"] }] });
            S.Inputs = S.Inputs.Concat(files.Select(f => f.TryGetLocalPath()).Where(v => v != null).Cast<string>()).Distinct().ToArray(); S.Save(); ShowPage();
        }));
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        row.Children.Add(Button(L("Проверить список", "Inspect inputs"), () => Start("inventory"), job: true));
        row.Children.Add(Button(L("Импорт / дополнение", "Import / update"), () => Start("import_project", "update"), true, true));
        row.Children.Add(Button(L("Новая выборка", "Fresh selection"), () => Start("import_project", "fresh"), job: true));
        card.Children.Add(row);
        card.Children.Add(Text(L("Донор стиля (пусто — все игроки)", "Style donor (empty — all players)")));
        var donor = new TextBox { Text = S.Donor };
        WhenEdited(donor, value => { S.Donor = value; S.Save(); });
        card.Children.Add(donor);
        card.Children.Add(Button(L("Изучить маршруты и стиль", "Analyze routes and style"), () => Start("analyze_project"), job: true));
        card.Children.Add(Button(L("Проверить движения через физику", "Verify motion through physics"), () => Start("fit_movement"), job: true));
        p.Children.Add(Card(card));
    }
    private void Jobs(StackPanel p)
    {
        Header(p, L("Задания и результаты", "Jobs and results"), L("Ход работы, причины отказа и результаты проверок.", "Progress, rejection reasons and evaluation results."));
        p.Children.Add(Button(L("Отменить текущее задание", "Cancel current job"), async () => await RequestStopAsync()));
        if (_log.Parent is Panel previous) previous.Children.Remove(_log);
        p.Children.Add(_log);
        if (_result.Length > 0) p.Children.Add(Card(Text(_result)));
        p.Children.Add(Text(_runner.JobFolder ?? L("Пока нет заданий", "No jobs yet")));
    }
    private Control PathRow(string value, Action<string> update, bool file)
    {
        var grid = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto"), ColumnSpacing = 10 };
        var field = new TextBox { Text = value, MinWidth = 100 };
        WhenEdited(field, update);
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
        var options = new[]
        {
            new ComboBoxItem { Content = L("Как в системе", "Use system language"), Tag = "system" },
            new ComboBoxItem { Content = "Русский", Tag = "ru" },
            new ComboBoxItem { Content = "English", Tag = "en" }
        };
        var language = new ComboBox { Name = "LanguageChoice", ItemsSource = options, MinWidth = 240,
            SelectedItem = options.First(o => (string)o.Tag! == S.Language) };
        language.SelectionChanged += (_, _) =>
        {
            if (language.SelectedItem is not ComboBoxItem { Tag: string value }) return;
            if (S.Language == value) return;
            S.Language = value; S.Save(); App.ApplyLanguage(); Dispatcher.UIThread.Post(BuildShell);
        };
        c.Children.Add(language);
        c.Children.Add(Text(L("По умолчанию: русский для русской системы, английский для остальных. Выбор сохраняется.",
            "Default: Russian for a Russian system, English otherwise. Your selection is saved."), 13));
        c.Children.Add(Text(L("Тема", "Theme")));
        c.Children.Add(Choice(["dark", "light", "system"], S.Theme, v => { S.Theme = v; App.ApplyTheme(); Dispatcher.UIThread.Post(BuildShell); }));
        p.Children.Add(Card(c));
        p.Children.Add(UpdateCard());
        p.Children.Add(ComputeCard());
        p.Children.Add(RuntimeCard());
        c = Stack(12);
        c.Children.Add(Text(L("Папка worker", "Worker folder")));
        c.Children.Add(PathRow(S.WorkerDirectory, v => { S.WorkerDirectory = v; S.Save(); }, false));
        c.Children.Add(Text(L("В комплектной сборке среда определяется автоматически. Эти пути нужны для выбора другой установленной среды обучения.",
            "Bundled builds detect their runtime automatically. These paths let you select another installed training runtime.")));
        p.Children.Add(Card(c));
        var docs = Stack(10);
        docs.Children.Add(Text(L("Документация", "Documentation"), 18, true));
        docs.Children.Add(Button("Руководство на русском (DOCX)", () => OpenGuide("RU")));
        docs.Children.Add(Button("User guide in English (DOCX)", () => OpenGuide("EN")));
        p.Children.Add(Card(docs));
    }
    private void OpenGuide(string language)
    {
        try
        {
            var path = Path.Combine(AppContext.BaseDirectory, "docs", $"Bot_Training_Studio_User_Guide_{language}.docx");
            if (!File.Exists(path)) throw new FileNotFoundException(L("Руководство отсутствует в комплекте программы.", "The guide is missing from the application package."));
            Process.Start(new ProcessStartInfo(path) { UseShellExecute = true });
        }
        catch (Exception ex) { _status.Text = L("Не удалось открыть руководство: ", "Could not open the guide: ") + ex.Message; }
    }
    private async void Start(string action, string mode = "fresh")
    {
        if (JobActive || _exitPromptOpen || _exitAfterJob) return;
        _jobUiActive = true;
        bool safeToClose = false;
        _progress.Value = 0; _result = "";
        _status.Text = L("Запускаем задание…", "Starting job…");
        foreach (var b in _jobButtons) b.IsEnabled = false;
        try
        {
            if (action is "train" or "train_sequences" or "train_decisions" or "import_project")
            {
                string destination = action == "import_project" ? S.Project : action == "train_sequences" ? S.TemporalStore : action == "train_decisions" ? S.DecisionStore : S.Store;
                if (FactoryModels.IsApplicationPath(destination)) throw new InvalidOperationException(L("Выбери пользовательскую папку вне файлов программы. Заводские Models изменять нельзя.",
                    "Select a user folder outside application files. Factory Models cannot be modified."));
            }
            if (_checkRuntime && (_runtimeProbe?.Ready != true || _runtimeProbePath != S.Python))
            {
                await RefreshRuntimeAsync();
                if (_runtimeProbe?.Ready != true) { Navigate("settings"); _status.Text = L("Подготовь среду обучения кнопкой установки или выбери готовую среду.", "Install the training runtime or select a ready environment."); return; }
            }
            var request = new Dictionary<string, object?> { ["action"] = action, ["backend"] = S.Backend,
                ["profile"] = S.Profile, ["dataset"] = S.Dataset, ["store"] = S.Store, ["mode"] = mode, ["epochs"] = 20,
                ["bsp"] = S.Bsp, ["inputs"] = S.Inputs, ["project"] = S.Project, ["projects"] = S.Projects,
                ["tricks"] = S.Tricks, ["include_chat"] = S.IncludeChat,
                ["batch_size"] = S.BatchSize,
                ["donor"] = S.Donor, ["context"] = S.Context, ["limit"] = S.SampleLimit, ["knowledge"] = S.Knowledge, ["package"] = S.Package,
                ["library"] = Path.Combine(StudioSettings.Home, "library") };
            request["factory_catalog"] = FactoryModels.DirectoryPath;
            if (action is "prepare_sequences" or "prepare_decisions") request["dataset"] = Path.Combine(StudioSettings.Home, "datasets", Guid.NewGuid().ToString("N"));
            if (action is "train_sequences" or "sequence_status" or "calibrate") { request["dataset"] = S.SequenceDataset; request["store"] = S.TemporalStore; request["profile"] = S.Profile == "reference" ? "compact" : S.Profile; }
            if (action is "train_decisions" or "calibrate_decisions") { request["dataset"] = S.DecisionDataset; request["store"] = S.DecisionStore; request["profile"] = S.Profile == "reference" ? "compact" : S.Profile; request["batch_size"] = S.DecisionBatchSize; }
            if (action == "compile_package") { request["output"] = Path.Combine(StudioSettings.Home, "exports", Guid.NewGuid().ToString("N") + ".btsknowledge"); if (S.IncludeModel) request["model_store"] = S.TemporalStore; }
            var terminal = await _runner.RunAsync(S, request);
            safeToClose = terminal.GetProperty("type").GetString() is "completed" or "cancelled";
            if (terminal.GetProperty("type").GetString() == "completed")
            {
                var result = terminal.GetProperty("result");
                if (action == "prepare_sequences") S.SequenceDataset = result.GetProperty("dataset").GetString()!;
                if (action == "prepare_decisions") S.DecisionDataset = result.GetProperty("dataset").GetString()!;
                if (action == "analyze_project") S.Knowledge = result.GetProperty("path").GetString()!;
                if (action == "compile_package") S.Package = result.GetProperty("package").GetString()!;
                if (action == "calibrate") S.BatchSize = result.GetProperty("batch_size").GetInt32();
                if (action == "calibrate_decisions") S.DecisionBatchSize = result.GetProperty("batch_size").GetInt32();
                S.Save();
                _result = await Task.Run(() => Summarize(action, result));
                if (_result.Length > 20000) _result = _result[..20000] + L("\n…Полный результат — в папке задания.", "\n…Full result is in the job folder.");
                if (action == "hardware") _hardware = string.Join("  ·  ", result.GetProperty("devices").EnumerateArray().Select(d => d.GetProperty("name").GetString()));
            }
        }
        catch (Exception ex)
        {
            safeToClose = false; FlushEvents();
            _status.Text = L("Не удалось выполнить задание: ", "Job failed: ") + ex.Message; AppendLog(ex.Message);
        }
        finally
        {
            _jobUiActive = false;
            FlushEvents();
            if (_exitAfterJob && !safeToClose) { _exitAfterJob = false; _stay.IsVisible = false; }
            foreach (var b in _jobButtons) b.IsEnabled = true;
            ShowPage();
            if (_exitAfterJob) { _allowClose = true; Close(); }
        }
    }
    private void AppendLog(string text)
    {
        var lines = ((_log.Text ?? "") + text + "\n").Split('\n');
        _log.Text = string.Join('\n', lines.TakeLast(120));
    }
    private string Summarize(string action, JsonElement result)
    {
        if (action == "train_decisions")
        {
            var decisionLines = new List<string> { L("Обучение завершено. Проверка на отдельных матчах:", "Training completed. Evaluation on held-out matches:") };
            foreach (var map in result.GetProperty("test").EnumerateObject())
            {
                if (map.Name == "all") continue;
                decisionLines.Add(map.Name);
                var weapon = map.Value.GetProperty("weapon");
                var pickup = map.Value.GetProperty("pickup");
                if (weapon.GetProperty("samples").GetInt32() > 0)
                {
                    decisionLines.Add(L("  Оружие следующего выстрела: ", "  Next-shot weapon: ") + weapon.GetProperty("accuracy").GetDouble().ToString("P1"));
                    if (weapon.TryGetProperty("persistence_accuracy", out var baseline) && baseline.ValueKind == JsonValueKind.Number)
                        decisionLines.Add(L("  Если всегда оставлять текущее оружие: ", "  Always keeping the current weapon: ") + baseline.GetDouble().ToString("P1"));
                    if (weapon.TryGetProperty("changed_shot_accuracy", out var changed) && changed.ValueKind == JsonValueKind.Number)
                        decisionLines.Add(L("  Верные решения при смене оружия: ", "  Correct actual weapon switches: ") + changed.GetDouble().ToString("P1"));
                }
                if (pickup.GetProperty("samples").GetInt32() > 0)
                {
                    decisionLines.Add(L("  Следующий наблюдаемый подбор: ", "  Next observed pickup: ") + pickup.GetProperty("accuracy").GetDouble().ToString("P1"));
                    if (pickup.TryGetProperty("observed_edge_accuracy", out var edges) && edges.ValueKind == JsonValueKind.Number)
                        decisionLines.Add(L("  Тип предмета при фактическом подборе: ", "  Item type on observed pickups: ") + edges.GetDouble().ToString("P1"));
                }
            }
            var qualified = result.GetProperty("qualified_observation_heads");
            decisionLines.Add(L("Проверка выбора оружия: ", "Weapon quality gate: ") + (qualified.GetProperty("weapon").GetBoolean() ? L("пройдена", "passed") : L("не пройдена", "not passed")));
            decisionLines.Add(L("Проверка подбора: ", "Pickup quality gate: ") + (qualified.GetProperty("pickup").GetBoolean() ? L("пройдена", "passed") : L("не пройдена", "not passed")));
            if (result.TryGetProperty("retention", out var retention) && retention.ValueKind == JsonValueKind.Object)
                foreach (var map in retention.GetProperty("test").EnumerateObject())
                    foreach (var head in map.Value.EnumerateObject())
                        decisionLines.Add(map.Name + " · " + head.Name + " · " + L("сохранение прежнего опыта: ", "prior experience retention: ") +
                            (head.Value.GetProperty("retained").GetBoolean() ? L("пройдено", "passed") : L("обнаружено ухудшение", "regression detected")));
            decisionLines.Add(L("Это прогноз действий из записей, не проверка победы бота. Подробности сохранены в папке задания. Установка в мод пока недоступна.",
                "This predicts recorded actions, not bot victories. Details are saved in the job folder. Game installation is not available yet."));
            return string.Join("\n", decisionLines);
        }
        if (action is "factory_status" or "import_project" or "analyze_project" or "prepare_sequences" or "prepare_decisions" or "train_decisions" or "compile_package" or "verify_package" or "install_offline" or "train_sequences" or "inventory" or "sequence_status" or "fit_movement" or "calibrate" or "calibrate_decisions")
            return JsonSerializer.Serialize(result, new JsonSerializerOptions { WriteIndented = true });
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
            _status.Text = _exitAfterJob ? L("Останавливаем задание… Окно закроется после остановки.", "Stopping the job… The window will close once it stops.")
                : e.TryGetProperty("epoch", out var epoch)
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
    }
    public static void RenderTests(string folder)
    {
        Directory.CreateDirectory(folder);
        int views = 0;
        int languageChecks = 0;
        foreach (var language in new[] { "ru", "en" })
        foreach (var theme in new[] { "dark", "light" })
        {
            App.Settings.Language = language; App.Settings.Theme = theme; App.ApplyLanguage(); App.ApplyTheme();
            App.Settings.Save();
            var window = new MainWindow(false) { ShowInTaskbar = false, ShowActivated = false,
                WindowStartupLocation = WindowStartupLocation.Manual, Position = new PixelPoint(-20000, -20000) };
            window.Show();
            // The sampler runs independently; obtain real readings before capturing the panel.
            Thread.Sleep(1600);
            window._resourceTimer.Stop();
            foreach (var page in new[] { "home", "project", "train", "models", "library", "jobs", "settings" })
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
                views++;
                if (page is "train" or "settings" && window._content.Content is ScrollViewer scroll)
                {
                    scroll.Offset = new Vector(0, scroll.Extent.Height);
                    Dispatcher.UIThread.RunJobs(); window.UpdateLayout();
                    using var lower = new RenderTargetBitmap(new PixelSize(1180, 820), new Vector(96, 96));
                    lower.Render(root);
                    string suffix = page == "train" ? "train-decisions" : "settings-docs";
                    using var lowerOutput = File.Create(Path.Combine(folder, $"{language}-{theme}-{suffix}.png"));
                    lower.Save(lowerOutput, new PngBitmapEncoderOptions());
                    views++;
                }
            }
            foreach (string choice in new[] { "en", "ru", "system" })
            {
                window.Navigate("settings"); Dispatcher.UIThread.RunJobs(); window.UpdateLayout();
                var selector = window.GetVisualDescendants().OfType<ComboBox>().Single(c => c.Name == "LanguageChoice");
                selector.SelectedItem = selector.Items.Cast<ComboBoxItem>().Single(item => (string)item.Tag! == choice);
                // Language changes rebuild the shell on the next normal UI turn. Pump the real
                // dispatcher here; this render entrypoint otherwise blocks framework startup.
                var frame = new DispatcherFrame();
                var beat = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(80) };
                beat.Tick += (_, _) => { beat.Stop(); frame.Continue = false; };
                beat.Start(); Dispatcher.UIThread.PushFrame(frame); window.UpdateLayout();
                if (App.Settings.Language != choice || StudioSettings.Load().Language != choice)
                    throw new InvalidOperationException("Language menu did not persist its choice");
                string expected = App.Settings.EffectiveLanguage == "ru" ? "Настройки" : "Settings";
                if (!window.GetVisualDescendants().OfType<TextBlock>().Any(t => t.Text == expected && t.FontSize == 29))
                    throw new InvalidOperationException("Language menu did not rebuild page text: " + choice + " / " + App.Settings.EffectiveLanguage
                        + " / " + string.Join("; ", window.GetVisualDescendants().OfType<TextBlock>().Where(t => t.FontSize > 24).Select(t => t.Text + ":" + t.FontSize))
                        + " logical=" + (((ScrollViewer)window._content.Content!).Content as StackPanel)!.Children.OfType<TextBlock>().First().Text);
                languageChecks++;
            }
            using (var automatic = new RenderTargetBitmap(new PixelSize(1180, 820), new Vector(96, 96)))
            {
                automatic.Render((Control)window.Content!);
                using var output = File.Create(Path.Combine(folder, $"{language}-{theme}-settings-system.png"));
                automatic.Save(output, new PngBitmapEncoderOptions()); views++;
            }
            App.Settings.Language = language; App.ApplyLanguage();
            window.Width = 980; window.Height = 700; window.Navigate("home");
            var fixtures = new[]
            {
                ("resources-busy", new ResourceSnapshot(DateTimeOffset.UtcNow, 92, 50L << 30, 64L << 30,
                    true, 38, 24L << 30, [new GpuLoad("test-a", "GPU A", 16, 1L << 30, 8L << 30, 0),
                    new GpuLoad("test-b", "GPU B · 32 GiB", 97, 30L << 30, 32L << 30, 1L << 30)])),
                ("resources-unavailable", ResourceSnapshot.Empty)
            };
            foreach (var (name, fixture) in fixtures)
            {
                window._meter!.Show(fixture);
                Dispatcher.UIThread.RunJobs(); window.UpdateLayout();
                var root = (Control)window.Content!;
                root.Measure(new Size(980, 700)); root.Arrange(new Rect(0, 0, 980, 700));
                using var image = new RenderTargetBitmap(new PixelSize(980, 700), new Vector(96, 96));
                image.Render(root);
                using var output = File.Create(Path.Combine(folder, $"{language}-{theme}-{name}.png"));
                image.Save(output, new PngBitmapEncoderOptions()); views++;
            }
            window.Close();
        }
        File.WriteAllText(Path.Combine(folder, "ui-test.json"), JsonSerializer.Serialize(new { pass = true, views, language_menu_checks = languageChecks }));
    }
}
