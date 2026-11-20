# Service-card thumbnails in JSON (TC)

Status: TC.1 contract frozen 2026-11-20. Implementation leaves TC.2–TC.10
remain open; the section brief in `PLAN.md` is the authority for intent, this
file for the frozen shapes.

## What is being kept

The worker already paints each service card. Until now the picture lived only
in `PreviewConsumer`'s byte-bounded LRU and died with the stream, so a reopened
service paid the whole path again: scene prepare, worker render, shared-memory
copy. The cache keeps the finished *slide surface* as a Base64 PNG inside
`service.json`, next to the authoritative shows. Card numbers, captions,
selection rings and LIVE borders are operator chrome and stay in QML — the
record stores exactly the pixels the worker rendered for a scene.

## Placement and version

`service.json` gains one optional root member:

```json
"thumbnailCache": {
  "format": 1,
  "entries": [
    {
      "showId": "show3",
      "slideId": "slide17",
      "fingerprint": "<64 lowercase hex>",
      "renderer": "openshow-scene-card/1;app=0.1.0",
      "width": 512,
      "height": 288,
      "pngBytes": 3809,
      "checksum": "<64 lowercase hex sha256 of the PNG bytes>",
      "pngBase64": "iVBORw0KGgo..."
    }
  ]
}
```

The service manifest stays **format 15**: the block is additive, independently
versioned (`thumbnailCache.format` = 1) and derived, so an older build that
reads a 15-file without knowing the key simply ignores it. An absent block, an
unsupported block format, an unknown entry key, a malformed record or a
checksum/header failure is a **miss**; the authored document must load first and
always. Only syntactically broken JSON keeps the existing load-failure
contract.

Entries are keyed by owning `showId` + `slideId` (slide ids are unique across
one service) and a render fingerprint. Repeated service occurrences of one
slide share one record; duplicate occurrences never produce a second picture.

## Entry limits (checked before allocation)

| Bound | Value | Where enforced |
|---|---|---|
| entries per service | 256 | codec parse, store eviction |
| PNG bytes per entry | 32 KiB | parse (declared length) and encode |
| Base64 bytes per entry | 44 KiB (`4*ceil(32K/3)`) | parse |
| total `pngBase64` bytes | 1.5 MiB | parse, store eviction |
| decoded width/height | 2..512 / 2..288 | parse + PNG IHDR check |
| aspect | within 2% of 16:9 | parse + IHDR check |

`pngBytes` must equal both `4*ceil(len/3)`-consistent Base64 length and the
decoded byte length. Base64 is the standard alphabet with padding and a strict
decoder (no whitespace, no URL alphabet). The checksum is verified on the
decode lane before PNG decoding, so a corrupt blob costs no allocation.

## Size policy (frozen)

One canonical 16:9 bucket: **512x288** — the largest frame the current
thumbnail envelope accepts at 16:9. A finished render is recorded by scaling it
to this bucket (down or up; a proxy picture is what this cache is for). A
persisted hit is decoded at 512x288 and smooth-scaled to the extent the list
asked for. Consequences:

- Window/zoom changes and moving between Show and Edit reuse the same record;
  the display extent is no longer part of the fingerprint.
- Resident images stay exactly as they are today: the *scaled* display-size
  image enters the existing 128-entry / 8 MiB LRU, so the 8 MiB budget is not
  raised and a 30-row window at a small cell keeps all its pictures. (At
  512x288 each resident image is 576 KiB; the old exact-size behaviour hit the
  same ceiling, which is why the persisted picture is not kept at that size in
  the LRU.)
- Re-encoding is capped at the 32 KiB per-entry bound; a picture that exceeds
  it is simply not persisted (a miss on the next run, never a failure).

### Measurements that chose it

Real worker card renders (the repository's 162 `scene-render-*.png` outputs,
960x540) re-encoded with Qt at the candidate extents, 2026-11-20, same machine:

| extent | PNG avg | PNG max | Base64 avg | decode base64 | decode PNG |
|---|---|---|---|---|---|
| 256x144 | 1 723 B | 9 064 B | 2 298 B | 0.01 ms | 0.11 ms |
| 320x180 | 2 128 B | 10 571 B | 2 838 B | 0.01 ms | 0.11 ms |
| 512x288 | 3 809 B | 19 531 B | 5 080 B | 0.02 ms | 0.25 ms |

