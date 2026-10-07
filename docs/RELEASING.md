# Выпуски / Releases

## Русский

Готовые сборки для пользователей публикуются в **GitHub Releases** репозитория
**Quake-Journey/Bot-Training-Studio**. Обновление исходников в GitHub не заменяет
выпуск приложения. Это правило не объявляет текущую предварительную версию готовой.

### Версии

- Каждая новая передаваемая сборка с доработками получает новый номер версии.
  Исправления увеличивают patch, новые совместимые возможности — minor;
  несовместимые изменения требуют отдельного решения о major и миграции данных.
- Для предварительных выпусков явно указывать preview/prerelease. Готовый
  пользовательский выпуск нельзя подменять непроверенной сборкой разработчика.
- Версия должна совпадать в приложении, метаданных EXE, `build.json`, руководствах,
  названии архива и Git-теге `v<version>`. Перед сборкой сверить значение
  `Version` в `src/BotTrainingStudio/BotTrainingStudio.csproj`. UI, EXE,
  `scripts/build_preview.ps1`, генератор DOCX и упаковщик читают эту версию;
  первая запись `docs/changelog.json` должна ей соответствовать.
- Опубликованный номер не использовать повторно для изменённых исполняемых файлов.
  Следующее исправление получает следующую версию. Правка только правил разработки
  сама по себе не означает, что выпущена новая версия приложения.

### Состав выпуска

1. Полный пакет приложения с исполняемыми файлами, worker/native-компонентами,
   библиотеками программы и модели, необходимыми ресурсами, лицензиями и данными
   моделей, если они требуются этому выпуску. Исходный архив GitHub не является
   установочным пакетом. Пользователь не собирает программу и не устанавливает
   PyTorch либо CUDA Toolkit вручную.
2. Включать проверенный официальный архив Python 3.13 x64 в пакет. Использовать
   совместимый установленный Python или готовую среду 3.12+; иначе предлагать
   установку из комплекта без интернета. В примечаниях указывать проверенные ОС,
   архитектуры, backend и требования к драйверу; непроверенные AMD/Intel не
   объявлять поддержанными. Разные аппаратные пакеты подписывать однозначно.
3. Актуальные русское и английское руководства DOCX внутри пакета и отдельными
   вложениями релиза. Их Markdown-исходники и сведения о проверке свежести
   обновляются в репозитории.
4. Описание изменений относительно предыдущего опубликованного выпуска: каждая
   новая возможность или улучшение один раз, без истории промежуточных починок.
   Указать ограничения и важные действия при обновлении, если они есть.
5. Контрольные суммы SHA-256 для публикуемых файлов и тег точного исходного коммита.
   Модели и большие двоичные пакеты не помещать в обычную историю исходников.

### Перед публикацией

Обновить документацию RU/EN при изменении поведения, пересобрать DOCX и проверить
их свежесть и отображение. Собрать пакет из зафиксированного состояния исходников.
Проверить распаковку в отдельную чистую папку, запуск, выбор/установку Python,
загрузку комплектных библиотек и заявленные вычислительные backend, отмену и выход,
наличие документации. Использовать изолированные настройки и сохранять данные
пользователя. Проверять фактические ограничения загрузки GitHub при подготовке
крупных архивов. Не публиковать серверные конфиги, ключи, частные демки, пользовательские
проекты, кэши и диагностические выгрузки.

После загрузки проверить состав вложений, размеры и контрольные суммы, соответствие
тега коммиту. Сообщить пользователю версию, страницу релиза и что именно проверено.
При необходимости согласования пользователь получает конкретный проверенный пакет;
существующее разрешение на публикацию сохраняется, повторного общего разрешения не нужно.

## English

Ready end-user builds are published in **GitHub Releases** of
**Quake-Journey/Bot-Training-Studio**. A source push is not an application release.
This policy does not declare the current development preview complete.

- Give each delivered application revision a new version: patch for fixes, minor
  for compatible features; decide major-version and migration requirements for
  incompatible changes. Mark preview releases explicitly.
- Keep the application/UI, EXE metadata, `build.json`, guides, archive names and
  `v<version>` tag consistent. The project `Version` is the source for the UI,
  EXE, build metadata, guides and packager; match the first `docs/changelog.json`
  entry to it. Never reuse a published version for
  changed binaries. A development-policy edit alone is not a new application release.
