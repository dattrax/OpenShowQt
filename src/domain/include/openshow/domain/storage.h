/*
 * Service persistence (task 2.2): a whole Documents saved to, and
 * reloaded from, a directory package.
 *
 *   <directory>/
 *     service.json   — the entire model: shows, slides, groups (as
 *                      slide labels), styles, playlists and assets,
 *                      ids included, in "format": 1
 *     assets/        — every asset's bytes, copied in at save under
 *                      a collision-proof "<asset id>-<sanitised name>"
 *
 * The package is self-contained: after saveService() the assets no
 * longer depend on wherever the original files happened to live. Save
 * writes service.json through a temporary file in the same directory
 * and renames it, so an interrupted save leaves the previous file
 * intact; asset copies run before the rename, so a service.json that
 * exists always names files that exist.
 *
 * loadService() replaces *documents only on success — a corrupt,
 * truncated, future-format or reference-dangling file fails with a
 * message naming the problem and leaves the target untouched. Ids are
 * restored verbatim, and the id counter resumes from the file, so
 * later operations never recycle an id the file already used.
 */

#pragma once

#include <filesystem>
#include <string>

#include "openshow/domain/documents.h"

namespace openshow::domain {

// Writes directory/service.json (+ assets/ when there are assets),
// creating the directory if needed. An asset whose source file is
// missing fails the save naming the asset. Re-saving a service loaded
// from this package is byte-identical: the file records package-
// relative paths, never the loaded sourcePath.
bool saveService(const Documents& documents, const std::filesystem::path& directory,
                 std::string* error = nullptr);

// Reads directory/service.json into *documents. See the header for the
// validation contract; a failed load leaves *documents unchanged.
bool loadService(Documents* documents, const std::filesystem::path& directory,
                 std::string* error = nullptr);

// Periodic crash insurance (task 2.3): keeps a live service mirrored
// into a scratch package so an interrupted session can recover. The
// package format is the one above, so recovery is just loadService and
// the crash-ordered save (assets, then tmp+rename manifest) means an
// autosave package is always loadable or absent — never half-written.
//
// Nothing here schedules itself: the editor (or a test) calls tick()
// when it likes — after each edit, from a timer, wherever. tick()
// compares the serialised state against what it last wrote and skips
// the disk entirely when nothing changed, so calling it liberally is
// cheap.
class Autosave {
public:
    explicit Autosave(std::filesystem::path directory);

    const std::filesystem::path& directory() const { return fDirectory; }

    // Writes the package when the state differs from the last write
    // (manifest bytes compared; the codec is deterministic, so equal
    // bytes mean an unchanged service). An unchanged state performs no
    // disk access at all.
    bool tick(const Documents& documents, std::string* error = nullptr);

    // Loads the scratch package into *documents — that is the whole
    // recovery story — and adopts it as the new baseline. After the
    // operator decides, the caller owns the recovered Documents and
    // calls clear() once the service itself has been saved (or the
    // recovery discarded).
    bool recover(Documents* documents, std::string* error = nullptr);

    // Removes the scratch package (used after a real save, or when the
    // operator declines recovery).
    bool clear(std::string* error = nullptr);

    // True when a scratch package is waiting (manifest present).
    static bool exists(const std::filesystem::path& directory);

private:
    std::filesystem::path fDirectory;
    std::string fLastManifest;  // what tick() last wrote ("" = never)
};

}  // namespace openshow::domain
