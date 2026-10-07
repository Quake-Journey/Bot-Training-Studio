# Bot Training Studio by ly

User guide • English • Version 0.3.0-preview.4, development preview

Updated: 7 October 2026

## 1. Purpose and current capabilities

Bot Training Studio is a standalone application for studying match recordings and training models for OpenTDM-X bots locally. Models run on your computer, using a GPU or CPU. Recordings and maps are not uploaded to the cloud.

This preview supports map projects, BSP and DM2/MVD2 import, route and observed-style analysis, dataset preparation, training, learning new data with replay of previous experience, and checkpoints. The interface and computation run independently.

**The full tactical model is not ready yet.** Predicting recorded actions accurately does not prove that a bot will play a good match. Weapon selection, item control, complete trick execution and tactics need further validation. Generated packages are currently for offline review inside the Studio: they cannot be installed in the mod's current game DLL.

The future game module will read validated data rather than run a neural network on the server. This requires an official update to the mod's loader; users will not need to compile the DLL themselves.

### Quick workflow

1. Extract the complete application package and launch **BotTrainingStudio.exe**.
2. Choose your language and theme in **Settings**. Check hardware in **Models & GPU**.
3. In **Map & demos**, select the BSP, match recordings, separate trick recordings and project folder.
4. Import the data, then analyze routes and style.
5. In **Training**, add projects, prepare a dataset and select a separate model folder.
6. Measure a suitable batch size, start training and review results on independent matches.
7. Keep the previous generation. For now, use packages from **Library** only for offline review.

<!-- page -->

## 2. Installation and interface settings

The current desktop build targets Windows x64. Extract the whole folder: **worker**, **docs**, **libraries**, **Models**. Application and model libraries are included. Compatible installed Python or a complete Python 3.12+ environment is reused. Otherwise, the included Python 3.13 x64 can be installed offline (section 8). No manual library or CUDA Toolkit installation is needed. RAR requires WinRAR/UnRAR.

CPU and NVIDIA CUDA have been tested. ROCm and Intel XPU code paths exist, but support for specific AMD/Intel GPUs has not yet been hardware-qualified. GPU profiles do not have a universal 4 GB VRAM limit. A larger profile requires more resources and does not by itself guarantee better quality.

At startup, a separate modal dialog checks Python and the bundled libraries in the background. It closes after a successful check. If Python is not ready, install it from the package, open Settings or continue without training. You can quit during the check. The installer does not appear on Home; the CPU/GPU computation setting stays in place.

After extracting a complete build into a new folder, the Studio uses that build's **worker**. An explicitly selected standalone development worker is preserved.

### Language: Settings → Language

| Choice | Behaviour |
| --- | --- |
| Use system language | Default. Russian when the system UI language is Russian; English in all other cases. |
| Русский | Russian UI regardless of the system language. |
| English | English UI regardless of the system language. |

Changes apply immediately and persist between launches. Detection uses the Windows user's UI language, not regional number formatting. An existing explicit selection is preserved; choose **Use system language** to return to automatic selection.

Theme is selected on the same page: **dark**, **light** or **system**. Technical log keys and third-party runtime messages may remain in English. Changing the language does not translate previously recorded logs again.

### Application pages

| Page | Purpose |
| --- | --- |
| Home | Runtime readiness and access to training. |
| Map & demos | BSP, recordings, import, routes, style and motion checks. |
| Training | Dataset preparation, training, learning new data and resuming. |
| Models & GPU | Device discovery, profiles and computation checks. |
| Library | Creation and verification of offline packages. |
| Jobs | Progress, logs, cancellation and results. |
| Settings | Language, theme, Python installation and runtime selection. |

<!-- page -->

Hover over fields, buttons, settings or load indicators for explanatory tooltips in the selected interface language.

## 3. Map, recordings and project

Each project uses the BSP of one map. List match inputs as DM2, MVD2, ZIP or RAR files, one path per line. Add separate movement demonstrations to the trick-recordings field. Keep the originals: an analyzer update may require re-importing them.

**Inspect inputs** lists recordings. **Import / update** adds material to the project and reuses the verified cache for unchanged recordings. **Fresh selection** creates a fresh selected collection; this is separate from training a new model. Earlier project revisions are preserved. Choose a new project folder for a completely independent experiment.

