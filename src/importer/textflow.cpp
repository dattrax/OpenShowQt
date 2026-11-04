// The engine: measure through the port, prefix-pack whole blocks, split a
// too-long head block at sentence then word boundaries, and refuse with a
// named sentence whenever the words cannot honestly fit — never clip,
// never repeat, never shrink. See the header for the rules this file
// carries out.
#include "openshow/importer/textflow.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>

namespace openshow::importer {

namespace {

// One thing still to be placed: a whole block, or the remainder of a block
// that had to be split (which keeps its id — block identity survives the
// split — and knows it starts mid-block). `lines` is the block's whole
// form, one entry per authored line; `joined` is those lines as one line
// of words, which is the shape every split works on (a split point lives
// inside the words, so no line-boundary assumption survives a cut).
struct Piece {
    std::string id;
    std::vector<std::string> lines;
    std::string joined;
    bool startsMid = false;
};

bool isUsableSpacing(double spacing) {
    return std::isfinite(spacing) && spacing >= 0.25 && spacing <= 8.0;
}

// A block whose every character is whitespace holds no words; placing it
// would put a blank slide on the screen and claim it carries the text.
bool hasWords(const std::string& text) {
    return text.find_first_not_of(" \t\n\r\v\f") != std::string::npos;
}

std::string join(const std::vector<std::string>& parts, std::size_t from, std::size_t to) {
    std::string out;
    for (std::size_t i = from; i < to && i < parts.size(); ++i) {
        if (i > from) {
            out.push_back(' ');
        }
        out += parts[i];
    }
    return out;
}

// Per-run measurement state: a content cache (an identical block is asked
// about exactly once AT ITS COMPLETE TYPOGRAPHY — the same words at two
// sizes, weights, faces or effect settings are different questions with
// different answers, and TS.3's whole point is that the packer asks about
// the typography the audience will get) and a failure latch (a dead
// engine is not re-asked block after block; the first failure's sentence
// is the run's).
class Measurer {
public:
    // One block's measured answer: height plus the worker's fit verdict
    // when the reply carried one (-1 unknown — a pre-v2 answer — 0 no,
    // 1 yes).
    struct Answer {
        double height = 0.0;
        int fits = -1;
    };

    explicit Measurer(const MeasureFn& fn) : fFn(fn) {}

    // One block's answer at the given box and typography: height plus the
    // worker's fit verdict when it sent one (-1 unknown / 0 no / 1 yes).
    bool block(const std::vector<std::string>& lines, const FlowStyle& style, double fontSize,
               double* heightOut, int* fitsOut) {
        std::vector<std::vector<std::string>> one{lines};
        std::vector<Answer> answers(1);
        if (!measureAll(one, answers.data(), style, fontSize)) {
            return false;
        }
        if (heightOut != nullptr) {
            *heightOut = answers[0].height;
        }
        if (fitsOut != nullptr) {
            *fitsOut = answers[0].fits;
        }
        return true;
    }

    // One answer per block, at the given box and typography. Every block
    // not already cached under its full key goes to the port in chunks of
    // at most kMaxMeasureChunk (the wire's batch cap, mirrored so a legal
    // run never meets a refusal).
    bool measureAll(const std::vector<std::vector<std::string>>& blocks, Answer* out,
                    const FlowStyle& style, double fontSize) {
        if (fFailed) {
            return false;
        }
        std::vector<MeasureRequest> fresh;
        std::vector<std::size_t> slots;
        std::vector<std::string> freshKeys;
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            MeasureRequest request = requestFor(blocks[i], style, fontSize);
            const std::string cacheKey = measureCacheKey(request);
            const auto cached = fCache.find(cacheKey);
            if (cached != fCache.end()) {
                out[i] = cached->second;
                continue;
            }
            freshKeys.push_back(cacheKey);
            fresh.push_back(std::move(request));
            slots.push_back(i);
        }
        if (fresh.empty()) {
            return true;
        }
        std::vector<Answer> answers(fresh.size(), Answer{0.0, -1});
        for (std::size_t start = 0; start < fresh.size(); start += kMaxMeasureChunk) {
            const std::size_t end = std::min<std::size_t>(start + kMaxMeasureChunk, fresh.size());
            std::vector<MeasureRequest> requests;
            requests.reserve(end - start);
            for (std::size_t i = start; i < end; ++i) {
                MeasureRequest request = fresh[i];
                request.key = "m" + std::to_string(fKeyCounter++);
                requests.push_back(std::move(request));
            }
            std::vector<MeasureReply> replies;
            std::string error;
            if (!fFn || !fFn(requests, &replies, &error)) {
                fFailed = true;
                if (fError.empty()) {
                    fError = error.empty() ? "the text could not be measured" : error;
                }
                return false;
            }
            // The answer must be the whole ask, in order, under the keys
            // that were sent: anything less fails the run rather than
            // packing a page on a half-understood height.
            if (replies.size() != requests.size()) {
                fFailed = true;
                if (fError.empty()) {
                    fError = "the measurement answer did not cover the whole batch";
                }
                return false;
            }
            for (std::size_t i = 0; i < requests.size(); ++i) {
                const MeasureReply& reply = replies[i];
                if (reply.key != requests[i].key || !std::isfinite(reply.height) ||
                    reply.height < 0.0) {
                    fFailed = true;
                    if (fError.empty()) {
                        fError = "the measurement answer was not the batch that was asked";
                    }
                    return false;
                }
                Answer answer{reply.height, reply.fits.has_value() ? (*reply.fits ? 1 : 0) : -1};
                if (fCache.size() < kMaxMeasureCacheEntries) {
                    fCache.emplace(freshKeys[start + i], answer);
                }
                answers[start + i] = answer;
            }
        }
        for (std::size_t k = 0; k < slots.size(); ++k) {
            out[slots[k]] = answers[k];
        }
        return true;
    }

