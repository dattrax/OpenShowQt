import QtQuick
import QtQuick.Controls

Button {
    id: control
    implicitWidth: Math.max(32, implicitContentWidth + 20)
    implicitHeight: 30
    topInset: 0
    bottomInset: 0
    leftInset: 0
    rightInset: 0
    padding: 6
    leftPadding: 10
    rightPadding: 10
    font.pixelSize: 12
    focusPolicy: Qt.NoFocus
    contentItem: Text {
        text: control.text
        font: control.font
        color: control.enabled ? "#eeeeee" : "#888888"
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    background: Rectangle {
        radius: 3
        color: control.down ? "#1764a6" : control.highlighted ? "#1678cf"
             : control.hovered ? "#575757" : "#454545"
        border.color: control.highlighted ? "#1685f8" : "#606060"
        opacity: control.enabled ? 1 : 0.5
    }
}