JSON costs 282 bytes per entry beyond its Base64 string; a full 200-card cache
at 512x288 is ~1.0 MiB of manifest. Because 512x288 costs only ~1.3 ms/100
cards of decode and buys one shared bucket for every consumer, exact-dimension
records and a second smaller bucket were both rejected. Base64 inflates the
PNG by 33.4%; the plan's "no speedup is claimed" still holds — TC.10 measures
the end-to-end effect.

## Fingerprint

`domain::renderFingerprint(...)` hashes a length-prefixed canonical string:

```text
openshow-thumbnail-fingerprint/1\n
renderer\n  <renderer identity>\n
policy\n    card-surface/1\n
extent\n    512x288\n
scene\n     <normalized scene JSON>\n
assets\n    <assetId>\x1f<byte size>\x1f<content probe>\n ... sorted
```

- **Normalized scene JSON**: the compiled scene payload parsed and re-serialized
  with every `assetPath`, `showId` and `slideId` value emptied. Package-local
  paths therefore cannot spoil a hit when a service moves, and a Library
  re-preparation — which mints new ids — still matches the look it re-renders;
  the asset identity below carries the bytes.
- **Asset identity**: for every asset the scene references, `assetId`, the file
  size and a probe hash over the first 4 KiB, the last 4 KiB and the size. The
  probe is memoized by (path, size, mtime), so the UI thread does not re-read
  bytes for an unchanged asset. This survives save-adopt (the package copy is
  byte-identical, its mtime is irrelevant) and detects in-place replacement
  with the same caveat as `AssetReuse`: a change that preserves size and both
  probed ends is invisible.
- **Renderer identity**: `openshow-scene-card/1;app=<applicationVersion>`.
  Fonts, fallbacks and renderer behaviour change only with a build, and a build
  bumps the version. Until the worker reports a font-environment identity this
  is the conservative substitution the plan allows; a scene needs no new
  worker metadata.
- Video, camera and time-driven content is **not persisted** (`bypass` in the
  policy): a still of live content is a lie, and it would never hit.

## Overlay API (owner: persistence, consumer: cache runtime)

Qt-free, in `openshow/domain/thumbnails.h`:

```cpp
struct ThumbnailCacheEntry {
    std::string showId, slideId, fingerprint, renderer;
    std::uint32_t width = 0, height = 0;
    std::uint64_t pngBytes = 0;
    std::string checksum, pngBase64;
};
struct ThumbnailCache {
    std::uint32_t format = kThumbnailCacheFormat;
    std::vector<ThumbnailCacheEntry> entries;
};
bool validateThumbnailCache(const ThumbnailCache&, std::string* error);   // bounds only
std::string pngChecksum(std::string_view png);                            // sha256 hex
std::string sha256Hex(std::string_view);
std::string base64Encode(std::string_view);
bool base64Decode(std::string_view, std::string* out);
const ThumbnailCacheEntry* findThumbnail(const ThumbnailCache&, std::string_view slideId,
                                         std::string_view fingerprint);
```

Service codec (`openshow/domain/storage.h`):

```cpp
bool saveService(const Documents&, const std::filesystem::path&, std::string* error,
                 const ThumbnailCache* cache = nullptr);
bool saveServiceAndAdopt(Documents&, const std::filesystem::path&,
                         std::vector<AssetRebind>* rebinds, std::string* error,
                         const ThumbnailCache* cache = nullptr);
bool loadService(Documents*, const std::filesystem::path&, std::string* error,
                 ThumbnailCache* cache = nullptr);
// Cache-only flush: read the manifest, refuse unless its authored digest still
// equals `expectedAuthoredDigest`, replace only the cache block, staged rename.
bool mergeThumbnailCache(const std::filesystem::path& directory, const ThumbnailCache&,
                         std::string_view expectedAuthoredDigest, std::string* error);
std::string authoredManifestDigest(const Documents&);  // sha256 of the cache-free shape
```

All new parameters are trailing and defaulted, so every existing caller keeps
its old meaning. `Documents` and history snapshots never hold a cache record.

Runtime overlay (Qt, `src/ui/ThumbnailStore.h`):

