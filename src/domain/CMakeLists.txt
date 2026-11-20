add_library(openshow_domain STATIC
  documents.cpp
  history.cpp
  jsoncanonical.cpp
  refloweligibility.cpp
  reflowsource.cpp
  storage.cpp
  themesnapshot.cpp
  thumbnails.cpp
  version.cpp
)
add_library(openshow::domain ALIAS openshow_domain)

target_include_directories(openshow_domain PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Boost.JSON is the project's one JSON library (same choice as
# openshow::protocol); storage.cpp is the only domain user so far.
target_link_libraries(openshow_domain PUBLIC Boost::json PRIVATE PkgConfig::FONTCONFIG)

# Invalidate persisted renderer pixels when the renderer is rebuilt even if
# the release version stays unchanged during development.
file(GLOB thumbnail_renderer_sources CONFIGURE_DEPENDS
  "${PROJECT_SOURCE_DIR}/src/worker/render/*.cpp"
  "${PROJECT_SOURCE_DIR}/src/worker/render/*.h")
set(thumbnail_renderer_signature "")
foreach(source IN LISTS thumbnail_renderer_sources)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")
  file(SHA256 "${source}" source_digest)
  string(APPEND thumbnail_renderer_signature "${source_digest}")
endforeach()
string(SHA256 thumbnail_renderer_signature "${thumbnail_renderer_signature}")
target_compile_definitions(openshow_domain PRIVATE
  OPENSHOW_THUMBNAIL_RENDERER_SIGNATURE="${thumbnail_renderer_signature}")

# Consumers of the header need the version string too.
target_compile_definitions(openshow_domain PUBLIC
  OPENSHOW_VERSION_STRING="${CMAKE_PROJECT_VERSION}"
)
