/*
 * Version information shared by the Qt frontend and the Qt-free worker.
 *
 * The document model itself (shows, slides, groups, styles, media and
 * playlists) arrives with milestone M2. This header exists so both binaries
 * report one version and so the Qt-free boundary has a symbol to test against.
 */

#pragma once

namespace openshow::domain {

// Application version from the CMake project version, e.g. "0.1.0".
const char* applicationVersion();

}  // namespace openshow::domain
