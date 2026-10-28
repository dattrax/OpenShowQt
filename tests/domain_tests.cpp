/*
 * Checks the shared version string, then the document model (task 2.1):
 * shows, slides, groups, styles and playlists — every operation with its
 * success path, its failure path, and the invariant the failure must not
 * have changed anything. Fails with a non-zero exit status so ctest
 * reports it.
 */

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <thread>
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
using openshow::domain::AssetRebind;
using openshow::domain::kDefaultLineSpacing;
using openshow::domain::kMaxSlideItems;
using Asset = openshow::domain::Asset;
using Autosave = openshow::domain::Autosave;
using History = openshow::domain::History;
using Playlist = openshow::domain::Playlist;
using Show = openshow::domain::Show;
using Slide = openshow::domain::Slide;
using Style = openshow::domain::Style;
using SlideItem = openshow::domain::SlideItem;
using openshow::domain::assetPathLooksLikeVideo;
using ItemKind = openshow::domain::ItemKind;

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
              docs.findShow(show)->slides[2].items[0].lines.size() == 3 &&
              docs.findShow(show)->slides[2].items[0].lines[1].empty(),
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

void exerciseGeometry() {
    Documents docs;
    std::string error;
    const Id show = docs.createShow("Deck", &error);
    const Id slide = docs.appendSlide(show, &error);

    // A fresh slide carries ONE box on the authored lyric geometry: the
    // canvas minus the margins, so untouched slides keep drawing exactly as
    // pre-4.3 did — and the box arrives with an id, because from the moment
    // it exists an edit has to be able to name it (task 6.17a).
    const auto& fresh = docs.findShow(show)->slides[0];
    check(fresh.items.size() == 1 && !fresh.items[0].id.empty(),
          "a new slide starts with exactly one identified text box");
    check(fresh.items[0].x == 160.0 && fresh.items[0].y == 100.0 &&
              fresh.items[0].width == 1600.0 && fresh.items[0].height == 880.0,
          "a new slide starts on the default lyric box");

    check(docs.setSlideGeometry(show, slide, 300.5, 200.0, 1200.0, 500.0, &error) &&
              docs.findShow(show)->slides[0].items[0].x == 300.5 &&
              docs.findShow(show)->slides[0].items[0].y == 200.0 &&
              docs.findShow(show)->slides[0].items[0].width == 1200.0 &&
              docs.findShow(show)->slides[0].items[0].height == 500.0,
          "setSlideGeometry moves and resizes the box");
    check(!docs.setSlideGeometry(show, slide, std::nan(""), 0.0, 100.0, 100.0, &error),
          "a NaN box edge is refused");
    check(!docs.setSlideGeometry(show, slide, 0.0, 0.0, 0.0, 100.0, &error) &&
              !docs.setSlideGeometry(show, slide, 0.0, 0.0, 100.0, -5.0, &error),
          "a non-positive width or height is refused");
    check(!docs.setSlideGeometry(show, "slide999", 0.0, 0.0, 100.0, 100.0, &error),
          "geometry on an unknown slide fails");
    // Every refusal above left the accepted box untouched.
    check(docs.findShow(show)->slides[0].items[0].x == 300.5 &&
              docs.findShow(show)->slides[0].items[0].height == 500.0,
          "a refused geometry edit leaves the box as it was");
}

// Task 6.17a (review R17): a slide is a STACK of text boxes, each with an id
// the store issued and every edit addresses it by. What an operator needs from
// this is order (what covers what), identity (that moving a box does not move
// their selection) and a ceiling they meet while editing rather than while
// presenting.
void exerciseTextBoxes() {
    Documents docs;
    std::string error;
    const Id show = docs.createShow("Deck", &error);
    const Id slide = docs.appendSlide(show, &error);
    const Id first = docs.findShow(show)->slides[0].items[0].id;

    SlideItem second;
    second.id = "already-taken";  // a caller's id is dropped, never adopted
    second.lines = {"second box"};
    second.x = 100.0;
    second.y = 200.0;
    second.width = 300.0;
    second.height = 120.0;
    const Id secondId = docs.addItem(show, slide, second, &error);
    const Slide& stacked = docs.findShow(show)->slides[0];
    check(!secondId.empty() && secondId != "already-taken" && stacked.items.size() == 2 &&
              stacked.items[1].id == secondId,
          "addItem puts a box on top of the stack under an id the store issued");
    check(stacked.items[1].lines == std::vector<std::string>({"second box"}) &&
              stacked.items[1].x == 100.0 && stacked.items[1].height == 120.0,
          "a new box arrives with the words it was given and the place it was given");
    // The rule that lets one slide be pasted twice: the same shape added
    // again is a DIFFERENT identity, so the two copies can never be edited
    // into each other.
    check(docs.addItem(show, slide, second, &error) != secondId,
          "adding the same box shape twice yields two distinct ids");
    check(docs.removeItem(show, slide, docs.findShow(show)->slides[0].items[2].id, &error),
          "clearing the third box back down to two");

    check(docs.setItemLines(show, slide, secondId, {"changed"}, &error) &&
              docs.findShow(show)->slides[0].items[1].lines ==
                  std::vector<std::string>({"changed"}) &&
              docs.findShow(show)->slides[0].items[0].lines.empty(),
          "setItemLines edits one box and not its neighbour");
    check(docs.setItemGeometry(show, slide, secondId, 0.0, 0.0, 640.0, 200.0, &error) &&
              docs.findShow(show)->slides[0].items[0].x == 160.0 &&
              docs.findShow(show)->slides[0].items[1].x == 0.0,
          "setItemGeometry moves one box and not its neighbour");
    check(!docs.setItemGeometry(show, slide, secondId, 0.0, 0.0, 0.0, 200.0, &error) &&
              !docs.setItemGeometry(show, slide, secondId, std::nan(""), 0.0, 640.0,
                                       200.0,
                                       &error) &&
              docs.findShow(show)->slides[0].items[1].width == 640.0,
          "an unplaceable box is refused naming the box, and the box keeps its size");
    SlideItem collapsed;
    collapsed.width = 0.0;
    check(docs.addItem(show, slide, collapsed, &error).empty() &&
              error.find("positive width") != std::string::npos &&
              docs.findShow(show)->slides[0].items.size() == 2,
          "a new box with nothing in it is refused naming the rule, stack unchanged");
    check(!docs.setItemLines(show, slide, "text999", {"x"}, &error) &&
              !docs.setItemGeometry(show, slide, "text999", 0.0, 0.0, 10.0, 10.0, &error) &&
              !docs.removeItem(show, slide, "text999", &error) &&
              !docs.moveItem(show, slide, "text999", 0, &error) &&
              error.find("text999") != std::string::npos,
          "every box operation refuses an unknown id naming that id");
    check(!docs.setItemLines(show, "slide999", secondId, {"x"}, &error),
          "a box edit on an unknown slide is refused");

    // The stack order IS the draw order, and a move names a destination in
    // the final list — the same rule moveSlide follows, so the operator's
    // hands do not have to learn a second one.
    check(docs.moveItem(show, slide, secondId, 0, &error) &&
              docs.findShow(show)->slides[0].items[0].id == secondId &&
              docs.findShow(show)->slides[0].items[1].id == first,
          "moveItem sends a box to the bottom of the stack");
    check(docs.moveItem(show, slide, secondId, 0, &error) &&
              docs.findShow(show)->slides[0].items[0].id == secondId,
          "moving a box onto its own slot succeeds and changes nothing");
    check(!docs.moveItem(show, slide, secondId, 2, &error),
          "a destination at the box count is out of range");

    // The ceiling is the scene wire's item limit: nine boxes would compile
    // into a scene the worker refuses, so the model refuses the ninth.
    bool filled = true;
    while (filled && docs.findShow(show)->slides[0].items.size() < kMaxSlideItems) {
        filled = !docs.addItem(show, slide, SlideItem{}, &error).empty();
    }
    check(filled && docs.findShow(show)->slides[0].items.size() == kMaxSlideItems,
          "a slide fills to the scene wire's box limit");
    check(docs.addItem(show, slide, SlideItem{}, &error).empty() &&
              error.find(std::to_string(kMaxSlideItems)) != std::string::npos &&
              docs.findShow(show)->slides[0].items.size() == kMaxSlideItems,
          "the next box is refused naming the limit, and the slide keeps its count");

    // Draining to nothing is legal — a background with no words is a slide an
    // operator makes — but the single-text operations then have nothing to
    // address, and refuse rather than silently growing a box back under
    // whoever was typing.
    std::vector<Id> drained;
    while (!docs.findShow(show)->slides[0].items.empty()) {
        const Id doomed = docs.findShow(show)->slides[0].items.front().id;
        drained.push_back(doomed);
        check(docs.removeItem(show, slide, doomed, &error), "draining one box");
    }
    check(docs.findShow(show)->slides[0].items.empty() &&
              !docs.setSlideLines(show, slide, {"into nothing"}, &error) &&
              error.find(slide) != std::string::npos,
          "a slide with no boxes refuses typing naming the slide");
    check(docs.findShow(show)->slides[0].items.empty(), "the refusal did not create a box");
    check(docs.addItem(show, slide, SlideItem{}, &error) != drained.front(),
          "a box added after the slide was emptied does not reuse the first id");

    // Addressing is BY ID, not by position (task 6.17a). Removing the MIDDLE
    // box of a three-box stack leaves the outer two in their order — a removal
    // that deleted whatever happened to sit at the front of the stack passes
    // every single-box fixture in this file, which is exactly why this one is
    // here rather than another empty-slide check.
    {
        Documents middle;
        const Id midShow = middle.createShow("Middle", &error);
        const Id midSlide = middle.appendSlide(midShow, &error);
        const Id low = middle.findShow(midShow)->slides[0].items[0].id;
        SlideItem shape;
        shape.lines = {"b"};
        const Id centre = middle.addItem(midShow, midSlide, shape, &error);
        const Id high = middle.addItem(midShow, midSlide, shape, &error);
        check(middle.removeItem(midShow, midSlide, centre, &error) &&
                  middle.findShow(midShow)->slides[0].items.size() == 2 &&
                  middle.findShow(midShow)->slides[0].items[0].id == low &&
                  middle.findShow(midShow)->slides[0].items[1].id == high,
              "removing the middle box by id leaves the outer two, in order");
        check(!middle.removeItem(midShow, midSlide, centre, &error) &&
                  middle.findShow(midShow)->slides[0].items.size() == 2,
              "a box that is already gone cannot be removed again");
    }
}

