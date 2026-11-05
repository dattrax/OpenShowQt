// Task BI.1 — the passage parser's regressions, on synthetic fixtures.
//
// The fixtures reproduce the STRUCTURE of a Bible Gateway passage page (a
// .passage-text container, version-XXX, per-verse spans classed "text
// Book-c-v", chapternum/versenum furniture, inline cross-reference sups,
// a publisher-info-bottom attribution block) with invented sentences, per
// the plan's rule that no fetched passage is ever stored in the repository.
// The live shapes behind this structure are recorded in
// docs/ui/bible-import-plan.md.
#include "openshow/importer/biblepassage.h"
#include "openshow/importer/bookindex.h"

#include <cstdio>
#include <string>

namespace {

int gFailed = 0;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++gFailed;
    }
    return condition;
}

// --- fixtures --------------------------------------------------------------

// A passage page in the live shape: heading spans wearing the same class
// token as the verse they head, verse 1 opening the chapter with a
// chapternum span, later verses carrying versenum sups, inline
// cross-references with escaped anchor HTML in a data-link attribute, a
// chapter boundary mid-page, entities and literal Unicode inside the
// words, and the attribution block far below outside the passage.
std::string passagePage(const std::string& version) {
    static const std::string kTemplate = R"HTML(<!DOCTYPE html>
<html><head><title>First Test 1 &amp; 2 EXAMPLE - Bible Gateway</title>
<script>var tracker = { verse: "text 1Test-9-9 not scripture" };</script>
</head><body>
<nav class="top-nav"><a href="/">Bible Gateway home</a> navigation furniture</nav>
<div class="passage-text">
<div class='passage-content passage-class-0'><div class="version-VERSION result-text-style-normal text-html">
 <h3><span id="t-1" class="text 1Test-1-1">The Opening</span></h3>
 <p class="chapter-1"><span class="text 1Test-1-1"><span class="chapternum">1&#160;</span>In the beginning the test wrote its&#160;words, and the words were <sup class='crossreference' data-cr='#tA' data-link='(&lt;a href=&quot;#tA&quot;&gt;A&lt;/a&gt;)'><a href="#tA">A</a></sup> good.</span></p>
 <p><span id="t-2" class="text 1Test-1-2"><sup class="versenum">2&#160;</sup>And the second verse said &ldquo;I keep my em&#8212;dash&rdquo;, with a quote&rsquo;s apostrophe.</span></p>
 <h3><span id="t-3" class="text 1Test-2-1">Second Chapter</span></h3>
 <p><span class="text 1Test-2-1"><sup class="versenum">2&#160;</sup>A verse past the chapter break&#8230; continues here.</span></p>
</div></div>
</div>
<div class="copyright-table"><div class="publisher-info-bottom with-bga">
<strong><a href="/versions/Example-Test-Bible/">Example Test Bible</a> (VERSION)</strong>
<p>Test&#160;Bible &#169;2026 by Example Press. Used by permission. All rights reserved.</p>
</div></div>
<footer>site footer, not scripture</footer>
</body></html>)HTML";
    std::string html{kTemplate};
    for (std::size_t at = html.find("VERSION"); at != std::string::npos; at = html.find("VERSION")) {
        html.replace(at, 7, version);
    }
    return html;
}

// Markup moved on: the container is still promised, the verse class is not.
const char* const kChangedMarkup =
    R"HTML(<div class="passage-text"><div class="newly-spun layout">
    <span class="line-of-some-kind">text nobody marked as a verse</span>
    </div></div>)HTML";

// The invalid-reference page answers without a passage container at all,
// carrying its own honest sentence.
const char* const kNoResultsPage =
    R"HTML(<html><body><div class="search-suggestions">
    <p /><h3>No results found.</h3>
    <p>No valid results were found for your search.</p>
    </div><div class="passage-not-text">chatter</div></body></html>)HTML";

const char* const kBlockedPage =
    R"HTML(<html><body><h1>Access Restricted</h1>
    <p>Your request has been flagged as unusual traffic.</p></body></html>)HTML";

}  // namespace

