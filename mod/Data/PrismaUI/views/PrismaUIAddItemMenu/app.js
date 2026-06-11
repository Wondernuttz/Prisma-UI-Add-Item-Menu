// Prisma UI AddItem Menu — front-end logic.
//
// Two-level drill-down navigation:
//   1. Mods view  — list of every plugin contributing items, with a search box
//                   to filter the list of mod names. Click a mod to drill in.
//   2. Items view — items belonging to the selected mod, server-side filtered
//                   by type/search and paginated. Click an item, set qty, add.
//
// All work happens against a small in-memory state object. We never hold the
// full item set client-side — each query is fetched fresh from C++ via
// __prismaUIAddItem_query / __prismaUIAddItem_getPlugins. This keeps the DOM
// tiny and makes huge modlists (50k+ items) scroll smoothly under Ultralight.
//
// Bridge contract — these globals are exposed by PrismaUI's RegisterJSListener
// on the C++ side. They take a single string argument; for structured data we
// JSON.stringify on the way out and parse on the response callback.
//
//   __prismaUIAddItem_getPlugins({excludeVanilla})  → __prismaUIAddItem_onPluginList(JSON)
//   __prismaUIAddItem_query     (req JSON)          → __prismaUIAddItem_onQueryResult(JSON)
//   __prismaUIAddItem_addItem   ({plugin,localId,count})
//   __prismaUIAddItem_close     ("")
//
// And from C++ → JS (called by Bridge::OpenMenu):
//   __prismaUIAddItem_onMenuOpen(excludeVanillaDefault: "true"|"false")
//     → resets to mods view + re-queries plugin list. Each menu open = fresh state.

