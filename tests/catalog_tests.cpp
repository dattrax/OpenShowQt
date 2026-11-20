/*
 * The shared catalog's regressions (tasks SL.1 and SL.2): identities,
 * categories, revisions, the closed validation, undo across restores, the
 * durable package and its refusals. Qt-free like the model; the failure
 * contract is the domain's — a refused edit changes nothing, and a failed
 * load leaves the target untouched.
 */

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "openshow/catalog/catalog.h"
#include "openshow/catalog/catalogstorage.h"
#include "openshow/catalog/history.h"
#include "openshow/domain/thumbnails.h"

namespace {

bool gFailed = false;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        gFailed = true;
    }
    return condition;
}

using openshow::catalog::Catalog;
using openshow::catalog::CatalogItem;
using openshow::catalog::Category;
using openshow::catalog::History;
using openshow::catalog::Id;
using openshow::catalog::CatalogThumbnailCache;
using openshow::catalog::SongMetadata;
using openshow::catalog::Stanza;
using openshow::domain::BoxRect;
using openshow::domain::TextContext;
using openshow::domain::ThemeSnapshot;
using openshow::domain::TransitionKind;

Stanza stanza(const std::string& label, const std::vector<std::string>& lines) {
    Stanza result;
    result.label = label;
    result.lines = lines;
    return result;
}

std::vector<Stanza> sampleStanzas() {
    return {
        stanza("Verse 1", {"Morning light is breaking", "Every shadow turning into gold"}),
        stanza("Chorus", {"Sing it out, sing it out", "Morning light is all around"}),
    };
}

SongMetadata sampleMetadata() {
    SongMetadata metadata;
    metadata.ccli = "9000001";
    metadata.authors = "Example Writer, Second Writer";
    metadata.copyright = "\u00a9 2024 Example Music";
    return metadata;
}

std::filesystem::path scratchDirectory() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("openshow-catalog-tests-" + std::to_string(::getpid()));
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
    std::filesystem::create_directories(path);
    return path;
}

std::string readWholeFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