int main() {
    using namespace openshow::importer;

    // The live shape parses to ordered verses with clean words.
    const PassageParse good = parsePassagePage(passagePage("EXAMPLE"), "EXAMPLE");
    check(good.ok, "a passage page in the live shape parses");
    if (good.ok) {
        check(good.passage.verses.size() == 3, "the page's three verse spans are three verses");
        check(good.passage.verses[0].reference == "1:1" &&
                  good.passage.verses[1].reference == "1:2" &&
                  good.passage.verses[2].reference == "2:1",
              "verse boundaries come from the page's own c-v tokens, chapter break included");
        check(good.passage.verses[0].text ==
                  "In the beginning the test wrote its words, and the words were good.",
              "the chapter number and the cross-reference anchor leave the words alone");
        check(good.passage.verses[1].text ==
                  "And the second verse said \u201cI keep my em\u2014dash\u201d, with a quote\u2019s "
                  "apostrophe.",
              "named and numeric entities decode to their Unicode, not to ASCII guesses");
        check(good.passage.verses[2].text ==
                  "A verse past the chapter break\u2026 continues here.",
              "literal multi-byte text survives the parse whole");
        check(good.passage.translation == "EXAMPLE", "the page answers with its translation");
        check(good.passage.attribution.find("Example Test Bible") != std::string::npos &&
                  good.passage.attribution.find("permission") != std::string::npos,
              "the attribution block arrives as the licence text it is");
    }

    // Everything the page is NOT stays out: navigation, headings, scripts,
    // the site footer and the search furniture.
    {
        const std::string joined = [&good] {
            std::string all;
            for (const PassageVerse& v : good.passage.verses) {
                all += v.text;
            }
            return all;
        }();
        check(joined.find("The Opening") == std::string::npos &&
                  joined.find("Second Chapter") == std::string::npos,
              "section headings are not scripture and never ride in a verse");
        check(joined.find("furniture") == std::string::npos &&
                  joined.find("not scripture") == std::string::npos &&
                  joined.find("footer") == std::string::npos,
              "navigation, script text and the footer are outside the passage container "
              "and stay out");
    }

    // A page that answered a different translation is refused outright.
    // A version token longer than string's inline buffer must keep its
    // attribute storage alive; a view of a temporary substring is dangling.
    const std::string longVersion = "EXAMPLE_LONG_TRANSLATION_IDENTIFIER";
    const auto longPage = passagePage(longVersion);
    const auto longAnswer = parsePassagePage(longPage, longVersion);
    check(longAnswer.ok && longAnswer.passage.translation == longVersion,
          "long translation class tokens parse from live attribute storage");
    const auto longMismatch = parsePassagePage(longPage, "EXAMPLE");
    check(!longMismatch.ok && longMismatch.error.find(longVersion) != std::string::npos,
          "a long mismatched translation is still named and refused correctly");
    const PassageParse mismatch = parsePassagePage(passagePage("OTHER"), "EXAMPLE");
    check(!mismatch.ok && mismatch.error.find("OTHER") != std::string::npos &&
              mismatch.error.find("substitutes") != std::string::npos,
          "a translation the operator did not ask for is refused, naming both");
    check(mismatch.passage.verses.empty(), "the refused passage carries no verses onward");

    // Invalid reference, blocked request, changed markup: three failures,
    // three honest sentences, nothing imported.
    const PassageParse missing = parsePassagePage(kNoResultsPage, "EXAMPLE");
    check(!missing.ok && missing.error.find("no passage") != std::string::npos,
          "an invalid reference reports that nothing was found");
    const PassageParse blocked = parsePassagePage(kBlockedPage, "EXAMPLE");
    check(!blocked.ok && blocked.error.find("refused") != std::string::npos,
          "a blocked page reports the refusal, not a passage");
    const PassageParse moved = parsePassagePage(kChangedMarkup, "EXAMPLE");
    check(!moved.ok && moved.error.find("markup") != std::string::npos,
          "markup the parser no longer recognises fails rather than guessing");

    // An empty body (a reply that arrived with no bytes) says so.
    check(!parsePassagePage("", "EXAMPLE").ok, "an empty page is refused");

    // A verse whose text is only whitespace is dropped; the real verse
    // that follows it survives untouched.
    {
        const std::string sparse =
            R"HTML(<div class="passage-text"><div class="version-EXAMPLE">
            <p><span class="text 1Test-3-1">   </span>
            <span class="text 1Test-3-2">Short.</span></p></div></div>)HTML";
        const PassageParse blanks = parsePassagePage(sparse, "EXAMPLE");
        check(blanks.ok && blanks.passage.verses.size() == 1 &&
                  blanks.passage.verses[0].text == "Short." &&
                  blanks.passage.verses[0].reference == "3:2",
              "a whitespace-only verse is dropped and the numbered verse after it survives");
    }

    // The translation menu the importer offers.
    const std::vector<Translation> menu = translations();
    bool hasNiv = false;
    bool hasKjv = false;
    for (const Translation& t : menu) {
        if (std::string{t.version} == "NIV") {
            hasNiv = true;
        }
        if (std::string{t.version} == "KJV") {
            hasKjv = true;
        }
    }
    check(!menu.empty() && hasNiv && hasKjv,
          "the menu carries the plan's example translation and the public-domain one");

    // --- BIF.3: the built-in canon answers when the operator names a book --
    {
        using openshow::importer::canonicalBook;
        using openshow::importer::suggestBooks;

        auto topName = [](const std::string& typed) -> std::string {
            const auto rows = suggestBooks(typed);
            return rows.empty() ? std::string() : rows[0].name;
        };

        // The same book, four ways of spelling its ordinal.
        check(topName("1 cor") == "1 Corinthians", "'1 cor' names 1 Corinthians");
        check(topName("1cor") == "1 Corinthians", "'1cor' names 1 Corinthians");
        check(topName("i cor") == "1 Corinthians", "'i cor' names 1 Corinthians");
        check(topName("1st cor") == "1 Corinthians", "'1st cor' names 1 Corinthians");
        check(topName("  1CoR  ") == "1 Corinthians",
              "case and stray whitespace are nothing to the matcher");

        // Common abbreviations off the shelf.
        check(topName("jn") == "John", "'jn' names John");
        check(topName("phm") == "Philemon", "'phm' names Philemon");
        check(topName("song") == "Song of Solomon", "'song' names Song of Solomon");
        check(topName("gen") == "Genesis", "'gen' names Genesis");
        check(topName("1jn") == "1 John", "'1jn' names 1 John, not John");
        check(topName("iii john") == "3 John", "'iii john' names 3 John");
        check(topName("2 Chr") == "2 Chronicles", "'2 Chr' names 2 Chronicles");

        // Numbered books offer in order, and only the numbered ones.
        const auto ones = suggestBooks("1 ");
        bool allFirst = !ones.empty();
        bool sawSamuel = false;
        bool sawCorinthians = false;
        for (const auto& row : ones) {
            if (row.name.compare(0, 2, "1 ") != 0) {
                allFirst = false;
            }
            if (row.name == "1 Samuel") {
                sawSamuel = true;
            }
            if (row.name == "1 Corinthians") {
                sawCorinthians = true;
            }
        }
        check(allFirst, "prefix '1 ' offers only first books");
        check(sawSamuel && sawCorinthians,
              "prefix '1 ' reaches from 1 Samuel to 1 Corinthians");
        const auto genesisRun = suggestBooks("G");
        check(!genesisRun.empty() && genesisRun[0].name == "Genesis",
              "books come back in canon order — Genesis heads the list");

        // The chapter tail survives the completion whole.
        const auto tailed = suggestBooks("1 cor 1:1-6");
        check(!tailed.empty() && tailed[0].completion == "1 Corinthians 1:1-6",
              "the completion keeps the typed chapter and verses exactly");
        const auto chaptered = suggestBooks("jn 3");
        check(!chaptered.empty() && chaptered[0].completion == "John 3",
              "a trailing chapter is not mistaken for part of the book");

        // Unmatched text is an empty answer — never a refusal, never a guess.
        check(suggestBooks("qqq").empty(), "text matching nothing suggests nothing");
        check(suggestBooks("").empty(), "nothing typed suggests nothing");
        check(!canonicalBook("1cor").has_value() || canonicalBook("1cor").value() == "1 Corinthians",
              "the canonical lookup agrees with the list");
        check(canonicalBook("jn") == "John" && canonicalBook("1 co") == "1 Corinthians",
              "the canonical lookup reads the abbreviations too");
        check(!canonicalBook("book of lost words").has_value(),
              "unknown text has no canonical book — and the lookup would still try it as typed");
    }

    if (gFailed == 0) {
        std::printf("bible_import_tests: all fixtures passed\n");
        return 0;
    }
    std::printf("bible_import_tests: %d FAILURES\n", gFailed);
    return 1;
}
