#!/usr/bin/env python3
"""Prepares Firefox's APK for arc-wasm's Android 8.1 (topic firefox8).

    python3 -I tools/firefox8_apk.py ORIGINAL.apk build/extra-apks8/Firefox.apk

Copies the APK with a few edits of GeckoView's chrome JavaScript in
assets/omni.ja (each edit checks the text it replaces, so another Firefox
version fails loudly instead of being half-patched). The VM is one process:
GeckoView must load web content in the parent process (e10s off, see
docs/topics/firefox8.md), but its <browser> element is always remote.

The output is not signed (arc-wasm's PackageManagerService checks no
signatures). Neither APK goes into git.
"""

import io
import sys
import zipfile

# (file in omni.ja, text, replacement)
OMNI_EDITS = [
    # geckoview.js createBrowser(): remote only when e10s is on
    # (MOZ_FORCE_DISABLE_E10S=1 in the GeckoView config turns it off).
    ('chrome/geckoview/content/geckoview.js',
     '  browser.setAttribute("maychangeremoteness", "true");\n'
     '  browser.setAttribute("remote", "true");\n'
     '  browser.setAttribute(\n'
     '    "remoteType",\n',
     '  // arc-wasm: one process (tools/firefox8_apk.py): no process switches.\n'
     '  const arcwasmRemote = Services.appinfo.browserTabsRemoteAutostart;\n'
     '  arcwasmRemote && browser.setAttribute("maychangeremoteness", "true");\n'
     '  dump(`arc-wasm: geckoview <browser> remote=${arcwasmRemote}\\n`);\n'
     '  // (Its presence makes the browser remote, whatever the value.)\n'
     '  arcwasmRemote && browser.setAttribute("remote", "true");\n'
     '  arcwasmRemote && browser.setAttribute(\n'
     '    "remoteType",\n'),
    # After the <browser> is in the document: what Gecko made of it.
    ('chrome/geckoview/content/geckoview.js',
     '    window.document.documentElement.appendChild(browser);\n',
     '    window.document.documentElement.appendChild(browser);\n'
     '    dump(`arc-wasm: <browser> isRemoteBrowser=${browser.isRemoteBrowser} '
     'remoteType=${browser.remoteType}\\n`);\n'
     # The window's renderer (compositor), now. Android's nsWindow makes it
     # lazily, at the first paint -- but skips paints while the compositor is
     # paused, and GeckoSession resumes it (gives it the Surface) only after
     # that compositor says it is open: nobody starts. With e10s the remote
     # <browser>'s rendering setup asks for the renderer and breaks the
     # circle; in one process (no remote <browser>) nothing did, and the page
     # area stayed white (docs/topics/firefox8-blank.md).
     # windowUtils.layerManagerType is nsWindow::GetWindowRenderer().
     '    try {\n'
     '      dump(`arc-wasm: window renderer ${window.windowUtils.layerManagerType}\\n`);\n'
     '    } catch (e) { dump(`arc-wasm: window renderer: ${e}\\n`); }\n'
     # Graphics state 15 s later (pref arcwasm.debug.gfx): renderer, gfx
     # feature decisions (why WebRender is or is not used).
     '    Services.prefs.getBoolPref("arcwasm.debug.gfx", false) && [15000, 45000, 90000].forEach(t => setTimeout(() => {\n'
     '      try {\n'
     '        const gi = Cc["@mozilla.org/gfx/info;1"].getService(Ci.nsIGfxInfo);\n'
     '        const b = window.browser;\n'
     '        dump(`arc-wasm: paint active=${b.docShellIsActive} renderLayers=${b.renderLayers} '
     'hasLayers=${b.hasLayers} hidden=${document.hidden} size=${window.innerWidth}x${window.innerHeight} '
     'url=${b.currentURI?.spec} title=${b.contentTitle} '
     'state=${b.contentDocument?.readyState}\\n`);\n'
     '        dump(`arc-wasm: gfx renderer=${window.windowUtils.layerManagerType} '
     'remote=${window.windowUtils.layerManagerRemote} '
     'features=${JSON.stringify(gi.getFeatures())} '
     'log=${JSON.stringify(gi.getFeatureLog())}\\n`);\n'
     '      } catch (e) { dump(`arc-wasm: gfx: ${e}\\n`); }\n'
     '    }, t));\n'),
]


def patch_omni(data):
    src = zipfile.ZipFile(io.BytesIO(data))
    out = io.BytesIO()
    pending = {name for name, _, _ in OMNI_EDITS}
    with zipfile.ZipFile(out, 'w') as dst:
        for info in src.infolist():
            body = src.read(info)
            for name, old, new in OMNI_EDITS:
                if info.filename != name:
                    continue
                text = body.decode('utf-8')
                if text.count(old) != 1:
                    sys.exit('firefox8_apk: %s: the text to replace is not there once '
                             '(another Firefox version?):\n%s' % (name, old))
                body = text.replace(old, new).encode('utf-8')
                pending.discard(name)
            dst.writestr(info, body, compress_type=info.compress_type)
    if pending:
        sys.exit('firefox8_apk: not in omni.ja: %s' % ', '.join(sorted(pending)))
    return out.getvalue()


def main(argv):
    if len(argv) != 3:
        sys.exit(__doc__)
    src = zipfile.ZipFile(argv[1])
    with zipfile.ZipFile(argv[2], 'w') as dst:
        for info in src.infolist():
            body = src.read(info)
            if info.filename == 'assets/omni.ja':
                body = patch_omni(body)
            dst.writestr(info, body, compress_type=info.compress_type)
    print('%s: %d edits of omni.ja' % (argv[2], len(OMNI_EDITS)))


if __name__ == '__main__':
    main(sys.argv)
