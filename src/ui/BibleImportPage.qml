import QtQuick
import QtQuick.Controls

// The Bible import page (task BI.5, restructured by task CS.3). One passage
// at a time, by request: the notice and any failure read FIRST, before the
// operator can type anything; the fetched verses are chosen in a compact
// scrolling list; the slides they generate are shown as WORKER PICTURES —
// each card paints the worker's own keyed off-air render of that generated
// slide, and the labelled OFF-AIR CANVAS below paints the selected one
// whole. Nothing on this page can put anything in front of the
// congregation: the Audience monitor on the right keeps showing the live
// output, and the old sentence promising pictures "in the preview panel"
// was never true of it. There is no Save to Library and no Present from
// here: Add joins the passage to today's schedule as one undoable item,
// and that is the only door out.
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
            if (asks.length >= 32)
                break
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
        worker.requestThumbnails(asks, w, Math.round(w * 9 / 16))
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

    // The visible page owns both off-air lanes (CS.3): entering re-asks
    // the canvas's scene and the cards' batch, leaving stands them both
    // down — Show, Themes and Bible never leave a hidden page's batch
    // racing the one on screen.
    onVisibleChanged: {
        if (visible) {
            if (scripture.slides.length > 0)
                scripture.previewSlide(scripture.selectedIndex >= 0
                                       ? scripture.selectedIndex : 0)
            bibleAsk.restart()
        } else {
            bibleAsk.stop()
            worker.clearSelectionPreview()
            worker.clearThumbnails()
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

        // --- the verse source (compact and scrollable since CS.3, so a
        // long passage cannot push Add and the previews off the page) ---
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
            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: bible.verses.length > 0
                text: "tick the verses to keep"
                color: "#808080"; font.pixelSize: 11
            }
        }
        ListView {
            id: bibleVerseList
            objectName: "bibleVerseList"
            width: biblePage.availableWidth
            height: Math.min(220, Math.max(0, bible.verses.length) * 46)
            visible: bible.verses.length > 0
            clip: true
            ScrollBar.vertical: ScrollBar {}
            model: bible.verses
            // Checkboxes are SOURCE selection controls: they choose which
            // verses enter the plan. Their readable verse text stays here;
            // what the slide will LOOK like is the worker's cards below,
            // never this row.
            delegate: CheckBox {
                required property var modelData
                required property int index
                objectName: "bibleVerseCheck"
                width: bibleVerseList.width
                height: Math.max(verseRow.implicitHeight + 8, 30)
                font.pixelSize: 12
                checked: modelData.selected === true
                contentItem: Label {
                    id: verseRow
                    text: modelData.reference + "  " + modelData.text
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
                // Selection is a browse state, and this card is the ONLY
                // kind of marker it deserves: blue chrome on the operator's
                // own pick, no live chip, no present. The card never calls
                // a presentation verb; previewSlide draws off air.
                selected: index === scripture.selectedIndex
                caption: (index + 1) + "  " + modelData.row.label
                Accessible.role: Accessible.Button
                Accessible.name: "Generated slide " + (index + 1) + ": " + modelData.row.label
                                 + (index === scripture.selectedIndex ? ", selected" : "")
                                 + " — off air"
                surface.controller: worker
                surface.target: "thumbnail"
                surface.key: scripture.previewKey(index)
                // Until the worker's own picture of THIS slide arrives, the
                // card says so with the slide's words — plainly styled, a
                // text line, never advertised as the final render.
                surface.placeholder: modelData.row.lines.join("\n")
                surface.placeholderColor: "#c8c8c8"
                surface.placeholderPixelSize: 11
                onActivated: scripture.previewSlide(index)
            }
        }

        // --- the selected off-air canvas (CS.3): the picture this page
        // always claimed to show and never did ---
        Text {
            text: "OFF-AIR CANVAS — the selected generated slide, off air"
            color: "#bbbbbb"; font.pixelSize: 11; font.bold: true
            visible: scripture.slides.length > 0
        }
        SlidePreviewSurface {
            id: bibleCanvas
            objectName: "biblePreviewCanvas"
            width: Math.min(biblePage.availableWidth, 720)
            height: width * 9 / 16
            visible: scripture.slides.length > 0
            controller: worker
            target: "selection"
            // CS.3 masking: the canvas paints only the frame that ANSWERS
            // the newest ask. Between plans — a re-pagination, a new lookup,
            // a cancelled plan — the old picture goes dark with an honest
            // sentence instead of going on captioning a plan that died.
            pixelsVisible: worker.selectionSupported && worker.selectionCurrent
            placeholder: !worker.selectionSupported
                         ? "this worker cannot draw off-air pictures of slides"
                         : (scripture.selectedIndex < 0
                            ? "pick one of the generated slides above and the "
                              + "worker's off-air render appears here"
                            : "waiting for the worker's render of the selected slide…")
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
            text: "Slides are measured with the same text engine the audience sees. "
                  + "The cards and the off-air canvas above show the worker's own render "
                  + "of the generated slides — nothing here goes to the audience, and the "
                  + "monitor on the right keeps showing the live output. The verse label "
                  + "is stored in each slide's notes; the slide's words are only the passage."
        }
    }
}
