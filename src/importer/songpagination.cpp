#include "openshow/importer/songpagination.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <sstream>

namespace openshow::importer {

namespace {

using openshow::catalog::Stanza;

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

bool isBlank(const std::string& text) {
    return std::all_of(text.begin(), text.end(), isSpace);
}

std::vector<std::string> trimmedEdges(const std::vector<std::string>& lines) {
    std::size_t begin = 0;
    std::size_t end = lines.size();
    while (begin < end && isBlank(lines[begin])) {
        ++begin;
    }
    while (end > begin && isBlank(lines[end - 1])) {
        --end;
    }
    return std::vector<std::string>(lines.begin() + static_cast<std::ptrdiff_t>(begin),
                                    lines.begin() + static_cast<std::ptrdiff_t>(end));
}

bool hasWords(const std::vector<std::string>& lines) {
    for (const std::string& line : lines) {
        if (!isBlank(line)) {
            return true;
        }
    }
    return false;
}

// One run's measurement cache. Keyed on the engine's own full-typography
// serializer, so a block at a different size or style is a different
// question and can never ride a stale answer.
class RunMeasurer {
public:
    RunMeasurer(const FlowStyle& style, const MeasureFn& measure)
        : fStyle(style), fMeasure(measure) {}

    // True when `lines` fit the style's box at its chosen typography.
    bool fits(const std::vector<std::string>& lines, std::string* error) {
        double height = 0.0;
        std::optional<bool> measuredFits;
        if (!heightOf(lines, &height, &measuredFits, error)) {
            return false;
        }
        if (measuredFits.has_value() && !*measuredFits) {
            return false;
        }
        return height <= fStyle.boxHeight;
    }

private:
    bool heightOf(const std::vector<std::string>& lines, double* height,
                  std::optional<bool>* fits, std::string* error) {
        MeasureRequest request;
        static unsigned long long counter = 0;
        request.key = "song-" + std::to_string(++counter);
        request.lines = lines;
        request.fontSize = fStyle.fontSize;
        request.width = fStyle.boxWidth;
        request.lineSpacing = fStyle.lineSpacing;
        request.fontFamily = fStyle.fontFamily;
        request.fontWeight = fStyle.fontWeight;
        request.fontStyle = fStyle.fontStyle;
        request.fitMode = "fixed";  // measurement never shrinks
        request.minFontSize = fStyle.minFontSize;
        request.stroke = fStyle.stroke;
        request.shadow = fStyle.shadow;
        const std::string cacheKey = measureCacheKey(request);
        const auto cached = fCache.find(cacheKey);
        if (cached != fCache.end()) {
            *height = cached->second.height;
            *fits = cached->second.fits;
            return true;
        }
        std::vector<MeasureRequest> batch{request};
        std::vector<MeasureReply> replies;
        if (!fMeasure(batch, &replies, error)) {
            if (error != nullptr && error->empty()) {
                *error = "the lyric text could not be measured";
            }
            return false;
        }
        if (replies.size() != 1 || replies.front().key != request.key) {
            if (error != nullptr) {
                *error = "the lyric measurement answered a different question";
            }
            return false;
        }
        Cached cachedValue;
        cachedValue.height = replies.front().height;
        cachedValue.fits = replies.front().fits;
        fCache.emplace(cacheKey, cachedValue);
        *height = cachedValue.height;
        *fits = cachedValue.fits;
        return true;
    }

    struct Cached {
        double height = 0.0;
        std::optional<bool> fits;
    };

