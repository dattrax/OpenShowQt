/*
 * Checks the shared version string, then the document model (task 2.1):
 * shows, slides, groups, styles and playlists — every operation with its
 * success path, its failure path, and the invariant the failure must not
 * have changed anything. Fails with a non-zero exit status so ctest
 * reports it.
 */

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

#include "openshow/domain/documents.h"
#include "openshow/domain/history.h"
#include "openshow/domain/storage.h"
#include "openshow/domain/version.h"

namespace {

bool gFailed = false;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        gFailed = true;
    }
    return condition;
}

using openshow::domain::Documents;
using openshow::domain::Id;
using Asset = openshow::domain::Asset;
using Autosave = openshow::domain::Autosave;
using History = openshow::domain::History;
using Playlist = openshow::domain::Playlist;
using Show = openshow::domain::Show;
using Slide = openshow::domain::Slide;
using Style = openshow::domain::Style;

void exerciseShows() {
    Documents docs;
    std::string error;

    const Id show = docs.createShow("Opening Praise", &error);
    check(!show.empty() && error.empty(), "createShow returns an id");
    check(docs.findShow(show) != nullptr && docs.findShow(show)->name == "Opening Praise",
          "the new show is findable under its name");
    check(docs.createShow("   ", &error) == Id() && !error.empty(),
          "a blank name is refused with an error");
    check(docs.createShow("", &error) == Id(), "an empty name is refused too");
    const Id second = docs.createShow("Opening Praise", &error);
    check(second != show, "duplicate names are allowed and distinguished by id");

    check(docs.renameShow(show, "  Renamed ", &error) && docs.findShow(show)->name == "Renamed",
          "rename stores the trimmed name");
    check(!docs.renameShow(show, " ", &error) && docs.findShow(show)->name == "Renamed",
          "a blank rename fails and keeps the old name");
    check(!docs.renameShow("show999", "x", &error), "renaming an unknown show fails");
    check(docs.setShowNotes(show, "band: 2 mics", &error) &&
              docs.findShow(show)->notes == "band: 2 mics",
          "show notes round-trip");
    check(docs.showIds() == std::vector<Id>({show, second}), "showIds lists creation order");

    check(docs.removeShow(second, &error) && docs.findShow(second) == nullptr,
          "removeShow drops the show");
    check(!docs.removeShow(second, &error), "removing twice fails");
}

void exerciseSlides() {
    Documents docs;
    std::string error;
    const Id show = docs.createShow("Deck", &error);

    const Id a = docs.appendSlide(show, &error);
    const Id b = docs.appendSlide(show, &error);
    const Id c = docs.appendSlide(show, &error);
    check(!a.empty() && !b.empty() && !c.empty() && a != b && b != c,
          "appends hand out distinct ids");
    check(docs.findShow(show)->slides.size() == 3, "appends land at the end, in order");
    const Id inserted = docs.insertSlide(show, 1, &error);
    check(docs.findShow(show)->slides[1].id == inserted, "insert lands at the named index");
    check(docs.insertSlide(show, 5, &error).empty() && !error.empty(),
          "insert past the end is refused");

    check(docs.setSlideLines(show, b, {"Amazing grace", "", "how sweet"}, &error) &&
              docs.findShow(show)->slides[2].lines.size() == 3 &&
              docs.findShow(show)->slides[2].lines[1].empty(),
          "lines round-trip including the meaningful blank line");
    check(docs.setSlideNotes(show, b, "quiet", &error) &&
              docs.findShow(show)->slides[2].notes == "quiet",
          "slide notes round-trip");
    check(!docs.setSlideLines(show, "slide999", {"x"}, &error),
          "lines on an unknown slide fail");

    // Presentation order: move b (index 2) in front of a (index 0).
    check(docs.moveSlide(show, b, 0, &error), "moveSlide to the front succeeds");
    const std::vector<Id> order{docs.findShow(show)->slides[0].id,
                                docs.findShow(show)->slides[1].id,
                                docs.findShow(show)->slides[2].id,
                                docs.findShow(show)->slides[3].id};
    check(order[0] == b && order[1] == a && order[2] == inserted && order[3] == c,
          "the vector reads back in the moved order");
    check(docs.moveSlide(show, b, 0, &error), "moving onto one's own position succeeds");
    check(!docs.moveSlide(show, b, 4, &error), "a destination at size() is out of range");
    check(!docs.moveSlide(show, "slide999", 1, &error), "moving an unknown slide fails");

    check(docs.removeSlide(show, inserted, &error) &&
              docs.findShow(show)->slides.size() == 3,
          "removeSlide drops exactly that slide");
    check(!docs.removeSlide(show, inserted, &error), "removing a slide twice fails");
}