Import checks the map, recordings and provenance. Decoder or observation-format changes may require decoding again. If an original input is missing, restore it and repeat the action. An error or cancellation must not replace the active revision with incomplete output.

After importing, use **Analyze routes and style**. **Verify motion through physics** checks reconstructed motion fragments. A successful short fragment does not mean that a complete difficult trick has already been learned and can be executed reliably.

### Style donor

The **Style donor** field limits analysis to a player; leave it empty to use all eligible players. Name formatting is normalized and similar names can be matched. Review the identified player in the results: a similar nickname does not guarantee the same person.

Current analysis describes observed habits: movement, weapons and behaviour under different conditions. It is not yet a complete player clone. Phrases must belong to the selected donor. Including chat in a package is a separate option, disabled by default.

### Input quality

- Use several independent matches, opponents and situations.
- Do not treat two recordings of one encounter as independent examples.
- DM2 does not contain every hidden state or original player command.
- An item in a network frame is not necessarily visible to the player; an absent item does not prove that it is unavailable on the map.
- A small or repetitive dataset cannot reliably establish tactical quality.

<!-- page -->

## 4. Dataset preparation and model training

In **Training**, list project folders, one per line. **Add current map** adds the project from the import page. Select the observed-history length and the maximum examples in each dataset partition. Longer histories require more memory and must fit the selected profile's context.

**Prepare project data** creates a sequence-model dataset. Related matches are split as whole groups into training, generation selection and final evaluation. Evaluation recordings must not leak into training. If there are too few independent groups, add recordings rather than mixing partitions.

### Profile and device

**Computation: CPU / GPU** is always available on Home and Settings, and in training options. It remains visible after Python validation and persists across launches. **Automatic** chooses an available GPU, otherwise CPU. You can explicitly select **NVIDIA GPU (CUDA)** or **CPU — processor**. Experimental AMD/Intel choices require matching libraries; this package does not qualify their support. The GPU list in the load panel changes monitoring only, not training computation.

| Setting | Purpose |
| --- | --- |
| auto | Select an available accelerator, with the implemented CPU fallback for insufficient memory during training. |
| cpu / cuda | Explicit CPU or NVIDIA CUDA selection. |
| rocm / xpu | Experimental AMD/Intel paths requiring a compatible environment and separate validation. |
| Compact / Balanced / Large / XL | Increasing experimental model capacity. Resource measurements must confirm the selection. |

**Measure batch size for memory and throughput** executes a real computational step of the selected model. The result applies to that model family. It does not measure gameplay quality. If memory is insufficient, reduce the batch size, context or profile; explicitly selecting a GPU does not imply automatic CPU fallback.

### Work modes

- **Train new generation** — train from scratch in a new model folder.
- **Start from factory model** — create a separate user overlay. Requires Compact or Balanced, 16 history frames, an empty donor and a new model folder.
- **Update** — learn new material while replaying earlier experience and checking its retention.
- **Resume** — continue an interrupted computation from a completed checkpoint, using compatible data and settings.

Do not mix model families, incompatible formats or profiles in one folder. Preserve the earlier generation separately. Cancellation does not turn an intermediate model into an accepted result.

### Separate experiment: shots and item pickups

The lower part of the page contains **Shots and item pickups**. It needs projects re-imported with the current format, separate event preparation, a separate model folder and its own batch-size measurement. This experiment learns observed shots and pickup messages, not proven optimal gameplay decisions.

<!-- page -->

## 5. Background work and hardware load

Computation, process launch and message handling run in the background. You can switch pages, read logs and change settings for the next job while work is running. One compute job runs at a time, protecting models and projects from concurrent writes.

The panel at the bottom updates approximately once per second:

| Reading | Meaning |
| --- | --- |
| CPU | Overall utilization of the computer's logical processors. |
| RAM | Used and total physical system memory, in GiB. |
| GPU | Utilization of the busiest hardware engine on the selected adapter. |
| Video memory | Used dedicated memory and the selected adapter's capacity reported to the system. |
| Job | CPU and resident RAM of the compute process and its live child processes. |

Selecting a GPU in this panel changes **only the readings**; select the training device in the job settings. Unavailable readings appear as a dash. Shared GPU memory is shown in the tooltip. Capacity can differ from the advertised amount, and summing child-process RAM can count shared pages more than once.

