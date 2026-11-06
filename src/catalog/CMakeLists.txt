# openshow::catalog — the shared song Library (tasks SL.1–SL.2).
#
# Qt-free like the domain: the model, its history and its package codec run
# with no display, no network and no database server. The UI adapter sits
# beside the other DocumentController-family objects in src/ui.
add_library(openshow_catalog STATIC
  catalog.cpp
  catalogstorage.cpp
  history.cpp
)
add_library(openshow::catalog ALIAS openshow_catalog)

target_include_directories(openshow_catalog PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(openshow_catalog PUBLIC Boost::json)
target_compile_features(openshow_catalog PUBLIC cxx_std_20)
