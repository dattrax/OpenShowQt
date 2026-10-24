/*
 * The demo fixture — see demo.h. The numbers echo TestScene's 1.x picture
 * (title #ffffff on the #14161f backdrop, subtitle #c5cfe4, 110/40px) so
 * a scene compiled from here reads as the same show the first milestone
 * drew, now arriving as data over the wire.
 */

#include "openshow/scene/demo.h"

#include <vector>

namespace openshow::scene {

namespace {

// The 1.x background's top stop; the scene backdrop is a solid fill.
constexpr const char* kBackdrop = "#14161f";

}  // namespace

domain::Documents demoDocuments() {
    domain::Documents documents;

    // A show default for lyric body text, and an override for the title
    // card, so the fixture exercises both resolution branches.
    const domain::Id lyric =
        documents.createStyle("Lyric", "#ffffff", kBackdrop, "", 72.0, "center",
                              domain::kDefaultLineSpacing);
    const domain::Id title =
        documents.createStyle("Title", "#ffffff", kBackdrop, "", 110.0, "center",
                              domain::kDefaultLineSpacing);
    const domain::Id subtitle =
        documents.createStyle("Subtitle", "#c5cfe4", kBackdrop, "", 40.0, "center",
                              domain::kDefaultLineSpacing);

    const domain::Id show = documents.createShow("OpenShowQt demo");
    documents.setShowDefaultStyle(show, lyric);

    // Slide 0: the title card — heading over the two lines the first
    // milestone drew, styled through per-slide overrides.
    const domain::Id titleCard = documents.appendSlide(show);
    documents.setSlideLines(show, titleCard,
                            {"OpenShowQt", "Task 2.4 · documents compiled to scenes",
                             "\xce\xa9 \xce\xb2 \xcf\x80 \xc2\xbd \xe2\x80\x94 caf\xc3\xa9 na\xc3\xafve"});
    documents.setSlideStyle(show, titleCard, title);

    // Slide 1: a lyric stanza under the show default (no override).
    const domain::Id stanza = documents.appendSlide(show);
    documents.setSlideLines(show, stanza,
                            {"Come, all people", "", "Sing the song of grace"});
    documents.setSlideGroup(show, titleCard, "Intro");
    documents.setSlideGroup(show, stanza, "Chorus");

    // Slide 2: a section-cue slide under the Subtitle override.
    const domain::Id cue = documents.appendSlide(show);
    documents.setSlideLines(show, cue, {"Reading"});
    documents.setSlideStyle(show, cue, subtitle);

    // Slide 3: the black interlude — no lines, so it compiles to an
    // empty scene that still fills the backdrop.
    documents.appendSlide(show);

    (void)lyric;  // referenced via the show default
    return documents;
}

domain::Id demoShowId(const domain::Documents& documents) {
    const std::vector<domain::Id> ids = documents.showIds();
    return ids.empty() ? domain::Id() : ids.front();
}

std::size_t demoSlideCount(const domain::Documents& documents) {
    const domain::Show* show = documents.findShow(demoShowId(documents));
    return show == nullptr ? 0 : show->slides.size();
}

bool compileDemoSlide(const domain::Documents& documents, std::size_t index, Scene* out,
                      std::string* error) {
    const domain::Id showId = demoShowId(documents);
    const domain::Show* show = documents.findShow(showId);
    if (show == nullptr || index >= show->slides.size()) {
        if (error != nullptr) {
            *error = "demo slide index " + std::to_string(index) + " is out of range";
        }
        return false;
    }
    return compileSlide(documents, showId, show->slides[index].id, out, error);
}

}  // namespace openshow::scene
