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

UI: RU/EN, light/dark/system themes, responsive background jobs, useful progress,
recoverable errors and bounded storage. Settings live outside tracked source.
Use ProcessStartInfo.ArgumentList; workers receive structured JSON, never shell
command fragments. Preserve previous model generation on failure/cancel.
Keep model computation, process/stream management and protocol parsing off the
UI thread. Bound/coalesce progress updates so worker output cannot flood the
dispatcher. Closing during a job must ask first, default to keeping work, and
wait asynchronously for cooperative shutdown after confirmation. Never silently
cancel on the first close click or kill a training process on normal exit.

Tests cover job protocol/cancellation, leak-free datasets, actual training and
checkpoint compatibility, resource selection and UI layout. Inspect rendered
UI before delivery. User communication is Russian informal ты; paths are full
absolute plain paths in code blocks. No runtime server launches for UI work.
