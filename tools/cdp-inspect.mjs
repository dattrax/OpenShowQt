#!/usr/bin/env node
// Chrome DevTools Protocol driver used to inspect FreeShow 1.6.7 while writing
// docs/reference.md, and later to compare our own output against it. It is a
// development tool: nothing in the OpenShowQt build uses it.
//
// usage: CDP_PORT=9222 node tools/cdp-inspect.mjs <command> <targetMatch> [args]
//   list                            print every target
//   info    <match>                 url, title, viewport, dpr, visibility
//   shot    <match> <out.png> [clipSelector] [scale]
//                                   PNG of the viewport or of one element
//   eval    <match> <exprFile>      run an expression, print JSON result
//   metrics <match> <w> <h>         override the viewport (0 0 clears)
//   key     <match> <Key> [code]    send a key press/release
//   click   <match> <selector>      click the centre of the first match
//   clickxy <match> <x> <y>         click at viewport coordinates
//   drag    <match> <x1> <y1> <x2> <y2>   press, move, release
//   hover   <match> <selector>      move the mouse over the first match
//   leave   <match>                 move the mouse far away
//   wait    <match> <selector>      poll until the selector exists
//   scroll  <match> <dy> [selector] wheel the page
//
// <match> is "id:<targetIdPrefix>", "#N" for the Nth page target, or a substring
// of "url title". Target order is not stable, so identify targets by feature
// first (see docs/reference.md section 12).
//
// Environment: CDP_FROM_SURFACE=0 captures windows that are off screen, which is
// how an output window with no display attached was read; CDP_FRONT=1 raises the
// window first, which unsticks captures of an occluded window. Captures retry
// three times.

import { readFileSync, writeFileSync } from 'node:fs'

const PORT = Number(process.env.CDP_PORT || 9222)

async function targetList() {
    const res = await fetch(`http://127.0.0.1:${PORT}/json/list`)
    if (!res.ok) throw new Error(`CDP ${PORT} not reachable: ${res.status}`)
    return res.json()
}

class Session {
    constructor(ws) {
        this.ws = ws
        this.nextId = 0
        this.pending = new Map()
        ws.onmessage = (event) => {
            const message = JSON.parse(event.data)
            if (message.id && this.pending.has(message.id)) {
                const { resolve, reject } = this.pending.get(message.id)
                this.pending.delete(message.id)
                if (message.error) reject(new Error(JSON.stringify(message.error)))
                else resolve(message.result)
            }
        }
    }

    static async open(match) {
        const targets = await targetList()
        const usable = targets.filter((t) => t.webSocketDebuggerUrl)
        const pick = match.startsWith('#') ? usable[Number(match.slice(1))]
            : match.startsWith('id:') ? usable.find((t) => t.id.toLowerCase().startsWith(match.slice(3).toLowerCase()))
            : usable.find((t) => (t.url + ' ' + t.title).includes(match))
        if (!pick) {
            throw new Error(`no target matching "${match}":\n` +
                usable.map((t, i) => `  #${i} ${t.type} ${t.title} ${t.url}`).join('\n'))
        }
        const ws = new WebSocket(pick.webSocketDebuggerUrl)
        await new Promise((resolve, reject) => {
            ws.onopen = resolve
            ws.onerror = () => reject(new Error('websocket failed to ' + pick.webSocketDebuggerUrl))
        })
        const session = new Session(ws)
        await session.send('Page.enable')
        await session.send('Runtime.enable')
        session.title = pick.title
        session.url = pick.url
        return session
    }

    send(method, params = {}) {
        const id = ++this.nextId
        this.ws.send(JSON.stringify({ id, method, params }))
        return new Promise((resolve, reject) => this.pending.set(id, { resolve, reject }))
    }

    async evaluate(expression) {
        const result = await this.send('Runtime.evaluate', {
            expression, returnByValue: true, awaitPromise: true, includeCommandLineAPI: true,
        })
        if (result.exceptionDetails) {
            throw new Error(result.exceptionDetails.exception?.description || 'evaluation failed')
        }
        return result.result.value
    }

    async centre(selector) {
        const box = await this.evaluate(`(() => {
            const el = document.querySelector(${JSON.stringify(selector)})
            if (!el) return null
            const r = el.getBoundingClientRect()
            return { x: r.left + r.width / 2, y: r.top + r.height / 2, width: r.width, height: r.height,
                     text: (el.innerText || '').replace(/\\s+/g, ' ').trim().slice(0, 60) }
        })()`)
        if (!box) throw new Error('no element for selector ' + selector)
        return box
    }

    async mouse(type, x, y, buttons = 0) {
        await this.send('Input.dispatchMouseEvent', { type, x, y, buttons, clickCount: type === 'mousePressed' ? 1 : 0, button: 'left' })
    }

    close() { this.ws.close() }
}

const [command, match, ...rest] = process.argv.slice(2)

if (command === 'list') {
    const targets = await targetList()
    console.log(targets.map((t) => `${t.type}\t${t.title}\t${t.url}`).join('\n'))
    process.exit(0)
}

if (!command || !match) {
    console.error('usage: cdp-inspect.mjs <list|info|shot|eval|metrics|key|click|clickxy|drag|hover|leave|wait|scroll> <targetMatch> [args]')
    process.exit(2)
}

const session = await Session.open(match)