    FlowStyle fStyle;
    MeasureFn fMeasure;
    std::map<std::string, Cached> fCache;
};

std::string trimLabel(const std::string& label) {
    std::size_t begin = 0;
    std::size_t end = label.size();
    while (begin < end && isSpace(label[begin])) {
        ++begin;
    }
    while (end > begin && isSpace(label[end - 1])) {
        --end;
    }
    return label.substr(begin, end - begin);
}

std::string continuationLabel(const std::string& label) {
    const std::string trimmed = trimLabel(label);
    return trimmed.empty() ? std::string() : trimmed + " (cont.)";
}

}  // namespace

SongPaginationOutcome paginateSong(const SongPaginationInput& input,
                                   const MeasureFn& measure) {
    SongPaginationOutcome outcome;
    std::string styleError;
    if (!validateFlowStyle(input.style, &styleError)) {
        outcome.error = styleError;
        return outcome;
    }
    RunMeasurer measurer(input.style, measure);

    const auto push = [&outcome](const std::vector<std::string>& rawLines,
                                 std::size_t stanzaIndex, const std::string& label,
                                 bool startsContinued) {
        SongSlide slide;
        slide.lines = trimmedEdges(rawLines);
        slide.label = label;
        slide.stanzaIndex = stanzaIndex;
        slide.startsContinued = startsContinued;
        outcome.slides.push_back(std::move(slide));
    };

    for (std::size_t stanzaIndex = 0; stanzaIndex < input.stanzas.size();
         ++stanzaIndex) {
        const Stanza& stanza = input.stanzas[stanzaIndex];
        const std::vector<std::string> lines = trimmedEdges(stanza.lines);
        if (!hasWords(lines)) {
            continue;  // a wordless stanza is not a slide
        }

        // Try the stanza whole: most stanzas are one slide.
        std::string error;
        if (measurer.fits(lines, &error)) {
            push(lines, stanzaIndex, trimLabel(stanza.label), false);
            continue;
        }
        if (!error.empty()) {
            outcome.error = error;
            return outcome;
        }

        // Pack whole authored lines inside this stanza, never across its
        // boundary. A line taller than the box is split by the shared
        // Unicode-safe engine and its continuation lines stay in this
        // stanza.
        std::vector<std::string> pending;
        bool emittedAny = false;
        for (std::size_t lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
            const std::string& line = lines[lineIndex];
            if (isBlank(line)) {
                if (!pending.empty()) {
                    pending.push_back(std::string());  // keep an internal break
                }
                continue;
            }
            std::vector<std::string> candidate = pending;
            candidate.push_back(line);
            std::string candidateError;
            if (measurer.fits(candidate, &candidateError)) {
                pending = std::move(candidate);
                continue;
            }
            if (!candidateError.empty()) {
                outcome.error = candidateError;
                return outcome;
            }
            if (!pending.empty()) {
                push(pending, stanzaIndex,
                     emittedAny ? continuationLabel(stanza.label)
                                : trimLabel(stanza.label),
                     emittedAny);
                emittedAny = true;
                pending.clear();
                // Re-try the same line against an empty slide.
                --lineIndex;
                continue;
            }

            // This single authored line does not fit by itself. Hand it to
            // the engine's sentence-then-word split at the same typography.
            FlowInput flowInput;
            FlowBlock block;
            block.id = trimLabel(stanza.label);
            block.lines = std::vector<std::string>{line};
            flowInput.blocks.push_back(std::move(block));
            flowInput.style = input.style;
            flowInput.subject = "the lyric line";
            const FlowOutcome flow = runTextFlow(flowInput, measure);
            if (!flow.ok) {
                outcome.error = flow.error;
                return outcome;
            }
            for (std::size_t part = 0; part < flow.slides.size(); ++part) {
                if (flow.slides[part].lines.empty()) {
                    continue;
                }
                push(flow.slides[part].lines, stanzaIndex,
                     (emittedAny || part > 0)
                         ? continuationLabel(stanza.label)
                         : trimLabel(stanza.label),
                     emittedAny || part > 0);
                emittedAny = true;
            }
        }
        if (!pending.empty()) {
            push(pending, stanzaIndex,
                 emittedAny ? continuationLabel(stanza.label)
                            : trimLabel(stanza.label),
                 emittedAny);
        }
    }

    if (outcome.slides.empty()) {
        outcome.error = "the song has no words to place";
        return outcome;
    }
    outcome.ok = true;
    return outcome;
}

std::string songPlanFingerprint(const SongPaginationInput& input) {
    std::ostringstream out;
    const FlowStyle& style = input.style;
    out << "box=" << style.boxWidth << 'x' << style.boxHeight
        << ";size=" << style.fontSize
        << ";spacing=" << style.lineSpacing
        << ";family=" << style.fontFamily
        << ";weight=" << style.fontWeight
        << ";slant=" << style.fontStyle
        << ";fit=" << style.fitMode
        << ";min=" << style.minFontSize
        << ";stroke=" << style.stroke.enabled << ',' << style.stroke.color << ','
        << style.stroke.width
        << ";shadow=" << style.shadow.enabled << ',' << style.shadow.color << ','
        << style.shadow.opacity << ',' << style.shadow.offsetX << ','
        << style.shadow.offsetY << ',' << style.shadow.blurSigma;
    for (const Stanza& stanza : input.stanzas) {
        out << "|stanza:" << stanza.label << '\n';
        for (const std::string& line : stanza.lines) {
            out << line << '\n';
        }
    }
    return out.str();
}

}  // namespace openshow::importer
