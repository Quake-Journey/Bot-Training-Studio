using Avalonia;
using Avalonia.Controls;
using Avalonia.LogicalTree;

namespace BotTrainingStudio;

public sealed partial class MainWindow
{
    private string Help(string label)
    {
        if (label.StartsWith("Обновить до ") || label.StartsWith("Update to ")) label = "Обновить";
        // Both labels address the same RU/EN entry, including mixed-language guides.
        foreach (var (ru,en,ruTip,enTip) in HelpEntries)
            if (label == ru || label == en) return L(ruTip,enTip);
        return L("Показывает сведения об этом пункте. Изменения настроек применяются к следующему заданию.",
            "Displays information about this item. Settings changes apply to the next job.");
    }
    private void Tip(Control control, string text)
    {
        ToolTip.SetTip(control,new TextBlock { Text=text, TextWrapping=Avalonia.Media.TextWrapping.Wrap, MaxWidth=440 });
        ToolTip.SetShowDelay(control,450);
    }
    private void ApplyHelp(Control root)
    {
        foreach (var control in root.GetLogicalDescendants().OfType<Control>().Prepend(root))
        {
            if (control is Button button) Tip(button,Help(button.Content?.ToString() ?? ""));
            if (control is CheckBox check) Tip(check,Help(check.Content?.ToString() ?? ""));
            if (control is TextBox or ComboBox && ToolTip.GetTip(control) == null)
            {
                string? label = null;
                Control child = control;
                for (int depth=0;depth<3 && label==null;depth++)
                {
                    if (child.Parent is not Panel panel) break;
                    label = panel.Children.TakeWhile(c=>c!=child).OfType<TextBlock>().LastOrDefault()?.Text;
                    child = panel;
                }
                Tip(control,Help(label ?? ""));
            }
        }
    }

