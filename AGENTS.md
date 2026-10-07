# Bot Training Studio

This is an independent application and local Git repository. Do not edit the
OpenTDM-X game module or MAPGEN Studio from this project. The initial learning
worker was extracted from the author-owned OpenTDM-X offline experiment; its
development now belongs here. See docs/PROVENANCE.md.

Product: autonomous map/gameplay/donor training, local GPU or CPU, with a
Mapgen Studio-like desktop interface. Public server runtime consumes bounded
validated data, never a model service, Torch or GPU. No user-side compiler.
The current motion experiment is NOT the final tactical model or VRAM ceiling.
Preserve the required 32GB/24GB GPU scaling, lower-memory profiles and CPU path.

Keep research, hardware capability, model learning, native gameplay validation,
export compatibility, PO acceptance and publication distinct. Never claim a
playable learned map from an observation-prediction loss alone. No artificial
green success for unsupported raw-demo/map training or unavailable hardware.

Public repository must contain source, reproducible setup and documentation;
never private recordings, server configs/secrets, developer absolute paths,
ML caches or user projects. Publish a checked intermediate build/source only.
Do not copy the old bot repository history or private operational reports.

Ready end-user builds belong in GitHub Releases for Quake-Journey/Bot-Training-Studio,
not just in source commits. Follow docs/RELEASING.md. Each delivered application
revision must have a new version; keep the EXE, UI, build.json, guides, archive
names and Git tag consistent. Maintain release notes as the net change from the
previous published version. Update both RU/EN guides for changed behavior and
include their DOCX files with the complete application/library package. Publish
checksums and verify the uploaded artifacts. A source backup is not a user release;
do not label an unfinished development preview as a finished user build.

Keep exactly one local runnable end-user build at dist/Release. Update this same
directory for each new version; do not create version-named runnable directories.
Keep only the current release archives in dist/packages. Published historical
versions remain in GitHub Releases. Preserve user data outside distribution files.
Development-only output must be explicitly requested with -DevelopmentOnly.

UI: RU/EN, light/dark/system themes, responsive background jobs, useful progress,
recoverable errors and bounded storage. Settings live outside tracked source.
Language defaults to the Windows user's UI language: Russian only for Russian,
English otherwise. Settings must offer system/Russian/English and preserve an
explicit choice. Maintain matching Russian AND English DOCX user guides under
docs with their Markdown sources; update both for user-facing changes. Rebuild
with scripts/build_user_docs.py, pass --check, inspect Word-rendered pages,
include both in packages and publish both on GitHub. Never add linked assets or
automatic external-field updates to a guide.
Use ProcessStartInfo.ArgumentList; workers receive structured JSON, never shell
command fragments. Preserve previous model generation on failure/cancel.
End-user packages must include application/model libraries (including required
GPU runtime DLLs); never require users to install PyTorch or full CUDA Toolkit.
Include the pinned official Python 3.13 x64 archive in complete packages. Reuse
compatible installed Python or a compute-verified complete Python 3.12+ environment;
otherwise offer private offline installation from the package. Do not download
Python or model libraries during user setup. This follows the PO's updated instruction.
Keep model computation, process/stream management and protocol parsing off the
UI thread. Bound/coalesce progress updates so worker output cannot flood the
dispatcher. Closing during a job must ask first, default to keeping work, and
wait asynchronously for cooperative shutdown after confirmation. Never silently
cancel on the first close click or kill a training process on normal exit.

Tests cover job protocol/cancellation, leak-free datasets, actual training and
checkpoint compatibility, resource selection and UI layout. Inspect rendered
UI before delivery. User communication is Russian informal ты; paths are full
absolute plain paths in code blocks. No runtime server launches for UI work.
