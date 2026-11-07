/*
 * Checks the shared version string, then the document model (task 2.1):
 * shows, slides, groups, themes and playlists — every operation with its
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

#include <boost/json.hpp>

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
using openshow::domain::ScriptureProvenance;
using openshow::domain::SongProvenance;
using Asset = openshow::domain::Asset;
using Autosave = openshow::domain::Autosave;
using History = openshow::domain::History;
using Playlist = openshow::domain::Playlist;
using Show = openshow::domain::Show;
using Slide = openshow::domain::Slide;
using Theme = openshow::domain::Theme;
using BoxRect = openshow::domain::BoxRect;
using SlideCustomization = openshow::domain::SlideCustomization;
using ResolvedAppearance = openshow::domain::ResolvedAppearance;
using SlideItem = openshow::domain::SlideItem;
using openshow::domain::assetPathLooksLikeVideo;
using openshow::domain::assetKindName;
using openshow::domain::assetPathExtension;
using openshow::domain::assetPathKind;
using openshow::domain::AssetKind;
using openshow::domain::kAudioExtensions;
using openshow::domain::kPictureExtensions;
using openshow::domain::kVideoExtensions;
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
    // Task UI.4a turned that predicate into one half of a kind, because the
    // media bin labels and filters its rows with the same table. So the whole
    // table is pinned here: what a name is, what that is called on the wire,
    // and that the two video questions still agree.
    check(assetPathKind("/media/loop.MP4") == AssetKind::Video
              && assetPathKind("/media/a.PNG") == AssetKind::Picture
              && assetPathKind("/media/hymn.mp3") == AssetKind::Audio,
          "assetPathKind reads a name's kind, case-insensitively");
    check(assetPathKind("/media/notes.txt") == AssetKind::Other
              && assetPathKind("/media/soundtrack") == AssetKind::Other
              && assetPathExtension("/media/archive.tar.GZ") == "gz"
              && assetPathExtension("/media/noext") == "",
          "a name nothing claims is Other, never a guessed Picture");
    check(std::string(assetKindName(AssetKind::Picture)) == "picture"
              && std::string(assetKindName(AssetKind::Video)) == "video"
              && std::string(assetKindName(AssetKind::Audio)) == "audio"
              && std::string(assetKindName(AssetKind::Other)) == "other",
          "a kind names itself with one lower-case word");
    {
        // Every extension the import dialog offers must land in its own kind,
        // and the video half must agree with the predicate the model and the
        // scene still ask — one table, three callers, no room for drift.
        auto allAre = [](const char* const* names, AssetKind kind) {
            for (; *names != nullptr; ++names) {
                const std::string path = std::string("/media/x.") + *names;
                if (assetPathKind(path) != kind
                    || assetPathLooksLikeVideo(path) != (kind == AssetKind::Video)) {
                    return false;
                }
            }
            return true;
        };
        check(allAre(kVideoExtensions, AssetKind::Video)
                  && allAre(kPictureExtensions, AssetKind::Picture)
                  && allAre(kAudioExtensions, AssetKind::Audio),
              "every offered extension lands in the kind the bin labels it");
        auto pairwiseDistinct = [](const char* const* left, const char* const* right) {
            for (; *left != nullptr; ++left) {
                for (const char* const* other = right; *other != nullptr; ++other) {
                    if (std::string(*left) == *other) {
                        return false;
                    }
                }
            }
            return true;
        };
        check(pairwiseDistinct(kVideoExtensions, kPictureExtensions)
                  && pairwiseDistinct(kVideoExtensions, kAudioExtensions)
                  && pairwiseDistinct(kPictureExtensions, kAudioExtensions),
              "no extension belongs to two kinds");
    }

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

    const Id theme = docs.createTheme("House", "#ffffff", "#202040", "", "DejaVu Serif", 64.0,
                                      "center", kDefaultLineSpacing, &error);
    check(!theme.empty(), "createStyle returns an id");
    check(docs.createTheme("Bad", "#12g456", "#000000", "", "x", 10, "center",
                           kDefaultLineSpacing, &error)
                  .empty() &&
              !error.empty(),
          "a non-hex text colour is refused");
    check(docs.createTheme("Bad", "#fff", "white", "", "x", 10, "center", kDefaultLineSpacing,
                           &error)
                  .empty(),
          "a named background colour is refused too");
    check(docs.createTheme("Bad", "#fff", "#fff", "", "x", -1, "center", kDefaultLineSpacing,
                           &error)
                  .empty(),
          "a negative font size is refused");

    check(docs.setShowDefaultTheme(show, theme, &error) &&
              docs.themeForSlide(show, slide) != nullptr &&
              docs.themeForSlide(show, slide)->id == theme,
          "the show default style resolves for unstyled slides");
    const Id other = docs.createTheme("Loud", "#ffff00", "#000000", "", "Impact", 96.0, "center",
                                      kDefaultLineSpacing, &error);
    check(docs.setSlideTheme(show, slide, other, &error) &&
              docs.themeForSlide(show, slide)->id == other,
          "a per-slide override beats the default");
    check(docs.setSlideTheme(show, slide, "", &error) &&
              docs.themeForSlide(show, slide)->id == theme,
          "clearing the override falls back to the default");
    check(!docs.setSlideTheme(show, slide, "style999", &error),
          "an unknown override id is refused");

    check(docs.updateTheme(other, "#ff0000", "#000", "", "Arial", 0.0, "left", 2.0, &error) &&
              docs.findTheme(other)->textColor == "#ff0000" &&
              docs.findTheme(other)->backgroundColor.empty() &&
              docs.findTheme(other)->fontSize == 0.0 &&
              docs.findTheme(other)->align == "left" &&
              docs.findTheme(other)->lineSpacing == 2.0,
          "updateStyle replaces the appearance and typography (0 size = renderer default)");
    check(!docs.updateTheme(other, "#ff0000", "#000", "", "Arial", 0.0, "middle",
                            kDefaultLineSpacing, &error),
          "an alignment outside left/center/right is refused");
    check(!docs.updateTheme(other, "#ff0000", "#000", "", "Arial", 0.0, "center", 0.0, &error) &&
              !docs.updateTheme(other, "#ff0000", "#000", "", "Arial", 0.0, "center", 99.0, &error),
          "line spacing outside 0.25..8 is refused");
    check(docs.renameTheme(other, "Louder", &error) && docs.findTheme(other)->name == "Louder",
          "styles rename");

    // The gradient's second colour (task 6.17c3): "" is a real answer —
    // flat, what every theme before the field was — any colour is legal,
    // and a non-colour is refused naming the ROLE, not just "colour", so
    // an operator staring at the text field knows which one to fix.
    const Id sunset = docs.createTheme("Sunset", "#ffffff", "#301020", "#c05020",
                                       "DejaVu Serif", 64.0, "center", kDefaultLineSpacing,
                                       &error);
    check(!sunset.empty() && docs.findTheme(sunset)->backgroundGradient.empty(),
          "legacy background parameters do not become theme fields");
    check(docs.findTheme(theme)->backgroundGradient.empty(),
          "a style that names none stays flat");
    error.clear();
    check(docs.createTheme("Bad", "#fff", "#000", "dusk", "x", 10, "center",
                           kDefaultLineSpacing, &error)
                  .empty() &&
              error.find("background gradient") != std::string::npos,
          "a named gradient colour is refused naming its role");
    check(docs.updateTheme(sunset, "#ffffff", "#301020", "#20c050", "DejaVu Serif", 64.0,
                           "center", kDefaultLineSpacing, &error) &&
              docs.findTheme(sunset)->backgroundGradient.empty(),
          "theme edits cannot add a gradient");
    check(docs.updateTheme(sunset, "#ffffff", "#301020", "", "DejaVu Serif", 64.0, "center",
                           kDefaultLineSpacing, &error) &&
              docs.findTheme(sunset)->backgroundGradient.empty(),
          "and the empty string paints it flat again");
    error.clear();
    check(!docs.updateTheme(sunset, "#ffffff", "#301020", "noon", "DejaVu Serif", 64.0,
                            "center", kDefaultLineSpacing, &error) &&
              error.find("background gradient") != std::string::npos,
          "an update naming a non-colour is refused naming its role");
    check(docs.findTheme(sunset)->backgroundGradient.empty(),
          "the refused update left the flat answer alone");

    check(docs.setSlideTheme(show, slide, other, &error) &&
              docs.setShowDefaultTheme(show, other, &error),
          "point both the override and the default at one style");
    check(docs.removeTheme(other, &error) && docs.findTheme(other) == nullptr &&
              docs.findShow(show)->defaultThemeId.empty() &&
              docs.findShow(show)->slides[0].themeId.empty(),
          "removing a style clears every reference to it");
    check(docs.themeForSlide(show, slide) == nullptr,
          "a show whose default was cleared resolves to no style");
    // Sunset was only ever a value-holder, never a reference — removal
    // clears `other` and leaves the rest in creation order, which starts
    // with the seeded Default floor (amendment 2026-11-02).
    check(docs.themeIds() == std::vector<Id>({"default", theme, sunset}),
          "styleIds reflects removal");
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
// themes with default + override, a playlist with a repeat show and
// sections, and one background asset whose file lives outside the
// package (created at assetSource).
void buildSampleService(Documents* docs, const fs::path& assetSource) {
    const Id house = docs->createTheme("House", "#ffffff", "#202040", "", "DejaVu Serif", 64.0,
                                       "center", kDefaultLineSpacing);
    const Id loud = docs->createTheme("Loud", "#ffff00", "#000000", "", "Impact", 96.0, "center",
                                      kDefaultLineSpacing);
    const Id asset = docs->addAsset("stage loop.jpg", assetSource.string());

    const Id order = docs->createShow("Service Order");
    docs->setShowDefaultTheme(order, house);
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
    docs->setSlideTheme(hymn, finalVerse, loud);

    const Id playlist = docs->createPlaylist("Sunday");
    docs->addPlaylistEntry(playlist, order, "Gathering");
    docs->addPlaylistEntry(playlist, hymn, "");
    docs->addPlaylistEntry(playlist, order, "Dismiss");
}

// Field-by-field model comparison (the Documents API is the only view).
bool sameService(const Documents& a, const Documents& b) {
    if (a.currentThemeId() != b.currentThemeId() || a.showIds() != b.showIds() ||
        a.themeIds() != b.themeIds() || a.playlistIds() != b.playlistIds() ||
        a.assetIds() != b.assetIds()) {
        return false;
    }
    for (const Id& showId : a.showIds()) {
        const Show* sa = a.findShow(showId);
        const Show* sb = b.findShow(showId);
        if (sa == nullptr || sb == nullptr || sa->name != sb->name ||
            sa->notes != sb->notes || sa->defaultThemeId != sb->defaultThemeId ||
            sa->scheduleOwned != sb->scheduleOwned || sa->scripture != sb->scripture ||
            // Task SL.6: the catalog link and the copied attribution are
            // service state; a round trip that dropped either would let the
            // running order's reference die silently.
            sa->catalogItemId != sb->catalogItemId || sa->song != sb->song ||
            sa->slides.size() != sb->slides.size()) {
            return false;
        }
        for (size_t i = 0; i < sa->slides.size(); ++i) {
            const Slide& x = sa->slides[i];
            const Slide& y = sb->slides[i];
            if (x.id != y.id || x.group != y.group || x.notes != y.notes ||
                x.themeId != y.themeId || x.customizations != y.customizations ||
                x.backgroundAssetId != y.backgroundAssetId ||
                // The named text context is slide state (task TF.1): a round
                // trip that dropped the name would compare equal while a
                // future theme silently stopped applying to the passage.
                x.textContext != y.textContext) {
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
    for (const Id& themeId : a.themeIds()) {
        const Theme* xa = a.findTheme(themeId);
        const Theme* xb = b.findTheme(themeId);
        if (xa == nullptr || xb == nullptr || xa->name != xb->name ||
            xa->textColor != xb->textColor || xa->backgroundColor != xb->backgroundColor ||
            xa->backgroundGradient != xb->backgroundGradient ||
            xa->fontFamily != xb->fontFamily || xa->fontSize != xb->fontSize ||
            xa->align != xb->align || xa->lineSpacing != xb->lineSpacing ||
            // The lyric-box placement is theme state too (amendment
            // 2026-11-01): a round trip that dropped it would compare equal
            // while quietly moving every wearing slide's words. Contexts
            // (task TF.1) follow the same rule, entry and field by field.
            xa->placement != xb->placement || xa->contexts != xb->contexts ||
            // The TS.1 style block is theme state by the same argument:
            // a round trip that dropped the weight or turned a disabled
            // record back into silence would compare equal while the
            // audience's text changed under it.
            xa->fontWeight != xb->fontWeight || xa->fontStyle != xb->fontStyle ||
            xa->fitMode != xb->fitMode || xa->minFontSize != xb->minFontSize ||
            xa->stroke != xb->stroke || xa->shadow != xb->shadow) {
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
    check(reloaded.themeForSlide(hymn, amen) != nullptr &&
              source.themeForSlide(hymn, amen) != nullptr &&
              reloaded.themeForSlide(hymn, amen)->id == source.themeForSlide(hymn, amen)->id,
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

    // --- typography (task 4.4): a theme's align and line spacing
    // round-trip; a service written before typography existed loads with
    // centred text at the renderer's line box; typography the renderer
    // cannot honour is refused naming its theme ---
    {
        Documents typed;
        const Id sharp = typed.createTheme("Sharp", "#ff8800", "#000000", "", "DejaVu Sans", 88.0,
                                           "right", 1.75, &error);
        check(!sharp.empty(), "createStyle accepts typography");
        const fs::path typeset = tempRoot / "typeset";
        Documents typeBack;
        check(openshow::domain::saveService(typed, typeset, &error) &&
                  openshow::domain::loadService(&typeBack, typeset, &error) &&
                  typeBack.findTheme(sharp)->align == "right" &&
                  typeBack.findTheme(sharp)->lineSpacing == 1.75,
              "style typography round-trips through the manifest");

        // A legacy manifest: the theme entry simply lacks the two fields.
        const fs::path oldType = tempRoot / "old-type";
        std::filesystem::create_directories(oldType);
        writeFile(oldType / "service.json",
                  R"({"format":1,"nextOrdinal":9,"shows":[],)"
                  R"("styles":[{"id":"style1","name":"House","textColor":"#ffffff",)"
                  R"("backgroundColor":"#000000","fontFamily":"","fontSize":64}],)"
                  R"("assets":[],"playlists":[]})");
        Documents oldTypeBack;
        check(openshow::domain::loadService(&oldTypeBack, oldType, &error) &&
                  oldTypeBack.findTheme("style1") != nullptr &&
                  oldTypeBack.findTheme("style1")->align == "center" &&
                  oldTypeBack.findTheme("style1")->lineSpacing == kDefaultLineSpacing,
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
        // above: this build writes 7 (task TF.1), and anchoring on a
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
        // service whose every slide has gone blank (task 6.17a). Since task
        // TH.1 the refusal lands one step earlier for this build's file: the
        // box-era keys spell the appearance entity "style", and a manifest
        // whose body carries the "theme" spellings is naming two eras at
        // once — refused on the show's own key, still refused.
        refuses(dir, "not both",
                "a manifest from this build claiming an older era is refused, not read as blank");
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
            for (const Id& id : source.themeIds()) {
                if (source.findTheme(id)->name == "Loud") {
                    return id;
                }
            }
            return Id();
        }();
        check(tamperedPackage(package, dir, [&](std::string& m) {
                  return replaceOnce(m, "\"themeId\":\"" + loud + "\"",
                                     "\"themeId\":\"theme-gone\"");
              }),
              "test tamper anchors exist (themeId)");
        refuses(dir, "unknown theme", "a slide naming a vanished theme is refused");
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
                      std::string("contain") &&
                  openshow::domain::backgroundFitName(openshow::domain::BackgroundFit::Blur) ==
                      std::string("blur"),
              "the domain spells the three fits, and they are the only three spellings");
        openshow::domain::BackgroundFit parsed = openshow::domain::BackgroundFit::Cover;
        check(openshow::domain::backgroundFitFromName("blur", &parsed) &&
                  parsed == openshow::domain::BackgroundFit::Blur,
              "'blur' parses from 6.17c2 — the name joined the set, the wire did not move");
        check(!openshow::domain::backgroundFitFromName("fill", &parsed) &&
                  !openshow::domain::backgroundFitFromName("", &parsed),
              "names outside the set do not parse: 'fill' (the distorting stretch) "
              "is not in the agreed set and no reader will grow it silently");
        check(fitted.findShow(show)->slides[0].backgroundFit == openshow::domain::BackgroundFit::Cover,
              "a brand-new slide defaults to cover — what the worker has drawn since 3.2");
        check(fitted.setSlideBackgroundFit(show, slideA, openshow::domain::BackgroundFit::Contain),
              "a slide chooses contain — legal before any picture exists, because the "
              "choice is the slide's standing answer, not the file's property");
        check(fitted.findShow(show)->slides[1].backgroundFit == openshow::domain::BackgroundFit::Cover,
              "and the other slide is untouched");
        check(fitted.setSlideBackgroundFit(show, slideB, openshow::domain::BackgroundFit::Blur),
              "a slide chooses blur (6.17c2) — the domain stores the choice; whether a"
              " video can carry it is the compile's and the editor's business");
        check(fitted.findShow(show)->slides[1].backgroundFit == openshow::domain::BackgroundFit::Blur,
              "blur is held, slide by slide like every other fit");
        const Id slideC = fitted.appendSlide(show, &error);
        (void)slideC;  // stays cover: the manifest must still spell the default by name
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
                  back.findShow(show)->slides[1].backgroundFit == openshow::domain::BackgroundFit::Blur,
              "the choice survives the round trip, slide by slide");
        check(sameService(fitted, back), "the whole fitted service compares field by field");
        const std::string manifest = readFile(fittedDir / "service.json");
        check(manifest.find("\"fit\":\"contain\"") != std::string::npos &&
                  manifest.find("\"fit\":\"blur\"") != std::string::npos &&
                  manifest.find("\"fit\":\"cover\"") != std::string::npos,
              "all three fits are written by name — cover included, because leaving the "
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

    // --- format 5 (task 6.17c3): every theme carries a
    //     "backgroundGradient", older files mean flat, and a hand-bumped
    //     key in one is refused naming itself ---
    {
        Documents graded;
        const Id show = graded.createShow("Graded", &error);
        const Id sunset = graded.createTheme("Sunset", "#ffffff", "#301020", "#c05020",
                                             "DejaVu Serif", 64.0, "center",
                                             kDefaultLineSpacing, &error);
        const Id plain = graded.createTheme("Plain", "#ffffff", "#000000", "", "DejaVu Serif",
                                            64.0, "center", kDefaultLineSpacing, &error);
        check(!sunset.empty() && !plain.empty() && graded.setShowDefaultTheme(show, sunset),
              "a graded style and a flat one are created");
        const fs::path gradedDir = tempRoot / "v5-graded";
        Documents back;
        error.clear();
        check(openshow::domain::saveService(graded, gradedDir, &error) &&
                  openshow::domain::loadService(&back, gradedDir, &error),
              "a service of graded styles saves and reloads");
        check(back.findTheme(sunset)->backgroundGradient.empty() &&
                  back.findTheme(plain)->backgroundGradient.empty(),
              "themes remain background-free after reopening");
        check(sameService(graded, back), "the whole graded service compares field by field");
        const std::string manifest = readFile(gradedDir / "service.json");
        check(manifest.find("\"backgroundGradient\"") == std::string::npos,
              "theme backgrounds are absent from saved files");
        check(manifest.find("\"format\":11") != std::string::npos,
              "the manifest says format 11 (foreground themes and the service"
              " catalog song link and attribution)");
        check(manifest.find("\"transition\":{") != std::string::npos,
              "the root transition is written by name, not left to an absent key");
        check(openshow::domain::saveService(back, tempRoot / "v5-regraded", &error) &&
                  readFile(tempRoot / "v5-regraded" / "service.json") == manifest,
              "re-saving a graded service writes a byte-identical manifest");

        // The honest pre-5 file: the key appears nowhere, and flat is not
        // an inference but that file's own history — a gradient was the
        // one background those builds could not draw.
        const std::string themeOld =
            R"({"id":"style1","name":"Old","textColor":"#ffffff","backgroundColor":"#000000",)"
            R"("fontFamily":"","fontSize":64})";
        for (unsigned long long era : {1ull, 2ull, 3ull, 4ull}) {
            const std::string head =
                R"({"format":)" + std::to_string(era) +
                R"(,"nextOrdinal":99,"shows":[],"styles":[)";
            const std::string file = head + themeOld + R"(],"assets":[],"playlists":[]})";
            const fs::path dir = tempRoot / ("v" + std::to_string(era) + "-ungraded");
            std::filesystem::create_directories(dir);
            writeFile(dir / "service.json", file);
            Documents old;
            error.clear();
            if (check(openshow::domain::loadService(&old, dir, &error),
                      "a pre-5 style with no gradient key loads")) {
                check(old.findTheme("style1")->backgroundGradient.empty(),
                      "and reads flat — the only background that era drew");
            }
        }

        // The lies: a format-5 theme with no key (the writer always writes
        // it, so its absence is a hand-edit), and a key hand-bumped into a
        // pre-5 file where it does not exist.
        const std::string themeNoKey =
            R"({"id":"style1","name":"Kept","textColor":"#ffffff","backgroundColor":"#000000",)"
            R"("fontFamily":"","fontSize":64})";
        refusesJson(R"({"format":5,"nextOrdinal":99,"shows":[],"styles":[)" + themeNoKey +
                        R"(],"assets":[],"playlists":[]})",
                    "'backgroundGradient' must be a string",
                    "a format-5 style with no gradient key is refused: the writer "
                    "always writes it, so its absence is a hand-edit");
        const std::string themeLied =
            R"({"id":"style1","name":"Lied","textColor":"#ffffff","backgroundColor":"#000000",)"
            R"("backgroundGradient":"#0000ff","fontFamily":"","fontSize":64})";
        refusesJson(R"({"format":4,"nextOrdinal":99,"shows":[],"styles":[)" + themeLied +
                        R"(],"assets":[],"playlists":[]})",
                    "where that key",
                    "a hand-bumped 'backgroundGradient' in a format-4 file is refused "
                    "rather than trusted — somebody else's build meant something by it");
    }

    // --- theme naming (task TH.1, themes plan) ---
    //
    // Format 6 spells the appearance entity THEME in three key names: the
    // array is "themes", a slide's assignment "themeId" and a show's
    // default "defaultThemeId". A 5-file spells all three "style" and
    // still loads; a file that MIXES the spellings is refused naming both
    // — half a rename is ambiguity, not intent (the gradient's rule).
    {
        Documents named;
        const fs::path v6 = tempRoot / "v6-named";
        const Id home = named.createShow("Named");
        const Id house = named.createTheme("House", "#ffd23f", "#101828", "", "DejaVu Serif",
                                           64.0, "left", 1.6, &error);
        const Id bright = named.createTheme("Bright", "#ffffff", "#000000", "", "DejaVu Serif",
                                            72.0, "center", kDefaultLineSpacing, &error);
        const Id first = named.appendSlide(home);
        const Id second = named.appendSlide(home);
        check(!house.empty() && !bright.empty() && !first.empty() && !second.empty() &&
                  named.setShowDefaultTheme(home, house) &&
                  named.setSlideTheme(home, second, bright),
              "a show with a default theme and one themed slide is authored");
        check(openshow::domain::saveService(named, v6, &error), "the themed service saves");
        const std::string manifest = readFile(v6 / "service.json");
        check(manifest.find("\"themes\":[") != std::string::npos &&
                  manifest.find("\"defaultThemeId\":\"" + house + "\"") != std::string::npos &&
                  manifest.find("\"themeId\":\"" + bright + "\"") != std::string::npos,
              "the writer names the entity theme in all three keys");
        Documents back;
        check(openshow::domain::loadService(&back, v6, &error) && sameService(named, back),
              "a format-6 service round-trips through the new names");

        // The 5-file: written by hand so the migration is what runs, not
        // the writer. Every key the entity touches spells "style".
        const fs::path v5 = tempRoot / "v5-named";
        std::filesystem::create_directories(v5);
        writeFile(v5 / "service.json",
                  R"({"format":5,"nextOrdinal":99,"shows":[{"id":"show1","name":"Old",)"
                  R"("defaultStyleId":"style1","slides":[)"
                  R"({"id":"slide1","group":"","items":[],"notes":"","styleId":"",)"
                  R"("backgroundAssetId":"","fit":"cover"},)"
                  R"({"id":"slide2","group":"","items":[],"notes":"","styleId":"style2",)"
                  R"("backgroundAssetId":"","fit":"cover"}]}],"styles":[)"
                  R"({"id":"style1","name":"House","textColor":"#ffd23f",)"
                  R"("backgroundColor":"#101828","backgroundGradient":"",)"
                  R"("fontFamily":"DejaVu Serif","fontSize":64,"align":"left","lineSpacing":1.6},)"
                  R"({"id":"style2","name":"Bright","textColor":"#ffffff",)"
                  R"("backgroundColor":"#000000","backgroundGradient":"",)"
                  R"("fontFamily":"DejaVu Serif","fontSize":72,"align":"center","lineSpacing":1.25}],)"
                  R"("assets":[],"playlists":[]})");
        Documents migrated;
        error.clear();
        if (check(openshow::domain::loadService(&migrated, v5, &error),
                  "a format-5 file spelled 'style' still loads")) {
            check(migrated.findTheme("style1") != nullptr &&
                      migrated.findTheme("style1")->name == "House",
                  "its 'styles' array reads as themes");
            check(migrated.findShow("show1")->defaultThemeId == "style1" &&
                      migrated.findShow("show1")->slides[1].themeId == "style2",
                  "the show default and the slide assignment followed the old keys");
            check(migrated.themeForSlide("show1", "slide1") != nullptr &&
                      migrated.themeForSlide("show1", "slide1")->id == "style1" &&
                      migrated.themeForSlide("show1", "slide2")->id == "style2",
                  "resolution answers through the migrated chain");
            const fs::path resaved = tempRoot / "v5-then-saved";
            check(openshow::domain::saveService(migrated, resaved, &error) &&
                      readFile(resaved / "service.json").find("\"themes\":[") !=
                          std::string::npos,
                  "the first save by this build re-spells the package to its own format");
        }

        // The mixes: a key from the other era, in either direction, is
        // refused naming both spellings.
        refusesJson(R"({"format":6,"nextOrdinal":99,"shows":[],"themes":[],"styles":[],)"
                        R"("assets":[],"playlists":[]})",
                    "not both",
                    "a format-6 file carrying both array names is refused");
        refusesJson(R"({"format":6,"nextOrdinal":99,"currentThemeId":"",)"
                    R"("shows":[{"id":"show1","name":"Mix",)"
                    R"("slides":[{"id":"slide1","group":"","items":[],"notes":"",)"
                        R"("themeId":"","styleId":"","backgroundAssetId":""}]}],)"
                        R"("themes":[],"assets":[],"playlists":[]})",
                    "not both",
                    "a slide spelled both ways is refused");
        refusesJson(R"({"format":5,"nextOrdinal":99,"shows":[{"id":"show1","name":"Mix",)"
                        R"("defaultThemeId":"","slides":[]}],"styles":[],"assets":[],)"
                        R"("playlists":[]})",
                    "not both",
                    "a format-5 file carrying the new show key is refused naming both");
    }

    // --- current theme and customization in the file (task TH.2) ---
    {
        Documents themed;
        const fs::path worn = tempRoot / "worn";
        const Id home = themed.createShow("Worn");
        const Id house = themed.createTheme("House", "#ffd23f", "#101828", "#000000",
                                            "DejaVu Serif", 64.0, "left", 1.6, &error);
        const Id first = themed.appendSlide(home);
        const Id second = themed.appendSlide(home);
        SlideCustomization changed;
        changed.textColor = "#abcdef";
        changed.backgroundGradient = "";  // explicitly flat over the graded theme
        changed.fontSize = 0.0;           // explicitly the renderer's default
        changed.lineSpacing = 2.5;
        check(themed.setCurrentTheme(house) &&
                  themed.setSlideCustomizations(home, first, changed, &error),
              "a current theme and a customized slide are authored");
        check(openshow::domain::saveService(themed, worn, &error), "the worn service saves");
        const std::string manifest = readFile(worn / "service.json");
        check(manifest.find("\"currentThemeId\":\"" + house + "\"") != std::string::npos,
              "the file names the service's current theme");
        check(manifest.find("\"textColor\":\"#abcdef\"") != std::string::npos &&
                  manifest.find("\"backgroundGradient\":\"\"") != std::string::npos &&
                  manifest.find("\"fontSize\":0") != std::string::npos &&
                  manifest.find("\"lineSpacing\":2.5") != std::string::npos,
              "the customized fields are written with their sparse keys");
        check(manifest.find("\"customizations\":{}") != std::string::npos,
              "the untouched slide carries the empty answer, not a missing key");
        Documents back;
        check(openshow::domain::loadService(&back, worn, &error) && sameService(themed, back),
              "current theme and customizations round-trip field by field");
        check(back.findShow(home)->slides[0].customizations.textColor ==
                      std::optional<std::string>("#abcdef") &&
                  back.findShow(home)->slides[0].customizations.fontSize ==
                      std::optional<double>(0.0) &&
                  back.findShow(home)->slides[1].customizations.empty(),
              "the customization lands on its slide alone");
    }

    // Era and shape refusals: the keys belong to format 6 and only format 6,
    // and inside the object every field is the shape its theme twin has.
    const std::string minimalSlide =
        R"({"id":"slide1","group":"","items":[],"notes":"","themeId":"","fit":"cover",)";
    refusesJson(R"({"format":6,"nextOrdinal":99,"shows":[],"themes":[],)"
                    R"("assets":[],"playlists":[]})",
                "currentThemeId",
                "a format-6 file with no currentThemeId is refused — the writer "
                "always writes it, so absence is a hand-edit");
    refusesJson(R"({"format":5,"nextOrdinal":99,"currentThemeId":"","shows":[],)"
                    R"("styles":[],"assets":[],"playlists":[]})",
                "does not exist",
                "a currentThemeId in a format-5 file is refused — that era had none");
    refusesJson(R"({"format":5,"nextOrdinal":99,"shows":[{"id":"show1","name":"Old",)"
                    R"("slides":[{"id":"slide1","group":"","items":[],"notes":"",)"
                      R"("fit":"cover","styleId":"","backgroundAssetId":"",)"
                      R"("customizations":{}}]}],"styles":[],"assets":[],)"
                      R"("playlists":[]})",
                "does not exist",
                "a customizations key in a format-5 file is refused naming the era");
    refusesJson(R"({"format":6,"nextOrdinal":99,"currentThemeId":"","shows":[)"
                    R"({"id":"show1","name":"Bare","slides":[)" + minimalSlide +
                    R"("backgroundAssetId":""}]}],"themes":[],"assets":[],)"
                    R"("playlists":[]})",
                "hand-edit",
                "a format-6 slide with no customizations key is refused as a hand-edit");
    refusesJson(R"({"format":6,"nextOrdinal":99,"currentThemeId":"","shows":[)"
                    R"({"id":"show1","name":"Wrong","slides":[)" + minimalSlide +
                    R"("backgroundAssetId":"","customizations":{"fontSize":"big"}}]}],)"
                    R"("themes":[],"assets":[],"playlists":[]})",
                "must be a number",
                "a customization of the wrong shape is refused naming its field");
    refusesJson(R"({"format":6,"nextOrdinal":99,"currentThemeId":"","shows":[)"
                    R"({"id":"show1","name":"Alien","slides":[)" + minimalSlide +
                    R"("backgroundAssetId":"","customizations":{"bold":true}}]}],)"
                    R"("themes":[],"assets":[],"playlists":[]})",
                "not a field",
                "a customization naming no known field is refused naming the key");
    refusesJson(R"({"format":6,"nextOrdinal":99,"currentThemeId":"","shows":[)"
                    R"({"id":"show1","name":"Green","slides":[)" + minimalSlide +
                    R"("backgroundAssetId":"","customizations":{"textColor":"green"}}]}],)"
                    R"("themes":[],"assets":[],"playlists":[]})",
                "cannot honour",
                "a customization no theme could carry is refused at the load");
    refusesJson(R"({"format":6,"nextOrdinal":99,"currentThemeId":"theme9","shows":[],)"
                    R"("themes":[],"assets":[],"playlists":[]})",
                "current theme 'theme9' is unknown",
                "a current theme the service does not have is refused");

    // --- placement on the wire (themes plan amendment 2026-11-01) ---
    // A theme's rectangle and a slide's placement customization round-trip,
    // absence stays absence, and a rectangle nobody could draw is refused
    // at the load naming its owner.
    {
        Documents placed;
        const Id show = placed.createShow("Placed", &error);
        const Id slide = placed.appendSlide(show, &error);
        const Id follow = placed.appendSlide(show, &error);
        const Id wide = placed.createTheme("Wide", "#ffffff", "#000000", "", "DejaVu Serif",
                                           64.0, "center", kDefaultLineSpacing, &error);
        const Id mute = placed.createTheme("Mute", "#eeeeee", "#101010", "", "DejaVu Serif",
                                           48.0, "left", kDefaultLineSpacing, &error);
        openshow::domain::BoxRect rect;
        rect.x = 240.5;
        rect.y = -30.0;
        rect.width = 1440.0;
        rect.height = 480.25;
        openshow::domain::BoxRect own;
        own.x = 10.0;
        own.y = 20.0;
        own.width = 300.0;
        own.height = 200.0;
        check(!wide.empty() && !mute.empty() &&
                  placed.setThemePlacement(wide, rect, &error) &&
                  placed.setShowDefaultTheme(show, wide) &&
                  placed.setSlideTheme(show, slide, mute),
              "a placing theme, a theme without one and two slides are authored");
        SlideCustomization changed;
        changed.placement = own;
        check(placed.setSlideCustomizations(show, slide, changed, &error),
              "one slide customizes its placement");

        const fs::path dir = tempRoot / "placement";
        check(openshow::domain::saveService(placed, dir, &error), "the placed service saves");
        const std::string manifest = readFile(dir / "service.json");
        // The four numbers are boost's shortest spelling of each double, so
        // the shape is what this asserts — an object under the key, exactly
        // two of them in the file (the theme's and the slide's, no more).
        // The values themselves are pinned where they belong: on the reload.
        auto countPlacementObjects = [&manifest]() {
            int found = 0;
            for (size_t at = manifest.find("\"placement\":{"); at != std::string::npos;
                 at = manifest.find("\"placement\":{", at + 1)) {
                ++found;
            }
            return found;
        };
        check(countPlacementObjects() == 2,
              "the theme's rectangle and the slide's customized one are each written once");
        Documents back;
        check(openshow::domain::loadService(&back, dir, &error) && sameService(placed, back),
              "placement round-trips field by field and the services compare equal");
        check(back.findTheme(wide)->placement == std::optional<openshow::domain::BoxRect>(rect) &&
                  back.findTheme(mute)->placement == std::nullopt,
              "the placing theme keeps its rectangle and the silent one stays silent");
        check(back.findShow(show)->slides[0].customizations.placement ==
                      std::optional<openshow::domain::BoxRect>(own) &&
                  back.findShow(show)->slides[1].customizations.placement == std::nullopt,
              "the customization lands on its slide alone");
        const fs::path again = tempRoot / "placement-again";
        check(openshow::domain::saveService(back, again, &error) &&
                  readFile(again / "service.json") == manifest,
              "re-saving a placed service writes a byte-identical manifest");

        // The one theme member whose ABSENCE is the answer: every theme
        // written before the amendment means "no opinion", so a file with
        // no placement key is not a hand-edit and nothing moves.
        refusesJson(R"({"format":6,"nextOrdinal":99,"currentThemeId":"","shows":[],)"
                        R"("themes":[{"id":"theme1","name":"Placed","textColor":"#ffffff",)"
                        R"("backgroundColor":"#000000","backgroundGradient":"","fontFamily":"",)"
                        R"("fontSize":64,"placement":[]}],"assets":[],"playlists":[]})",
                    "not an object",
                    "a theme placement that is not an object is refused");
        refusesJson(R"({"format":6,"nextOrdinal":99,"currentThemeId":"","shows":[],)"
                        R"("themes":[{"id":"theme1","name":"Half","textColor":"#ffffff",)"
                        R"("backgroundColor":"#000000","backgroundGradient":"","fontFamily":"",)"
                        R"("fontSize":64,"placement":{"x":1,"y":2,"width":3}}],)"
                        R"("assets":[],"playlists":[]})",
                    R"(placement needs a number "height")",
                    "a theme placement missing a corner is refused naming it");
        refusesJson(R"({"format":6,"nextOrdinal":99,"currentThemeId":"","shows":[],)"
                        R"("themes":[{"id":"theme1","name":"Flat","textColor":"#ffffff",)"
                        R"("backgroundColor":"#000000","backgroundGradient":"","fontFamily":"",)"
                        R"("fontSize":64,"placement":{"x":1,"y":2,"width":0,"height":3}}],)"
                        R"("assets":[],"playlists":[]})",
                    "cannot be placed",
                    "a zero-width theme rectangle is refused at the load");
        refusesJson(R"({"format":5,"nextOrdinal":99,"shows":[],)"
                        R"("styles":[{"id":"style1","name":"Old","textColor":"#ffffff",)"
                        R"("backgroundColor":"#000000","backgroundGradient":"",)"
                        R"("fontFamily":"","fontSize":64,"placement":)"
                        R"({"x":1,"y":2,"width":3,"height":4}}],"assets":[],)"
                        R"("playlists":[]})",
                    "does not exist",
                    "a hand-bumped \"placement\" in a format-5 file is refused naming the era");
        refusesJson(R"JSON({"format":6,"nextOrdinal":99,"currentThemeId":"","shows":[
            {"id":"show1","name":"Bad","slides":[
                {"id":"slide1","group":"","items":[],"notes":"","themeId":"","fit":"cover",
                 "backgroundAssetId":"","customizations":
                     {"placement":{"x":1,"y":2,"width":300,"height":0}}}]}],
            "themes":[],"assets":[],"playlists":[]})JSON",
                "cannot be placed",
                "a customized rectangle with nothing in it is refused at the load");
        refusesJson(R"JSON({"format":6,"nextOrdinal":99,"currentThemeId":"","shows":[
            {"id":"show1","name":"Bad","slides":[
                {"id":"slide1","group":"","items":[],"notes":"","themeId":"","fit":"cover",
                 "backgroundAssetId":"","customizations":{"placement":"everywhere"}}]}],
            "themes":[],"assets":[],"playlists":[]})JSON",
                "not an object",
                "a customized placement of the wrong shape is refused naming its owner");
    }

    // --- text contexts on the wire (task TF.1) ---
    // A theme's named typography-and-box overrides and a slide's context
    // name round-trip field by field; a name no theme carries is preserved
    // untouched for the theme that will grow it; and the shape refusals
    // guard the one rule a context must never break — it stores how text
    // is SET, never how many lines it gets.
    {
        Documents lectern;
        const Id show = lectern.createShow("Lectern", &error);
        const Id verse = lectern.appendSlide(show, &error);
        const Id chorus = lectern.appendSlide(show, &error);
        const Id orphan = lectern.appendSlide(show, &error);
        const Id theme = lectern.createTheme("House", "#ffffff", "#101020", "", "DejaVu Serif",
                                             64.0, "center", kDefaultLineSpacing, &error);
        openshow::domain::TextContext reading;
        reading.name = "scripture";
        reading.fontFamily = "Lectern Serif";
        reading.fontSize = 40.0;
        reading.lineSpacing = 1.25;
        reading.align = "left";
        openshow::domain::BoxRect readingBox;
        readingBox.x = 120.0;
        readingBox.y = 80.0;
        readingBox.width = 1680.0;
        readingBox.height = 920.5;
        reading.box = readingBox;
        openshow::domain::TextContext narrow;  // silent on everything but its inset
        narrow.name = "narrow";
        narrow.inset = 160.0;
        check(!theme.empty() &&
                  lectern.setThemeContexts(theme, {reading, narrow}, &error),
              "a theme accepts a named reading context and a silent inset one");
        check(lectern.setSlideTextContext(show, verse, "scripture", &error) &&
                  lectern.setSlideTextContext(show, orphan, "not-yet-a-context", &error) &&
                  lectern.setShowDefaultTheme(show, theme, &error),
              "the passage names scripture, one slide names a context nobody carries yet");

        const fs::path dir = tempRoot / "contexts";
        check(openshow::domain::saveService(lectern, dir, &error), "the contexted service saves");
        const std::string manifest = readFile(dir / "service.json");
        check(manifest.find("\"contexts\":[") != std::string::npos &&
                  manifest.find("\"textContext\":\"scripture\"") != std::string::npos,
              "the contexts array and the slide's context name are both written by name");
        check(manifest.find("\"textContext\":\"not-yet-a-context\"") != std::string::npos,
              "the name no theme carries still travels");
        // The numbers are boost's shortest spelling of each double (the
        // placement rule again), so the manifest is asked only whether the
        // inset key travels and whether the FORBIDDEN keys do not; the
        // values themselves are pinned on the reload below.
        check(manifest.find("\"inset\":") != std::string::npos &&
                  manifest.find("\"maxLines\"") == std::string::npos &&
                  manifest.find("\"lineCount\"") == std::string::npos,
              "the inset travels and no line-count key exists to travel in");
        int namedSlides = 0;
        for (size_t at = manifest.find("\"textContext\""); at != std::string::npos;
             at = manifest.find("\"textContext\"", at + 1)) {
            ++namedSlides;
        }
        check(namedSlides == 2,
              "only the two named slides carry a context key — silence is absence");
        Documents back;
        check(openshow::domain::loadService(&back, dir, &error) && sameService(lectern, back),
              "contexts and names round-trip field by field and the services compare equal");
        const Theme* reloaded = back.findTheme(theme);
        check(reloaded != nullptr && reloaded->contexts.size() == 2 &&
                  reloaded->contexts[0] == reading && reloaded->contexts[1] == narrow,
              "both contexts land exactly as authored, the silent one still silent");
        const fs::path again = tempRoot / "contexts-again";
        check(openshow::domain::saveService(back, again, &error) &&
                  readFile(again / "service.json") == manifest,
              "re-saving a contexted service writes a byte-identical manifest");
    }

    // The lies a context file can tell: a line count (the field that must
    // never exist), another era's key, a squatter's name twice, and both
    // answers to "where" at once.
    {
        const std::string goodTheme =
            R"({"id":"theme1","name":"House","textColor":"#ffffff","backgroundColor":"#000000",)"
            R"("backgroundGradient":"","fontFamily":"","fontSize":64,"align":"center",)"
            R"("lineSpacing":1.5})";
        const std::string head =
            R"({"format":7,"nextOrdinal":99,"currentThemeId":"","shows":[],"themes":[)";
        const std::string tail = R"(],"assets":[],"playlists":[]})";
        refusesJson(head +
                        R"({"id":"theme1","name":"House","textColor":"#ffffff",)"
                        R"("backgroundColor":"#000000","backgroundGradient":"",)"
                        R"("fontFamily":"","fontSize":64,"align":"center","lineSpacing":1.5,)"
                        R"("contexts":[{"name":"scripture","maxLines":6}]})" + tail,
                    "never a line count",
                    "a context carrying 'maxLines' is refused naming the rule it breaks");
        refusesJson(R"({"format":6,"nextOrdinal":99,"currentThemeId":"","shows":[],"themes":[)"
                        R"({"id":"theme1","name":"House","textColor":"#ffffff",)"
                        R"("backgroundColor":"#000000","backgroundGradient":"",)"
                        R"("fontFamily":"","fontSize":64,"align":"center","lineSpacing":1.5,)"
                        R"("contexts":[{"name":"scripture","fontSize":40}]}],"assets":[],)"
                        R"("playlists":[]})",
                    "does not exist",
                    "a hand-bumped \"contexts\" in a format-6 file is refused naming the era");
        refusesJson(R"({"format":6,"nextOrdinal":99,"currentThemeId":"","shows":[)"
                        R"({"id":"show1","name":"Old","slides":[)"
                        R"({"id":"slide1","group":"","items":[],"notes":"","themeId":"",)"
                        R"("fit":"cover","backgroundAssetId":"","customizations":{},)"
                        R"("textContext":"scripture"}]}],"themes":[],"assets":[],)"
                        R"("playlists":[]})",
                    "does not exist",
                    "a hand-bumped \"textContext\" in a format-6 file is refused naming the era");
        refusesJson(head +
                        R"({"id":"theme1","name":"House","textColor":"#ffffff",)"
                        R"("backgroundColor":"#000000","backgroundGradient":"",)"
                        R"("fontFamily":"","fontSize":64,"align":"center","lineSpacing":1.5,)"
                        R"("contexts":[{"name":"scripture","fontSize":40},)"
                        R"({"name":"scripture","fontSize":30}]})" + tail,
                    "names context 'scripture' twice",
                    "a theme that names one context twice is refused");
        refusesJson(head +
                        R"({"id":"theme1","name":"House","textColor":"#ffffff",)"
                        R"("backgroundColor":"#000000","backgroundGradient":"",)"
                        R"("fontFamily":"","fontSize":64,"align":"center","lineSpacing":1.5,)"
                        R"("contexts":[{"fontSize":40}]})" + tail,
                    "has no name",
                    "a context with no name is refused naming its theme");
        refusesJson(head +
                        R"({"id":"theme1","name":"House","textColor":"#ffffff",)"
                        R"("backgroundColor":"#000000","backgroundGradient":"",)"
                        R"("fontFamily":"","fontSize":64,"align":"center","lineSpacing":1.5,)"
                        R"("contexts":[{"name":"scripture","fontSize":40,"inset":80,)"
                        R"("box":{"x":0,"y":0,"width":100,"height":100}}]})" + tail,
                    "both a box and an inset",
                    "box and inset in one context are refused — the inset would shrink nothing");
        refusesJson(head +
                        R"({"id":"theme1","name":"House","textColor":"#ffffff",)"
                        R"("backgroundColor":"#000000","backgroundGradient":"",)"
                        R"("fontFamily":"","fontSize":64,"align":"center","lineSpacing":1.5,)"
                        R"("contexts":[{"name":"scripture","align":"justify"}]})" + tail,
                    "not an alignment this build can draw",
                    "a context naming an alignment the renderer cannot draw is refused");
        refusesJson(head +
                        R"({"id":"theme1","name":"House","textColor":"#ffffff",)"
                        R"("backgroundColor":"#000000","backgroundGradient":"",)"
                        R"("fontFamily":"","fontSize":64,"align":"center","lineSpacing":1.5,)"
                        R"("contexts":[{"name":"scripture","textColor":"#00ff00"}]})" + tail,
                    "never a line count or a colour",
                    "a context reaching for a colour is refused — it styles placement, never look");
        refusesJson(head +
                        R"({"id":"theme1","name":"House","textColor":"#ffffff",)"
                        R"("backgroundColor":"#000000","backgroundGradient":"",)"
                        R"("fontFamily":"","fontSize":64,"align":"center","lineSpacing":1.5,)"
                        R"("contexts":"scripture"})" + tail,
                    "'contexts' must be an array",
                    "a 'contexts' that is not an array is refused");
    }
}

// The full appearance chain (task TH.2, themes plan): what one slide draws
// is its own customization first, then the service's current theme, then
// the slide's theme, then the show's default — property by property, so a
// slide that tweaked only the background still takes the current theme's
// font. Below every layer sit the scene compile's built-ins, which this
// layering reports as silence ("" and 0) rather than guessing.
void exerciseThemeChain() {
    std::string error;
    Documents docs;
    const Id show = docs.createShow("Chained");
    const Id slide = docs.appendSlide(show);
    const Id other = docs.appendSlide(show);
    const Id showTheme = docs.createTheme("Show", "#111111", "#222222", "", "Show Serif",
                                          40.0, "left", 1.1, &error);
    const Id slideTheme = docs.createTheme("Slide", "#333333", "#444444", "", "Slide Serif",
                                           50.0, "right", 1.3, &error);
    const Id current = docs.createTheme("Current", "#555555", "#666666", "#777777",
                                        "Current Sans", 60.0, "center", 1.5, &error);
    check(!show.empty() && !slide.empty() && !other.empty() && !showTheme.empty() &&
              !slideTheme.empty() && !current.empty() &&
              docs.setShowDefaultTheme(show, showTheme) &&
              docs.setSlideTheme(show, slide, slideTheme),
          "a show default, a slide theme and a candidate current theme exist");

    // With no current theme the chain is the two layers the service always had.
    ResolvedAppearance a = docs.appearanceForSlide(show, slide);
    check(a.textColor == "#333333" && a.backgroundColor.empty() &&
              a.backgroundGradient.empty() && a.fontFamily == "Slide Serif" &&
              a.fontSize == 50.0 && a.align == "right" && a.lineSpacing == 1.3,
          "with no current theme the slide theme speaks every property");
    a = docs.appearanceForSlide(show, other);
    check(a.textColor == "#111111" && a.fontSize == 40.0 && a.align == "left",
          "a slide following the show resolves the show default");

    // The current theme is worn over both: every property it names is its own.
    check(docs.setCurrentTheme(current) && docs.currentThemeId() == current,
          "setting the current theme names it service-wide");
    a = docs.appearanceForSlide(show, slide);
    check(a.textColor == "#555555" && a.backgroundColor.empty() &&
              a.backgroundGradient.empty() && a.fontFamily == "Current Sans" &&
              a.fontSize == 60.0 && a.align == "center" && a.lineSpacing == 1.5,
          "the current theme outranks the slide theme and the show default");

    // Unknown themes are refused and change nothing; clearing is "" and always fits.
    check(!docs.setCurrentTheme("theme-gone", &error) && docs.currentThemeId() == current &&
                  error.find("theme") != std::string::npos,
              "an unknown current theme is refused and the current one survives");
    check(docs.setCurrentTheme("") && docs.currentThemeId().empty(),
          "the empty id takes the current theme off");
    a = docs.appearanceForSlide(show, slide);
    check(a.textColor == "#333333", "with the current theme off the slide theme speaks again");
    check(docs.setCurrentTheme(current), "and it can go back on");

    // Customization, per property.
    SlideCustomization changed;
    changed.textColor = "#eeeeee";
    check(docs.setSlideCustomizations(show, slide, changed, &error),
          "a slide customizes one colour");
    a = docs.appearanceForSlide(show, slide);
    check(a.textColor == "#eeeeee" && a.backgroundColor.empty() &&
              a.fontFamily == "Current Sans" && a.fontSize == 60.0,
          "the customized field wins and every other still comes from the chain");

    // The empties and zeroes that ARE answers: flat over graded, default over sized.
    changed.backgroundGradient = "";
    changed.fontSize = 0.0;
    check(docs.setSlideCustomizations(show, slide, changed, &error),
          "an empty gradient and a size of 0 are accepted as answers");
    a = docs.appearanceForSlide(show, slide);
    check(a.backgroundGradient.empty() && a.fontSize == 0.0 && a.textColor == "#eeeeee",
          "flat-over-graded and renderer-default-over-sized resolve as asked");

    // setSlideCustomizations REPLACES: the last write is the whole set.
    SlideCustomization onlyAlign;
    onlyAlign.align = "left";
    check(docs.setSlideCustomizations(show, slide, onlyAlign, &error) &&
              docs.findShow(show)->slides[0].customizations.textColor ==
                  std::nullopt,
          "writing the set replaces it — an unset field is not customized");
    a = docs.appearanceForSlide(show, slide);
    check(a.align == "left" && a.textColor == "#555555",
          "the replaced set resolves its own field and re-inherits the rest");

    // Refusals: a slide can never customize itself into an appearance no theme could carry.
    SlideCustomization badColor;
    badColor.textColor = "green";
    check(!docs.setSlideCustomizations(show, slide, badColor, &error),
          "a bad customized colour is refused");
    SlideCustomization emptyText;
    emptyText.textColor = "";
    check(!docs.setSlideCustomizations(show, slide, emptyText, &error),
          "an empty text colour is a hand-edit, not an answer");
    SlideCustomization badAlign;
    badAlign.align = "middle";
    check(!docs.setSlideCustomizations(show, slide, badAlign, &error),
          "an unknown alignment is refused");
    SlideCustomization badSpacing;
    badSpacing.lineSpacing = 20.0;
    check(!docs.setSlideCustomizations(show, slide, badSpacing, &error),
          "a line multiplier out of range is refused");
    check(docs.findShow(show)->slides[0].customizations == onlyAlign,
          "every refusal left the previous set exactly as it was");
    check(!docs.setSlideCustomizations(show, "slide-gone", onlyAlign, &error) &&
              !docs.setSlideCustomizations("show-gone", slide, onlyAlign, &error),
          "customizing an unknown show or slide is refused");

    // Removing restores one slide — and refuses when there was nothing to restore.
    check(docs.removeSlideCustomizations(show, slide, &error) &&
              docs.findShow(show)->slides[0].customizations.empty(),
          "removing a slide's customizations sends it back to the themes");
    check(!docs.removeSlideCustomizations(show, slide, &error) &&
              error.find("no customizations") != std::string::npos,
          "removing twice refuses — a restore that restored nothing is a wrong click");

    // Bulk restore for a whole show: one call clears every customized slide.
    SlideCustomization tint;
    tint.backgroundColor = "#123456";
    check(docs.setSlideCustomizations(show, slide, tint, &error) &&
              docs.setSlideCustomizations(show, other, tint, &error),
          "both slides of the show are customized");
    check(docs.removeShowCustomizations(show, &error) &&
              docs.findShow(show)->slides[0].customizations.empty() &&
              docs.findShow(show)->slides[1].customizations.empty(),
          "restoring the show clears every slide in one call");
    check(!docs.removeShowCustomizations(show, &error) &&
              !docs.removeShowCustomizations("show-gone", &error),
          "restoring a clean show, or no show, is refused");

    // Bulk apply (TH.5's surface, domain today): one mutation, all-or-nothing.
    const Id redo = docs.createTheme("Redo", "#abcdef", "#000000", "", "DejaVu Serif",
                                     64.0, "center", kDefaultLineSpacing, &error);
    check(docs.applyThemeToSlides(show, {slide}, redo, &error) &&
              docs.findShow(show)->slides[0].themeId == redo &&
              docs.findShow(show)->slides[1].themeId.empty(),
          "applying to named slides touches exactly those slides");
    check(docs.applyThemeToSlides(show, {slide, other}, "", &error) &&
              docs.findShow(show)->slides[0].themeId.empty() &&
              docs.findShow(show)->slides[1].themeId.empty(),
          "the empty theme sends the whole list back to following the show");
    check(!docs.applyThemeToSlides(show, {}, redo, &error),
          "a bulk that names no slides is a wrong click, not a success");
    check(!docs.applyThemeToSlides(show, {slide, "slide-gone"}, redo, &error) &&
              docs.findShow(show)->slides[0].themeId.empty() &&
              docs.findShow(show)->slides[1].themeId.empty(),
          "one dead id in the list applies nothing — no half-applied bulk");
    check(!docs.applyThemeToSlides(show, {slide}, "theme-gone", &error),
          "applying a vanished theme is refused");

    // Undo wraps these operations exactly as it wraps everything else:
    // History snapshots whole states, and wearing and customization are
    // part of the state — TH.4's buttons get undo by inheritance, not
    // by special handling.
    History history(&docs);
    check(history.apply([&] { return docs.setSlideCustomizations(show, slide, tint, &error); }) &&
              docs.findShow(show)->slides[0].customizations.backgroundColor ==
                  std::optional<std::string>("#123456"),
          "a customization applied through history is live");
    check(history.undo(&error) && docs.findShow(show)->slides[0].customizations.empty(),
          "undo puts the customization back where it was — absent");
    check(history.apply([&] { return docs.setCurrentTheme(""); }) &&
                  docs.currentThemeId().empty() && history.undo(&error) &&
                  docs.currentThemeId() == current,
          "undoing the current theme puts it back on");

    // Removing the current theme removes the reference: nothing wears a ghost.
    check(docs.removeTheme(current, &error) && docs.currentThemeId().empty(),
          "removing the current theme clears it rather than stranding it");

    // Silence below the floor: an id that resolves to no slide says nothing.
    const ResolvedAppearance nothing = docs.appearanceForSlide(show, "slide-gone");
    check(nothing.textColor.empty() && nothing.fontSize == 0.0 &&
              nothing.align.empty() && nothing.lineSpacing == kDefaultLineSpacing,
          "an unknown slide resolves to silence, which the scene compile fills");
}

// The themes plan amendment (2026-11-01): a theme carries the PRIMARY
// lyric box's rectangle and the chain resolves it exactly like every other
// property — the slide's placement customization, then the current theme,
// then the slide's theme, then the show default. Silence here is an empty
// optional rather than "" or 0, because a rectangle has no honest zero:
// with no layer speaking the slide draws the box it was authored with.
void exerciseThemePlacement() {
    std::string error;
    Documents docs;
    const Id show = docs.createShow("Placed");
    const Id slide = docs.appendSlide(show);
    const Id follower = docs.appendSlide(show);
    const Id showTheme = docs.createTheme("Show", "#111111", "#222222", "", "Show Serif",
                                          40.0, "left", 1.1, &error);
    const Id slideTheme = docs.createTheme("Slide", "#333333", "#444444", "", "Slide Serif",
                                           50.0, "right", 1.3, &error);
    const Id current = docs.createTheme("Current", "#555555", "#666666", "", "Current Sans",
                                        60.0, "center", 1.5, &error);
    check(!showTheme.empty() && !slideTheme.empty() && !current.empty() &&
              docs.setShowDefaultTheme(show, showTheme) &&
              docs.setSlideTheme(show, slide, slideTheme),
          "a show, two slides and three themes exist to carry placement");

    // A theme with no placement has no opinion; the model says so with an
    // empty optional, and no layer may read that as a zero-sized box.
    check(docs.findTheme(showTheme)->placement == std::nullopt &&
              docs.appearanceForSlide(show, slide).placement == std::nullopt &&
              docs.appearanceForSlide(show, follower).placement == std::nullopt,
          "with no placement anywhere every slide keeps its authored box");

    // The vetting is the authored box's own, applied to the theme's rect.
    BoxRect rect;
    rect.x = 240.0;
    rect.y = 300.0;
    rect.width = 1440.0;
    rect.height = 480.0;
    check(!docs.setThemePlacement("theme-gone", rect, &error) &&
              error.find("does not exist") != std::string::npos,
          "placing a theme that is not there is refused");
    BoxRect flat = rect;
    flat.width = 0.0;
    check(!docs.setThemePlacement(slideTheme, flat, &error) &&
              error.find("positive width") != std::string::npos &&
              docs.findTheme(slideTheme)->placement == std::nullopt,
          "a zero-width rectangle is refused and the theme keeps no placement");
    BoxRect off = rect;
    off.height = -12.0;
    check(!docs.setThemePlacement(slideTheme, off, &error),
          "a negative height is refused — there would be nothing to draw");
    BoxRect nan = rect;
    nan.x = std::nan("");
    check(!docs.setThemePlacement(slideTheme, nan, &error),
          "a NaN coordinate is refused, exactly as an authored box refuses one");
    check(docs.setThemePlacement(slideTheme, rect, &error) &&
              docs.findTheme(slideTheme)->placement == rect,
          "a real rectangle lands on the theme");

    // Negative x and y are legitimate: a box may hang off the canvas's top
    // left, and the reference editor's own default boxes do.
    BoxRect edge;
    edge.x = -60.0;
    edge.y = -60.0;
    edge.width = 100.0;
    edge.height = 100.0;
    check(docs.setThemePlacement(showTheme, edge, &error),
          "a rectangle hanging off the canvas is a placement, not an error");

    // The chain, layer by layer. The slide's own theme speaks first.
    ResolvedAppearance a = docs.appearanceForSlide(show, slide);
    check(a.placement == std::optional<BoxRect>(rect),
          "the slide's theme places its lyric box");
    a = docs.appearanceForSlide(show, follower);
    check(a.placement == std::optional<BoxRect>(edge),
          "a slide following the show takes the show default's placement");

    // A theme without placement is SILENT, not an answer: the fall
    // continues past it to the next layer that carries a rectangle.
    const Id mute = docs.createTheme("Mute", "#abcdef", "#000000", "", "Mute Serif", 44.0,
                                     "center", kDefaultLineSpacing, &error);
    check(!mute.empty() && docs.setSlideTheme(show, slide, mute, &error) &&
              docs.appearanceForSlide(show, slide).placement == std::optional<BoxRect>(edge),
          "a slide theme with no placement falls through to the show default");
    check(docs.setSlideTheme(show, slide, slideTheme, &error),
          "and putting the placing theme back on brings its rectangle back");

    // The current theme is worn over both, placement included.
    check(docs.setCurrentTheme(current), "the service wears a current theme");
    check(docs.appearanceForSlide(show, slide).placement == std::optional<BoxRect>(rect) &&
              docs.appearanceForSlide(show, follower).placement == std::optional<BoxRect>(edge),
          "a current theme with no placement changes nobody's geometry");
    BoxRect wide;
    wide.x = 100.0;
    wide.y = 700.0;
    wide.width = 1720.0;
    wide.height = 220.0;
    check(docs.setThemePlacement(current, wide, &error), "and then it learns a placement");
    a = docs.appearanceForSlide(show, slide);
    check(a.placement == std::optional<BoxRect>(wide),
          "the current theme outranks the slide theme and the show default");
    check(docs.appearanceForSlide(show, follower).placement == std::optional<BoxRect>(wide),
          "and it places the followers too");

    // A slide that wants its own spot customizes placement: one unit,
    // beating every layer, and the rest of the look still comes through.
    BoxRect own;
    own.x = 800.0;
    own.y = 100.0;
    own.width = 300.0;
    own.height = 200.0;
    SlideCustomization changed;
    changed.placement = own;
    check(docs.setSlideCustomizations(show, slide, changed, &error),
          "a slide customizes its placement as one field");
    a = docs.appearanceForSlide(show, slide);
    check(a.placement == std::optional<BoxRect>(own) && a.textColor == "#555555" &&
              a.fontSize == 60.0,
          "the slide keeps its own spot and the current theme's colours");
    check(docs.appearanceForSlide(show, follower).placement == std::optional<BoxRect>(wide),
          "the next slide still follows the theme — placement is per slide");

    // It is a field in every sense the other fields have.
    check(docs.findShow(show)->slides[0].customizations.hasField("placement") &&
              openshow::domain::isCustomizableField("placement"),
          "\"placement\" is a known customization field");
    check(!docs.findShow(show)->slides[0].customizations.hasField("x"),
          "and the rectangle is not seven revertable fields, it is one");
    SlideCustomization half;
    half.placement = BoxRect{};  // the default rect is 0x0: nothing to draw
    check(!docs.setSlideCustomizations(show, slide, half, &error) &&
              docs.findShow(show)->slides[0].customizations.placement ==
                  std::optional<BoxRect>(own),
          "a zero-sized customized rectangle is refused and the old one survives");

    // Reverting one field, and restoring the whole slide, both hand the
    // box back to the themes.
    SlideCustomization reverted = docs.findShow(show)->slides[0].customizations;
    reverted.clearField("placement");
    check(!reverted.placement.has_value() && reverted.empty(),
          "clearing the only customized field leaves an empty set");
    check(docs.setSlideCustomizations(show, slide, reverted, &error) &&
              docs.appearanceForSlide(show, slide).placement == std::optional<BoxRect>(wide),
          "reverting the placement sends the slide back to the theme's rectangle");
    changed.placement = own;
    check(docs.setSlideCustomizations(show, slide, changed, &error) &&
              docs.removeSlideCustomizations(show, slide, &error) &&
              docs.appearanceForSlide(show, slide).placement == std::optional<BoxRect>(wide),
          "removing the whole set restores the themed placement too");
    check(docs.setSlideCustomizations(show, slide, changed, &error) &&
              docs.setSlideCustomizations(show, follower, changed, &error) &&
              docs.removeShowCustomizations(show, &error) &&
              docs.findShow(show)->slides[0].customizations.empty() &&
              docs.findShow(show)->slides[1].customizations.empty(),
          "restoring the show clears placement on every slide in one call");

    // Taking a theme's placement off stops it placing, colours and all.
    check(docs.clearThemePlacement(current, &error) &&
              docs.findTheme(current)->placement == std::nullopt &&
              docs.findTheme(current)->textColor == "#555555" &&
              docs.appearanceForSlide(show, slide).placement == std::optional<BoxRect>(rect),
          "a theme without placement stops placing and the layer below speaks");
    check(!docs.clearThemePlacement(current, &error) &&
              error.find("no placement to remove") != std::string::npos,
          "removing a placement that was never there is refused — a change that "
          "changed nothing must not exist in the history");
    check(docs.setCurrentTheme("") && docs.setShowDefaultTheme(show, "") &&
              docs.setSlideTheme(show, slide, "") &&
              docs.appearanceForSlide(show, slide).placement == std::nullopt,
          "with every placing layer gone the slide keeps its authored box");

    // Undo wraps it like everything else: History snapshots whole states.
    History history(&docs);
    check(history.apply([&] { return docs.setThemePlacement(slideTheme, wide, &error); }) &&
              docs.findTheme(slideTheme)->placement == std::optional<BoxRect>(wide),
          "a placement set through history is live");
    check(history.undo(&error) && docs.findTheme(slideTheme)->placement == rect,
          "undo puts the theme's own rectangle back");
    check(history.apply([&] { return docs.clearThemePlacement(slideTheme, &error); }) &&
              docs.findTheme(slideTheme)->placement == std::nullopt &&
              history.undo(&error) && docs.findTheme(slideTheme)->placement == rect,
          "undoing the removal puts the placement back");

    // Removing a theme that placed slides takes the placement with it:
    // nothing wears a ghost rectangle.
    check(docs.setSlideTheme(show, slide, slideTheme, &error) &&
              docs.appearanceForSlide(show, slide).placement == std::optional<BoxRect>(rect) &&
              docs.removeTheme(slideTheme, &error) && docs.findTheme(slideTheme) == nullptr,
          "the slide wears the placing theme once more, and that theme is deleted");
    check(docs.appearanceForSlide(show, slide).placement == std::nullopt,
          "and its slide keeps its authored box rather than a ghost");
}

// Task TF.1 (text-flow plan): a slide that names a text context layers that
// context's typography and box BETWEEN the slide's own customizations and
// the theme chain. The first theme in chain order carrying the name supplies
// it — wholesale, not merged across themes — and a context answers only how
// text is SET: colours still come from the themes, and there is no line
// count anywhere, because how many lines fit is what the worker's
// measurement decides. A slide with no name, or a name no theme carries,
// resolves exactly as it did before the name existed.
void exerciseTextContextChain() {
    std::string error;
    Documents docs;
    const Id show = docs.createShow("Served");
    const Id reading = docs.appendSlide(show);   // names scripture
    const Id columned = docs.appendSlide(show);  // names column (inset only)
    const Id whisper = docs.appendSlide(show);   // names whisper (size 0 = renderer default)
    const Id song = docs.appendSlide(show);      // names nothing
    const Id ghost = docs.appendSlide(show);     // names a context no theme carries yet
    const Id showTheme = docs.createTheme("Show", "#111111", "#222222", "", "Show Serif",
                                          40.0, "left", 1.1, &error);
    const Id current = docs.createTheme("Current", "#555555", "#666666", "", "Current Sans",
                                        60.0, "center", 1.5, &error);
    openshow::domain::BoxRect themeRect;
    themeRect.x = 160.0;
    themeRect.y = 90.0;
    themeRect.width = 1600.0;
    themeRect.height = 900.0;
    openshow::domain::TextContext loud;
    loud.name = "scripture";
    loud.fontSize = 28.0;
    loud.lineSpacing = 1.15;
    loud.align = "right";
    openshow::domain::BoxRect lectern;
    lectern.x = 240.0;
    lectern.y = 120.0;
    lectern.width = 1440.0;
    lectern.height = 840.5;
    loud.box = lectern;
    openshow::domain::TextContext column;
    column.name = "column";
    column.inset = 80.0;
    openshow::domain::TextContext tiny;
    tiny.name = "whisper";
    tiny.fontSize = 0.0;  // present means answer: 0 is "renderer default", a thing operators mean
    // The same name on a lower theme, quieter: the chain finds the CURRENT
    // theme's context and never merges the two — what the current one leaves
    // unsaid falls through to the themes, not to another theme's context.
    openshow::domain::TextContext quiet;
    quiet.name = "scripture";
    quiet.fontSize = 20.0;
    check(!showTheme.empty() && !current.empty() &&
              docs.setThemeContexts(current, {loud, column, tiny}, &error) &&
              docs.setThemeContexts(showTheme, {quiet}, &error),
          "a placing theme carries three contexts and the show theme a quieter namesake");
    check(docs.setSlideTextContext(show, reading, "scripture", &error) &&
              docs.setSlideTextContext(show, columned, "column", &error) &&
              docs.setSlideTextContext(show, whisper, "whisper", &error) &&
              docs.setSlideTextContext(show, ghost, "karaoke", &error) &&
              docs.setShowDefaultTheme(show, showTheme, &error) &&
              docs.setThemePlacement(current, themeRect, &error) &&
              docs.setCurrentTheme(current, &error),
          "slides name their contexts and the themes are put in chain");

    const ResolvedAppearance a = docs.appearanceForSlide(show, reading);
    check(a.fontSize == 28.0 && a.lineSpacing == 1.15 && a.align == "right",
          "the context sets size, spacing and alignment over both themes");
    check(a.textColor == "#555555" && a.backgroundColor.empty() &&
              a.fontFamily == "Current Sans",
          "the look still comes from the themes — a context carries no colour, and its "
          "silence on a family falls through to its own theme, not a namesake's");
    check(a.placement == std::optional<BoxRect>(lectern),
          "the context's own box answers, outranking the themed placement");
    const ResolvedAppearance plain = docs.appearanceForSlide(show, song);
    check(plain.fontSize == 60.0 && plain.align == "center" && plain.lineSpacing == 1.5 &&
              plain.placement == std::optional<BoxRect>(themeRect),
          "the unnamed song slide of the same theme is untouched by every context");
    // Naming WHICH context layered is the resolver's own answer (task
    // TF.4): the pages' plan sentences read it instead of re-walking.
    check(a.contextName == "scripture" && plain.contextName.empty(),
          "the resolution names the context it layered and stays silent for the slide it did not");
    const ResolvedAppearance haunted = docs.appearanceForSlide(show, ghost);
    check(haunted.contextName.empty(),
          "a name no theme carries is not an answer anyone may print");
    check(haunted.fontSize == plain.fontSize && haunted.align == plain.align &&
              haunted.lineSpacing == plain.lineSpacing &&
              haunted.placement == plain.placement,
          "a name no theme carries is no layer at all — the slide resolves as before");
    const ResolvedAppearance narrowed = docs.appearanceForSlide(show, columned);
    check(narrowed.fontSize == 60.0 && narrowed.align == "center" &&
              narrowed.lineSpacing == 1.5,
          "an inset-only context says nothing about typography");
    check(narrowed.placement ==
              std::optional<BoxRect>(BoxRect{240.0, 170.0, 1440.0, 740.0}),
          "its inset shrinks the themed placement on every side");
    const ResolvedAppearance hushed = docs.appearanceForSlide(show, whisper);
    check(hushed.fontSize == 0.0,
          "a context size of 0 is the answer \"renderer default\", not silence to fall through");

    // The slide's own customization still outranks the context, field by
    // field, exactly as it outranks the themes.
    openshow::domain::BoxRect own;
    own.x = 10.0;
    own.y = 20.0;
    own.width = 300.0;
    own.height = 200.0;
    SlideCustomization changed;
    changed.fontSize = 75.0;
    check(docs.setSlideCustomizations(show, reading, changed, &error),
          "the reading slide customizes its size");
    ResolvedAppearance tuned = docs.appearanceForSlide(show, reading);
    check(tuned.fontSize == 75.0 && tuned.align == "right" && tuned.lineSpacing == 1.15 &&
              tuned.placement == std::optional<BoxRect>(lectern),
          "the customization answers size while the context keeps answering the rest");
    changed.placement = own;
    check(docs.setSlideCustomizations(show, reading, changed, &error),
          "and it customizes its placement too");
    tuned = docs.appearanceForSlide(show, reading);
    check(tuned.placement == std::optional<BoxRect>(own),
          "the slide's own rectangle outranks the context's box");
    check(docs.setSlideCustomizations(show, reading, SlideCustomization{}, &error) &&
              docs.appearanceForSlide(show, reading).fontSize == 28.0 &&
              docs.appearanceForSlide(show, reading).placement ==
                  std::optional<BoxRect>(lectern),
          "reverting the customization hands the answers back to the context");

    // With no theme placing anything, the inset shrinks the box the slide
    // would otherwise land on — its own authored primary item.
    check(docs.clearThemePlacement(current, &error) &&
              docs.appearanceForSlide(show, song).placement == std::nullopt,
          "the theme stops placing and the plain slide returns to its authored box");
    const SlideItem* authored = docs.findShow(show)->slides[0].firstItem();
    check(authored != nullptr, "the slide has an authored box to shrink");
    const ResolvedAppearance still = docs.appearanceForSlide(show, columned);
    check(still.placement ==
              std::optional<BoxRect>(BoxRect{authored->x + 80.0, authored->y + 80.0,
                                             authored->width - 160.0,
                                             authored->height - 160.0}),
          "the inset now shrinks the slide's own authored box");

    // What the loader refuses, the edit door refuses too — a theme this
    // verb accepted must never be a theme the next save refuses.
    check(!docs.setThemeContexts(current, {loud, loud}, &error) &&
              error.find("twice") != std::string::npos,
          "naming one context twice is refused by the mutator");
    openshow::domain::TextContext greedy;
    greedy.name = "greedy";
    greedy.box = lectern;
    greedy.inset = 40.0;
    check(!docs.setThemeContexts(current, {greedy}, &error) &&
              error.find("both a box and an inset") != std::string::npos,
          "box and inset together are refused by the mutator too");
    check(!docs.setSlideTextContext(show, "slide-gone", "scripture", &error),
          "a slide that is not there is refused like everywhere else");
    check(docs.setSlideTextContext(show, song, "scripture", &error) &&
              docs.appearanceForSlide(show, song).fontSize == 28.0,
          "naming a context moves the plain slide onto it");
    check(docs.setSlideTextContext(show, song, "", &error) &&
              docs.appearanceForSlide(show, song).fontSize == 60.0,
          "and the empty name takes it off again");
}

// TS.1: the text-style verbs, their bands, and the resolver's walk over the
// style block. The contract lives in documents.h; what this exercises is
// that the doors QUOTE it — the theme verb, the customization door, and the
// chain's first-carrier rule with `enabled:false` honoured as an answer.
void exerciseTextStyle() {
    std::string error;
    Documents docs;
    const Id show = docs.createShow("Styled");
    const Id plain = docs.appendSlide(show);
    const Id current = docs.createTheme("Current", "#555555", "#666666", "", "Current Sans",
                                        60.0, "center", 1.5, &error);
    const Id base = docs.createTheme("Base", "#111111", "#222222", "", "Base Serif",
                                     40.0, "left", 1.1, &error);
    check(!show.empty() && !plain.empty() && !current.empty() && !base.empty() &&
              docs.setShowDefaultTheme(show, base, &error) &&
              docs.setCurrentTheme(current, &error),
          "a show with a base default theme and a current theme worn over it exists");

    // An unstyled chain resolves to the CONTRACT's concrete answers — the
    // numbers describe what every renderer has always drawn, so an old
    // document keeps its exact pixels without carrying a single new key.
    ResolvedAppearance look = docs.appearanceForSlide(show, plain);
    check(look.fontWeight == 400 && look.fontStyle == "normal" && look.fitMode == "shrink" &&
              look.minFontSize == 12.0 && !look.stroke.enabled && !look.shadow.enabled &&
              look.stroke.color == "#000000" && look.stroke.width == 2.0 &&
              look.shadow.opacity == 0.65 && look.shadow.offsetX == 2.0 &&
              look.shadow.blurSigma == 4.0,
          "the style-less chain lands on the contract's defaults to the number");

    // --- the theme verb: whole-set replacement, in setThemeContexts' manner ---
    openshow::domain::TextStroke outline;
    outline.enabled = true;
    outline.color = "#ffcc00";
    outline.width = 3.0;
    openshow::domain::ThemeTextStyle style;
    style.fontWeight = 700;
    style.stroke = outline;
    check(docs.setThemeTextStyle(current, style, &error) &&
              docs.findTheme(current)->fontWeight == 700 &&
              docs.findTheme(current)->stroke == outline &&
              !docs.findTheme(current)->fontStyle.has_value(),
          "the verb sets the fields it was given and leaves the rest of the theme silent");
    openshow::domain::ThemeTextStyle quieter;
    quieter.fontWeight = 500;
    check(docs.setThemeTextStyle(current, quieter, &error) &&
              docs.findTheme(current)->fontWeight == 500 &&
              !docs.findTheme(current)->stroke.has_value(),
          "replacement is WHOLE: a key the new set does not give stops being the theme's "
          "answer, exactly like applyThemeContext");
    check(docs.setThemeTextStyle(current, openshow::domain::ThemeTextStyle{}, &error) &&
              !docs.findTheme(current)->fontWeight.has_value(),
          "the empty set silences the theme entirely — back to carrying no style opinion");

    // --- the bands the verbs refuse, each naming its own ---
    const auto refusedStyle = [&](const openshow::domain::ThemeTextStyle& candidate,
                                  const std::string& expectIn, const char* what) {
        error.clear();
        const bool refused = !docs.setThemeTextStyle(current, candidate, &error);
        if (!check(refused && error.find(expectIn) != std::string::npos, what)) {
            std::fprintf(stderr, "   wanted \"%s\", verb %s: %s\n", expectIn.c_str(),
                         refused ? "said" : "ACCEPTED", error.c_str());
        }
    };
    openshow::domain::ThemeTextStyle bad;
    bad.fontWeight = 99;
    refusedStyle(bad, "100..900", "a weight below the CSS scale is refused naming the band");
    bad.fontWeight = 901;
    refusedStyle(bad, "100..900", "and one above it");
    bad = openshow::domain::ThemeTextStyle{};
    bad.fontStyle = "oblique";
    refusedStyle(bad, "\"normal\" or \"italic\"",
                 "a made-up font style is refused naming its two words");
    bad.fontStyle = "";
    refusedStyle(bad, "\"normal\" or \"italic\"",
                 "and so is the empty string: silence is absence, not a word");
    bad = openshow::domain::ThemeTextStyle{};
    bad.fitMode = "auto";
    refusedStyle(bad, "\"shrink\" or \"fixed\"", "a made-up fit is refused naming its two words");
    bad = openshow::domain::ThemeTextStyle{};
    bad.minFontSize = 0.0;
    refusedStyle(bad, "1..4096", "a minimum of 0 is refused: invisible text is a clear, not a fit");
    bad.minFontSize = 4097.0;
    refusedStyle(bad, "1..4096", "and so is one past the ceiling");
    bad = openshow::domain::ThemeTextStyle{};
    openshow::domain::TextStroke fat;
    fat.enabled = true;
    fat.width = 33.0;
    bad.stroke = fat;
    refusedStyle(bad, "0..32", "a 33-pixel stroke is refused naming the band");
    bad.stroke->width = -1.0;
    refusedStyle(bad, "0..32", "and a negative one with it");
    bad.stroke->width = 2.0;
    bad.stroke->color = "#12345";
    refusedStyle(bad, "is not #rgb or #rrggbb",
                 "an effect colour faces the same colour rule as every other");
    bad = openshow::domain::ThemeTextStyle{};
    openshow::domain::TextShadow cast;
    cast.enabled = true;
    cast.opacity = 1.5;
    bad.shadow = cast;
    refusedStyle(bad, "0..1", "a 150% shadow is refused naming its band");
    bad.shadow->opacity = 0.5;
    bad.shadow->offsetX = 257.0;
    refusedStyle(bad, "-256..256", "and an offset a pixel past a quarter of the canvas");
    bad.shadow->offsetX = 2.0;
    bad.shadow->blurSigma = 65.0;
    refusedStyle(bad, "0..64", "and a blur beyond the ceiling");
    check(!docs.findTheme(current)->fontWeight.has_value() &&
              !docs.findTheme(current)->stroke.has_value() &&
              !docs.findTheme(current)->shadow.has_value(),
          "a refused verb leaves the theme exactly as it was — no half style survives");

    // The widest legal block is ACCEPTED: every band edge and a disabled
    // record with a look of its own are data, not errors.
    openshow::domain::ThemeTextStyle widest;
    widest.fontWeight = 900;
    widest.fontStyle = "italic";
    widest.fitMode = "fixed";
    widest.minFontSize = 4096.0;
    openshow::domain::TextStroke hairline;  // enabled, width 0: as if outlined, no ink
    hairline.enabled = true;
    hairline.color = "#abc";
    hairline.width = 0.0;
    widest.stroke = hairline;
    openshow::domain::TextShadow extreme;
    extreme.enabled = false;  // a refusal that still carries its full look
    extreme.color = "#ffffff";
    extreme.opacity = 1.0;
    extreme.offsetX = -256.0;
    extreme.offsetY = 256.0;
    extreme.blurSigma = 64.0;
    widest.shadow = extreme;
    check(docs.setThemeTextStyle(current, widest, &error) &&
              docs.findTheme(current)->fontWeight == 900 &&
              docs.findTheme(current)->stroke == hairline &&
              docs.findTheme(current)->shadow == extreme,
          "every band edge and a disabled record with a full look are accepted as authored");
    widest.fontWeight = 100;
    widest.minFontSize = 1.0;
    check(docs.setThemeTextStyle(current, widest, &error),
          "and so is the bottom of each band");

    // --- the resolver's walk: customization, then first CARRIER, then default ---
    openshow::domain::ThemeTextStyle baseStyle;
    baseStyle.fontWeight = 300;
    openshow::domain::TextShadow flat;  // base's explicit "no shadow here"
    flat.enabled = false;
    baseStyle.shadow = flat;
    openshow::domain::ThemeTextStyle currentStyle;
    currentStyle.fontStyle = "italic";
    currentStyle.stroke = outline;
    check(docs.setThemeTextStyle(current, currentStyle, &error) &&
              docs.setThemeTextStyle(base, baseStyle, &error),
          "the current theme carries slant and stroke; the base carries weight and a refusal");
    look = docs.appearanceForSlide(show, plain);
    check(look.fontWeight == 300 && look.fontStyle == "italic" && look.fitMode == "shrink" &&
              look.minFontSize == 12.0 && look.stroke == outline && look.shadow == flat,
          "each field falls to the first theme that carries it, and a carried enabled:false "
          "shadow resolves as the explicit refusal it is — not as silence");

    // The slide's own set outranks both, field by field, including a
    // customized refusal over a themed outline.
    openshow::domain::TextStroke off;
    off.enabled = false;
    off.color = "#00ff00";
    off.width = 0.0;
    SlideCustomization changed;
    changed.fontWeight = 900;
    changed.stroke = off;
    check(docs.setSlideCustomizations(show, plain, changed, &error),
          "the slide customizes its weight and switches the themed outline off");
    look = docs.appearanceForSlide(show, plain);
    check(look.fontWeight == 900 && look.stroke == off && look.fontStyle == "italic" &&
              look.shadow == flat,
          "the slide answers its two fields while the themes keep answering the rest");
    const SlideCustomization& spoken = docs.findShow(show)->slides[0].customizations;
    check(spoken.hasField("fontWeight") && spoken.hasField("stroke") &&
              !spoken.hasField("fontStyle") && !spoken.hasField("shadow"),
          "the sparse set says WHICH fields the slide spoke, style names included");

    // What the theme door refuses, the customization door refuses too: a
    // slide cannot style itself into an appearance no theme could carry.
    SlideCustomization rogue;
    rogue.fontWeight = 45;
    check(!docs.setSlideCustomizations(show, plain, rogue, &error) &&
              error.find("100..900") != std::string::npos,
          "a customized weight off the scale is refused by the customization door");
    rogue = SlideCustomization{};
    rogue.shadow = extreme;
    rogue.shadow->blurSigma = 100.0;
    check(!docs.setSlideCustomizations(show, plain, rogue, &error) &&
              error.find("0..64") != std::string::npos,
          "and so is a shadow record beyond the blur ceiling");
    check(docs.findShow(show)->slides[0].customizations.fontWeight == 900,
          "the refused customization left the slide's own set untouched");

    // Reverting a style field hands it back: clearField speaks the names,
    // and the whole revert returns the slide to the chain's answer.
    SlideCustomization minusStroke = changed;
    minusStroke.clearField("stroke");
    check(docs.setSlideCustomizations(show, plain, minusStroke, &error) &&
              docs.appearanceForSlide(show, plain).stroke == outline,
          "clearing the customized stroke hands the outline back to the current theme");
    check(docs.setSlideCustomizations(show, plain, SlideCustomization{}, &error) &&
              docs.appearanceForSlide(show, plain).fontWeight == 300 &&
              docs.appearanceForSlide(show, plain).stroke == outline,
          "the full revert returns every style field to the themes");
}

// TS.1's storage half: a styled service round-trips through the file to the
// number, silence is written as silence, and the format-era guards refuse
// every way a pre-8 file can carry a meaning it never defined.
void exerciseTextStyleStorage(const fs::path& tempRoot) {
    std::string error;
    int seq = 0;
    const auto refusesJson = [&](const std::string& manifest, const std::string& expectIn,
                                 const char* what) {
        const fs::path dir = tempRoot / ("style-refuse-" + std::to_string(++seq));
        std::filesystem::create_directories(dir);
        writeFile(dir / "service.json", manifest);
        Documents attempt;
        error.clear();
        const bool refused = openshow::domain::loadService(&attempt, dir, &error);
        if (!check(!refused && error.find(expectIn) != std::string::npos, what)) {
            std::fprintf(stderr, "   wanted \"%s\", load %s: %s\n", expectIn.c_str(),
                         refused ? "succeeded" : "said", error.c_str());
        }
    };

    // --- a styled service: written, read back, and written again identically ---
    {
        const fs::path dir = tempRoot / "style-round";
        Documents docs;
        const Id show = docs.createShow("Lit");
        const Id styled = docs.appendSlide(show);
        const Id plain = docs.appendSlide(show);
        const Id theme = docs.createTheme("Bold", "#ffffff", "#000000", "", "Serif", 64.0,
                                          "center", 1.5, &error);
        openshow::domain::TextStroke outline;
        outline.enabled = true;
        outline.color = "#ff0000";
        outline.width = 7.5;
        openshow::domain::ThemeTextStyle style;
        style.fontWeight = 800;
        style.fontStyle = "italic";
        style.fitMode = "fixed";
        style.minFontSize = 24.5;
        style.stroke = outline;
        openshow::domain::TextShadow cast;
        cast.enabled = true;
        cast.color = "#0000ff";
        cast.opacity = 0.25;
        cast.offsetX = -30.0;
        cast.offsetY = 12.0;
        cast.blurSigma = 0.0;
        style.shadow = cast;
        SlideCustomization changed;
        changed.fontWeight = 100;
        openshow::domain::TextStroke off;  // the slide's refusal, whole, with its own look
        off.enabled = false;
        off.color = "#00ff00";
        off.width = 0.0;
        changed.stroke = off;
        check(!theme.empty() && !styled.empty() &&
                  docs.setThemeTextStyle(theme, style, &error) &&
                  docs.setSlideCustomizations(show, styled, changed, &error) &&
                  openshow::domain::saveService(docs, dir, &error),
              "a theme with the whole style block and a slide with two style answers save");
        const std::string manifest = readFile(dir / "service.json");
        // Keyed by NAME, not by digits: this boost spells 24.5 as 2.45E1,
        // so a number-exact string check would test the JSON library's
        // formatter rather than this codec's discipline. The values get
        // pinned field-for-field by the round trip right below.
        check(manifest.find("\"fontWeight\":800") != std::string::npos &&
                  manifest.find("\"fitMode\":\"fixed\"") != std::string::npos &&
                  manifest.find("\"minFontSize\":") != std::string::npos &&
                  manifest.find("\"blurSigma\":") != std::string::npos &&
                  manifest.find("\"offsetX\":") != std::string::npos,
              "the theme's style is written BY NAME, the zero blur included — "
              "leaving an authored answer to an absent key is the rule this project refuses");
        Documents back;
        check(openshow::domain::loadService(&back, dir, &error) && sameService(back, docs) &&
                  back.findTheme(theme)->fontWeight == 800 &&
                  back.findTheme(theme)->fontStyle == "italic" &&
                  back.findTheme(theme)->fitMode == "fixed" &&
                  back.findTheme(theme)->minFontSize == 24.5 &&
                  back.findTheme(theme)->stroke == outline &&
                  back.findTheme(theme)->shadow == cast,
              "the styled theme reads back field for field and the whole service compares");
        const SlideCustomization& spoken = back.findShow(show)->slides[0].customizations;
        check(spoken.fontWeight == 100 && spoken.stroke == off && !spoken.shadow.has_value(),
              "the slide's weight and its whole disabled stroke survive, and the fields it "
              "never spoke stay unspoken");
        check(openshow::domain::saveService(back, tempRoot / "style-again", &error) &&
                  readFile(tempRoot / "style-again" / "service.json") == manifest,
              "re-saving a styled service writes a byte-identical manifest");
    }

    // Silence is written as silence: a service nobody styled gains not one
    // of the new keys, so old documents keep their shape through this build.
    {
        Documents docs;
        const Id show = docs.createShow("Plain");
        docs.appendSlide(show);
        const Id theme = docs.createTheme("Quiet", "#ffffff", "#000000", "", "Serif", 64.0,
                                          "center", 1.5, &error);
        const fs::path dir = tempRoot / "style-silent";
        check(!theme.empty() && openshow::domain::saveService(docs, dir, &error),
              "a plain service saves");
        const std::string manifest = readFile(dir / "service.json");
        for (const char* key : {"\"fontWeight\"", "\"fontStyle\"", "\"fitMode\"",
                                "\"minFontSize\"", "\"stroke\"", "\"shadow\""}) {
            check(manifest.find(key) == std::string::npos,
                  (std::string("a theme that carries no style writes no ") + key)
                      .c_str());
        }
    }

    // --- the era guards: a style key in a pre-8 file is somebody else's meaning ---
    {
        // One theme fixture, spelled complete, waiting for exactly one
        // style key to be appended before its closing brace.
        const std::string themeStart =
            R"({"id":"theme1","name":"House","textColor":"#ffffff","backgroundColor":"#000000",)"
            R"("backgroundGradient":"","fontFamily":"","fontSize":64,"align":"center",)"
            R"("lineSpacing":1.5)";
        const std::string themesTail = R"(],"assets":[],"playlists":[]})";
        const auto themed = [&](unsigned long long format, const std::string& extraStyle) {
            return std::string(R"({"format":)") + std::to_string(format) +
                   R"(,"nextOrdinal":99,"currentThemeId":"","shows":[],"themes":[)" +
                   themeStart + extraStyle + "}" + themesTail;
        };
        refusesJson(themed(7, R"(,"fontWeight":700)"),
                    "carries \"fontWeight\" in a format 7 file",
                    "a hand-bumped theme weight in a format-7 file is refused naming the era");
        refusesJson(themed(7, R"(,"stroke":{"enabled":true,"color":"#000000","width":2})"),
                    "carries \"stroke\" in a format 7 file",
                    "and a stroke record in that same era is refused with it");
        // The customization object exists since format 6 — but its STYLE
        // fields are format-8 meanings one level deeper, and the guard
        // reaches them there.
        const std::string slideStart =
            R"({"id":"slide1","group":"","items":[],"notes":"","themeId":"",)"
            R"("fit":"cover","backgroundAssetId":"","customizations":)";
        const auto slideWith = [&](unsigned long long format, const std::string& customJson) {
            return std::string(R"({"format":)") + std::to_string(format) +
                   R"(,"nextOrdinal":99,"currentThemeId":"","shows":[{"id":"show1",)"
                   R"("name":"Old","slides":[)" +
                   slideStart + customJson +
                   R"(}]}],"themes":[],"assets":[],"playlists":[]})";
        };
        refusesJson(slideWith(6, R"({"stroke":{"enabled":true,"color":"#000000","width":2}})"),
                    "customization 'stroke' in a format 6 file",
                    "a stroke inside a format-6 slide's customizations is refused naming the era");
        refusesJson(slideWith(7, R"({"minFontSize":20})"),
                    "customization 'minFontSize' in a format 7 file",
                    "and a minimum size inside a format-7 one with it");
        // Format 8 is where these keys mean something — but MEANING is the
        // domain's sentence: a file in the right era can still carry a
        // value no renderer could honour, and it is refused for that.
        refusesJson(themed(8, R"(,"fontWeight":901)"),
                    "carries text style the renderer cannot honour",
                    "a format-8 theme with an off-scale weight is refused by the domain's "
                    "own sentence, not just the reader's shape check");
        refusesJson(themed(8, R"(,"stroke":{"enabled":true})"),
                    "whole record or nothing",
                    "half a stroke record is refused: an effect travels complete or not at all");
        refusesJson(themed(8, R"(,"shadow":{"enabled":true,"color":"#000000","opacity":0.5,)"
                               R"("offsetX":2,"offsetY":2,"blurSigma":4,"feather":8})"),
                    "shadow has no field 'feather'",
                    "and a shadow member the record never had is refused naming itself");
        refusesJson(slideWith(8, R"({"fontWeight":99})"),
                    "carries a customization the renderer cannot honour",
                    "an off-scale customized weight in a format-8 file meets the same "
                    "sentence the verb quotes");
        refusesJson(slideWith(8, R"({"fitMode":"auto"})"),
                    "\"shrink\" or \"fixed\"",
                    "and a made-up fit name with it");
    }
}

// --- task TR.1: the transition contract, in the model -----------------------
//
// One question — "how does the audience get to this slide?" — with exactly one
// answer at runtime, found by walking named seats in a fixed order. These
// fixtures pin the three things that make that true: the band every door
// quotes, the priority the walk follows, and the fact that silence (an absent
// override) is different from an answer that happens to look like the default.
void exerciseTransition() {
    using openshow::domain::TransitionKind;
    using openshow::domain::TransitionOptions;
    std::string error;

    // --- the band, and what the gate asks about it --------------------------
    {
        const TransitionOptions& quiet = TransitionOptions::defaults();
        check(quiet.kind == TransitionKind::kFade && quiet.textDurationMs == 500 &&
                  quiet.mediaDurationMs == 800 && quiet.isDefault() &&
                  quiet.durationsAreDefault(),
              "the contract's defaults are the 500 ms words / 800 ms media crossfade "
              "every present before TR.1 performed");

        TransitionOptions cut;
        cut.kind = TransitionKind::kCut;
        check(cut.durationsAreDefault() && !cut.isDefault() &&
                  openshow::domain::Documents::validTransitionOptions(cut, &error),
              "a Cut with default lengths is NOT a default record but IS legacy-expressible: "
              "scene.present has spelled \"cut\" since task 3.2, so the gate lets it travel");

        TransitionOptions slower;
        slower.textDurationMs = 501;
        check(!slower.durationsAreDefault(),
              "one moved millisecond is a value a pre-TR.1 worker has never met");

        const auto refusedBand = [&](const TransitionOptions& candidate,
                                     const std::string& expectIn, const char* what) {
            error.clear();
            const bool rejected = !openshow::domain::Documents::validTransitionOptions(
                candidate, &error);
            if (!check(rejected && error.find(expectIn) != std::string::npos, what)) {
                std::fprintf(stderr, "   wanted \"%s\", sentence said: %s\n", expectIn.c_str(),
                             error.c_str());
            }
        };
        for (int tooLong : {10001, 2147483647}) {
            TransitionOptions longWay;
            longWay.textDurationMs = tooLong;
            refusedBand(longWay, "words duration",
                        "a words duration above the band is refused naming the track");
            TransitionOptions longMedia;
            longMedia.mediaDurationMs = tooLong;
            refusedBand(longMedia, "background duration",
                        "and a background duration above it naming the other");
        }
        for (int negative : {-1, -2000}) {
            TransitionOptions backwards;
            backwards.mediaDurationMs = negative;
            refusedBand(backwards, "background duration",
                        "a negative duration is refused rather than clamped to zero — "
                        "an operator who typed -1 did not ask for an instant cut");
        }
        // The ends of the band are IN: 0 means "this track lands immediately",
        // and 10000 is a long fade somebody has a use for.
        TransitionOptions bothEnds;
        bothEnds.textDurationMs = 0;
        bothEnds.mediaDurationMs = openshow::domain::kMaxTransitionDurationMs;
        check(openshow::domain::Documents::validTransitionOptions(bothEnds, &error),
              "0 ms on one track and 10 s on the other are both inside the band");
        check(openshow::domain::transitionKindName(TransitionKind::kFade) == std::string("fade") &&
                  openshow::domain::transitionKindName(TransitionKind::kCut) == std::string("cut"),
              "the two kinds are spelled \"fade\" and \"cut\" — the names the wire and the "
              "service file share");
        TransitionKind named = TransitionKind::kCut;
        check(!openshow::domain::transitionKindFromName("dissolve", &named) &&
                  named == TransitionKind::kCut,
              "an unknown kind name is refused and leaves the value it was handed alone");
    }

    // --- the walk: context, then first theme in chain order, then service ---
    Documents docs;
    const Id show = docs.createShow("Moving");
    const Id plain = docs.appendSlide(show);
    const Id scripture = docs.appendSlide(show);
    const Id current = docs.createTheme("Current", "#555555", "#666666", "", "Current Sans",
                                        60.0, "center", 1.5, &error);
    const Id base = docs.createTheme("Base", "#111111", "#222222", "", "Base Serif", 40.0,
                                     "left", 1.1, &error);
    const auto options = [](TransitionKind kind, int textMs, int mediaMs) {
        TransitionOptions out;
        out.kind = kind;
        out.textDurationMs = textMs;
        out.mediaDurationMs = mediaMs;
        return out;
    };
    check(!show.empty() && !plain.empty() && !scripture.empty() && !current.empty() &&
              !base.empty() && docs.setShowDefaultTheme(show, base, &error) &&
              docs.setCurrentTheme(current, &error) &&
              docs.setSlideTextContext(show, scripture, "scripture", &error),
          "a show with a current theme worn over a show default, and a slide set in "
          "the scripture context, exists");

    // Nothing anywhere has said anything: the service's own concrete answer.
    check(docs.resolveTransitionForSlide(show, plain) == TransitionOptions::defaults() &&
              docs.serviceDefaultTransition() == TransitionOptions::defaults() &&
              !docs.findTheme(current)->transition.has_value() &&
              !docs.findTheme(base)->transition.has_value(),
          "a service nobody configured resolves every slide to the contract's 500 / 800 "
          "and carries no override key anywhere");

    // The service default is a value, not a maybe, and it moves the answer for
    // every slide no layer speaks for.
    const TransitionOptions service = options(TransitionKind::kCut, 0, 0);
    check(docs.setServiceDefaultTransition(service, &error) &&
              docs.resolveTransitionForSlide(show, plain) == service,
          "setting the service default makes every unclaimed slide cut");

    // A theme in the chain answers for the slides wearing it, and the first
    // theme in chain order wins — here the current theme, worn over the
    // show's default.
    const TransitionOptions brisk = options(TransitionKind::kFade, 200, 300);
    const TransitionOptions house = options(TransitionKind::kCut, 40, 60);
    check(docs.setThemeTransition(current, brisk, &error) &&
              docs.setThemeTransition(base, house, &error) &&
                  docs.resolveTransitionForSlide(show, plain) == brisk,
          "the first theme in chain order answers, and an explicit Cut under it is "
          "not consulted — a decision is not an absence");

    // Silencing the nearer theme lets the walk fall through to the NEXT named
    // layer, which is not the service: absence and an answer that merely LOOKS
    // like a default are different things, three seats deep.
    check(docs.setThemeTransition(current, std::nullopt, &error) &&
              docs.resolveTransitionForSlide(show, plain) == house,
          "taking the nearer theme's override off is a real inherit: the walk reaches "
          "the show default's record, not the service's");

    // The context speaks for the slide it set, ahead of every theme.
    const TransitionOptions slow = options(TransitionKind::kFade, 1000, 1200);
    openshow::domain::TextContext verse;
    verse.name = "scripture";
    verse.transition = slow;
    check(docs.setThemeContexts(base, {verse}, &error) &&
              docs.resolveTransitionForSlide(show, scripture) == slow &&
                  docs.resolveTransitionForSlide(show, plain) == house,
          "a context's override answers for the slide set in it while the slide "
          "without a context keeps the theme's answer");

    // A context that carries no transition is silence, not a zero-length fade.
    verse.transition = std::nullopt;
    check(docs.setThemeContexts(base, {verse}, &error) &&
              docs.resolveTransitionForSlide(show, scripture) == house,
          "a context with no transition inherits like anything else — the slide it set "
          "still gets the theme's answer");

    // The whole-set write cannot smuggle past the sentence the verb quotes.
    openshow::domain::TextContext tooFar;
    tooFar.name = "scripture";
    tooFar.transition = options(TransitionKind::kFade, 10001, 800);
    error.clear();
    check(!docs.setThemeContexts(base, {tooFar}, &error) &&
              error.find("transition") != std::string::npos &&
                  docs.resolveTransitionForSlide(show, scripture) == house,
          "a context set carrying an out-of-band duration is refused by the same sentence "
          "the single-field verb quotes, and the context keeps what it had");

    // --- the verbs refuse the wrong clicks, naming them ---------------------
    error.clear();
    check(!docs.setServiceDefaultTransition(options(TransitionKind::kFade, -5, 800), &error) &&
              error.find("words duration") != std::string::npos &&
                  docs.serviceDefaultTransition() == service,
          "a refused service-default write leaves the standing value exactly where it was");
    error.clear();
    check(!docs.setThemeTransition("no-such-theme", options(TransitionKind::kCut, 0, 0), &error) &&
              error.find("no-such-theme") != std::string::npos,
          "overriding a theme that does not exist is refused naming it");
    error.clear();
    check(!docs.setTextContextTransition(current, "scripture",
                                         options(TransitionKind::kCut, 0, 0),
                                         &error) &&
              error.find("scripture") != std::string::npos,
          "overriding a context the theme does not carry is refused naming the context — "
          "a silent success would teach the operator the view has no override");
    check(docs.setTextContextTransition(base, "scripture", options(TransitionKind::kCut, 10, 20),
                                        &error) &&
              !docs.findTheme(base)->contexts.empty() &&
              docs.findTheme(base)->contexts[0].transition ==
                  options(TransitionKind::kCut, 10, 20) &&
                  docs.resolveTransitionForSlide(show, scripture) ==
                      options(TransitionKind::kCut, 10, 20),
          "the single-context verb sets one context's record and the walk finds it");
    check(docs.setTextContextTransition(base, "scripture", std::nullopt, &error) &&
              !docs.findTheme(base)->contexts[0].transition.has_value() &&
                  docs.resolveTransitionForSlide(show, scripture) == house,
          "and the same verb with no record takes the override off, back to the theme");

    // An unknown slide or show is not a failure: the cue still has to move.
    check(docs.resolveTransitionForSlide(show, "no-such-slide") == service &&
              docs.resolveTransitionForSlide("no-such-show", plain) == service,
          "a slide the model cannot name resolves to the service's own record rather "
          "than to nothing — the audience is owed a reveal either way");
}

// --- task TR.1: the same contract, in the bytes -----------------------------
void exerciseTransitionStorage(const fs::path& tempRoot) {
    using openshow::domain::TransitionKind;
    using openshow::domain::TransitionOptions;
    const auto options = [](TransitionKind kind, int textMs, int mediaMs) {
        TransitionOptions out;
        out.kind = kind;
        out.textDurationMs = textMs;
        out.mediaDurationMs = mediaMs;
        return out;
    };
    std::string error;
    int seq = 0;
    const auto refusesJson = [&](const std::string& manifest, const std::string& expectIn,
                                 const char* what) {
        const fs::path dir = tempRoot / ("transition-refuse-" + std::to_string(++seq));
        std::filesystem::create_directories(dir);
        writeFile(dir / "service.json", manifest);
        Documents attempt;
        error.clear();
        const bool refused = openshow::domain::loadService(&attempt, dir, &error);
        if (!check(!refused && error.find(expectIn) != std::string::npos, what)) {
            std::fprintf(stderr, "   wanted \"%s\", load %s: %s\n", expectIn.c_str(),
                         refused ? "succeeded" : "said", error.c_str());
        }
    };

    // --- written, read back, and written again identically ------------------
    {
        const fs::path dir = tempRoot / "transition-round";
        Documents docs;
        const Id show = docs.createShow("Timed");
        const Id slide = docs.appendSlide(show);
        const Id theme = docs.createTheme("Brisk", "#ffffff", "#000000", "", "Serif", 64.0,
                                          "center", 1.5, &error);
        openshow::domain::TextContext verse;
        verse.name = "scripture";
        verse.transition = options(TransitionKind::kFade, 1500, 1800);
        check(!theme.empty() && !slide.empty() &&
                  docs.setServiceDefaultTransition(options(TransitionKind::kCut, 0, 0), &error) &&
                  docs.setThemeTransition(theme, options(TransitionKind::kFade, 100, 200), &error) &&
                  docs.setShowDefaultTheme(show, theme, &error) &&
                  docs.setThemeContexts(theme, {verse}, &error) &&
                  openshow::domain::saveService(docs, dir, &error),
              "a service with a moved service default, a theme override and a context "
              "override saves");
        const std::string manifest = readFile(dir / "service.json");
        check(manifest.find("\"kind\":\"cut\"") != std::string::npos &&
                  manifest.find("\"kind\":\"fade\"") != std::string::npos,
              "both kinds are written by name");
        Documents back;
        check(openshow::domain::loadService(&back, dir, &error) && sameService(back, docs) &&
                  back.serviceDefaultTransition() == options(TransitionKind::kCut, 0, 0) &&
                  back.findTheme(theme)->transition ==
                      options(TransitionKind::kFade, 100, 200) &&
                  !back.findTheme(theme)->contexts.empty() &&
                  back.findTheme(theme)->contexts[0].transition ==
                      options(TransitionKind::kFade, 1500, 1800),
              "all three seats read back field for field and the whole service compares");
        check(back.resolveTransitionForSlide(show, slide) ==
                  options(TransitionKind::kFade, 100, 200),
              "and the reloaded document resolves the same slide to the same record — "
              "a cue's move survives save and reopen");
        check(openshow::domain::saveService(back, tempRoot / "transition-again", &error) &&
                  readFile(tempRoot / "transition-again" / "service.json") == manifest,
              "re-saving a transition-carrying service writes a byte-identical manifest");
    }

    // Silence is written as silence at the two OVERRIDE seats — an unconfigured
    // document keeps the shape it had before the contract existed — while the
    // ROOT record is always there, because a service's own answer has no
    // "absent" state to fall back to.
    {
        Documents docs;
        const Id show = docs.createShow("Quiet");
        docs.appendSlide(show);
        const Id theme = docs.createTheme("Still", "#ffffff", "#000000", "", "Serif", 64.0,
                                          "center", 1.5, &error);
        openshow::domain::TextContext bare;
        bare.name = "scripture";
        const fs::path dir = tempRoot / "transition-silent";
        check(!theme.empty() && docs.setShowDefaultTheme(show, theme, &error) &&
                  docs.setThemeContexts(theme, {bare}, &error) &&
                  openshow::domain::saveService(docs, dir, &error),
              "a service nobody configured saves");
        const std::string manifest = readFile(dir / "service.json");
        check(manifest.find("\"transition\":{") != std::string::npos,
              "the root transition record is written even when nobody opened the settings "
              "— the service's answer is a value, not a maybe");
        // Count them: exactly one in the file (the root's), so neither the
        // theme nor the context wrote a key for carrying nothing.
        std::size_t occurrences = 0;
        for (std::size_t at = manifest.find("\"transition\""); at != std::string::npos;
             at = manifest.find("\"transition\"", at + 1)) {
            ++occurrences;
        }
        check(occurrences == 1,
              "and exactly one — a theme or a context that says nothing about movement "
              "writes no key, so old documents keep their shape through this build");
    }

    // --- the era guards -----------------------------------------------------
    // One theme fixture, spelled complete, waiting for one transition key to
    // be appended before its closing brace, plus a root that may or may not
    // carry the service's own record.
    const std::string themeStart =
        R"({"id":"theme1","name":"House","textColor":"#ffffff","backgroundColor":"#000000",)"
        R"("backgroundGradient":"","fontFamily":"","fontSize":64,"align":"center",)"
        R"("lineSpacing":1.5)";
    const std::string serviceHead =
        R"({"format":)" + std::string("{F}") +
        R"(,"nextOrdinal":99,"currentThemeId":"",{R}"shows":[],"themes":[)";
    const std::string themesTail = R"(],"assets":[],"playlists":[]})";
    const auto themed = [&](unsigned long long format, const std::string& rootRecord,
                            const std::string& extraThemeKey) {
        std::string head = serviceHead;
        head.replace(head.find("{F}"), 3, std::to_string(format));
        head.replace(head.find("{R}"), 3, rootRecord);
        return head + themeStart + extraThemeKey + "}" + themesTail;
    };
    const std::string rootFade =
        R"("transition":{"kind":"fade","textDurationMs":500,"mediaDurationMs":800},)";
    const std::string goodRecord =
        R"(,"transition":{"kind":"cut","textDurationMs":0,"mediaDurationMs":0})";

    // Format 8 never knew the key, in the root or on a theme.
    refusesJson(themed(8, "", goodRecord), "theme 'theme1' carries \"transition\" in a format 8",
                "a theme transition in a format-8 file is refused naming the era");
    const std::string rootOnly8 =
        R"({"format":8,"nextOrdinal":99,"currentThemeId":"","transition":{"kind":"fade",)"
        R"("textDurationMs":500,"mediaDurationMs":800},"shows":[],"themes":[],)"
        R"("assets":[],"playlists":[]})";
    refusesJson(rootOnly8, "\"transition\" is present in a format 8 file",
                "and a root transition in that era is refused the same way — it is another "
                "build's meaning of a key this writer never emitted");
    // Format 9 knows the key, so a bad VALUE in it is refused by the sentence,
    // and a missing root record is not a package this build produced.
    refusesJson(themed(9, rootFade, R"(,"transition":{"kind":"fade","textDurationMs":10001,)"
                                    R"("mediaDurationMs":800})"),
                "words duration",
                "a format-9 theme override outside the band meets the domain's own sentence");
    refusesJson(themed(9, rootFade, R"(,"transition":{"kind":"fade","textDurationMs":300})"),
                "whole record or nothing",
                "half a transition record is refused: kind and both lengths travel together");
    refusesJson(themed(9, rootFade,
                       R"(,"transition":{"kind":"fade","textDurationMs":300,"mediaDurationMs":400,)"
                       R"("curve":"ease"})"),
                "transition has no field 'curve'",
                "and a member the record never had is refused naming itself");
    refusesJson(themed(9, rootFade, R"(,"transition":{"kind":"wipe","textDurationMs":1,)"
                                     R"("mediaDurationMs":2})"),
                "kind is 'wipe'",
                "an unknown kind name in the file is refused, not read as a fade");
    refusesJson(themed(9, "", goodRecord), "has no \"transition\" record of its own",
                "a format-9 package with no root transition is refused — a 9-file always "
                "carries its service's answer");
    refusesJson(themed(9,
                       R"("transition":{"kind":"fade","textDurationMs":500,"mediaDurationMs":-1},)",
                       goodRecord),
                "background duration",
                "and an out-of-band service default is refused before the model is built");

    // A context's override is format 9's too, one level deeper.
    const std::string contextHead =
        R"({"format":)" + std::string("{F}") +
        R"(,"nextOrdinal":99,"currentThemeId":"","shows":[],"themes":[{"id":"theme1",)"
        R"("name":"House","textColor":"#ffffff","backgroundColor":"#000000",)"
        R"("backgroundGradient":"","fontFamily":"","fontSize":64,"align":"center",)"
        R"("lineSpacing":1.5,"contexts":[{"name":"scripture"{C}}]}],"assets":[],)"
        R"("playlists":[]})";
    const auto withContext = [&](unsigned long long format, const std::string& rootRecord,
                                 const std::string& extraContextKey) {
        std::string head = contextHead;
        head.replace(head.find("{F}"), 3, std::to_string(format));
        // The root record member has to land before "shows", so splice it in
        // where the fixture left the marker for it.
        const std::size_t at = head.find("\"shows\"");
        head.insert(at, rootRecord);
        head.replace(head.find("{C}"), 3, extraContextKey);
        return head;
    };
    const std::string contextRecord =
        R"(,"transition":{"kind":"fade","textDurationMs":1,"mediaDurationMs":2})";
    refusesJson(withContext(8, "", contextRecord),
                "carries \"transition\" in a format 8 file, where a context",
                "a context transition in a format-8 file is refused naming the era and "
                "what a context held then");
    check(!withContext(9, rootFade, contextRecord).empty(),
          "the same record in a format-9 file is the shape the writer uses");
    {
        const fs::path dir = tempRoot / "transition-context-era";
        std::filesystem::create_directories(dir);
        writeFile(dir / "service.json", withContext(9, rootFade, contextRecord));
        Documents back;
        error.clear();
        check(openshow::domain::loadService(&back, dir, &error) &&
                  !back.findTheme("theme1")->contexts.empty() &&
                  back.findTheme("theme1")->contexts[0].transition ==
                      options(TransitionKind::kFade, 1, 2),
              "and a format-9 file's context override reads back as the record it says");
    }
}

void exerciseForegroundThemeStorage(const fs::path& tempRoot) {
    Documents docs;
    std::string error;
    const auto theme = docs.createTheme("Bright", "#ffd23f", "#101828", "#303848", "", 64, "left", 1.6);
    const auto show = docs.createShow("Lyrics");
    const auto slide = docs.appendSlide(show);
    docs.setShowDefaultTheme(show, theme);
    check(docs.findTheme(theme)->backgroundColor.empty() && docs.findTheme(theme)->backgroundGradient.empty()
              && docs.appearanceForSlide(show, slide).backgroundColor.empty(),
          "theme creation cannot store or resolve a backdrop");
    check(docs.updateTheme(theme, "#ffffff", "#f00", "#00f", "", 72, "center", 1.25)
              && docs.findTheme(theme)->backgroundColor.empty(),
          "legacy theme edits also leave the backdrop absent");
    const auto dir = tempRoot / "foreground-themes";
    check(openshow::domain::saveService(docs, dir, &error), "save foreground themes");
    auto json = boost::json::parse(readFile(dir / "service.json"));
    check(json.at("format").as_int64() == 11, "foreground theme files identify format 11");
    for (auto& value : json.at("themes").as_array()) {
        check(!value.as_object().contains("backgroundColor") && !value.as_object().contains("backgroundGradient"),
              "new service files contain no theme background settings");
        value.as_object()["backgroundColor"] = "#101828";
        value.as_object()["backgroundGradient"] = "";
    }
    writeFile(dir / "service.json", boost::json::serialize(json));
    Documents back;
    check(!openshow::domain::loadService(&back, dir, &error), "format 11 refuses theme backgrounds");
    json.as_object()["format"] = 10;
    writeFile(dir / "service.json", boost::json::serialize(json));
    check(openshow::domain::loadService(&back, dir, &error)
              && back.findTheme(theme)->backgroundColor.empty()
              && back.findTheme(theme)->textColor == "#ffffff"
              && back.appearanceForSlide(show, slide).backgroundColor.empty(),
          "legacy backgrounds are retired while text styling and references survive");
}

void exerciseDefaultThemeFloor(const fs::path& tempRoot) {
    Documents docs;
    std::string error;
    // The floor (operator amendment 2026-11-02): every service stands on
    // the embedded Default theme, seeded at construction before anything
    // else can stack on it.
    check(docs.themeIds() == std::vector<Id>({"default"}) && docs.defaultTheme().id == "default" &&
              docs.findTheme("default") != nullptr,
          "a fresh service is seeded with exactly the Default theme");
    const Theme& floor = *docs.findTheme("default");
    check(floor.name == "Default" && floor.textColor == "#ffffff" &&
              floor.backgroundColor.empty() && floor.backgroundGradient.empty() &&
              floor.fontFamily.empty() && floor.fontSize == 0.0 && floor.align == "center" &&
              floor.lineSpacing == kDefaultLineSpacing && !floor.placement,
          "the Default look is the built-in look it replaced, property for property");
    const Id show = docs.createShow("Deck", &error);
    const Id slide = docs.appendSlide(show, &error);
    const ResolvedAppearance bare = docs.appearanceForSlide(show, slide);
    check(bare.textColor == "#ffffff" && bare.backgroundColor.empty() && !bare.placement,
          "an unthemed slide resolves through the Default floor");
    // A theme like any other, except at the delete button...
    check(docs.renameTheme("default", "House floor", &error) &&
              docs.findTheme("default")->name == "House floor",
          "the Default theme renames");
    const BoxRect spot{120.0, 640.0, 1680.0, 300.0};
    check(docs.setThemePlacement("default", spot, &error) &&
              docs.appearanceForSlide(show, slide).placement == std::optional<BoxRect>(spot),
          "the Default theme places the primary box while it speaks");
    check(docs.clearThemePlacement("default", &error) &&
              !docs.appearanceForSlide(show, slide).placement,
          "and its placement clears back to no opinion");
    check(!docs.removeTheme("default", &error) && docs.findTheme("default") != nullptr &&
              error.find("cannot be removed") != std::string::npos,
          "the Default theme refuses to be removed and stays put");
    // A service written before the amendment — or hand-edited into its
    // absence — opens on the floor again: the loader rebuilds the documents
    // wholesale and ensureDefaultTheme lands after it (storage.cpp).
    const fs::path preAmendment = tempRoot / "no-floor";
    std::filesystem::create_directories(preAmendment);
    writeFile(preAmendment / "service.json",
              R"({"format":1,"nextOrdinal":9,"shows":[{"id":"show1","name":"Earlier",)"
              R"("slides":[{"id":"slide1","lines":["Ancient line"]}]}],)"
              R"("styles":[],"assets":[],"playlists":[]})");
    Documents healed;
    check(openshow::domain::loadService(&healed, preAmendment, &error) &&
              healed.themeIds() == std::vector<Id>({"default"}),
          "loading a pre-amendment service re-ensures the Default theme");
    check(healed.appearanceForSlide("show1", "slide1").textColor == "#ffffff",
          "the healed service draws its slides on the floor");
}

void exerciseScriptureShows(const fs::path& tempRoot) {
    Documents docs;
    std::string error;
    // The Bible importer's destination shape (task BI.3): a real show that
    // belongs to the schedule and carries where its words came from.
    const Id plain = docs.createShow("Hymn", &error);
    const ScriptureProvenance where{"Psalm 1:1-2", "NIV", "NIV copyright text"};
    const Id scripture = docs.createScriptureShow("", where, &error);
    check(!scripture.empty() && docs.findShow(scripture)->name == "Psalm 1:1-2",
          "a scripture show takes the reference as its name when nothing else is given");
    check(docs.findShow(scripture)->scheduleOwned &&
              docs.findShow(scripture)->scripture->translation == "NIV" &&
              docs.findShow(scripture)->scripture->attribution == "NIV copyright text",
          "ownership and provenance are there from the first moment of existence");
    check(docs.createScriptureShow("x", {"", "NIV", ""}, &error).empty() &&
              error.find("reference") != std::string::npos,
          "a scripture item without the reference it was looked up as is refused");
    check(docs.createScriptureShow("x", {"Psalm 1", "", ""}, &error).empty() &&
              error.find("translation") != std::string::npos,
          "a scripture item that cannot name its translation is refused too");
    check(docs.findShow(plain)->scheduleOwned == false && !docs.findShow(plain)->scripture,
          "an ordinary show carries neither flag nor provenance");
    const Id slide = docs.appendSlide(scripture, &error);
    check(!slide.empty() &&
              docs.setSlideLines(scripture, slide, {"Blessed is the one", "who walks not"},
                                 &error),
          "the scripture show holds ordinary slides");

    // Round trip: save, reload, compare; and the manifest carries the
    // promise in its own words.
    const fs::path package = tempRoot / "scripture";
    std::filesystem::create_directories(package);
    check(openshow::domain::saveService(docs, package, &error), "the scripture service saves");
    {
        const boost::json::value manifest =
            boost::json::parse(readFile(package / "service.json"));
        const boost::json::array& shows = manifest.at("shows").as_array();
        check(shows[0].as_object().find("scheduleOwned") == shows[0].as_object().end() &&
                  shows[0].as_object().find("scripture") == shows[0].as_object().end(),
              "the Library show's entry gained no keys — files without the importer save as before");
        check(shows[1].at("scheduleOwned").as_bool() &&
                  shows[1].at("scripture").at("reference").as_string() == "Psalm 1:1-2" &&
                  shows[1].at("scripture").at("translation").as_string() == "NIV" &&
                  shows[1].at("scripture").at("attribution").as_string() == "NIV copyright text",
              "the schedule-owned entry names its reference, translation and licence");
    }
    Documents back;
    check(openshow::domain::loadService(&back, package, &error) && sameService(docs, back),
          "the scripture item round-trips field for field");
    check(back.findShow(scripture)->scheduleOwned &&
              back.findShow(scripture)->scripture->reference == "Psalm 1:1-2",
          "and the reloaded item is still schedule-owned scripture");

    // A pre-6 file carrying either key is another build meaning something
    // by it: refused naming the key, not guessed (the placement rule).
    const fs::path warped = tempRoot / "scripture-warped";
    std::filesystem::create_directories(warped);
    const std::string kFormat1Head =
        R"({"format":1,"nextOrdinal":9,"shows":[{"id":"show1","name":"Earlier",)";
    writeFile(warped / "service.json",
              kFormat1Head +
                  R"("scheduleOwned":true,"slides":[],"notes":""}],"styles":[],"assets":[],"playlists":[]})");
    Documents healed;
    check(!openshow::domain::loadService(&healed, warped, &error) &&
              error.find("scheduleOwned") != std::string::npos,
          "a format-1 file claiming scheduleOwned is refused naming the key");
    writeFile(warped / "service.json",
              kFormat1Head +
                  R"("scripture":{"reference":"Psalm 1","translation":"NIV","attribution":""},)"
                  R"("slides":[],"notes":""}],"styles":[],"assets":[],"playlists":[]})");
    check(!openshow::domain::loadService(&healed, warped, &error) &&
              error.find("scripture") != std::string::npos,
          "a format-1 file carrying scripture provenance is refused too");
    // Format 6 keeps its own pair honest: provenance belongs to a
    // schedule-owned show and to nothing else.
    const fs::path mismatched = tempRoot / "scripture-mismatched";
    std::filesystem::create_directories(mismatched);
    writeFile(mismatched / "service.json",
              R"({"format":6,"nextOrdinal":9,"currentThemeId":"","shows":[)"
              R"({"id":"show1","name":"False claim","notes":"","defaultThemeId":"",)"
              R"("slides":[],"scripture":{"reference":"Psalm 1","translation":"NIV",)"
              R"("attribution":""}}],"themes":[],"assets":[],"playlists":[]})");
    Documents liar;
    check(!openshow::domain::loadService(&liar, mismatched, &error) &&
              error.find("schedule-owned") != std::string::npos,
          "provenance on a show that is not schedule-owned is refused");
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

void exerciseSongReference(const fs::path& tempRoot) {
    Documents documents;
    std::string error;
    const Id authored = documents.createShow("Authored", &error);
    const Id prepared = documents.createShow("Morning Light", &error);
    check(!authored.empty() && !prepared.empty(), "the song-reference service builds");
    check(documents.findShow(prepared)->catalogItemId.empty() &&
              !documents.findShow(prepared)->song.has_value(),
          "a new show carries neither a catalog link nor song attribution");
    check(documents.showForCatalogItem("") == nullptr &&
              documents.showForCatalogItem("item9") == nullptr,
          "an empty or unknown catalog id finds no show");

    const SongProvenance attribution{"9000001", "Example Writer",
                                     "\xc2\xa9 2024 Example Music"};
    check(documents.setShowSongReference(prepared, "item3", attribution, &error),
          "a show is linked to the shared catalog song");
    check(documents.findShow(prepared)->catalogItemId == "item3" &&
              documents.findShow(prepared)->song == attribution,
          "the link and the copied attribution are stored");
    check(documents.showForCatalogItem("item3") == documents.findShow(prepared),
          "the linked show is found by its catalog id");
    check(documents.findShow(authored)->catalogItemId.empty(),
          "other shows are untouched");

    check(!documents.setShowSongReference(prepared, "item3",
                                          SongProvenance{"CCLI 9000001", "", ""},
                                          &error),
          "a non-numeric CCLI field is refused");
    check(error.find("CCLI") != std::string::npos,
          "the refusal names the CCLI field");
    check(documents.findShow(prepared)->song == attribution,
          "the refused write changed nothing");
    check(!documents.setShowSongReference("show999", "item3", attribution, &error),
          "linking an unknown show is refused");

    // The service is self-contained: the link and attribution round-trip
    // without the catalog.
    const fs::path package = tempRoot / "song-reference";
    std::filesystem::create_directories(package);
    check(openshow::domain::saveService(documents, package, &error),
          "the song-linked service saves");
    {
        const boost::json::value manifest =
            boost::json::parse(readFile(package / "service.json"));
        const boost::json::array& shows = manifest.at("shows").as_array();
        check(shows[0].as_object().at("catalogItemId").as_string().empty() &&
                  shows[0].as_object().find("song") == shows[0].as_object().end(),
              "an authored show writes an empty link and no attribution key");
        check(shows[1].as_object().at("catalogItemId").as_string() == "item3" &&
                  shows[1].as_object().at("song").at("ccli").as_string() == "9000001" &&
                  shows[1].as_object().at("song").at("authors").as_string() ==
                      "Example Writer",
              "the linked show names its catalog item and attribution");
    }
    Documents back;
    check(openshow::domain::loadService(&back, package, &error) &&
              sameService(documents, back),
          "the catalog link and attribution round-trip field for field");
    check(back.findShow(prepared)->catalogItemId == "item3" &&
              back.findShow(prepared)->song == attribution,
          "the reloaded show still carries both");

    // A pre-10 file carrying either key is another build's meaning: refused
    // naming the keys, not guessed.
    const fs::path earlier = tempRoot / "song-reference-earlier";
    std::filesystem::create_directories(earlier);
    writeFile(earlier / "service.json",
              R"({"format":9,"nextOrdinal":9,"currentThemeId":"",)"
              R"("transition":{"kind":"fade","textDurationMs":500,)"
              R"("mediaDurationMs":800},"shows":[{"id":"show1",)"
              R"("name":"Earlier","notes":"","defaultThemeId":"",)"
              R"("catalogItemId":"item3","slides":[]}],"themes":[],)"
              R"("assets":[],"playlists":[]})");
    Documents older;
    check(!openshow::domain::loadService(&older, earlier, &error) &&
              error.find("catalogItemId") != std::string::npos,
          "a format-9 file claiming a catalog link is refused naming the key");

    // Detaching (the catalog record was deleted) keeps the attribution so
    // reports survive; the show remains playable on its own.
    check(documents.setShowSongReference(prepared, "", attribution, &error),
          "the link can be cleared without losing the song");
    check(documents.findShow(prepared)->catalogItemId.empty() &&
              documents.findShow(prepared)->song == attribution,
          "the attribution stays after the catalog link is gone");
}

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
    exerciseThemeChain();
    exerciseThemePlacement();
    exerciseTextContextChain();
    exerciseTextStyle();
    exerciseTransition();
    exercisePlaylists();
    exerciseFailureLeavesNoTrace();

    // Persistence works in a private temp directory, removed at exit.
    const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() /
                                           ("openshow-domain-tests-" + std::to_string(getpid()));
    std::filesystem::remove_all(tempRoot);
    exercisePersistence(tempRoot);
    exerciseBoxStorage(tempRoot);
    exerciseTextStyleStorage(tempRoot);
    exerciseTransitionStorage(tempRoot);
    exerciseForegroundThemeStorage(tempRoot);
    exerciseDefaultThemeFloor(tempRoot);
    exerciseScriptureShows(tempRoot);
    exerciseSongReference(tempRoot);
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