void exerciseGroups() {
    Documents docs;
    std::string error;
    const Id show = docs.createShow("Deck", &error);
    const Id one = docs.appendSlide(show, &error);
    const Id two = docs.appendSlide(show, &error);
    const Id three = docs.appendSlide(show, &error);

    check(docs.setSlideGroup(show, two, "Chorus", &error) &&
              docs.findShow(show)->slides[1].group == "Chorus",
          "setting a group labels the slide");
    check(docs.setSlideGroup(show, three, "  ", &error) &&
              docs.findShow(show)->slides[2].group.empty(),
          "a blank group ungroups");
    // Deck: one=Chorus, two=Verse, three=Chorus (a wrapping chorus).
    docs.setSlideGroup(show, one, "Chorus", &error);
    docs.setSlideGroup(show, two, "Verse", &error);
    docs.setSlideGroup(show, three, "Chorus", &error);
    check(docs.groups(show) == std::vector<std::string>({"Chorus", "Verse"}),
          "groups list in order of first appearance");
    check(docs.slidesInGroup(show, "Chorus") == std::vector<Id>({one, three}),
          "slidesInGroup lists members in presentation order");
    check(docs.slidesInGroup(show, "Bridge").empty(), "an unknown group lists nothing");

    check(docs.renameGroup(show, "Chorus", "Refren", &error) &&
              docs.findShow(show)->slides[0].group == "Refren" &&
              docs.findShow(show)->slides[2].group == "Refren" &&
              docs.findShow(show)->slides[1].group == "Verse",
          "rename retags exactly the members");
    check(!docs.renameGroup(show, "Chorus", "x", &error),
          "renaming a group nobody carries fails");
    check(!docs.renameGroup(show, "Verse", "", &error),
          "renaming to blank is refused - dissolving is its own operation");
    check(docs.dissolveGroup(show, "Refren", &error) && docs.groups(show).size() == 1 &&
              docs.findShow(show)->slides[0].group.empty(),
          "dissolve ungroups every member");
    check(!docs.dissolveGroup(show, "Refren", &error), "dissolving twice fails");
    check(!docs.setSlideGroup(show, "slide999", "x", &error),
          "labelling an unknown slide fails");
}

void exerciseStyles() {
    Documents docs;
    std::string error;
    const Id show = docs.createShow("Deck", &error);
    const Id slide = docs.appendSlide(show, &error);

    const Id style = docs.createStyle("House", "#ffffff", "#202040", "DejaVu Serif", 64.0,
                                      &error);
    check(!style.empty(), "createStyle returns an id");
    check(docs.createStyle("Bad", "#12g456", "#000000", "x", 10, &error).empty() &&
              !error.empty(),
          "a non-hex text colour is refused");
    check(docs.createStyle("Bad", "#fff", "white", "x", 10, &error).empty(),
          "a named background colour is refused too");
    check(docs.createStyle("Bad", "#fff", "#fff", "x", -1, &error).empty(),
          "a negative font size is refused");

    check(docs.setShowDefaultStyle(show, style, &error) &&
              docs.styleForSlide(show, slide) != nullptr &&
              docs.styleForSlide(show, slide)->id == style,
          "the show default style resolves for unstyled slides");
    const Id other = docs.createStyle("Loud", "#ffff00", "#000000", "Impact", 96.0, &error);
    check(docs.setSlideStyle(show, slide, other, &error) &&
              docs.styleForSlide(show, slide)->id == other,
          "a per-slide override beats the default");
    check(docs.setSlideStyle(show, slide, "", &error) &&
              docs.styleForSlide(show, slide)->id == style,
          "clearing the override falls back to the default");
    check(!docs.setSlideStyle(show, slide, "style999", &error),
          "an unknown override id is refused");

    check(docs.updateStyle(other, "#ff0000", "#000", "Arial", 0.0, &error) &&
              docs.findStyle(other)->textColor == "#ff0000" &&
              docs.findStyle(other)->backgroundColor == "#000" &&
              docs.findStyle(other)->fontSize == 0.0,
          "updateStyle replaces the appearance (0 size = renderer default)");
    check(docs.renameStyle(other, "Louder", &error) && docs.findStyle(other)->name == "Louder",
          "styles rename");

    check(docs.setSlideStyle(show, slide, other, &error) &&
              docs.setShowDefaultStyle(show, other, &error),
          "point both the override and the default at one style");
    check(docs.removeStyle(other, &error) && docs.findStyle(other) == nullptr &&
              docs.findShow(show)->defaultStyleId.empty() &&
              docs.findShow(show)->slides[0].styleId.empty(),
          "removing a style clears every reference to it");
    check(docs.styleForSlide(show, slide) == nullptr,
          "a show whose default was cleared resolves to no style");
    check(docs.styleIds() == std::vector<Id>({style}), "styleIds reflects removal");
}

