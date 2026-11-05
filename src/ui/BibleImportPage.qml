import QtQuick
import QtQuick.Controls

// One lookup replaces the passage. The generated worker thumbnails are the
// only preview; Add places the complete passage in the schedule, never on air.
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

    // The thumbnail ask for THIS plan (CS.3): the collection reports its
    // visible band, the page compiles that band into one bounded batch of
    // generation-keyed scenes, and the worker's existing lane (32-scene
    // cap, dedupe, replace-not-merge) does the rest. Keys are
    // `scripture:<plan>:<index>`, so a re-pagination cannot leave a new
    // card wearing the old plan's pixels.
    function askPlanThumbnails() {
        if (!biblePage.visible)
            return
        const slides = scripture.slides
        if (slides.length === 0) {
            worker.clearThumbnails()
            return
        }
        // 16:9 inside the 32..512 envelope the wire accepts at the 640x360
        // slot — the same rule the Show band follows, from this grid's own
        // cell width. A size chosen here can never be refused on the wire.
        const w = Math.max(32, Math.min(512, Math.round(bibleCells.cellWidth - 18)))
        const asks = []
        const seen = ({})
        const wanted = bibleCells.visibleIndexes()
        for (const i of wanted) {
            const key = scripture.previewKey(i)
            if (seen[key])
                continue
            const scene = scripture.previewSceneJson(i)
            if (scene === "")
                continue
            seen[key] = true
            asks.push({key: key, scene: scene})
        }
        if (asks.length === 0) {
            worker.clearThumbnails()
            return
        }
        worker.requestThumbnailWindow(asks, w, Math.round(w * 9 / 16))
    }
    Timer {
        id: bibleAsk
        interval: 80
        onTriggered: biblePage.askPlanThumbnails()
    }
    Connections {
        target: scripture
        function onSlidesChanged() { bibleAsk.restart() }
    }

    // This page owns thumbnails only. It has no Selection canvas to render.
    onVisibleChanged: {
        if (visible) {
            bibleAsk.restart()
        } else {
            bibleAsk.stop()
            worker.clearThumbnails()
        }
    }
    Component.onCompleted: scripture.previewActive = false

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

        Flow {
            objectName: "bibleLookupForm"
            width: parent.width
            spacing: 8
            TextField {
                id: referenceField
                objectName: "bibleReferenceField"
                width: Math.min(biblePage.availableWidth,
                                Math.max(220, biblePage.availableWidth - 470))
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
                            required property int index
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
                width: Math.min(200, biblePage.availableWidth)
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
                enabled: referenceField.text.trim().length > 0
                         && (!bible.busy || referenceField.text.trim() !== bible.reference.trim())
                onClicked: {
                    bible.reference = referenceField.text
                    bookSuggest.visible = false
                    bible.lookup()
                }
            }
        }

        // --- the generated plan: worker pictures, not text frames (CS.3) ---
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
        SlidePreviewCollection {
            id: bibleCells
            objectName: "bibleSlideCollection"
            width: biblePage.availableWidth
            height: Math.min(460, Math.ceil(Math.max(1, scripture.slides.length)
                                            / Math.max(1, columns)) * cellHeight)
            visible: scripture.slides.length > 0
            cellWidth: 210
            grouped: false
            source: scripture.slides
            onVisibleBandChanged: biblePage.askPlanThumbnails()
            delegate: SlidePreviewCard {
                required property var modelData
                required property int index
                objectName: "bibleSlideCard"
                width: bibleCells.cellWidth - 12
                height: bibleCells.cellHeight - 12
                caption: (index + 1) + "  " + modelData.row.label
                Accessible.role: Accessible.StaticText
                Accessible.name: "Generated slide " + (index + 1) + ": " + modelData.row.label
                surface.controller: worker
                surface.target: "thumbnail"
                surface.key: scripture.previewKey(index)
                // Until the worker's own picture of THIS slide arrives, the
                // card says so with the slide's words — plainly styled, a
                // text line, never advertised as the final render.
                surface.placeholder: modelData.row.lines.join("\n")
                surface.placeholderColor: "#c8c8c8"
                surface.placeholderPixelSize: 11
            }
        }

        // The one door out — and it says where it goes. No Library, no
        // present, no other destination exists to be added by accident.
        Row {
            spacing: 8
            Button {
                objectName: "bibleAddButton"
                text: "Add to today's schedule"
                enabled: scripture.canAdd && referenceField.text.trim() === bible.reference.trim()
                onClicked: scripture.addToSchedule()
            }
            Button {
                objectName: "bibleCancelButton"
                text: "Cancel"
                onClicked: scripture.cancel()
            }
        }

    }
}