- Attach complete application packages, workers/native tools, application/model
  libraries, required model data/resources and licenses. The GitHub source archive
  is not an installable package. Include the pinned Python archive and offer
  offline setup when compatible Python or a complete 3.12+ environment is unavailable; no user-side compiler, PyTorch or CUDA Toolkit setup.
- Identify tested OS/architecture/backend/driver requirements and distinguish
  hardware packages. Do not advertise unqualified AMD/Intel support.
- Update both RU/EN guides for changed behavior, regenerate/validate/render DOCX,
  include them in the package and attach them separately to the release. Keep their
  editable Markdown sources and freshness metadata current in the repository.
- Describe the net change from the previous published version, each feature or
  improvement once, plus relevant limitations and migration steps.
- Publish SHA-256 checksums and tag the exact source commit. Keep large binaries
  and models out of ordinary source history. Verify GitHub upload constraints when
  preparing large archives. Never upload private recordings, server configurations,
  secrets, user projects, caches or diagnostic dumps.
- Qualify extraction into a clean location, launch, Python reuse/setup, bundled
  libraries, advertised computation backends, cancellation/exit and documentation
  using isolated settings. Preserve user data. Verify uploaded assets, sizes,
  checksums and tag/commit consistency, then report the version, release page and
  validation scope. If approval is needed, present the concrete verified package;
  existing publication authorization persists without repeated general requests.

## Release tooling / Инструменты выпуска

Локальная актуальная программа всегда находится в `dist/Release`, архивы
текущей версии — в `dist/packages`. Не создавать отдельные локальные каталоги
программы для каждого номера версии. Перед подготовкой нового пакета удалять
только проверенные опубликованные архивы предыдущего выпуска; историю хранить
в GitHub Releases, локальные небольшие свидетельства проверки — в artifacts.
Это изменение правил локальной сборки не требует новой версии приложения.

Use one stable local application directory, `dist/Release`, and only the current
release archives in `dist/packages`. Historical builds belong in GitHub Releases;
keep small verification receipts in artifacts. Do not accumulate version-named
local runnable copies. These build-tool defaults do not change the application version.

`scripts/build_release.py` defaults to `--package dist/Release` and
`--out dist/packages`, and takes the `--rar` executable. It builds the complete multipart RAR,
the smaller application ZIP, checksummed library ZIP parts, updater manifest,
paired DOCX attachments and SHA256SUMS. Library parts can be prepared first with
`--libraries-only`; a matching verified local receipt permits reuse. The pinned official Python archive belongs in `worker/runtime/`; personal settings,
recordings and user-trained models do not belong in this package. Factory weights
are the explicit exception: only files pinned by `packaging/factory-models.lock.json`
may be distributed. Keep replay and checkpoints private. The full RAR contains
`Models/` beside the EXE. The app ZIP transports the same verified files under
`worker/factory-models/` for compatibility with previously published updaters;
the new application materializes `Models/` on first launch, without touching user
stores. Existing user overlays retain their pinned bases, not the newest factory
weights. Run
`scripts/fetch_python_embed.py` at build time before `build_preview.ps1`.

`scripts/publish_release.py --assets <release-assets.json> --notes <release-body.md>`
first verifies local files without publishing. Add `--publish` only for an
authorized release: source must be clean and pushed, tag must match HEAD.
Git's credential helper supplies authentication without output or storage of
the token. Assets upload to a draft; the draft is published only after GitHub's
size and SHA256 digest match every asset. Never replace a published build.

Полный RAR — для первой установки пользователем; все тома лежат вместе,
распаковывается первый. App ZIP и library ZIP parts — для встроенного обновлятора,
их нельзя выдавать за полный установочный комплект. Предварительные выпуски
публикуются с признаком prerelease. Проверка обновлений включена по умолчанию;
установка требует подтверждения и не прерывает задания.

The full RAR is the first-install package; extract its first volume with all
volumes in one folder. App ZIP/library parts are updater assets, not complete
user installations. Preview builds use GitHub's prerelease flag. Automatic
checking is enabled by default; installing requires confirmation and cannot
interrupt a running job.