The monitoring selector lists physical GPUs. Virtual displays and the Windows software renderer are excluded. Two genuine cards of the same model remain separate, numbered entries. This selector chooses load readings; training computation is selected separately.

### Cancellation and exit

Cancel the current job on the **Jobs** page. Closing the window while a job runs opens a question. **Keep working** is the default and leaves computation running. **Stop and quit** requests a cooperative stop and closes the window after the process finishes.

The previous model and completed checkpoints are preserved. Unfinished work within the current step may be lost. The interface remains usable while stopping. **Stay in the application after stopping** cancels automatic exit but does not resume computation that is already being cancelled.

Opening the question does not cancel work by itself. If execution fails, the window stays open with the result. Forcibly terminating the application through the operating system is not cooperative cancellation.

<!-- page -->

## 6. Results, library and data preservation

**Jobs** shows progress, logs, a result summary and the job-folder path. A complete successful result is saved as **result.json**; **events.jsonl** records progress and **stderr.txt** contains runtime diagnostics.

Compare the model against simple baselines, and inspect individual maps, rare events and independent matches. Overall weapon accuracy can be high just because the recorded player held the same weapon for a long time. That does not establish good switching, control or shooting.

**Library** builds an offline package from the project and analysis output, verifies its integrity and adds it to the local library. Including a model and chat is optional. Components that failed validation must not be represented as ready for gameplay.

**The current package cannot be installed on a game server.** Passing an integrity check means that the data can be read without corruption, not that a new map is ready for matches.

### Factory bases and your overlays

The **Models** folder beside the EXE contains factory Compact and Balanced experimental models trained on q2duel5 and ztn2dm3. These are observation models, not complete bot tactics. User training never changes their weights. Large/XL remain research architectures without bundled pretrained weights.

**Start from factory model** pins a copy of the exact base in your **bases** folder and trains a separate **user-delta.safetensors** overlay. Export loads the base followed by the overlay. Incompatible schemas, profiles and contexts are rejected. A full snapshot is also retained for recovery. This is a full weight delta, not LoRA.

Application updates preserve user generations. If a newer release ships a different factory model, existing overlays remain bound to their original base; new weights are not silently mixed in. Original factory recordings are not distributed, so learning on your recordings alone does not establish retention of all prior skills.

### Data locations

The default settings and workspace root is:

```text
%LOCALAPPDATA%\QuakeJourney\BotTrainingStudio
```

Settings are in **settings.json**. The main folders are **projects**, **datasets**, **UserModels**, **jobs**, **exports** and **library**. Developers can override the root with **BTS_HOME** for isolated tests. Manually selected project and model folders stay in their selected locations. Legacy **models** folders are preserved; **UserModels** is the default for new settings. Keep training outside application files.

For recovery, preserve projects, original BSP files and recordings, model folders with history and checkpoints, settings and required exported packages. Publishing source on GitHub does not back up your local recordings or trained weights.

<!-- page -->

## 7. Troubleshooting

| Situation | Action |
| --- | --- |
| Runtime not found | Check the complete libraries/worker package and installed Python. Set Python and worker paths in Settings if needed. |
| GPU not detected | Run the hardware check. Verify that the installed runtime supports the GPU; select CPU for a basic check. |
| Insufficient memory | Reduce batch size, context or profile and repeat resource measurement. Keep the previous model. |
| RAR cannot be opened | Install WinRAR/UnRAR or provide extracted DM2/MVD2 files or ZIP. |
| Project format is outdated | Re-import original recordings and prepare the dataset again. Use a new folder for an incompatible model. |
| Too few independent matches | Add new encounters. Do not move evaluation data into training. |
| Update rejected | Review retention results for earlier maps. Preserve the old generation and inspect the new material. |
| Stopping takes time | Wait for the current operation and saving to finish. You can stay in the application; its window should remain responsive. |

For a recurring failure, preserve the application version, action, **request.json**, **events.jsonl**, **stderr.txt** and a description of the inputs. Review personal paths, chat and private recordings before sharing. Do not publish keys or server configurations.

### Documentation and updates

The Russian and English guides ship in the application's **docs** folder: **Bot_Training_Studio_User_Guide_RU.docx** and **Bot_Training_Studio_User_Guide_EN.docx**. The repository contains the same DOCX files and editable Markdown sources. Both guides are maintained alongside user-facing changes.

