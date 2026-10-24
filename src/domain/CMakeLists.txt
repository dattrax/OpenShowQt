add_library(openshow_domain STATIC
  documents.cpp
  storage.cpp
  version.cpp
)
add_library(openshow::domain ALIAS openshow_domain)

target_include_directories(openshow_domain PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Boost.JSON is the project's one JSON library (same choice as
# openshow::protocol); storage.cpp is the only domain user so far.
target_link_libraries(openshow_domain PUBLIC Boost::json)

# Consumers of the header need the version string too.
target_compile_definitions(openshow_domain PUBLIC
  OPENSHOW_VERSION_STRING="${CMAKE_PROJECT_VERSION}"
)
