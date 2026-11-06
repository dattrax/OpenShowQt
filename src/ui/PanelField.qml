import QtQuick
import QtQuick.Controls

/*
 * The workspace's text field (task 4.4, shared from task 6.19b4).
 *
 * Material's default TextField is too tall for a 250px column once the
 * background is replaced — the placeholder and text split vertically — so
 * this carries the workspace palette and metrics. It was an inline
 * component on the window until the panels started living in their own
 * files: a component must style itself, not borrow the window's private
 * declarations.
 */
TextField {
    id: field
    // A compact list filter can opt out of Material's floating placeholder.
    property string fixedPlaceholderText: ""
    Text {
        objectName: "fixedFieldPlaceholder"
        anchors.fill: parent
        anchors.leftMargin: field.leftPadding
        anchors.rightMargin: field.rightPadding
        verticalAlignment: Text.AlignVCenter
        text: field.fixedPlaceholderText
        font: field.font
        color: field.placeholderTextColor
        visible: field.fixedPlaceholderText !== "" && field.text.length === 0
    }
    color: "#eeeeee"
    font.pixelSize: 13
    height: 30
    verticalAlignment: Text.AlignVCenter
    leftPadding: 6
    rightPadding: 6
    background: Rectangle {
        color: "#242424"
        border.color: "#4a4a4a"
    }
}