Repository: https://github.com/Quake-Journey/Bot-Training-Studio

Builds: https://github.com/Quake-Journey/Bot-Training-Studio/releases

The project is distributed under GPL-2.0-or-later. Dependency licenses remain in the package. File names, technical commands and format keys are the same in both languages.

<!-- page -->

## 8. Installing bundled Python

Settings contains **Python interpreter**. Application and model libraries ship in **libraries**; the verified official Python 3.13 x64 archive ships in **worker/runtime**. Startup validates the selected environment with a small computation.

1. If a ready environment is found, work immediately. **Select installed Python…** allows manual selection. Python 3.13 uses Studio libraries; complete Python 3.12+ environments can use their own libraries after computation checks.
2. Otherwise, click **Install Python** in the startup dialog or **Install bundled Python** in Settings. The Studio verifies the local archive's SHA256 and extracts it into its own data folder. No internet or administrator access is required.
3. Follow progress at the bottom of the window. Navigation remains available. Training is disabled during setup.
4. After extraction, the Studio checks libraries and computation. Only successful setup becomes active, and the choice is saved.

Allow about **100 MiB of free space including headroom**. Package libraries are not downloaded or copied again. NVIDIA needs a compatible driver, not the full CUDA Toolkit. CPU works without a GPU. AMD/Intel GPU paths are not separately qualified yet.

### Finding Python

The Studio checks selected Python, its managed installations, PATH and Windows registrations. Packaged binary libraries require Python 3.13 x64. Complete Python 3.12+ environments are checked separately; incompatible bundled libraries are not injected. If **libraries** or **worker/runtime** is missing, extract the complete package.

### Cancellation and retry

**Cancel installation** stops extraction or validation and removes incomplete attempt files. Closing first asks whether to keep working. The previous environment is preserved. For a corrupt archive, extract a verified complete package again. A healthy installed Python is reused.

Installed Python lives in **runtimes** under the Studio data folder (section 6). Setup does not change PATH or other Python installations. Do not delete selected Python while the application is running.

<!-- page -->

## 9. Version, change notes and updates

The exact version appears in the window title and sidebar. **Version history** opens on the first launch of a new version and includes every release, including previously viewed versions. Scroll to reach earlier releases. Once acknowledged, it is not automatically repeated for that version. The **What's new** button in the sidebar and settings opens the full history at any time, offline.

**Settings → Version and updates** enables automatic checks by default; you can turn them off. **Update** and **Check for updates** use published GitHub Releases. A preview build sees newer previews; drafts are excluded. A network failure does not mean the installed version is current.

Installation requires choosing **Update** rather than **Later**. Updates cannot install during training or Python setup: finish the job and check again. Downloading and verification run in the background. Closing during a download offers to keep working or cancel the download and exit, preserving the installed application.

After checksum verification, the Studio closes, a separate helper replaces application files and restarts the new version. Ordinary replacement errors restore the previous files. Projects, models, settings and user files are preserved. Unchanged bundled libraries are verified and are not downloaded again. Replacing libraries requires additional free space; temporary **.updates** files are stored beside the application and successful downloads are cleaned after restart.

For a first installation, download all **BotTrainingStudio-…-win-x64.part…rar** volumes into one folder and extract the first volume with a RAR-capable archiver. All libraries are included. Separate **app** and **libraries.zip.00…** assets are for the built-in updater; GitHub's automatically generated **Source code** archives are for developers. DOCX guides are also attached separately. A complete release can be extracted into a new folder; projects and settings are stored separately.

<!-- page -->

## 10. Natural-language training instructions

This is a planned feature; this build does not contain a chat assistant. Current models consume gameplay observations rather than free-form text.

A separate local language assistant is planned: describe a problem in Russian or English and optionally specify a demo and timestamp. It proposes a reviewable task: conditions, map or style scope, evidence to learn from and criteria for checking the result. You review whether it understood your instruction correctly.

The Studio must then train a new user generation and check physical feasibility, mod compatibility and retention on previous maps. Text alone does not change model weights or add missing game DLL capabilities. Arbitrary scripts from model responses are not executed.

Instructions, clarification history and results will be separate from factory data and preserved by updates. The language model runs only in the Studio; the game server still receives bounded validated data.
