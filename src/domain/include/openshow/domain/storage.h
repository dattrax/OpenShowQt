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
 * longer depend on wherever the original files happened to live. Saving
 * is a commit (task 6.13a, review R8): each asset streams into a
 * staging file inside assets/, is synced, and is renamed into place;
 * the manifest is written to a temporary in the same directory, synced,
 * and renamed; the directories are synced before success. Nothing a
 * live manifest references is ever written in place, so at every instant
 * an interrupted save leaves service.json naming complete files — the
 * old copy_file(overwrite) could leave HALF a file under a name the
 * still-valid manifest named; that hole is what this closes. A changed
 * asset gets its new version under the same name, so the previous
 * manifest never resolves it to torn bytes but could resolve it to the
 * whole newer ones. saveServiceAndAdopt() carries the promise one step
 * further and retargets the MODEL at the package: afterwards the
 * originals are releasable while the session runs.
 *
 * loadService() replaces *documents only on success — a corrupt,
 * truncated, future-format or reference-dangling file fails with a
 * message naming the problem and leaves the target untouched. Ids are
 * restored verbatim, and the id counter resumes from the file, so
 * later operations never recycle an id the file already used. Loaded
 * ids are validated as names, never paths (letters, digits, '-' and
 * '_'; an asset's id is the prefix of the filename its bytes are
 * written under), and asset files are contained to the package —
 * absolute paths, climbing paths and paths whose real location (every
 * symlink followed) sits outside the package directory are refused
 * naming the asset (task 6.13b, review R8). Authorised import of
 * external files at save time is unaffected: that is the operator's
 * choice, not a manifest's.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>

#include "openshow/domain/documents.h"

namespace openshow::domain {

// Writes directory/service.json (+ assets/ when there are assets),
// creating the directory when needed. The write is a COMMIT (task 6.13a,
// review R8): every asset streams into a staging file inside the
// package's assets directory, is synced, and is renamed into place; the
// manifest is written to a temporary, synced, renamed over service.json,
// and both directories are synced before success is reported. No file a
// live manifest references is ever written in place, so at every instant
// — including mid-save, including a process kill — service.json names
// only complete files, and a save that fails names the step it failed
// at and leaves the previous package readable (the single exception, a
// failure of the FINAL directory sync, says so and leaves a consistent
// new one). A save that returns true has synced every byte and every
// naming directory entry to the storage device: the commit survives
// subsequent process crashes and power loss, as far as a device that
// answers fsync honestly can promise. OPENSHOW_STORAGE_FAULT (see
// storage.cpp) injects per-step refusals so fixtures can prove it. An
// asset whose source file is missing fails the save naming the asset.
// Re-saving a service loaded from this package is byte-identical: the
// file records package-relative paths, never the loaded sourcePath.
bool saveService(const Documents& documents, const std::filesystem::path& directory,
                 std::string* error = nullptr);

// Reads directory/service.json into *documents. See the header for the
// validation contract; a failed load leaves *documents unchanged.
bool loadService(Documents* documents, const std::filesystem::path& directory,
                 std::string* error = nullptr);

// The same save, then ADOPT: on success every asset's sourcePath becomes
// the package-owned file its bytes were just copied to, and *rebinds (when
// given) records one entry per moved asset (id, old path, new path). The
// model is not touched when the save fails — no copy happened, so nothing
// has a new home — and an asset whose bytes were already inside the
// package (a loaded service, unchanged) produces no rebind at all.
//
// This is what makes a package an asset's permanent home rather than one
// copy among several. Without it the live document keeps pointing at
// whatever file it last saw — the original import, or a scratch autosave
// tree a recovery loaded from — and deleting that file while the session
// runs breaks the next present, save or autosave tick even though the
// bytes are safe inside the package (task 6.12, review R7). Callers that
// observe the model anywhere else — history snapshots, cached scene
// payloads — must apply the reported rebinds too.
bool saveServiceAndAdopt(Documents& documents, const std::filesystem::path& directory,
                         std::vector<AssetRebind>* rebinds = nullptr,
                         std::string* error = nullptr);

// Which asset bytes a package already holds (task 6.14b, review R14).
// Autosave ticks rewrite the whole package, but text edits do not change
// media — re-copying a gigabyte of video after every keystroke burst is
// exactly how R14 froze the UI. A save given an AssetReuse ledger may
// skip an asset copy only when BOTH sides still identify: the source is
// the same file (canonical path, size and modification time as when it
// was last copied in) AND the destination file still exists. Stat
// identity is the contract's edge: content changed in place while size
// and mtime stayed identical is invisible to it — every path the store
// itself takes (re-import, replaced file, renamed source, deleted
// scratch) moves at least one of the three. Entries are keyed by
// destination file and only ever written after the bytes are installed;
// a failed save leaves them describing the whole file that is still
// there, so the next save re-checks and re-copies where needed.
struct AssetReuse {
    struct Stamp {
        std::filesystem::path canonicalSource;
        std::uintmax_t size = 0;
        std::filesystem::file_time_type modified{};

        bool operator==(const Stamp& other) const {
            return canonicalSource == other.canonicalSource && size == other.size &&
                   modified == other.modified;
        }
    };
    std::map<std::string, Stamp> byDestination;
};

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
// cheap — and where the state did change, unchanged assets ride the
// AssetReuse ledger instead of being copied again.
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
    AssetReuse fReuse;          // installed asset bytes (task 6.14b)
};

}  // namespace openshow::domain
