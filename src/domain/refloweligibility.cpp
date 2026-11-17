#include "openshow/domain/refloweligibility.h"

#include <algorithm>
#include <cctype>
#include <vector>

#include "openshow/domain/reflowsource.h"

namespace openshow::domain {

namespace {

struct SlideShape {
    std::size_t textItems = 0;
    std::size_t imageItems = 0;
    bool hasWords = false;
};

SlideShape shapeOf(const Slide& slide) {
    SlideShape shape;
    for (const SlideItem& item : slide.items) {
        if (item.kind == ItemKind::Image) {
            ++shape.imageItems;
            continue;
        }
        ++shape.textItems;
        for (const std::string& line : item.lines) {
            if (std::any_of(line.begin(), line.end(),
                            [](char c) { return !std::isspace(
                                             static_cast<unsigned char>(c)); })) {
                shape.hasWords = true;
                break;
            }
        }
    }
    return shape;
}

}  // namespace

ReflowEligibility reflowEligibility(const Documents& documents,
                                    const std::string& showId) {
    ReflowEligibility out;
    const Show* show = documents.findShow(showId);
    if (show == nullptr) {
        out.reason = "that item no longer exists";
        return out;
    }
    if (show->slides.empty()) {
        out.reason = "this item has no words to reflow";
        return out;
    }

    const std::size_t slideCount = show->slides.size();
    std::vector<SlideShape> shapes;
    shapes.reserve(slideCount);
    std::size_t firstText = slideCount;
    std::size_t lastText = 0;
    std::size_t textSlides = 0;
    for (std::size_t i = 0; i < slideCount; ++i) {
        const SlideShape shape = shapeOf(show->slides[i]);
        shapes.push_back(shape);
        if (shape.textItems > 1) {
            out.reason = "a slide holds more than one text box — reflow needs one "
                         "box per slide";
            return out;
        }
        if (!shape.hasWords) {
            continue;
        }
        if (shape.imageItems > 0) {
            out.reason = "a slide carries a picture as well as words";
            return out;
        }
        if (firstText == slideCount) {
            firstText = i;
        }
        lastText = i;
        ++textSlides;
    }
    if (textSlides == 0) {
        // A picture-only or PPTX deck gains no fake text-reflow action.
        out.reason = "this item has no words to reflow";
        return out;
    }

    // Only ONE optional leading cue may precede the words; anything else
    // without text (a picture cue, an internal blank, a trailing blank) is
    // structure reflow must not silently delete.
    for (std::size_t i = 0; i < firstText; ++i) {
        if (shapes[i].imageItems > 0) {
            out.reason = "the item begins with a picture-only cue";
            return out;
        }
    }
    if (firstText > 1) {
        out.reason = "the item begins with more than one blank cue";
        return out;
    }
    out.hasLeadingBlankCue = firstText == 1;
    for (std::size_t i = firstText; i < slideCount; ++i) {
        if (!shapes[i].hasWords) {
            out.reason = i > lastText
                             ? "the item ends with a wordless cue"
                             : "the item contains a wordless cue inside its words";
            return out;
        }
    }

    // Foreground pictures on a word slide are additional authored media.
    for (std::size_t i = firstText; i <= lastText; ++i) {
        if (shapes[i].imageItems > 0) {
            out.reason = "a slide carries a picture as well as words";
            return out;
        }
    }

    const Slide& first = show->slides[firstText];
    out.backgroundAssetId = first.backgroundAssetId;
    out.backgroundFit = first.backgroundFit;
    out.textContext = first.textContext;
    for (std::size_t i = firstText; i <= lastText; ++i) {
        const Slide& slide = show->slides[i];
        if (slide.backgroundAssetId != out.backgroundAssetId) {
            out.reason = "the words use different backgrounds";
            return out;
        }
        if (slide.backgroundFit != out.backgroundFit) {
            out.reason = "the words use different background fitting";
            return out;
        }
        if (slide.textContext != out.textContext) {
            out.reason = "the words use different text settings";
            return out;
        }
    }

    out.contentSlideCount = textSlides;
    if (show->reflowSource.has_value() &&
        reflowSourceMatches(documents, showId, *show->reflowSource)) {
        out.source = *show->reflowSource;
        out.usesStoredSource = true;
    } else {
        // Legacy or edited words: the conservative fallback, never the
        // Library or a provider.
        out.source = sourceFromShowProjection(documents, showId);
        out.usesStoredSource = false;
    }
    if (out.source.blocks.empty()) {
        out.reason = "this item has no words to reflow";
        return out;
    }
    out.sectionStartsNewSlide =
        out.source.kind != ReflowSourceKind::kScripture;
    out.eligible = true;
    return out;
}

}  // namespace openshow::domain
