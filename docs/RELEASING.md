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
  `Version` в `src/BotTrainingStudio/BotTrainingStudio.csproj` и версию, которую
  записывает `scripts/build_preview.ps1`; не оставлять старую константу в сборщике.
- Опубликованный номер не использовать повторно для изменённых исполняемых файлов.
  Следующее исправление получает следующую версию. Правка только правил разработки
  сама по себе не означает, что выпущена новая версия приложения.

### Состав выпуска

1. Полный пакет приложения с исполняемыми файлами, worker/native-компонентами,
   библиотеками программы и модели, необходимыми ресурсами, лицензиями и данными
   моделей, если они требуются этому выпуску. Исходный архив GitHub не является
   установочным пакетом. Пользователь не собирает программу и не устанавливает
   PyTorch либо CUDA Toolkit вручную.
2. Python не включать в пакет: использовать совместимый установленный либо
   предложить установку только Python. В примечаниях указывать проверенные ОС,
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
  `v<version>` tag consistent. Check both the project `Version` and the version
  written by `scripts/build_preview.ps1`. Never reuse a published version for
  changed binaries. A development-policy edit alone is not a new application release.
- Attach complete application packages, workers/native tools, application/model
  libraries, required model data/resources and licenses. The GitHub source archive
  is not an installable package. Reuse compatible Python or offer installation of
  Python alone; no user-side compiler, PyTorch or CUDA Toolkit setup.
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
