// Web half of the priority-tiered asset loader.
//
// Fetches the background packages built by web/Makefile's `packages`
// target and unpacks them into MEMFS, so the existing synchronous
// loaders (fopen/stb_image/splat_load_spz) keep working untouched — the
// files simply appear later than they used to.
//
// Each package is a file_packager --separate-metadata pair:
//   assets/assets-<name>.data           concatenated payload
//   assets/assets-<name>.js.metadata    {"files":[{filename,start,end}],…}
// We parse the manifest and write the slices ourselves rather than
// running the generated .js, which would register Emscripten run
// dependencies — those are meant for startup, and firing them after the
// runtime is up can re-trigger the module's dependency callbacks.
//
// Downloads run one at a time in priority order (asset_loader's
// assets_next_download) so the tier a game actually needs lands first.

#include "../asset_loader.h"

#include <emscripten.h>

#include <cstdio>

namespace {

// Group whose bytes have arrived but whose GL upload hasn't run yet.
// main_web.cpp drains this each frame, does the upload on the main
// thread, and only then marks the group ASSET_READY — so "ready" always
// means "actually usable", not merely "downloaded".
AssetGroup g_installed_queue[ASSET_GROUP_COUNT];
int g_installed_count = 0;

const char* package_name(AssetGroup g) {
    switch (g) {
        case ASSET_GAME:             return "game";
        case ASSET_RETRO:            return "retro";
        case ASSET_SPLAT_MEDIEVAL:   return "splat-medieval";
        case ASSET_SPLAT_DATACENTER: return "splat-datacenter";
        case ASSET_PUZZLES:          return "puzzles";
        default:                     return nullptr;   // CORE is in chess.data
    }
}

}  // namespace

// Streams one package into MEMFS. Progress is reported per chunk so the
// loading bar moves on slow connections; any failure resolves the group
// as failed rather than leaving it in flight forever.
EM_JS(void, js_fetch_asset_package, (int group, const char* name_ptr), {
    var name = UTF8ToString(name_ptr);
    var bust = (typeof window !== 'undefined' && window.__BUILD_HASH)
        ? ('?v=' + window.__BUILD_HASH) : '';
    var base = 'assets/assets-' + name;
    (async function () {
        try {
            var metaResp = await fetch(base + '.js.metadata' + bust);
            if (!metaResp.ok) throw new Error('metadata HTTP ' + metaResp.status);
            var meta = await metaResp.json();
            var total = meta.remote_package_size || 0;

            var dataResp = await fetch(base + '.data' + bust);
            if (!dataResp.ok) throw new Error('data HTTP ' + dataResp.status);

            var buf;
            if (dataResp.body && dataResp.body.getReader && total > 0) {
                // The manifest tells us the exact payload size, so
                // allocate once and copy each chunk straight into place.
                // Accumulating chunks and concatenating afterwards would
                // hold the package twice over and end with a single
                // 20-30 MB memcpy — a visible hitch on its own.
                buf = new Uint8Array(total);
                var reader = dataResp.body.getReader();
                var got = 0;
                for (;;) {
                    var r = await reader.read();
                    if (r.done) break;
                    if (got + r.value.length <= total) {
                        buf.set(r.value, got);
                    } else {
                        // Server disagreed with the manifest — fall back
                        // to growing rather than truncating the package.
                        var bigger = new Uint8Array(got + r.value.length);
                        bigger.set(buf.subarray(0, got), 0);
                        bigger.set(r.value, got);
                        buf = bigger;
                    }
                    got += r.value.length;
                    Module.ccall('on_asset_progress', null,
                                 ['number', 'number', 'number'],
                                 [group, got, total]);
                }
                if (got < buf.length) buf = buf.subarray(0, got);
            } else {
                // No streaming body (older Safari) or unknown length:
                // one shot, no progress.
                buf = new Uint8Array(await dataResp.arrayBuffer());
            }

            // Write into MEMFS in slices, yielding to the event loop
            // between them. Unpacking a whole package in one go blocks
            // the frame — the puzzle package alone is 1723 FS.writeFile
            // calls, which is a visible hitch in the menu animation.
            var mkdirp = function (dir) {
                var parts = dir.split('/'), cur = '';
                for (var pi = 0; pi < parts.length; pi++) {
                    if (!parts[pi].length) continue;
                    cur += '/' + parts[pi];
                    // FS.mkdirTree isn't guaranteed to be exported, and
                    // re-creating an existing directory throws.
                    try { FS.mkdir(cur); } catch (e) {}
                }
            };
            var fi = 0;
            var BATCH_FILES = 32;         // caps per-tick work for many small files
            var BATCH_BYTES = 1 << 20;    // ...and for a few large ones
            // Pace with requestAnimationFrame rather than setTimeout(0):
            // a zero timeout floods the task queue between frames and
            // competes with the render loop, which is exactly the stutter
            // we're trying to avoid. One batch per frame is plenty (a
            // 22 MB package unpacks in well under a second).
            var nextTick = (typeof requestAnimationFrame === 'function')
                ? requestAnimationFrame
                : function (fn) { setTimeout(fn, 16); };
            var writeSome = function () {
                var bytes = 0, n = 0;
                while (fi < meta.files.length && n < BATCH_FILES &&
                       bytes < BATCH_BYTES) {
                    var f = meta.files[fi++];
                    var slash = f.filename.lastIndexOf('/');
                    if (slash > 0) mkdirp(f.filename.substring(0, slash));
                    FS.writeFile(f.filename, buf.subarray(f.start, f.end));
                    bytes += (f.end - f.start);
                    n++;
                }
                if (fi < meta.files.length) {
                    nextTick(writeSome);
                } else {
                    Module.ccall('on_asset_done', null, ['number', 'number'],
                                 [group, 1]);
                }
            };
            nextTick(writeSome);
        } catch (e) {
            if (typeof console !== 'undefined')
                console.warn('[assets] ' + name + ' failed:', e);
            Module.ccall('on_asset_done', null, ['number', 'number'],
                         [group, 0]);
        }
    })();
});

