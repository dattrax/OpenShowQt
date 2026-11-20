/*
 * A canonical spelling of a JSON value (TC.9).
 *
 * Derived data rides beside authored manifests, and a cache-only write has to
 * answer one question: "is the authored half still exactly what this session
 * last wrote or loaded?" boost::json does not preserve member order across a
 * parse and a re-serialize (an erase or an assignment can move a member), so
 * comparing raw bytes would refuse perfectly valid files — and comparing
 * nothing would overwrite an external edit. The canonical form sorts object
 * members by name recursively and keeps every scalar exactly as boost::json
 * spells it; hashing that is the stable answer both manifests share.
 */

#pragma once

#include <string>

#include <boost/json.hpp>

namespace openshow::domain {

// Object members sorted by key, arrays in order, scalars in boost::json's own
// spelling.
std::string canonicalJsonText(const boost::json::value& value);

// Lowercase hexadecimal SHA-256 of canonicalJsonText().
std::string canonicalJsonDigest(const boost::json::value& value);

}  // namespace openshow::domain
