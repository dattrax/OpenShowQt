import QtQuick
import QtQuick.Controls

Rectangle {
    id: bar
    height: 64
    color: "#505052"
    property string page: "show"
    signal pageSelected(string name)

    Row {
        anchors.left: parent.left
        anchors.leftMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        spacing: 8
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: "OpenShowQt"
            color: "#eeeeee"
            font.pixelSize: 16
            font.bold: true
        }
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: 1; height: 32; color: "#666668"
        }
        Repeater {
            model: [{name: "show", label: "Show", glyph: "\u25b6"},
                    {name: "edit", label: "Edit", glyph: "\u270e"},
                    {name: "settings", label: "Settings", glyph: "\u2699"}]
            delegate: AbstractButton {
                id: tab
                required property var modelData
                width: 66; height: 52
                padding: 0
                focusPolicy: Qt.NoFocus
                Accessible.name: modelData.label
                onClicked: bar.pageSelected(modelData.name)
                background: Rectangle {
                    radius: 4
                    color: bar.page === tab.modelData.name ? "#626265"
                         : tab.hovered ? "#59595b" : "transparent"
                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width; height: 2
                        color: "#1685f8"
                        visible: bar.page === tab.modelData.name
                    }
                }
                contentItem: Item {
                    Column {
                        anchors.centerIn: parent
                        width: parent.width
                        spacing: 2
                        Text {
                            width: parent.width
                            horizontalAlignment: Text.AlignHCenter
                            text: tab.modelData.glyph
                            color: "#dedede"; font.pixelSize: 23
                        }
                        Text {
                            width: parent.width
                            horizontalAlignment: Text.AlignHCenter
                            text: tab.modelData.label
                            color: "#dedede"; font.pixelSize: 12
                        }
                    }
                }
            }
        }
    }

    Row {
        anchors.right: parent.right
        anchors.rightMargin: 16
        anchors.verticalCenter: parent.verticalCenter
        spacing: 10
        Text {
            anchors.verticalCenter: parent.verticalCenter
            color: "#d8c07a"; font.pixelSize: 11
            visible: docs.dirty
            text: "Unsaved changes"
        }
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: 10; height: 10; radius: 5
            color: worker.outputLive ? "#63bd4c" : "#d46767"
        }
        Column {
            spacing: 3
            Text { color: "#eeeeee"; font.pixelSize: 12; text: "AUDIENCE" }
            Text {
                width: Math.min(260, bar.width * 0.25)
                elide: Text.ElideRight
                color: worker.statusNeedsAttention || worker.mediaError !== "" || worker.mediaAudio === "degraded"
                       || worker.mediaAudio === "no-output" ? "#e0a030" : "#cccccc"
                font.pixelSize: 11
                text: worker.mediaError !== "" || worker.standaloneError !== ""
                      || worker.foregroundErrors.length > 0 || worker.mediaAudio === "degraded"
                      || worker.mediaAudio === "no-output" ? "Media needs attention"
                    : worker.statusNeedsAttention ? "Action needs attention"
                    : !worker.outputLive ? "Output unavailable"
                    : worker.previewActive ? "Ready" : "Starting output"
                ToolTip.visible: statusHover.containsMouse
                ToolTip.text: "Connection and media details are available in Settings"
                MouseArea { id: statusHover; anchors.fill: parent; hoverEnabled: true; acceptedButtons: Qt.NoButton }
            }
        }
    }
}
