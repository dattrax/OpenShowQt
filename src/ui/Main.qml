import QtQuick
import QtQuick.Controls

Window {
    id: root
    width: 960
    height: 760
    visible: true
    color: "#141414"
    title: "OpenShowQt " + applicationVersion

    // Task 1.2: closing the UI shuts the worker down before the window
    // actually goes away.
    onClosing: function(close) {
        worker.shutdownNow()
    }

    Column {
        anchors.centerIn: parent
        spacing: 18

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            color: "#e8e8e8"
            font.pixelSize: 22
            text: "OpenShowQt " + applicationVersion
        }

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            color: worker.outputLive ? "#7dd47d" : "#d47d7d"
            font.pixelSize: 15
            text: worker.status
        }

        Text {
            visible: worker.outputLive
            anchors.horizontalCenter: parent.horizontalCenter
            color: "#9a9a9a"
            font.pixelSize: 13
            // The panel name is looked up in displays, guarded: status can
            // arrive before the display list (or after a hotplug empties it).
            text: {
                const panel = worker.currentMonitor >= 0
                    ? worker.displays[worker.currentMonitor] : null
                const where = worker.currentMonitor < 0 ? "windowed"
                    : (panel ? panel.name : "monitor " + worker.currentMonitor)
                return worker.device + " · frame " + worker.frames
                       + " · revision " + worker.revision + " · " + where
            }
        }

        Row {
            visible: worker.outputLive
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 8

            Text {
                anchors.verticalCenter: parent.verticalCenter
                color: "#9a9a9a"
                font.pixelSize: 13
                text: "Destination:"
            }

            ComboBox {
                id: destination
                model: [{value: -1, label: "Windowed"}].concat(
                    worker.displays.map(d => ({value: d.index,
                                               label: d.name + " — " + d.size})))
                textRole: "label"
                valueRole: "value"
                // Bound to the worker's applied state, so the picker shows
                // where the output actually is until a switch confirms.
                currentIndex: destination.indexOfValue(worker.currentMonitor)
                onActivated: worker.selectDisplay(currentValue)
            }
        }

        Row {
            visible: worker.outputLive
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 10

            Repeater {
                model: ["background", "images", "words", "aux", "clock"]

                delegate: CheckBox {
                    required property string modelData
                    text: modelData
                    checked: worker.layers[modelData] === true
                    onToggled: worker.setLayerEnabled(modelData, checked)
                }
            }
        }

        // Task 2.4c: compiled scenes through the same path real content
        // will take — presentDemoSlide compiles the fixture's slide with
        // compileSlide, prepares it, and presents the revision the
        // worker acknowledges. The label shows the worker's store.
        Row {
            visible: worker.outputLive && worker.demoSlideCount > 0
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 10

            Repeater {
                model: worker.demoSlideCount

                Button {
                    required property int index
                    text: "scene " + (index + 1)
                    onClicked: worker.presentDemoSlide(index)
                }
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                color: "#9a9a9a"
                font.pixelSize: 12
                text: "scene " + worker.sceneActive
                      + (worker.scenePending !== worker.sceneActive
                         ? " \u2192 " + worker.scenePending : "")
                      + " \u00b7 " + worker.scenePrepared + " prepared"
            }
        }

        // Task 1.4: the audience output as the worker sees it, streamed
        // over shared memory. Qt only paints bytes here; the scene is the
        // worker's Skia render, downscaled to preview size.
        Column {
            visible: worker.outputLive
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 4

            Rectangle {
                width: 480
                height: 270
                color: "black"
                border.color: "#3a3a3a"

                PreviewView {
                    anchors.fill: parent
                    anchors.margins: 1
                    controller: worker
                }
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                color: "#9a9a9a"
                font.pixelSize: 12
                text: worker.previewActive
                    ? "preview · " + worker.previewFrames + " frames shown · "
                      + worker.previewDropped + " dropped"
                    : (worker.previewError !== ""
                       ? "preview: " + worker.previewError : "preview starting...")
            }
        }
    }
}
