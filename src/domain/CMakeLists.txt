add_library(openshow_domain STATIC
  version.cpp
)
add_library(openshow::domain ALIAS openshow_domain)

target_include_directories(openshow_domain PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Consumers of the header need the version string too.
target_compile_definitions(openshow_domain PUBLIC
  OPENSHOW_VERSION_STRING="${CMAKE_PROJECT_VERSION}"
)
