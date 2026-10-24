/*
 * Checks the shared version string, then the document model (task 2.1):
 * shows, slides, groups, styles and playlists — every operation with its
 * success path, its failure path, and the invariant the failure must not
 * have changed anything. Fails with a non-zero exit status so ctest
 * reports it.
 */

#include <cctype>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "openshow/domain/documents.h"
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

    if (!ok || gFailed) {
        return 1;
    }
    std::printf("ok: applicationVersion \"%s\"; document model exercised\n",
                std::string(version).c_str());
    return 0;
}
