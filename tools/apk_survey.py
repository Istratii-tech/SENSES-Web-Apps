#!/usr/bin/env python3
"""What an APK needs from the runtime: a static survey for the Android 8.1
port (docs/android8-plan.md, topic apps8-survey; docs/topics/apps8-survey.md).

For every APK (or split set: .apks/.xapk/.apkm, or a directory holding
base.apk and its splits) it reports
  * the manifest: package, minSdk/targetSdk, launcher activity, Application,
    permissions, components, content providers, features, libraries;
  * the dex files: count, versions (035/037/038/039), default and static
    interface methods, invoke-polymorphic/invoke-custom, call sites and
    method handles, use of java.time, java.util.stream/function,
    java.lang.invoke (and of their desugared j$ copies);
  * framework and libcore APIs above level 23 the code refers to, by
    api-versions.xml of the SDK (references to a platform class/method/field,
    receivers resolved through the app's own class hierarchy); references to
    classes neither in the APK nor in the SDK (hidden APIs, org.apache.http);
  * system services (manager classes, Context service names), AIDL interface
    descriptors, Google Play services/Firebase/Play Core/Integrity/SafetyNet,
    WebView, media (ExoPlayer, MediaCodec...), OpenGL ES version, Vulkan,
    engines and SDKs (Unity, Flutter, React Native...);
  * native libraries by ABI (is there 32-bit ARM, or only arm64-v8a?), their
    system imports by the API level of Android 8.1's NDK
    (tests/ndk/android81-ndk-symbols.txt, tools/gen_ndk81_symbols.py), and
    with --ndk-check, what the NDK bridge cannot resolve
    (tools/ndk_check_all_imports.py on the 32-bit libraries; needs
    build/ndk/ndk_unit_test.js: make build/ndk/ndk_unit_test.js).

  python3 -I tools/apk_survey.py APK|DIR... [--ndk-check] [--work DIR]
      [--md REPORT.md] [--json REPORT.json] [--api-versions api-versions.xml]

Only the standard library; APKs are only read (native libraries are copied to
--work for the bridge check, a temporary directory by default).
"""
import argparse
import collections
import io
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
API_VERSIONS = os.path.expanduser(
    '~/Library/Android/sdk/platforms/android-33/data/api-versions.xml')
NDK81_SYMBOLS = os.path.join(ROOT, 'tests', 'ndk', 'android81-ndk-symbols.txt')
NDK60_SYMBOLS = os.path.join(ROOT, 'tests', 'ndk', 'android-system-symbols.txt')
NDK_CHECK = os.path.join(ROOT, 'tools', 'ndk_check_all_imports.py')
NDK_UNIT = os.path.join(ROOT, 'build', 'ndk', 'ndk_unit_test.js')

OUR_API = 23          # the newest stack in main (6.0.1)
TARGET_API = 27       # the port (8.1)

# System libraries the NDK bridge loads (shim/ndk/ndk_core.cc IsHostLibrary).
BRIDGE_LIBS = {'libc.so', 'libm.so', 'libdl.so', 'liblog.so', 'libstdc++.so', 'libz.so',
               'libGLESv1_CM.so', 'libGLESv2.so', 'libGLESv3.so', 'libEGL.so', 'libandroid.so',
               'libjnigraphics.so', 'libOpenSLES.so', 'libOpenMAXAL.so'}

# Services the lean system_server of 6.0.1 registers
# (framework/java-arcwasm6/arcwasm/server/SystemServer.java).
LEAN60_SERVICES = {'display', 'power', 'user', 'appops', 'package', 'mount', 'accessibility',
                   'input_method', 'window', 'alarm', 'content', 'vibrator', 'connectivity',
                   'location', 'audio', 'notification', 'clipboard', 'textservices',
                   'usagestats', 'jobscheduler', 'deviceidle', 'activity', 'input'}

# Context service name -> (manager class, API level, in-process: needs no system service)
SERVICES = {
    'window': ('android/view/WindowManager', 1, False),
    'layout_inflater': ('android/view/LayoutInflater', 1, True),
    'activity': ('android/app/ActivityManager', 1, False),
    'power': ('android/os/PowerManager', 1, False),
    'alarm': ('android/app/AlarmManager', 1, False),
    'notification': ('android/app/NotificationManager', 1, False),
    'keyguard': ('android/app/KeyguardManager', 1, False),
    'location': ('android/location/LocationManager', 1, False),
    'search': ('android/app/SearchManager', 1, False),
    'sensor': ('android/hardware/SensorManager', 1, False),
    'storage': ('android/os/storage/StorageManager', 9, False),
    'vibrator': ('android/os/Vibrator', 1, False),
    'connectivity': ('android/net/ConnectivityManager', 1, False),
    'wifi': ('android/net/wifi/WifiManager', 1, False),
    'wifip2p': ('android/net/wifi/p2p/WifiP2pManager', 14, False),
    'servicediscovery': ('android/net/nsd/NsdManager', 16, False),
    'audio': ('android/media/AudioManager', 1, False),
    'media_router': ('android/media/MediaRouter', 16, False),
    'media_session': ('android/media/session/MediaSessionManager', 21, False),
    'media_projection': ('android/media/projection/MediaProjectionManager', 21, False),
    'phone': ('android/telephony/TelephonyManager', 1, False),
    'telephony_subscription_service': ('android/telephony/SubscriptionManager', 22, False),
    'carrier_config': ('android/telephony/CarrierConfigManager', 23, False),
    'telecom': ('android/telecom/TelecomManager', 21, False),
    'clipboard': ('android/content/ClipboardManager', 1, False),
    'input_method': ('android/view/inputmethod/InputMethodManager', 3, False),
    'textservices': ('android/view/textservice/TextServicesManager', 14, False),
    'accessibility': ('android/view/accessibility/AccessibilityManager', 4, False),
    'captioning': ('android/view/accessibility/CaptioningManager', 19, False),
    'account': ('android/accounts/AccountManager', 5, False),
    'device_policy': ('android/app/admin/DevicePolicyManager', 8, False),
    'uimode': ('android/app/UiModeManager', 8, False),
    'download': ('android/app/DownloadManager', 9, True),
    'dropbox': ('android/os/DropBoxManager', 8, False),
    'wallpaper': ('android/app/WallpaperManager', 1, False),
    'usb': ('android/hardware/usb/UsbManager', 12, False),
    'nfc': ('android/nfc/NfcManager', 10, False),
    'bluetooth': ('android/bluetooth/BluetoothManager', 18, False),
    'display': ('android/hardware/display/DisplayManager', 17, False),
    'user': ('android/os/UserManager', 17, False),
    'input': ('android/hardware/input/InputManager', 16, False),
    'appops': ('android/app/AppOpsManager', 19, False),
    'consumer_ir': ('android/hardware/ConsumerIrManager', 19, False),
    'print': ('android/print/PrintManager', 19, False),
    'camera': ('android/hardware/camera2/CameraManager', 21, False),
    'launcherapps': ('android/content/pm/LauncherApps', 21, False),
    'restrictions': ('android/content/RestrictionsManager', 21, False),
    'jobscheduler': ('android/app/job/JobScheduler', 21, False),
    'tv_input': ('android/media/tv/TvInputManager', 21, False),
    'batterymanager': ('android/os/BatteryManager', 21, False),
    'appwidget': ('android/appwidget/AppWidgetManager', 3, False),
    'usagestats': ('android/app/usage/UsageStatsManager', 22, False),
    'fingerprint': ('android/hardware/fingerprint/FingerprintManager', 23, False),
    'midi': ('android/media/midi/MidiManager', 23, False),
    'netstats': ('android/app/usage/NetworkStatsManager', 23, False),
    'systemhealth': ('android/os/health/SystemHealthManager', 24, False),
    'hardware_properties': ('android/os/HardwarePropertiesManager', 24, False),
    'shortcut': ('android/content/pm/ShortcutManager', 25, False),
    'storagestats': ('android/app/usage/StorageStatsManager', 26, False),
    'companiondevice': ('android/companion/CompanionDeviceManager', 26, False),
    'autofill': ('android/view/autofill/AutofillManager', 26, False),
    'textclassification': ('android/view/textclassifier/TextClassificationManager', 26, False),
    'wifiaware': ('android/net/wifi/aware/WifiAwareManager', 26, False),
    'crossprofileapps': ('android/content/pm/crossprofile/CrossProfileApps', 28, False),
    'euicc': ('android/telephony/euicc/EuiccManager', 28, False),
    'slice': ('android/app/slice/SliceManager', 28, False),
    'biometric': ('android/hardware/biometrics/BiometricManager', 29, False),
    'role': ('android/app/role/RoleManager', 29, False),
}
# Service names too common as plain words to count without the manager class.
AMBIGUOUS_NAMES = {'window', 'activity', 'power', 'alarm', 'notification', 'location', 'search',
                   'sensor', 'storage', 'audio', 'phone', 'account', 'download', 'display', 'user',
                   'input', 'print', 'camera', 'restrictions', 'usb', 'nfc', 'bluetooth', 'wifi',
                   'connectivity', 'clipboard', 'accessibility', 'keyguard', 'vibrator',
                   'wallpaper', 'shortcut', 'autofill', 'role', 'slice', 'midi', 'euicc',
                   'telecom', 'captioning', 'appwidget', 'biometric', 'fingerprint', 'uimode'}

