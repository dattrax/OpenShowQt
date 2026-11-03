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
                id: referenceField
                objectName: "bibleReferenceField"
                width: Math.max(220, biblePage.availableWidth - 470)
                placeholderText: "Reference, e.g. 1 Corinthians 1:1-6"
                text: bible.reference
                onEditingFinished: bible.reference = text
                color: "#eeeeee"
                font.pixelSize: 13

                // BIF.3: the built-in canon answers as the operator names a
                // book — the dropdown filters with every keystroke and a
                // click or Enter finishes the book name while keeping the
                // chapter/verse tail. It is a helper only: unmatched text
                // shows no rows, and Lookup sends exactly what is typed no
                // matter what the table thinks.
                function refreshSuggestions() {
                    const rows = bible.suggestBooks(text)
                    // Once the field already reads like the top row, the
                    // help has been taken — put it away.
                    bookSuggest.rows =
                        (rows.length > 0 && rows[0].completion.trim() !== text.trim()) ? rows : []
                    bookSuggest.visible = bookSuggest.rows.length > 0
                }
                function completeTop() {
                    if (bookSuggest.rows.length > 0) {
                        text = bookSuggest.rows[0].completion
                        bible.reference = text
                    }
                    bookSuggest.visible = false
                }
                onTextEdited: refreshSuggestions()
                onAccepted: {
                    if (bookSuggest.visible)
                        completeTop()
                    else
                        bible.reference = text
                }
                Keys.onEscapePressed: bookSuggest.visible = false

                Popup {
                    id: bookSuggest
                    objectName: "bibleBookPopup"
                    property var rows: []
                    y: referenceField.height
                    width: referenceField.width
                    padding: 2
                    modal: false
                    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
                    // No Material fade (measured): Popup only flips
                    // visible=false AFTER its exit transition finishes, so
                    // a fading dropdown lags the keystrokes it follows and
                    // reports stale state while unrendered. A completer
                    // retires the instant the completion is taken.
                    enter: null
                    exit: null
                    // The ListView IS the content item: a Popup sizes
                    // itself to its contentItem's implicit size, and a
                    // free-floating child never feeds that — leaving the
                    // popup 4 px tall and its rows outside its own click
                    // area (found by measuring the live popup, not by
                    // guessing). The implicit height caps at six-ish rows.
                    contentItem: ListView {
                        id: bookSuggestList
                        objectName: "bibleBookSuggestionList"
                        implicitHeight: Math.min(contentHeight, 216)
                        clip: true
                        model: bookSuggest.rows
                        delegate: ItemDelegate {
                            objectName: "bibleBookSuggestionRow"
                            width: bookSuggestList.width
                            required property var modelData
                            text: modelData.name
                            font.pixelSize: 13
                            onClicked: {
                                referenceField.text = modelData.completion
                                bible.reference = modelData.completion
                                bookSuggest.visible = false
                                referenceField.forceActiveFocus()
                            }
                        }
                    }
                }
            }
            ComboBox {
                objectName: "bibleTranslationPicker"
                id: translationPicker
                width: 200
                model: bible.translations
                textRole: "version"; valueRole: "version"
                // BIF.2: `currentIndex: indexOfValue(...)` is a plain
                // function call — the binding engine cannot see what it
                // reads (the control's own model is not a dependency it
                // can capture), so it never resolved and the picker opened
                // unselected. This expression reads the session's property
                // directly — engine-tracked — so the row matching
                // bible.translation is selected from the first paint, and
                // a session-side change to the translation re-runs it.
                // (Popup activation writes currentIndex from C++, which
                // keeps a declarative binding alive; onActivated feeds the
                // session, and this binding follows it back.)
                currentIndex: {
                    const wanted = bible.translation
                    const rows = bible.translations
                    for (let i = 0; i < rows.length; ++i) {
                        if (rows[i].version === wanted)
                            return i
                    }
                    return -1
                }
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
