#include <QCoreApplication>
#include <QTemporaryDir>
#include <cstdio>
#include "DocumentController.h"

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir scratch;
    openshow::ui::DocumentController docs(nullptr, scratch.path().toStdString());
    bool ok = true;
    auto check = [&](bool value, const char* message) {
        if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); ok = false; }
    };
    docs.createSong("Repeated presentation");
    const auto a = docs.currentSongId();
    docs.addShowToPlaylist(a);
    docs.addShowToPlaylist(a);
    docs.createSong("Other presentation");
    docs.addShowToPlaylist(docs.currentSongId());
    const auto before = docs.playlistEntries();
    const auto first = before[0].toMap().value("entryId").toString();
    const auto middle = before[1].toMap().value("entryId").toString();
    const auto last = before[2].toMap().value("entryId").toString();
    check(docs.removePlaylistEntries({first, last, first}), "batch removes unique selected entry identities");
    check(docs.playlistEntries().size() == 1
          && docs.playlistEntries()[0].toMap().value("entryId") == middle,
          "another occurrence of the same presentation stays in place");
    check(docs.songs().size() == 2, "service removal preserves Library sources");
    docs.undo();
    check(docs.playlistEntries() == before, "one undo restores the entire batch in its original order");
    check(!docs.removePlaylistEntries({first, "missing"}) && docs.playlistEntries() == before,
          "a stale identity refuses the whole batch without removing a valid entry");
    docs.redo();
    check(docs.playlistEntries().size() == 1, "refusal preserves redo history");
    return ok ? 0 : 1;
}
