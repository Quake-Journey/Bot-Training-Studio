# Provenance and licensing

The initial Python motion-learning experiment was extracted from the
OpenTDM-X development repository, commits ff2d4dff55565b0bb1243bcbd8ac0c6f3f279dd7
and 1a6bdc7e9, on 2026-10-07. This project starts a separate history and does not
include the game module, private repository history, recordings or server files.
Subsequent changes belong to Bot Training Studio.

The desktop application is a new implementation. Mapgen Studio is the requested
visual/interaction reference; its active source files were not imported.

Preview 0.2 vendors the dependency closure of the standalone Q2PRO-X demo
decoder and offline MAPGEN BSP/Pmove adapter under `native/engine` (56 C/header
files). These are offline engine components, not OpenTDM-X game DLL code.
`native/source-manifest.json` records extraction-time hashes, not a claim that
local adapters remain unmodified. Studio adds detailed static traces and
continuous inverse-movement fitting; its own build script lists all targets.
Python `bts_analysis` contains the corresponding extracted decoder-table,
combat/grouping/motion helpers. Unused legacy mod-library compiler was omitted.
No active Mapgen/Claude checkout is needed to build or run the Studio.

Studio's DM2 adapter also recognizes terminal disconnect/reconnect playback
commands, rejects partial packet-length EOF and reports missing delta bases.
This changes only the vendored offline decoder, not either game's client or
module. Project decoder/transport changes invalidate the affected cached
observations and trigger re-decoding from original inputs.

The runtime builder uses the official [CPython embedded distribution](https://www.python.org/downloads/release/python-31316/)
with its published SHA-256 and pinned [PyTorch wheels](https://pytorch.org/get-started/previous-versions/#v2-10-0).
Runtime lock files preserve the exact wheel URLs and hashes. Binary runtimes
retain package `.dist-info` licenses and CPython's LICENSE; build outputs and
downloaded wheels are not checked into Git. Embedding follows the
[CPython distribution guidance](https://docs.python.org/3/using/windows.html#the-embeddable-package).

Project source is distributed under GPL-2.0-or-later; see LICENSE. Frameworks
and installed Python packages retain their own licenses. No pretrained model
weights or third-party game content are distributed in this initial source.
Binary packaging must preserve third-party notices for bundled components.