    const std::string& error() const { return fError; }

    // The request a block of words asks at this box and typography — the
    // style block forwarded VERBATIM, so the engine's question names the
    // same numbers the drawing will use. Shared with the cache key so the
    // two can never disagree about what was asked.
    static MeasureRequest requestFor(const std::vector<std::string>& lines,
                                     const FlowStyle& style, double fontSize) {
        MeasureRequest request;
        request.lines = lines;
        request.fontSize = fontSize;
        request.width = style.boxWidth;
        request.lineSpacing = style.lineSpacing;
        request.fontFamily = style.fontFamily;
        request.fontWeight = style.fontWeight;
        request.fontStyle = style.fontStyle;
        request.fitMode = style.fitMode;
        request.minFontSize = style.minFontSize;
        request.stroke = style.stroke;
        request.shadow = style.shadow;
        return request;
    }

private:
    const MeasureFn& fFn;
    std::map<std::string, Answer> fCache;
    bool fFailed = false;
    std::string fError;
    std::uint64_t fKeyCounter = 0;
};

// The largest k in [1, n] for which candidate(k) measures at or under
// `available`; 0 when even candidate(1) does not fit. `candidate(k)`
// builds the k-th block of a NESTED sequence — whole blocks stacking
// lines, sentence fragments growing by sentence, word prefixes growing
// by word — and the run's honesty rests on that nesting being monotone:
// adding to a candidate can never make it shorter. Geometric probes (ONE
// batched call for all of them, they are independent) bracket the answer
// and a binary search finishes it; the cache means no block is measured
// twice.
template <typename BuildFn>
bool largestFitting(Measurer& measurer, std::size_t n, const FlowStyle& style, double available,
                    const BuildFn& candidate, std::size_t* out) {
    // TS.3's fit criterion, in one place: the measured HEIGHT must clear
    // the room AND the worker's FILL verdict must not have said no — a
    // word too long for the width wraps to a line that overhangs the box,
    // and no re-paging fixes it, so that candidate does not suit. A
    // `fits` the answer does not carry (a pre-16 worker, which also has
    // no styled effects to overflow) leaves the height as the whole
    // judgment — the pre-TS.3 behaviour, honoured rather than guessed
    // over.
    const auto suits = [](const Measurer::Answer& answer) {
        return answer.fits != 0;
    };
    if (n == 0) {
        *out = 0;
        return true;
    }
    // Probes: 1, 2, 4, ... and n itself.
    std::vector<std::size_t> probes;
    for (std::size_t p = 1; p < n; p *= 2) {
        probes.push_back(p);
    }
    probes.push_back(n);
    std::vector<std::vector<std::string>> probeBlocks;
    probeBlocks.reserve(probes.size());
    for (const std::size_t p : probes) {
        probeBlocks.push_back(candidate(p));
    }
    std::vector<Measurer::Answer> answers(probes.size());
    if (!measurer.measureAll(probeBlocks, answers.data(), style, style.fontSize)) {
        return false;
    }
    std::size_t over = probes.size();  // index of the first probe that does NOT fit
    for (std::size_t i = 0; i < probes.size(); ++i) {
        if (answers[i].height > available || !suits(answers[i])) {
            over = i;
            break;
        }
    }
    if (over == probes.size()) {
        *out = n;  // everything fits
        return true;
    }
    if (over == 0) {
        *out = 0;  // not even one candidate fits
        return true;
    }
    std::size_t lo = probes[over - 1];  // measured to fit
    std::size_t hi = probes[over];      // measured to overflow
    while (hi - lo > 1) {
        const std::size_t mid = lo + (hi - lo) / 2;
        double height = 0.0;
        int fits = -1;
        if (!measurer.block(candidate(mid), style, style.fontSize, &height, &fits)) {
            return false;
        }
        if (height <= available && fits != 0) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    *out = lo;
    return true;
}

}  // namespace

std::vector<std::string> splitSentences(std::string_view text) {
    // Sentence terminators, as UTF-8 byte strings. A terminator ends a
    // sentence only when whitespace or the end of the text follows, so
    // abbreviations mid-sentence keep their punctuation. The terminator
    // rides with the sentence that owns it; the separating whitespace is
    // dropped (rejoining with single spaces is exactly lossless for text
    // the caller already collapsed to single spaces).
    static const std::string_view kEnders[] = {
        std::string_view("."),   std::string_view("!"),    std::string_view("?"),
        std::string_view("\xE0\xA5\xA4"),   // U+0964 DEVANAGARI DANDA
        std::string_view("\xE3\x80\x82"),   // U+3002 IDEOGRAPHIC FULL STOP
        std::string_view("\xEF\xBC\x81"),   // U+FF01 FULLWIDTH EXCLAMATION MARK
        std::string_view("\xEF\xBC\x9F"),   // U+FF1F FULLWIDTH QUESTION MARK
    };
    auto isSpace = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
    };
    std::vector<std::string> sentences;
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        for (const std::string_view ender : kEnders) {
            const std::size_t len = ender.size();
            if (i + len > text.size() || text.compare(i, len, ender) != 0) {
                continue;
            }
            const std::size_t after = i + len;
            if (after == text.size() || isSpace(text[after])) {
                // Skip a run of terminators ("..." or "?!") with the sentence.
                std::size_t sentenceEnd = after;
                for (;;) {
                    bool grew = false;
                    for (const std::string_view other : kEnders) {
                        if (sentenceEnd + other.size() <= text.size() &&
                            text.compare(sentenceEnd, other.size(), other) == 0) {
                            sentenceEnd += other.size();
                            grew = true;
                            break;
                        }
                    }
                    if (!grew) {
                        break;
                    }
                }
                if (sentenceEnd == text.size() || isSpace(text[sentenceEnd])) {
                    std::string_view sentence = text.substr(start, sentenceEnd - start);
                    // A run of whitespace before the terminator's own end?
                    // Trailing spaces inside the sentence are dropped too.
                    while (!sentence.empty() && isSpace(sentence.back())) {
                        sentence.remove_suffix(1);
                    }
                    if (!sentence.empty()) {
                        sentences.emplace_back(sentence);
                    }
                    start = sentenceEnd;
                    while (start < text.size() && isSpace(text[start])) {
                        ++start;
                    }
                    i = sentenceEnd > i ? sentenceEnd - 1 : i;
                    break;
                }
            }
        }
    }
    if (start < text.size()) {
        std::string_view tail = text.substr(start);
        while (!tail.empty() && isSpace(tail.back())) {
            tail.remove_suffix(1);
        }
        while (!tail.empty() && isSpace(tail.front())) {
            tail.remove_prefix(1);
        }
        if (!tail.empty()) {
            sentences.emplace_back(tail);
        }
    }
    return sentences;
}