void exercisePlaylists() {
    Documents docs;
    std::string error;
    const Id playlist = docs.createPlaylist("Sunday", &error);
    const Id first = docs.createShow("Call to Worship", &error);
    const Id second = docs.createShow("Sending Out", &error);

    check(docs.findPlaylist(playlist) != nullptr && docs.createPlaylist(" ", &error).empty(),
          "playlists are named or refused");

    const Id entryA = docs.addPlaylistEntry(playlist, first, "Gathering", &error);
    const Id entryB = docs.addPlaylistEntry(playlist, second, "", &error);
    check(!entryA.empty() && !entryB.empty(), "entries name a show and get ids");
    check(docs.addPlaylistEntry(playlist, "show999", "", &error).empty(),
          "an entry needs an existing show");
    const Id entryMid = docs.insertPlaylistEntry(playlist, first, "", 1, &error);
    check(!entryMid.empty() && docs.findPlaylist(playlist)->entries[1].id == entryMid,
          "the same show may run twice, inserted mid-list");
    check(docs.insertPlaylistEntry(playlist, first, "", 9, &error).empty(),
          "insert past the end is refused");
    check(docs.findPlaylist(playlist)->entries[0].section == "Gathering" &&
              docs.findPlaylist(playlist)->entries[1].section.empty(),
          "sections round-trip, empty means none");
    check(docs.setPlaylistEntrySection(playlist, entryB, "Dismiss", &error) &&
              docs.findPlaylist(playlist)->entries[2].section == "Dismiss",
          "entry sections update");

    check(docs.movePlaylistEntry(playlist, entryB, 0, &error) &&
              docs.findPlaylist(playlist)->entries[0].id == entryB,
          "entries move");
    check(!docs.movePlaylistEntry(playlist, entryB, 3, &error),
          "an out-of-range destination is refused");
    check(docs.removePlaylistEntry(playlist, entryMid, &error) &&
              docs.findPlaylist(playlist)->entries.size() == 2,
          "entries remove");
    check(!docs.removePlaylistEntry(playlist, entryMid, &error), "removing twice fails");

    // Deleting the show reaps its entries everywhere...
    check(docs.removeShow(first, &error) &&
              docs.findPlaylist(playlist)->entries.size() == 1 &&
              docs.findPlaylist(playlist)->entries[0].showId == second,
          "removing a show drops the playlist entries naming it");
    // ...and removing the playlist keeps the shows.
    check(docs.removePlaylist(playlist, &error) && docs.findPlaylist(playlist) == nullptr &&
              docs.findShow(second) != nullptr,
          "removing the playlist keeps the shows alive");
    check(docs.playlistIds().empty(), "playlistIds empties");
}

void exerciseFailureLeavesNoTrace() {
    Documents docs;
    std::string error;
    const Id show = docs.createShow("Deck", &error);
    const Id a = docs.appendSlide(show, &error);
    const Id b = docs.appendSlide(show, &error);

    // Every failing mutation must leave the model byte-identical.
    docs.moveSlide(show, b, 0, &error);  // legal: b, a
    const auto before = docs.findShow(show)->slides;
    check(!docs.moveSlide(show, a, 9, &error) && !docs.removeSlide(show, "slide999", &error) &&
              !docs.setSlideLines(show, "slide999", {"boom"}, &error) &&
              !docs.setSlideGroup(show, "slide999", "Nope", &error) &&
              !docs.renameShow(show, "", &error),
          "five different invalid mutations all refuse");
    const auto& after = docs.findShow(show)->slides;
    check(before.size() == after.size() && before[0].id == after[0].id &&
              before[1].id == after[1].id && after[0].id == b && after[0].group.empty() &&
              after[0].lines.empty(),
          "the model is untouched after the refusals");
}

