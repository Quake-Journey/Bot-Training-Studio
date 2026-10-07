# Private runtime setup

The application/model libraries are included in the end-user package. They
are prepared from a qualified builder runtime by `bundle_libraries.py`, without
copying Python site-packages. The pinned official interpreter archive is shipped separately in runtime/. Users never install PyTorch or the full CUDA Toolkit.

The application reuses an installed Python 3.13 x64 (selection, PATH, Windows
Python registration, previous managed installation). If missing it offers to
install the included **CPython archive offline**, using .NET; no Python/pip is needed for bootstrap. Complete Python 3.12+ environments can instead supply their own compute-verified libraries.
The private installation needs no admin rights, registry or PATH changes.
Mapgen Studio's install-or-choose workflow was reviewed and followed.

`packaging/runtime-{cpu,cuda}-win-x64.lock.json` is embedded in the EXE and
shared with the build-time bundler. URLs and SHA256 digests are pinned.
Bundled Python is SHA256-verified before extraction; unexpected
sources, corrupt downloads, archive links and escaping paths are rejected.
The installer never downloads model libraries. Package libraries are passed
to probes and workers explicitly, with system site-packages disabled. Moving
the application does not leave an absolute library path in Python's `_pth`.

Each attempt has an exclusive OS-held lock and its own staging folder beneath
the user's Studio home. A completed runtime must import its libraries, run a
Torch gradient calculation and fit a small sklearn tree before activation.
Only then is a generation renamed and its pointer atomically written. A failed
or cancelled attempt does not change the previous pointer or selected Python.
Temporary file locks after process exit are retried briefly on Windows.

Python selection validates the Windows PE/x64 header before execution. System
loader error boxes are disabled for this application: failures appear in its
UI rather than unexpected desktop dialogs. Runtime checks are bounded and
their subprocess is reaped on cancellation; learning jobs retain cooperative
cancellation. Setup runs in the background, with throttled progress and explicit
confirmation on exit. Navigation works while setup runs; computation cannot
start concurrently with setup. Existing custom environments are probed but
never upgraded in place.

## Verification

- `--runtime-test <new-directory>`: manifest, icon extraction, hash/network
  failures, invalid EXE, archive layouts, path boundaries and cancellation.
- `--setup-ui-test <new-directory> cuda` with a **fresh BTS_HOME**: real offline Python setup
  without Python on PATH, UI navigation, exit prompt, cancellation, retry,
  activation, reload, repeated setup, reuse of an existing bare Python and a
  real worker operation using the packaged libraries.
- `--setup-exit-test <new-directory>` after setup: stop-and-quit while installing
  into a separate test home, retaining the previous interpreter.
- `--ui-test <directory>` and `--lifecycle-test <python> <directory>`: rendered
  RU/EN UI and preservation of training cancellation/exit behaviour.

The end-to-end setup test installs bundled Python without network access. Run it in a disposable local
application home; never point it at real user settings or projects.

## Upstream contracts

- [CPython embedded distribution](https://docs.python.org/3.13/using/windows.html#the-embeddable-package):
  isolated application-local interpreter with application-managed dependencies.
- [Wheel installation scheme](https://packaging.python.org/en/latest/specifications/binary-distribution-format/):
  relocate purelib/platlib/data/scripts/headers within the installation scheme.

These sources were checked on 7 October 2026. CPU and CUDA manifests retain the
already tested versions; AMD/Intel remain separate unqualified backends.

## Previous package verification (before bundled Python)

- 24 offline checks passed, including PE validation before execution and icon
  extraction through the Windows shell.
- 26 setup/UI checks passed with no Python on PATH: only Python was downloaded;
  the package supplied the scientific/model libraries. Existing bare Python
  was subsequently discovered and reused without copying it.
- 7 exit-during-setup checks passed; the old interpreter remained selected.
- 18 existing job lifecycle checks passed, including cooperative training exit.
- The newly installed interpreter loaded PyTorch 2.10.0+cu128 from the package
  and completed forward/backward computation on the RTX 5090. These checks
  validate setup/computation, not tactical-model quality or AMD/Intel support.
- Both updated eight-page manuals opened read-only and rendered in Word.

## AMD / Intel distribution plan

They need their own compatible, bundled compute libraries, not CUDA Toolkit.
Backend selection must be checked against the OS/GPU/driver and an actual
model training step before claiming support. Unsupported hardware retains CPU.
The user must not have to assemble a Python/PyTorch/driver dependency recipe.

For Intel, the native PyTorch XPU distribution supports documented Intel Arc
and Core Ultra GPU families on Windows. See the [PyTorch 2.10 XPU matrix](https://docs.pytorch.org/docs/2.10/notes/get_start_xpu.html).
For AMD, native Windows ROCm/PyTorch bundles have a version-specific GPU list;
the [ROCm 7.2.1 Windows matrix](https://rocm.docs.amd.com/projects/radeon-ryzen/en/latest/docs/compatibility/compatibilityrad/windows/windows_compatibility.html)
uses Python 3.12, unlike this CPU/NVIDIA package's Python 3.13 ABI. Newer
ROCm releases require a fresh matrix/package check. Therefore AMD cannot be
enabled simply by renaming the CUDA selector. Neither vendor is advertised as
hardware-qualified by the current preview.
