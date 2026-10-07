# Сборка предварительной версии 0.1

Проверенная среда: Windows x64, .NET SDK 10.0.111, Python 3.12,
PyTorch 2.10.0+cu128, RTX 5090. Это инструкция разработчика; окончательная
программа должна устанавливать среду сама и не требовать этих действий.

## Рабочий модуль

Для первой версии подходит отдельная Python-среда с зависимостями из
`worker/requirements.txt` и PyTorch 2.10.0. Выбор CUDA/CPU делается при
установке среды; официальные индексы доступны на
[странице PyTorch](https://pytorch.org/get-started/previous-versions/#v2-10-0).
Нужен только torch: torchvision/torchaudio этому прототипу не требуются.
Подготовка новой среды с нуля пока не включена в GUI.

Из корня репозитория, с активированной подготовленной средой:

```powershell
python -m pip install -r worker/requirements.txt
Push-Location worker
python -m unittest opentdm_x_trainer.test_contracts opentdm_x_trainer.test_studio -v
Pop-Location
```

CPU-вычисления доступны и в проверенной CUDA-сборке torch; поведение на
других сборках/вендорах нужно проверять отдельно. Нет скрытого обещания, что
любой установленный GPU автоматически поддерживается.

## Desktop

Из корня репозитория:

```powershell
dotnet build src/BotTrainingStudio/BotTrainingStudio.csproj -c Release
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build_preview.ps1
```

Локальная сборка появится в `dist/preview-win-x64/BotTrainingStudio.exe`.
GUI самодостаточен по .NET, но Python/ML-среда пока выбирается отдельно.
В настройках указываются Python нужной среды и папка `worker` из сборки.
В «Обучении» указывается подготовленный датасет (`manifest.json` +
`samples.npz`) и отдельная папка модели. Обычный пользователь пока не сможет
заменить эту подготовку выбором сырой демки: это следующий этап реализации.

Настройки по умолчанию: `%LOCALAPPDATA%/QuakeJourney/BotTrainingStudio`.
Для изолированной проверки можно задать `BTS_HOME`. Данные не копируются
в исходники автоматически и не загружаются в GitHub.

## Проверки

```powershell
python scripts/verify_workers.py --out artifacts/cpu --backend cpu
```

Для подтверждённой NVIDIA-среды используется `--backend cuda`.
Параметр `--dataset` дополнительно запускает настоящее обучение двух
профилей на локальном подготовленном наборе. Проверка оборудования и
синтетический шаг обучения не являются проверкой качества игры.

Desktop поддерживает внутренние режимы `--ui-test <каталог>` (24 представления)
и `--bridge-test <python> <worker> <receipt> [dataset]` (процессный протокол,
вычисление, ошибка и, с датасетом, обучение/отмена). При запуске GUI-приложения
из PowerShell необходимо дождаться процесса, если читается его exit code.

Публичная выкладка 0.1 содержит исходники. Пользовательский установщик,
ML-runtime, демки и обученные веса не входят в неё. Локальную development-
сборку нельзя выдавать за готовый автономный пользовательский дистрибутив.
