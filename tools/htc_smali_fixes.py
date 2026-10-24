#!/usr/bin/env python3
"""Fixes in HTC's code for paths a device never takes (topic userspace).

HTC Sense runs here as on a device without what its phones always had (GPU
rendering, mostly), and some of its code paths for that case were never
right. Each fix below names the class and method, what goes wrong and what
it changes; it applies to the smali of an app or library before assembly
(tools/fetch_system_apks_htc.sh) and fails loudly when the code it expects
is not there.

Usage: htc_smali_fixes.py DIR...   (smali trees; fixed in place)
"""
import os
import sys

FIXES = [
    {
        # Prism, software rendering only (isHardwareAccelerated() false):
        # Workspace.onPageEndMoving clears every page's drawing cache and
        # casts each child to CellLayout, BlinkFeed's page too
        # (ClassCastException, launcher closed). enableChildrenCache checks
        # with instance-of first; this does the same.
        'class': 'Lcom/htc/launcher/Workspace;',
        'method': '.method clearChildrenCache()V',
        'old': '''    move-result-object v1

    check-cast v1, Lcom/htc/launcher/CellLayout;''',
        'new': '''    move-result-object v1

    instance-of v3, v1, Lcom/htc/launcher/CellLayout;

    if-eqz v3, :cond_1a

    check-cast v1, Lcom/htc/launcher/CellLayout;''',
    },
    {
        # SystemUI's Recents, Back: it logs the second recent task before
        # checking there is one (IndexOutOfBoundsException, SystemUI and so
        # every app gone). A phone always has two (Recents' own and home's);
        # here Recents can share home's task. The log now waits for the
        # check the method makes right after it.
        'class': 'Lcom/android/systemui/recent/RecentAppFxActivity;',
        'method': '.method private dismissAndGoBack()V',
        'old': '''    const-string v3, "RecentApps"

    new-instance v2, Ljava/lang/StringBuilder;''',
        'new': '''    const-string v3, "RecentApps"

    invoke-interface {v0}, Ljava/util/List;->size()I

    move-result v2

    if-le v2, v5, :cond_4b

    new-instance v2, Ljava/lang/StringBuilder;''',
    },
    {
        # Prism, BlinkFeed: the launcher's loader waits on the first launch
        # until BlinkFeed's first sync completes, at most 50 s. Here there
        # is no feed provider (news, social apps), the sync never completes,
        # and the workspace and the app list came only after the 50 s (the
        # app list showed its spinner that long). It waits 1 s now.
        'class': 'Lcom/htc/launcher/feeds/FeedHostManager;',
        'method': '.method onFeedCustomization()V',
        'old': '''    const-wide/32 v3, 0xc350

    invoke-virtual {v1, v3, v4}, Ljava/lang/Object;->wait(J)V''',
        'new': '''    const-wide/16 v3, 0x3e8

    invoke-virtual {v1, v3, v4}, Ljava/lang/Object;->wait(J)V''',
    },
]


def main(dirs):
    pending = {f['class']: [] for f in FIXES}
    for f in FIXES:
        pending[f['class']].append(f)
    done = 0
    for root in dirs:
        for d, _, files in os.walk(root):
            for name in files:
                if not name.endswith('.smali'):
                    continue
                path = os.path.join(d, name)
                text = open(path).read()
                cls = text.split('\n', 1)[0].split()[-1]
                for fix in pending.get(cls, []):
                    start = text.index(fix['method'])
                    end = text.index('.end method', start)
                    body = text[start:end]
                    if body.count(fix['old']) != 1:
                        sys.exit('%s %s: the code to fix is not there' % (cls, fix['method']))
                    text = text[:start] + body.replace(fix['old'], fix['new']) + text[end:]
                    done += 1
                    print('fixed %s %s' % (cls, fix['method'].split()[-1]))
                if cls in pending and pending[cls]:
                    open(path, 'w').write(text)
    if done != len(FIXES):
        sys.exit('%d of %d fixes applied' % (done, len(FIXES)))


if __name__ == '__main__':
    main(sys.argv[1:])
