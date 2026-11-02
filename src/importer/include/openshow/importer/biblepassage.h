// Online Bible passage extraction (milestone BI, task BI.1).
//
// One requested passage page from Bible Gateway, turned structurally into
// ordered verses — nothing else. This header owns the shape the rest of the
// importer (network, pagination, UI) passes around; the parse itself is a
// pure function over the fetched bytes so it can be regression-tested on
// synthetic fixtures with no network and no stored fetched passage.
#ifndef OPENSHOW_IMPORTER_BIBLEPASSAGE_H
#define OPENSHOW_IMPORTER_BIBLEPASSAGE_H

#include <string>
#include <string_view>
#include <vector>

namespace openshow::importer {

// One scripture verse as the passage page states it.
struct PassageVerse {
    // "chapter:verse" as the page's own per-verse class token says it
    // (e.g. "1:2" for 1 Corinthians 1:2). The book itself is carried by
    // the reference the operator looked up, not invented here.
    std::string reference;
    // Plain text: markup removed, entities decoded, whitespace collapsed
    // to single spaces. Unicode punctuation and letters survive exactly —
    // there is no conversion that could lose them.
    std::string text;
};

// The whole parsed passage.
struct Passage {
    // The translation the PAGE answered with (its version-XXX container),
    // upper-cased — what was fetched, not merely what was asked for.
    std::string translation;
    // The publisher/attribution block of the page as plain text: version
    // name plus the copyright statement the translation's licence
    // requires. Kept separate from the verses so the schedule item can
    // carry it without polluting slide text.
    std::string attribution;
    std::vector<PassageVerse> verses;
};

// The outcome of one page parse. A refusal is total: on failure the
// importer must show the error and import nothing — the parser never
// salvages "whatever text was nearby" from a page that stopped looking
// like a passage.
struct PassageParse {
    bool ok = false;
    std::string error;  // operator-facing sentence
    Passage passage;
};

// Parse a Bible Gateway passage page (the UTF-8 HTML body).
// `requestedVersion` is the version parameter sent with the lookup; the
// result must agree with it or the parse refuses — a page that answered a
// different translation is never silently accepted.
//
// What is excluded by construction, per the plan: navigation, adverts,
// headings, footnotes, cross-reference markers and study material —
// only the inside of the passage container is read, and within it only
// verse spans, with <sup>/<a>/verse-number spans skipped whole.
PassageParse parsePassagePage(std::string_view html, std::string_view requestedVersion);

// One translation the importer offers. `version` is the Bible Gateway
// version parameter; `attributionNote` is advisory for the UI list — the
// authoritative attribution always comes from the fetched page itself.
struct Translation {
    const char* version;
    const char* name;
    const char* attributionNote;
};

// The importer's translation menu. Every one of these is served by
// Bible Gateway's passage URL under its `version` parameter; the set is
// deliberately short, and a lookup of any entry must fail loudly rather
// than substitute another if the page ever stops carrying it.
std::vector<Translation> translations();

}  // namespace openshow::importer

#endif  // OPENSHOW_IMPORTER_BIBLEPASSAGE_H
