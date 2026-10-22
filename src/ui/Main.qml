import QtQuick
import QtQuick.Controls

Window {
    id: root
    width: 960
    height: 600
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
            text: worker.device + " · frame " + worker.frames
                  + " · revision " + worker.revision
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
    }
}
