import QtQuick
import QtQuick.Controls

// The Bible import page (task BI.5). One passage at a time, by request:
// the notice and any failure read FIRST, before the operator can type
// anything; the fetched verses are chosen here; the slides they generate
// are measured by the audience's own renderer and shown OFF AIR — the
// picture arrives in the preview panel on the right, and nothing on this
// page can put anything in front of the congregation. There is no Save to
// Library and no Present from here: Add joins the passage to today's
// schedule as one undoable item, and that is the only door out.
ScrollView {
    id: biblePage
    objectName: "bibleImportPage"
    contentWidth: availableWidth
    clip: true
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

    // A theme change while this page is open re-paginates from the words
    // the session already holds — no second network request (the plan's
    // rule, executed at the page level).
    Connections {
        target: docs
        function onDocumentsChanged() {
            if (biblePage.visible)
                scripture.regenerate()
        }
    }

    Column {
        width: biblePage.availableWidth
        spacing: 10

        Text { text: "Bible Import"; color: "#eeeeee"; font.pixelSize: 18; font.bold: true }

        // The notice comes first, and every failure keeps this line: the
        // plan's "the online notice and any failure are visible" is one
        // honest label, and it is never hidden behind the form.
        Text {
            objectName: "bibleNoticeLabel"
            width: parent.width
            wrapMode: Text.WordWrap
            color: bible.busy ? "#b0b0b0"
                 : bible.status.length > 0 ? "#d8c07a" : "#8ec9f5"
            font.pixelSize: 12
            text: bible.status
        }
        Text {
            objectName: "bibleImportStatusLabel"
            width: parent.width
            wrapMode: Text.WordWrap
            visible: scripture.status.length > 0
            color: "#b0b0b0"
            font.pixelSize: 12
            text: scripture.status
        }

        Row {
            spacing: 8
            TextField {
                objectName: "bibleReferenceField"
                width: Math.max(220, biblePage.availableWidth - 470)
                placeholderText: "Reference, e.g. 1 Corinthians 1:1-6"
                text: bible.reference
                onEditingFinished: bible.reference = text
                color: "#eeeeee"
                font.pixelSize: 13
            }
            ComboBox {
                objectName: "bibleTranslationPicker"
                width: 200
                model: bible.translations
                textRole: "version"; valueRole: "version"
                currentIndex: indexOfValue(bible.translation)
                displayText: bible.translation + (currentIndex >= 0 && model[currentIndex]
                                                  ? " — " + model[currentIndex].name : "")
                onActivated: bible.translation = currentValue
                ToolTip.text: model.length > 0 && currentIndex >= 0
                              ? model[currentIndex].attributionNote : ""
                ToolTip.visible: hovered
            }
            Button {
                objectName: "bibleLookupButton"
                text: bible.busy ? "Looking up…" : "Lookup"
                enabled: !bible.busy && bible.reference.trim().length > 0
                onClicked: bible.lookup()
            }
            Button {
                objectName: "bibleRetryButton"
                text: "Retry"
                visible: !bible.busy && bible.verses.length === 0
                         && bible.reference.trim().length > 0
                onClicked: bible.retry()
            }
        }

        Row {
            spacing: 8
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "VERSES"; color: "#bbbbbb"; font.pixelSize: 11; font.bold: true
            }
            Button {
                objectName: "bibleSelectAllButton"
                flat: true
                text: "All"
                enabled: bible.verses.length > 0
                onClicked: bible.selectAllVerses(true)
            }
            Button {
                objectName: "bibleSelectNoneButton"
                flat: true
                text: "None"
                enabled: bible.verses.length > 0
                onClicked: bible.selectAllVerses(false)
            }
        }
        Repeater {
            model: bible.verses
            delegate: CheckBox {
                required property var modelData
                required property int index
                objectName: "bibleVerseCheck"
                width: biblePage.availableWidth
                text: modelData.reference + "  " + modelData.text
                font.pixelSize: 12
                checked: modelData.selected === true
                contentItem: Label {
                    text: parent.text
                    font: parent.font
                    color: parent.checked ? "#8ec9f5" : "#cccccc"
                    wrapMode: Text.WordWrap
                    verticalAlignment: Text.AlignVCenter
                }
                onToggled: bible.setVerseSelected(index, checked)
            }
        }
        Text {
            objectName: "bibleNothingSelectedHint"
            // A failed pagination leaves its own sentence showing; the
            // hint must not overwrite it with a wrong explanation (BIF.1).
            visible: bible.verses.length > 0 && scripture.slides.length === 0
                     && !scripture.measuring && scripture.status.length === 0
            width: parent.width; wrapMode: Text.WordWrap
            color: "#909090"; font.pixelSize: 11
            text: "No verses are selected — keep at least one and the slides will generate."
        }

        Row {
            spacing: 8
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "GENERATED SLIDES"
                color: "#bbbbbb"; font.pixelSize: 11; font.bold: true
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: scripture.measuring
                text: "measuring with the audience's renderer…"
                color: "#8ec9f5"; font.pixelSize: 11
            }
        }
        Repeater {
            model: scripture.slides
            delegate: Frame {
                required property var modelData
                required property int index
                objectName: "bibleSlideRow"
                width: biblePage.availableWidth
                padding: 8
                // Clicking a row asks the worker to draw THAT slide into
                // the preview stream — off air. Selecting never presents.
                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton
                    onClicked: scripture.previewSlide(index)
                }
                Column {
                    width: parent.width
                    spacing: 4
                    Row {
                        spacing: 6
                        Text {
                            text: (index + 1) + "."
                            color: "#8ec9f5"; font.pixelSize: 11; font.bold: true
                        }
                        Text {
                            text: modelData.label
                            color: "#bbbbbb"; font.pixelSize: 11
                        }
                    }
                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        color: "#dddddd"; font.pixelSize: 12
                        text: modelData.lines.join("\n")
                    }
                }
            }
        }

        // The one door out — and it says where it goes. No Library, no
        // present, no other destination exists to be added by accident.
        Row {
            spacing: 8
            Button {
                objectName: "bibleAddButton"
                text: "Add to today's schedule"
                enabled: scripture.canAdd
                onClicked: scripture.addToSchedule()
            }
            Button {
                objectName: "bibleCancelButton"
                text: "Cancel"
                onClicked: scripture.cancel()
            }
        }
        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            color: "#909090"; font.pixelSize: 11
            text: "Slides are measured with the same text engine the audience sees and "
                  + "previewed off air in the preview panel. The verse label is stored in "
                  + "each slide's notes; the slide's words are only the passage."
        }
    }
}
