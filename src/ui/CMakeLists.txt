qt_policy(SET QTP0001 NEW)

# The document session (task 3.3) is Qt Core only, so the headless test
# drives the very class the QML panel binds to — no display server needed.
# The worker supervisor (task 6.3a) joins it so its reply-correlation
# fixtures drive the real WorkerController: QProcess/QLocalSocket need Qt
# Network and QImage needs Qt Gui, but neither needs a display, so the
# supervisor's tests stay display-free as well.
add_library(openshow-ui-core STATIC
  AppSettings.cpp
  AppSettings.h
  DocumentController.cpp
  DocumentController.h
  LibrarySearch.cpp
  LibrarySearch.h
  SceneEdit.cpp
  SceneEdit.h
  WorkerController.cpp
  WorkerController.h
  WorkspacePanels.cpp
  WorkspacePanels.h
)
add_library(openshow::ui-core ALIAS openshow-ui-core)
set_target_properties(openshow-ui-core PROPERTIES AUTOMOC TRUE)
target_include_directories(openshow-ui-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(openshow-ui-core PUBLIC
  Qt6::Core
  Qt6::Gui
  Qt6::Network
  openshow::domain
  openshow::protocol
  openshow::scene
)

# In-tree development path to the worker; packaging (task 6.3) will place
# them side by side and can drop this.
target_compile_definitions(openshow-ui-core PRIVATE
  OPENSHOW_WORKER_PATH="$<TARGET_FILE:openshow-worker>"
)
add_dependencies(openshow-ui-core openshow-worker)

qt_add_executable(openshow-ui
  main.cpp
)
add_executable(openshow::ui ALIAS openshow-ui)

qt_add_qml_module(openshow-ui
  URI OpenShow
  VERSION 1.0
  QML_FILES Main.qml TopBar.qml SidePanel.qml LibraryDrawer.qml EditCanvas.qml
  SOURCES PreviewView.h PreviewView.cpp
)

target_link_libraries(openshow-ui PRIVATE
  Qt6::Core
  Qt6::Gui
  Qt6::Quick
  Qt6::QuickControls2
  Qt6::Network
  openshow::ui-core
  openshow::domain
  openshow::protocol
  openshow::scene
)
