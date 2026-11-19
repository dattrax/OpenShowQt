#include "openshow/importer/reflowpagination.h"

#include <algorithm>
#include <cctype>
#include <string_view>

#include "openshow/catalog/catalog.h"
#include "openshow/importer/scripturepagination.h"
#include "openshow/importer/songpagination.h"

namespace openshow::importer {

namespace {

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

bool hasWords(const std::vector<std::string>& lines) {
    for (const std::string& line : lines) {
        if (std::any_of(line.begin(), line.end(), [](char c) { return !isSpace(c); })) {
            return true;
        }
    }
    return false;
}

std::string trimmed(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && isSpace(text[begin])) {
        ++begin;
    }
    while (end > begin && isSpace(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::string sectionLabel(const std::string& label) {
    std::string name = trimmed(label);
    constexpr std::string_view suffix = " (cont.)";
    while (name.ends_with(suffix)) {
        name = trimmed(name.substr(0, name.size() - suffix.size()));
    }
    return name;
}

// The word sequence, with every whitespace run collapsed. Used both for
// labels? No: words only, so an authored blank line or a re-wrapped line
// cannot be mistaken for lost text.
std::vector<std::string> wordTokens(const std::vector<std::string>& lines) {
    std::vector<std::string> tokens;
    for (const std::string& line : lines) {
        std::string current;
        for (const char c : line) {
            if (isSpace(c)) {
                if (!current.empty()) {
                    tokens.push_back(std::move(current));
                    current.clear();
                }
            } else {
                current.push_back(c);
            }
        }
        if (!current.empty()) {
            tokens.push_back(std::move(current));
        }
    }
    return tokens;
}

}  // namespace

bool reflowWordsPreserved(const std::vector<openshow::domain::ReflowSourceBlock>& blocks,
                          const std::vector<ReflowSlide>& slides) {
    std::vector<std::string> expected;
    for (const openshow::domain::ReflowSourceBlock& block : blocks) {
        const std::vector<std::string> tokens = wordTokens(block.lines);
        expected.insert(expected.end(), tokens.begin(), tokens.end());
    }
    std::vector<std::string> actual;
    for (const ReflowSlide& slide : slides) {
        const std::vector<std::string> tokens = wordTokens(slide.lines);
        actual.insert(actual.end(), tokens.begin(), tokens.end());
    }
    return expected == actual;
}

ReflowPaginationOutcome paginateReflow(const ReflowPaginationInput& input,
                                       const MeasureFn& measure) {
    ReflowPaginationOutcome outcome;
    std::string styleError;
    if (!validateFlowStyle(input.style, &styleError)) {
        outcome.error = styleError;
        return outcome;
    }

    // Wordless blocks are not slides and carry nothing to place; everything
    // with words keeps its authored order.
    std::vector<std::size_t> sourceIndex;
    std::vector<openshow::domain::ReflowSourceBlock> blocks;
    for (std::size_t i = 0; i < input.blocks.size(); ++i) {
        if (!hasWords(input.blocks[i].lines)) {
            continue;
        }
        auto block = input.blocks[i];
        if (input.policy == ReflowPolicy::kSectionStartsNewSlide) {
            block.label = sectionLabel(block.label);
            // A paginated source can contain several fragments of one
            // verse/chorus. Reassemble that consecutive section occurrence
            // before fitting, without moving a later returning chorus.
            if (!block.label.empty() && !blocks.empty() &&
                blocks.back().label == block.label) {
                blocks.back().lines.insert(blocks.back().lines.end(),
                                            block.lines.begin(), block.lines.end());
                continue;
            }
        }
        sourceIndex.push_back(i);
        blocks.push_back(std::move(block));
    }
    if (blocks.empty()) {
        outcome.error = "the item has no words to reflow";
        return outcome;
    }

    if (input.policy == ReflowPolicy::kSectionStartsNewSlide) {
        SongPaginationInput song;
        song.style = input.style;
        song.stanzas.reserve(blocks.size());
        for (const openshow::domain::ReflowSourceBlock& block : blocks) {
            openshow::catalog::Stanza stanza;
            stanza.label = trimmed(block.label);
            stanza.lines = block.lines;
            song.stanzas.push_back(std::move(stanza));
        }
        const SongPaginationOutcome packed = paginateSong(song, measure);
        if (!packed.ok) {
            outcome.error = packed.error;
            return outcome;
        }
        for (const SongSlide& slide : packed.slides) {
            ReflowSlide out;
            out.lines = slide.lines;
            out.label = slide.label;
            out.sectionLabel = song.stanzas[slide.stanzaIndex].label;
            out.blockIndex = sourceIndex[std::min(slide.stanzaIndex, sourceIndex.size() - 1)];
            out.startsContinued = slide.startsContinued;
            outcome.slides.push_back(std::move(out));
        }
        outcome.ok = true;
        return outcome;
    }

    // Scripture: the caption reservation first (the import's own rule and
    // its own measured arithmetic), then greedy whole-block packing.
    double reserved = 0.0;
    FlowStyle captionStyle = input.style;
    captionStyle.fontSize = reservationFontSize(input.style);
    if (!trimmed(input.reference).empty() || !trimmed(input.attribution).empty()) {
        std::vector<std::vector<std::string>> reservations;
        if (!trimmed(input.reference).empty()) {
            reservations.push_back({trimmed(input.reference)});
        }
        if (!trimmed(input.attribution).empty()) {
            reservations.push_back({trimmed(input.attribution)});
        }
        std::string reservationError;
        if (!measureReservation(reservations, captionStyle, captionStyle.fontSize,
                                measure, &reserved, &reservationError)) {
            outcome.error = reservationError;
            return outcome;
        }
    }

    FlowInput flow;
    flow.style = input.style;
    flow.subject = "the passage";
    flow.reservedHeight = reserved;
    std::vector<std::string> labels;
    labels.reserve(blocks.size());
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        const std::string label = trimmed(blocks[i].label);
        labels.push_back(label.empty() ? trimmed(input.fallbackLabel) : label);
        // The block id is the INDEX, so two blocks that happen to share a
        // label stay two distinct blocks to the engine and its labeler.
        flow.blocks.push_back(FlowBlock{std::to_string(i), blocks[i].lines});
    }
    flow.labeler = [labels](const std::vector<std::string>& ids, bool startsContinued,
                            bool continues) {
        if (ids.empty()) {
            return std::string();
        }
        const auto indexOf = [](const std::string& id) {
            return static_cast<std::size_t>(std::stoul(id));
        };
        const std::size_t first = indexOf(ids.front());
        const std::size_t last = indexOf(ids.back());
        std::string label = startsContinued ? "(cont.) " : "";
        const std::string& firstLabel =
            first < labels.size() ? labels[first] : std::string();
        const std::string& lastLabel =
            last < labels.size() ? labels[last] : std::string();
        label += first == last || firstLabel == lastLabel
                     ? firstLabel
                     : firstLabel + "-" + lastLabel;
        if (continues) {
            label += " (continues)";
        }
        return label;
    };

    const FlowOutcome packed = runTextFlow(flow, measure);
    if (!packed.ok) {
        outcome.error = packed.error;
        return outcome;
    }
    for (const FlowSlide& slide : packed.slides) {
        ReflowSlide out;
        out.lines = slide.lines;
        out.label = slide.label;
        out.startsContinued = slide.startsContinued;
        out.blockIndex = slide.blockIds.empty()
                             ? 0
                             : sourceIndex[static_cast<std::size_t>(
                                   std::stoul(slide.blockIds.front()))];
        outcome.slides.push_back(std::move(out));
    }
    if (outcome.slides.empty()) {
        outcome.error = "the item has no words to reflow";
        return outcome;
    }
    outcome.reservedHeight = reserved;
    outcome.ok = true;
    return outcome;
}

}  // namespace openshow::importer