# (label, class prefixes, native library names) -- engines, SDKs, libraries.
SDKS = [
    ('Unity', ['Lcom/unity3d/player/'], ['libunity.so', 'libil2cpp.so', 'libmono.so']),
    ('Unreal Engine', ['Lcom/epicgames/'], ['libUE4.so', 'libUnreal.so']),
    ('Flutter', ['Lio/flutter/'], ['libflutter.so']),
    ('React Native', ['Lcom/facebook/react/'], ['libreactnativejni.so', 'libhermes.so',
                                                 'libjsc.so']),
    ('Xamarin/.NET', ['Lmono/', 'Lcrc64'], ['libmonodroid.so', 'libmonosgen-2.0.so']),
    ('Cocos2d-x', ['Lorg/cocos2dx/'], ['libcocos2dcpp.so', 'libcocos2djs.so', 'libcocos.so']),
    ('Godot', ['Lorg/godotengine/'], ['libgodot_android.so']),
    ('libGDX', ['Lcom/badlogic/gdx/'], ['libgdx.so']),
    ('Cordova/Capacitor (WebView app)', ['Lorg/apache/cordova/', 'Lcom/getcapacitor/'], []),
    ('Kotlin', ['Lkotlin/'], []),
    ('Kotlin coroutines', ['Lkotlinx/coroutines/'], []),
    ('Jetpack Compose', ['Landroidx/compose/'], []),
    ('AndroidX', ['Landroidx/'], []),
    ('Support library', ['Landroid/support/'], []),
    ('core library desugaring (j$)', ['Lj$/'], []),
    ('Google Play services', ['Lcom/google/android/gms/'], []),
    ('Firebase', ['Lcom/google/firebase/'], []),
    ('Play Core', ['Lcom/google/android/play/core/'], []),
    ('Play Integrity', ['Lcom/google/android/play/core/integrity/',
                        'Lcom/google/android/play/integrity/'], []),
    ('Play Billing', ['Lcom/android/billingclient/', 'Lcom/android/vending/billing/'], []),
    ('Install Referrer', ['Lcom/android/installreferrer/'], []),
    ('Huawei HMS', ['Lcom/huawei/hms/'], []),
    ('ExoPlayer', ['Lcom/google/android/exoplayer2/', 'Lcom/google/android/exoplayer/'], []),
    ('Media3', ['Landroidx/media3/'], []),
    ('IjkPlayer', ['Ltv/danmaku/ijk/'], ['libijkplayer.so']),
    ('VLC', ['Lorg/videolan/'], ['libvlc.so']),
    ('CameraX', ['Landroidx/camera/'], []),
    ('WorkManager', ['Landroidx/work/'], []),
    ('Room', ['Landroidx/room/'], []),
    ('OkHttp', ['Lokhttp3/', 'Lcom/squareup/okhttp/'], []),
    ('Retrofit', ['Lretrofit2/'], []),
    ('Cronet', ['Lorg/chromium/net/'], ['libcronet']),
    ('gRPC', ['Lio/grpc/'], []),
    ('RxJava', ['Lio/reactivex/', 'Lrx/'], []),
    ('Glide', ['Lcom/bumptech/glide/'], []),
    ('Fresco', ['Lcom/facebook/imagepipeline/'], ['libimagepipeline.so']),
    ('Lottie', ['Lcom/airbnb/lottie/'], []),
    ('Realm', ['Lio/realm/'], ['librealm-jni.so']),
    ('SQLCipher', ['Lnet/sqlcipher/', 'Lnet/zetetic/'], ['libsqlcipher.so']),
    ('Facebook SDK', ['Lcom/facebook/login/', 'Lcom/facebook/share/', 'Lcom/facebook/appevents/'],
     []),
    ('AppsFlyer', ['Lcom/appsflyer/'], []),
    ('Adjust', ['Lcom/adjust/sdk/'], []),
    ('AdMob', ['Lcom/google/android/gms/ads/'], []),
    ('AppLovin', ['Lcom/applovin/'], []),
    ('ironSource', ['Lcom/ironsource/'], []),
    ('Unity Ads', ['Lcom/unity3d/ads/', 'Lcom/unity3d/services/'], []),
    ('Mintegral', ['Lcom/mbridge/'], []),
    ('Pangle', ['Lcom/bytedance/sdk/openadsdk/'], []),
    ('Yandex Mobile Ads', ['Lcom/yandex/mobile/ads/'], []),
    ('myTarget', ['Lcom/my/target/'], []),
    ('VK SDK', ['Lcom/vk/'], []),
    ('Sentry', ['Lio/sentry/'], []),
]

# Native libraries of packers/protectors (they decrypt dex at run time).
PACKERS = {'libjiagu.so': '360 Jiagu', 'libjiagu_x86.so': '360 Jiagu', 'libsecexe.so': 'Bangcle',
           'libDexHelper.so': 'SecNeo', 'libshella': 'Tencent Legu', 'libshellx': 'Tencent Legu',
           'libnqshield.so': 'NQ Shield', 'libprotectClass.so': 'Ijiami', 'libexec.so': 'Ijiami',
           'libapssec.so': 'Alibaba', 'libmobisec.so': 'Alibaba', 'libbaiduprotect.so': 'Baidu',
           'libAppSealing': 'AppSealing', 'libdexprotector': 'DexProtector',
           'libpairipcore.so': 'Google Play (PairIP) app integrity'}

# Packages whose references are counted separately (libcore of OpenJDK 8).
JAVA8_PACKAGES = [('java.time', 'Ljava/time/'), ('java.util.stream', 'Ljava/util/stream/'),
                  ('java.util.function', 'Ljava/util/function/'),
                  ('java.lang.invoke', 'Ljava/lang/invoke/'),
                  ('j$ (desugared java.*)', 'Lj$/')]
ACC_STATIC, ACC_ABSTRACT, ACC_INTERFACE = 0x8, 0x400, 0x200


# ---------------------------------------------------------------------------------------------
# Binary XML (AndroidManifest.xml)

ATTR_IDS = {0x01010003: 'name', 0x0101020c: 'minSdkVersion', 0x01010270: 'targetSdkVersion',
            0x01010281: 'glEsVersion', 0x01010018: 'authorities', 0x0101028e: 'required',
            0x01010010: 'exported', 0x0101021b: 'versionCode', 0x0101021c: 'versionName',
            0x01010024: 'value', 0x01010202: 'targetActivity'}


def string_pool(data, off):
    count, _, flags, start = struct.unpack_from('<IIII', data, off + 8)
    utf8 = flags & 0x100
    offsets = struct.unpack_from('<%dI' % count, data, off + 28)
    out = []
    for o in offsets:
        p = off + start + o
        if utf8:
            n = data[p]
            p += 2 if n & 0x80 else 1
            n = data[p]
            if n & 0x80:
                n = ((n & 0x7f) << 8) | data[p + 1]
                p += 1
            p += 1
            out.append(data[p:p + n].decode('utf-8', 'replace'))
        else:
            n = struct.unpack_from('<H', data, p)[0]
            if n & 0x8000:
                n = ((n & 0x7fff) << 16) | struct.unpack_from('<H', data, p + 2)[0]
                p += 2
            p += 2
            out.append(data[p:p + 2 * n].decode('utf-16-le', 'replace'))
    return out


