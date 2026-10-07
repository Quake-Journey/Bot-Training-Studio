using Avalonia.Controls;

namespace BotTrainingStudio;

public sealed partial class MainWindow
{
    private ComboBox ComputeChoice()
    {
        var options = new[]
        {
            new ComboBoxItem { Tag = "auto", Content = L("Автоматически — GPU, иначе CPU", "Automatic — GPU, otherwise CPU") },
            new ComboBoxItem { Tag = "cuda", Content = "NVIDIA GPU (CUDA)" },
            new ComboBoxItem { Tag = "cpu", Content = L("CPU — процессор", "CPU — processor") },
            new ComboBoxItem { Tag = "rocm", Content = L("AMD GPU — экспериментально", "AMD GPU — experimental") },
            new ComboBoxItem { Tag = "xpu", Content = L("Intel GPU — экспериментально", "Intel GPU — experimental") }
        };
        var choice = new ComboBox { Name = "ComputeChoice", ItemsSource = options, MinWidth = 280,
            SelectedItem = options.FirstOrDefault(o => (string)o.Tag! == S.Backend) ?? options[0], IsEnabled = !JobActive };
        Tip(choice,L("Устройство следующего задания. Автоматически — доступная GPU, иначе CPU. NVIDIA поддерживается комплектом; AMD/Intel экспериментальны. Нижняя панель GPU выбирает только мониторинг.",
            "Device for the next job. Automatic uses an available GPU, otherwise CPU. NVIDIA is bundled; AMD/Intel are experimental. The bottom GPU panel selects monitoring only."));
        choice.SelectionChanged += (_, _) =>
        {
            if (choice.SelectedItem is ComboBoxItem { Tag: string backend } && !JobActive)
            { S.Backend = backend; S.Save(); }
        };
        return choice;
    }

    private Border ComputeCard()
    {
        var panel = Stack(10);
        panel.Children.Add(Text(L("Вычисления: CPU / GPU", "Computation: CPU / GPU"), 19, true));
        panel.Children.Add(ComputeChoice());
        panel.Children.Add(Text(L("Этот выбор применяется к следующему заданию обучения. В режиме «Автоматически» используется доступная GPU; если её нет — CPU. Список видеокарт внизу переключает только мониторинг нагрузки.",
            "This choice applies to the next learning job. Automatic mode uses an available GPU, otherwise CPU. The GPU list in the bottom panel changes monitoring only."), 13));
        panel.Children.Add(Text(L("В комплекте проверены CPU и NVIDIA. AMD/Intel требуют соответствующих библиотек и пока не подтверждены для этой сборки; выбор не выдаёт их за доступное оборудование.",
            "The package is qualified for CPU and NVIDIA. AMD/Intel need their matching libraries and are not yet qualified for this build; choosing them does not make unavailable hardware supported."), 13));
        return Card(panel);
    }
}