try {
    if (command === 'info') {
        console.log(JSON.stringify({
            title: session.title, url: session.url,
            ...await session.evaluate(`(() => ({
                innerWidth: window.innerWidth, innerHeight: window.innerHeight,
                devicePixelRatio: window.devicePixelRatio,
                visibility: document.visibilityState,
                bodyClass: document.body.className,
            }))()`),
        }, null, 2))
    } else if (command === 'shot') {
        const params = { format: 'png' }
        if (process.env.CDP_FROM_SURFACE === '0') params.fromSurface = false
        if (rest[1]) {
            const box = await session.centre(rest[1])
            params.clip = {
                x: box.x - box.width / 2, y: box.y - box.height / 2,
                width: box.width, height: box.height, scale: Number(rest[2] || 1),
            }
            params.captureBeyondViewport = false
        }
        if (process.env.CDP_FRONT === '1') await session.send('Page.bringToFront')
        let data = null
        for (let attempt = 1; attempt <= 3 && !data; attempt += 1) {
            try {
                const result = await Promise.race([
                    session.send('Page.captureScreenshot', params),
                    new Promise((_, reject) => setTimeout(() => reject(new Error('capture timed out')), 15000)),
                ])
                data = result.data
            } catch (err) {
                console.log(`attempt ${attempt} failed: ${err.message}`)
                if (process.env.CDP_FRONT === '1') await session.send('Page.bringToFront')
                await new Promise((r) => setTimeout(r, 500))
            }
        }
        if (!data) throw new Error('no screenshot after 3 attempts')
        writeFileSync(rest[0], Buffer.from(data, 'base64'))
        console.log(`wrote ${rest[0]} (${data.length} b64 bytes)`)
    } else if (command === 'eval') {
        const value = await session.evaluate(readFileSync(rest[0], 'utf8'))
        console.log(typeof value === 'string' ? value : JSON.stringify(value, null, 2))
    } else if (command === 'metrics') {
        const [width, height] = rest.map(Number)
        if (width === 0) await session.send('Emulation.clearDeviceMetricsOverride')
        else await session.send('Emulation.setDeviceMetricsOverride',
            { width, height, deviceScaleFactor: 0, mobile: false })
        console.log(`viewport ${width}x${height}`)
    } else if (command === 'key') {
        const [key, code = key] = rest
        const codeName = /^Key([A-Z])$/.test(code) ? code : (/^[a-z]$/.test(code) ? 'Key' + code.toUpperCase() : code)
        await session.send('Input.dispatchKeyEvent', { type: 'rawKeyDown', key, code: codeName, windowsVirtualKeyCode: key.toUpperCase().charCodeAt(0) })
        if (key.length === 1) await session.send('Input.dispatchKeyEvent', { type: 'char', key, code: codeName, text: key })
        await session.send('Input.dispatchKeyEvent', { type: 'keyUp', key, code: codeName, windowsVirtualKeyCode: key.toUpperCase().charCodeAt(0) })
        console.log(`sent ${key}`)
    } else if (command === 'click') {
        const box = await session.centre(rest[0])
        await session.mouse('mousePressed', box.x, box.y, 1)
        await session.mouse('mouseReleased', box.x, box.y, 0)
        console.log(`clicked ${rest[0]} at ${Math.round(box.x)},${Math.round(box.y)} text="${box.text}"`)
    } else if (command === 'clickxy') {
        const [x, y] = rest.map(Number)
        await session.mouse('mouseMoved', x, y)
        await session.mouse('mousePressed', x, y, 1)
        await session.mouse('mouseReleased', x, y, 0)
        console.log(`clicked ${x},${y}`)
    } else if (command === 'hover') {
        const box = await session.centre(rest[0])
        await session.mouse('mouseMoved', box.x, box.y)
        console.log(`hovered ${rest[0]} at ${Math.round(box.x)},${Math.round(box.y)} text="${box.text}"`)
    } else if (command === 'drag') {
        const [x1, y1, x2, y2] = rest.map(Number)
        await session.mouse('mouseMoved', x1, y1)
        await session.mouse('mousePressed', x1, y1, 1)
        // FreeShow's resize handler ignores coalesced moves, so move slowly and
        // in discrete steps rather than in one burst.
        const steps = 25
        for (let i = 1; i <= steps; i += 1) {
            await session.mouse('mouseMoved', x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps, 1)
            await new Promise((r) => setTimeout(r, 20))
        }
        await session.mouse('mouseReleased', x2, y2, 0)
        console.log(`dragged ${x1},${y1} -> ${x2},${y2}`)
    } else if (command === 'leave') {
        await session.mouse('mouseMoved', 2, 2)
        console.log('moved mouse to 2,2')
    } else if (command === 'wait') {
        const deadline = Date.now() + 10000
        let found = null
        while (Date.now() < deadline && !found) {
            found = await session.evaluate(`!!document.querySelector(${JSON.stringify(rest[0])})`)
            if (!found) await new Promise((r) => setTimeout(r, 200))
        }
        console.log(found ? `found ${rest[0]}` : `missing ${rest[0]}`)
    } else if (command === 'scroll') {
        const box = await session.centre(rest[1] || 'body')
        await session.send('Input.dispatchMouseEvent', { type: 'mouseWheel', x: box.x, y: box.y, deltaX: 0, deltaY: Number(rest[0]) })
        console.log(`scrolled ${rest[0]} at ${rest[1] || 'body'}`)
    } else {
        console.error('unknown command ' + command)
        process.exit(2)
    }
} finally {
    session.close()
}