```cpp
class ThumbnailStore : public QObject {
    // Adopted at open/save/load/recovery, and by the catalog bridge.
    void adopt(domain::ThumbnailCache cache, std::string authoredDigest, bool writable);
    // Demand lane: deduped by (slideId, fingerprint); bounded 2 threads; latest
    // wins per slide; a request for a picture already queued is a no-op.
    void request(const QString& showId, const QString& slideId,
                 const QString& fingerprint, qint64 displayWidth, qint64 displayHeight);
    // A finished worker render, still at the extent the worker sent it.
    void record(const QString& showId, const QString& slideId,
                const QString& fingerprint, const QImage& rendered);
    // Coalesced cache-only flush request (2 s idle, >= 5 s apart); the store
    // calls the host's save hook only when its snapshot is still durable.
    std::function<bool(const domain::ThumbnailCache&, QString* error)> flush;
signals:
    void pictureReady(QString slideId, QString fingerprint, QImage image);
    void pictureMissed(QString slideId, QString fingerprint);
    void countersChanged();
};
```

`PreviewConsumer` consults the store through two injected callbacks
(`lookup(slideId, fingerprint, w, h)` and `record(...)`), never through a QObject
it does not own; with no store attached its behaviour is exactly today's.

## Read, write and lifecycle rules

1. Visible row → memory picture → validated persisted record (decode lane) →
   worker batch. A persisted hit removes the row from the batch before it is
   sent; it never "paints early and renders anyway". A miss (corrupt, stale,
   over-budget) re-enters the batch and is reported as a miss only to counters.
2. A record is written only when the render's captured owner, fingerprint and
   extent still match when the pixels are released; shared-memory slots are
   released before encode/disk work.
3. Explicit save and autosave snapshots merge the current cache into the
   manifest. Cache arrivals change neither `dirty`, history, undo nor catalog
   preparation.
4. A clean, writable, already-saved service may be flushed cache-only. The
   flush merges only entries matching the last durable snapshot
   (`authoredManifestDigest`), preserves unsaved edits in memory, and is
   serialized against save/load/recovery epochs through `DocumentPersistence`.
   An external manifest change makes it skip, never overwrite.
5. Unsaved, new or read-only packages keep entries in memory only.
6. Cache write failure leaves the previous manifest valid and never fails an
   otherwise good save (retry the save without the block).
7. Pruned: deleted shows/slides, stale fingerprints, excess count/bytes (oldest
   last-used first). The manifest is never rewritten per frame.
8. `catalog.json` gains bounded per-item variants under the same entry contract
   (TC.9, format 3). A variant names the exact prepared-layout fingerprint;
   cached pixels never establish slide boundaries or bypass preparation.

## Fixtures and evidence plan

- `domain_tests` / new `thumbnail_cache_tests`: SHA-256/Base64 vectors, entry
  bounds, fingerprint stability across relocation, visual/probe changes,
  duplicate occurrences, deterministic codec round-trip, corrupt/oversize
  records falling back without losing `Documents`, cache-only flush digest
  mismatch.
- `preview_*` fixtures: injected store lookup/record, all-hit window sends no
  `preview.thumbnails` RPC, mixed window sends only misses, superseded decode
  never paints another slide.
- Real-shell journey (TC.10): build a song+Bible service, warm it, quit, reopen
  in a fresh process and count worker thumbnail renders (must be zero for hits)
  and first-visible-card time, with cache enabled/disabled.

## Status log

- 2026-11-20 TC.1: contract above frozen; extent comparison measured; service
  format stays 15 with an independently versioned block.
- 2026-11-20 TC.2–TC.9 landed: fingerprint/asset probes, service codec and
  cache-only merge, bounded store lanes, persistence/autosave integration,
  cache-first consumer, QML wiring with separate persisted availability,
  fault injection, and catalog format 3 variants with bridge reuse.
- 2026-11-20 TC.10 measured on the real worker (`thumbnail_reopen_tests`):
  12-slide service 877 ms / 12 thumbnail renders cold; a separate-process
  reopen 6 ms / 0 thumbnail renders / 12 persisted hits; 200-slide service
  parse 51 031 B plain vs 790 220 B with a full block, ~1 ms both. The
  256-record / 32 KiB / 1.5 MiB bounds and the resident LRU are unchanged.
  TC closes with no speedup claim beyond these measurements.