// --- persistence (task 2.2) ---------------------------------------------------

namespace fs = std::filesystem;

bool writeFile(const fs::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << bytes;
    return out.good();
}

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

bool replaceOnce(std::string& text, const std::string& from, const std::string& to) {
    const size_t position = text.find(from);
    if (position == std::string::npos) {
        return false;
    }
    text.replace(position, from.size(), to);
    return true;
}

// Builds the "sample service": two shows with groups and notes, two
// styles with default + override, a playlist with a repeat show and
// sections, and one background asset whose file lives outside the
// package (created at assetSource).
void buildSampleService(Documents* docs, const fs::path& assetSource) {
    const Id house = docs->createStyle("House", "#ffffff", "#202040", "DejaVu Serif", 64.0);
    const Id loud = docs->createStyle("Loud", "#ffff00", "#000000", "Impact", 96.0);
    const Id asset = docs->addAsset("stage loop.jpg", assetSource.string());

    const Id order = docs->createShow("Service Order");
    docs->setShowDefaultStyle(order, house);
    const Id intro = docs->appendSlide(order);
    const Id psalm = docs->appendSlide(order);
    const Id chorus1 = docs->appendSlide(order);
    const Id chorus2 = docs->appendSlide(order);
    docs->setShowNotes(order, "communion after the second chorus");
    docs->setSlideLines(order, intro, {"Come, all people", ""});
    docs->setSlideLines(order, psalm, {"The earth is the Lord's", "and the fullness thereof"});
    docs->setSlideGroup(order, intro, "Intro");
    docs->setSlideGroup(order, psalm, "Psalm");
    docs->setSlideGroup(order, chorus1, "Chorus");
    docs->setSlideGroup(order, chorus2, "Chorus");
    docs->setSlideNotes(order, chorus1, "repeat ad lib");
    docs->setSlideBackground(order, chorus1, asset);

    const Id hymn = docs->createShow("Hymn");
    const Id verse = docs->appendSlide(hymn);
    const Id finalVerse = docs->appendSlide(hymn);
    docs->setSlideLines(hymn, verse, {"Old 100th, line one"});
    docs->setSlideLines(hymn, finalVerse, {"Old 100th, amen"});
    docs->setSlideStyle(hymn, finalVerse, loud);

    const Id playlist = docs->createPlaylist("Sunday");
    docs->addPlaylistEntry(playlist, order, "Gathering");
    docs->addPlaylistEntry(playlist, hymn, "");
    docs->addPlaylistEntry(playlist, order, "Dismiss");
}

// Field-by-field model comparison (the Documents API is the only view).
bool sameService(const Documents& a, const Documents& b) {
    if (a.showIds() != b.showIds() || a.styleIds() != b.styleIds() ||
        a.playlistIds() != b.playlistIds() || a.assetIds() != b.assetIds()) {
        return false;
    }
    for (const Id& showId : a.showIds()) {
        const Show* sa = a.findShow(showId);
        const Show* sb = b.findShow(showId);
        if (sa == nullptr || sb == nullptr || sa->name != sb->name ||
            sa->notes != sb->notes || sa->defaultStyleId != sb->defaultStyleId ||
            sa->slides.size() != sb->slides.size()) {
            return false;
        }
        for (size_t i = 0; i < sa->slides.size(); ++i) {
            const Slide& x = sa->slides[i];
            const Slide& y = sb->slides[i];
            if (x.id != y.id || x.group != y.group || x.lines != y.lines ||
                x.notes != y.notes || x.styleId != y.styleId ||
                x.backgroundAssetId != y.backgroundAssetId) {
                return false;
            }
        }
    }
    for (const Id& styleId : a.styleIds()) {
        const Style* xa = a.findStyle(styleId);
        const Style* xb = b.findStyle(styleId);
        if (xa == nullptr || xb == nullptr || xa->name != xb->name ||
            xa->textColor != xb->textColor || xa->backgroundColor != xb->backgroundColor ||
            xa->fontFamily != xb->fontFamily || xa->fontSize != xb->fontSize) {
            return false;
        }
    }
    for (const Id& playlistId : a.playlistIds()) {
        const Playlist* pa = a.findPlaylist(playlistId);
        const Playlist* pb = b.findPlaylist(playlistId);
        if (pa == nullptr || pb == nullptr || pa->name != pb->name ||
            pa->entries.size() != pb->entries.size()) {
            return false;
        }
        for (size_t i = 0; i < pa->entries.size(); ++i) {
            if (pa->entries[i].id != pb->entries[i].id ||
                pa->entries[i].showId != pb->entries[i].showId ||
                pa->entries[i].section != pb->entries[i].section) {
                return false;
            }
        }
    }
    for (const Id& assetId : a.assetIds()) {
        const Asset* aa = a.findAsset(assetId);
        const Asset* ab = b.findAsset(assetId);
        if (aa == nullptr || ab == nullptr || aa->name != ab->name) {
            return false;
        }
    }
    return true;
}

