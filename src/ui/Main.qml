import QtQuick

Window {
    id: root
    width: 960
    height: 600
    visible: true
    color: "#141414"
    title: "OpenShowQt " + applicationVersion

    Text {
        anchors.centerIn: parent
        horizontalAlignment: Text.AlignHCenter
        color: "#e8e8e8"
        font.pixelSize: 18
        text: "OpenShowQt " + applicationVersion + "\n"
              + "no worker connected\n"
              + "worker would offer " + workerOffer
    }
}
