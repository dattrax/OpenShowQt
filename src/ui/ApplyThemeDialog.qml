import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Task PT.7: the ONE whole-item apply workflow, opened from a left-hand song
// or passage row (or a Library presentation row). The target identity is
// captured when the dialog opens and every action uses it — a later cursor
// move cannot retarget the apply.
//
// Apply only is the safe default and keeps every current slide break. Apply
// and reflow measures the item's OWN words against the chosen look and is
// offered only when the item is structurally eligible; the reason is shown
// rather than hidden. An unprepared Library song only needs Remember theme:
// it has no service breaks yet, and applying must not create a hidden
// presentation.
Dialog {
    id: dialog
    objectName: "applyThemeDialog"
    modal: true
    title: "Apply theme"
    width: Math.min(520, (parent ? parent.width : 520) - 40)
    closePolicy: Popup.CloseOnEscape
    padding: 16

    // Captured target.
    property string targetShowId: ""
    property string catalogItemId: ""
    property string itemTitle: ""
    property int occurrenceCount: 0
    // A Library song with no presentation in this service: remember only.
    property bool rememberOnly: false
    property string defaultThemeId: ""
    property string preparedSnapshot: ""

    property bool preparing: false
    property string notice: ""

    // Set explicitly when the dialog opens. A QML binding over a plain
    // function call is cached until a DEPENDENCY changes, so reopening on the
    // SAME show would keep an eligibility answer from before an edit.
    property var eligibility: ({})
    readonly property bool canReflow:
        !dialog.rememberOnly && dialog.targetShowId !== "" &&
        dialog.eligibility.eligible === true
    readonly property string eligibilityText: {
        if (dialog.rememberOnly)
            return "This song is not in the service yet. The chosen look is remembered for its next Add; nothing is added to the running order."
        if (dialog.eligibility.eligible === true)
            return "Apply and reflow re-paginates the words this item already holds."
        return "Reflow unavailable: " + (dialog.eligibility.reason || "this item's structure")
               + ". Apply only still works."
    }

    function chooseDefaultTheme() {
        const applied = dialog.targetShowId !== ""
                        ? (docs.showInfo(dialog.targetShowId).appliedThemeId || "")
                        : ""
        if (applied !== "" && dialog.themeIndex(applied) >= 0) {
            themePicker.currentIndex = dialog.themeIndex(applied)
            return
        }
        const current = docs.currentTheme ? (docs.currentTheme.id || "") : ""
        if (current !== "" && dialog.themeIndex(current) >= 0) {
            themePicker.currentIndex = dialog.themeIndex(current)
            return
        }
        // Prefer a named theme over the seeded Default floor.
        for (let i = 0; i < docs.themes.length; ++i) {
            if (docs.themes[i].id !== "default") {
                themePicker.currentIndex = i
                return
            }
        }
        themePicker.currentIndex = docs.themes.length > 0 ? 0 : -1
    }
    function themeIndex(id) {
        for (let i = 0; i < docs.themes.length; ++i) {
            if (docs.themes[i].id === id)
                return i
        }
        return -1
    }
    function chosenSnapshot() {
        const id = themePicker.currentValue || ""
        if (id === "")
            return ""
        return docs.themeSnapshotJson(id)
    }

    function openForShow(showId, title) {
        dialog.targetShowId = showId
        dialog.itemTitle = title !== undefined && title !== "" ? title
                           : (docs.showInfo(showId).name || "")
        dialog.catalogItemId = docs.showInfo(showId).catalogItemId || ""
        dialog.rememberOnly = false
        dialog.occurrenceCount = docs.playlistEntries.filter(
            row => row.showId === showId).length
        dialog.preparing = false
        dialog.notice = ""
        dialog.eligibility = reflow.eligibility(showId)
        dialog.chooseDefaultTheme()
        dialog.open()
    }
    function openForCatalogItem(itemId, title) {
        const item = library.itemOf(itemId)
        dialog.catalogItemId = itemId
        dialog.itemTitle = title !== undefined && title !== ""
                           ? title : (item.title || "")
        const linked = docs.showForCatalogItem(itemId)
        if (linked !== "") {
            dialog.openForShow(linked, dialog.itemTitle)
            dialog.catalogItemId = itemId
            return
        }
        dialog.targetShowId = ""
        dialog.rememberOnly = true
        dialog.occurrenceCount = 0
        dialog.preparing = false
        dialog.notice = ""
        dialog.eligibility = ({})
        dialog.chooseDefaultTheme()
        dialog.open()
    }
    function cancelPreparation() {
        if (dialog.preparing)
            reflow.cancel()
        dialog.preparing = false
    }

    onClosed: dialog.cancelPreparation()

    Connections {
        target: reflow
        function onPrepared(ok, error) {
            if (!dialog.preparing)
                return
            dialog.preparing = false
            if (!ok) {
                dialog.notice = error
                return
            }
            if (!applySession.applyReflow(dialog.targetShowId,
                                          dialog.preparedSnapshot,
                                          reflow.plan(),
                                          dialog.catalogItemId,
                                          dialog.catalogItemId !== "")) {
                dialog.notice = applySession.lastError
            }
        }
    }
    Connections {
        target: applySession
        function onApplied(ok, error) {
            if (ok) {
                dialog.close()
            } else {
                dialog.notice = error
            }
        }
    }

    contentItem: ColumnLayout {
        spacing: 10

        Label {
            objectName: "applyThemeTitle"
            text: dialog.itemTitle
            font.pixelSize: 16
            font.bold: true
            elide: Text.ElideRight
            Layout.fillWidth: true
        }
        Label {
            objectName: "applyThemeTargetLine"
            text: dialog.rememberOnly
                  ? "Not in this service"
                  : (dialog.occurrenceCount === 1
                     ? "1 occurrence in the running order"
                     : dialog.occurrenceCount + " occurrences in the running order")
            color: "#b0b0b0"
            Layout.fillWidth: true
        }
        RowLayout {
            Layout.fillWidth: true
            Label { text: "Theme" }
            ComboBox {
                id: themePicker
                objectName: "applyThemePicker"
                Layout.fillWidth: true
                textRole: "name"
                valueRole: "id"
                model: docs.themes
            }
        }
        Label {
            objectName: "applyThemeExplanation"
            text: dialog.rememberOnly
                  ? "The chosen look is remembered in the Library for this song's next Add."
                  : "Replaces text formatting and placement for the whole item; words, media and notes stay."
            wrapMode: Text.WordWrap
            color: "#b0b0b0"
            Layout.fillWidth: true
        }
        Label {
            objectName: "applyThemeEligibility"
            text: dialog.preparing ? "Measuring the words\u2026" : dialog.eligibilityText
            wrapMode: Text.WordWrap
            color: dialog.canReflow || dialog.rememberOnly ? "#9fd3a8" : "#e0b060"
            Layout.fillWidth: true
        }
        Label {
            objectName: "applyThemeNotice"
            visible: dialog.notice !== ""
            text: dialog.notice
            wrapMode: Text.WordWrap
            color: "#ff9a9a"
            Layout.fillWidth: true
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Button {
                objectName: "applyThemeOnlyButton"
                text: "Apply only"
                enabled: !dialog.preparing && !dialog.rememberOnly
                onClicked: {
                    const snapshot = dialog.chosenSnapshot()
                    if (snapshot === "") {
                        dialog.notice = docs.status
                        return
                    }
                    dialog.notice = ""
                    applySession.applyOnly(dialog.targetShowId, snapshot,
                                           dialog.catalogItemId,
                                           dialog.catalogItemId !== "")
                }
            }
            Button {
                objectName: "applyThemeReflowButton"
                text: "Apply and reflow"
                enabled: dialog.canReflow && !dialog.preparing
                onClicked: {
                    const snapshot = dialog.chosenSnapshot()
                    if (snapshot === "") {
                        dialog.notice = docs.status
                        return
                    }
                    dialog.preparedSnapshot = snapshot
                    dialog.notice = ""
                    dialog.preparing = true
                    if (!reflow.prepare(dialog.targetShowId, snapshot)) {
                        dialog.preparing = false  // the refusal already fired
                    }
                }
            }
            Button {
                objectName: "applyThemeRememberButton"
                visible: dialog.rememberOnly
                text: "Remember theme"
                enabled: !dialog.preparing
                onClicked: {
                    const snapshot = dialog.chosenSnapshot()
                    if (snapshot === "") {
                        dialog.notice = docs.status
                        return
                    }
                    if (library.rememberItemTheme(dialog.catalogItemId, snapshot)) {
                        dialog.close()
                    } else {
                        dialog.notice = library.status
                    }
                }
            }
            Item { Layout.fillWidth: true }
            Button {
                objectName: "applyThemeCancelButton"
                text: "Cancel"
                onClicked: dialog.close()
            }
        }
    }
}