// Writes a tampered copy of the good package to `target` applying
// `tamper` to the manifest text; returns false if the tamper did not
// even find its anchor (a broken test, not a passing load).
bool tamperedPackage(const fs::path& good, const fs::path& target,
                     const std::function<bool(std::string&)>& tamper) {
    std::string manifest = readFile(good / "service.json");
    if (!tamper(manifest)) {
        return false;
    }
    std::filesystem::create_directories(target);
    std::filesystem::copy(good / "assets", target / "assets",
                          std::filesystem::copy_options::recursive);
    return writeFile(target / "service.json", manifest);
}

void exercisePersistence(const fs::path& tempRoot) {
    std::string error;
    std::filesystem::create_directories(tempRoot);
    const fs::path package = tempRoot / "service";
    const fs::path assetSource = tempRoot / "outside";
    writeFile(assetSource, "PNGBYTES-not-really-a-jpeg");

    Documents source;
    buildSampleService(&source, assetSource);
    check(!source.showIds().empty() && !source.assetIds().empty(),
          "the sample service builds");

    // --- save: manifest appears, asset bytes join the package ---
    check(openshow::domain::saveService(source, package, &error), "saveService succeeds");
    check(fs::is_regular_file(package / "service.json"), "the package holds service.json");
    const Id asset = source.assetIds().front();
    const fs::path packaged = package / "assets" / (asset + "-stage_loop.jpg");
    check(readFile(packaged) == "PNGBYTES-not-really-a-jpeg",
          "the asset was copied into assets/ under '<id>-stage_loop.jpg' with its bytes");

    // --- load: field-by-field identical service ---
    Documents reloaded;
    check(openshow::domain::loadService(&reloaded, package, &error),
          "loadService reads the package back");
    check(sameService(source, reloaded),
          "the reloaded service matches field by field (ids, order, refs)");
    const Id order = source.showIds()[0];
    check(reloaded.groups(order) == source.groups(order) &&
              reloaded.slidesInGroup(order, "Chorus") == source.slidesInGroup(order, "Chorus"),
          "groups resolve identically after the round-trip");
    const Id hymn = source.showIds()[1];
    const Id amen = source.findShow(hymn)->slides[1].id;
    check(reloaded.styleForSlide(hymn, amen) != nullptr &&
              source.styleForSlide(hymn, amen) != nullptr &&
              reloaded.styleForSlide(hymn, amen)->id == source.styleForSlide(hymn, amen)->id,
          "style resolution survives (override and default)");
    const Asset* reloadedAsset = reloaded.findAsset(asset);
    check(reloadedAsset != nullptr && readFile(reloadedAsset->sourcePath) ==
              "PNGBYTES-not-really-a-jpeg",
          "the loaded asset's sourcePath points at the packaged file");

    // --- re-save stability: the manifest is byte-identical, and a
    // reloaded service re-saves identically too ---
    const std::string firstManifest = readFile(package / "service.json");
    const Id freshAsset = reloaded.findAsset(asset)->id;  // now points into the package
    check(openshow::domain::saveService(reloaded, package, &error),
          "re-saving over an existing package succeeds");
    check(readFile(package / "service.json") == firstManifest,
          "re-save writes a byte-identical manifest");
    (void)freshAsset;

    const fs::path copied = tempRoot / "resaved";
    Documents fromPackage;
    check(openshow::domain::loadService(&fromPackage, package, &error) &&
              openshow::domain::saveService(fromPackage, copied, &error) &&
              readFile(copied / "service.json") == firstManifest,
          "save(load(save(s))) is byte-stable");
    check(readFile(copied / "assets" / (asset + "-stage_loop.jpg")) ==
              "PNGBYTES-not-really-a-jpeg",
          "the asset survives second-hand packaging");

    // --- id continuity: the reloaded store must not recycle ---
    const Id freshShow = reloaded.createShow("Later", &error);
    bool freshIsNew = !freshShow.empty();
    for (const Id& id : source.showIds()) {
        freshIsNew = freshIsNew && id != freshShow;
    }
    check(freshIsNew && reloaded.findShow(freshShow) != nullptr,
          "a show created after loading gets a brand-new id");

    // --- save fails loudly on a missing source file ---
    Documents broken;
    buildSampleService(&broken, tempRoot / "gone.jpg");
    const fs::path brokenDir = tempRoot / "broken-save";
    check(!openshow::domain::saveService(broken, brokenDir, &error) &&
              error.find("gone.jpg") != std::string::npos,
          "saving an asset whose file vanished fails naming the asset");

    // --- load refusals: each names its cause, the target survives ---
    Documents keeper;
    keeper.createShow("Untouched", &error);
    const auto refuses = [&](const fs::path& dir, const char* expectIn, const char* what) {
        error.clear();
        Documents attempt;
        attempt.createShow("Keeper");
        check(!openshow::domain::loadService(&attempt, dir, &error) &&
                  error.find(expectIn) != std::string::npos &&
                  attempt.showIds().size() == 1 &&
                  attempt.findShow(attempt.showIds()[0])->name == "Keeper",
              what);
        (void)keeper;
    };
    {
        const fs::path dir = tempRoot / "garbage";
        fs::create_directories(dir);
        writeFile(dir / "service.json", readFile(package / "service.json") + "}}broken");
        refuses(dir, "malformed", "a corrupt manifest is refused");
    }
    {
        const fs::path dir = tempRoot / "future";
        check(tamperedPackage(package, dir, [](std::string& m) {
                  return replaceOnce(m, "\"format\":1", "\"format\":999");
              }),
              "test tamper anchors exist (format)");
        refuses(dir, "unsupported format", "a future format version is refused");
    }
    {
        const fs::path dir = tempRoot / "dangling-show";
        check(tamperedPackage(package, dir, [](std::string& m) {
                  return replaceOnce(m, "\"showId\":\"", "\"showId\":\"ghost-");
              }),
              "test tamper anchors exist (showId)");
        refuses(dir, "unknown show", "an entry naming a vanished show is refused");
    }
    {
        const fs::path dir = tempRoot / "dangling-style";
        const Id loud = [&] {
            for (const Id& id : source.styleIds()) {
                if (source.findStyle(id)->name == "Loud") {
                    return id;
                }
            }
            return Id();
        }();
        check(tamperedPackage(package, dir, [&](std::string& m) {
                  return replaceOnce(m, "\"styleId\":\"" + loud + "\"",
                                     "\"styleId\":\"style-gone\"");
              }),
              "test tamper anchors exist (styleId)");
        refuses(dir, "unknown style", "a slide naming a vanished style is refused");
    }
    {
        const fs::path dir = tempRoot / "no-asset-bytes";
        check(tamperedPackage(package, dir, [](std::string& m) { return true; }),
              "test tamper anchors exist (copy)");
        fs::remove_all(dir / "assets");
        refuses(dir, "missing from the package",
                "an asset file deleted from the package fails the load");
    }
    {
        const fs::path dir = tempRoot / "duplicate";
        const Id first = source.findShow(order)->slides[0].id;
        const Id second = source.findShow(order)->slides[1].id;
        check(tamperedPackage(package, dir, [&](std::string& m) {
                  return replaceOnce(m, "\"" + second + "\"",
                                     "\"" + first + "\"");
              }),
              "test tamper anchors exist (duplicate)");
        refuses(dir, "duplicate id", "a repeated id is refused");
    }
    {
        Documents attempt;
        attempt.createShow("Keeper");
        error.clear();
        check(!openshow::domain::loadService(&attempt, tempRoot / "nowhere", &error) &&
                  attempt.showIds().size() == 1,
              "loading a directory with no manifest fails and leaves the target alone");
    }
}

