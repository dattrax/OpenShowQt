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
using openshow::catalog::SongMetadata;
using openshow::catalog::Stanza;

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

}  // namespace

int main() {
    testIdentitiesAndCategories();
    testRevisionsAndRefusals();
    testValidationLimits();
    testHistoryAndIdentityWatermark();
    testRoundTripAndLoadStrictness();
    testFaultedSavesKeepThePreviousCatalog();
    if (gFailed) {
        std::fprintf(stderr, "catalog_tests: FAILED\n");
        return 1;
    }
    std::printf("catalog_tests: all checks passed\n");
    return 0;
}
