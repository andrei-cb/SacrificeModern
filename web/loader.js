/* Game assets are imported locally; only the engine is fetched from the host. */
var Module = window.Module || {};
(() => {
    'use strict';
    const $ = id => document.getElementById(id);
    const status = text => { $('status').textContent = text; };
    let ready;
    const runtimeReady = new Promise(resolve => { ready = resolve; });
    let busy = false, running = false, manifest = null;
    // The data and maps folders may be picked separately; each waits here until the other arrives.
    const staged = {data: null, maps: null};
    Module.canvas = $('canvas');
    Module.noInitialRun = true;
    Module.onRuntimeInitialized = () => { document.body.dataset.core = 'ready'; ready(); };
    Module.showError = text => {
        $('error-message').textContent = String(text);
        $('error').hidden = false;
        document.body.dataset.state = 'error';
    };
    Module.onAbort = reason => Module.showError(reason);
    Module.setStatus = text => { if (text && !busy && !running) status(text); };
    Module.appReady = () => {
        $('loader').hidden = true;
        $('fullscreen').disabled = false;
        $('hint').textContent = 'WASD: wizard | Tab: spells | Middle-drag: orbit | F9: free camera | F10: inspector';
        document.body.dataset.state = 'ready';
        Module.canvas.focus();
    };
    const request = operation => new Promise((resolve, reject) => {
        operation.onsuccess = () => resolve(operation.result);
        operation.onerror = () => reject(operation.error);
    });
    const finished = transaction => new Promise((resolve, reject) => {
        transaction.oncomplete = resolve;
        transaction.onabort = transaction.onerror = () => reject(transaction.error || new Error('Cache transaction failed'));
    });
    const database = new Promise((resolve, reject) => {
        const open = indexedDB.open('SacrificeModern-game-data', 1);
        open.onupgradeneeded = () => {
            open.result.createObjectStore('files');
            open.result.createObjectStore('meta');
        };
        open.onsuccess = () => { open.result.onversionchange = () => open.result.close(); resolve(open.result); };
        open.onerror = () => reject(open.error);
        open.onblocked = () => status('Close other SacrificeModern tabs to open the asset cache.');
    });
    const lock = work => navigator.locks ? navigator.locks.request('SacrificeModern-game-data', work) : work();
    const put = async (db, store, key, value) => {
        const transaction = db.transaction(store, 'readwrite'), done = finished(transaction);
        transaction.objectStore(store).put(value, key);
        await done;
    };
    const read = (db, store, key) => request(db.transaction(store).objectStore(store).get(key));
    const cleanFiles = (db, keep) => new Promise((resolve, reject) => {
        const transaction = db.transaction('files', 'readwrite'), store = transaction.objectStore('files');
        // A key cursor cannot delete (IDBCursor.delete throws InvalidStateError); delete through the store.
        const cursor = store.openKeyCursor();
        cursor.onsuccess = () => {
            const item = cursor.result;
            if (!item) return;
            if (!keep || !String(item.key).startsWith(keep + '|')) store.delete(item.primaryKey);
            item.continue();
        };
        transaction.oncomplete = resolve;
        transaction.onabort = transaction.onerror = () => reject(transaction.error);
    });
    const updateButtons = () => {
        for (const id of ['folder', 'data-folder', 'maps-folder']) $(id).disabled = busy || running;
        $('data-pick').classList.toggle('done', !!staged.data);
        $('maps-pick').classList.toggle('done', !!staged.maps);
        $('start').disabled = busy || running || !manifest;
        $('forget').disabled = busy || running || !manifest;
    };
    const errorText = error => error?.name === 'QuotaExceededError'
        ? 'The browser has insufficient storage for the game files. Free browser storage and try again.'
        : (error?.message || String(error));
    const normalize = relative => {
        const parts = relative.replaceAll('\\', '/').split('/');
        if (!['data', 'maps'].includes(parts[0]?.toLowerCase())) parts.shift();
        if (!['data', 'maps'].includes(parts[0]?.toLowerCase()) || parts.length < 2) return null;
        if (parts.some(part => !part || part === '.' || part === '..')) throw new Error('Invalid file path in selected folder');
        return parts.join('/').toLowerCase();
    };
    async function importFiles(selection) {
        if (busy || running) return;
        busy = true; updateButtons();
        try {
            const files = [], paths = new Set();
            for (const item of selection) {
                const path = normalize(item.path);
                if (!path) continue;
                if (paths.has(path)) throw new Error('Duplicate file path: ' + path);
                paths.add(path); files.push({path, blob: item.blob});
            }
            if (!paths.has('data/main.wad') || !files.some(file => file.path.startsWith('maps/') && file.path.endsWith('.scp')))
                throw new Error('Select both the data folder (with main.wad) and the maps folder (with .scp maps).');
            if (navigator.storage?.persist) await navigator.storage.persist();
            const generation = crypto.randomUUID(), bytes = files.reduce((sum, file) => sum + file.blob.size, 0);
            $('progress').hidden = false; $('progress').max = bytes; $('progress').value = 0;
            await lock(async () => {
                const db = await database;
                const active = await read(db, 'meta', 'active');
                // Clean abandoned imports first, while preserving the last complete installation.
                await cleanFiles(db, active?.generation);
                let copied = 0;
                try {
                    for (const file of files) {
                        status('Caching ' + file.path);
                        await put(db, 'files', generation + '|' + file.path, file.blob);
                        $('progress').value = copied += file.blob.size;
                    }
                    const next = {generation, bytes, files: files.map(file => ({path: file.path, size: file.blob.size}))};
                    await put(db, 'meta', 'active', next);
                    manifest = next;
                } catch (error) { await cleanFiles(db, active?.generation); throw error; }
                await cleanFiles(db, generation);
            });
            status('Cached ' + files.length + ' files. Open cached files to start the preview.');
        } catch (error) { status(errorText(error)); }
        finally { busy = false; $('progress').hidden = true; updateButtons(); }
    }
    $('folder').addEventListener('change', event => {
        const selection = Array.from(event.target.files, blob => ({path: blob.webkitRelativePath, blob}));
        event.target.value = '';
        importFiles(selection);
    });
    // A picked folder keeps its own name as the first path part; it is renamed to the role it was picked for.
    const stage = (role, event) => {
        const files = Array.from(event.target.files, blob => {
            const parts = blob.webkitRelativePath.replaceAll('\\', '/').split('/');
            parts[0] = role;
            return {path: parts.join('/'), blob};
        });
        event.target.value = '';
        if (!files.length || busy || running) return;
        const valid = role === 'data'
            ? files.some(file => file.path.toLowerCase() === 'data/main.wad')
            : files.some(file => file.path.toLowerCase().endsWith('.scp'));
        if (!valid) {
            status(role === 'data' ? 'That folder has no main.wad. Select the data folder inside the Sacrifice installation.'
                                   : 'That folder has no .scp maps. Select the maps folder inside the Sacrifice installation.');
            return;
        }
        staged[role] = files;
        if (staged.data && staged.maps) {
            const selection = [...staged.data, ...staged.maps];
            staged.data = staged.maps = null;
            importFiles(selection);
        } else {
            status(role === 'data' ? 'Data folder selected. Now select the maps folder.' : 'Maps folder selected. Now select the data folder.');
            updateButtons();
        }
    };
    $('data-folder').addEventListener('change', event => stage('data', event));
    $('maps-folder').addEventListener('change', event => stage('maps', event));
    async function walk(entry, prefix, result) {
        const path = prefix + entry.name;
        if (entry.isFile) result.push({path, blob: await new Promise((resolve, reject) => entry.file(resolve, reject))});
        else if (entry.isDirectory) {
            const reader = entry.createReader();
            for (;;) {
                const batch = await new Promise((resolve, reject) => reader.readEntries(resolve, reject));
                if (!batch.length) break;
                for (const child of batch) await walk(child, path + '/', result);
            }
        }
    }
    $('drop').addEventListener('dragover', event => { event.preventDefault(); $('drop').classList.add('drag'); });
    $('drop').addEventListener('dragleave', () => $('drop').classList.remove('drag'));
    $('drop').addEventListener('drop', async event => {
        event.preventDefault(); $('drop').classList.remove('drag');
        if (busy || running) return;
        try {
            const entries = Array.from(event.dataTransfer.items, item => item.webkitGetAsEntry?.()).filter(Boolean);
            const selection = [];
            for (const entry of entries) await walk(entry, '', selection);
            await importFiles(selection);
        } catch (error) { status(errorText(error)); }
    });
    const sync = populate => new Promise((resolve, reject) => Module.FS.syncfs(populate, error => error ? reject(error) : resolve()));
    // Share the busy/again flags used by DocumentStore's write-triggered syncs.
    const flushPreferences = () => {
        const state = Module.saveSync || (Module.saveSync = {busy: false, again: false});
        state.again = true;
        if (state.busy) return;
        const flush = () => {
            state.busy = true; state.again = false;
            sync(false).catch(error => console.error('Preference sync failed', error)).finally(() => {
                state.busy = false;
                if (state.again) flush();
            });
        };
        flush();
    };
    $('start').addEventListener('click', async () => {
        if (busy || running) return;
        if (!window.isSecureContext || !navigator.gpu) {
            status('WebGPU is unavailable. Use a supported desktop browser over HTTPS or localhost.'); return;
        }
        busy = true; updateButtons();
        try {
            status('Preparing the game files…');
            await runtimeReady;
            const FS = Module.FS;
            FS.mkdirTree('/game/data/local'); FS.mkdirTree('/game/maps'); FS.mkdirTree('/persist');
            FS.mount(Module.IDBFS, {}, '/persist');
            await sync(true);
            await lock(async () => {
                const db = await database;
                manifest = await read(db, 'meta', 'active');
                if (!manifest) throw new Error('The cached files were removed. Select the installation again.');
                $('progress').hidden = false; $('progress').max = manifest.bytes; $('progress').value = 0;
                let loaded = 0;
                for (const file of manifest.files) {
                    const blob = await read(db, 'files', manifest.generation + '|' + file.path);
                    if (!blob || blob.size !== file.size) throw new Error('Incomplete cache: ' + file.path + '. Import the installation again.');
                    const destination = '/game/' + file.path;
                    FS.mkdirTree(destination.slice(0, destination.lastIndexOf('/')));
                    FS.writeFile(destination, new Uint8Array(await blob.arrayBuffer()), {canOwn: true});
                    $('progress').value = loaded += file.size;
                    status('Loading ' + file.path);
                }
            });
            const args = ['--game-dir', '/game'], query = new URLSearchParams(location.search);
            if (query.has('debug-trial')) args.push('--debug-trial');
            if (query.has('file')) args.push('--file', query.get('file'));
            if (query.has('map')) args.push('--map', query.get('map'));
            running = true;
            $('canvas').hidden = false;
            document.body.dataset.state = 'starting';
            status('Opening the game assets…');
            Module.callMain(args);
        } catch (error) { Module.showError(errorText(error)); }
        finally { busy = false; updateButtons(); }
    });
    $('forget').addEventListener('click', async () => {
        if (busy || running) return;
        busy = true; updateButtons();
        try {
            await lock(async () => {
                const db = await database, transaction = db.transaction(['files', 'meta'], 'readwrite');
                const done = finished(transaction);
                transaction.objectStore('files').clear(); transaction.objectStore('meta').clear();
                await done;
            });
            manifest = null; status('Game files removed from the cache. Saved preferences are retained.');
        } catch (error) { status(errorText(error)); }
        finally { busy = false; updateButtons(); }
    });
    $('fullscreen').addEventListener('click', () => $('viewport').requestFullscreen().catch(error => Module.showError(errorText(error))));
    $('canvas').addEventListener('contextmenu', event => event.preventDefault());
    window.addEventListener('keydown', event => {
        if (running && !['INPUT', 'TEXTAREA'].includes(event.target.tagName) &&
            ['F9', 'F10', 'Tab', ' ', 'ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight'].includes(event.key)) event.preventDefault();
    });
    document.addEventListener('visibilitychange', () => {
        if (running && document.hidden) flushPreferences();
    });
    database.then(async db => {
        manifest = await read(db, 'meta', 'active');
        status(manifest ? 'Cached installation found. Open cached files to continue.' : 'Select your Sacrifice installation to begin.');
        updateButtons();
    }).catch(error => Module.showError(errorText(error)));
})();