// --- undo/redo and autosave (task 2.3) ---------------------------------------

void exerciseHistory() {
    std::string error;
    Documents docs;
    History history(&docs);
    check(!history.canUndo() && !history.canRedo(),
          "a fresh history has nothing to undo or redo");
    check(!history.undo(&error) && !error.empty(),
          "undo with an empty stack refuses with a message");

    Id show;
    check(history.apply([&] { show = docs.createShow("Order"); return !show.empty(); }),
          "apply runs a creation");
    Id slide;
    check(history.apply([&] { slide = docs.appendSlide(show); return !slide.empty(); }),
          "apply runs an append");
    check(history.apply([&] { return docs.setSlideLines(show, slide, {"First"}); }),
          "apply runs an edit");
    check(docs.findShow(show)->slides[0].lines == std::vector<std::string>({"First"}),
          "the edit is live in the store");
    check(history.undoDepth() == 3, "three edits sit on the undo stack");

    check(history.undo(&error), "undo succeeds");
    check(docs.findShow(show)->slides.size() == 1 &&
              docs.findShow(show)->slides[0].lines.empty(),
          "undo reverses the lines edit");
    check(history.canRedo(), "the undone edit waits on the redo stack");
    check(history.redo(&error), "redo replays the edit");
    check(docs.findShow(show)->slides[0].lines == std::vector<std::string>({"First"}),
          "the edit is back");

    check(history.undo(&error) && history.undo(&error), "undo twice more");
    check(docs.findShow(show)->slides.empty(), "undoing the append removed the slide");
    check(history.redo(&error) && history.redo(&error), "redo twice back to the edge");
    check(docs.findShow(show)->slides.size() == 1 &&
              docs.findShow(show)->slides[0].id == slide,
          "redo restores the very same slide, id included");

    const std::size_t depth = history.undoDepth();
    check(!history.apply([&] { return docs.setSlideLines(show, "slide-missing", {"X"}); }),
          "apply reports a refused edit as false");
    check(history.undoDepth() == depth && !history.canRedo(),
          "a refusal leaves both stacks untouched");

    check(history.undo(&error), "undo one");
    check(history.canRedo(), "the undone edit could be replayed");
    check(history.apply([&] { return docs.setShowNotes(show, "new direction"); }),
          "an edit made after undoing branches the history");
    check(!history.canRedo(), "the abandoned branch is discarded");

    // Undoing a creation must not put its id back into circulation.
    Id doomed;
    check(history.apply([&] { doomed = docs.createShow("Doomed"); return !doomed.empty(); }),
          "a show is created through history");
    check(history.undo(&error) && docs.findShow(doomed) == nullptr,
          "undo removes the created show");
    Id replacement;
    check(history.apply([&] { replacement = docs.createShow("Replacement");
                              return !replacement.empty(); }),
          "a later creation succeeds");
    check(replacement != doomed,
          "ids never recycle across undo (the watermark keeps the counter moving)");

    // Whole-service rewind: undo everything to an empty store, redo it
    // all back, comparing field by field.
    Documents whole;
    History wholeHistory(&whole);
    Id wholeShow;
    wholeHistory.apply([&] { wholeShow = whole.createShow("Whole"); return !wholeShow.empty(); });
    Id wholeSlide;
    wholeHistory.apply([&] { wholeSlide = whole.appendSlide(wholeShow); return !wholeSlide.empty(); });
    wholeHistory.apply([&] { return whole.setSlideGroup(wholeShow, wholeSlide, "Tag"); });
    wholeHistory.apply([&] { return whole.setShowNotes(wholeShow, "note"); });
    const Documents edited = whole;
    const std::size_t applied = wholeHistory.undoDepth();
    std::size_t undoneCount = 0;
    while (wholeHistory.canUndo() && wholeHistory.undo(&error)) {
        ++undoneCount;
    }
    check(undoneCount == applied && whole.showIds().empty(),
          "undoing all the way back leaves an empty store");
    std::size_t redoneCount = 0;
    while (wholeHistory.canRedo() && wholeHistory.redo(&error)) {
        ++redoneCount;
    }
    check(redoneCount == applied && sameService(whole, edited),
          "redoing all the way forward rebuilds the exact service");

    for (std::size_t i = 0; i < History::kMaxDepth + 5; ++i) {
        wholeHistory.apply([&] { return whole.setShowNotes(wholeShow, "note " + std::to_string(i)); });
    }
    check(wholeHistory.undoDepth() == History::kMaxDepth,
          "the undo stack stays bounded at kMaxDepth");
}