std::vector<std::string> splitWords(std::string_view text) {
    auto isSpace = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
    };
    std::vector<std::string> words;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && isSpace(text[i])) {
            ++i;
        }
        const std::size_t start = i;
        while (i < text.size() && !isSpace(text[i])) {
            ++i;
        }
        if (i > start) {
            words.emplace_back(text.substr(start, i - start));
        }
    }
    return words;
}

bool validateFlowStyle(const FlowStyle& style, std::string* error) {
    // The measurement is only as honest as the numbers asked with: the
    // box must be a real area and the typography the layout's own band.
    // Nothing is "corrected" here — an unusable box is exactly the case
    // that blocks the caller with an explanation. Task TS.3 vets the
    // style block in the same pass: a NaN sigma or a weight of 10^9 that
    // reaches the wire costs the whole batch its answer, and the worker
    // refusing it is the LAST place anyone should find out.
    const auto fail = [error](const std::string& sentence) {
        if (error != nullptr) {
            *error = sentence;
        }
        return false;
    };
    if (!std::isfinite(style.boxWidth) || style.boxWidth < 1.0 ||
        !std::isfinite(style.boxHeight) || style.boxHeight < 1.0 ||
        !std::isfinite(style.fontSize) || style.fontSize < 0.0 ||
        !isUsableSpacing(style.lineSpacing)) {
        return fail(
            "this theme's text area or chosen text size cannot be measured — the box must "
            "have a real width, height and a size (0 is the renderer's own default)");
    }
    if (!openshow::domain::isFontWeightInBand(style.fontWeight)) {
        return fail("this theme's text weight cannot be measured — it must be an integer "
                    "weight from 100 to 900");
    }
    if (!openshow::domain::isFontStyleName(style.fontStyle)) {
        return fail("this theme's text style cannot be measured — it must be normal or italic");
    }
    if (!openshow::domain::isFitModeName(style.fitMode)) {
        return fail("this theme's fit mode cannot be measured — it must be shrink or fixed");
    }
    if (!std::isfinite(style.minFontSize) || style.minFontSize < 1.0 ||
        style.minFontSize > 4096.0) {
        return fail("this theme's smallest readable size cannot be measured — it must be "
                    "between 1 and 4096");
    }
    if (!std::isfinite(style.stroke.width) || style.stroke.width < 0.0 ||
        style.stroke.width > 32.0) {
        return fail("this theme's text outline width cannot be measured — it must be "
                    "between 0 and 32");
    }
    if (!std::isfinite(style.shadow.opacity) || style.shadow.opacity < 0.0 ||
        style.shadow.opacity > 1.0 || !std::isfinite(style.shadow.offsetX) ||
        style.shadow.offsetX < -256.0 || style.shadow.offsetX > 256.0 ||
        !std::isfinite(style.shadow.offsetY) || style.shadow.offsetY < -256.0 ||
        style.shadow.offsetY > 256.0 || !std::isfinite(style.shadow.blurSigma) ||
        style.shadow.blurSigma < 0.0 || style.shadow.blurSigma > 64.0) {
        return fail("this theme's text shadow cannot be measured — its opacity must be "
                    "between 0 and 1, its offsets between -256 and 256, and its blur "
                    "between 0 and 64");
    }
    return true;
}

