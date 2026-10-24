/*
 * The 1.x demo content as a fixture (task 2.4c).

 * Through task 1.x the audience picture was code: TestScene drew a title,
 * a line of what the frame proved, and a shaping sample directly with
 * SkParagraph. From here the same content is *data* — a Documents the
 * frontend owns, compiled through the ordinary compileSlide path. This is
 * the "demo content becomes a fixture that builds a scene" graduation
 * step from PLAN.md, and it is shared: the Qt demo adapter and the test
 * suite both drive the exact same compile, so the scene the UI prepares
 * is byte-for-byte the one the tests prove the worker accepts.
 */

#pragma once

#include <cstddef>
#include <string>

#include "openshow/domain/documents.h"
#include "openshow/scene/scene.h"

namespace openshow::scene {

// Builds the demo service: one show reproducing the 1.x picture — the
// "OpenShowQt" title slide, a lyric stanza styled through a show default,
// and a black interlude — plus the styles they resolve through. Every
// slide is compiled with compileSlide like any real content.
domain::Documents demoDocuments();

// The demo show is the only show the fixture creates.
domain::Id demoShowId(const domain::Documents& documents);

// How many slides the demo show carries.
std::size_t demoSlideCount(const domain::Documents& documents);

// Compiles the demo slide at `index` (0-based, within demoSlideCount).
// Fails with an error if the index is out of range.
bool compileDemoSlide(const domain::Documents& documents, std::size_t index, Scene* out,
                      std::string* error = nullptr);

}  // namespace openshow::scene