void exerciseAutosave(const fs::path& tempRoot) {
    std::string error;
    std::filesystem::create_directories(tempRoot);
    const fs::path scratch = tempRoot / "autosave";
    const fs::path assetSource = tempRoot / "autosave-src.jpg";
    writeFile(assetSource, "AUTOSAVEBYTES");

    Documents docs;
    buildSampleService(&docs, assetSource);
    History history(&docs);

    Autosave autosave(scratch);
    check(!Autosave::exists(scratch), "no scratch package exists before the first tick");
    check(autosave.tick(docs, &error), "the first tick writes the scratch package");
    check(Autosave::exists(scratch), "the autosave package appears");
    const Id asset = docs.assetIds().front();
    check(fs::is_regular_file(scratch / "assets" / (asset + "-stage_loop.jpg")),
          "autosave packages assets too");

    // Unchanged state means no disk access: delete the manifest and a
    // tick must not put it back. A refused edit must not either.
    fs::remove(scratch / "service.json");
    check(autosave.tick(docs, &error) && !fs::exists(scratch / "service.json"),
          "an unchanged tick touches no disk");

    history.apply([&] { return docs.setShowNotes("show-missing", "nope"); });  // refused
    check(autosave.tick(docs, &error) && !fs::exists(scratch / "service.json"),
          "a refused edit does not dirty the autosave baseline");

    const Id order = docs.showIds()[0];
    check(history.apply([&] { return docs.setShowNotes(order, "touched"); }),
          "an edit goes through history");
    check(autosave.tick(docs, &error) && fs::is_regular_file(scratch / "service.json"),
          "the edit reaches the next tick");

    // The interruption: everything is thrown away, a fresh session
    // finds the scratch package and recovers the whole service.
    Documents recovered;
    Autosave nextSession(scratch);
    check(nextSession.recover(&recovered, &error), "a new session recovers the scratch package");
    check(sameService(docs, recovered), "the recovered service matches the interrupted one");
    fs::remove(scratch / "service.json");
    check(nextSession.tick(recovered, &error) && !fs::exists(scratch / "service.json"),
          "the recovered baseline is not immediately rewritten");

    // Crash debris: a half-written scratch must be refused, not
    // trusted, and must leave the recovery target alone.
    Documents keeper;
    keeper.createShow("Keeper", &error);
    writeFile(scratch / "service.json", "{ \"format\": 1, trunca");
    Autosave debrisSession(scratch);
    check(!debrisSession.recover(&keeper, &error) && keeper.showIds().size() == 1,
          "a corrupted scratch package is refused and the target survives");

    check(debrisSession.clear(&error) && !fs::exists(scratch),
          "clear removes the scratch package");
    Documents nowhere;
    check(!Autosave(scratch).recover(&nowhere, &error),
          "recovering from a cleared scratch fails cleanly");
}

}  // namespace

