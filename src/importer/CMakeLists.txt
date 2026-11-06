# openshow::importer — online content extraction (milestone BI).
#
# Qt-free on purpose, like the domain: the passage parser is a pure
# function over fetched bytes so its regressions run headless on synthetic
# fixtures, and the fetcher that later joins it is deliberately NOT here —
# network I/O belongs to the frontend (task BI.2).
add_library(openshow_importer STATIC
  biblepassage.cpp
  bookindex.cpp
  lyrics.cpp
  scripturepagination.cpp
  songpagination.cpp
  textflow.cpp
)
target_include_directories(openshow_importer PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
# Task TS.3: the flow's typography IS the domain's — FlowStyle and
# MeasureRequest carry domain::TextStroke/TextShadow and vet the scene
# bands through domain's own predicates. "Identical font/style inputs"
# is a type-level claim, not a comment; domain is Qt-free like this lib.
target_link_libraries(openshow_importer PUBLIC openshow_domain openshow_catalog)
target_compile_features(openshow_importer PUBLIC cxx_std_20)
add_library(openshow::importer ALIAS openshow_importer)
