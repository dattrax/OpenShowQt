add_library(openshow_protocol STATIC
  protocol.cpp
)
add_library(openshow::protocol ALIAS openshow_protocol)

target_include_directories(openshow_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Boost.JSON is the only JSON library in the project: the frontend uses it
# through this library, the worker uses it directly.
target_link_libraries(openshow_protocol PUBLIC Boost::json)
