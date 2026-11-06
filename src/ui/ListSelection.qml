import QtQuick

// UI part: ListSelection. Stable identities shared by Library and service lists.
QtObject {
    property var keys: []
    property string currentKey: ""
    property string anchorKey: ""

    function contains(key) { return keys.indexOf(key) >= 0 }
    function clear() { keys = []; currentKey = ""; anchorKey = "" }
    function select(key, order, modifiers) {
        const control = (modifiers & Qt.ControlModifier) !== 0
        const shift = (modifiers & Qt.ShiftModifier) !== 0
        let next = control ? keys.slice() : []
        const anchor = order.indexOf(anchorKey)
        const at = order.indexOf(key)
        if (at < 0) return
        if (shift && anchor >= 0) {
            for (let i = Math.min(anchor, at); i <= Math.max(anchor, at); ++i)
                if (next.indexOf(order[i]) < 0) next.push(order[i])
        } else if (control) {
            const i = next.indexOf(key)
            if (i < 0) next.push(key)
            else next.splice(i, 1)
            anchorKey = key
        } else {
            next = [key]
            anchorKey = key
        }
        currentKey = key
        keys = next
    }
    function selectAll(order) {
        keys = order.slice()
        currentKey = order.length ? order[order.length - 1] : ""
        anchorKey = order.length ? order[0] : ""
    }
    function retain(order) {
        const next = keys.filter(key => order.indexOf(key) >= 0)
        if (next.length !== keys.length) keys = next
        if (order.indexOf(currentKey) < 0) currentKey = next.length ? next[next.length - 1] : ""
        if (order.indexOf(anchorKey) < 0) anchorKey = currentKey
    }
}