def parse_axml(data):
    """A binary XML document as nested {'tag', 'attrs', 'children'}."""
    root = {'tag': None, 'attrs': {}, 'children': []}
    stack = [root]
    strings, resmap = [], []
    pos = struct.unpack_from('<H', data, 2)[0]
    while pos + 8 <= len(data):
        ctype, hsize, csize = struct.unpack_from('<HHI', data, pos)
        if csize < 8:
            break
        if ctype == 0x0001:
            strings = string_pool(data, pos)
        elif ctype == 0x0180:
            resmap = struct.unpack_from('<%dI' % ((csize - hsize) // 4), data, pos + hsize)
        elif ctype == 0x0102:
            ext = pos + hsize
            _, name, astart, asize, acount = struct.unpack_from('<IIHHH', data, ext)
            attrs = {}
            for i in range(acount):
                a = ext + astart + i * asize
                _, aname, raw, _, _, dtype, val = struct.unpack_from('<IIIHBBI', data, a)
                key = strings[aname] if aname < len(strings) else ''
                if aname < len(resmap) and resmap[aname] in ATTR_IDS:
                    key = ATTR_IDS[resmap[aname]]
                if dtype == 0x03:
                    v = strings[val] if val < len(strings) else ''
                elif dtype in (0x10, 0x11):
                    v = val - (1 << 32) if val & 0x80000000 and dtype == 0x10 else val
                elif dtype == 0x12:
                    v = val != 0
                elif dtype == 0x01:
                    v = '@0x%08x' % val
                else:
                    v = strings[raw] if raw != 0xffffffff and raw < len(strings) else val
                attrs[key] = v
            el = {'tag': strings[name], 'attrs': attrs, 'children': []}
            stack[-1]['children'].append(el)
            stack.append(el)
        elif ctype == 0x0103:
            if len(stack) > 1:
                stack.pop()
        pos += csize
    return root['children'][0] if root['children'] else root


def walk(el, tag=None):
    for c in el['children']:
        if tag is None or c['tag'] == tag:
            yield c


def full_name(pkg, name):
    if not isinstance(name, str) or not name:
        return name
    if name.startswith('.'):
        return pkg + name
    if '.' not in name:
        return pkg + '.' + name
    return name


def read_manifest(data):
    m = parse_axml(data) if data else {'tag': 'manifest', 'attrs': {}, 'children': []}
    a = m['attrs']
    pkg = a.get('package', '')
    info = {'package': pkg, 'versionName': a.get('versionName'),
            'versionCode': a.get('versionCode'), 'split': a.get('split'),
            'minSdk': 1, 'targetSdk': None, 'permissions': [], 'features': [],
            'libraries': [], 'glEs': None, 'application': None, 'launcher': None,
            'activities': [], 'services': [], 'receivers': [], 'providers': [],
            'metadata': {}, 'app_attrs': {}}
    for us in walk(m, 'uses-sdk'):
        for k, dst in (('minSdkVersion', 'minSdk'), ('targetSdkVersion', 'targetSdk')):
            if k in us['attrs']:
                v = us['attrs'][k]
                info[dst] = v if isinstance(v, int) else str(v)
    if info['targetSdk'] is None:
        info['targetSdk'] = info['minSdk']
    for tag in ('uses-permission', 'uses-permission-sdk-23', 'uses-permission-sdk-m'):
        for p in walk(m, tag):
            info['permissions'].append(p['attrs'].get('name'))
    for f in walk(m, 'uses-feature'):
        fa = f['attrs']
        if 'glEsVersion' in fa:
            v = fa['glEsVersion']
            if isinstance(v, int):
                gl = '%d.%d' % (v >> 16, v & 0xffff)
                info['glEs'] = max(info['glEs'] or '0', gl)
        if fa.get('name'):
            info['features'].append((fa['name'], fa.get('required', True)))
    app = next(walk(m, 'application'), None)
    if app:
        info['application'] = full_name(pkg, app['attrs'].get('name'))
        for k in ('extractNativeLibs', 'usesCleartextTraffic', 'largeHeap', 'hardwareAccelerated',
                  'appComponentFactory', 'networkSecurityConfig', 'debuggable', 'process'):
            if k in app['attrs']:
                info['app_attrs'][k] = app['attrs'][k]
        for ul in walk(app, 'uses-library'):
            info['libraries'].append((ul['attrs'].get('name'), ul['attrs'].get('required', True)))
        for md in walk(app, 'meta-data'):
            info['metadata'][md['attrs'].get('name')] = md['attrs'].get(
                'value', md['attrs'].get('resource'))
        for c in app['children']:
            kind = {'activity': 'activities', 'activity-alias': 'activities',
                    'service': 'services', 'receiver': 'receivers',
                    'provider': 'providers'}.get(c['tag'])
            if not kind:
                continue
            ca = c['attrs']
            name = full_name(pkg, ca.get('name'))
            actions, cats = set(), set()
            for f in walk(c, 'intent-filter'):
                actions |= {x['attrs'].get('name') for x in walk(f, 'action')}
                cats |= {x['attrs'].get('name') for x in walk(f, 'category')}
            entry = {'name': name, 'exported': ca.get('exported'), 'actions': sorted(
                x for x in actions if x)}
            if c['tag'] == 'provider':
                entry['authorities'] = ca.get('authorities')
            if c['tag'] == 'activity-alias':
                entry['target'] = full_name(pkg, ca.get('targetActivity'))
            info[kind].append(entry)
            if kind == 'activities' and 'android.intent.action.MAIN' in actions and \
                    'android.intent.category.LAUNCHER' in cats and not info['launcher']:
                info['launcher'] = entry.get('target') or name
    return info


# ---------------------------------------------------------------------------------------------
# dex

def uleb(data, p):
    result = shift = 0
    while True:
        b = data[p]
        p += 1
        result |= (b & 0x7f) << shift
        if b < 0x80:
            return result, p
        shift += 7


# Code units per opcode (formats of the Dalvik bytecode, dex 039).
def _widths():
    w = [1] * 256
    for op, n in ((0x02, 2), (0x03, 3), (0x05, 2), (0x06, 3), (0x08, 2), (0x09, 3), (0x13, 2),
                  (0x14, 3), (0x15, 2), (0x16, 2), (0x17, 3), (0x18, 5), (0x19, 2), (0x1a, 2),
                  (0x1b, 3), (0x1c, 2), (0x1f, 2), (0x20, 2), (0x22, 2), (0x23, 2), (0x24, 3),
                  (0x25, 3), (0x26, 3), (0x29, 2), (0x2a, 3), (0x2b, 3), (0x2c, 3), (0xfa, 4),
                  (0xfb, 4), (0xfc, 3), (0xfd, 3), (0xfe, 2), (0xff, 2)):
        w[op] = n
    for lo, hi, n in ((0x2d, 0x31, 2), (0x32, 0x3d, 2), (0x44, 0x6d, 2), (0x6e, 0x72, 3),
                      (0x74, 0x78, 3), (0x90, 0xaf, 2), (0xd0, 0xe2, 2)):
        for op in range(lo, hi + 1):
            w[op] = n
    return w


WIDTHS = _widths()


def scan_code(data, off, counts):
    """Counts invoke-polymorphic/-custom and const-method-handle/-type in one code_item."""
    insns_size = struct.unpack_from('<I', data, off + 12)[0]
    p, end = off + 16, off + 16 + insns_size * 2
    while p < end:
        unit = data[p] | (data[p + 1] << 8)
        op = unit & 0xff
        if op == 0 and unit:
            if unit == 0x0100:
                n = struct.unpack_from('<H', data, p + 2)[0]
                p += (n * 2 + 4) * 2
            elif unit == 0x0200:
                n = struct.unpack_from('<H', data, p + 2)[0]
                p += (n * 4 + 2) * 2
            elif unit == 0x0300:
                width, n = struct.unpack_from('<HI', data, p + 2)
                p += ((n * width + 1) // 2 + 4) * 2
            else:
                p += 2
            continue
        if op in (0xfa, 0xfb):
            counts['invoke-polymorphic'] += 1
        elif op in (0xfc, 0xfd):
            counts['invoke-custom'] += 1
        elif op in (0xfe, 0xff):
            counts['const-method-handle/type'] += 1
        p += WIDTHS[op] * 2


class Dex:
    def __init__(self, name, data):
        self.name = name
        self.version = data[4:7].decode('ascii', 'replace')
        (nstr, ostr, ntype, otype, nproto, oproto, nfield, ofield, nmeth, ometh, ncls,
         ocls) = struct.unpack_from('<12I', data, 56)
        map_off = struct.unpack_from('<I', data, 52)[0]
        strings = []
        for o in struct.unpack_from('<%dI' % nstr, data, ostr):
            _, p = uleb(data, o)
            strings.append(data[p:data.index(b'\0', p)].decode('utf-8', 'replace'))
        self.strings = strings
        types = [strings[i] for i in struct.unpack_from('<%dI' % ntype, data, otype)]
        self.types = types

        def type_list(off):
            if not off:
                return []
            n = struct.unpack_from('<I', data, off)[0]
            return [types[i] for i in struct.unpack_from('<%dH' % n, data, off + 4)]

        protos = []
        for i in range(nproto):
            _, ret, params = struct.unpack_from('<III', data, oproto + 12 * i)
            protos.append('(%s)%s' % (''.join(type_list(params)), types[ret]))
        self.methods = []   # (class, name, proto)
        for i in range(nmeth):
            c, pr, n = struct.unpack_from('<HHI', data, ometh + 8 * i)
            self.methods.append((types[c], strings[n], protos[pr]))
        self.fields = []
        for i in range(nfield):
            c, _, n = struct.unpack_from('<HHI', data, ofield + 8 * i)
            self.fields.append((types[c], strings[n]))
        self.map = {}
        n = struct.unpack_from('<I', data, map_off)[0]
        for i in range(n):
            t, _, size, _ = struct.unpack_from('<HHII', data, map_off + 4 + 12 * i)
            self.map[t] = size
        self.classes = {}   # descriptor -> (superclass, interfaces, flags)
        self.default_methods = self.static_iface_methods = 0
        self.ops = collections.Counter()
        scan = self.version >= '038'
        for i in range(ncls):
            cidx, flags, sup, ifaces, _, _, cdata, _ = struct.unpack_from('<8I', data,
                                                                          ocls + 32 * i)
            desc = types[cidx]
            self.classes[desc] = (types[sup] if sup != 0xffffffff else None, type_list(ifaces),
                                  flags)
            if not cdata or not (scan or flags & ACC_INTERFACE):
                continue
            p = cdata
            sf, p = uleb(data, p)
            inf, p = uleb(data, p)
            dm, p = uleb(data, p)
            vm, p = uleb(data, p)
            for _ in range(sf + inf):
                _, p = uleb(data, p)
                _, p = uleb(data, p)
            midx = 0
            for k in range(dm + vm):
                if k == dm:
                    midx = 0
                d, p = uleb(data, p)
                midx += d
                acc, p = uleb(data, p)
                code, p = uleb(data, p)
                if flags & ACC_INTERFACE and code:
                    if k >= dm:
                        self.default_methods += 1
                    elif acc & ACC_STATIC and self.methods[midx][1] != '<clinit>':
                        self.static_iface_methods += 1
                if scan and code:
                    scan_code(data, code, self.ops)


# ---------------------------------------------------------------------------------------------
# api-versions.xml

class Api:
    def __init__(self, path):
        self.classes = {}
        self.method_names = set()
        for _, el in ET.iterparse(path):
            if el.tag != 'class':
                continue
            since = int(float(el.get('since', '1')))
            supers, methods, fields = [], {}, {}
            for ch in el:
                if ch.tag in ('extends', 'implements'):
                    supers.append(ch.get('name'))
                elif ch.tag == 'method':
                    methods[ch.get('name')] = int(float(ch.get('since', since)))
                elif ch.tag == 'field':
                    fields[ch.get('name')] = int(float(ch.get('since', since)))
            self.classes['L%s;' % el.get('name')] = (since, ['L%s;' % s for s in supers], methods,
                                                      fields)
            self.method_names.update(m.split('(')[0] for m in methods)
            el.clear()
        self.memo = {}

    def member(self, cls, key, kind):
        """The level a method ('name(args)ret') or field is in CLS or its
        supertypes at, or None."""
        k = (cls, key, kind)
        if k in self.memo:
            return self.memo[k]
        best, seen, todo = None, set(), [cls]
        while todo:
            c = todo.pop()
            if c in seen or c not in self.classes:
                continue
            seen.add(c)
            since, supers, methods, fields = self.classes[c]
            v = (methods if kind == 'm' else fields).get(key)
            if v is not None:
                v = max(v, since)
                best = v if best is None else min(best, v)
            todo.extend(supers)
        self.memo[k] = best
        return best


# ---------------------------------------------------------------------------------------------
# ELF

def elf_info(data):
    """{'machine', 'bits', 'needed', 'soname', 'undefined': {name: weak}, 'defined': set} or None."""
    if data[:4] != b'\x7fELF':
        return None
    bits = 64 if data[4] == 2 else 32
    if bits == 32:
        (_, machine, _, _, phoff, shoff, _, _, phentsize, phnum, shentsize, shnum,
         _) = struct.unpack_from('<HHIIIIIHHHHHH', data, 16)
    else:
        (_, machine, _, _, phoff, shoff, _, _, phentsize, phnum, shentsize, shnum,
         _) = struct.unpack_from('<HHIQQQIHHHHHH', data, 16)
    info = {'machine': {40: 'arm', 183: 'arm64', 3: 'x86', 62: 'x86_64', 8: 'mips'}.get(
        machine, str(machine)), 'bits': bits, 'needed': [], 'soname': None,
        'undefined': {}, 'defined': set()}
    loads, dyn = [], None
    for i in range(phnum):
        o = phoff + i * phentsize
        if bits == 32:
            ptype, off, vaddr, _, filesz, _, _, _ = struct.unpack_from('<8I', data, o)
        else:
            ptype, _, off, vaddr, _, filesz, _, _ = struct.unpack_from('<IIQQQQQQ', data, o)
        if ptype == 1:
            loads.append((vaddr, off, filesz))
        elif ptype == 2:
            dyn = (off, filesz)

    def v2o(v):
        for vaddr, off, size in loads:
            if vaddr <= v < vaddr + size:
                return v - vaddr + off
        return None

    tags = collections.defaultdict(list)
    if dyn:
        fmt, step = ('<iI', 8) if bits == 32 else ('<qQ', 16)
        for o in range(dyn[0], dyn[0] + dyn[1], step):
            tag, val = struct.unpack_from(fmt, data, o)
            if tag == 0:
                break
            tags[tag].append(val)
    strtab = v2o(tags[5][0]) if tags.get(5) else None
    symtab = v2o(tags[6][0]) if tags.get(6) else None
    nsyms = None
    # .dynsym's size from the section headers, else from DT_HASH / DT_GNU_HASH.
    for i in range(shnum if shoff and shoff < len(data) else 0):
        o = shoff + i * shentsize
        if bits == 32:
            _, stype, _, _, off, size, _, _, _, ent = struct.unpack_from('<10I', data, o)
        else:
            _, stype, _, _, off, size, _, _, _, ent = struct.unpack_from('<IIQQQQIIQQ', data, o)
        if stype == 11 and ent:
            nsyms = size // ent
    if nsyms is None and tags.get(4):
        h = v2o(tags[4][0])
        if h is not None:
            nsyms = struct.unpack_from('<I', data, h + 4)[0]
    if nsyms is None and tags.get(0x6ffffef5):
        h = v2o(tags[0x6ffffef5][0])
        if h is not None:
            nbuckets, symoff, bloom, _ = struct.unpack_from('<4I', data, h)
            b = h + 16 + bloom * (bits // 8)
            buckets = struct.unpack_from('<%dI' % nbuckets, data, b)
            last = max(buckets) if buckets else 0
            if last >= symoff:
                c = b + 4 * nbuckets + 4 * (last - symoff)
                while not struct.unpack_from('<I', data, c)[0] & 1:
                    c += 4
                    last += 1
            nsyms = last + 1
    if strtab is None:
        return info

    def cstr(o):
        return data[strtab + o:data.index(b'\0', strtab + o)].decode('latin-1')

    info['needed'] = [cstr(v) for v in tags.get(1, [])]
    info['soname'] = cstr(tags[14][0]) if tags.get(14) else None
    if symtab is not None and nsyms:
        fmt, step = ('<IIIBBH', 16) if bits == 32 else ('<IBBHQQ', 24)
        for i in range(1, nsyms):
            o = symtab + i * step
            if o + step > len(data):
                break
            if bits == 32:
                name, _, _, sinfo, other, shndx = struct.unpack_from(fmt, data, o)
            else:
                name, sinfo, other, shndx, _, _ = struct.unpack_from(fmt, data, o)
            bind = sinfo >> 4
            if not name or bind not in (1, 2):
                continue
            n = cstr(name)
            if shndx == 0:
                info['undefined'][n] = bind == 2
            elif other & 3 == 0:
                info['defined'].add(n)
    return info


def _ror(v, n):
    return ((v >> n) | (v << (32 - n))) & 0xffffffff if n else v


def _thumb_expand_imm(imm12):
    if imm12 >> 10 == 0:
        v, kind = imm12 & 0xff, (imm12 >> 8) & 3
        return [v, v | v << 16, v << 8 | v << 24, v * 0x01010101][kind]
    return _ror(0x80 | (imm12 & 0x7f), imm12 >> 7)


def thumb_r7(seg, p):
    """The value r7 holds at a Thumb SVC at P: from the instructions in the 16
    bytes before it; '?' if r7 is copied from a register, None if nothing sets it."""
    hw = {k: struct.unpack_from('<H', seg, p - 2 * k)[0] for k in range(1, 9)}
    value, ops = None, []
    for k in range(8, 0, -1):          # oldest first
        h = hw[k]
        h2 = hw.get(k - 1) if k > 1 else None
        if h2 is not None and h2 & 0x8f00 == 0x0700:
            imm16 = ((h & 0xf) << 12) | ((h >> 10 & 1) << 11) | ((h2 >> 12 & 7) << 8) | \
                (h2 & 0xff)
            imm12 = ((h >> 10 & 1) << 11) | ((h2 >> 12 & 7) << 8) | (h2 & 0xff)
            if h & 0xfbf0 == 0xf240:                    # movw r7, #imm16
                value, ops = imm16, []
                continue
            if h & 0xfbf0 == 0xf2c0:                    # movt r7, #imm16
                ops.append(('t', imm16))
                continue
            if h & 0xfbef == 0xf04f:                    # mov.w r7, #const
                value, ops = _thumb_expand_imm(imm12), []
                continue
            if h & 0xfbef == 0xf107:                    # add.w r7, r7, #const
                ops.append(('+', _thumb_expand_imm(imm12)))
                continue
        prev = hw.get(k + 1, 0)
        if prev >= 0xe800:      # the second half of a 32-bit instruction
            continue
        if h & 0xff00 == 0x2700:                        # movs r7, #imm8
            value, ops = h & 0xff, []
        elif h & 0xff87 == 0x4607:                      # mov r7, rN
            value, ops = '?', []
    if value is None or value == '?':
        return value
    for op, v in ops:
        value = (value & 0xffff) | v << 16 if op == 't' else (value + v) & 0xffffffff
    return value


def direct_syscalls(data):
    """Raw system calls in the executable segments of a 32-bit ARM library: SVC #0
    (ARM 0xef000000, Thumb 0xdf00) with the number written to r7 just before
    (mov/movw/movt, movs). Without mapping symbols ARM and Thumb are told apart by
    the pattern itself. {number: count}; '?' = the number comes from a register
    (a syscall() wrapper)."""
    out = collections.Counter()
    if data[:4] != b'\x7fELF' or data[4] != 1:
        return out
    _, _, _, _, phoff, _, _, _, phentsize, phnum, _, _, _ = struct.unpack_from(
        '<HHIIIIIHHHHHH', data, 16)
    for i in range(phnum):
        ptype, off, _, _, filesz, _, flags, _ = struct.unpack_from('<8I', data,
                                                                   phoff + i * phentsize)
        if ptype != 1 or not flags & 1:
            continue
        seg = data[off:off + filesz]
        # ARM: svc #0 (unconditional), r7 set by one of the 4 instructions before.
        p = seg.find(b'\x00\x00\x00\xef')
        while p != -1:
            if p % 4 == 0 and p >= 16:
                num = '?'
                lo = hi = None
                for k in range(1, 5):
                    w = struct.unpack_from('<I', seg, p - 4 * k)[0]
                    if w & 0xfffff000 == 0xe3a07000:          # mov r7, #imm
                        lo = _ror(w & 0xff, 2 * ((w >> 8) & 0xf))
                        break
                    if w & 0xfff0f000 == 0xe3007000:          # movw r7, #imm16
                        lo = ((w >> 4) & 0xf000) | (w & 0xfff)
                        break
                    if w & 0xfff0f000 == 0xe3407000:          # movt r7, #imm16
                        hi = ((w >> 4) & 0xf000) | (w & 0xfff)
                if lo is not None:
                    num = lo | ((hi or 0) << 16)
                    out[num] += 1
                elif any(struct.unpack_from('<I', seg, p - 4 * k)[0] & 0x0ffff000 in
                         (0x01a07000,) for k in range(1, 5)):  # mov r7, rN
                    out['?'] += 1
            p = seg.find(b'\x00\x00\x00\xef', p + 1)
        # Thumb: svc #0 with r7 set within the 16 bytes before it (movs, movw/movt,
        # mov.w and add.w r7 with a Thumb-2 modified immediate).
        p = seg.find(b'\x00\xdf')
        while p != -1:
            if p % 2 == 0 and p >= 16:
                num = thumb_r7(seg, p)
                if num is not None:
                    out[num] += 1
            p = seg.find(b'\x00\xdf', p + 1)
    return out


# ---------------------------------------------------------------------------------------------

class Apk:
    """An app: its APK or the base and split APKs of a bundle."""

    def __init__(self, path, work):
        self.path = path
        self.size = 0
        self.parts = []   # (name, ZipFile)
        if os.path.isdir(path):
            files = [os.path.join(path, f) for f in sorted(os.listdir(path)) if f.endswith('.apk')]
        else:
            files = [path]
        for f in files:
            self.size += os.path.getsize(f)
            z = zipfile.ZipFile(f)
            inner = [n for n in z.namelist() if n.endswith('.apk')]
            if 'AndroidManifest.xml' not in z.namelist() and inner:
                d = tempfile.mkdtemp(prefix='bundle-', dir=work)
                for n in inner:
                    out = os.path.join(d, os.path.basename(n))
                    with z.open(n) as src, open(out, 'wb') as dst:
                        shutil.copyfileobj(src, dst)
                    self.parts.append((os.path.basename(n), zipfile.ZipFile(out)))
            else:
                self.parts.append((os.path.basename(f), z))

    def entries(self):
        for pname, z in self.parts:
            for info in z.infolist():
                yield pname, z, info


def survey(path, api, ndk81, ndk60, args):
    t0 = time.time()
    apk = Apk(path, args.work)
    r = {'file': os.path.basename(path.rstrip('/')), 'size': apk.size,
         'parts': [p for p, _ in apk.parts]}
    # --- manifest: the part without a split name
    for pname, z in apk.parts:
        if 'AndroidManifest.xml' in z.namelist():
            m = read_manifest(z.read('AndroidManifest.xml'))
            if not m['split'] or 'manifest' not in r:
                r['manifest'] = m
    if 'manifest' not in r:   # a bare dex in a zip (tests)
        r['manifest'] = read_manifest(b'')
    man = r['manifest']

    # --- dex
    dexes, app_classes = [], {}
    dex_entries = [(p, z, i) for p, z, i in apk.entries()
                   if re.match(r'(classes\d*\.dex|.*\.dex)$', i.filename)]
    for pname, z, i in dex_entries:
        data = z.read(i)
        if data[:4] != b'dex\n':
            continue
        d = Dex('%s:%s' % (pname, i.filename), data)
        d.size = len(data)
        del data
        app_classes.update(d.classes)
        dexes.append(d)
    other_dex = [i.filename for p, z, i in dex_entries
                 if not re.match(r'classes\d*\.dex$', i.filename)]
    r['dex'] = {
        'count': len(dexes), 'size': sum(d.size for d in dexes),
        'versions': dict(collections.Counter(d.version for d in dexes)),
        'other_dex_files': other_dex,
        'classes': len(app_classes),
        'default_methods': sum(d.default_methods for d in dexes),
        'static_interface_methods': sum(d.static_iface_methods for d in dexes),
        'call_sites': sum(d.map.get(0x0007, 0) for d in dexes),
        'method_handles': sum(d.map.get(0x0008, 0) for d in dexes),
        'ops': dict(sum((d.ops for d in dexes), collections.Counter())),
    }

    # --- references to the platform
    strings = set()
    type_refs, meth_refs, field_refs = set(), set(), set()
    for d in dexes:
        strings.update(d.strings)
        type_refs.update(d.types)
        meth_refs.update(d.methods)
        field_refs.update(d.fields)
        d.strings = d.methods = d.fields = d.types = None

    def base(t):
        return t.lstrip('[')

    platform_supers = {}

    def platform_bases(c):
        """The platform types an app class extends or implements (nearest ones)."""
        if c in platform_supers:
            return platform_supers[c]
        platform_supers[c] = []
        out, todo, seen = [], [c], set()
        while todo:
            x = todo.pop()
            if x in seen or x is None:
                continue
            seen.add(x)
            if x in app_classes:
                sup, ifaces, _ = app_classes[x]
                todo.extend([sup] + ifaces)
            elif x in api.classes:
                out.append(x)
        platform_supers[c] = out
        return out

    new_api = collections.defaultdict(lambda: collections.Counter())  # level -> {ref: 1}
    outside = collections.Counter()   # class neither in the APK nor in the SDK -> refs
    java8 = collections.Counter()

    new_api_classes = collections.defaultdict(set)   # level -> old classes with new members
    new_classes = collections.defaultdict(set)       # level -> classes new in that level

    def note(level, cls, member=None):
        if level and level > OUR_API:
            c = cls[1:-1].replace('/', '.')
            new_api[level][c + ('.' + member if member else '')] += 1
            (new_classes if api.classes[cls][0] > OUR_API else new_api_classes)[level].add(c)

    for t in type_refs:
        b = base(t)
        if not b.startswith('L'):
            continue
        for label, prefix in JAVA8_PACKAGES:
            if b.startswith(prefix) and b not in app_classes:
                java8[label] += 1
        if b in app_classes:
            continue
        if b in api.classes:
            note(api.classes[b][0], b)
        else:
            outside[b] += 0
    for kind, refs in (('m', meth_refs), ('f', field_refs)):
        for ref in refs:
            cls, name = base(ref[0]), ref[1]
            key = name + ref[2] if kind == 'm' else name
            if not cls.startswith('L'):
                if kind == 'm' and ref[0].startswith('['):  # array clone() etc.
                    continue
                continue
            if cls in app_classes:
                if kind == 'm' and name not in api.method_names:
                    continue
                found = [(api.member(b, key, kind), b) for b in platform_bases(cls)]
                found = [x for x in found if x[0] is not None]
                if found:
                    lv, owner = min(found)
                    note(lv, owner, name if kind == 'f' else name + '()')
                continue
            if cls not in api.classes:
                outside[cls] += 1
                continue
            lv = api.member(cls, key, kind)
            if lv is not None:
                note(max(lv, api.classes[cls][0]), cls, name if kind == 'f' else name + '()')
            elif cls.startswith(('Landroid/', 'Lcom/android/')):
                outside['%s->%s' % (cls, name)] += 1   # hidden member of a public class
    r['api'] = {str(lv): sorted(new_api[lv]) for lv in sorted(new_api)}
    r['api_classes'] = {str(lv): sorted(new_api_classes[lv]) for lv in sorted(new_api)}
    r['api_new_classes'] = {str(lv): sorted(new_classes[lv]) for lv in sorted(new_api)}
    r['java8'] = dict(java8)
    hidden = collections.Counter()
    for k, n in outside.items():
        cls = k.split('->')[0]
        if cls.startswith(('Landroid/support/', 'Landroid/arch/', 'Ldalvik/annotation/')):
            continue
        if cls.startswith(('Ljava/', 'Ljavax/', 'Landroid/', 'Ldalvik/', 'Lcom/android/',
                           'Llibcore/', 'Lsun/', 'Lorg/apache/http/', 'Lorg/json/',
                           'Lorg/xmlpull/', 'Lorg/w3c/', 'Lorg/xml/', 'Ljunit/',
                           'Lorg/apache/harmony/')):
            hidden[k[1:].replace(';', '')] += max(n, 1)
    r['outside_sdk'] = [k for k, _ in hidden.most_common()]

    # --- services, AIDL, SDKs
    services = {}
    for name, (mgr, level, local) in SERVICES.items():
        by_class = ('L%s;' % mgr) in type_refs
        by_name = name in strings and name not in AMBIGUOUS_NAMES
        if by_class or by_name:
            services[name] = {'level': level, 'manager': by_class, 'name': name in strings,
                              'local': local, 'lean60': name in LEAN60_SERVICES}
    r['services'] = services
    r['aidl'] = sorted(s for s in strings if re.match(
        r'^(android|com\.android|com\.google|com\.huawei)\.[\w.$]*\.I[A-Z][a-z]\w*$', s) and
        ('L%s;' % s.replace('.', '/')) not in app_classes)
    r['aidl_own'] = sum(1 for s in strings if re.match(r'^[\w.$]+\.I[A-Z][a-z]\w*$', s) and
                        ('L%s;' % s.replace('.', '/')) in app_classes)

    natives = [i.filename for p, z, i in apk.entries() if i.filename.endswith('.so')]
    lib_names = {os.path.basename(n) for n in natives}
    sdks = {}
    prefix_count = collections.Counter()
    for c in app_classes:
        for label, prefixes, _ in SDKS:
            if c.startswith(tuple(prefixes)):
                prefix_count[label] += 1
    for label, prefixes, libs in SDKS:
        hits = [l for l in libs if any(n.startswith(l) for n in lib_names)]
        if prefix_count[label] or hits:
            sdks[label] = {'classes': prefix_count[label], 'libs': hits}
    r['sdks'] = sdks
    r['packers'] = sorted({v for k, v in PACKERS.items() if any(n.startswith(k)
                                                                for n in lib_names)})
    gms = {
        'availability_check': any(c.endswith(('GoogleApiAvailability;',
                                              'GoogleApiAvailabilityLight;',
                                              'GooglePlayServicesUtil;',
                                              'GooglePlayServicesUtilLight;')) and
                                  n.startswith('isGooglePlayServicesAvailable')
                                  for c, n, _ in meth_refs),
        'gms_package_string': 'com.google.android.gms' in strings,
        'vending_string': 'com.android.vending' in strings,
        'metadata': man['metadata'].get('com.google.android.gms.version'),
        'safetynet': any(c.startswith('Lcom/google/android/gms/safetynet/') for c in type_refs),
        'integrity': any(c.startswith(('Lcom/google/android/play/core/integrity/',
                                       'Lcom/google/android/play/integrity/'))
                         for c in type_refs),
        'fcm': any(s['name'].endswith('FirebaseMessagingService') or
                   'com.google.firebase.MESSAGING_EVENT' in s['actions']
                   for s in man['services']),
        'gms_modules': sorted(collections.Counter(
            c.split('/')[4] for c in app_classes
            if c.startswith('Lcom/google/android/gms/') and c.count('/') > 4)),
        'firebase_modules': sorted(collections.Counter(
            c.split('/')[3] for c in app_classes
            if c.startswith('Lcom/google/firebase/') and c.count('/') > 3)),
    }
    r['gms'] = gms
    def refs(prefix):
        return sum(1 for c in type_refs if c.startswith(prefix))

    r['media'] = {k: refs(v) for k, v in (
        ('MediaCodec', 'Landroid/media/MediaCodec'), ('MediaExtractor', 'Landroid/media/MediaExtractor'),
        ('MediaPlayer', 'Landroid/media/MediaPlayer'), ('AudioTrack', 'Landroid/media/AudioTrack'),
        ('AudioRecord', 'Landroid/media/AudioRecord'), ('SoundPool', 'Landroid/media/SoundPool'),
        ('MediaMetadataRetriever', 'Landroid/media/MediaMetadataRetriever'),
        ('MediaRecorder', 'Landroid/media/MediaRecorder'), ('MediaMuxer', 'Landroid/media/MediaMuxer'),
        ('Camera (old)', 'Landroid/hardware/Camera'), ('camera2', 'Landroid/hardware/camera2/'),
        ('ExoPlayer', 'Lcom/google/android/exoplayer'), ('Media3', 'Landroidx/media3/'),
        ('VideoView', 'Landroid/widget/VideoView'), ('TextureView', 'Landroid/view/TextureView'),
        ('SurfaceView', 'Landroid/view/SurfaceView'),
        ('SurfaceTexture', 'Landroid/graphics/SurfaceTexture')) if refs(v)}
    r['gl'] = {k: refs(v) for k, v in (
        ('GLSurfaceView', 'Landroid/opengl/GLSurfaceView'), ('GLES20', 'Landroid/opengl/GLES20;'),
        ('GLES30', 'Landroid/opengl/GLES30;'), ('GLES31', 'Landroid/opengl/GLES31'),
        ('GLES32', 'Landroid/opengl/GLES32;'), ('EGL14', 'Landroid/opengl/EGL14;'),
        ('RenderScript', 'Landroid/renderscript/')) if refs(v)}
    r['webview'] = {
        'WebView refs': refs('Landroid/webkit/WebView;'),
        'WebView subclasses': sum(1 for c in app_classes if 'Landroid/webkit/WebView;' in
                                  platform_bases(c)),
        'androidx.webkit': refs('Landroidx/webkit/'),
        'Custom Tabs': refs('Landroidx/browser/customtabs/') + refs('Landroid/support/customtabs/'),
    }

    # --- native libraries
    by_abi = collections.defaultdict(dict)   # abi -> {name: (part, zip, info)}
    for p, z, i in apk.entries():
        m = re.match(r'lib/([^/]+)/([^/]+\.so)$', i.filename)
        if m:
            by_abi[m.group(1)][m.group(2)] = (p, z, i)
    stray = [n for n in natives if not n.startswith('lib/')]
    r['native'] = {'abis': {abi: {'count': len(v), 'size': sum(e[2].file_size for e in v.values())}
                            for abi, v in sorted(by_abi.items())},
                   'outside_lib': stray}
    abis = set(by_abi)
    r['native']['only_arm64'] = 'arm64-v8a' in abis and not abis & {'armeabi-v7a', 'armeabi'}
    for abi in ('armeabi-v7a', 'armeabi', 'arm64-v8a'):
        if abi not in by_abi:
            continue
        if abi == 'armeabi' and 'armeabi-v7a' in by_abi:
            continue
        r['native'][abi] = native_imports(apk, by_abi[abi], abi, ndk81, ndk60, args)
    r['seconds'] = round(time.time() - t0, 1)
    return r


def native_imports(apk, libs, abi, ndk81, ndk60, args):
    arch = 'arm64' if abi == 'arm64-v8a' else 'arm'
    infos, not_elf, svc = {}, [], {}
    for name, (_, z, i) in sorted(libs.items()):
        data = z.read(i)
        e = elf_info(data)
        if e is None:
            not_elf.append(name)
        else:
            infos[name] = e
            if arch == 'arm':
                calls = direct_syscalls(data)
                if calls:
                    svc[name] = {(hex(k) if k != '?' and k >= 0xf0000 else str(k)): v
                                 for k, v in sorted(calls.items(),
                                                    key=lambda kv: -1 if kv[0] == '?' else kv[0])}
    defined = set()
    for e in infos.values():
        defined |= e['defined']
    system_libs = BRIDGE_LIBS | {lib for v in ndk81.values() for lib, _ in v}
    sys_needed = collections.Counter()
    by_level = collections.defaultdict(lambda: collections.defaultdict(set))  # level -> lib -> syms
    weak_new = collections.defaultdict(set)
    unknown = collections.Counter()
    for name, e in infos.items():
        for n in e['needed']:
            if n not in libs:
                sys_needed[n] += 1
        for s, weak in e['undefined'].items():
            if s in defined:
                continue
            hits = ndk81.get(s)
            if not hits:
                if s in ndk60:   # libz and the like: in the 6.0 list, no map.txt
                    by_level['<=23']['?'].add(s)
                else:
                    unknown[s] += 1
                continue
            lib, levels = hits[0]
            lv = levels[0 if arch == 'arm' else 1]
            if lv.isdigit():
                key = '<=23' if int(lv) <= OUR_API else int(lv)
            else:
                key = lv
            if weak and key not in ('<=23',):
                weak_new[str(key)].add(s)
                continue
            by_level[key][lib].add(s)
    out = {'libs': len(infos), 'not_elf': not_elf,
           'system_needed': dict(sys_needed),
           'needed_beyond_bridge': sorted(n for n in sys_needed if n not in BRIDGE_LIBS and
                                          n in system_libs),
           'absent_app_libs': sorted(n for n in sys_needed if n not in system_libs),
           'imports_by_level': {str(k): {lib: sorted(v) for lib, v in libs_.items()}
                                for k, libs_ in sorted(by_level.items(), key=lambda kv: str(kv[0]))
                                if k != '<=23'},
           'imports_le23': sum(len(v) for v in by_level.get('<=23', {}).values()),
           'weak_new': {k: sorted(v) for k, v in weak_new.items()},
           'unresolved_other': len(unknown),
           'direct_syscalls': svc}
    if args.ndk_check and arch == 'arm':
        out['bridge'] = bridge_check(apk, libs, abi, ndk81, args)
    return out


def bridge_check(apk, libs, abi, ndk81, args):
    """tools/ndk_check_all_imports.py on the libraries: what the bridge cannot resolve."""
    if not os.path.exists(NDK_UNIT):
        return {'error': 'no %s (make build/ndk/ndk_unit_test.js)' % NDK_UNIT}
    d = tempfile.mkdtemp(prefix='libs-%s-' % abi, dir=args.work)
    try:
        for name, (_, z, i) in libs.items():
            with z.open(i) as src, open(os.path.join(d, name), 'wb') as dst:
                shutil.copyfileobj(src, dst)
        p = subprocess.run([sys.executable, '-I', NDK_CHECK, d, '--verbose',
                            '--jobs', str(args.jobs)], capture_output=True, text=True)
    finally:
        shutil.rmtree(d, ignore_errors=True)
    section, system, other, absent = None, [], [], []
    for line in p.stdout.splitlines():
        if line.startswith('== '):
            section = line
            continue
        if not line.startswith('  '):
            continue
        if 'without a thunk' in (section or ''):
            system.append(line.split()[0])
        elif 'lacks' in (section or ''):
            absent.append(line.split()[0].rstrip(','))
        elif 'other unresolved' in (section or ''):
            lib, _, rest = line.strip().partition(': ')
            other.extend(rest.split()[1:])
    # Sort what the bridge lacks by Android 8.1's NDK level.
    gaps = collections.defaultdict(lambda: collections.defaultdict(set))
    nonsys = set()
    for s in set(system) | set(other):
        hits = ndk81.get(s)
        if not hits:
            if s in system:
                gaps['<=23']['?'].add(s)
            else:
                nonsys.add(s)
            continue
        lib, levels = hits[0]
        lv = levels[0]
        key = ('<=23' if int(lv) <= OUR_API else lv) if lv.isdigit() else lv
        gaps[key][lib].add(s)
    return {'exit': p.returncode,
            'missing_system_60': sorted(system),
            'gaps': {k: {lib: sorted(v) for lib, v in libs_.items()} for k, libs_ in gaps.items()},
            'absent_libraries': absent,
            'non_system_unresolved': len(nonsys)}


# ---------------------------------------------------------------------------------------------
# Report

def mb(n):
    return '%.1f MB' % (n / 1e6)


def verdict(r):
    """What blocks the app on our 8.1, by phase of docs/android8-plan.md."""
    out = []
    man, dex = r['manifest'], r['dex']
    vers = sorted(dex['versions'])
    if any(v > '035' for v in vers) or dex['default_methods'] or dex['call_sites'] or \
            dex['ops'].get('invoke-polymorphic'):
        out.append(('art8', 'dex %s, default methods %d, invoke-custom %d, invoke-polymorphic %d'
                    % ('/'.join(vers), dex['default_methods'], dex['ops'].get('invoke-custom', 0),
                       dex['ops'].get('invoke-polymorphic', 0))))
    j8 = {k: v for k, v in r['java8'].items() if not k.startswith('j$')}
    if j8:
        out.append(('art8 (libcore 8.1)', ', '.join('%s %d' % kv for kv in sorted(j8.items()))))
    api = {int(k): v for k, v in r['api'].items()}
    n2427 = sum(len(v) for k, v in api.items() if k <= TARGET_API)
    if n2427:
        out.append(('fw8-system/fw8-core', '%d references to API 24-27' % n2427))
    missing = [n for n, s in r['services'].items() if not s['lean60'] and not s['local'] and
               s['manager']]
    if missing:
        out.append(('fw8-system', 'services absent from the lean server: ' + ', '.join(
            sorted(missing))))
    gl = r['manifest'].get('glEs')
    if gl and gl > '3.0':
        out.append(('fw8-graphics', 'OpenGL ES %s required (WebGL2 = ES 3.0)' % gl))
    for abi in ('armeabi-v7a', 'armeabi', 'arm64-v8a'):
        n = r['native'].get(abi)
        if n and n.get('needed_beyond_bridge'):
            out.append(('ndk (%s)' % abi, 'system libraries the bridge lacks: ' + ', '.join(
                n['needed_beyond_bridge'])))
    if r['native']['only_arm64']:
        out.append(('ndk-arm64', 'native code only for arm64-v8a'))
    if r['packers']:
        out.append(('protection', ', '.join(r['packers'])))
    g = r['gms']
    if g['availability_check'] or g['safetynet'] or g['integrity']:
        out.append(('GMS', 'checks for Play services%s%s' % (
            ', SafetyNet' if g['safetynet'] else '', ', Play Integrity' if g['integrity'] else '')))
    return out


def report_md(r):
    man, dex, nat = r['manifest'], r['dex'], r['native']
    L = []
    L.append('### %s — `%s` %s' % (r['file'], man['package'], man.get('versionName') or ''))
    L.append('')
    L.append('| | |')
    L.append('|---|---|')
    L.append('| minSdk / targetSdk | %s / %s |' % (man['minSdk'], man['targetSdk']))
    L.append('| размер | %s%s |' % (mb(r['size']), ', части: ' + ', '.join(r['parts'])
                                     if len(r['parts']) > 1 else ''))
    L.append('| launcher | `%s` |' % man['launcher'])
    L.append('| Application | `%s` |' % man['application'])
    L.append('| dex | %d файлов, %s, версии %s, классов %d |' % (
        dex['count'], mb(dex['size']), ', '.join('%s×%d' % kv for kv in sorted(
            dex['versions'].items())), dex['classes']))
    L.append('| Java 8 в dex | default-методов %d, static в интерфейсах %d, call sites %d, '
             'method handles %d, %s |' % (
                 dex['default_methods'], dex['static_interface_methods'], dex['call_sites'],
                 dex['method_handles'], ', '.join('%s %d' % kv for kv in sorted(
                     dex['ops'].items())) or 'invoke-polymorphic/custom нет'))
    L.append('| java.time/stream/function/invoke | %s |' % (', '.join(
        '%s %d' % kv for kv in sorted(r['java8'].items())) or '—'))
    abis = ', '.join('%s (%d, %s)' % (a, v['count'], mb(v['size']))
                     for a, v in nat['abis'].items()) or 'нет'
    L.append('| ABI | %s%s |' % (abis, ' — **только arm64**' if nat['only_arm64'] else ''))
    L.append('| движок/SDK | %s |' % ', '.join(
        '%s%s' % (k, ' (%s)' % ', '.join(v['libs']) if v['libs'] else '')
        for k, v in r['sdks'].items()))
    if r['packers']:
        L.append('| защита | %s |' % ', '.join(r['packers']))
    g = r['gms']
    L.append('| Play services | проверка наличия: %s; SafetyNet: %s; Play Integrity: %s; FCM: %s; '
             'модули GMS: %s; Firebase: %s |' % (
                 'да' if g['availability_check'] else 'нет', 'да' if g['safetynet'] else 'нет',
                 'да' if g['integrity'] else 'нет', 'да' if g['fcm'] else 'нет',
                 ', '.join(g['gms_modules']) or '—', ', '.join(g['firebase_modules']) or '—'))
    L.append('| WebView | %s |' % ', '.join('%s %d' % kv for kv in r['webview'].items() if kv[1])
             or '—')
    L.append('| медиа | %s |' % (', '.join('%s %d' % kv for kv in r['media'].items()) or '—'))
    L.append('| OpenGL ES | манифест: %s; Java: %s |' % (
        man.get('glEs') or '—', ', '.join(r['gl']) or '—'))
    vk = [f for f, _ in man['features'] if 'vulkan' in f]
    L.append('| uses-feature | %s |' % (', '.join('%s%s' % (f, '' if req else ' (необяз.)')
                                                   for f, req in man['features']) or '—'))
    if man['libraries']:
        L.append('| uses-library | %s |' % ', '.join('%s%s' % (n, '' if req else ' (необяз.)')
                                                      for n, req in man['libraries']))
    L.append('| компоненты | activity %d, service %d, receiver %d, provider %d |' % (
        len(man['activities']), len(man['services']), len(man['receivers']),
        len(man['providers'])))
    L.append('| разрешения (%d) | %s |' % (len(man['permissions']), ', '.join(
        sorted(p.replace('android.permission.', '') for p in man['permissions'] if p))))
    L.append('| provider\'ы | %s |' % ', '.join(sorted('`%s`' % p['name'].split('.')[-1]
                                                       for p in man['providers'])))
    L.append('')
    sv = r['services']
    L.append('**Системные сервисы** (менеджер в dex / строка имени; ✗ — нет в облегчённом '
             'system_server 6.0.1): ' + ', '.join(
                 '%s%s%s' % (n, '' if s['lean60'] or s['local'] else ' ✗',
                             ' (API %d)' % s['level'] if s['level'] > OUR_API else '')
                 for n, s in sorted(sv.items())))
    L.append('')
    if r['aidl']:
        L.append('**Чужие AIDL-интерфейсы:** ' + ', '.join('`%s`' % a for a in r['aidl'][:25]) +
                 (' … (%d)' % len(r['aidl']) if len(r['aidl']) > 25 else ''))
        L.append('')
    api = r['api']
    if api:
        L.append('**API выше 23** (ссылки из dex; уровни > 27 приложение обязано проверять '
                 '`SDK_INT`, на 8.1 они не вызываются):')
        L.append('')
        for lv, refs in api.items():
            if int(lv) <= TARGET_API:
                L.append('- API %s (%d): %s' % (lv, len(refs), ', '.join(
                    '`%s`' % x for x in refs[:40]) + (' …' if len(refs) > 40 else '')))
        later = sum(len(v) for k, v in api.items() if int(k) > TARGET_API)
        L.append('- API 28+: %d ссылок' % later)
        L.append('')
    if r['outside_sdk']:
        L.append('**Вне SDK** (скрытые API, `org.apache.http`): ' + ', '.join(
            '`%s`' % x for x in r['outside_sdk'][:30]) + (' … (%d)' % len(r['outside_sdk'])
                                                         if len(r['outside_sdk']) > 30 else ''))
        L.append('')
    for abi in ('armeabi-v7a', 'armeabi', 'arm64-v8a'):
        n = nat.get(abi)
        if not n:
            continue
        L.append('**NDK `%s`:** %d ELF%s; системные библиотеки: %s%s' % (
            abi, n['libs'], ', не ELF: %d (%s)' % (len(n['not_elf']), ', '.join(
                n['not_elf'][:8]) + (' …' if len(n['not_elf']) > 8 else '')) if n['not_elf']
            else '', ', '.join(sorted(set(n['system_needed']) - set(n['absent_app_libs']))),
            '; библиотек приложения нет в APK: ' + ', '.join(n['absent_app_libs'])
            if n['absent_app_libs'] else ''))
        if n['needed_beyond_bridge']:
            L.append('— **нет в мосте:** ' + ', '.join(n['needed_beyond_bridge']))
        for lv, libs in n['imports_by_level'].items():
            L.append('- импорты уровня %s: %s' % (lv, '; '.join(
                '%s: %s' % (lib, ', '.join(syms[:15]) + (' …(%d)' % len(syms)
                                                          if len(syms) > 15 else ''))
                for lib, syms in sorted(libs.items()))))
        if n['weak_new']:
            L.append('- слабые (необязательные) импорты новее 23: %s' % '; '.join(
                '%s: %d' % kv for kv in ((k, len(v)) for k, v in n['weak_new'].items())))
        if n.get('direct_syscalls'):
            L.append('- прямые `svc` (номер EABI: сколько мест; ? — номер в регистре): ' + '; '.join(
                '%s: %s' % (lib, ', '.join('%s×%d' % kv for kv in calls.items()))
                for lib, calls in n['direct_syscalls'].items()))
        b = n.get('bridge')
        if b:
            if 'error' in b:
                L.append('- мост: %s' % b['error'])
            else:
                L.append('- **мост NDK** (`ndk_check_all_imports.py`): системных символов без '
                         'переходника по списку 6.0 — %d; по уровням 8.1: %s; отсутствующих '
                         'библиотек приложения: %d; прочих несистемных символов: %d' % (
                             len(b['missing_system_60']),
                             '; '.join('%s: %s' % (k, ', '.join(
                                 '%s %s' % (lib, ' '.join(s[:12]) + (' …(%d)' % len(s)
                                                                     if len(s) > 12 else ''))
                                 for lib, s in sorted(v.items())))
                                 for k, v in sorted(b['gaps'].items())) or 'нет',
                             len(b['absent_libraries']), b['non_system_unresolved']))
        L.append('')
    v = verdict(r)
    if v:
        L.append('**Что нужно для запуска на 8.1:**')
        L.append('')
        for phase, what in v:
            L.append('- `%s`: %s' % (phase, what))
        L.append('')
    return '\n'.join(L)


def summary_md(results):
    L = ['| Приложение | min/target | dex | Java 8 | ABI | движок | API 24–27 | сервисы ✗ | '
         'NDK: мост | GMS | WebView | медиа | GL |', '|' + '---|' * 13]
    for r in results:
        man, dex, nat = r['manifest'], r['dex'], r['native']
        engine = [k for k in ('Unity', 'Unreal Engine', 'Flutter', 'React Native', 'Xamarin/.NET',
                              'Cocos2d-x', 'Godot', 'libGDX', 'Cordova/Capacitor (WebView app)')
                  if k in r['sdks']]
        lang = 'Kotlin' if 'Kotlin' in r['sdks'] else 'Java'
        api = {int(k): len(v) for k, v in r['api'].items() if int(k) <= TARGET_API}
        abis = '/'.join(a.replace('armeabi-v7a', 'v7a').replace('arm64-v8a', 'arm64')
                        for a in nat['abis']) or '—'
        if nat['only_arm64']:
            abis = '**только arm64**'
        bridge = []
        for abi in ('armeabi-v7a', 'armeabi'):
            b = nat.get(abi, {}).get('bridge')
            if b and 'gaps' in b:
                bridge.append('%d (≤23: %d)' % (
                    sum(len(s) for v in b['gaps'].values() for s in v.values()),
                    sum(len(s) for s in b['gaps'].get('<=23', {}).values())))
            elif nat.get(abi):
                n = nat[abi]
                bridge.append('%d новых' % sum(len(s) for k, v in n['imports_by_level'].items()
                                                for s in v.values()))
        missing = [n for n, s in r['services'].items() if not s['lean60'] and not s['local'] and
                   s['manager']]
        g = r['gms']
        L.append('| %s | %s/%s | %d × %s | def %d, call-site %d, poly %d | %s | %s | %s | %s | %s |'
                 ' %s | %s | %s | %s |' % (
                     man['package'], man['minSdk'], man['targetSdk'], dex['count'],
                     '/'.join(sorted(dex['versions'])), dex['default_methods'],
                     dex['call_sites'], dex['ops'].get('invoke-polymorphic', 0), abis,
                     ', '.join(engine) or lang, ' '.join('%d:%d' % kv for kv in sorted(api.items()))
                     or '—', ', '.join(sorted(missing)) or '—', ', '.join(bridge) or '—',
                     ('проверка' if g['availability_check'] else '') +
                     (' SafetyNet' if g['safetynet'] else '') +
                     (' Integrity' if g['integrity'] else '') +
                     (' FCM' if g['fcm'] else '') or ('клиент' if g['gms_modules'] else '—'),
                     r['webview']['WebView refs'] or '—',
                     ', '.join(k for k in r['media'] if k in (
                         'MediaCodec', 'ExoPlayer', 'Media3', 'MediaPlayer', 'camera2')) or '—',
                     man.get('glEs') or ('GLES30' if 'GLES30' in r['gl'] else
                                         'GLES20' if 'GLES20' in r['gl'] else '—')))
    return '\n'.join(L)


def aggregate_md(results, top=40):
    """What the most apps need: API 24-27 classes, services, NDK gaps."""
    n = len(results)
    L = ['Приложений: %d. Только arm64: %d. dex 037+: %d. Default-методы: %d. '
         'invoke-polymorphic/custom: %d. Проверка Play services: %d. Play Integrity/SafetyNet: %d.'
         % (n, sum(r['native']['only_arm64'] for r in results),
            sum(any(v > '035' for v in r['dex']['versions']) for r in results),
            sum(bool(r['dex']['default_methods']) for r in results),
            sum(bool(r['dex']['ops'].get('invoke-polymorphic') or
                     r['dex']['ops'].get('invoke-custom') or r['dex']['call_sites'])
                for r in results),
            sum(r['gms']['availability_check'] for r in results),
            sum(r['gms']['integrity'] or r['gms']['safetynet'] for r in results)), '']
    for key, title in (('api_new_classes', 'Новые классы API 24–27'),
                       ('api_classes', 'Старые классы с новыми методами/полями API 24–27')):
        classes, level_of = collections.Counter(), {}
        for r in results:
            seen = set()
            for lv, cs in r[key].items():
                if int(lv) <= TARGET_API:
                    for c in cs:
                        level_of[c] = min(int(lv), level_of.get(c, 99))
                        seen.add(c)
            classes.update(seen)
        ranked = sorted(classes.items(), key=lambda kv: (-kv[1], level_of[kv[0]], kv[0]))
        L.append('**%s (приложений, уровень):** ' % title + ', '.join(
            '`%s` %d (%d)' % (c, k, level_of[c]) for c, k in ranked[:top]) +
            (' … всего %d' % len(ranked) if len(ranked) > top else ''))
        L.append('')
    services = collections.Counter()
    for r in results:
        services.update(s for s, v in r['services'].items() if v['manager'] and not v['lean60']
                        and not v['local'])
    L.append('**Сервисы, которых нет в облегчённом system_server 6.0.1:** ' + ', '.join(
        '%s %d' % kv for kv in services.most_common()))
    L.append('')
    gaps, libs = collections.Counter(), collections.Counter()
    for r in results:
        seen, seen_libs = set(), set()
        for abi in ('armeabi-v7a', 'armeabi', 'arm64-v8a'):
            nat = r['native'].get(abi)
            if not nat:
                continue
            seen_libs.update(nat['needed_beyond_bridge'])
            b = nat.get('bridge') or {}
            for lv, by_lib in b.get('gaps', {}).items():
                for lib, syms in by_lib.items():
                    seen.update('%s %s (%s)' % (lib, s, lv) for s in syms)
            if abi == 'arm64-v8a' and not r['native'].get('armeabi-v7a'):
                for lv, by_lib in nat['imports_by_level'].items():
                    for lib, syms in by_lib.items():
                        seen.update('%s %s (%s, arm64)' % (lib, s, lv) for s in syms)
        gaps.update(seen)
        libs.update(seen_libs)
    L.append('**Системные библиотеки, которых нет в мосте NDK:** ' + (', '.join(
        '%s %d' % kv for kv in libs.most_common()) or 'нет'))
    L.append('')
    L.append('**Символы, которых не хватает мосту NDK (сколько приложений):** ' + (', '.join(
        '`%s` %d' % kv for kv in sorted(gaps.items(), key=lambda kv: (-kv[1], kv[0]))[:top * 2])
        or 'нет') +
        (' … всего %d' % len(gaps) if len(gaps) > top * 2 else ''))
    return '\n'.join(L)


def load_ndk81():
    table = collections.defaultdict(list)
    with open(NDK81_SYMBOLS) as f:
        for line in f:
            if line.startswith('#'):
                continue
            sym, lib, arm, arm64 = line.split()
            table[sym].append((lib, (arm, arm64)))
    return table


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('apks', nargs='+', help='APK, bundle (.apks/.xapk/.apkm) or directory')
    ap.add_argument('--api-versions', default=API_VERSIONS)
    ap.add_argument('--ndk-check', action='store_true',
                    help='run the NDK bridge on the 32-bit ARM libraries')
    ap.add_argument('--work', help='where to unpack bundles and libraries (default: a temp dir)')
    ap.add_argument('--jobs', type=int, default=6)
    ap.add_argument('--md', help='write the Markdown report here')
    ap.add_argument('--json', help='write every finding as JSON here')
    args = ap.parse_args()
    own_work = args.work is None
    args.work = tempfile.mkdtemp(prefix='apk-survey-', dir=args.work)
    t = time.time()
    api = Api(args.api_versions)
    print('api-versions: %d classes, %.1f s' % (len(api.classes), time.time() - t), file=sys.stderr)
    ndk81 = load_ndk81()
    ndk60 = set(open(NDK60_SYMBOLS).read().split())
    paths = []
    for p in args.apks:
        if os.path.isdir(p) and not os.path.exists(os.path.join(p, 'base.apk')):
            paths += sorted(os.path.join(p, f) for f in os.listdir(p)
                            if f.endswith(('.apk', '.apks', '.xapk', '.apkm')))
        else:
            paths.append(p)
    results = []
    try:
        for p in paths:
            print('surveying %s ...' % p, file=sys.stderr)
            try:
                r = survey(p, api, ndk81, ndk60, args)
            except Exception as e:  # one broken APK must not stop the survey
                import traceback
                traceback.print_exc()
                print('%s: %s' % (p, e), file=sys.stderr)
                continue
            print('  %s: %.1f s' % (r['manifest']['package'], r['seconds']), file=sys.stderr)
            results.append(r)
    finally:
        shutil.rmtree(args.work, ignore_errors=True)
    md = '## Сводка\n\n' + summary_md(results) + '\n\n' + aggregate_md(results) + \
        '\n\n## По приложениям\n\n' + '\n\n'.join(
        report_md(r) for r in results) + '\n'
    if args.md:
        with open(args.md, 'w') as f:
            f.write(md)
    else:
        print(md)
    if args.json:
        with open(args.json, 'w') as f:
            json.dump(results, f, ensure_ascii=False, indent=1, default=sorted)


if __name__ == '__main__':
    main()