void writeWholeFile(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

// --- SL.1: the model -----------------------------------------------------

void testIdentitiesAndCategories() {
    Catalog catalog;
    std::string error;

    const Id first = catalog.createSong("Amazing Grace", sampleStanzas(),
                                        sampleMetadata(), &error);
    check(!first.empty(), "a valid song is created");
    check(first == "item1", "the first item's id is the store's first ordinal");
    const Id second =
        catalog.createSong("Amazing Grace", sampleStanzas(), {}, &error);
    check(!second.empty() && second != first,
          "a duplicate title is a distinct item, not a merge");
    check(catalog.size() == 2, "both duplicates are kept");
    check(catalog.count(Category::Songs) == 2,
          "both begin in the Songs category");

    const CatalogItem* item = catalog.find(first);
    check(item != nullptr, "the item is found by id");
    if (item != nullptr) {
        check(item->recordRevision == 1 && item->contentRevision == 1,
              "a new record starts at revision 1/1");
        check(item->metadata.ccli == "9000001", "the CCLI number is stored");
        check(item->metadata.authors.find("Example") != std::string::npos,
              "the author text is stored");
        check(item->metadata.copyright.find("\xc2\xa9") != std::string::npos,
              "Unicode copyright text survives the model");
        check(item->stanzas.size() == 2 &&
                  item->stanzas[1].label == "Chorus" &&
                  item->stanzas[1].lines.size() == 2,
              "stanza labels and lines are kept in order");
    }

    check(catalog.idsInCategory(Category::Presentations).empty(),
          "the other category starts empty");

    // A move changes classification only: revisions keep their content
    // half, and repeating it is a no-op.
    check(catalog.moveToCategory(first, Category::Presentations, &error),
          "an item moves to Presentations");
    const CatalogItem* moved = catalog.find(first);
    check(moved != nullptr && moved->category == Category::Presentations,
          "the category moved");
    check(moved != nullptr && moved->contentRevision == 1,
          "a category move does not change the content revision");
    check(moved != nullptr && moved->recordRevision == 2,
          "a category move advances the record revision");
    check(moved != nullptr && moved->stanzas == sampleStanzas(),
          "a category move preserves the words");
    check(catalog.moveToCategory(first, Category::Presentations, &error),
          "repeating a move succeeds");
    check(catalog.find(first)->recordRevision == 2,
          "repeating a move is a no-op, not another revision");
    check(catalog.idsInCategory(Category::Songs) == std::vector<Id>{second},
          "each category lists only its own items");
    check(catalog.count(Category::Presentations) == 1,
          "counts follow the category");
}

void testRevisionsAndRefusals() {
    Catalog catalog;
    std::string error;
    const Id id = catalog.createSong("Grace Abounding", sampleStanzas(), {},
                                     &error);
    check(!id.empty(), "the song is created");

    // Rename: record only.
    check(catalog.rename(id, "  Grace Abounding (Live)  ", &error),
          "a rename is accepted and trimmed");
    const CatalogItem* item = catalog.find(id);
    check(item != nullptr && item->title == "Grace Abounding (Live)",
          "the trimmed title is stored");
    check(item != nullptr && item->contentRevision == 1,
          "a rename leaves the content revision alone");
    check(item != nullptr && item->recordRevision == 2,
          "a rename advances the record revision");
    check(catalog.rename(id, "Grace Abounding (Live)", &error),
          "an unchanged rename succeeds");
    check(catalog.find(id)->recordRevision == 2,
          "an unchanged rename costs nothing");

    // Content + metadata: both revisions.
    std::vector<Stanza> changed = sampleStanzas();
    changed[0].lines.push_back("A new line");
    SongMetadata metadata = sampleMetadata();
    metadata.ccli = "1234567";
    check(catalog.updateSong(id, changed, metadata, &error),
          "content and metadata are replaced together");
    item = catalog.find(id);
    check(item != nullptr && item->recordRevision == 3 &&
              item->contentRevision == 2,
          "a content edit advances both revisions");
    check(item != nullptr && item->metadata.ccli == "1234567",
          "the corrected CCLI number is stored");

    // Metadata-only still changes the content revision: a report built
    // from a stale number is wrong.
    metadata.ccli = "7654321";
    check(catalog.setMetadata(id, metadata, &error),
          "metadata alone is replaceable");
    check(catalog.find(id)->contentRevision == 3,
          "a metadata correction advances the content revision");
    check(catalog.setMetadata(id, metadata, &error),
          "an unchanged metadata write succeeds");
    check(catalog.find(id)->contentRevision == 3,
          "an unchanged metadata write is a no-op");

    // Refusals leave the record exactly as it was.
    const CatalogItem before = *catalog.find(id);
    check(!catalog.rename(id, "   ", &error), "a blank title is refused");
    check(*catalog.find(id) == before, "a refused rename changes nothing");
    SongMetadata bad = metadata;
    bad.ccli = "CCLI 7654321";
    check(!catalog.setMetadata(id, bad, &error),
          "a non-numeric CCLI field is refused");
    check(*catalog.find(id) == before, "a refused metadata write changes nothing");
    std::vector<Stanza> noWords = {stanza("Chorus", {"  "})};
    check(!catalog.updateSong(id, noWords, metadata, &error),
          "a stanza set with no words is refused");
    check(*catalog.find(id) == before, "a refused content write changes nothing");
    check(catalog.createSong("", sampleStanzas(), {}, &error).empty(),
          "a nameless song is refused");
    check(!catalog.moveToCategory("item999", Category::Songs, &error),
          "moving an unknown item is refused");
    check(!catalog.remove("item999", &error), "removing an unknown item is refused");
    check(!catalog.rename("item999", "x", &error), "renaming an unknown item is refused");
    check(catalog.size() == 1, "every refusal left the catalog size alone");
}

void testValidationLimits() {
    Catalog catalog;
    std::string error;
    std::vector<Stanza> tooMany(openshow::catalog::kMaxStanzas + 1,
                                stanza("Verse", {"one line"}));
    check(catalog.createSong("Too long", tooMany, {}, &error).empty(),
          "too many stanzas is refused");
    check(error.find("at most") != std::string::npos,
          "the refusal names the stanza limit");

    std::vector<Stanza> longLine = {
        stanza("Verse", {std::string(openshow::catalog::kMaxLineBytes + 1, 'a')})};
    check(catalog.createSong("Long line", longLine, {}, &error).empty(),
          "an over-long line is refused");
    check(error.find("longer than") != std::string::npos,
          "the refusal names the line cap");

    std::string hugeTitle(openshow::catalog::kMaxTitleBytes + 1, 't');
    check(catalog.createSong(hugeTitle, sampleStanzas(), {}, &error).empty(),
          "an over-long title is refused");

    SongMetadata hugeMetadata;
    hugeMetadata.authors = std::string(openshow::catalog::kMaxMetadataBytes + 1, 'a');
    check(!openshow::catalog::validSong("Title", sampleStanzas(), hugeMetadata, &error),
          "an over-long author field is refused");
    check(catalog.size() == 0, "no refused song was created");
}

void testHistoryAndIdentityWatermark() {
    Catalog catalog;
    History history(&catalog);
    std::string error;

    check(history.apply([&] {
              return !catalog.createSong("First", sampleStanzas(), {}, &error).empty();
          }),
          "a creation through history succeeds");
    check(history.canUndo(), "the creation is undoable");
    check(history.undo(&error), "undo succeeds");
    check(catalog.size() == 0, "undo removed the creation");
    check(history.canRedo(), "the creation is redoable");
    check(history.redo(&error), "redo succeeds");
    check(catalog.size() == 1 && catalog.itemIds()[0] == "item1",
          "redo restored the item and its id");

    check(history.undo(&error), "undo again succeeds");
    check(history.apply([&] {
              return !catalog.createSong("Second", sampleStanzas(), {}, &error).empty();
          }),
          "a new creation after undo succeeds");
    check(catalog.itemIds()[0] != "item1",
          "the undone id is never reissued (the monotonic watermark)");
    check(!history.apply([&] {
              return !catalog.createSong("", sampleStanzas(), {}, &error).empty();
          }),
          "a refused edit is invisible to history");
    check(history.undoDepth() == 1,
          "the refusal pushed no undo entry");
    check(!history.redo(&error), "a new edit discarded the abandoned branch");
    check(catalog.size() == 1, "the refused creation left nothing behind");
}

// --- SL.2: the durable package -------------------------------------------

void testRoundTripAndLoadStrictness() {
    const std::filesystem::path dir = scratchDirectory();
    Catalog catalog;
    std::string error;
    const Id a = catalog.createSong("Amazing Grace", sampleStanzas(),
                                    sampleMetadata(), &error);
    const Id b = catalog.createSong("Grace Abounding", sampleStanzas(), {},
                                    &error);
    catalog.rename(b, "Grace Abounding (Live)", &error);
    catalog.moveToCategory(b, Category::Presentations, &error);

    check(openshow::catalog::saveCatalog(catalog, dir, &error),
          "the catalog saves");
    check(std::filesystem::is_regular_file(dir / "catalog.json"),
          "catalog.json is written where the package promises");

    Catalog loaded;
    check(openshow::catalog::loadCatalog(&loaded, dir, &error),
          "the catalog loads");
    check(loaded.size() == 2, "both items round-trip");
    check(loaded.count(Category::Songs) == 1 &&
              loaded.count(Category::Presentations) == 1,
          "categories round-trip");
    const CatalogItem* first = loaded.find(a);
    check(first != nullptr && first->title == "Amazing Grace" &&
              first->metadata == sampleMetadata() &&
              first->stanzas == sampleStanzas() &&
              first->recordRevision == 1 && first->contentRevision == 1,
          "a record round-trips field for field");
    const CatalogItem* second = loaded.find(b);
    check(second != nullptr && second->title == "Grace Abounding (Live)" &&
              second->recordRevision == 3 && second->contentRevision == 1,
          "revisions round-trip separately");

    // A fresh id after loading never collides with a loaded id.
    const Id c = loaded.createSong("Third", sampleStanzas(), {}, &error);
    check(!c.empty() && c != a && c != b, "a new id after load is fresh");

    // A malformed, future or unknown-key file is refused and leaves the
    // target untouched.
    const std::string good = readWholeFile(dir / "catalog.json");
    Catalog untouched;
    untouched.createSong("Keep", sampleStanzas(), {}, &error);
    const CatalogItem keep = *untouched.find("item1");

    writeWholeFile(dir / "catalog.json", "{not json");
    check(!openshow::catalog::loadCatalog(&untouched, dir, &error),
          "malformed JSON is refused");
    check(*untouched.find("item1") == keep,
          "a refused load leaves the target untouched");

    writeWholeFile(dir / "catalog.json",
                   good.substr(0, good.find("\"format\"")) +
                       "\"format\": 99, \"nextOrdinal\": 1, \"items\": []}");
    check(!openshow::catalog::loadCatalog(&untouched, dir, &error),
          "a future format is refused");
    check(error.find("unsupported format 99") != std::string::npos,
          "the format refusal names the version");

    writeWholeFile(dir / "catalog.json", "{\"format\":1,\"nextOrdinal\":1,"
                                         "\"items\":[],\"surprise\":true}");
    check(!openshow::catalog::loadCatalog(&untouched, dir, &error),
          "an unknown key is refused");
    check(error.find("surprise") != std::string::npos,
          "the unknown-key refusal names the key");

    // A duplicate id, an unsafe id and a bad category are each refused.
    std::string duplicate = good;
    const std::size_t firstItem = duplicate.find("\"id\":\"item1\"");
    duplicate.replace(firstItem, std::string("\"id\":\"item1\"").size(),
                      "\"id\":\"item2\"");
    writeWholeFile(dir / "catalog.json", duplicate);
    check(!openshow::catalog::loadCatalog(&untouched, dir, &error),
          "a duplicate id is refused");

    std::string unsafe = good;
    unsafe.replace(unsafe.find("\"id\":\"item1\""),
                   std::string("\"id\":\"item1\"").size(),
                   "\"id\":\"../evil\"");
    writeWholeFile(dir / "catalog.json", unsafe);
    check(!openshow::catalog::loadCatalog(&untouched, dir, &error),
          "an unsafe id is refused");

    std::string badCategory = good;
    const std::size_t categoryAt = badCategory.find("\"category\":\"songs\"");
    badCategory.replace(categoryAt, std::string("\"category\":\"songs\"").size(),
                        "\"category\":\"folders\"");
    writeWholeFile(dir / "catalog.json", badCategory);
    check(!openshow::catalog::loadCatalog(&untouched, dir, &error),
          "an unknown category is refused");

    writeWholeFile(dir / "catalog.json", good);
    check(*untouched.find("item1") == keep,
          "every refused load left the target untouched");

    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
}

void testFaultedSavesKeepThePreviousCatalog() {
    const std::filesystem::path dir = scratchDirectory();
    Catalog catalog;
    std::string error;
    catalog.createSong("Original", sampleStanzas(), {}, &error);
    check(openshow::catalog::saveCatalog(catalog, dir, &error),
          "the original catalog saves");
    const std::string committed = readWholeFile(dir / "catalog.json");

    catalog.rename("item1", "Changed", &error);
    for (const char* step : {"write", "fsync", "close", "rename"}) {
        ::setenv("OPENSHOW_CATALOG_FAULT", step, 1);
        const bool saved = openshow::catalog::saveCatalog(catalog, dir, &error);
        ::unsetenv("OPENSHOW_CATALOG_FAULT");
        check(!saved, step);
        check(error.find("simulated fault") != std::string::npos,
              "a faulted save names its step");
        check(readWholeFile(dir / "catalog.json") == committed,
              "a faulted save leaves the committed catalog readable and unchanged");
        check(!std::filesystem::exists(dir / "catalog.json.tmp"),
              "a faulted save leaves no staging file behind");
    }

    // A missing package is a clean "cannot open", not a silent empty load.
    Catalog missing;
    check(!openshow::catalog::loadCatalog(&missing, dir / "absent", &error),
          "loading a missing catalog is refused");
    check(missing.size() == 0, "a refused load created nothing");

    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
}

// --- PT.2: the remembered Library look ------------------------------------

void testRememberedLook() {
    const std::filesystem::path dir = scratchDirectory();
    Catalog catalog;
    std::string error;
    const Id id = catalog.createSong("Remembered", sampleStanzas(), sampleMetadata(),
                                     &error);
    check(!id.empty(), "a song exists to remember a look for");
    const CatalogItem before = *catalog.find(id);

    ThemeSnapshot look;
    look.name = "Brun";
    look.textColor = "#f0e0d0";
    look.fontFamily = "Noto Serif";
    look.fontSize = 48.0;
    look.align = "left";
    look.lineSpacing = 1.4;
    look.fontWeight = 700;
    look.fontStyle = "italic";
    look.fontUnderline = true;
    look.fitMode = "fixed";
    look.minFontSize = 18.0;
    look.verticalAlign = "bottom";
    look.allCaps = true;
    look.stroke = openshow::domain::TextStroke{true, "#ff0000", 3.5};
    look.shadow = openshow::domain::TextShadow{true, "#00ff00", 0.4, -4.0, 6.0, 2.5};
    look.placement = BoxRect{120.0, 80.0, 1680.0, 900.0};
    look.transition.kind = TransitionKind::kCut;
    look.transition.textDurationMs = 0;
    look.transition.mediaDurationMs = 10000;
    // A captured snapshot carries RESOLVED contexts, so every field is an
    // explicit answer; the codec canonicalises exactly that shape.
    TextContext scripture;
    scripture.name = "scripture";
    scripture.fontFamily = look.fontFamily;
    scripture.fontSize = 32.0;
    scripture.lineSpacing = look.lineSpacing;
    scripture.align = look.align;
    scripture.verticalAlign = "top";
    scripture.allCaps = false;
    scripture.transition = look.transition;
    TextContext future;
    future.name = "future view";
    future.fontFamily = look.fontFamily;
    future.fontSize = look.fontSize;
    future.lineSpacing = look.lineSpacing;
    future.align = "right";
    future.verticalAlign = look.verticalAlign;
    future.allCaps = look.allCaps;
    future.transition = look.transition;
    look.contexts = {scripture, future};

    check(catalog.setThemeSnapshot(id, look, &error),
          "the Library remembers the chosen look");
    const CatalogItem after = *catalog.find(id);
    check(after.recordRevision == before.recordRevision + 1 &&
              after.contentRevision == before.contentRevision + 1,
          "remembering a look advances both revisions");
    check(after.title == before.title && after.stanzas == before.stanzas &&
              after.metadata == before.metadata,
          "the canonical words and attribution are untouched");
    check(after.themeSnapshot.has_value() && *after.themeSnapshot == look,
          "the remembered value is exactly the one set");

    check(catalog.setThemeSnapshot(id, look, &error) &&
              catalog.find(id)->recordRevision == after.recordRevision,
          "setting the same look again costs no revision");

    ThemeSnapshot invalid = look;
    invalid.align = "middle";
    check(!catalog.setThemeSnapshot(id, invalid, &error),
          "an invalid remembered look is refused");
    check(catalog.find(id)->themeSnapshot == look,
          "the refusal leaves the previous preference in place");
    check(!catalog.setThemeSnapshot("item-does-not-exist", look, &error),
          "remembering a look on an unknown item is refused");

    // The durable package round-trips the value; the words stay canonical.
    check(openshow::catalog::saveCatalog(catalog, dir, &error),
          "a catalog with a remembered look saves");
    Catalog loaded;
    check(openshow::catalog::loadCatalog(&loaded, dir, &error),
          "the catalog reloads");
    const CatalogItem* reloaded = loaded.find(id);
    check(reloaded != nullptr && reloaded->themeSnapshot.has_value() &&
              *reloaded->themeSnapshot == look,
          "the remembered look round-trips through disk");
    check(reloaded != nullptr && reloaded->stanzas == before.stanzas &&
              reloaded->metadata == before.metadata,
          "the reloaded words and metadata are unchanged");

    // Format 1 loads with no preference; naming the newer key there is
    // refused by name.
    const std::string legacy =
        R"({"format":1,"nextOrdinal":2,"items":[{"id":"item1","category":"songs","title":"Legacy","recordRevision":1,"contentRevision":1,"kind":"lyrics","metadata":{"ccli":"","authors":"","copyright":""},"stanzas":[{"label":"Verse 1","lines":["Old words"]}]}]})";
    writeWholeFile(dir / "catalog.json", legacy);
    Catalog legacyLoaded;
    check(openshow::catalog::loadCatalog(&legacyLoaded, dir, &error),
          "a format-1 catalog still loads");
    check(legacyLoaded.find("item1") != nullptr &&
              !legacyLoaded.find("item1")->themeSnapshot.has_value(),
          "a format-1 record means no remembered look");
    std::string legacyWithKey = legacy;
    legacyWithKey.replace(legacyWithKey.find("\"stanzas\""), 9,
                          "\"themeSnapshot\":{},\"stanzas\"");
    writeWholeFile(dir / "catalog.json", legacyWithKey);
    Catalog refused;
    check(!openshow::catalog::loadCatalog(&refused, dir, &error) &&
              error.find("themeSnapshot") != std::string::npos,
          "a format-1 record naming the newer key is refused by name");

    // A malformed snapshot in a format-2 file refuses, naming the rule.
    const std::string good = readWholeFile(dir / "catalog.json");
    check(openshow::catalog::saveCatalog(catalog, dir, &error),
          "the current catalog is written for the malformed case");
    std::string malformed = readWholeFile(dir / "catalog.json");
    const std::size_t colour = malformed.find("\"textColor\":\"#f0e0d0\"");
    check(colour != std::string::npos, "the snapshot colour is in the file");
    if (colour != std::string::npos) {
        malformed.replace(colour, std::string("\"textColor\":\"#f0e0d0\"").size(),
                          "\"textColor\":\"red\"");
        writeWholeFile(dir / "catalog.json", malformed);
        check(!openshow::catalog::loadCatalog(&refused, dir, &error) &&
                  error.find("#rgb") != std::string::npos,
              "a malformed remembered look refuses naming its rule");
    }
    writeWholeFile(dir / "catalog.json", good);

    // Library history owns the preference: one undo forgets it, redo
    // restores it, and the song's words never move.
    Catalog remembered;
    const Id rememberedId =
        remembered.createSong("History", sampleStanzas(), {}, &error);
    History history(&remembered);
    check(history.apply([&] {
              return remembered.setThemeSnapshot(rememberedId, look, &error);
          }),
          "remembering through Library history succeeds");
    check(history.undo(&error) &&
              !remembered.find(rememberedId)->themeSnapshot.has_value(),
          "one Library undo forgets the remembered look");
    check(history.redo(&error) &&
              remembered.find(rememberedId)->themeSnapshot == look,
          "Library redo restores it");
    check(remembered.find(rememberedId)->stanzas == sampleStanzas(),
          "the remembered-look history never touches the words");

    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
}

}  // namespace