    private static readonly (string Ru,string En,string RuTip,string EnTip)[] HelpEntries =
    [
        ("Главная","Home","Обзор программы, готовность среды обучения и выбор вычислений.","Application overview, training runtime readiness and computation choice."),
        ("Карта и демки","Map & demos","Выбери BSP, игровые демки и отдельные записи триксов; затем импортируй и изучи их.","Choose a BSP, match demos and separate trick recordings, then import and analyze them."),
        ("Обучение","Training","Подготовь независимые выборки и обучи новое пользовательское поколение модели.","Prepare independent dataset partitions and train a new user model generation."),
        ("Модели и GPU","Models & GPU","Проверь оборудование и вычисления модели. Эти пробы не измеряют качество игры бота.","Check hardware and model computation. These probes do not measure bot gameplay quality."),
        ("Библиотека","Library","Заводская основа, пользовательские модели и офлайн-пакеты карты хранятся раздельно.","Factory bases, user models and offline map packages are stored separately."),
        ("Задания","Jobs","Прогресс, журнал, результат и безопасная отмена текущего задания.","Progress, logs, results and safe cancellation of the current job."),
        ("Настройки","Settings","Язык, тема, обновления, вычисления и среда Python.","Language, theme, updates, computation and Python runtime."),
        ("Что нового","What's new","Вся история версий, начиная с последней. Прокручивай окно для просмотра ранних версий.","The entire version history, latest first. Scroll the dialog to view earlier releases."),
        ("Обновить","Update","Проверить GitHub Releases. Обновление предлагается перед установкой и сохраняет пользовательское обучение.","Check GitHub Releases. Installation requires confirmation and preserves user training."),
        ("Проверить обновления","Check for updates","Проверить наличие более новой опубликованной версии. Нужен доступ к GitHub.","Check for a newer published version. Access to GitHub is required."),
        ("Открыть обучение","Open training","Перейти к подготовке данных и обучению моделей на этом компьютере.","Open dataset preparation and model training on this computer."),
        ("Настройка Python","Python setup","Выбрать готовую совместимую среду либо установить Python из комплекта без интернета.","Select a ready compatible environment or install bundled Python offline."),
        ("Проверить оборудование","Check hardware","Определить доступные устройства через библиотеки обучения. Выбор в панели нагрузки меняет только мониторинг.","Detect available devices through the training libraries. The load-panel selector changes monitoring only."),
        ("Выбрать…","Browse…","Открыть выбор файла или папки для этого поля. Можно также вставить полный путь вручную.","Browse for the file or folder for this field. You can also paste a full path."),
        ("Проекты карт — по одному пути в строке","Map projects — one folder per line","Папки импортированных проектов с project.json. Каждая карта или проект — на отдельной строке.","Imported project folders containing project.json. Put each map or project on a separate line."),
        ("Добавить текущую карту","Add current map","Добавить папку из раздела «Карта и демки» в список проектов обучения без повторов.","Add the folder from Map & demos to the training projects without duplicates."),
        ("История наблюдений (кадров)","Observed history (frames)","Сколько предыдущих кадров видит модель. Длинная история требует больше памяти. Заводские модели используют 16 кадров.","How many past frames the model sees. Longer histories use more memory. Factory models use 16 frames."),
        ("Максимум примеров в каждой выборке","Maximum examples per dataset partition","Ограничение размера обучающей, проверочной и итоговой выборок. Матчи между выборками не смешиваются.","Limit the training, validation and test partition sizes. Matches are not mixed across partitions."),
        ("Подготовить данные проектов","Prepare project data","Собрать наблюдения из выбранных проектов в новый датасет. Исходные демки и предыдущие наборы сохраняются.","Build a new observation dataset from the selected projects. Original demos and previous datasets are preserved."),
        ("Подготовленный набор","Prepared dataset","Папка с manifest.json и samples.npz, полученная подготовкой данных проектов.","A folder containing manifest.json and samples.npz produced by project-data preparation."),
        ("Модель и контрольные точки","Model and checkpoints","Пользовательская папка поколений, контрольных точек и опыта. Не выбирай папку заводских Models или файлов приложения.","User folder for generations, checkpoints and replay experience. Do not select factory Models or application files."),
        ("Подобрать размер пакета по памяти и скорости","Measure batch size for memory and throughput","Пробная обучающая итерация подбирает размер пакета на выбранном устройстве. Большой пакет не гарантирует лучшее качество.","A trial training iteration measures batch size on the selected device. Larger batches do not guarantee better quality."),
        ("Обучить новое поколение","Train new generation","Обучить модель с нуля. Это не дообучение заводской основы; прежние поколения не удаляются.","Train a model from scratch. This does not refine the factory base; previous generations are preserved."),
        ("Начать от заводской модели","Start from factory model","Создать отдельное пользовательское дополнение к заводской основе. Нужны compact или balanced, 16 кадров и пустое поле донора; выбери новую папку модели.","Create a separate user overlay over the factory base. Requires compact or balanced, 16 frames and no donor; select a new model folder."),
        ("Дообучить","Update","Продолжить активное пользовательское поколение с повтором прежнего опыта. Заводские веса не изменяются.","Refine the active user generation while replaying prior experience. Factory weights stay unchanged."),
        ("Дообучить","Learn new data","Добавить новый опыт к модели решений; прежние матчи проверяются на ухудшение отдельно.","Add new experience to the decision model; prior matches are evaluated separately for regressions."),
        ("Возобновить","Resume","Продолжить после завершённой контрольной эпохи. Датасет, профиль, история и донор должны совпадать.","Continue from the last completed checkpoint epoch. Dataset, profile, context and donor must match."),
        ("Подготовить события игры","Prepare gameplay events","Подготовить наблюдаемые выстрелы и изменения сообщений о подборе. Это отдельный датасет для модели решений.","Prepare observed shots and pickup-message changes as a separate decision-model dataset."),
        ("Набор событий","Event dataset","Папка подготовленных наблюдаемых выстрелов и подборов. Не заменяет набор последовательностей движения.","Prepared observed-shot and pickup dataset folder. This does not replace the movement-sequence dataset."),
        ("Отдельная папка модели решений","Separate decision model folder","Экспериментальные поколения прогнозов оружия и предметов. Хранятся отдельно от модели движения.","Experimental weapon and item prediction generations, kept separately from motion models."),
        ("Подобрать пакет для модели решений","Measure decision model batch size","Измерить память и скорость выбранного профиля на примере модели решений.","Measure memory and throughput for the selected decision-model profile."),
        ("Обучить","Train","Обучить новую модель решений по подготовленному набору событий. Результат пока не устанавливается в мод.","Train a new decision model on the prepared event dataset. The result cannot yet be installed in the mod."),
        ("Проверить модель","Probe model","Выполнить настоящую пробную итерацию на синтетических данных и проверить изменение весов. Это не обучение по твоим демкам.","Run a real trial iteration on synthetic data and check weight changes. This is not training on your demos."),
        ("Проверить и открыть результат","Verify and view result","Проверить целостность активного пользовательского поколения и показать его метрики. Отсутствие файла active.json означает, что принятого поколения ещё нет.","Verify the active user generation and display its metrics. No active.json means no accepted generation yet."),
        ("Проверить заводские модели","Verify factory models","Проверить хеши комплектных весов и описаний моделей. Это проверка целостности файлов, а не игрового качества бота.","Verify bundled model weights and metadata hashes. This checks file integrity, not bot gameplay quality."),
        ("Результат анализа","Analysis result","JSON с маршрутами, предметами и наблюдаемым стилем из анализа текущего проекта.","JSON containing routes, items and observed style from analysis of the current project."),
        ("Добавить обученную модель (карта и донор должны совпадать)","Include the learned model (map and donor must match)","Собрать ограниченный офлайн-прогноз из проверенных выходов модели. Заводская основа и пользовательское дополнение читаются совместно; карта и донор должны совпадать.","Compile a bounded offline predictor from validated model heads. Factory base and user overlay are loaded together; map and donor must match."),
        ("Включить фразы игрока в пакет","Include player phrases in the package","Добавить извлечённые фразы выбранного донора в экспортируемые данные. По умолчанию выключено.","Include extracted phrases of the selected donor in exported data. Off by default."),
        ("Собрать пакет карты","Compile map package","Создать новый проверяемый офлайн-пакет маршрутов, стиля и выбранной модели. Он не устанавливается на сервер.","Create a new verifiable offline package of routes, style and the selected model. It is not installed on a server."),
        ("Проверить пакет","Verify package","Проверить хеши, формат и ограничения выбранного .btsknowledge; проверка не подтверждает качество игры.","Verify hashes, format and bounds of the selected .btsknowledge. This does not qualify gameplay quality."),
        ("Файл пакета","Package file","Выбери существующий .btsknowledge для проверки или добавления в офлайн-библиотеку. После сборки здесь автоматически указывается новый файл.","Select an existing .btsknowledge to verify or add to the offline library. Compiling a package updates this field automatically."),
        ("Добавить в библиотеку студии","Add to Studio library","Сохранить проверенный пакет в локальной офлайн-библиотеке с историей поколений. Файлы сервера не изменяются.","Store a verified package in the local offline library with generation history. Server files are not changed."),
        ("Карта BSP","BSP map","Файл карты Quake II. Геометрия и хеш BSP связывают маршруты и обучение с конкретной картой.","A Quake II map file. Its geometry and BSP hash bind routes and training to the exact map."),
        ("Папка проекта","Project folder","Отдельная папка импортированной карты, анализа и истории. Исходные файлы остаются на месте.","Separate folder for the imported map, analysis and history. Original files remain in place."),
        ("DM2 / MVD2 или ZIP / RAR","DM2 / MVD2 or ZIP / RAR","Игровые записи или архивы с ними. Один полный путь на строке; приватные записи не отправляются в облако.","Match recordings or archives containing them. One full path per line; private recordings are not uploaded."),
        ("Отдельные записи триксов (по одному пути в строке)","Separate trick recordings (one path per line)","Отдельно помеченные записи прыжков и маршрутов. Их движения проверяются в игровой физике, а не объявляются успешными по одной траектории.","Separately marked jump and route recordings. Motion is checked in game physics, not qualified from trajectories alone."),
        ("Добавить записи…","Add recordings…","Выбрать несколько DM2, MVD2, ZIP или RAR и дополнить список игровых записей.","Select multiple DM2, MVD2, ZIP or RAR files and append them to the match-recording list."),
        ("Проверить список","Inspect inputs","Показать доступные записи и содержимое архивов до импорта.","Inspect available recordings and archive members before import."),
        ("Импорт / дополнение","Import / update","Создать новую ревизию проекта из выбранных записей, сохранив прежнюю историю.","Create a new project revision from selected recordings while preserving history."),
        ("Новая выборка","Fresh selection","Сформировать новую ревизию по текущему выбору записей вместо дополнения старой выборки.","Create a new revision from the current recording selection instead of extending the old selection."),
        ("Донор стиля (пусто — все игроки)","Style donor (empty — all players)","Ник игрока для изучения стиля, с нормализацией и поиском похожих имён. Пустое поле включает всех игроков.","Player nickname for style analysis, with normalization and similar-name matching. Empty selects all players."),
        ("Изучить маршруты и стиль","Analyze routes and style","Извлечь наблюдаемые маршруты, предметы и привычки игрока. Неполные данные демки не подменяются выдуманными.","Extract observed routes, items and player habits. Missing demo observations are not invented."),
        ("Проверить движения через физику","Verify motion through physics","Проверить воспроизводимость наблюдаемых движений совместимым физическим стендом.","Check whether observed movements can be reproduced in the compatible physics harness."),
        ("Отменить текущее задание","Cancel current job","Запросить безопасную остановку. Завершённые эпохи и прежнее поколение сохраняются; текущая GPU-операция сначала закончится.","Request a safe stop. Completed epochs and the previous generation are preserved; the current GPU operation finishes first."),
        ("Язык","Language","По умолчанию русский для русской Windows, английский для остальных. Язык интерфейса и подсказок меняется сразу.","Defaults to Russian for Russian Windows, English otherwise. Interface and tooltip language change immediately."),
        ("Тема","Theme","Тёмная, светлая или системная тема оформления. Выбор сохраняется.","Dark, light or system appearance. Your choice is saved."),
        ("Автоматически проверять обновления при запуске","Automatically check for updates at startup","Проверять только опубликованные версии GitHub при запуске. Автоматической установки без подтверждения нет.","Check published GitHub versions at startup. Updates are not installed without confirmation."),
        ("Папка worker","Worker folder","Комплектный модуль анализа выбирается автоматически. Меняй путь только для другой совместимой среды разработки.","The bundled analysis worker is selected automatically. Change this only for another compatible development environment."),
        ("Установить Python из комплекта","Install bundled Python","Установить приватный Python из проверенного архива без интернета и прав администратора. Системный Python не заменяется.","Install private Python from the verified bundled archive offline without administrator access. System Python is not replaced."),
        ("Указать установленный Python…","Select installed Python…","Выбрать python.exe; среда должна пройти проверку библиотек и настоящих вычислений.","Select python.exe; its environment must pass library and actual computation checks."),
        ("Отменить установку","Cancel installation","Безопасно остановить установку. Предыдущая рабочая среда сохраняется.","Safely stop setup. The previous working runtime is preserved."),
        ("Остаться в программе после остановки","Stay in the application after stopping","Отменить автоматический выход после остановки. Само задание продолжит останавливаться.","Cancel automatic exit after stopping. The job will still stop."),
        ("Руководство на русском (DOCX)","Russian user guide (DOCX)","Открыть комплектное руководство на русском в приложении для DOCX.","Open the bundled Russian guide in a DOCX application."),
        ("User guide in English (DOCX)","Английское руководство (DOCX)","Открыть комплектное руководство на английском в приложении для DOCX.","Open the bundled English guide in a DOCX application.")
    ];
}
