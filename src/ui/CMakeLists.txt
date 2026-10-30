qt_policy(SET QTP0001 NEW)

# The document session (task 3.3) is Qt Core only, so the headless test
# drives the very class the QML panel binds to — no display server needed.
# The worker supervisor (task 6.3a) joins it so its reply-correlation
# fixtures drive the real WorkerController: QProcess/QLocalSocket need Qt
# Network and QImage needs Qt Gui, but neither needs a display, so the
# supervisor's tests stay display-free as well. RpcClient (task 6.19a1) is
# the transport half of that conversation and lives here with it: the
# fixtures assert through the controller either way, and a component the UI
# cannot run without is not worth a target of its own. PreviewConsumer
# (task 6.19a2) is the picture half of the same conversation — the shm
# mapping and the three targets drawn into it — so it shares the target for
# the same reason: its fixtures read the same controller, and the boundary
# that matters is the one between its files, not between build outputs.
# PresentationModel (task 6.19a3) is the acknowledged-output half of the same
# conversation — the revision mirror, the pending logical presentation and the
# restore point behind the scene properties — and shares the target for the
# same reason: worker_controller_tests asserts the go/clear/restore promises
# through the controller either way. DocumentPersistence (task 6.19b1) is the
# durability half of the document session and joins for the same reason again:
# document_controller_tests pins the scratch, the decision protocol and the
# survivor wording through DocumentController, which is what the QML binds.
# DocumentViews (task 6.19b2) is the row-shape half of the same session and
# joins for the same reason once more: the fixtures pin the rows through the
# controller's properties, and the boundary that keeps the views honest is
# the one in its header, not a second build target.
add_library(openshow-ui-core STATIC
  AppSettings.cpp
  AppSettings.h
  AutosaveScheduler.cpp
  AutosaveScheduler.h
  DocumentController.cpp
  DocumentController.h
  DocumentPersistence.cpp
  DocumentPersistence.h
  DocumentViews.cpp
  DocumentViews.h
  LibrarySearch.cpp
  LibrarySearch.h
  PresentationModel.cpp
  PresentationModel.h
  PreviewConsumer.cpp
  PreviewConsumer.h
  RpcClient.cpp
  RpcClient.h
  SceneAsk.h
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
            ShortcutMap.qml OutputPanel.qml PanelField.qml PanelButton.qml
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