// --- TC.9: bounded Library render variants --------------------------------

// A Library song can be played under several looks and every preparation mints
// new slide ids, so a variant is identified by its render fingerprint alone and
// lives BESIDE the catalog model. Nothing here may move a revision, and a
// broken block may only cost pictures.
void testRenderVariants() {
    const std::filesystem::path dir = scratchDirectory();
    Catalog catalog;
    std::string error;
    const Id a = catalog.createSong("Variant Song", sampleStanzas(), sampleMetadata(), &error);
    const Id b = catalog.createSong("Second Song", sampleStanzas(), {}, &error);
    const CatalogItem beforeA = *catalog.find(a);
    const CatalogItem beforeB = *catalog.find(b);

    auto variant = [](const std::string& fingerprint, unsigned char fill) {
        openshow::domain::ThumbnailCacheEntry entry;
        entry.fingerprint = openshow::domain::sha256Hex(fingerprint);
        entry.renderer = openshow::domain::rendererIdentity();
        entry.width = openshow::domain::kThumbnailWidth;
        entry.height = openshow::domain::kThumbnailHeight;
        std::string png("\x89PNG\r\n\x1a\n", 8);
        png.append(300, static_cast<char>(fill));
        entry.pngBytes = png.size();
        entry.checksum = openshow::domain::pngChecksum(png);
        entry.pngBase64 = openshow::domain::base64Encode(png);
        return entry;
    };

    CatalogThumbnailCache cache;
    cache.variants[a].push_back(variant("look-one", 'a'));
    cache.variants[a].push_back(variant("look-one", 'b'));   // duplicate look
    cache.variants[a].push_back(variant("look-two", 'c'));
    openshow::domain::ThumbnailCacheEntry broken = variant("look-broken", 'd');
    broken.pngBase64 = "not base64!!";
    cache.variants[a].push_back(broken);
    cache.variants[b].push_back(variant("look-b", 'e'));

    check(openshow::catalog::saveCatalog(catalog, dir, &error, &cache),
          "TC.9: the catalog saves with variants");
    const std::string saved = readWholeFile(dir / "catalog.json");
    check(saved.find("\"format\":3") != std::string::npos,
          "TC.9: the catalog format names the block's era");
    check(saved.find("\"thumbnailVariants\":{\"format\":1") != std::string::npos,
          "TC.9: the variant block carries its own version");
    check(saved.find("not base64!!") == std::string::npos,
          "TC.9: a malformed variant is not written");

    Catalog loaded;
    CatalogThumbnailCache loadedCache;
    check(openshow::catalog::loadCatalog(&loaded, dir, &error, &loadedCache),
          "TC.9: the catalog loads with variants");
    check(loaded.size() == 2 && *loaded.find(a) == beforeA && *loaded.find(b) == beforeB,
          "TC.9: variants change no authored field and no revision");
    check(loadedCache.variants[a].size() == 2 &&
              loadedCache.variants[a][0].fingerprint ==
                  openshow::domain::sha256Hex("look-one") &&
              loadedCache.variants[a][1].fingerprint ==
                  openshow::domain::sha256Hex("look-two"),
          "TC.9: the valid, distinct looks round-trip in file order");
    check(loadedCache.variants[b].size() == 1, "TC.9: every item keeps its own looks");
    check(openshow::catalog::catalogAuthoredDigest(catalog) ==
              openshow::catalog::catalogAuthoredDigest(loaded),
          "TC.9: the authored digest does not see the variant block");
    check(openshow::catalog::saveCatalog(loaded, dir, &error, &loadedCache) &&
              readWholeFile(dir / "catalog.json") == saved,
          "TC.9: save/load round-trips the block byte for byte");

    // --- the cache-only replacement --------------------------------------
    const std::string digest = openshow::catalog::catalogAuthoredDigest(catalog);
    check(openshow::catalog::saveCatalog(catalog, dir, &error) &&
              readWholeFile(dir / "catalog.json").find("thumbnailVariants") ==
                  std::string::npos,
          "TC.9: a save without the overlay writes the pre-TC shape");
    check(openshow::catalog::mergeCatalogThumbnails(dir, cache, digest, &error),
          "TC.9: a cache-only catalog write installs its block");
    const std::string merged = readWholeFile(dir / "catalog.json");
    std::string mergedDigest;
    check(openshow::catalog::loadCatalog(&loaded, dir, &error, &loadedCache) &&
              loadedCache.variants.size() == 2 && *loaded.find(a) == beforeA,
          "TC.9: the merged block is what a load sees, with authored fields intact");
    check(openshow::catalog::catalogAuthoredDigest(loaded) == digest,
          "TC.9: the merge did not move the authored identity");
    check(!openshow::catalog::mergeCatalogThumbnails(dir, cache, std::string(64, '0'), &error),
          "TC.9: a stale digest refuses the merge");
    check(error.find("changed after it was loaded") != std::string::npos,
          "TC.9: the refusal names the external change");
    check(readWholeFile(dir / "catalog.json") == merged,
          "TC.9: the refused merge left the file alone");
    ::setenv("OPENSHOW_CATALOG_FAULT", "write", 1);
    CatalogThumbnailCache changed = cache;
    changed.variants[b].push_back(variant("look-b-two", 'f'));
    const bool flushed = openshow::catalog::mergeCatalogThumbnails(dir, changed, digest, &error);
    ::unsetenv("OPENSHOW_CATALOG_FAULT");
    check(!flushed && readWholeFile(dir / "catalog.json") == merged,
          "TC.9: an injected write failure leaves the previous catalog");

    // --- a broken block is only a miss -----------------------------------
    {
        std::string text = merged;
        const std::size_t at = text.find("\"format\":1");
        if (at != std::string::npos) {
            text.replace(at, std::string("\"format\":1").size(), "\"format\":99");
        }
        writeWholeFile(dir / "catalog.json", text);
        Catalog kept;
        CatalogThumbnailCache none;
        check(openshow::catalog::loadCatalog(&kept, dir, &error, &none),
              "TC.9: another block version still loads the catalog");
        check(kept.size() == 2 && none.variants.empty(),
              "TC.9: another block version is a miss");
    }
    {
        // Variants for an item that no longer exists are dropped with it, and
        // a look beyond the per-item cap is refused rather than stored.
        std::string text = merged;
        const std::size_t itemsAt = text.find("\"thumbnailVariants\"");
        check(itemsAt != std::string::npos, "TC.9: the block is present to edit");
        CatalogThumbnailCache pruned;
        Catalog loadedPruned;
        std::string mangled = text;
        const std::size_t idAt = mangled.find("\"id\":\"" + a + "\"",
                                              mangled.find("\"thumbnailVariants\""));
        check(idAt != std::string::npos, "TC.9: the variant owner is present");
        mangled.replace(idAt, std::string("\"id\":\"" + a + "\"").size(),
                        "\"id\":\"item404\"");
        writeWholeFile(dir / "catalog.json", mangled);
        check(openshow::catalog::loadCatalog(&loadedPruned, dir, &error, &pruned),
              "TC.9: an orphaned variant block still loads");
        check(pruned.variants.count(a) == 0 && pruned.variants.count(b) == 1,
              "TC.9: variants for a deleted item are dropped, the other item keeps its own");
    }
    {
        // The per-item ceilings are enforced by the sanitiser the codec uses.
        std::vector<openshow::domain::ThumbnailCacheEntry> many;
        for (int i = 0; i < 10; ++i) {
            many.push_back(variant("capped-" + std::to_string(i), 'g'));
        }
        many.push_back(variant("capped-0", 'h'));  // duplicate fingerprint
        check(openshow::domain::sanitizeThumbnailVariants(&many) == 5,
              "TC.9: the per-item count and duplicate rule refuse the excess");
        check(many.size() == openshow::domain::kMaxThumbnailVariantsPerItem,
              "TC.9: exactly the bound survives");
        std::vector<openshow::domain::ThumbnailCacheEntry> heavy;
        openshow::domain::ThumbnailCacheEntry big = variant("heavy", 'i');
        for (int i = 0; i < 8; ++i) {
            openshow::domain::ThumbnailCacheEntry entry = big;
            entry.fingerprint = openshow::domain::sha256Hex("heavy-" + std::to_string(i));
            heavy.push_back(entry);
        }
        openshow::domain::sanitizeThumbnailVariants(&heavy);
        std::uint64_t encoded = 0;
        for (const auto& entry : heavy) {
            encoded += entry.pngBase64.size();
        }
        check(encoded <= openshow::domain::kMaxThumbnailVariantBytesPerItem,
              "TC.9: the per-item byte ceiling survives");
        check(openshow::domain::findThumbnailVariant(heavy, heavy.front().fingerprint) ==
                  &heavy.front(),
              "TC.9: a variant is found by its fingerprint alone");
        check(openshow::domain::findThumbnailVariant(heavy, "nope") == nullptr,
              "TC.9: another look has no picture");
    }
    {
        // A format-2 file may not smuggle the format-3 key in.
        std::string text = merged;
        const std::size_t formatAt = text.find("\"format\":3");
        text.replace(formatAt, std::string("\"format\":3").size(), "\"format\":2");
        writeWholeFile(dir / "catalog.json", text);
        Catalog refused;
        check(!openshow::catalog::loadCatalog(&refused, dir, &error),
              "TC.9: a format-2 file carrying the block is refused");
    }

    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
}

int main() {
    testIdentitiesAndCategories();
    testRevisionsAndRefusals();
    testValidationLimits();
    testHistoryAndIdentityWatermark();
    testRoundTripAndLoadStrictness();
    testRenderVariants();
    testRememberedLook();
    testFaultedSavesKeepThePreviousCatalog();
    if (gFailed) {
        std::fprintf(stderr, "catalog_tests: FAILED\n");
        return 1;
    }
    std::printf("catalog_tests: all checks passed\n");
    return 0;
}