std::string measureCacheKey(const MeasureRequest& request) {
    // One canonical string for one full question. Strings carry their
    // byte length, so no family name or line can be written to imitate a
    // field boundary; numbers go out at %.9g, which distinguishes every
    // float a layout can actually see (the worker narrows to float) and
    // is stable across runs and machines. The whole style is in the key
    // — colour and opacity included, though they cannot move a metric —
    // because "the key misses only what cannot change the answer" is a
    // claim about today's layout, and a cache should not rest on one.
    std::string key;
    const auto putString = [&key](const std::string& value) {
        key += std::to_string(value.size());
        key.push_back(':');
        key += value;
    };
    const auto putNumber = [&key](double value) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.9g", value);
        key += buffer;
        key.push_back('|');
    };
    putNumber(request.fontSize);
    putNumber(request.width);
    putNumber(request.lineSpacing);
    putString(request.fontFamily);
    putNumber(request.fontWeight);
    putString(request.fontStyle);
    putString(request.fitMode);
    putNumber(request.minFontSize);
    putNumber(request.stroke.enabled ? 1.0 : 0.0);
    putString(request.stroke.color);
    putNumber(request.stroke.width);
    putNumber(request.shadow.enabled ? 1.0 : 0.0);
    putString(request.shadow.color);
    putNumber(request.shadow.opacity);
    putNumber(request.shadow.offsetX);
    putNumber(request.shadow.offsetY);
    putNumber(request.shadow.blurSigma);
    key.push_back('#');
    for (const std::string& line : request.lines) {
        putString(line);
    }
    return key;
}

bool measureReservation(const std::vector<std::vector<std::string>>& blocks,
                        const FlowStyle& style, double fontSize, const MeasureFn& measure,
                        double* height, std::string* error) {
    Measurer measurer(measure);
    std::vector<Measurer::Answer> answers(blocks.size());
    if (!measurer.measureAll(blocks, answers.data(), style, fontSize)) {
        if (error != nullptr) {
            *error = measurer.error();
        }
        return false;
    }
    double sum = 0.0;
    for (const Measurer::Answer& answer : answers) {
        sum += answer.height;
    }
    if (height != nullptr) {
        *height = sum;
    }
    return true;
}

