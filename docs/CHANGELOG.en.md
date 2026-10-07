# Bot Training Studio

## 0.3.0-preview.3 — 2026-10-07

- Python 3.13 x64 is included in the complete package. If no ready environment is found, the startup dialog offers offline installation from the package without administrator access.
- Complete Python 3.12+ environments with the required libraries can be reused after computation checks; incompatible bundled binary libraries are not injected into them.
- Updated setup, integrity checks, application updates and RU/EN guides for bundled Python.

## 0.3.0-preview.2 — 2026-10-07

- Startup checks for Python and libraries now use a separate modal dialog; the installer no longer appears and disappears on Home.
- The GPU list contains physical adapters, excluding virtual displays, the Windows software renderer and unnamed counter entries. Two genuine GPUs of the same model remain separate.
- A newly extracted portable copy uses its own bundled learning worker instead of a previous installation folder.
- Updated RU/EN guides. Runtime checks do not overlap change notes or block UI event processing.

## 0.3.0-preview.1 — 2026-10-07

- Persistent CPU/GPU selection on Home, Training and Settings: checking Python no longer hides computation choices.
- Version in the title and interface; change notes on the first launch of a new version and an on-demand What's new button.
- Automatic GitHub Releases checks and a manual update button. Updates wait for jobs to finish, verify files and restore the previous build on installation errors.
- Application and model libraries are included; compatible installed Python is reused or installation of Python alone is offered.
- Application icon and updated Russian/English guides. This remains a research preview; full tactical learning and game-module export are not complete.

## 0.2.0 — 2026-10-07

- Initial development preview: map/recording import, experimental learning, memory profiles, RU/EN interface and resource monitoring.
