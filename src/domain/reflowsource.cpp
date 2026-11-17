/*
 * Fingerprints and projection extraction for the bounded reflow source
 * (task PT.4). No I/O, no providers, no Library: this file can only read
 * the words a service already holds.
 */

#include "openshow/domain/reflowsource.h"

#include <cstdio>
#include <string_view>

#include "openshow/domain/documents.h"

namespace openshow::domain {

namespace {

// FNV-1a 64-bit. Not a security hash — a change detector with no
// dependency, so two equal projections can never disagree and a save/reopen
// round-trip reproduces the value exactly.
class Fingerprint {
public:
    void add(std::string_view bytes) {
        for (const char c : bytes) {
            fHash ^= static_cast<unsigned long long>(
                static_cast<unsigned char>(c));
            fHash *= 1099511628211ULL;
        }
    }
    // Length-prefix every string so "ab","c" and "a","bc" cannot collide.
    void addField(const std::string& text) {
        add(std::to_string(text.size()));
        add(":");
        add(text);
        add("\n");
    }
    std::string hex() const {
        char buffer[17];
        std::snprintf(buffer, sizeof(buffer), "%016llx", fHash);
        return std::string(buffer);
    }

private:
    unsigned long long fHash = 1469598103934665603ULL;
};

std::string slideLabel(const Slide& slide) {
    return slide.group.empty() ? slide.notes : slide.group;
}

}  // namespace

std::string reflowFingerprint(const std::vector<ReflowSourceBlock>& blocks) {
    Fingerprint print;
    // A version tag: if the canonical spelling ever changes, old stored
    // fingerprints stop matching instead of silently meaning something else.
    print.add("reflow1\n");
    print.addField(std::to_string(blocks.size()));
    for (const ReflowSourceBlock& block : blocks) {
        print.addField(block.label);
        print.addField(std::to_string(block.lines.size()));
        for (const std::string& line : block.lines) {
            print.addField(line);
        }
    }
    return print.hex();
}

std::vector<ReflowSourceBlock> showTextProjection(const Documents& documents,
                                                  const std::string& showId) {
    std::vector<ReflowSourceBlock> blocks;
    const Show* show = documents.findShow(showId);
    if (show == nullptr) {
        return blocks;
    }
    blocks.reserve(show->slides.size());
    for (const Slide& slide : show->slides) {
        // The primary text item is the first text entry, exactly as the
        // canvas and the compile treat it. A wordless cue contributes no
        // words; its presence is an eligibility question, not a projection one.
        const SlideItem* primary = nullptr;
        for (const SlideItem& item : slide.items) {
            if (item.kind != ItemKind::Image) {
                primary = &item;
                break;
            }
        }
        if (primary == nullptr || primary->lines.empty()) {
            continue;
        }
        ReflowSourceBlock block;
        block.label = slideLabel(slide);
        block.lines = primary->lines;
        blocks.push_back(std::move(block));
    }
    return blocks;
}

bool reflowSourceMatches(const Documents& documents, const std::string& showId,
                         const ReflowSource& source) {
    if (source.projectionFingerprint.empty()) {
        return false;
    }
    return reflowFingerprint(showTextProjection(documents, showId)) ==
           source.projectionFingerprint;
}

ReflowSource sourceFromShowProjection(const Documents& documents,
                                      const std::string& showId) {
    ReflowSource source;
    const Show* show = documents.findShow(showId);
    if (show == nullptr) {
        return source;
    }
    if (show->scripture.has_value()) {
        source.kind = ReflowSourceKind::kScripture;
        source.reference = show->scripture->reference;
        source.translation = show->scripture->translation;
        source.attribution = show->scripture->attribution;
    } else {
        source.kind = ReflowSourceKind::kLocalSong;
    }
    // The fingerprint describes the projection AS THE SLIDES SPELL IT; the
    // honest passage label below is extra provenance for the blocks and must
    // not make the source disagree with its own fingerprint.
    source.projectionFingerprint =
        reflowFingerprint(showTextProjection(documents, showId));
    source.blocks = showTextProjection(documents, showId);
    if (source.kind == ReflowSourceKind::kScripture) {
        for (ReflowSourceBlock& block : source.blocks) {
            if (block.label.empty()) {
                // The whole known passage, never an invented verse range.
                block.label = source.reference;
            }
        }
    }
    return source;
}

}  // namespace openshow::domain
