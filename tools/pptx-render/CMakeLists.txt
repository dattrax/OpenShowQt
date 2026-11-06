# PowerPoint deck renderer (PX.1): a small Rust sidecar built against the
# pinned BetterOffice submodule. Cargo owns its own dependency graph and
# incremental rebuilds; CMake only names the product the UI and tests use.
#
# The binary is a process, not a library: a malformed deck cannot crash the
# operator UI, and the C++ build never links a Rust ABI. CARGO_TARGET_DIR is
# kept inside the build tree so a clean checkout never writes into tools/.

set(PPTX_RENDER_TARGET_DIR "${CMAKE_BINARY_DIR}/cargo")
set(PPTX_RENDER_BIN
  "${PPTX_RENDER_TARGET_DIR}/release/pptx-render${CMAKE_EXECUTABLE_SUFFIX}")
set(OPENSHOW_PPTX_RENDER_BIN "${PPTX_RENDER_BIN}" CACHE INTERNAL
  "The built pptx-render executable")

add_custom_target(pptx_render ALL
  COMMAND "${CMAKE_COMMAND}" -E env
          "CARGO_TARGET_DIR=${PPTX_RENDER_TARGET_DIR}"
          "${CARGO_EXECUTABLE}" build --release --locked
          --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  COMMENT "Building pptx-render (BetterOffice)"
  VERBATIM
)

# The UI locates this at runtime from this path; packaging adds an install rule.