extern "C" {

EMSCRIPTEN_KEEPALIVE
void on_asset_progress(int group, double received, double total) {
    if (group < 0 || group >= ASSET_GROUP_COUNT) return;
    assets_set_progress(static_cast<AssetGroup>(group), received, total);
}

EMSCRIPTEN_KEEPALIVE
void on_asset_done(int group, int ok) {
    if (group < 0 || group >= ASSET_GROUP_COUNT) return;
    AssetGroup g = static_cast<AssetGroup>(group);
    if (!ok) {
        std::fprintf(stderr, "[assets] %s failed to download\n",
                     assets_group_label(g));
        assets_set_state(g, ASSET_FAILED);
        return;
    }
    // Bytes are in MEMFS; the GL-side install still has to happen on the
    // main thread. Queue it and let the frame loop finish the job.
    if (g_installed_count < ASSET_GROUP_COUNT)
        g_installed_queue[g_installed_count++] = g;
}

}  // extern "C"

// Kick off the next pending package, one at a time so the priority
// order is actually honoured by the network.
void assets_web_pump_downloads(int environment) {
    for (int i = 0; i < ASSET_GROUP_COUNT; i++)
        if (assets_state(static_cast<AssetGroup>(i)) == ASSET_LOADING)
            return;   // one in flight already

    AssetGroup next = assets_next_download(environment);
    if (next == ASSET_GROUP_COUNT) return;
    const char* name = package_name(next);
    if (!name) { assets_set_state(next, ASSET_READY); return; }

    assets_set_state(next, ASSET_LOADING);
    assets_set_progress(next, 0, 0);
    js_fetch_asset_package(static_cast<int>(next), name);
}

// Pop a downloaded-but-not-yet-installed group, or return false.
bool assets_web_take_installed(AssetGroup* out) {
    if (g_installed_count <= 0) return false;
    *out = g_installed_queue[0];
    for (int i = 1; i < g_installed_count; i++)
        g_installed_queue[i - 1] = g_installed_queue[i];
    g_installed_count--;
    return true;
}
