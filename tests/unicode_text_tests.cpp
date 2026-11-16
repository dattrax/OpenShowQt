/*
 * Task 6.2b fixtures (review R9): the worker's Unicode backend is the
 * public ICU-backed SkUnicode module, and these fixtures pin what that
 * upgrade actually buys over the retired hard-coded backend — real grapheme
 * clustering (combining marks, emoji sequences, regional indicators), UAX
 * #14 line breaking that can break between CJK characters, real UBA bidi
 * regions for mixed Hebrew/Latin text, and working break iterators.
 *
 * Layout assertions run through the production SceneTextEngine (same
 * engine, fonts and FontCollection fallback as the audience words layer);
 * property assertions use the engine's own SkUnicode through the public
 * interface. Exits 77 (skip) when no usable fonts exist, and a fixture
 * whose script has no installed font is skipped loudly, never silently.
 */

#include <cstdio>
#include <string>
#include <vector>

#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkTypeface.h"
#include "include/ports/SkFontMgr_fontconfig.h"
#include "include/ports/SkFontScanner_FreeType.h"

#include "openshow/scene/scene.h"
#include "render/SceneText.h"

namespace scene = openshow::scene;
namespace worker = openshow::worker;

namespace {

int failures = 0;

bool check(bool condition, const std::string& what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
    return condition;
}

// Count of code-unit positions flagged as the start of a grapheme cluster.
int graphemeClusters(SkUnicode& unicode, const std::string& utf8) {
    skia_private::TArray<SkUnicode::CodeUnitFlags, true> flags;
    // The interface takes a mutable buffer; copy so the fixture can keep
    // the literal intact.
    std::vector<char> buffer(utf8.begin(), utf8.end());
    if (!unicode.computeCodeUnitFlags(buffer.data(), static_cast<int>(utf8.size()), false,
                                      &flags)) {
        return -1;
    }
    int clusters = 0;
    // The results array has one slot past the text, and the grapheme pass
    // flags that boundary too; clusters are the starts inside the text.
    for (size_t i = 0; i < utf8.size(); ++i) {
        if (flags[static_cast<int>(i)] & SkUnicode::CodeUnitFlags::kGraphemeStart) {
            ++clusters;
        }
    }
    return clusters;
}

void fixtureCombiningMarks(SkUnicode& unicode) {
    // U+0301 is not a cluster of its own: "e" + combining acute is ONE
    // grapheme (the hard-coded backend counted every code point).
    check(graphemeClusters(unicode, "e\u0301") == 1, "\"e\" + U+0301 is one grapheme cluster");
    // Vowel point + tilde + a second base stays two.
    check(graphemeClusters(unicode, "a\u0300\u0303b") == 2, "decomposed accents cluster right");
    // The hard-break / whitespace semantics the lyric layout depends on
    // survive the backend swap.
    skia_private::TArray<SkUnicode::CodeUnitFlags, true> flags;
    std::string text = "ab cd\n";
    std::vector<char> buffer(text.begin(), text.end());
    check(unicode.computeCodeUnitFlags(buffer.data(), static_cast<int>(text.size()), false, &flags),
          "computeCodeUnitFlags answers for ASCII");
    check(flags[flags.size() - 1] & SkUnicode::CodeUnitFlags::kHardLineBreakBefore,
          "trailing newline reports a hard break after it");
}

void fixtureEmoji(SkUnicode& unicode) {
    // thumbs up + skin-tone modifier: one cluster
    check(graphemeClusters(unicode, "\U0001F44D\U0001F3FD") == 1,
          "skin-tone emoji is one grapheme cluster");
    // family ZWJ sequence: one cluster
    check(graphemeClusters(unicode, "\U0001F468\u200D\U0001F469\u200D\U0001F467") == 1,
          "ZWJ family emoji is one grapheme cluster");
    // regional indicator pair (flag): one cluster
    check(graphemeClusters(unicode, "\U0001F1EC\U0001F1E7") == 1,
          "regional-indicator flag is one grapheme cluster");
    // cluster plus ASCII plus cluster: three
    check(graphemeClusters(unicode, "a\U0001F44D\U0001F3FDz") == 3,
          "emoji cluster sits between letters correctly");
}

bool hasScriptFont(SkUnichar codePoint, const char* bcp47) {
    // The same fontconfig manager the engine assembles, asked the same
    // fallback question the FontCollection would ask for this code point.
    sk_sp<SkFontMgr> mgr =
        SkFontMgr_New_FontConfig(nullptr, SkFontScanner_Make_FreeType());
    if (!mgr) {
        return false;
    }
    const char* langs[] = {bcp47};
    return mgr->matchFamilyStyleCharacter(nullptr, SkFontStyle::Normal(), langs, 1, codePoint) !=
           nullptr;
}

void fixtureCjkWrapping(worker::SceneTextEngine& engine, SkUnicode& unicode) {
    if (!hasScriptFont(0x8A69, "zh")) {  // 詩
        std::printf("SKIP: no CJK font installed; CJK wrapping not exercised\n");
        return;
    }
    // 30 ideographic characters with no spaces anywhere: UAX #14 allows a
    // line break between Han characters, so a narrow box must wrap. The
    // hard-coded backend saw no break opportunity and produced one line.
    std::string han;
    for (int i = 0; i < 30; ++i) {
        han += "\u8A69";
    }
    scene::SceneItem item;
    item.lines = {han};
    item.fontSize = 96;
    item.width = 480;   // four glyphs wide at 96 px
    item.height = 400;
    const worker::SceneTextEngine::LaidOutItem laid = engine.layout(item, 1.0f, item.width);
    check(laid.hasText, "CJK fixture laid out");
    check(laid.lineCount >= 2, "unspaced CJK wraps inside a narrow box");
    check(laid.lineCount <= 8, "CJK wrap is sane (not per-character over-split)");

    // ICU reports ideographic code points through the UAX property.
    skia_private::TArray<SkUnicode::CodeUnitFlags, true> flags;
    std::vector<char> buffer(han.begin(), han.end());
    bool ideographic = false;
    if (unicode.computeCodeUnitFlags(buffer.data(), static_cast<int>(buffer.size()), false,
                                     &flags)) {
        for (int i = 0; i < flags.size(); ++i) {
            if (flags[i] & SkUnicode::CodeUnitFlags::kIdeographic) {
                ideographic = true;
                break;
            }
        }
    }
    check(ideographic, "Han characters carry the ideographic flag");
}

void fixtureMixedBidi(worker::SceneTextEngine& engine, SkUnicode& unicode) {
    const std::string text = "\u05E9\u05DC\u05D5\u05DD world \u05E2\u05D1\u05E8\u05D9\u05EA";
    std::vector<SkUnicode::BidiRegion> regions;
    const bool ok = unicode.getBidiRegions(text.c_str(), static_cast<int>(text.size()),
                                           SkUnicode::TextDirection::kLTR, &regions);
    // The hard-coded stub answered "one LTR level-0 region" for everything.
    check(ok && regions.size() >= 3, "mixed Hebrew/Latin yields multiple bidi regions");
    bool shape = ok && regions.size() >= 3;
    if (shape) {
        // LTR base paragraph: Hebrew (RTL) — Latin — Hebrew again, with the
        // spaces riding the paragraph direction.
        check(regions.front().start == 0 && regions.front().level == 1,
              "leading Hebrew is RTL level 1");
        check(regions.back().level == 1, "trailing Hebrew is RTL level 1");
        bool sawLtr = false;
        for (const auto& region : regions) {
            if (region.level == 0) {
                sawLtr = true;
            }
        }
        check(sawLtr, "\"world\" rides level 0");
        // Regions tile the text exactly.
        size_t covered = 0;
        for (const auto& region : regions) {
            covered += region.end - region.start;
        }
        check(covered == text.size(), "bidi regions tile the whole string");
    }

    // UBA L2 with an LTR base does not swap run ORDER, only reverses
    // inside the RTL runs — so the visual run map stays the identity here.
    // Pinning it distinguishes "real UBA" from "whatever" once the base
    // direction rule is applied downstream.
    const SkUnicode::BidiLevel levels[3] = {1, 0, 1};
    int32_t reorderMap[3] = {-1, -1, -1};
    unicode.reorderVisual(levels, 3, reorderMap);
    check(reorderMap[0] == 0 && reorderMap[1] == 1 && reorderMap[2] == 2,
          "LTR-base levels {1,0,1} keep visual run order (UBA L2)");

    // The bidi iterator the old backend refused to hand out now exists.
    check(unicode.makeBidiIterator(text.c_str(), static_cast<int>(text.size()),
                                   SkBidiIterator::kLTR) != nullptr,
          "makeBidiIterator returns a real iterator");

    // The engine still lays this mixed string out with both scripts
    // covered by the FontCollection's fallback (Noto Sans Hebrew).
    if (hasScriptFont(0x05E9, "he")) {  // ש
        scene::SceneItem item;
        item.lines = {text};
        item.fontSize = 48;
        item.width = 1600;
        item.height = 200;
        const auto laid = engine.layout(item, 1.0f, item.width);
        check(laid.hasText && laid.lineCount == 1, "mixed bidi lyric lays out on one line");
    } else {
        std::printf("SKIP: no Hebrew font installed; mixed-bidi layout not exercised\n");
    }
}

void fixtureBreakIterators(SkUnicode& unicode) {
    // The hard-coded backend returned nullptr for every break iterator.
    auto graphemes = unicode.makeBreakIterator(SkUnicode::BreakType::kGraphemes);
    check(graphemes != nullptr, "grapheme break iterator exists");
    if (graphemes) {
        const std::string text = "\U0001F468\u200D\U0001F469\u200D\U0001F467 hi";
        // Feed the iterator the UTF-8 form via its setText overload.
        if (graphemes->setText(text.c_str(), static_cast<int>(text.size()))) {
            std::vector<size_t> breaks;
            for (size_t pos = graphemes->first(); !graphemes->isDone(); pos = graphemes->next()) {
                breaks.push_back(pos);
            }
            // family cluster, space, h, i, end
            check(breaks.size() == 5, "grapheme iterator yields 5 breaks for family+\" hi\"");
        } else {
            check(false, "grapheme iterator accepts UTF-8 text");
        }
    }
    auto lines = unicode.makeBreakIterator(SkUnicode::BreakType::kLines);
    check(lines != nullptr, "line break iterator exists");
}

// Task TL.3: the display capitals transform. One pass through the block's
// own ICU backend with the ROOT locale, before shaping; authored line
// breaks and empty lines survive, and the source lines are never rewritten.
void fixtureUppercaseDisplay(worker::SceneTextEngine& engine) {
    scene::SceneItem item;
    item.lines = {"stra\xC3\x9F" "e", "", "\xC3\xBC" "ber", "i"};
    // "straße" -> "STRASSE" (ß expands to SS; Unicode case mapping, not a
    // byte-wise toupper), the empty line stays empty, "über" -> "ÜBER" and
    // root-locale "i" -> "I" (a Turkish locale would answer "İ").
    item.allCaps = true;
    std::vector<std::string> lines;
    std::string error;
    if (!check(engine.displayLines(item, &lines, &error),
               "the display transform answers: " + error)) {
        return;
    }
    check(lines.size() == 4, "the transform preserves the line count");
    check(lines[0] == "STRASSE", "ß expands to SS through ICU's full uppercase");
    check(lines[1].empty(), "an authored empty line stays empty");
    check(lines[2] == "\xC3\x9C" "BER", "a non-ASCII letter uppercases (ü -> Ü)");
    check(lines[3] == "I", "root-locale uppercase of i is I, never a dotted capital");

    // The SOURCE is untouched: allCaps is a display flag, not an edit.
    check(item.lines[0] == "stra\xC3\x9F" "e" && item.lines[3] == "i",
          "the authored lines keep their exact original case");

    // Off is the identity, and it copies rather than aliases.
    scene::SceneItem plain = item;
    plain.allCaps = false;
    std::vector<std::string> same;
    check(engine.displayLines(plain, &same, &error) && same == item.lines,
          "the transform is the identity when allCaps is off");

    // The transform must not slip past the scene contract's own budgets.
    // Uppercase can lengthen a line, so the check is on the RESULT.
    scene::SceneItem longLine;
    longLine.allCaps = true;
    {
        // U+01F0 (ǰ) uppercases to "J" + combining caron: two bytes grow to
        // three, so a source at the byte budget exceeds it after the
        // transform. This is the expansion the budget guard exists for.
        std::string word;
        for (int i = 0; i < static_cast<int>(scene::kMaxLineChars) / 2; ++i) {
            word += "\xC7\xB0";
        }
        longLine.lines = {std::move(word)};
    }
    std::vector<std::string> refused;
    error.clear();
    check(!engine.displayLines(longLine, &refused, &error) &&
              error.find("line") != std::string::npos &&
              error.find(std::to_string(scene::kMaxLineChars)) != std::string::npos,
          "a transform that overflows the line budget is refused naming the budget");

    // The item budget is the sum over lines, and the scene budget sums the
    // items — both named, both refusing before any shaping.
    scene::SceneItem fatItem;
    fatItem.allCaps = true;
    fatItem.lines.assign(33, std::string(512, 'a'));  // 16896 > 16 KiB
    check(!engine.displayLines(fatItem, &refused, &error) &&
              error.find("item") != std::string::npos,
          "an item over the per-item budget is refused naming the item budget");
    scene::Scene crowded;
    for (int i = 0; i < 17; ++i) {
        scene::SceneItem one;
        one.allCaps = true;
        one.lines.assign(4, std::string(1000, 'a'));  // 17*4*1000 > 64 KiB
        crowded.items.push_back(std::move(one));
    }
    error.clear();
    check(!engine.validateSceneText(crowded, &error) &&
              error.find("scene") != std::string::npos,
          "a transformed scene over the scene-wide budget is refused naming it");
    scene::Scene calm = crowded;
    calm.items.resize(2);
    check(engine.validateSceneText(calm, &error),
          "and a scene inside the budgets validates");
}

}  // namespace

int main() {
    worker::SceneTextEngine engine;
    std::string error;
    if (!engine.create(&error)) {
        std::fprintf(stderr, "unicode_text_tests: no usable font setup (%s); skipping\n",
                     error.c_str());
        return 77;
    }
    const sk_sp<SkUnicode>& unicode = engine.unicode();
    if (!unicode) {
        std::fprintf(stderr, "FAIL: engine.unicode() is null\n");
        return 1;
    }

    fixtureCombiningMarks(*unicode);
    fixtureEmoji(*unicode);
    fixtureCjkWrapping(engine, *unicode);
    fixtureMixedBidi(engine, *unicode);
    fixtureBreakIterators(*unicode);
    fixtureUppercaseDisplay(engine);

    if (failures != 0) {
        std::fprintf(stderr, "unicode_text_tests: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("unicode_text_tests: all fixtures passed\n");
    return 0;
}