// Foreground image items (task 6.17b, review R17). What is being pinned here
// is the DISCIPLINE, not the drawing: a stack entry is text or a picture, the
// kind says which content fields mean anything, and an item the compiler could
// never honour is refused by the model rather than at present time.
void exerciseImageItems() {
    Documents docs;
    std::string error;
    const Id show = docs.createShow("Mixed", &error);
    const Id slide = docs.appendSlide(show, &error);
    const Id still = docs.addAsset("plate.jpg", "/media/plate.jpg", &error);
    const Id loop = docs.addAsset("loop.MP4", "/media/loop.MP4", &error);
    check(!still.empty() && !loop.empty(), "a still and a video asset are recorded");
    // The predicate the model refuses with is the one the scene lib classifies
    // backgrounds with, and it is case-insensitive: an asset called UPPER.MP4
    // is a video, and the check below is what stops that name list from
    // drifting apart from the model's rule.
    check(assetPathLooksLikeVideo("/media/loop.MP4") && !assetPathLooksLikeVideo("/media/a.png"),
          "assetPathLooksLikeVideo reads a name's kind, case-insensitively");

    SlideItem picture;
    picture.kind = ItemKind::Image;
    picture.assetId = still;
    picture.id = "borrowed";  // dropped, like every caller's id
    picture.x = 900.0;
    picture.y = 60.0;
    picture.width = 900.0;
    picture.height = 500.0;
    const Id pictureId = docs.addItem(show, slide, picture, &error);
    const Slide& withPicture = docs.findShow(show)->slides[0];
    check(!pictureId.empty() && pictureId != "borrowed" && withPicture.items.size() == 2 &&
              withPicture.items[1].id == pictureId &&
              withPicture.items[1].kind == ItemKind::Image &&
              withPicture.items[1].assetId == still && withPicture.items[1].x == 900.0,
          "addItem places a picture on top of the stack under an id the store issued");
    check(withPicture.items[1].lines.empty(),
          "an image item carries no words — the kind is what says so");
    check(pictureId.find("image") == 0, "the id the store issues names the kind it belongs to");
    // One id space across kinds: the counter is shared, so an image and a text
    // item created one after the other can never wear the same identity.
    SlideItem words;
    words.lines = {"over the plate"};
    const Id wordsId = docs.addItem(show, slide, words, &error);
    check(!wordsId.empty() && wordsId != pictureId &&
              docs.findShow(show)->slides[0].items[2].kind == ItemKind::Text,
          "a text item added beside a picture gets its own id in the same space");

    // --- kind discipline: a half-filled item is refused, naming the rule ---
    SlideItem bare;
    bare.kind = ItemKind::Image;
    check(docs.addItem(show, slide, bare, &error).empty() &&
              error.find("asset") != std::string::npos,
          "an image item naming no asset is refused naming the asset rule");
    bare.assetId = "asset-missing";
    check(docs.addItem(show, slide, bare, &error).empty() &&
              error.find("asset-missing") != std::string::npos,
          "an image item naming an unknown asset is refused naming that id");
    bare.assetId = loop;
    check(docs.addItem(show, slide, bare, &error).empty() &&
              error.find("video") != std::string::npos,
          "an image item naming a VIDEO asset is refused: foreground items composite stills");
    check(docs.findShow(show)->slides[0].items.size() == 3,
          "and none of the three refusals grew the stack");
    bare.assetId = still;
    bare.lines = {"words on a picture"};
    check(docs.addItem(show, slide, bare, &error).empty() &&
              error.find("words") != std::string::npos,
          "an image item carrying lines is refused naming the words rule");
    SlideItem texty;
    texty.assetId = still;
    check(docs.addItem(show, slide, texty, &error).empty() &&
              error.find("text item") != std::string::npos,
          "a text item naming an asset is refused: an asset belongs to an image");

    // Words belong to a text item. Naming a picture here is the mistake an
    // editor wired to the wrong item makes, and the operator typed something
    // that can never draw.
    check(!docs.setItemLines(show, slide, pictureId, {"into a jpeg"}, &error) &&
              error.find(pictureId) != std::string::npos &&
              docs.findShow(show)->slides[0].items[1].lines.empty(),
          "setItemLines refuses an image item naming that item, and the item stays empty");
    check(docs.setItemGeometry(show, slide, pictureId, 10.0, 20.0, 300.0, 200.0, &error) &&
              docs.findShow(show)->slides[0].items[1].x == 10.0 &&
              docs.findShow(show)->slides[0].items[1].width == 300.0,
          "setItemGeometry moves a picture exactly as it moves a text box");
    check(!docs.setItemGeometry(show, slide, pictureId, 0.0, 0.0, 100.0, -1.0, &error) &&
              docs.findShow(show)->slides[0].items[1].height == 200.0,
          "an unplaceable picture box is refused and the picture keeps its size");
    check(docs.moveItem(show, slide, pictureId, 0, &error) &&
              docs.findShow(show)->slides[0].items[0].id == pictureId,
          "moveItem sends a picture to the bottom of the draw stack");

    // The legacy single-text view addresses item 0, and item 0 can now be a
    // picture. Both lies are refused: writing words INTO the picture, and
    // inventing a text box underneath it.
    {
        Documents front;
        const Id frontShow = front.createShow("Front image", &error);
        const Id frontSlide = front.appendSlide(frontShow, &error);
        const Id plate = front.addAsset("plate.png", "/media/plate.png", &error);
        SlideItem cover;
        cover.kind = ItemKind::Image;
        cover.assetId = plate;
        const Id coverId = front.addItem(frontShow, frontSlide, cover, &error);
        const Id defaultBox = front.findShow(frontShow)->slides[0].items[0].id;
        check(!coverId.empty() && front.removeItem(frontShow, frontSlide, defaultBox, &error) &&
                  front.findShow(frontShow)->slides[0].items.front().id == coverId,
              "a slide whose only remaining item is a picture is a legal slide");
        check(!front.setSlideLines(frontShow, frontSlide, {"nowhere to type"}, &error) &&
                  error.find(coverId) != std::string::npos &&
                  front.findShow(frontShow)->slides[0].items.front().lines.empty(),
              "setSlideLines refuses a slide whose front item is an image, naming that item");
        check(front.setSlideGeometry(frontShow, frontSlide, 40.0, 40.0, 800.0, 400.0, &error) &&
                  front.findShow(frontShow)->slides[0].items.front().x == 40.0,
              "setSlideGeometry still edits the front item's box: geometry belongs to both kinds");
    }

    // The stack is ONE budget for both kinds: pictures cover words where they
    // overlap, and what bounds a composition is the scene message, not the
    // kind of the thing being added.
    bool filled = true;
    while (filled &&
           docs.findShow(show)->slides[0].items.size() < kMaxSlideItems) {
        SlideItem next;
        if (docs.findShow(show)->slides[0].items.size() % 2 == 0) {
            next.kind = ItemKind::Image;
            next.assetId = still;
        }
        filled = !docs.addItem(show, slide, next, &error).empty();
    }
    check(filled && docs.findShow(show)->slides[0].items.size() == kMaxSlideItems,
          "a slide fills to the scene wire's item limit with kinds mixed");
    SlideItem ninth;
    ninth.kind = ItemKind::Image;
    ninth.assetId = still;
    check(docs.addItem(show, slide, ninth, &error).empty() &&
              error.find(std::to_string(kMaxSlideItems)) != std::string::npos &&
              docs.findShow(show)->slides[0].items.size() == kMaxSlideItems,
          "a ninth item is refused naming the limit whether it is a picture or words");

    // Deleting the asset takes its foreground items with it. A background
    // degrades to none; a stack entry without its picture is not a half-drawn
    // item but a claim about the composition the worker could never honour.
    {
        Documents owner;
        History history(&owner);
        const Id ownerShow = owner.createShow("Owner", &error);
        const Id ownerSlide = owner.appendSlide(ownerShow, &error);
        const Id firstAsset = owner.addAsset("one.jpg", "/media/one.jpg", &error);
        const Id secondAsset = owner.addAsset("two.jpg", "/media/two.jpg", &error);
        const Id keepText = owner.findShow(ownerShow)->slides[0].items[0].id;
        SlideItem over;
        over.kind = ItemKind::Image;
        over.assetId = firstAsset;
        const Id dropOne = owner.addItem(ownerShow, ownerSlide, over, &error);
        over.assetId = secondAsset;
        const Id keepTwo = owner.addItem(ownerShow, ownerSlide, over, &error);
        check(!dropOne.empty() && !keepTwo.empty(), "two pictures on one slide are placed");
        check(history.apply([&] { return owner.removeAsset(firstAsset); }),
              "removing the asset runs through history");
        check(owner.findShow(ownerShow)->slides[0].items.size() == 2,
              "removing an asset drops exactly one stack entry");
        const Slide& after = owner.findShow(ownerShow)->slides[0];
        check(after.items[0].id == keepText && after.items[1].id == keepTwo,
              "the words and the other picture survive, in order — a removal that "
              "deleted whatever sat at the front would pass every other fixture here");
        bool orphaned = false;
        for (const Id& showId : owner.showIds()) {
            for (const Slide& sl : owner.findShow(showId)->slides) {
                for (const SlideItem& it : sl.items) {
                    if (it.kind == ItemKind::Image && it.assetId == firstAsset) {
                        orphaned = true;
                    }
                }
            }
        }
        check(!orphaned && owner.findAsset(firstAsset) == nullptr,
              "no stack entry anywhere still names the deleted asset: a picture whose "
              "file is gone is not a half-drawn item");
        // The undo restores the asset AND the item beside it — history holds
        // whole states, and a slide the operator composed must not come back
        // with a hole in the middle of its stack.
        check(history.undo(&error) && owner.findAsset(firstAsset) != nullptr,
              "undo brings the asset back");
        const Slide& restored = owner.findShow(ownerShow)->slides[0];
        check(restored.items.size() == 3 && restored.items[1].id == dropOne &&
                  restored.items[1].kind == ItemKind::Image &&
                  restored.items[1].assetId == firstAsset,
              "and undo restores the picture with its own id in its own slot");
    }
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
                                      "center", kDefaultLineSpacing, &error);
    check(!style.empty(), "createStyle returns an id");
    check(docs.createStyle("Bad", "#12g456", "#000000", "x", 10, "center",
                           kDefaultLineSpacing, &error)
                  .empty() &&
              !error.empty(),
          "a non-hex text colour is refused");
    check(docs.createStyle("Bad", "#fff", "white", "x", 10, "center", kDefaultLineSpacing,
                           &error)
                  .empty(),
          "a named background colour is refused too");
    check(docs.createStyle("Bad", "#fff", "#fff", "x", -1, "center", kDefaultLineSpacing,
                           &error)
                  .empty(),
          "a negative font size is refused");

    check(docs.setShowDefaultStyle(show, style, &error) &&
              docs.styleForSlide(show, slide) != nullptr &&
              docs.styleForSlide(show, slide)->id == style,
          "the show default style resolves for unstyled slides");
    const Id other = docs.createStyle("Loud", "#ffff00", "#000000", "Impact", 96.0, "center",
                                      kDefaultLineSpacing, &error);
    check(docs.setSlideStyle(show, slide, other, &error) &&
              docs.styleForSlide(show, slide)->id == other,
          "a per-slide override beats the default");
    check(docs.setSlideStyle(show, slide, "", &error) &&
              docs.styleForSlide(show, slide)->id == style,
          "clearing the override falls back to the default");
    check(!docs.setSlideStyle(show, slide, "style999", &error),
          "an unknown override id is refused");

    check(docs.updateStyle(other, "#ff0000", "#000", "Arial", 0.0, "left", 2.0, &error) &&
              docs.findStyle(other)->textColor == "#ff0000" &&
              docs.findStyle(other)->backgroundColor == "#000" &&
              docs.findStyle(other)->fontSize == 0.0 &&
              docs.findStyle(other)->align == "left" &&
              docs.findStyle(other)->lineSpacing == 2.0,
          "updateStyle replaces the appearance and typography (0 size = renderer default)");
    check(!docs.updateStyle(other, "#ff0000", "#000", "Arial", 0.0, "middle",
                            kDefaultLineSpacing, &error),
          "an alignment outside left/center/right is refused");
    check(!docs.updateStyle(other, "#ff0000", "#000", "Arial", 0.0, "center", 0.0, &error) &&
              !docs.updateStyle(other, "#ff0000", "#000", "Arial", 0.0, "center", 99.0, &error),
          "line spacing outside 0.25..8 is refused");
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
              after[0].items[0].lines.empty(),
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
    const Id house = docs->createStyle("House", "#ffffff", "#202040", "DejaVu Serif", 64.0,
                                       "center", kDefaultLineSpacing);
    const Id loud = docs->createStyle("Loud", "#ffff00", "#000000", "Impact", 96.0, "center",
                                      kDefaultLineSpacing);
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
            if (x.id != y.id || x.group != y.group || x.notes != y.notes ||
                x.styleId != y.styleId || x.backgroundAssetId != y.backgroundAssetId) {
                return false;
            }
            // A slide's boxes are compared IN ORDER, id and geometry included
            // (task 6.17a). Order and identity are the document's own: a round
            // trip that swapped two boxes, or handed one a new id, would leave
            // the operator's selection pointing at something else. The old
            // comparison passed over the geometry silently, so this is also
            // where a saved box's place first gets pinned.
            if (x.items.size() != y.items.size()) {
                return false;
            }
            for (size_t box = 0; box < x.items.size(); ++box) {
                const SlideItem& bx = x.items[box];
                const SlideItem& by = y.items[box];
                if (bx.id != by.id || bx.kind != by.kind || bx.assetId != by.assetId ||
                    bx.lines != by.lines || bx.x != by.x || bx.y != by.y ||
                    bx.width != by.width || bx.height != by.height) {
                    return false;
                }
            }
        }
    }
    for (const Id& styleId : a.styleIds()) {
        const Style* xa = a.findStyle(styleId);
        const Style* xb = b.findStyle(styleId);
        if (xa == nullptr || xb == nullptr || xa->name != xb->name ||
            xa->textColor != xb->textColor || xa->backgroundColor != xb->backgroundColor ||
            xa->fontFamily != xb->fontFamily || xa->fontSize != xb->fontSize ||
            xa->align != xb->align || xa->lineSpacing != xb->lineSpacing) {
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

    // --- text box (task 4.3): authored geometry round-trips; a service
    // written before boxes existed loads with the default box; a box that
    // cannot be placed is refused naming its slide ---
    {
        Documents boxes;
        const Id boxShow = boxes.createShow("Boxed", &error);
        const Id boxSlide = boxes.appendSlide(boxShow, &error);
        check(boxes.setSlideGeometry(boxShow, boxSlide, 240.25, 180.0, 1400.0, 720.0, &error),
              "setSlideGeometry accepts an authored box");
        const fs::path boxed = tempRoot / "boxed";
        Documents back;
        check(openshow::domain::saveService(boxes, boxed, &error) &&
                  openshow::domain::loadService(&back, boxed, &error) &&
                  back.findShow(boxShow)->slides[0].items[0].x == 240.25 &&
                  back.findShow(boxShow)->slides[0].items[0].y == 180.0 &&
                  back.findShow(boxShow)->slides[0].items[0].width == 1400.0 &&
                  back.findShow(boxShow)->slides[0].items[0].height == 720.0,
              "a text box round-trips through the manifest");

        // A legacy manifest: the same shape without the four box fields.
        const fs::path legacy = tempRoot / "legacy";
        std::filesystem::create_directories(legacy);
        writeFile(legacy / "service.json",
                  R"({"format":1,"nextOrdinal":9,"shows":[{"id":"show1","name":"Legacy",)"
                  R"("slides":[{"id":"slide1","lines":["Ancient line"]}]}],)"
                  R"("styles":[],"assets":[],"playlists":[]})");
        Documents oldBack;
        check(openshow::domain::loadService(&oldBack, legacy, &error) &&
                  oldBack.findShow("show1")->slides[0].items[0].x == 160.0 &&
                  oldBack.findShow("show1")->slides[0].items[0].y == 100.0 &&
                  oldBack.findShow("show1")->slides[0].items[0].width == 1600.0 &&
                  oldBack.findShow("show1")->slides[0].items[0].height == 880.0,
              "a manifest without geometry loads with the default box");

        const fs::path warped = tempRoot / "warped";
        std::filesystem::create_directories(warped);
        writeFile(warped / "service.json",
                  R"({"format":1,"nextOrdinal":9,"shows":[{"id":"show1","name":"Warped",)"
                  R"("slides":[{"id":"slide1","lines":["x"],"x":0,"y":0,"width":0,)"
                  R"("height":100}]}],"styles":[],"assets":[],"playlists":[]})");
        Documents squashed;
        check(!openshow::domain::loadService(&squashed, warped, &error) &&
                  error.find("slide1") != std::string::npos,
              "a slide whose box cannot be placed is refused on load");
    }

    // --- typography (task 4.4): a style's align and line spacing
    // round-trip; a service written before typography existed loads with
    // centred text at the renderer's line box; typography the renderer
    // cannot honour is refused naming its style ---
    {
        Documents typed;
        const Id sharp = typed.createStyle("Sharp", "#ff8800", "#000000", "DejaVu Sans", 88.0,
                                           "right", 1.75, &error);
        check(!sharp.empty(), "createStyle accepts typography");
        const fs::path typeset = tempRoot / "typeset";
        Documents typeBack;
        check(openshow::domain::saveService(typed, typeset, &error) &&
                  openshow::domain::loadService(&typeBack, typeset, &error) &&
                  typeBack.findStyle(sharp)->align == "right" &&
                  typeBack.findStyle(sharp)->lineSpacing == 1.75,
              "style typography round-trips through the manifest");

        // A legacy manifest: the style entry simply lacks the two fields.
        const fs::path oldType = tempRoot / "old-type";
        std::filesystem::create_directories(oldType);
        writeFile(oldType / "service.json",
                  R"({"format":1,"nextOrdinal":9,"shows":[],)"
                  R"("styles":[{"id":"style1","name":"House","textColor":"#ffffff",)"
                  R"("backgroundColor":"#000000","fontFamily":"","fontSize":64}],)"
                  R"("assets":[],"playlists":[]})");
        Documents oldTypeBack;
        check(openshow::domain::loadService(&oldTypeBack, oldType, &error) &&
                  oldTypeBack.findStyle("style1") != nullptr &&
                  oldTypeBack.findStyle("style1")->align == "center" &&
                  oldTypeBack.findStyle("style1")->lineSpacing == kDefaultLineSpacing,
              "a manifest without typography loads centred at the default line box");

        const fs::path skewed = tempRoot / "skewed";
        std::filesystem::create_directories(skewed);
        writeFile(skewed / "service.json",
                  R"({"format":1,"nextOrdinal":9,"shows":[],)"
                  R"("styles":[{"id":"style9","name":"Odd","textColor":"#ffffff",)"
                  R"("backgroundColor":"#000000","fontFamily":"","fontSize":64,)"
                  R"("align":"middle","lineSpacing":1.25}],)"
                  R"("assets":[],"playlists":[]})");
        Documents tiltBack;
        check(!openshow::domain::loadService(&tiltBack, skewed, &error) &&
                  error.find("style9") != std::string::npos,
              "a style with an unknown align is refused on load naming its id");

        const fs::path crushed = tempRoot / "crushed";
        std::filesystem::create_directories(crushed);
        writeFile(crushed / "service.json",
                  R"({"format":1,"nextOrdinal":9,"shows":[],)"
                  R"("styles":[{"id":"style2","name":"Flat","textColor":"#ffffff",)"
                  R"("backgroundColor":"#000000","fontFamily":"","fontSize":64,)"
                  R"("align":"center","lineSpacing":0.01}],)"
                  R"("assets":[],"playlists":[]})");
        Documents flatBack;
        check(!openshow::domain::loadService(&flatBack, crushed, &error) &&
                  error.find("style2") != std::string::npos,
              "a style with a collapsed line spacing is refused on load");
    }

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
                  // The refusal is about a version this build does NOT read,
                  // so push up whatever number it actually wrote rather than
                  // hard-coding today's. The hard-coded "1" this test used
                  // until task 6.17a stopped matching the day the format moved
                  // to 2, and a tamper that finds no anchor is a test passing
                  // by not testing.
                  const std::string key = "\"format\":";
                  const size_t at = m.find(key);
                  if (at == std::string::npos) {
                      return false;
                  }
                  const size_t from = at + key.size();
                  size_t to = from;
                  while (to < m.size() && std::isdigit(static_cast<unsigned char>(m[to])) != 0) {
                      ++to;
                  }
                  const unsigned long long written = std::strtoull(
                      m.substr(from, to - from).c_str(), nullptr, 10);
                  m.replace(from, to - from, std::to_string(written + 1000));
                  return true;
              }),
              "test tamper anchors exist (format)");
        refuses(dir, "unsupported format", "a future format version is refused");
    }
    {
        const fs::path dir = tempRoot / "wrong-era";
        // Same push-the-written-number discipline as the format tamper
        // above: this build writes 3 (task 6.17b), and anchoring on a
        // literal would stop matching the day the format moves again.
        check(tamperedPackage(package, dir, [](std::string& m) {
                  const std::string key = "\"format\":";
                  const size_t at = m.find(key);
                  if (at == std::string::npos) {
                      return false;
                  }
                  const size_t from = at + key.size();
                  size_t to = from;
                  while (to < m.size() && std::isdigit(static_cast<unsigned char>(m[to])) != 0) {
                      ++to;
                  }
                  m.replace(from, to - from, "1");
                  return true;
              }),
              "test tamper anchors exist (this build's format)");
        // A package claiming the OLDER era is the dangerous lie: a format-1
        // reader looks for each slide's own "lines", and these slides keep
        // their words in boxes. The load must refuse rather than hand back a
        // service whose every slide has gone blank (task 6.17a).
        refuses(dir, "must be an array",
                "a format-2 manifest claiming to be format 1 is refused, not read as blank");
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

// Task 6.17a's storage half: what a slide's boxes look like in a file, what a
// file written before they existed means, and every way a boxes-shaped file
// can lie to the loader.
void exerciseBoxStorage(const fs::path& tempRoot) {
    std::string error;
    int seq = 0;
    const auto refusesJson = [&](const std::string& manifest, const std::string& expectIn,
                                 const char* what) {
        const fs::path dir = tempRoot / ("refuse-" + std::to_string(++seq));
        std::filesystem::create_directories(dir);
        writeFile(dir / "service.json", manifest);
        Documents attempt;
        error.clear();
        const bool refused = openshow::domain::loadService(&attempt, dir, &error);
        if (!check(!refused && error.find(expectIn) != std::string::npos, what)) {
            // Which refusal did we get instead (or none at all)? Without this
            // line the answer is a guess, and guessing is how a wrong expected
            // string gets "fixed" to whatever the code happens to say.
            std::fprintf(stderr, "   wanted \"%s\", load %s: %s\n", expectIn.c_str(),
                         refused ? "succeeded" : "said", error.c_str());
        }
    };

    // --- three boxes: order, identity and place all survive the file ---
    {
        Documents authored;
        const Id show = authored.createShow("Three boxes", &error);
        const Id slide = authored.appendSlide(show, &error);
        const Id firstId = authored.findShow(show)->slides[0].items[0].id;
        SlideItem middle;
        middle.lines = {"middle"};
        middle.x = 200.0;
        middle.y = 300.0;
        middle.width = 400.0;
        middle.height = 90.0;
        const Id middleId = authored.addItem(show, slide, middle, &error);
        SlideItem top;
        top.lines = {"top"};
        top.x = 20.0;
        top.y = 30.0;
        top.width = 500.0;
        top.height = 1000.0;
        const Id topId = authored.addItem(show, slide, top, &error);
        check(!middleId.empty() && !topId.empty() && middleId != firstId && topId != middleId,
              "two more boxes are accepted under their own ids");

        const fs::path package = tempRoot / "three-boxes";
        Documents back;
        check(openshow::domain::saveService(authored, package, &error) &&
                  openshow::domain::loadService(&back, package, &error),
              "a three-box slide saves and reloads");
        const Slide& reloaded = back.findShow(show)->slides[0];
        check(reloaded.items.size() == 3 && reloaded.items[0].id == firstId &&
                  reloaded.items[1].id == middleId && reloaded.items[2].id == topId,
              "the boxes come back in the same DRAW ORDER under the same ids");
        check(reloaded.items[1].lines == std::vector<std::string>({"middle"}) &&
                  reloaded.items[2].width == 500.0 && reloaded.items[2].y == 30.0,
              "each box keeps its own words and its own rectangle");
        check(readFile(package / "service.json").find("\"items\":[{\"id\"") !=
                  std::string::npos,
              "the manifest names its boxes and their ids, in order");

        // The file's order is the model's rather than a hash walk's, so saving
        // what was just loaded is byte-identical — and sameService() now
        // compares box ids and geometry, which it passed over before.
        const fs::path again = tempRoot / "three-boxes-again";
        Documents twice;
        check(openshow::domain::saveService(back, again, &error) &&
                  openshow::domain::loadService(&twice, again, &error) &&
                  sameService(back, twice) &&
                  readFile(again / "service.json") == readFile(package / "service.json"),
              "a multi-box service re-saves byte-identically and compares equal");

        // Order is document state, so reordering must change the file: a
        // manifest that recorded boxes in id order would lose the composition.
        check(back.moveItem(show, slide, topId, 0, &error), "the reloaded stack reorders");
        const fs::path reordered = tempRoot / "three-boxes-moved";
        Documents movedBack;
        check(openshow::domain::saveService(back, reordered, &error) &&
                  openshow::domain::loadService(&movedBack, reordered, &error) &&
                  movedBack.findShow(show)->slides[0].items[0].id == topId &&
                  readFile(reordered / "service.json") != readFile(package / "service.json"),
              "a reordered stack round-trips in its new order, in a different file");
    }

    // --- migration: a format-1 slide is ONE box, deterministically ---
    {
        const fs::path legacy = tempRoot / "migrate-1";
        std::filesystem::create_directories(legacy);
        writeFile(legacy / "service.json",
                  R"({"format":1,"nextOrdinal":9,"shows":[{"id":"show1","name":"Old",)"
                  R"("slides":[{"id":"slide1","lines":["Ancient","line"],)"
                  R"("x":50,"y":60,"width":700,"height":300}]}],)"
                  R"("styles":[],"assets":[],"playlists":[]})");
        Documents first;
        Documents second;
        error.clear();
        const bool loadedOnce = openshow::domain::loadService(&first, legacy, &error);
        error.clear();
        const bool loadedTwice = openshow::domain::loadService(&second, legacy, &error);
        check(loadedOnce && loadedTwice, "a format-1 service still loads");
        const Slide& migrated = first.findShow("show1")->slides[0];
        check(migrated.items.size() == 1 &&
                  migrated.items[0].lines == std::vector<std::string>({"Ancient", "line"}),
              "a format-1 slide reads as exactly one box carrying its words");
        check(migrated.items[0].x == 50.0 && migrated.items[0].y == 60.0 &&
                  migrated.items[0].width == 700.0 && migrated.items[0].height == 300.0,
              "the legacy slide's own geometry becomes the box's geometry");
        check(migrated.items[0].id == "slide1-text1" &&
                  second.findShow("show1")->slides[0].items[0].id == migrated.items[0].id,
              "the migrated box's derived id is the same on every load of that file");

        // The migration closes behind it: the next save writes the id, so from
        // then on the box's identity lives in the file like any other.
        const fs::path promoted = tempRoot / "migrate-promoted";
        Documents promotedBack;
        check(openshow::domain::saveService(first, promoted, &error) &&
                  openshow::domain::loadService(&promotedBack, promoted, &error),
              "the migrated service saves as format 2 and reloads");
        check(readFile(promoted / "service.json").find("\"id\":\"slide1-text1\"") !=
                  std::string::npos,
              "the re-save writes the migrated box's id into the file");
        check(promotedBack.findShow("show1")->slides[0].items[0].id == "slide1-text1" &&
                  promotedBack.findShow("show1")->slides[0].items[0].lines ==
                      std::vector<std::string>({"Ancient", "line"}),
              "the id and the words survive the promotion unchanged");
        check(promotedBack.setItemGeometry("show1", "slide1", "slide1-text1", 5.0, 5.0, 640.0,
                                              200.0, &error) &&
                  promotedBack.findShow("show1")->slides[0].items[0].x == 5.0,
              "a migrated box is editable by the id the migration gave it");
    }

    // --- a format-2 file can lie in eight ways; each is refused, naming it ---
    {
        const std::string head = R"({"format":2,"nextOrdinal":99,"shows":[{"id":"show1",)"
                                 R"("name":"Boxed","slides":[{"id":"slide1",)";
        const std::string tail = R"(]}],"styles":[],"assets":[],"playlists":[]})";
        const std::string goodBox =
            R"({"id":"text1","lines":["a"],"x":1,"y":1,"width":10,"height":10})";
        const fs::path goodDir = tempRoot / "v2-good";
        std::filesystem::create_directories(goodDir);
        writeFile(goodDir / "service.json", head + R"("items":[)" + goodBox + "]}" + tail);
        Documents fine;
        error.clear();
        check(openshow::domain::loadService(&fine, goodDir, &error) &&
                  fine.findShow("show1")->slides[0].items[0].id == "text1",
              "a hand-written format-2 manifest loads and the box keeps the file's id");

        const std::string dupeBox =
            R"({"id":"text1","lines":["b"],"x":1,"y":1,"width":10,"height":10})";
        refusesJson(head + R"("items":[)" + goodBox + "," + dupeBox + "]}" + tail,
                    "duplicate id",
                    "two boxes sharing an id are refused: an edit could not say which");
        refusesJson(head + R"("notes":"no boxes here"})" + tail, "must be an array",
                    "a format-2 slide with no items list is refused, not read as empty");
        refusesJson(head + R"("lines":["hand-bumped"],"items":[]})" + tail, "format 1",
                    "a format-1 slide inside a format-2 file is refused naming the slide");
        refusesJson(
            head + R"("items":[{"lines":["a"],"x":1,"y":1,"width":10,"height":10}]})" + tail,
            "must be a string", "a box with no id of its own is refused");
        refusesJson(head +
                        R"("items":[{"id":"../escape","lines":["a"],"x":1,"y":1,"width":10,)"
                        R"("height":10}]})" + tail,
                    "unsafe id", "a box id that is not a name is refused (6.13b's rule)");
        refusesJson(head + R"("items":[{"id":"text1","lines":["a"],"x":1,"y":1,"width":0,)"
                        R"("height":10}]})" + tail,
                    "cannot be placed",
                    "a box with no width is refused naming the slide and the box");
        refusesJson(head + R"("items":[{"id":"text1","x":1,"y":1,"width":10,"height":10}]})" +
                        tail,
                    "must be an array", "a box with no lines list is refused");
        refusesJson(head + R"("items":["text1"]})" + tail, "not an object",
                    "a bare string where a box should be is refused");

        std::string many = head + R"("items":[)";
        for (int i = 0; i <= static_cast<int>(kMaxSlideItems); ++i) {
            if (i > 0) {
                many += ",";
            }
            many += R"({"id":"text)" + std::to_string(i + 1) +
                    R"(","lines":[],"x":1,"y":1,"width":10,"height":10})";
        }
        many += "]}" + tail;
        refusesJson(many, std::to_string(kMaxSlideItems),
                    "more boxes than a scene can carry are refused naming the limit");

        // Exactly at the limit loads — the refusal above is about the ceiling,
        // not about the feature.
        std::string exact = head + R"("items":[)";
        for (size_t i = 0; i < kMaxSlideItems; ++i) {
            if (i > 0) {
                exact += ",";
            }
            exact += R"({"id":"text)" + std::to_string(i + 1) +
                     R"(","lines":[],"x":1,"y":1,"width":10,"height":10})";
        }
        exact += "]}" + tail;
        const fs::path exactDir = tempRoot / "v2-exact";
        std::filesystem::create_directories(exactDir);
        writeFile(exactDir / "service.json", exact);
        Documents atLimit;
        error.clear();
        check(openshow::domain::loadService(&atLimit, exactDir, &error) &&
                  atLimit.findShow("show1")->slides[0].items.size() == kMaxSlideItems,
              "a slide at exactly the limit loads");
    }

    // --- format 3: kinds and asset references round-trip, and a file that
    //     lies about either is refused naming the lie (task 6.17b) ---
    {
        Documents mixed;
        const Id show = mixed.createShow("Stacked", &error);
        const Id slide = mixed.appendSlide(show, &error);
        const fs::path plateSource = tempRoot / "plate.png";
        writeFile(plateSource, "PLATEBYTES");
        const Id plate = mixed.addAsset("plate.png", plateSource.string(), &error);
        check(!plate.empty(), "the mixed fixture has an asset to name");
        check(mixed.setItemLines(show, slide,
                                 mixed.findShow(show)->slides[0].items[0].id, {"under"}),
              "the fixture's text box is written first");
        SlideItem picture;
        picture.kind = ItemKind::Image;
        picture.assetId = plate;
        picture.x = 1000.0;
        picture.y = 80.0;
        picture.width = 700.0;
        picture.height = 400.0;
        const Id pictureId = mixed.addItem(show, slide, picture, &error);
        SlideItem caption;
        caption.lines = {"over"};
        const Id captionId = mixed.addItem(show, slide, caption, &error);

        const fs::path stacked = tempRoot / "v3-stacked";
        Documents back;
        error.clear();
        check(openshow::domain::saveService(mixed, stacked, &error) &&
                  openshow::domain::loadService(&back, stacked, &error),
              "a slide of text, picture, text saves and reloads");
        const Slide& reloaded = back.findShow(show)->slides[0];
        check(reloaded.items.size() == 3 && reloaded.items[0].id != pictureId &&
                  reloaded.items[0].kind == ItemKind::Text &&
                  reloaded.items[0].lines == std::vector<std::string>({"under"}),
              "the bottom text item keeps its words and its place");
        check(reloaded.items[1].id == pictureId &&
                  reloaded.items[1].kind == ItemKind::Image &&
                  reloaded.items[1].assetId == plate && reloaded.items[1].lines.empty() &&
                  reloaded.items[1].x == 1000.0 && reloaded.items[1].height == 400.0,
              "the picture comes back as a picture: its id, its asset, its box, and no words");
        check(reloaded.items[2].id == captionId &&
                  reloaded.items[2].kind == ItemKind::Text &&
                  reloaded.items[2].lines == std::vector<std::string>({"over"}),
              "and the top text item is where the operator left it");
        check(sameService(mixed, back), "the whole mixed service compares field by field");
        const std::string manifest = readFile(stacked / "service.json");
        check(manifest.find("\"kind\":\"text\"") != std::string::npos,
              "a TEXT item's kind is written too: leaving it to inference is how a "
              "future default would redefine what an old item drew");
        check(manifest.find("\"kind\":\"image\"") != std::string::npos &&
                  manifest.find("\"assetId\":\"" + plate + "\"") != std::string::npos,
              "the manifest names the item's kind and the asset it shows");
        size_t linesKeys = 0;
        for (size_t at = manifest.find("\"lines\":"); at != std::string::npos;
             at = manifest.find("\"lines\":", at + 1)) {
            ++linesKeys;
        }
        check(linesKeys == 2,
              "exactly two lines lists in the manifest — one per text item: a picture's "
              "words are not stored as an empty list the reader would have to forgive");
        const fs::path restacked = tempRoot / "v3-restacked";
        check(openshow::domain::saveService(back, restacked, &error) &&
                  readFile(restacked / "service.json") == manifest,
              "re-saving a mixed service writes a byte-identical manifest");

        // And the mixed slide is editable after the reload, by the ids the
        // file carried: the picture is still a picture the operator can move.
        check(back.setItemGeometry(show, slide, pictureId, 10.0, 10.0, 100.0, 100.0) &&
                  !back.setItemLines(show, slide, pictureId, {"nope"}, &error) &&
                  error.find("image") != std::string::npos,
              "a reloaded picture moves, and still refuses words");

        // --- the lies a hand-edited format-3 file can tell ---
        const std::string head3 = R"({"format":3,"nextOrdinal":99,"shows":[{"id":"show1",)"
                                  R"("name":"Boxed","slides":[{"id":"slide1",)";
        const std::string tail3 = R"(]}],"styles":[],"assets":[],"playlists":[]})";
        const std::string goodText =
            R"({"id":"text1","kind":"text","lines":["a"],"x":1,"y":1,"width":10,"height":10})";
        const std::string goodImage =
            R"({"id":"image2","kind":"image","assetId":"asset1","x":1,"y":1,"width":10,)"
            R"("height":10})";
        refusesJson(head3 + R"("items":[{"id":"image2","kind":"video","assetId":"asset1",)"
                        R"("x":1,"y":1,"width":10,"height":10}]})" + tail3,
                    "neither 'text' nor 'image'",
                    "an item whose kind this build does not draw is refused naming it");
        refusesJson(head3 +
                        R"("items":[{"id":"text1","lines":["a"],"x":1,"y":1,"width":10,)"
                        R"("height":10}]})" + tail3,
                    "'kind' must be a string",
                    "a format-3 item with no kind of its own is refused");
        refusesJson(head3 + R"("items":[)" + goodText + "," + goodImage + "]}" + tail3,
                    "names unknown asset",
                    "an image item naming an asset the service does not hold is refused "
                    "naming the slide, the item and the id");
        refusesJson(head3 +
                        R"("items":[{"id":"image2","kind":"image","assetId":"asset1",)"
                        R"("lines":["a"],"x":1,"y":1,"width":10,"height":10}]})" + tail3,
                    "words belong to a text item",
                    "an image item carrying lines is refused, not read as a half-item");
        refusesJson(head3 +
                        R"("items":[{"id":"text1","kind":"text","assetId":"asset1",)"
                        R"("lines":["a"],"x":1,"y":1,"width":10,"height":10}]})" + tail3,
                    "only images do",
                    "a text item naming an asset is refused");
        refusesJson(head3 +
                        R"("items":[{"id":"image2","kind":"image","assetId":"",)"
                        R"("x":1,"y":1,"width":10,"height":10}]})" + tail3,
                    "names no asset",
                    "an image item with an empty asset reference is refused");

        // A "kind" in a FORMAT-2 file: the key did not exist there, so a file
        // carrying one was not written by this build, and reading it as a text
        // item would guess at what a newer build meant by it.
        const std::string head2 = R"({"format":2,"nextOrdinal":99,"shows":[{"id":"show1",)"
                                  R"("name":"Boxed","slides":[{"id":"slide1",)";
        refusesJson(head2 + R"("items":[)" + goodText + "]}" + tail3,
                    "format-2 file",
                    "a hand-bumped 'kind' in a format-2 file is refused rather than "
                    "read as text");

        // The honest format-3 hand-written file: the asset is in the package,
        // the item names it, and the load takes both as written — including
        // the item id, which is what lets an operator's selection survive.
        {
            const fs::path real = tempRoot / "v3-hand-written";
            std::filesystem::create_directories(real / "assets");
            const std::string packaged = "assets/" + plate + "-plate.png";
            writeFile(real / packaged, "PLATEBYTES");
            const std::string realImage =
                R"({"id":"image2","kind":"image","assetId":")" + plate +
                R"(","x":1,"y":1,"width":10,"height":10})";
            writeFile(real / "service.json",
                      std::string(R"({"format":3,"nextOrdinal":99,"shows":[{"id":"show1",)"
                      R"("name":"Boxed","slides":[{"id":"slide1","items":[)") +
                          goodText + "," + realImage +
                          R"(]}]}],"styles":[],"assets":[{"id":")" + plate +
                          R"(","name":"plate.png","file":")" + packaged +
                          R"("}],"playlists":[]})");
            Documents handWritten;
            error.clear();
            const bool handLoaded =
                openshow::domain::loadService(&handWritten, real, &error);
            if (!handLoaded) {
                std::fprintf(stderr, "   hand-written load said: %s\n", error.c_str());
            }
            check(handLoaded &&
                      handWritten.findShow("show1")->slides[0].items.size() == 2 &&
                      handWritten.findShow("show1")->slides[0].items[1].id == "image2" &&
                      handWritten.findShow("show1")->slides[0].items[1].kind ==
                          ItemKind::Image &&
                      handWritten.findShow("show1")->slides[0].items[1].assetId == plate,
                  "a hand-written format-3 file with a real asset loads, item id included");
            if (check(handWritten.findAsset(plate) != nullptr, "and its asset is there")) {
                check(readFile(handWritten.findAsset(plate)->sourcePath) == "PLATEBYTES",
                      "pointing at the packaged bytes like any other asset");
            }
        }
    }

    // --- format 4: the fit is written on EVERY slide, older files mean
    //     cover, and a file that lies about the fit is refused naming the
    //     lie (task 6.17c1) ---
    {
        Documents fitted;
        const Id show = fitted.createShow("Fitted", &error);
        const Id slideA = fitted.appendSlide(show, &error);
        const Id slideB = fitted.appendSlide(show, &error);
        check(openshow::domain::backgroundFitName(openshow::domain::BackgroundFit::Cover) ==
                      std::string("cover") &&
                  openshow::domain::backgroundFitName(openshow::domain::BackgroundFit::Contain) ==
                      std::string("contain"),
              "the domain spells the two fits, and they are the only two spellings");
        openshow::domain::BackgroundFit parsed = openshow::domain::BackgroundFit::Cover;
        check(!openshow::domain::backgroundFitFromName("blur", &parsed) &&
                  !openshow::domain::backgroundFitFromName("", &parsed),
              "names outside the pair do not parse: 'blur' arrives in 6.17c2, "
              "and until then a file claiming it must not be approximated");
        check(fitted.findShow(show)->slides[0].backgroundFit == openshow::domain::BackgroundFit::Cover,
              "a brand-new slide defaults to cover — what the worker has drawn since 3.2");
        check(fitted.setSlideBackgroundFit(show, slideA, openshow::domain::BackgroundFit::Contain),
              "a slide chooses contain — legal before any picture exists, because the "
              "choice is the slide's standing answer, not the file's property");
        check(fitted.findShow(show)->slides[1].backgroundFit == openshow::domain::BackgroundFit::Cover,
              "and the other slide is untouched");
        error.clear();
        check(!fitted.setSlideBackgroundFit(show, "no-such-slide",
                                            openshow::domain::BackgroundFit::Cover, &error) &&
                  error.find("slide") != std::string::npos,
              "a fit on an unknown slide is refused naming the id");

        const fs::path fittedDir = tempRoot / "v4-fitted";
        Documents back;
        error.clear();
        check(openshow::domain::saveService(fitted, fittedDir, &error) &&
                  openshow::domain::loadService(&back, fittedDir, &error),
              "a service of fitted slides saves and reloads");
        check(back.findShow(show)->slides[0].backgroundFit == openshow::domain::BackgroundFit::Contain &&
                  back.findShow(show)->slides[1].backgroundFit == openshow::domain::BackgroundFit::Cover,
              "the choice survives the round trip, slide by slide");
        check(sameService(fitted, back), "the whole fitted service compares field by field");
        const std::string manifest = readFile(fittedDir / "service.json");
        check(manifest.find("\"fit\":\"contain\"") != std::string::npos &&
                  manifest.find("\"fit\":\"cover\"") != std::string::npos,
              "both fits are written by name — cover included, because leaving the "
              "default to inference is 6.17b1's rule for why a key is always written");
        const fs::path refitted = tempRoot / "v4-refitted";
        check(openshow::domain::saveService(back, refitted, &error) &&
                  readFile(refitted / "service.json") == manifest,
              "re-saving a fitted service writes a byte-identical manifest");

        // The honest format-3 file: no "fit" key anywhere, and cover is not
        // an inference but that file's own history — cover is the only fit
        // the worker of that era could draw.
        {
            const fs::path legacy = tempRoot / "v3-unfit";
            std::filesystem::create_directories(legacy);
            writeFile(legacy / "service.json",
                      std::string(R"({"format":3,"nextOrdinal":99,"shows":[{"id":"show1",)"
                                  R"("name":"Unfit","slides":[{"id":"slide1","items":[)") +
                          R"({"id":"text1","kind":"text","lines":["a"],"x":1,"y":1,)"
                          R"("width":10,"height":10}]}]}],"styles":[],"assets":[],)"
                          R"("playlists":[]})");
            Documents old;
            error.clear();
            check(openshow::domain::loadService(&old, legacy, &error) &&
                      old.findShow("show1")->slides[0].backgroundFit == openshow::domain::BackgroundFit::Cover,
                  "a format-3 slide with no fit key loads as cover");
            check(openshow::domain::saveService(old, tempRoot / "v3-promoted", &error) &&
                      readFile(tempRoot / "v3-promoted" / "service.json")
                          .find("\"fit\":\"cover\"") != std::string::npos,
                  "and the re-save promotes it: the choice is now a fact in the file");
        }

        // The lies: an unknown name, a missing name in a format-4 slide, and
        // a hand-bumped "fit" in a pre-4 file where the key does not exist.
        const std::string tailFit = R"(]}],"styles":[],"assets":[],"playlists":[]})";
        const std::string head4 = R"({"format":4,"nextOrdinal":99,"shows":[{"id":"show1",)"
                                  R"("name":"Fitted","slides":[{"id":"slide1",)";
        const std::string head3fit =
            R"({"format":3,"nextOrdinal":99,"shows":[{"id":"show1",)"
            R"("name":"Fitted","slides":[{"id":"slide1",)";
        const std::string box4 =
            R"("items":[{"id":"text1","kind":"text","lines":["a"],"x":1,"y":1,)"
            R"("width":10,"height":10}],)";
        refusesJson(head4 + box4 + R"("fit":"stretch"})" + tailFit,
                    "not a fit this build can draw",
                    "a fit name this build cannot draw is refused naming itself");
        refusesJson(head4 + box4 + R"("fit":"Blur"})" + tailFit,
                    "has fit 'Blur'",
                    "and case is not forgiven: the name list is exact");
        refusesJson(head4 + box4 + R"("backgroundAssetId":""})" + tailFit,
                    "'fit' must be a string",
                    "a format-4 slide with no fit is refused: the writer always "
                    "writes it, so its absence is a hand-edit");
        refusesJson(head3fit + box4 + R"("fit":"cover"})" + tailFit,
                    "where that key",
                    "a hand-bumped 'fit' in a format-3 file is refused rather than "
                    "trusted — somebody else's build meant something by that key");
    }

    // --- a slide with no boxes at all is a slide, not a corrupt file ---
    {
        Documents drained;
        const Id show = drained.createShow("Backdrop only", &error);
        const Id slide = drained.appendSlide(show, &error);
        check(drained.removeItem(show, slide,
                                    drained.findShow(show)->slides[0].items[0].id, &error),
              "the only box on a slide can be removed");
        const fs::path package = tempRoot / "no-boxes";
        Documents back;
        check(openshow::domain::saveService(drained, package, &error) &&
                  openshow::domain::loadService(&back, package, &error) &&
                  back.findShow(show)->slides[0].items.empty(),
              "a slide with no text boxes round-trips as a slide with no text boxes");
    }
}

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
    check(docs.findShow(show)->slides[0].items[0].lines == std::vector<std::string>({"First"}),
          "the edit is live in the store");
    check(history.undoDepth() == 3, "three edits sit on the undo stack");

    check(history.undo(&error), "undo succeeds");
    check(docs.findShow(show)->slides.size() == 1 &&
              docs.findShow(show)->slides[0].items[0].lines.empty(),
          "undo reverses the lines edit");
    check(history.canRedo(), "the undone edit waits on the redo stack");
    check(history.redo(&error), "redo replays the edit");
    check(docs.findShow(show)->slides[0].items[0].lines == std::vector<std::string>({"First"}),
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

    // Task 6.17a, the two properties a stack of boxes adds to that rule:
    // draw order is document state an undo step restores, and a box whose
    // creation was undone does not come back as the identity of the next box
    // an operator adds — the selection a canvas holds is a box id, and a
    // recycled id is a selection that silently moved to different words.
    {
        Documents stacked;
        History boxHistory(&stacked);
        Id boxShow, boxSlide;
        boxHistory.apply([&] { boxShow = stacked.createShow("Stack"); return !boxShow.empty(); });
        boxHistory.apply([&] { boxSlide = stacked.appendSlide(boxShow);
                               return !boxSlide.empty(); });
        const Id bottom = stacked.findShow(boxShow)->slides[0].items[0].id;
        SlideItem shape;
        shape.lines = {"added"};
        Id added;
        check(boxHistory.apply([&] { added = stacked.addItem(boxShow, boxSlide, shape);
                                      return !added.empty(); }),
              "adding a box runs through history");
        check(boxHistory.apply([&] { return stacked.moveItem(boxShow, boxSlide, added, 0); }),
              "reordering the stack runs through history");
        check(stacked.findShow(boxShow)->slides[0].items[0].id == added &&
                  stacked.findShow(boxShow)->slides[0].items[1].id == bottom,
              "the stack reads in the moved order");
        check(boxHistory.undo(&error) &&
                  stacked.findShow(boxShow)->slides[0].items[0].id == bottom,
              "one undo puts the stack back in the order it was");
        check(boxHistory.redo(&error) &&
                  stacked.findShow(boxShow)->slides[0].items[0].id == added,
              "redo takes the new order back");
        check(boxHistory.undo(&error) && boxHistory.undo(&error) &&
                  stacked.findShow(boxShow)->slides[0].items.size() == 1,
              "undoing the add leaves the slide its one original box");
        Id next;
        check(boxHistory.apply([&] { next = stacked.addItem(boxShow, boxSlide, shape);
                                      return !next.empty(); }),
              "a new box added after the undo is accepted");
        check(next != added && next != bottom,
              "the undone box's id is not back in circulation");
    }

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

// --- task 6.12 (review R7): the save adopts, and everything follows ---------

// R7's mechanism in one sentence: a save COPIES asset bytes into the
// package but left the model pointing at wherever the file happened to
// live before — the original import, or a scratch autosave tree that the
// very same save then deletes. These fixtures run the storage-level
// sequence the UI runs, so the ownership rule is pinned without Qt.
void exerciseAdoption(const fs::path& tempRoot) {
    std::string error;
    const fs::path package = tempRoot / "adopt-service";
    const fs::path scratch = tempRoot / "adopt-autosave";
    const fs::path originals = tempRoot / "adopt-originals";
    std::filesystem::create_directories(originals);
    const fs::path still = originals / "still.jpg";
    const fs::path loop = originals / "loop.mp4";
    writeFile(still, "STILLBYTES");
    writeFile(loop, "LOOPBYTES");

    Documents docs;
    const Id a1 = docs.addAsset("still.jpg", still.string(), &error);
    const Id a2 = docs.addAsset("loop.mp4", loop.string(), &error);
    check(!a1.empty() && !a2.empty(), "adoption: two assets are recorded");
    const Id show = docs.createShow("Petition", &error);
    // A history snapshot taken BEFORE the save: it holds the assets at the
    // original paths, exactly the state an undo must not resurrect.
    History history(&docs);
    check(history.apply([&] { return docs.setShowNotes(show, "touched after import"); }),
          "adoption: an edit goes through history");

    std::vector<AssetRebind> rebinds;
    check(saveServiceAndAdopt(docs, package, &rebinds, &error),
          "adoption: the save succeeds");
    const fs::path packaged1 = (package / "assets" / (a1 + "-still.jpg")).lexically_normal();
    check(rebinds.size() == 2, "adoption: both moves are reported");
    check(rebinds[0].assetId == a1 && rebinds[0].from == still.string() &&
              rebinds[0].to == packaged1.string(),
          "adoption: the rebind names the asset, the old path and the new one");
    check(docs.findAsset(a1)->sourcePath == packaged1.string(),
          "adoption: the model now names the package's own copy");
    check(readFile(packaged1) == "STILLBYTES", "adoption: the bytes really are in the package");

    history.adoptAssets(rebinds);
    check(history.undo(&error), "adoption: the edit undoes");
    check(docs.findAsset(a1)->sourcePath == packaged1.string(),
          "adoption: undo does not put the original path back");

    // The outside files go away — the operator's cleanup, or in the defect
    // this task fixes, the scratch tree a real save deletes. Nothing may
    // care: the package owns the bytes now.
    fs::remove(still);
    fs::remove(loop);
    check(saveService(docs, package, &error),
          "adoption: a later save works with the originals deleted");
    rebinds.clear();
    check(saveServiceAndAdopt(docs, package, &rebinds, &error) && rebinds.empty(),
          "adoption: re-saving an adopted service reports no moves");
    check(docs.findAsset(a1)->sourcePath == packaged1.string() &&
              docs.findAsset(a2)->sourcePath ==
                  (package / "assets" / (a2 + "-loop.mp4")).lexically_normal().string(),
          "adoption: paths stay exactly where the first adoption put them");

    // A save that FAILS adopts nothing: the model must not point at bytes
    // the save refused to copy.
    const fs::path extra = originals / "extra.jpg";
    writeFile(extra, "EXTRA");
    Documents half;
    const Id good = half.addAsset("extra.jpg", extra.string(), &error);
    half.addAsset("gone.jpg", (originals / "gone.jpg").string(), &error);  // never written
    rebinds.clear();
    check(!saveServiceAndAdopt(half, tempRoot / "adopt-half", &rebinds, &error),
          "adoption: a save refused by a missing asset fails");
    check(rebinds.empty() && half.findAsset(good)->sourcePath == extra.string(),
          "adoption: a failed save rebinds nothing, reported or real");

    // Autosave mirrors; it does NOT adopt. (tick takes the state by const
    // reference precisely so the scratch cannot steal the model's paths.)
    Documents live;
    const Id one = live.addAsset("extra.jpg", extra.string(), &error);
    Autosave saver(scratch);
    check(saver.tick(live, &error), "adoption: the autosave tick succeeds");
    check(live.findAsset(one)->sourcePath == extra.string(),
          "adoption: the tick leaves the model pointing at the original");

    // R7's exact sequence at this level: recover from scratch (paths INTO
    // the scratch tree), save-and-adopt, delete the scratch — everything
    // downstream keeps working.
    Documents recovered;
    Autosave nextSession(scratch);
    check(nextSession.recover(&recovered, &error), "adoption: the scratch package recovers");
    const std::string scratchFile = recovered.findAsset(one)->sourcePath;
    check(scratchFile == (scratch / "assets" / (one + "-extra.jpg")).lexically_normal().string(),
          "adoption: the recovered asset names a file inside the scratch tree");
    const fs::path package3 = tempRoot / "adopt-recovered";
    rebinds.clear();
    check(saveServiceAndAdopt(recovered, package3, &rebinds, &error),
          "adoption: saving the recovered session succeeds");
    check(rebinds.size() == 1 && rebinds[0].from == scratchFile,
          "adoption: the one move reported is scratch -> package");
    check(nextSession.clear(&error), "adoption: the scratch tree is removed, as a real save does");
    fs::remove(extra);  // the last copy outside the package, gone
    rebinds.clear();
    check(saveServiceAndAdopt(recovered, package3, &rebinds, &error) && rebinds.empty(),
          "adoption: with the scratch deleted, a later save still succeeds");
    Autosave later(scratch);
    check(later.tick(recovered, &error),
          "adoption: and so does the next autosave tick - before this task it read a deleted file");
    Documents reloaded;
    check(loadService(&reloaded, package3, &error) &&
              reloaded.findAsset(one)->sourcePath ==
                  (package3 / "assets" / (one + "-extra.jpg")).lexically_normal().string(),
          "adoption: a reload names the same package file the model already names");

    // adoptAssets matches id AND path: an asset retargeted after the save
    // keeps its newer path instead of being yanked back by a stale rebind.
    Documents picky;
    const Id x = picky.addAsset("x.jpg", "/outside/x.jpg", &error);
    check(picky.adoptAssets({{"asset-nope", "/outside/x.jpg", "/pkg/x.jpg"}}) == 0,
          "adoption: a rebind for an unknown id is skipped");
    check(picky.adoptAssets({{x, "/somewhere/else.jpg", "/pkg/x.jpg"}}) == 0,
          "adoption: a rebind whose 'from' does not match is skipped");
    check(picky.adoptAssets({{x, "/outside/x.jpg", "/pkg/x.jpg"}}) == 1 &&
              picky.findAsset(x)->sourcePath == "/pkg/x.jpg",
          "adoption: id and path match, the asset is retargeted");
}

// --- task 6.13a (review R8): a failed commit buries nothing and smears nothing -

// Every step of the save's commit, run as an injected refusal AGAINST a
// valid existing package: the previous service.json must survive
// byte-identical (dir-fsync excepted, the documented case where the new
// manifest is already installed and consistent), the packaged asset file
// the old manifest names must never hold a torn mix of versions, the
// package must stay loadable, and the failed save must leave no staging
// or temp debris. R8's mechanism was copy_file(overwrite_existing)
// smearing partial bytes over the file the still-valid manifest named —
// in the round that faults mid-copy, THIS check is the defect detector.
void exerciseCommitAtomicity(const fs::path& tempRoot) {
    std::string error;
    const fs::path package = tempRoot / "commit";
    const fs::path originals = tempRoot / "commit-originals";
    fs::create_directories(originals);
    const fs::path alpha = originals / "alpha.jpg";
    const fs::path beta = originals / "beta.png";
    const std::string alphaOld(300 * 1024, 'A');
    const std::string alphaNew(500 * 1024, 'Z');
    writeFile(alpha, alphaOld);
    writeFile(beta, "DEADBEEFBYTES");

    Documents docs;
    const Id show = docs.createShow("Commit", &error);
    const Id a = docs.addAsset("alpha.jpg", alpha.string(), &error);
    const Id b = docs.addAsset("beta.png", beta.string(), &error);
    check(saveService(docs, package, &error), "commit: the first package is saved");
    const fs::path manifest = package / "service.json";
    const fs::path alphaFile = package / "assets" / (a + "-alpha.jpg");
    const std::string goldenManifest = readFile(manifest);
    check(readFile(alphaFile) == alphaOld,
          "commit: the packaged alpha holds the whole original file");
    check(fs::is_regular_file(package / "assets" / (b + "-beta.png")),
          "commit: the second asset landed too");

    // The asset's bytes CHANGE, and so does the model. Every faulted save
    // below must leave the packaged copy — named by the manifest that is
    // still committed — as a whole version, never half of each, and the
    // committed manifest must still be the golden one: these saves have
    // work to commit, they are not no-ops.
    writeFile(alpha, alphaNew);
    check(docs.setShowNotes(show, "second version, waiting to be committed", &error),
          "commit: the model has a pending change to commit");

    // dir-fsync is LAST on purpose: it is the one failure the commit can
    // only report from after installing the new manifest, and the rounds
    // before it all compare against the golden manifest.
    for (const char* step : {"asset-copy", "asset-fsync", "asset-rename", "assets-fsync",
                             "manifest-write", "manifest-fsync", "manifest-close",
                             "manifest-rename", "dir-fsync"}) {
        ::setenv("OPENSHOW_STORAGE_FAULT", step, 1);
        error.clear();
        const bool refused = !saveService(docs, package, &error);
        ::unsetenv("OPENSHOW_STORAGE_FAULT");
        check(refused, (std::string("commit: the save refuses the injected fault at '") + step +
                        "'")
                           .c_str());
        check(error.find(step) != std::string::npos,
              (std::string("commit: the failure message names the step '") + step + "'")
                  .c_str());

        if (std::string_view(step) == "dir-fsync") {
            check(readFile(manifest) != goldenManifest,
                  "commit: the final-sync failure reports from AFTER the install - the new "
                  "manifest is in place, exactly as the header documents");
        } else {
            check(readFile(manifest) == goldenManifest,
                  (std::string("commit: the fault at '") + step +
                   "' left the previous service.json byte-identical")
                      .c_str());
        }
        const std::string packagedAlpha = readFile(alphaFile);
        const bool whole = packagedAlpha == alphaOld || packagedAlpha == alphaNew;
        check(whole,
              (std::string("commit: the packaged alpha is a WHOLE version after the fault at '") +
               step + "', never a torn mix - R8's smeared overwrite detector")
                  .c_str());
        if (std::string_view(step) == "asset-copy") {
            check(packagedAlpha == alphaOld,
                  "commit: a fault INSIDE the copy never touched the destination - the old "
                  "manifest's file is byte-for-byte what it was");
        }
        Documents reloaded;
        check(loadService(&reloaded, package, &error),
              (std::string("commit: after the fault at '") + step +
               "' the package still loads")
                  .c_str());
        check(!reloaded.findAsset(a)->sourcePath.empty() &&
                  readFile(reloaded.findAsset(a)->sourcePath) == packagedAlpha,
              "commit: and its asset reads back the whole file the manifest names");
        bool debris = false;
        for (fs::recursive_directory_iterator it(package), end; it != end; ++it) {
            if (it->is_regular_file()) {
                const std::string name = it->path().filename().string();
                if (name.find(".openshow-staging-") != std::string::npos ||
                    name == "service.json.tmp") {
                    debris = true;
                }
            }
        }
        check(!debris,
              (std::string("commit: the failed save swept away its staging and temp files '") +
               step + "'")
                  .c_str());
    }

    // With the faults cleared, the real commit lands in full.
    check(saveService(docs, package, &error), "commit: the clean retry after the faults succeeds");
    check(readFile(manifest) != goldenManifest && readFile(alphaFile) == alphaNew,
          "commit: the new manifest AND the changed asset's new version are both installed");
    Documents committed;
    check(loadService(&committed, package, &error), "commit: the committed package loads");
    const std::string secondManifest = readFile(manifest);
    check(saveService(docs, package, &error) && readFile(manifest) == secondManifest,
          "commit: re-saving the committed state is byte-identical - staging names never "
          "leak into the manifest");

    // The autosave rides the same commit, so an injected refusal reaches
    // tick()'s error too - and a refused tick claims no baseline.
    const fs::path scratch = tempRoot / "commit-autosave";
    Autosave guarded(scratch);
    ::setenv("OPENSHOW_STORAGE_FAULT", "asset-copy", 1);
    error.clear();
    check(!guarded.tick(docs, &error) && error.find("asset-copy") != std::string::npos,
          "commit: an autosave tick refused mid-copy reports the step");
    ::unsetenv("OPENSHOW_STORAGE_FAULT");
    check(!Autosave::exists(scratch),
          "commit: the refused tick left no manifest behind - no half-written recovery bait");
    check(guarded.tick(docs, &error) && Autosave::exists(scratch),
          "commit: the retry after the cleared fault writes the scratch for real");
    Documents recovered;
    check(guarded.recover(&recovered, &error),
          "commit: and that scratch recovers - the commit kept its word end to end");
}

// --- task 6.13b (review R8): a package cannot name the world ----------------

// Loaded ids are NAMES, never paths (an asset's id prefixes the filename
// its bytes are written under), and an asset's recorded file must live
// inside the package where it really is - every symlink followed. A
// service package can arrive from an archive, a USB stick or a shared
// folder, so containment is about bytes on disk, not about what the
// manifest's strings look like. Every refusal leaves the target
// Documents untouched. Authorised import at save stays external-by-
// design: that is the operator's choice, not a manifest's.
void exercisePackageContainment(const fs::path& tempRoot) {
    std::string error;
    fs::create_directories(tempRoot);
    const fs::path originals = tempRoot / "originals";
    fs::create_directories(originals);
    writeFile(originals / "ok.jpg", "OKBYTES");

    Documents docs;
    const Id show = docs.createShow("Contained", &error);
    const Id asset = docs.addAsset("ok.jpg", (originals / "ok.jpg").string(), &error);
    const fs::path package = tempRoot / "service";
    check(saveService(docs, package, &error), "containment: the honest package saves");
    const std::string packaged = "assets/" + asset + "-ok.jpg";
    check(readFile(package / "service.json").find("\"file\":\"" + packaged + "\"") !=
              std::string::npos,
          "containment: the manifest records the package-relative file");

    int variant = 0;
    const auto copyBytes = [&](const fs::path& dir) {
        fs::create_directories(dir / "assets");
        fs::copy_file(package / "assets" / (asset + "-ok.jpg"),
                      dir / "assets" / (asset + "-ok.jpg"), fs::copy_options::overwrite_existing);
    };
    // Same package, but the asset's `file` field now says `value`.
    const auto fileRefused = [&](const std::string& value, const char* expectIn,
                                 const std::function<void(const fs::path&)>& setup,
                                 const char* what) {
        const fs::path dir = tempRoot / ("file-" + std::to_string(++variant));
        std::string manifest = readFile(package / "service.json");
        if (!replaceOnce(manifest, "\"file\":\"" + packaged + "\"",
                         "\"file\":\"" + value + "\"")) {
            check(false, "containment: the file-field tamper found its anchor");
            return;
        }
        copyBytes(dir);
        writeFile(dir / "service.json", manifest);
        if (setup) {
            setup(dir);
        }
        error.clear();
        Documents attempt;
        attempt.createShow("Keeper");
        check(!loadService(&attempt, dir, &error) &&
                  error.find(expectIn) != std::string::npos && attempt.showIds().size() == 1 &&
                  attempt.findShow(attempt.showIds()[0])->name == "Keeper",
              what);
    };

    // Refusals by name, before any filesystem is consulted.
    fileRefused("/etc/hostname", "absolute", nullptr,
                "an absolute file field is refused even though the file exists");
    fileRefused("assets/../../escaped.jpg", "climbs out", nullptr,
                "a file field that climbs out of the package is refused");

    // Refusals by where the name REALLY leads.
    fileRefused("assets/alias.jpg", "resolves outside",
                [&](const fs::path& dir) {
                    fs::create_symlink("../../originals/ok.jpg", dir / "assets/alias.jpg");
                },
                "a symlink that leaves the package is an escape its name cannot hide");
    fs::path siblingEvil;
    fileRefused("assets/sib.jpg", "resolves outside",
                [&](const fs::path& dir) {
                    siblingEvil = dir.parent_path() / (dir.filename().string() + "-evil");
                    fs::create_directories(siblingEvil);
                    writeFile(siblingEvil / "secret.txt", "SIBLINGSECRET");
                    // From dir/assets/, two levels up is tempRoot again.
                    fs::create_symlink("../../" + dir.filename().string() + "-evil/secret.txt",
                                       dir / "assets/sib.jpg");
                },
                "the sibling directory whose NAME starts with the package's is not inside it");
    check(siblingEvil.empty() || readFile(siblingEvil / "secret.txt") == "SIBLINGSECRET",
          "containment: the escape attempts touched nothing outside their packages");

    // A symlink that stays inside the package is contained, and the bytes
    // still arrive - the rule is containment, not link paranoia.
    {
        const fs::path dir = tempRoot / ("file-" + std::to_string(++variant));
        std::string manifest = readFile(package / "service.json");
        check(replaceOnce(manifest, "\"file\":\"" + packaged + "\"",
                          "\"file\":\"assets/alias.jpg\""),
              "containment: the inside-symlink anchor");
        copyBytes(dir);
        writeFile(dir / "service.json", manifest);
        fs::create_symlink(asset + "-ok.jpg", dir / "assets/alias.jpg");
        Documents attempt;
        check(loadService(&attempt, dir, &error) &&
                  readFile(attempt.findAsset(asset)->sourcePath) == "OKBYTES",
              "containment: a symlink that stays inside the package loads fine");
    }

    // Unsafe ids - the tamper that used to survive into assetFileName()
    // and name a file OUTSIDE the package on the next save.
    const auto idRefused = [&](const std::string& newId, const char* what) {
        const fs::path dir = tempRoot / ("id-" + std::to_string(++variant));
        std::string manifest = readFile(package / "service.json");
        if (!replaceOnce(manifest, "\"id\":\"" + asset + "\"", "\"id\":\"" + newId + "\"")) {
            check(false, "containment: the id tamper found its anchor");
            return;
        }
        copyBytes(dir);
        writeFile(dir / "service.json", manifest);
        error.clear();
        Documents attempt;
        attempt.createShow("Keeper");
        check(!loadService(&attempt, dir, &error) &&
                  error.find("unsafe id") != std::string::npos && attempt.showIds().size() == 1,
              what);
    };
    idRefused("../../evil", "an asset id that names a path is refused");
    idRefused("sneaky name", "an id with a space is refused");
    idRefused("x.png", "an id with a dot is refused - ids are opaque, not filenames");
    {
        const fs::path dir = tempRoot / ("id-show-" + std::to_string(++variant));
        std::string manifest = readFile(package / "service.json");
        check(replaceOnce(manifest, "\"id\":\"" + show + "\"", "\"id\":\"..\\\\evil\""),
              "containment: the show-id anchor");
        copyBytes(dir);
        writeFile(dir / "service.json", manifest);
        error.clear();
        Documents attempt;
        attempt.createShow("Keeper");
        check(!loadService(&attempt, dir, &error) &&
                  error.find("unsafe id") != std::string::npos && attempt.showIds().size() == 1,
              "an unsafe SHOW id is refused too - the rule covers every id space");
    }
    {
        // Charset-legal ids still load, references intact. (A numeric
        // suffix would collide with the id-counter check — this fixture's
        // manifest has only two ids issued — so the legal id here is one
        // without a numeric tail, which is also legal.)
        const fs::path dir = tempRoot / ("id-ok-" + std::to_string(++variant));
        std::string manifest = readFile(package / "service.json");
        check(replaceOnce(manifest, "\"id\":\"" + asset + "\"", "\"id\":\"renamed-ok\""),
              "containment: the rename anchor");
        copyBytes(dir);
        writeFile(dir / "service.json", manifest);
        Documents attempt;
        check(loadService(&attempt, dir, &error) && attempt.findAsset("renamed-ok") != nullptr &&
                  readFile(attempt.findAsset("renamed-ok")->sourcePath) == "OKBYTES",
              "containment: an opaque, charset-legal id loads and keeps its file");
    }

    // The authorised side of the rule: imports still copy external bytes.
    const fs::path fresh = tempRoot / "import-still-works";
    check(saveService(docs, fresh, &error),
          "containment: saving an asset whose file lives outside still copies it in");
    Documents back;
    check(loadService(&back, fresh, &error) &&
              readFile(back.findAsset(asset)->sourcePath) == "OKBYTES",
          "containment: and the imported service round-trips");

    // A hostile-looking NAME is already defused at the filename (dots and
    // all, it stays one component); it must land inside the package.
    Documents wild;
    const Id sneaky = wild.addAsset("../Escape.png", (originals / "ok.jpg").string(), &error);
    const fs::path wildDir = tempRoot / "wild";
    check(saveService(wild, wildDir, &error), "containment: an unruly asset name saves");
    Documents wildBack;
    check(loadService(&wildBack, wildDir, &error) &&
              readFile(wildBack.findAsset(sneaky)->sourcePath) == "OKBYTES",
          "containment: the unruly name became a safe file inside the package");
    bool climbs = false;
    for (const std::filesystem::path& part :
         fs::relative(wildBack.findAsset(sneaky)->sourcePath, wildDir)) {
        if (part == "..") {
            climbs = true;
        }
    }
    check(!climbs, "containment: no recorded path is a climb");
}

// --- task 6.14b (review R14): a tick must not re-copy unchanged media ----------

// R14 measured that every autosave tick re-saved the WHOLE package — a
// text edit after a video import copied the video again, on the UI
// thread, every time. Asset copy reuse (storage.h) says a tick may skip
// a copy only where both sides still identify: the source is the file
// (canonical path, size, mtime) whose bytes were last installed, AND the
// destination still exists. The fixtures arm the 6.13a `asset-copy`
// refusal to make the skip VISIBLE: a text-only tick must succeed with
// copies refused (the step never ran), and any rule the reuse breaks is
// caught by the refusal biting where it must.
void exerciseAssetReuse(const fs::path& tempRoot) {
    std::string error;
    fs::create_directories(tempRoot);
    const fs::path originals = tempRoot / "originals";
    fs::create_directories(originals);
    writeFile(originals / "video.mp4", "VIDEO-VERSION-ONE!!");

    Documents docs;
    const Id show = docs.createShow("Reused", &error);
    const Id asset = docs.addAsset("video.mp4", (originals / "video.mp4").string(), &error);
    const fs::path scratch = tempRoot / "autosave";
    Autosave autosave(scratch);
    check(autosave.tick(docs, &error), "reuse: the first tick copies the asset");
    const fs::path packaged = scratch / "assets" / (asset + "-video.mp4");
    check(readFile(packaged) == "VIDEO-VERSION-ONE!!", "reuse: the packaged asset holds v1");

    // (1) Only the text changed: the copy step must not run at all, so a
    // storage that refuses asset copies cannot stop this tick.
    ::setenv("OPENSHOW_STORAGE_FAULT", "asset-copy", 1);
    check(docs.setShowNotes(show, "second note", &error), "reuse: the text changed");
    check(autosave.tick(docs, &error),
          "reuse: a text-only tick succeeds with asset copies REFUSED - the step never ran");

    // (2) The media changed: the copy runs again, and the refusal bites.
    writeFile(originals / "video.mp4", "VIDEO-VERSION-TWO!");
    check(docs.setShowNotes(show, "third note", &error), "reuse: text again");
    check(!autosave.tick(docs, &error) && error.find("asset-copy") != std::string::npos,
          "reuse: changed media re-enters the copy step where it can be refused");
    ::unsetenv("OPENSHOW_STORAGE_FAULT");
    check(autosave.tick(docs, &error), "reuse: the healthy retry installs v2");
    check(readFile(packaged) == "VIDEO-VERSION-TWO!", "reuse: the packaged asset holds v2");

    // (3) Same SIZE, new mtime: stat identity still sees a different file.
    // The replacement MUST keep the exact byte length — an earlier draft
    // was one byte longer, so a reuse rule that ignored mtime still saw
    // a different size and this check passed vacuously (m02 escaped).
    // Equal size is what isolates the mtime leg of the stamp.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    writeFile(originals / "video.mp4", "VIDEO-VERSION-TRE!");  // same length, new mtime
    check(docs.setShowNotes(show, "fourth note", &error), "reuse: text once more");
    ::setenv("OPENSHOW_STORAGE_FAULT", "asset-copy", 1);
    check(!autosave.tick(docs, &error) && error.find("asset-copy") != std::string::npos,
          "reuse: a same-size replacement is NOT the same bytes - it re-copies");
    ::unsetenv("OPENSHOW_STORAGE_FAULT");
    check(autosave.tick(docs, &error) && readFile(packaged) == "VIDEO-VERSION-TRE!",
          "reuse: and the replacement lands");

    // (4) The destination vanishes behind the ledger's back: identity of
    // the SOURCE alone must not authorise a skip - the scratch has to
    // still hold the bytes.
    fs::remove(packaged);
    ::setenv("OPENSHOW_STORAGE_FAULT", "asset-copy", 1);
    check(docs.setShowNotes(show, "fifth note", &error), "reuse: text after the deletion");
    check(!autosave.tick(docs, &error) && error.find("asset-copy") != std::string::npos,
          "reuse: a missing destination forces the copy even for an unchanged source");
    ::unsetenv("OPENSHOW_STORAGE_FAULT");
    check(autosave.tick(docs, &error) && readFile(packaged) == "VIDEO-VERSION-TRE!",
          "reuse: the deleted asset is rebuilt whole");

    // (5) clear() forgets the ledger - a rebuilt scratch is written whole,
    // not assembled from promises about deleted files. (The rebuild runs
    // on `docs`, whose assets still name the external original; a
    // recovered document would name files clear() just deleted.)
    check(autosave.clear(&error), "reuse: clear removes the scratch");
    ::setenv("OPENSHOW_STORAGE_FAULT", "asset-copy", 1);
    check(!autosave.tick(docs, &error) && error.find("asset-copy") != std::string::npos,
          "reuse: after clear the next tick must copy again - the ledger died with the package");
    ::unsetenv("OPENSHOW_STORAGE_FAULT");
    check(autosave.tick(docs, &error) && readFile(packaged) == "VIDEO-VERSION-TRE!",
          "reuse: and the rebuilt scratch is whole");

    // (6) Recovered assets already live INSIDE the scratch tree: the next
    // text edit ticks with copies refused - nothing outside has to move.
    Documents recovered;
    check(autosave.recover(&recovered, &error), "reuse: recovery reads the scratch back");
    ::setenv("OPENSHOW_STORAGE_FAULT", "asset-copy", 1);
    check(recovered.setShowNotes(show, "sixth note", &error), "reuse: edit the recovery");
    check(autosave.tick(recovered, &error),
          "reuse: a recovered asset that already lives in the package needs no copy");
    ::unsetenv("OPENSHOW_STORAGE_FAULT");
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
    exerciseGeometry();
    exerciseTextBoxes();
    exerciseImageItems();
    exerciseGroups();
    exerciseStyles();
    exercisePlaylists();
    exerciseFailureLeavesNoTrace();

    // Persistence works in a private temp directory, removed at exit.
    const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() /
                                           ("openshow-domain-tests-" + std::to_string(getpid()));
    std::filesystem::remove_all(tempRoot);
    exercisePersistence(tempRoot);
    exerciseBoxStorage(tempRoot);
    std::filesystem::remove_all(tempRoot);

    exerciseHistory();

    const std::filesystem::path commitRoot = std::filesystem::temp_directory_path() /
                                             ("openshow-domain-commit-" +
                                              std::to_string(getpid()));
    std::filesystem::remove_all(commitRoot);
    exerciseCommitAtomicity(commitRoot);
    std::filesystem::remove_all(commitRoot);

    const std::filesystem::path containRoot = std::filesystem::temp_directory_path() /
                                              ("openshow-domain-contain-" +
                                               std::to_string(getpid()));
    std::filesystem::remove_all(containRoot);
    exercisePackageContainment(containRoot);
    std::filesystem::remove_all(containRoot);

    const std::filesystem::path reuseRoot = std::filesystem::temp_directory_path() /
                                            ("openshow-domain-reuse-" + std::to_string(getpid()));
    std::filesystem::remove_all(reuseRoot);
    exerciseAssetReuse(reuseRoot);
    std::filesystem::remove_all(reuseRoot);

    const std::filesystem::path autosaveRoot = std::filesystem::temp_directory_path() /
                                              ("openshow-domain-autosave-" + std::to_string(getpid()));
    std::filesystem::remove_all(autosaveRoot);
    exerciseAutosave(autosaveRoot);
    std::filesystem::remove_all(autosaveRoot);

    const std::filesystem::path adoptionRoot = std::filesystem::temp_directory_path() /
                                               ("openshow-domain-adoption-" +
                                                std::to_string(getpid()));
    std::filesystem::remove_all(adoptionRoot);
    exerciseAdoption(adoptionRoot);
    std::filesystem::remove_all(adoptionRoot);

    if (!ok || gFailed) {
        return 1;
    }
    std::printf("ok: applicationVersion \"%s\"; document model exercised\n",
                std::string(version).c_str());
    return 0;
}
