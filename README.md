# OpenShowQt

A Linux presentation application for lyrics, slides, images and video, in the
shape of FreeShow. FreeShow itself is Electron; this is a native rebuild with a
new architecture, not a fork.

The operator interface is C++ and Qt 6 Quick/QML. Output runs in a separate
Qt-free worker process: Skia over Vulkan for composition, FFmpeg for decoding,
GLFW for the output window and PipeWire for audio. The frontend owns documents
and compiles them into immutable render scenes, which it sends to the worker as
JSON-RPC over a Unix-domain socket. Preview frames go through shared memory
instead, so a slow UI never blocks the audience output.

The first release targets service playlists, lyrics, a basic visual editor,
images, video, audio, operator previews and one audience display. Stage
outputs, FreeShow file compatibility, Windows and macOS are deliberately out of
scope.

Nothing is usable yet. [PLAN.md](PLAN.md) holds the accepted architecture and
milestones; [PROGRESS.md](PROGRESS.md) records where the work actually stands.
