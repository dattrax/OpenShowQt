import QtQuick
import QtQuick.Controls

/*
 * UI part: WorkerFontPicker; contract: docs/ui/control-sets.md#workerfontpicker
 *
 * The font row every inspector needs (task CS.5): a TYPED family field —
 * the path that always worked and works with the worker gone — beside a
 * Browse button that opens the audience worker's OWN family list. Never
 * Qt's installed fonts: a picker of the operator's desktop would offer
 * names the show silently falls back from. The missing-family warning
 * needs a COMPLETED worker list, and a requested name is never rewritten
 * by a fallback verdict — the scene keeps what the author asked for.
 *
 * Extracted verbatim in behaviour from ThemeEditorPage's row (task TS.4):
 * the status line refuses to guess while no list has landed, Browse sleeps
 * for a worker that cannot enumerate, and choosing a family is exactly as
 * if the operator had typed it. Hosts pass their pinned automation names;
 * the popup lives at the page level through `overlayParent` because a
 * centred modal cannot hang off a row inside a scroll rail.
 */
Column {
    id: picker

    required property var worker           // WorkerController* — the family source
    required property InspectorAdapter adapter
    required property string field         // "fontFamily"
    // Sparse targets (context, slide) answer "" = silence: an empty typed
    // field INHERITS, and the display shows the own value or empty, never
    // the resolved one the fallback would paint.
    property bool sparse: false
    // Both are the host's prose: the Songs row says ("\"\" = platform"),
    // the Bible row ("\"\" = theme's").
    property string label: "Font family (\"\" = platform)"
    property bool showStatus: true
    property bool showBrowse: true         // the reading row types; no Browse
    property bool allowOpen: true          // ANDed with worker capability
    // The page the centred popup belongs to (a row inside a scroll rail is
    // the wrong coordinate system for a modal).
    property Item overlayParent: null
    // Kept current by sync(); the enabled bindings below watch it, so a
    // host that moves `allowOpen` re-evaluates without another sync pass.
    property bool editable: false

    property string labelObjectName: ""
    property string familyObjectName: "fontFamilyField"
    property string browseObjectName: "fontBrowse"
    property string statusObjectName: "fontStatus"
    property string popupObjectName: "fontPopup"
    property string searchObjectName: "fontSearch"
    property string listStatusObjectName: "fontListStatus"
    property string listObjectName: "fontList"
    property string rowObjectName: "fontRow"
    property string cancelObjectName: "fontCancel"

    signal familyChosen(string name)

    spacing: 4
    width: parent ? parent.width : 0

    function state() { return adapter.fieldState(picker.field) }
    // What the field shows and what the status line says: assignment, not
    // binding — the adapter's answers are function calls the binding
    // engine cannot track, and a control the operator is mid-typing on
    // must not be yanked by an unrelated change. A refused commit re-reads
    // through the same door the model's own emit uses.
    function sync() {
        const s = state()
        picker.editable = s.editable === true
        // A missing target answers value-less: the fields go blank exactly
        // as the page blanked them when no row was selected.
        if (picker.sparse)
            familyField.text = s.isSet === true ? String(s.ownValue) : ""
        else
            familyField.text = s.value === undefined ? "" : String(s.value)
        statusLine.text = picker.statusText(s)
        statusLine.color = picker.familyMissing(s) ? "#e0a94a" : "#888888"
    }
    function commitTyped() {
        const t = familyField.text.trim()
        if (picker.sparse) {
            // "" is the sparse target's silence — the field goes back to
            // inheriting rather than storing an empty name.
            if (t === "") {
                if (!adapter.reset(picker.field))
                    picker.sync()
            } else if (!adapter.commit(picker.field, t)) {
                picker.sync()
            }
        } else if (!adapter.commit(picker.field, familyField.text)) {
            picker.sync()
        }
        picker.familyChosen(t)
    }
    // The family list narrowed by the search box, re-derived from BOTH the
    // query and the worker's arriving pages (the TS.4 rule).
    function fontMatches(query) {
        const needle = query.trim().toLowerCase()
        return worker.fontFamilies.filter(f => needle === ""
                                               || f.toLowerCase().indexOf(needle) >= 0)
    }
    // The name the status line judges: the model's own for a sparse
    // target, the resolved one otherwise (never rewritten by a verdict).
    function wanted(s) {
        const st = s !== undefined ? s : state()
        return picker.sparse
                ? (st.isSet === true ? String(st.ownValue) : "")
                : String(st.value === undefined ? "" : st.value)
    }
    // "Can THIS worker draw the family the model holds?" Only a COMPLETED
    // list answers, and only the worker's list counts; no list, an
    // in-flight walk, or a worker that cannot list all say NOTHING.
    function familyMissing(s) {
        const want = picker.wanted(s)
        if (want === "" || !worker.fontListSupported || worker.fontListState !== "ready")
            return false
        const lower = want.toLowerCase()
        return !worker.fontFamilies.some(f => f.toLowerCase() === lower)
    }
    // The note refuses to guess: no list, no verdict.
    function statusText(s) {
        const want = picker.wanted(s)
        if (want === "")
            return ""
        if (!worker.fontListSupported)
            return "typed name — this worker cannot list its fonts"
        if (worker.fontListState !== "ready")
            return "checking the audience's fonts…"
        return picker.familyMissing(s)
                ? "\"" + want + "\" is not installed on the audience's machine "
                  + "— it will fall back"
                : "installed on the audience's machine"
    }

    Connections {
        target: picker.adapter
        function onChanged() { picker.sync() }
        function onTargetChanged() { picker.sync() }
    }
    Component.onCompleted: picker.sync()

    Text {
        objectName: picker.labelObjectName
        text: picker.label
        color: "#b0b0b0"
        font.pixelSize: 11
    }
    Row {
        spacing: 4
        PanelField {
            id: familyField
            objectName: picker.familyObjectName
            width: picker.showBrowse
                    ? picker.width - browseFonts.width - 4
                    : picker.width
            onEditingFinished: picker.commitTyped()
        }
        PanelButton {
            id: browseFonts
            objectName: picker.browseObjectName
            visible: picker.showBrowse
            text: "Browse"
            enabled: worker.fontListSupported && picker.allowOpen && picker.editable
            onClicked: {
                fontSearchField.text = ""
                if (worker.fontListState === "idle")
                    worker.refreshFontFamilies()
                fontPopup.open()
            }
        }
    }
    Text {
        id: statusLine
        objectName: picker.statusObjectName
        visible: picker.showStatus
        width: picker.width
        wrapMode: Text.WordWrap
        font.pixelSize: 10
        color: "#888888"
        text: ""
    }

    // The family picker (task TS.4's popup, moved verbatim). It lists ONLY
    // the audience worker's own families; the sample renders the hovered
    // family in its own face; choosing writes through the same row verb.
    Popup {
        id: fontPopup
        objectName: picker.popupObjectName
        parent: picker.overlayParent !== null ? picker.overlayParent : picker
        x: Math.max(0, ((parent !== null ? parent.width : 0) - width) / 2)
        y: 60
        width: 360
        height: 440
        modal: true
        focus: true
        padding: 12
        background: Rectangle { color: "#2b2b2b"; border.color: "#555555"; radius: 4 }
        // The host page standing down (mode exit, another page) takes its
        // popup with it — an orphaned modal overlay would keep the keyboard
        // the shell is trying to hand back (the TS.4 rule).
        Connections {
            target: picker.overlayParent
            enabled: picker.overlayParent !== null
            function onVisibleChanged() {
                if (picker.overlayParent.visible !== true)
                    fontPopup.close()
            }
        }
        Column {
            width: parent.width
            spacing: 8
            Text { text: "Fonts the audience's worker can draw"; color: "#dddddd"
                   font.pixelSize: 12; font.bold: true }
            PanelField {
                id: fontSearchField
                objectName: picker.searchObjectName
                width: parent.width
                placeholderText: "search families…"
            }
            Row {
                spacing: 8
                Rectangle {
                    width: 90; height: 40; color: "#ffffff"; radius: 3
                    Text {
                        anchors.centerIn: parent
                        font.family: fontPreviewList.currentFamily
                        font.pixelSize: 20
                        color: "#000000"
                        text: "Ag"
                    }
                }
                Text {
                    objectName: picker.listStatusObjectName
                    width: parent.width - 98
                    anchors.verticalCenter: parent.verticalCenter
                    wrapMode: Text.WordWrap
                    color: "#888888"
                    font.pixelSize: 10
                    text: worker.fontListState === "loading" ? "asking the audience's worker…"
                                                              : ""
                }
            }
            ListView {
                id: fontPreviewList
                objectName: picker.listObjectName
                width: parent.width
                height: 300
                clip: true
                property string currentFamily: ""
                model: picker.fontMatches(fontSearchField.text)
                Connections {
                    target: worker
                    function onFontListChanged() {
                        fontPreviewList.model = picker.fontMatches(fontSearchField.text)
                    }
                }
                delegate: ItemDelegate {
                    width: fontPreviewList.width
                    objectName: picker.rowObjectName
                    required property string modelData
                    text: modelData
                    font.family: modelData
                    onClicked: {
                        familyField.text = modelData
                        picker.commitTyped()
                        fontPopup.close()
                    }
                    hoverEnabled: true
                    onHoveredChanged: if (hovered)
                                          fontPreviewList.currentFamily = modelData
                }
            }
            PanelButton {
                objectName: picker.cancelObjectName
                text: "Close"
                onClicked: fontPopup.close()
            }
        }
    }
}
