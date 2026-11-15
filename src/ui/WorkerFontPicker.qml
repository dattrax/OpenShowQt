import QtQuick
import QtQuick.Controls

/*
 * UI part: WorkerFontPicker; contract: docs/ui/control-sets.md#workerfontpicker
 *
 * The font row every inspector needs (task CS.5, revised 2026-11-10):
 * the family field IS the picker — clicking the typeface name opens a
 * normal non-modal drop-down under the field, listing the audience
 * worker's OWN families, with a chevron saying so. There is no separate
 * Browse button. Never Qt's installed fonts: a picker of the
 * operator's desktop would offer names the show silently falls back from.
 * A worker that cannot enumerate leaves the field typeable (the path that
 * always worked and works with the worker gone); the missing-family
 * warning needs a COMPLETED worker list, and a requested name is never
 * rewritten by a fallback verdict — the scene keeps what the author asked
 * for. The positive "installed on the audience's machine" sentence is
 * retired; only a real problem speaks.
 *
 * Extracted verbatim in behaviour from ThemeEditorPage's row (task TS.4):
 * the status line refuses to guess while no list has landed, the field
 * sleeps for a worker that cannot enumerate, and choosing a family is
 * exactly as if the operator had typed it. Hosts pass their pinned
 * automation names; the popup lives at the page level through
 * `overlayParent` because a centred modal cannot hang off a row inside a
 * scroll rail.
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
    property bool showInheritedValues: false
    // Both are the host's prose: the Songs row says ("\"\" = platform"),
    // the Bible row ("\"\" = theme's").
    property string label: "Font family (\"\" = platform)"
    property bool showStatus: true
    property bool allowOpen: true          // ANDed with worker capability
    // The Edit page's badge tradition (see BackgroundControls): a ↺ named
    // <prefix>FontFamily stands beside the row while the target owns a
    // family to hand back.
    property bool showRevertChip: false
    property string revertBadgePrefix: "customizeBadge"
    // The page the centred popup belongs to (a row inside a scroll rail is
    // the wrong coordinate system for a modal).
    property Item overlayParent: null
    // Kept current by sync(); the enabled bindings below watch it, so a
    // host that moves `allowOpen` re-evaluates without another sync pass.
    property bool editable: false

    property string labelObjectName: ""
    property string familyObjectName: "fontFamilyField"
    property string statusObjectName: "fontStatus"
    property string popupObjectName: "fontPopup"
    property string searchObjectName: "fontSearch"
    property string listStatusObjectName: "fontListStatus"
    property string listObjectName: "fontList"
    property string rowObjectName: "fontRow"

    signal familyChosen(string name)

    spacing: 4
    width: parent ? parent.width : 0

    // The worker can enumerate AND this target may be edited: the field
    // becomes the drop-down trigger and stops taking typed text. Without
    // a list (no worker, an old worker) it stays a typed field, which is
    // the one path that does not depend on the audience machine.
    readonly property bool canList: worker.fontListSupported === true
                                    && picker.allowOpen && picker.editable

    function state() { return adapter.fieldState(picker.field) }
    function openPicker() {
        fontSearchField.text = ""
        if (worker.fontListState === "idle")
            worker.refreshFontFamilies()
        fontPopup.open()
    }
    function resetOrSync() {
        if (!adapter.reset(picker.field))
            picker.sync()
    }
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
        if (picker.sparse && !picker.showInheritedValues)
            familyField.text = s.isSet === true ? String(s.ownValue) : ""
        else
            familyField.text = s.value === undefined ? "" : String(s.value)
        statusLine.text = picker.statusText(s)
        statusLine.color = picker.familyMissing(s) ? "#e0a94a" : "#888888"
        statusLine.visible = picker.showStatus && statusLine.text !== ""
        familyField.readOnly = picker.canList
        familyChip.visible = picker.showRevertChip && s.canReset === true
    }
    function commitTyped() {
        const t = familyField.text.trim()
        if (picker.sparse) {
            if (picker.showInheritedValues && t === String(state().value)) {
                picker.sync()
                return  // viewing an inherited family never materializes it
            }
            // "" is the sparse target's silence — the field goes back to
            // inheriting rather than storing an empty name.
            if (t === "") {
                if (!adapter.reset(picker.field))
                    picker.sync()
            } else if (!adapter.commit(picker.field, t)) {
                picker.sync()
            }
        } else if (t === String(state().value)) {
            picker.sync()  // a focus-out restating the family is no edit
        } else if (!adapter.commit(picker.field, t)) {
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
        return picker.sparse && !picker.showInheritedValues
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
                : ""
    }

    Connections {
        target: picker.adapter
        function onChanged() { picker.sync() }
        function onTargetChanged() { picker.sync() }
    }
    Component.onCompleted: picker.sync()

    Text {
        objectName: picker.labelObjectName
        visible: picker.label !== ""
        text: picker.label
        color: "#b0b0b0"
        font.pixelSize: 11
    }
    Row {
        spacing: 4
        Item {
            id: familyBox
            width: picker.width - (familyChip.visible ? familyChip.width + 4 : 0)
            height: familyField.height
            PanelField {
                id: familyField
                objectName: picker.familyObjectName
                anchors.fill: parent
                onEditingFinished: picker.commitTyped()
            }
            Text {
                visible: picker.canList
                anchors.right: parent.right
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                text: fontPopup.visible ? "\u25b4" : "\u25be"
                color: "#b0b0b0"
                font.pixelSize: 12
            }
            MouseArea {
                anchors.fill: parent
                enabled: picker.canList
                visible: picker.canList
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: fontPopup.visible ? fontPopup.close() : picker.openPicker()
            }
        }
        // The Edit page's RevertBadge, extracted (see BackgroundControls).
        Rectangle {
            id: familyChip
            objectName: picker.revertBadgePrefix
                        + picker.field.charAt(0).toUpperCase()
                        + picker.field.substring(1)
            Accessible.name: "Revert " + picker.field + " customization"
            width: 24
            height: 30
            radius: 4
            visible: false // sync() decides, from the model's canReset
            color: familyChipArea.containsMouse ? "#565659" : "#3a3a3c"
            border.color: "#6a6a6d"
            Text {
                anchors.centerIn: parent
                color: "#ffd666"
                font.pixelSize: 13
                text: "\u21ba"
            }
            MouseArea {
                id: familyChipArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: picker.resetOrSync()
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

    // The family drop-down (review 2026-11-10): a normal, NON-MODAL list
    // attached under the field rather than a centred modal dialog. It
    // lists ONLY the audience worker's own families, filters as the
    // operator types, and choosing a row writes through the same verb a
    // typed name uses. Clicking away or Escape closes it; the list never
    // takes the page hostage.
    Popup {
        id: fontPopup
        objectName: picker.popupObjectName
        parent: familyBox
        x: familyBox.width - width
        y: familyBox.height + 2
        width: Math.max(260, familyBox.width)
        height: Math.min(320, fontSearchField.height + 8
                         + Math.max(28, fontPreviewList.contentHeight)
                         + (fontListStatus.visible ? fontListStatus.height + 4 : 0)
                         + 12)
        modal: false
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        padding: 6
        background: Rectangle { color: "#2b2b2b"; border.color: "#555555"; radius: 4 }
        onOpened: {
            fontSearchField.forceActiveFocus()
            const at = fontPreviewList.model.indexOf(familyField.text)
            if (at >= 0)
                fontPreviewList.positionViewAtIndex(at, ListView.Center)
        }
        // The host page standing down (mode exit, another page) takes its
        // popup with it — an orphaned overlay would keep the keyboard the
        // shell is trying to hand back (the TS.4 rule).
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
            spacing: 4
            PanelField {
                id: fontSearchField
                objectName: picker.searchObjectName
                width: parent.width
                height: 28
                placeholderText: "search families…"
            }
            Text {
                id: fontListStatus
                objectName: picker.listStatusObjectName
                width: parent.width
                visible: text !== ""
                wrapMode: Text.WordWrap
                color: "#888888"
                font.pixelSize: 10
                text: worker.fontListState === "loading" ? "asking the audience's worker…"
                      : ((worker.fontListState === "ready" && fontPreviewList.count === 0)
                         ? "no families match" : "")
            }
            ListView {
                id: fontPreviewList
                objectName: picker.listObjectName
                width: parent.width
                height: Math.min(260, Math.max(28, contentHeight))
                clip: true
                model: picker.fontMatches(fontSearchField.text)
                Connections {
                    target: worker
                    function onFontListChanged() {
                        fontPreviewList.model = picker.fontMatches(fontSearchField.text)
                    }
                }
                delegate: ItemDelegate {
                    id: fontRow
                    width: fontPreviewList.width
                    height: 28
                    objectName: picker.rowObjectName
                    required property string modelData
                    text: modelData
                    font.family: modelData
                    font.pixelSize: 12
                    leftPadding: 8
                    rightPadding: 8
                    topPadding: 0
                    bottomPadding: 0
                    hoverEnabled: true
                    background: Rectangle {
                        color: fontRow.hovered ? "#3d3d40"
                               : (modelData === familyField.text ? "#333336" : "transparent")
                    }
                    onClicked: {
                        familyField.text = modelData
                        picker.commitTyped()
                        fontPopup.close()
                    }
                }
            }
        }
    }
}