(function() {
    'use strict';

    // ── State ──────────────────────────────────────────────────────────────
    const state = {
        view: 'mods',              // 'mods' | 'items'

        // Mod list
        plugins: [],               // [{name, count}]
        modSearch: '',
        excludeVanilla: false,

        // Items in selected mod
        selectedPlugin: '',        // name of mod we're drilled into
        type: '',                  // type filter ("" = all)
        search: '',
        page: 0,
        pageSize: 100,
        totalMatched: 0,
        totalAll: 0,
        items: [],
        selectedIndex: -1,
        pendingItemKey: '',
        pendingItemIndex: -1,
        addPending: false,
        usePending: false,
        lastAction: 'add',
        qtyLocked: false,
        hotkeyScancode: 0x3B,
    };

    const els = {
        // mods view
        modsView:        document.getElementById('modsView'),
        modSearch:       document.getElementById('modSearch'),
        excludeToggle:   document.getElementById('excludeVanillaToggle'),
        modList:         document.getElementById('modList'),
        modsStatus:      document.getElementById('modsStatus'),

        // items view
        itemsView:       document.getElementById('itemsView'),
        itemsTitle:      document.getElementById('itemsTitle'),
        itemSearch:      document.getElementById('itemSearch'),
        tabs:            document.getElementById('tabs'),
        itemList:        document.getElementById('itemList'),
        itemsStatus:     document.getElementById('itemsStatus'),
        useBtn:          document.getElementById('useBtn'),
        addBtn:          document.getElementById('addBtn'),
        addToast:        document.getElementById('addToast'),
        qtyInput:        document.getElementById('qtyInput'),
        qtyMinus:        document.getElementById('qtyMinus'),
        qtyPlus:         document.getElementById('qtyPlus'),
        qtyLock:         document.getElementById('qtyLock'),
        prevPage:        document.getElementById('prevPage'),
        nextPage:        document.getElementById('nextPage'),
        pageLabel:       document.getElementById('pageLabel'),
    };

    // ── Bridge helpers ─────────────────────────────────────────────────────
    function debugBridge(msg) {
        try { console.log('[PrismaUIAddItemMenu] ' + msg); } catch (e) {}
        try {
            if (typeof window.__prismaUIAddItem_debug === 'function') {
                window.__prismaUIAddItem_debug(String(msg));
            }
        } catch (e) {}
    }

    function setBridgeWaitStatus(fn, attempt) {
        if (fn !== '__prismaUIAddItem_getPlugins' && fn !== '__prismaUIAddItem_query') return;
        const target = fn === '__prismaUIAddItem_query' ? els.itemsStatus : els.modsStatus;
        if (!target) return;
        target.textContent = attempt < 50
            ? 'Waiting for native bridge...'
            : 'Native bridge did not bind; check PrismaUIAddItemMenu.log';
    }

    function callBridgeRetry(fn, payload, attempt) {
        const wirePayload = typeof payload === 'string' ? payload : JSON.stringify(payload);
        if (typeof window[fn] === 'function') {
            try {
                if (attempt > 0) debugBridge(fn + ' bound after ' + attempt + ' retries');
                window[fn](wirePayload);
            } catch (e) {
                debugBridge(fn + ' call threw: ' + (e && e.message ? e.message : e));
                console.error('bridge call failed', fn, e);
            }
            return;
        }

        setBridgeWaitStatus(fn, attempt);
        if (attempt === 0) debugBridge(fn + ' not bound yet; retrying');
        if (attempt < 50) {
            window.setTimeout(() => callBridgeRetry(fn, payload, attempt + 1), 100);
        } else {
            debugBridge(fn + ' never bound');
        }
    }

    function callBridge(fn, payload) {
        callBridgeRetry(fn, payload, 0);
    }

    function debounce(fn, ms) {
        let t;
        return function(...args) {
            clearTimeout(t);
            t = setTimeout(() => fn.apply(this, args), ms);
        };
    }

    // ── 3D model preview (PrismaUI ModelPreview API, optional) ─────────────
    // Only present on PrismaUI builds with the ModelPreview feature; on
    // upstream/flat builds these globals are undefined and the pane stays hidden.
    function previewAvailable() {
        return typeof window.__prismaUI_showModelPreview === 'function';
    }

    function previewRectPx() {
        const r = document.getElementById('previewRect').getBoundingClientRect();
        return { x: Math.round(r.left), y: Math.round(r.top), w: Math.round(r.width), h: Math.round(r.height) };
    }

    function showPreviewNow(it) {
        if (!previewAvailable() || !it) return;
        const rect = previewRectPx();
        if (rect.w <= 0 || rect.h <= 0) return;
        window.__prismaUI_showModelPreview(JSON.stringify({
            plugin: it.plugin,
            localId: Number(it.localId) >>> 0,
            x: rect.x, y: rect.y, w: rect.w, h: rect.h,
        }));
    }

    function hidePreview() {
        if (previewAvailable()) window.__prismaUI_hideModelPreview('');
    }

    function syncPreviewPane() {
        const pane = document.getElementById('previewPane');
        pane.classList.toggle('hidden', !previewAvailable());
        if (!previewAvailable()) return;
        // Square preview sized to whatever the pane allows
        const rect = document.getElementById('previewRect');
        const size = Math.max(200, Math.min(pane.clientWidth - 16, pane.clientHeight - 70));
        rect.style.width = size + 'px';
        rect.style.height = size + 'px';
    }

    // ── View switching ─────────────────────────────────────────────────────
    let addFeedbackTimer = 0;
    let addToastTimer = 0;

    function typeLabel(type) {
        return ({
            WEAP: 'WEAP', ARMO: 'ARMO', POTN: 'POTION', POIS: 'POISON', FOOD: 'FOOD',
            INGR: 'INGR', BOOK: 'BOOK', STOM: 'TOME', MISC: 'MISC', AMMO: 'AMMO',
            SLGM: 'SOUL', KEYM: 'KEY',
        })[type] || type || '';
    }

    function directActionLabel(it) {
        if (!it) return 'No Direct Action';
        return ({
            POTN: 'Drink',
            FOOD: 'Eat',
            STOM: 'Learn Spell',
        })[it.type] || 'No Direct Action';
    }

    function hasDirectAction(it) {
        return directActionLabel(it) !== 'No Direct Action';
    }

    function itemKey(it) {
        if (!it) return '';
        return `${it.plugin}|${Number(it.localId) >>> 0}`;
    }

    function setButtonMode(btn, mode, label) {
        btn.classList.remove('adding', 'added', 'failed');
        if (mode) btn.classList.add(mode);
        btn.textContent = label;
        updateActionButtons();
    }

    function flashAddToast(message, ok) {
        if (!els.addToast) return;
        clearTimeout(addToastTimer);
        els.addToast.textContent = message;
        els.addToast.classList.toggle('ok', !!ok);
        els.addToast.classList.toggle('error', !ok);
        els.addToast.classList.remove('hidden', 'show');
        void els.addToast.offsetWidth;
        els.addToast.classList.add('show');
        addToastTimer = window.setTimeout(() => {
            els.addToast.classList.remove('show');
            addToastTimer = window.setTimeout(() => els.addToast.classList.add('hidden'), 220);
        }, ok ? 1500 : 2200);
    }

    function resetAddFeedbackSoon() {
        clearTimeout(addFeedbackTimer);
        addFeedbackTimer = window.setTimeout(() => {
            state.addPending = false;
            state.usePending = false;
            restoreActionButtonLabels();
        }, 1200);
    }

    function showView(name) {
        state.view = name;
        els.modsView.classList.toggle('active',  name === 'mods');
        els.itemsView.classList.toggle('active', name === 'items');
    }

    // ── Backend response handlers (called from C++) ────────────────────────
    window.__prismaUIAddItem_onMenuOpen = function(excludeVanillaDefaultStr) {
        debugBridge('onMenuOpen excludeVanilla=' + excludeVanillaDefaultStr);
        // Each menu open resets to the mod list. Apply the default vanilla
        // toggle from settings on this fresh open (user can still flip it).
        const def = (excludeVanillaDefaultStr === 'true');
        state.excludeVanilla = def;
        els.excludeToggle.checked = def;

        state.modSearch = '';
        els.modSearch.value = '';
        state.pendingItemKey = '';
        state.pendingItemIndex = -1;

        showView('mods');
        els.modsStatus.textContent = 'Loading mod list…';
        callBridge('__prismaUIAddItem_getPlugins', { excludeVanilla: state.excludeVanilla });
    };

    window.__prismaUIAddItem_onPluginList = function(jsonStr) {
        let arr;
        try { arr = JSON.parse(jsonStr); }
        catch (e) { debugBridge('bad plugin list: ' + e.message); console.error('bad plugin list', e); return; }
        if (!Array.isArray(arr)) return;

        state.plugins = arr;
        debugBridge('plugin list received: ' + arr.length);
        renderModList();
    };

    window.__prismaUIAddItem_onQueryResult = function(jsonStr) {
        let data;
        try { data = JSON.parse(jsonStr); }
        catch (e) { debugBridge('bad query result: ' + e.message); console.error('bad query result', e); return; }

        state.items = data.items || [];
        state.totalMatched = data.totalMatched || 0;
        state.totalAll = data.totalAll || 0;
        state.selectedIndex = -1;

        debugBridge('query result received: ' + state.items.length + ' items, matched=' + state.totalMatched);

        renderItemList(true);
        updateItemsStatus();
        updatePagination();
        restoreActionButtonLabels();
        syncPreviewPane();
        hidePreview();
    };

    window.__prismaUIAddItem_onAddResult = function(jsonStr) {
        let data;
        try { data = JSON.parse(jsonStr); }
        catch (e) { debugBridge('bad add result: ' + e.message); console.error('bad add result', e); return; }

        const msg = data.message || (data.ok ? 'Done' : 'Action failed');
        const targetBtn = state.lastAction === 'use' ? els.useBtn : els.addBtn;
        state.addPending = false;
        state.usePending = false;
        els.itemsStatus.textContent = msg;
        els.itemsStatus.classList.toggle('ok', !!data.ok);
        els.itemsStatus.classList.toggle('error', !data.ok);
        setButtonMode(targetBtn, data.ok ? 'added' : 'failed', data.ok ? 'Done' : 'Failed');
        flashAddToast(msg, !!data.ok);
        if (data.ok) advanceAfterSuccessfulAction();
        resetAddFeedbackSoon();
        debugBridge('action result: ' + msg);
    };

    // ── Render: mod list ───────────────────────────────────────────────────
    window.__prismaUIAddItem_onHotkeyResult = function(jsonStr) {
        let data;
        try { data = JSON.parse(jsonStr); }
        catch (e) { debugBridge('bad hotkey result: ' + e.message); console.error('bad hotkey result', e); return; }

        state.hotkeyScancode = data.scancode || 0x3B;
    };

    function renderModList() {
        const needle = state.modSearch.toLowerCase().trim();
        const filtered = needle
            ? state.plugins.filter(p => p.name.toLowerCase().includes(needle))
            : state.plugins;

        const frag = document.createDocumentFragment();
        for (const p of filtered) {
            const li = document.createElement('li');
            li.dataset.name = p.name;

            const name  = document.createElement('span'); name.className  = 'mod-name';  name.textContent  = p.name;
            const count = document.createElement('span'); count.className = 'mod-count'; count.textContent = `${p.count} item${p.count === 1 ? '' : 's'}`;
            li.append(name, count);

            li.addEventListener('click', () => enterMod(p.name));
            frag.appendChild(li);
        }
        els.modList.replaceChildren(frag);
        els.modList.scrollTop = 0;

        els.modsStatus.textContent = needle
            ? `Showing ${filtered.length} of ${state.plugins.length} mods`
            : `${state.plugins.length} mods`;
    }

    // ── Render: items in mod ───────────────────────────────────────────────
    function renderItemList(resetScroll) {
        const frag = document.createDocumentFragment();
        for (let i = 0; i < state.items.length; ++i) {
            const it = state.items[i];
            const li = document.createElement('li');
            li.dataset.idx = i;

            const name   = document.createElement('span'); name.className   = 'name';   name.textContent   = it.name;
            const type   = document.createElement('span'); type.className   = 'type';   type.textContent   = typeLabel(it.type);
            const weight = document.createElement('span'); weight.className = 'weight'; weight.textContent = (it.weight || 0).toFixed(1);
            const value  = document.createElement('span'); value.className  = 'value';  value.textContent  = `${it.value || 0}g`;
            li.append(name, type, weight, value);

            li.addEventListener('click', () => selectItemIndex(i));
            li.addEventListener('dblclick', () => { selectItemIndex(i); doPrimaryAction(); });
            frag.appendChild(li);
        }
        els.itemList.replaceChildren(frag);
        if (resetScroll) els.itemList.scrollTop = 0;
        restoreActionButtonLabels();
    }

    function keepItemVisible(row) {
        if (!row) return;
        const listTop = els.itemList.scrollTop;
        const listBottom = listTop + els.itemList.clientHeight;
        const rowTop = row.offsetTop;
        const rowBottom = rowTop + row.offsetHeight;

        if (rowTop < listTop) {
            els.itemList.scrollTop = rowTop;
        } else if (rowBottom > listBottom) {
            els.itemList.scrollTop = rowBottom - els.itemList.clientHeight;
        }
    }

    function selectItemIndex(i, scrollIntoView) {
        const prev = els.itemList.querySelector('li.selected');
        if (prev) prev.classList.remove('selected');
        state.selectedIndex = i;
        const next = els.itemList.querySelector(`li[data-idx="${i}"]`);
        if (next) next.classList.add('selected');
        if (scrollIntoView) keepItemVisible(next);
        showPreviewNow(state.items[i]);
        restoreActionButtonLabels();
    }

    function updateItemsStatus() {
        els.itemsStatus.classList.remove('ok', 'error');
        const startIdx = state.totalMatched === 0 ? 0 : (state.page * state.pageSize) + 1;
        const endIdx   = Math.min(state.totalMatched, (state.page + 1) * state.pageSize);
        els.itemsStatus.textContent =
            `Showing ${startIdx}–${endIdx} of ${state.totalMatched}` +
            (state.totalMatched !== state.totalAll ? ` (${state.totalAll} in mod)` : '');
    }

    function updatePagination() {
        const totalPages = Math.max(1, Math.ceil(state.totalMatched / state.pageSize));
        els.prevPage.disabled = state.page <= 0;
        els.nextPage.disabled = state.page >= totalPages - 1;
        els.pageLabel.textContent = `page ${state.page + 1} / ${totalPages}`;
    }

    function advanceAfterSuccessfulAction() {
        const fallbackIndex = state.pendingItemIndex >= 0 ? state.pendingItemIndex : state.selectedIndex;
        const currentIndex = state.items.findIndex(it => itemKey(it) === state.pendingItemKey);
        const baseIndex = currentIndex >= 0 ? currentIndex : fallbackIndex;
        const nextIndex = Math.min(baseIndex + 1, state.items.length - 1);

        state.pendingItemKey = '';
        state.pendingItemIndex = -1;
        if (!state.qtyLocked) setQty(1);

        if (state.items.length > 0) {
            selectItemIndex(Math.max(0, nextIndex), true);
        } else {
            state.selectedIndex = -1;
            restoreActionButtonLabels();
        }

        updateItemsStatus();
    }

    function updateActionButtons() {
        const it = state.items[state.selectedIndex];
        const busy = state.addPending || state.usePending;
        const hasAction = hasDirectAction(it);
        els.addBtn.disabled = state.selectedIndex < 0 || busy;
        els.useBtn.classList.toggle('hidden', !hasAction);
        els.useBtn.disabled = state.selectedIndex < 0 || busy || !hasAction;
    }

    function restoreActionButtonLabels() {
        const it = state.items[state.selectedIndex];
        els.addBtn.classList.remove('adding', 'added', 'failed');
        els.useBtn.classList.remove('adding', 'added', 'failed');
        els.addBtn.textContent = 'Add to Inventory';
        els.useBtn.textContent = directActionLabel(it);
        updateActionButtons();
    }

    // ── Drill-down ─────────────────────────────────────────────────────────
    function enterMod(pluginName) {
        state.selectedPlugin = pluginName;
        state.search = '';
        state.type = '';
        state.page = 0;
        state.selectedIndex = -1;
        state.pendingItemKey = '';
        state.pendingItemIndex = -1;

        els.itemSearch.value = '';
        for (const t of els.tabs.querySelectorAll('.tab')) t.classList.remove('active');
        els.tabs.querySelector('.tab[data-type=""]').classList.add('active');
        els.itemsTitle.textContent = pluginName;

        showView('items');
        els.itemsStatus.textContent = 'Loading items…';
        syncPreviewPane();
        hidePreview();
        queryItems();
    }

    function backToMods() {
        hidePreview();
        showView('mods');
    }

    function queryItems() {
        callBridge('__prismaUIAddItem_query', {
            type:           state.type,
            search:         state.search,
            plugin:         state.selectedPlugin,
            excludeVanilla: false,    // already filtered at the mod-list level
            page:           state.page,
            pageSize:       state.pageSize,
        });
    }

    const queryItemsDebounced = debounce(queryItems, 150);

    const KEY_SCANCODES = {
        Escape: 0x01, Digit1: 0x02, Digit2: 0x03, Digit3: 0x04, Digit4: 0x05, Digit5: 0x06,
        Digit6: 0x07, Digit7: 0x08, Digit8: 0x09, Digit9: 0x0A, Digit0: 0x0B,
        Minus: 0x0C, Equal: 0x0D, Backspace: 0x0E, Tab: 0x0F,
        KeyQ: 0x10, KeyW: 0x11, KeyE: 0x12, KeyR: 0x13, KeyT: 0x14, KeyY: 0x15, KeyU: 0x16, KeyI: 0x17, KeyO: 0x18, KeyP: 0x19,
        BracketLeft: 0x1A, BracketRight: 0x1B, Enter: 0x1C, ControlLeft: 0x1D,
        KeyA: 0x1E, KeyS: 0x1F, KeyD: 0x20, KeyF: 0x21, KeyG: 0x22, KeyH: 0x23, KeyJ: 0x24, KeyK: 0x25, KeyL: 0x26,
        Semicolon: 0x27, Quote: 0x28, Backquote: 0x29, ShiftLeft: 0x2A, Backslash: 0x2B,
        KeyZ: 0x2C, KeyX: 0x2D, KeyC: 0x2E, KeyV: 0x2F, KeyB: 0x30, KeyN: 0x31, KeyM: 0x32,
        Comma: 0x33, Period: 0x34, Slash: 0x35, ShiftRight: 0x36, NumpadMultiply: 0x37, AltLeft: 0x38, Space: 0x39,
        CapsLock: 0x3A, F1: 0x3B, F2: 0x3C, F3: 0x3D, F4: 0x3E, F5: 0x3F, F6: 0x40, F7: 0x41, F8: 0x42, F9: 0x43, F10: 0x44,
        NumLock: 0x45, ScrollLock: 0x46, F11: 0x57, F12: 0x58, Insert: 0xD2, Delete: 0xD3, Home: 0xC7, End: 0xCF, PageUp: 0xC9, PageDown: 0xD1,
    };

    function scancodeToName(code) {
        for (const [key, value] of Object.entries(KEY_SCANCODES)) {
            if (value === code) return key.replace(/^Key/, '').replace(/^Digit/, '');
        }
        return '0x' + Number(code || 0).toString(16).toUpperCase();
    }

    function doAdd() {
        if (state.selectedIndex < 0) return;
        const it = state.items[state.selectedIndex];

        // HARD CLAMP — engine stores per-item count as int16 in ExtraCount for
        // most form types; values above 32767 wrap negative and silently
        // corrupt the container state. We snap the input visually so the user
        // sees the cap kick in (no silent truncation), then send the clamped
        // value. C++ side also clamps as defense-in-depth.
        let raw = parseInt(els.qtyInput.value, 10);
        if (!Number.isFinite(raw) || raw < 1) raw = 1;
        const count = Math.min(QTY_MAX, raw);
        if (count !== raw) setQty(count);  // snap-back so user sees the cap

        clearTimeout(addFeedbackTimer);
        state.lastAction = 'add';
        state.pendingItemKey = itemKey(it);
        state.pendingItemIndex = state.selectedIndex;
        state.addPending = true;
        els.itemsStatus.textContent = `Adding ${count} x ${it.name}...`;
        els.itemsStatus.classList.remove('ok', 'error');
        setButtonMode(els.addBtn, 'adding', 'Adding...');
        callBridge('__prismaUIAddItem_addItem', {
            plugin:  it.plugin,
            localId: it.localId,
            count:   count,
        });
    }

    function doPrimaryAction() {
        if (state.selectedIndex < 0) return;
        const it = state.items[state.selectedIndex];
        if (!hasDirectAction(it)) return;

        let raw = parseInt(els.qtyInput.value, 10);
        if (!Number.isFinite(raw) || raw < 1) raw = 1;
        const count = Math.min(QTY_MAX, raw);
        if (count !== raw) setQty(count);

        const label = directActionLabel(it);
        clearTimeout(addFeedbackTimer);
        state.lastAction = 'use';
        state.pendingItemKey = itemKey(it);
        state.pendingItemIndex = state.selectedIndex;
        state.usePending = true;
        els.itemsStatus.textContent = `${label} ${it.name}...`;
        els.itemsStatus.classList.remove('ok', 'error');
        setButtonMode(els.useBtn, 'adding', label + '...');
        callBridge('__prismaUIAddItem_useItem', {
            plugin:  it.plugin,
            localId: it.localId,
            count:   count,
        });
    }

    function doClose() {
        hidePreview();
        callBridge('__prismaUIAddItem_close', '');
    }

    // ── Event wiring ───────────────────────────────────────────────────────
    // Mods view
    els.modSearch.addEventListener('input', () => {
        state.modSearch = els.modSearch.value;
        renderModList();   // mod-list filter is client-side (small data, instant)
    });

    els.excludeToggle.addEventListener('change', () => {
        state.excludeVanilla = els.excludeToggle.checked;
        callBridge('__prismaUIAddItem_getPlugins', { excludeVanilla: state.excludeVanilla });
    });

    // Items view
    els.itemSearch.addEventListener('input', () => {
        state.search = els.itemSearch.value;
        state.page = 0;
        queryItemsDebounced();
    });

    els.tabs.addEventListener('click', (ev) => {
        const btn = ev.target.closest('.tab');
        if (!btn) return;
        for (const t of els.tabs.querySelectorAll('.tab')) t.classList.remove('active');
        btn.classList.add('active');
        state.type = btn.dataset.type;
        state.page = 0;
        queryItems();
    });

    // Engine-side cap: AddObjectToContainer takes int32 but the practical safe
    // limit is signed-16-bit (matches AIM-NG and Skyrim's own item-count fields).
    const QTY_MAX = 32767;

    els.qtyMinus.addEventListener('click', () => {
        setQty(Math.max(1, (parseInt(els.qtyInput.value, 10) || 1) - 1));
    });
    els.qtyPlus.addEventListener('click', () => {
        setQty(Math.min(QTY_MAX, (parseInt(els.qtyInput.value, 10) || 1) + 1));
    });

    // Quick-set qty presets (×1/×10/×100/×1000). One click to power-add.
    function setQty(v) {
        const clamped = Math.max(1, Math.min(QTY_MAX, v | 0));
        els.qtyInput.value = String(clamped);
        for (const p of document.querySelectorAll('.qty-presets .preset')) {
            p.classList.toggle('active', parseInt(p.dataset.qty, 10) === clamped);
        }
    }
    function setQtyLock(locked) {
        state.qtyLocked = !!locked;
        els.qtyLock.classList.toggle('active', state.qtyLocked);
        els.qtyLock.setAttribute('aria-pressed', state.qtyLocked ? 'true' : 'false');
        els.qtyLock.textContent = state.qtyLocked ? 'Qty Locked' : 'Lock Qty';
    }
    document.body.addEventListener('click', (ev) => {
        const preset = ev.target.closest('.qty-presets .preset');
        if (!preset) return;
        setQty(parseInt(preset.dataset.qty, 10) || 1);
    });
    els.qtyLock.addEventListener('click', () => {
        setQtyLock(!state.qtyLocked);
    });
    // If the user types into the input directly, drop preset highlighting.
    els.qtyInput.addEventListener('input', () => {
        for (const p of document.querySelectorAll('.qty-presets .preset')) p.classList.remove('active');
    });
    // Snap-clamp on blur so the field never displays an out-of-range value
    // (defense in depth — doAdd also clamps before sending).
    els.qtyInput.addEventListener('blur', () => {
        let raw = parseInt(els.qtyInput.value, 10);
        if (!Number.isFinite(raw) || raw < 1) raw = 1;
        const clamped = Math.min(QTY_MAX, raw);
        if (String(clamped) !== els.qtyInput.value) setQty(clamped);
    });

    els.useBtn.addEventListener('click', doPrimaryAction);
    els.addBtn.addEventListener('click', doAdd);

    els.prevPage.addEventListener('click', () => {
        if (state.page > 0) { state.page--; queryItems(); }
    });
    els.nextPage.addEventListener('click', () => {
        const totalPages = Math.max(1, Math.ceil(state.totalMatched / state.pageSize));
        if (state.page < totalPages - 1) { state.page++; queryItems(); }
    });

    // Both views: header buttons
    document.body.addEventListener('click', (ev) => {
        const btn = ev.target.closest('[data-action]');
        if (!btn) return;
        if (btn.dataset.action === 'close') doClose();
        if (btn.dataset.action === 'back')  backToMods();
    });

    // Keyboard shortcuts
    document.addEventListener('keydown', (ev) => {
        if (KEY_SCANCODES[ev.code] === state.hotkeyScancode) {
            doClose();
            ev.preventDefault();
            return;
        }
        if (ev.key === 'Escape') {
            if (state.view === 'items') backToMods();
            else doClose();
        }
        if (ev.key === 'Enter' && state.view === 'items' && state.selectedIndex >= 0) {
            doPrimaryAction();
        }
    });

    // First load — fetch the mod list. C++ will also fire onMenuOpen on each
    // subsequent menu invocation, so this is just for the very first display.
    window.addEventListener('load', () => {
        callBridge('__prismaUIAddItem_getPlugins', { excludeVanilla: false });
    });
})();
