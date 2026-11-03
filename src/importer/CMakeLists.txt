# openshow::importer — online content extraction (milestone BI).
#
# Qt-free on purpose, like the domain: the passage parser is a pure
# function over fetched bytes so its regressions run headless on synthetic
# fixtures, and the fetcher that later joins it is deliberately NOT here —
# network I/O belongs to the frontend (task BI.2).
add_library(openshow_importer STATIC
  biblepassage.cpp
  bookindex.cpp
  scripturepagination.cpp
  textflow.cpp
)
target_include_directories(openshow_importer PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(openshow_importer PUBLIC cxx_std_20)
add_library(openshow::importer ALIAS openshow_importer)
