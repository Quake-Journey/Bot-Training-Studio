# Bot Training Studio by ly

Локальная программа обучения ботов OpenTDM-X по опыту игроков.

**0.3.0-preview.2 — предварительная версия для разработки.** Работают прямой импорт
BSP + DM2/MVD2, проекты карт, извлечение маршрутов и обучение по истории
матчей. Это офлайн-исследование: полноценная тактическая модель ещё не готова,
устанавливать полученные пакеты в текущий игровой мод нельзя.

Руководства: **[Русский — DOCX](docs/Bot_Training_Studio_User_Guide_RU.docx)** ·
**[English — DOCX](docs/Bot_Training_Studio_User_Guide_EN.docx)**.
Читать на GitHub: [RU](docs/USER_GUIDE_RU.md) · [EN](docs/USER_GUIDE_EN.md).
В программе они доступны в «Настройки → Документация».

Полные предварительные сборки доступны в
[GitHub Releases](https://github.com/Quake-Journey/Bot-Training-Studio/releases)
с номером версии, описанием изменений и документацией. Текущая версия остаётся
предварительной. [Правила выпуска и нумерации](docs/RELEASING.md).

## Что нового в 0.3.0-preview.2

- Стартовая проверка Python и библиотек перенесена в отдельное модальное окно; установщик больше не появляется и не исчезает на главной странице.
- Список GPU показывает физические видеокарты: исключены виртуальные дисплеи, программный рендерер Windows и безымянные записи счётчиков. Две настоящие карты одной модели остаются отдельными устройствами.
- После запуска новой распакованной копии используется её комплектный модуль обучения, а не модуль из прежней папки.
- Обновлены руководства RU/EN. Проверка среды не перекрывается журналом изменений и не блокирует обработку интерфейса.

[Полный список изменений RU](docs/CHANGELOG.ru.md) · [English change notes](docs/CHANGELOG.en.md).

## Что работает

- Отдельное настольное приложение: русский/английский интерфейс, светлая,
  тёмная и системная темы; запуск, прогресс, отмена и результаты заданий.
  Фоновая работа не блокирует навигацию. Выход во время задания требует
  подтверждения и ожидает штатной остановки с сохранением контрольных точек.
- Язык по умолчанию соответствует интерфейсу системы: русский для русской
  системы, английский для остальных. В настройках можно выбрать «Как в системе»,
  «Русский» или «English»; выбор сохраняется между запусками.
- Постоянная панель CPU, RAM, GPU и видеопамяти, как в Mapgen Studio:
  значения и цветные полоски, выбор видеокарты, отдельные CPU/RAM задания.
  Мониторинг Windows работает в фоне и не требует запуска Python.
- Вычисления в отдельном процессе: CPU и проверенный NVIDIA CUDA.
  В коде предусмотрены ROCm и Intel XPU, но на соответствующем оборудовании
  эта версия ещё не проверялась.
- Библиотеки программы и модели входят в комплект. Используется подходящий
  установленный Python; если его нет, программа предлагает установить только
  Python. Никаких ручных pip-команд или установки CUDA Toolkit пользователю
  не требуется. Установка работает в фоне, с проверкой и отменой.
- Эталонная небольшая модель и четыре экспериментальных Transformer-профиля:
  Compact, Balanced, Large, XL. Это начало исследования, а не окончательные
  модели тактики и не потолок доступной видеопамяти.
- Обучение с нуля и дообучение с сохранением прошлых наблюдений, отдельная
  проверка качества на старых картах, поколения, контроль целостности и откат.
- Проверка обновления весов, ошибок и отмены; прежняя активная модель
  сохраняется при неудаче. Данные обрабатываются локально.
- Самостоятельный декодер DM2/MVD2, выбор записей из ZIP, проверка BSP,
  неизменяемые ревизии проектов и кэш с проверкой хешей. RAR требует UnRAR.
- Последовательности наблюдений с геометрией и неизвестными состояниями;
  разделение по целым парам соперников, а не по случайным кадрам.
- Сохранение смертей и короткой истории после респавна, маски неизвестного
  будущего, наблюдаемый боезапас и выстрелы. Отдельные прогнозы существенной
  потери стека, восстановления и смерти с проверкой на простых базовых моделях.
- Условные профили поведения по дистанции, стеку и высоте противника;
  тренировочные матчи отделены от проверочных, редкие ситуации отмечаются.
- Встроенный Pmove для обычных маршрутов и непрерывной проверки восстановленных
  движений. Успешный короткий фрагмент не объявляется целым выученным триксом.
- Контрольные точки модели и оптимизатора, продолжение после остановки,
  защита прежних карт через повтор данных и сохранение предсказаний старой модели.
- Пакеты данных с проверкой целостности и библиотекой отката. Непрошедшие
  проверку выходы модели не включаются в пакет вместе с успешными.
- Сборщик сред CPU/CUDA для разработчика и комплектных библиотек для
  пользовательского пакета. Python используется установленный либо отдельно
  устанавливается программой. AMD/Intel ещё не проверены.

## К чему идём

Пользователь выбирает карту, игровые и триксовые демки; программа готовит
маршруты, приёмы движения, контроль предметов и тактические данные. Отдельный
режим создаёт стиль игрока по его демкам и фразам с поиском похожих ников.
Готовый проверенный пакет читает заранее собранный игровой мод: без GPU,
Python и компиляции DLL на стороне пользователя.

Сейчас формат взаимодействия с модом спроектирован, но будущий универсальный
загрузчик потребует согласованного официального обновления OpenTDM-X.
Экспериментальная модель в этой версии предсказывает наблюдения, **не команды
игровому боту**. Низкая ошибка предсказания не доказывает хорошую игру.

## Начать разработку

См. [сборку и запуск](docs/BUILD_RU.md),
[архитектуру и этапы](docs/ARCHITECTURE_RU.md),
[результаты проверок 0.2](docs/VALIDATION_0.2_RU.md).
Дальнейшие результаты: [обучение исходам эпизодов](docs/VALIDATION_OUTCOMES_RU.md).
Текущий эксперимент: [выстрелы и подборы предметов](docs/VALIDATION_DECISIONS_RU.md).

Исходный код: GPL-2.0-or-later, см. LICENSE и
[происхождение компонентов](docs/PROVENANCE.md). Демки, карты, обученные веса,
конфиги серверов и личные данные в репозитории отсутствуют.

## English

An independent offline desktop training application for OpenTDM-X bots.
Version **0.3.0-preview.2** adds persistent compute-device selection, bilingual change notes and verified GitHub updates with cancellation and rollback.
This development preview contains a bilingual Avalonia UI, bundled native
demo/physics tools, transactional map projects, sequence learning and continual
replay. CPU/CUDA model libraries are bundled; compatible Python is reused or
installed separately by the application.
**Tactical learning and installable server knowledge are not complete.**
No game-module source, private recordings or pretrained weights are included.
See the architecture and validation documents for current limits.

Complete preview builds are published in
[GitHub Releases](https://github.com/Quake-Journey/Bot-Training-Studio/releases)
with version numbers, change notes and documentation. This remains a development
preview. See the bilingual [release policy](docs/RELEASING.md).

The UI defaults to Russian for a Russian system UI language, and English
otherwise. **Settings → Language** offers **Use system language**, **Русский**
and **English** with immediate switching and a saved preference.

**[English user guide (DOCX)](docs/Bot_Training_Studio_User_Guide_EN.docx)** ·
**[Russian user guide (DOCX)](docs/Bot_Training_Studio_User_Guide_RU.docx)**.
Both are bundled in `docs` and accessible through **Settings → Documentation**.
Editable guides: [English](docs/USER_GUIDE_EN.md), [Russian](docs/USER_GUIDE_RU.md).

Application and model libraries are included. An existing compatible Python
is reused; **Install Python** downloads only the interpreter if needed. No
user-side pip commands, PyTorch setup or CUDA Toolkit installation. Downloads
and computations are checked before activation. The application has an
embedded Windows icon for Explorer, its window and the taskbar.