int main() {
    const std::string_view version = openshow::domain::applicationVersion();
    bool ok = check(!version.empty(), "applicationVersion is not empty");
    ok = check(version.find('.') != std::string_view::npos,
               "applicationVersion contains a dot separator") && ok;
    ok = check(std::isdigit(static_cast<unsigned char>(version.front())) != 0,
               "applicationVersion starts with a digit") && ok;

    exerciseShows();
    exerciseSlides();
    exerciseGroups();
    exerciseStyles();
    exercisePlaylists();
    exerciseFailureLeavesNoTrace();

    // Persistence works in a private temp directory, removed at exit.
    const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() /
                                           ("openshow-domain-tests-" + std::to_string(getpid()));
    std::filesystem::remove_all(tempRoot);
    exercisePersistence(tempRoot);
    std::filesystem::remove_all(tempRoot);

    exerciseHistory();

    const std::filesystem::path autosaveRoot = std::filesystem::temp_directory_path() /
                                              ("openshow-domain-autosave-" + std::to_string(getpid()));
    std::filesystem::remove_all(autosaveRoot);
    exerciseAutosave(autosaveRoot);
    std::filesystem::remove_all(autosaveRoot);

    if (!ok || gFailed) {
        return 1;
    }
    std::printf("ok: applicationVersion \"%s\"; document model exercised\n",
                std::string(version).c_str());
    return 0;
}