FlowOutcome runTextFlow(const FlowInput& input, const MeasureFn& measure) {
    FlowOutcome outcome;
    const FlowStyle& style = input.style;
    const std::string subject = input.subject.empty() ? std::string("the text") : input.subject;
    const auto measureFail = [&subject](const std::string& why) {
        return subject + " could not be measured: " + why;
    };
    if (!validateFlowStyle(style, &outcome.error)) {
        return outcome;
    }
    std::vector<Piece> pieces;
    pieces.reserve(input.blocks.size());
    for (const FlowBlock& block : input.blocks) {
        std::string joined = join(block.lines, 0, block.lines.size());
        if (!hasWords(joined)) {
            continue;  // a block with no words carries nothing to place
        }
        pieces.push_back(Piece{block.id, block.lines, std::move(joined), false});
    }
    if (pieces.empty()) {
        outcome.error = subject + " has nothing to place";
        return outcome;
    }

    Measurer measurer(measure);
    const double available = style.boxHeight - input.reservedHeight;
    outcome.availableHeight = available;
    if (available < 1.0) {
        outcome.error = "what stands above the words leaves no room for " + subject +
                        " in this theme's text area — choose a larger box or a smaller text size";
        return outcome;
    }

    while (!pieces.empty()) {
        // candidate(k) = the block of the first k pieces, each whole block
        // contributing its own lines: a whole-block slide.
        std::size_t whole = 0;
        if (!largestFitting(
                measurer, pieces.size(), style, available,
                [&pieces](std::size_t k) {
                    std::vector<std::string> lines;
                    for (std::size_t i = 0; i < k; ++i) {
                        for (const std::string& line : pieces[i].lines) {
                            lines.push_back(line);
                        }
                    }
                    return lines;
                },
                &whole)) {
            outcome.error = measureFail(measurer.error());
            return outcome;
        }
        if (whole >= 1) {
            FlowSlide slide;
            // A slide that opens on the tail of a block a previous slide
            // split says so: the reader must be able to tell "this block
            // continues here" from "this block starts here".
            for (std::size_t i = 0; i < whole; ++i) {
                slide.blockIds.push_back(pieces[i].id);
                for (const std::string& line : pieces[i].lines) {
                    slide.lines.push_back(line);
                }
            }
            slide.startsContinued = pieces[0].startsMid;
            if (input.labeler) {
                slide.label = input.labeler(slide.blockIds, slide.startsContinued, false);
            }
            outcome.slides.push_back(std::move(slide));
            pieces.erase(pieces.begin(),
                         pieces.begin() + static_cast<std::ptrdiff_t>(whole));
            continue;
        }

        // Not even the front block fits whole: split it. Sentences first
        // (the bigger honest pieces), words as the fallback. Both drop and
        // re-add only whitespace, so the words survive the split exactly.
        Piece& head = pieces.front();
        std::vector<std::string> parts = splitSentences(head.joined);
        std::size_t take = 0;
        if (parts.size() > 1) {
            if (!largestFitting(
                    measurer, parts.size(), style, available,
                    [&parts](std::size_t j) { return std::vector<std::string>{join(parts, 0, j)}; },
                    &take)) {
                outcome.error = measureFail(measurer.error());
                return outcome;
            }
        }
        if (take == 0) {
            parts = splitWords(head.joined);
            if (parts.empty()) {
                // Unreachable in practice (empty pieces were filtered), but
                // a block with no words must not loop forever.
                outcome.error = "a block of " + subject + " has no words to place";
                return outcome;
            }
            if (!largestFitting(
                    measurer, parts.size(), style, available,
                    [&parts](std::size_t j) { return std::vector<std::string>{join(parts, 0, j)}; },
                    &take)) {
                outcome.error = measureFail(measurer.error());
                return outcome;
            }
            if (take == 0) {
                // The honest dead end: the first word alone is taller/wider
                // than the box at the chosen size. Clipping it or shrinking
                // the font are both forbidden, so the caller blocks.
                outcome.error = "a single word of " +
                                (head.id.empty() ? subject : head.id) +
                                " does not fit this theme's text area at the chosen size — "
                                "choose a larger box or a smaller text size";
                return outcome;
            }
        }
        FlowSlide slide;
        slide.blockIds.push_back(head.id);
        slide.lines.push_back(join(parts, 0, take));
        slide.startsContinued = head.startsMid;
        slide.continues = true;  // the split's premise; the tail decides below
        if (input.labeler) {
            slide.label = input.labeler(slide.blockIds, slide.startsContinued, true);
        }
        outcome.slides.push_back(std::move(slide));
        if (take >= parts.size()) {
            // The reassembled fragment covered the whole block (only
            // reachable if measurement came out differently for the very
            // same text — then it is simply whole and it is done).
            pieces.erase(pieces.begin());
        } else {
            head.joined = join(parts, take, parts.size());
            head.lines = {head.joined};  // the tail is one honest line
            head.startsMid = true;
        }
    }
    outcome.ok = true;
    return outcome;
}

}  // namespace openshow::importer
