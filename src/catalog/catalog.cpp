#include "openshow/catalog/catalog.h"

#include <algorithm>

namespace openshow::catalog {

namespace {

void fail(const std::string& message, std::string* error) {
    if (error != nullptr) {
        *error = message;
    }
}

bool isAsciiSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

bool allDigits(const std::string& text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) {
        return c >= '0' && c <= '9';
    });
}

// What a revision move costs. Kept in one place so a new verb cannot
// accidentally advance the content revision for a bookkeeping edit.
void bumpRecord(CatalogItem& item) { ++item.recordRevision; }
void bumpContent(CatalogItem& item) {
    ++item.recordRevision;
    ++item.contentRevision;
}

CatalogItem* findMutable(std::vector<CatalogItem>& items, const Id& id) {
    for (CatalogItem& item : items) {
        if (item.id == id) {
            return &item;
        }
    }
    return nullptr;
}

}  // namespace

std::string trimAscii(const std::string& text) {
    std::size_t begin = 0;
    while (begin < text.size() && isAsciiSpace(text[begin])) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && isAsciiSpace(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool isBlank(const std::string& text) {
    return std::all_of(text.begin(), text.end(), isAsciiSpace);
}

bool validSong(const std::string& title, const std::vector<Stanza>& stanzas,
               const SongMetadata& metadata, std::string* error) {
    if (isBlank(title)) {
        fail("a song needs a title", error);
        return false;
    }
    if (title.size() > kMaxTitleBytes) {
        fail("the title is longer than " + std::to_string(kMaxTitleBytes) +
                 " bytes",
             error);
        return false;
    }
    if (stanzas.empty()) {
        fail("a song needs at least one stanza of words", error);
        return false;
    }
    if (stanzas.size() > kMaxStanzas) {
        fail("a song may hold at most " + std::to_string(kMaxStanzas) +
                 " stanzas",
             error);
        return false;
    }
    bool anyWords = false;
    for (std::size_t s = 0; s < stanzas.size(); ++s) {
        const Stanza& stanza = stanzas[s];
        if (stanza.lines.size() > kMaxStanzaLines) {
            fail("stanza " + std::to_string(s + 1) + " holds more than " +
                     std::to_string(kMaxStanzaLines) + " lines",
                 error);
            return false;
        }
        for (std::size_t l = 0; l < stanza.lines.size(); ++l) {
            if (stanza.lines[l].size() > kMaxLineBytes) {
                fail("stanza " + std::to_string(s + 1) + " line " +
                         std::to_string(l + 1) + " is longer than " +
                         std::to_string(kMaxLineBytes) + " bytes",
                     error);
                return false;
            }
            if (!isBlank(stanza.lines[l])) {
                anyWords = true;
            }
        }
    }
    if (!anyWords) {
        fail("a song needs at least one line of words", error);
        return false;
    }
    const std::string ccli = trimAscii(metadata.ccli);
    if (!ccli.empty()) {
        // Empty is legal (a source without a CCLI number). Anything present
        // must be the NUMBER: "CCLI Song #123" belongs to the parser, never
        // to the stored field.
        if (!allDigits(ccli)) {
            fail("the CCLI number must be digits only", error);
            return false;
        }
        if (ccli.size() > kMaxCcliDigits) {
            fail("the CCLI number is longer than " +
                     std::to_string(kMaxCcliDigits) + " digits",
                 error);
            return false;
        }
    }
    if (metadata.authors.size() > kMaxMetadataBytes ||
        metadata.copyright.size() > kMaxMetadataBytes) {
        fail("the author/copyright fields are longer than " +
                 std::to_string(kMaxMetadataBytes) + " bytes",
             error);
        return false;
    }
    return true;
}

const CatalogItem* Catalog::find(const Id& id) const {
    for (const CatalogItem& item : fItems) {
        if (item.id == id) {
            return &item;
        }
    }
    return nullptr;
}

std::vector<Id> Catalog::itemIds() const {
    std::vector<Id> ids;
    ids.reserve(fItems.size());
    for (const CatalogItem& item : fItems) {
        ids.push_back(item.id);
    }
    return ids;
}

std::vector<Id> Catalog::idsInCategory(Category category) const {
    std::vector<Id> ids;
    for (const CatalogItem& item : fItems) {
        if (item.category == category) {
            ids.push_back(item.id);
        }
    }
    return ids;
}

std::size_t Catalog::count(Category category) const {
    return std::count_if(fItems.begin(), fItems.end(),
                         [category](const CatalogItem& item) {
                             return item.category == category;
                         });
}

Id Catalog::createSong(const std::string& title, const std::vector<Stanza>& stanzas,
                       const SongMetadata& metadata, std::string* error) {
    if (!validSong(title, stanzas, metadata, error)) {
        return {};
    }
    CatalogItem item;
    item.id = nextId();
    item.category = Category::Songs;
    item.title = trimAscii(title);
    item.kind = ContentKind::Lyrics;
    item.stanzas = stanzas;
    item.metadata = metadata;
    item.metadata.ccli = trimAscii(metadata.ccli);
    item.recordRevision = 1;
    item.contentRevision = 1;
    fItems.push_back(std::move(item));
    return fItems.back().id;
}

bool Catalog::updateSong(const Id& id, const std::vector<Stanza>& stanzas,
                         const SongMetadata& metadata, std::string* error) {
    CatalogItem* item = findMutable(fItems, id);
    if (item == nullptr) {
        fail("no catalog item named " + id, error);
        return false;
    }
    if (!validSong(item->title, stanzas, metadata, error)) {
        return false;
    }
    SongMetadata normalized = metadata;
    normalized.ccli = trimAscii(normalized.ccli);
    if (normalized == item->metadata && stanzas == item->stanzas) {
        return true;  // unchanged content costs no undo step
    }
    item->stanzas = stanzas;
    item->metadata = std::move(normalized);
    bumpContent(*item);
    return true;
}

bool Catalog::setMetadata(const Id& id, const SongMetadata& metadata,
                          std::string* error) {
    CatalogItem* item = findMutable(fItems, id);
    if (item == nullptr) {
        fail("no catalog item named " + id, error);
        return false;
    }
    if (!validSong(item->title, item->stanzas, metadata, error)) {
        return false;
    }
    SongMetadata normalized = metadata;
    normalized.ccli = trimAscii(normalized.ccli);
    if (normalized == item->metadata) {
        return true;  // no-op succeeds without an undo step
    }
    item->metadata = std::move(normalized);
    bumpContent(*item);
    return true;
}

bool Catalog::rename(const Id& id, const std::string& title, std::string* error) {
    CatalogItem* item = findMutable(fItems, id);
    if (item == nullptr) {
        fail("no catalog item named " + id, error);
        return false;
    }
    if (isBlank(title)) {
        fail("a song needs a title", error);
        return false;
    }
    if (title.size() > kMaxTitleBytes) {
        fail("the title is longer than " + std::to_string(kMaxTitleBytes) +
                 " bytes",
             error);
        return false;
    }
    const std::string trimmed = trimAscii(title);
    if (trimmed == item->title) {
        return true;  // an unchanged rename costs nothing
    }
    item->title = trimmed;
    bumpRecord(*item);
    return true;
}

bool Catalog::moveToCategory(const Id& id, Category category, std::string* error) {
    CatalogItem* item = findMutable(fItems, id);
    if (item == nullptr) {
        fail("no catalog item named " + id, error);
        return false;
    }
    if (item->category == category) {
        return true;  // repeating a move is a no-op, not an undo step
    }
    item->category = category;
    bumpRecord(*item);  // content revision deliberately unchanged
    return true;
}

bool Catalog::remove(const Id& id, std::string* error) {
    const auto found = std::find_if(fItems.begin(), fItems.end(),
                                    [&id](const CatalogItem& item) {
                                        return item.id == id;
                                    });
    if (found == fItems.end()) {
        fail("no catalog item named " + id, error);
        return false;
    }
    fItems.erase(found);
    return true;
}

Id Catalog::nextId() {
    // A kind prefix plus a store-wide counter, the domain's own convention:
    // ids stay unique across every item kind and are reproducible in tests.
    return "item" + std::to_string(fNextOrdinal++);
}

}  // namespace openshow::catalog
