add_library(openshow_scene STATIC
  scene.cpp
)
add_library(openshow::scene ALIAS openshow_scene)

target_include_directories(openshow_scene PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# The compiled scene is the domain's output type: no Qt, no GPU, no
# files — Boost.JSON for the wire form, same choice as the domain and
# the protocol.
target_link_libraries(openshow_scene PUBLIC openshow::domain Boost::json)
