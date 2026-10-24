#!/bin/sh
# Runs the Java tests in arc-tests.jar on Dalvik and prints one summary line
# per test. Exit status: number of tests that failed.
#
#   run_java_tests.sh node [TEST...]     build/dalvik/dalvikvm.js under Node
#   run_java_tests.sh safari [TEST...]   build/dalvik-web in Safari, through a
#                                        running tools/serve.py (PORT, default
#                                        8125, serving build/ with LOG, default
#                                        build/browser-log.txt)
#   run_java_tests.sh chrome [TEST...]   the same in Google Chrome (WebGPU there:
#                                        VulkanTest's real path)
# Default tests: VmTest LooperTest BinderTest ApkTest GraphicsTest
# FrameworkResTest ViewTest InputTest WindowTest ActivityTest GlesTest NdkTest CryptoTest
# Ndk6Test NdkRelocTest NetTest NetDenyTest NetOffTest ConnectivityTest (class names in package arcwasm). NetTest needs the internet and an
# echo server: serve.py's (PORT + 1) in Safari; under Node this script starts
# one (tools/net_relay.py --echo) and the kernel uses Node's sockets.
# NetDenyTest and NetOffTest (all four stacks; topic app-network,
# docs/topics/app-network.md) are NetPolicyTest with an app's own network
# setting as SENSES Web Apps gives it: ARC_NET_DENY=... (closed names; needs
# the internet and the echo server too) and ARC_NET=0 (no network).
#
# VM=art runs them on ART (Android 6.0.1) instead: build/art/artvm.js and
# build/art-web (mk/topic-art.mk); default tests VmTest, ArtSpeedTest (topic
# art-speed) and the framework 6.0.1 ones of topics fw6-core, fw6-graphics,
# fw6-crypto, fw6-system, mediacodec and tiktok (the DEFAULT_TESTS below).
#
# A VM may ask to be started again (exit status 75: topic mem-budget's
# restart for memory, where a page reloads): under Node the test is run
# again, up to 3 times; the tests that do (RESTART_TESTS) get a /data that
# outlives the VM, as OPFS does (ARC_NODE_OPFS_DIR, a directory made for the
# test). In Safari the page reloads itself.
#
# VM=art8 runs them on ART 8.1 (Android 8.1): build/art8/artvm8.js and
# build/art8-web (mk/topic-art8.mk); default tests of topics art8 (VmTest,
# ArtSpeedTest, Java8Test), fw8-core, fw8-crypto-media, fw8-graphics and fw8-system
# (WindowTest, ActivityTest8, AppSupportTest8, NdkTest, Ndk6Test, Ndk8Test); later 8.1
# topics add theirs. NdkRelocTest (topic relocs32) runs on all three 32-bit stacks.
# WebViewTest (topic webview8): in Node only the stub; the real WebView with
# PAGE=fw8-graphics-web.
#
# NativeActivityTest (topic native-activity) is in the 6.0.1, 8.1 and 10 lists:
# android.app.NativeActivity running an ARM library (android_native_app_glue);
# its GL frame only where the VM has WebGL (PAGE=...-graphics-web).
# NativeActivityArm64Test (8.1, 10): the same with the app's arm64-v8a build.
#
# VM=art10 runs them on ART 10 (Android 10): build/art10/artvm10.js and
# build/art10-web (mk/topic-art10.mk); default tests of topic art10 (VmTest,
# ArtSpeedTest, Java8Test, Java9Test), fw10-core (LooperTest, BinderTest,
# ApkTest, FrameworkResTest, Framework10Test), fw10-graphics (GraphicsTest,
# ViewTest, InputTest, SurfaceTest, HwuiTest: the 10 versions of the last two are in
# tests/java10; PAGE=fw10-graphics-web for the display on the page's canvas),
# fw10-crypto-media (CryptoTest, NetTest, XmlTest, DatabaseTest, AudioTest,
# MediaTest, NdkMediaTest) and fw10-system (ActivityTest10, AppSupportTest10,
# NdkTest, NdkArm64Test, InstallTest, SplitInstallTest: the system server 10,
# the NDK bridge on ART 10, SENSES Web Apps' library on stack a10); later 10
# topics add theirs.
#
# JIT=0 runs every test "without JIT" (topic guest-jit, docs/topics/guest-jit.md):
# ARC_JIT=0 in the VM's environment, as SENSES Web Apps' shell gives an app
# whose JIT is off; the VM's ro.arcwasm.jit is then 0 (no ARM/arm64
# translation to wasm, no ART JIT).
#
# GmsTest and GmsMicrogTest (topic gms, docs/topics/gms.md; 8.1 and 10, Node
# only): an app on Google's real client libraries without Google services,
# and with microG installed as SENSES Web Apps' shell installs it (its /data
# gets /data/google/PACKAGE/base.apk from build/gms/microg: make microg-apks);
# GmsOffTest: microG installed so too, the app's switch off (ARC_GMS=0, the
# shell's «Сервисы Google: Выкл»): an app sees no Google services.
# VulkanTest (topic vulkan, docs/topics/vulkan.md) on 8.1 and 10: in Node
# "no device" and the null device; with WebGPU (chrome, Safari 26) the real
# device, PAGE=fw8-graphics-web / fw10-graphics-web.
#
# InstallTest (topic install8) is in all three lists: SENSES Web Apps' app
# library, with the VM's environment as the shell gives it (below); so is
# SplitInstallTest (topic apks-split: apps as split APKs, 4.4 only checks
# that it leaves them out).
cd "$(dirname "$0")/.."
ROOT=$(pwd)
mode=$1; shift
case ${VM:-dalvik} in
  art) VMDIR=art; VMJS=artvm.js; WEB=art-web
       DEFAULT_TESTS="VmTest ArtSpeedTest LooperTest BinderTest ApkTest FrameworkResTest GraphicsTest ViewTest InputTest SurfaceTest HwuiTest WindowTest ActivityTest NdkTest Ndk6Test NdkRelocTest GlesTest CryptoTest NetTest NetDenyTest NetOffTest XmlTest MediaTest AppSupportTest InstallTest SplitInstallTest NdkJitTest NativeActivityTest" ;;
  art8) VMDIR=art8; VMJS=artvm8.js; WEB=art8-web
        DEFAULT_TESTS="VmTest ArtSpeedTest Java8Test LooperTest BinderTest ApkTest FrameworkResTest GraphicsTest ViewTest InputTest SurfaceTest HwuiTest WindowTest ActivityTest8 AppSupportTest8 NdkTest Ndk6Test Ndk8Test NdkRelocTest CryptoTest NetTest NetDenyTest NetOffTest XmlTest AudioTest MediaTest NdkMediaTest NdkMediaArmTest TextureViewTest MemoryTest InstallTest NdkArm64Test NdkMediaArm64Test SplitInstallTest WebViewTest NdkJitTest MultiProcessTest GmsTest GmsMicrogTest GmsOffTest VulkanTest NativeActivityTest NativeActivityArm64Test" ;;
  art10) VMDIR=art10; VMJS=artvm10.js; WEB=art10-web
         DEFAULT_TESTS="VmTest ArtSpeedTest Java8Test Java9Test DormantTest LooperTest BinderTest ApkTest FrameworkResTest Framework10Test GraphicsTest ViewTest InputTest SurfaceTest HwuiTest CryptoTest NetTest NetDenyTest NetOffTest XmlTest DatabaseTest AudioTest MediaTest NdkMediaTest ActivityTest10 AppSupportTest10 NdkTest Ndk6Test NdkArm64Test InstallTest SplitInstallTest NdkJitTest WebViewTest MultiProcessTest GmsTest GmsMicrogTest GmsOffTest VulkanTest NativeActivityTest NativeActivityArm64Test" ;;
  *) VMDIR=dalvik; VMJS=dalvikvm.js; WEB=dalvik-web
     DEFAULT_TESTS="VmTest LooperTest BinderTest ApkTest GraphicsTest FrameworkResTest ViewTest InputTest WindowTest ActivityTest GlesTest NdkTest CryptoTest Ndk6Test NdkRelocTest NetTest NetDenyTest NetOffTest ConnectivityTest InstallTest SplitInstallTest NdkJitTest" ;;
esac
# PAGE=DIR: the browser page under build/ (default: the VM's plain page;
# e.g. PAGE=fw8-graphics-web for the display on the page's canvas).
WEB=${PAGE:-$WEB}
TESTS=${*:-$DEFAULT_TESTS}
# Tests that restart their VM (exit 75) and need /data to outlive it.
RESTART_TESTS="MemoryTest InstallTest SplitInstallTest"
case ${VM:-dalvik} in art) STACK=a60 ;; art8) STACK=a81 ;; art10) STACK=a10 ;; *) STACK=a44 ;; esac
PORT=${PORT:-8125}
LOG=${LOG:-build/browser-log.txt}
bad=0
echo_pid=
http_pid=
trap '[ -n "$echo_pid" ] && kill $echo_pid 2>/dev/null; [ -n "$http_pid" ] && kill $http_pid 2>/dev/null' EXIT
for t in $TESTS; do
  targs=
  if [ "$t" = NetTest ] || [ "$t" = NetDenyTest ] || [ "$t" = NetOffTest ]; then
    if [ "$mode" = node ]; then
      if [ -z "$echo_pid" ]; then
        fifo=$(mktemp -u); mkfifo "$fifo"
        python3 tools/net_relay.py --echo 0 > "$fifo" & echo_pid=$!
        read -r echo_port < "$fifo"; rm -f "$fifo"
      fi
      targs=127.0.0.1:$echo_port
    else
      targs=127.0.0.1:$((PORT + 1))
    fi
  fi
  if [ "$t" = MediaTest ]; then
    # Its HTTP checks: tests/media as build/media-http (mk/topic-mediacodec.mk),
    # from serve.py in Safari; under Node from a plain HTTP server started here.
    if [ "$mode" = node ]; then
      if [ -z "$http_pid" ]; then
        fifo=$(mktemp -u); mkfifo "$fifo"
        python3 -c 'import functools, http.server
class H(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *a): pass
s = http.server.ThreadingHTTPServer(("127.0.0.1", 0), functools.partial(H, directory="build"))
print(s.server_address[1], flush=True)
s.serve_forever()' > "$fifo" & http_pid=$!
        read -r http_port < "$fifo"; rm -f "$fifo"
      fi
      targs=http://127.0.0.1:$http_port/media-http/
    else
      targs=http://127.0.0.1:$PORT/media-http/
    fi
  fi
  # Environment of the VM (getenv): InstallTest runs as SENSES Web Apps' shell
  # starts a stack (topic install8): its own /data, the app library at
  # /mnt/apps, and (the test plays the shell) the whole storage at /mnt/opfs.
  tenv=
  if [ "$t" = InstallTest ] || [ "$t" = SplitInstallTest ]; then
    tenv="ARC_DATA_DIR=senses/data/$STACK ARC_APPS_DIR=senses/apps ARC_OPFS_ROOT_MOUNT=/mnt/opfs ARC_TEST_RUN=$$-$(date +%s)"
  fi
  # (and the test app's OBB files mounted, as the shell does for an .xapk's)
  [ "$t" = SplitInstallTest ] && tenv="$tenv ARC_APP_OBB=org.arcwasm.splittest"
  # The activity tests turn the display for a landscape activity (topic
  # rotation): Node's headless display portrait, as a phone's.
  case ${VM:-dalvik}:$t in art:ActivityTest|art8:ActivityTest8|art10:ActivityTest10) tenv="$tenv ARC_HEADLESS_DISPLAY=320x480" ;; esac
  [ "${JIT:-1}" = 0 ] && tenv="$tenv ARC_JIT=0"
  [ -n "$ARC_GMS" ] && tenv="$tenv ARC_GMS=$ARC_GMS"
  # GmsMicrogTest: GmsTest with microG in the stack's /data, as the shell
  # puts it ("Сервисы Google"); the APKs cloned in (APFS: no copy).
  cls=$t
  # NetDenyTest, NetOffTest: an app's network setting (topic app-network).
  case $t in
    NetDenyTest) cls=NetPolicyTest; targs="deny $targs"
                 tenv="$tenv ARC_NET_DENY=example.com,.ample.org,Blocked.Test." ;;
    NetOffTest) cls=NetPolicyTest; targs="off $targs"; tenv="$tenv ARC_NET=0" ;;
  esac
  if [ "$t" = GmsMicrogTest ] || [ "$t" = GmsOffTest ]; then
    cls=GmsTest
    targs=microg
    tenv="$tenv ARC_DATA_DIR=senses/data/$STACK"
    [ "$t" = GmsOffTest ] && targs=off && tenv="$tenv ARC_GMS=0"
  fi
  case $mode in
    node)
      data=
      case " $RESTART_TESTS " in *" $t "*) data=$(mktemp -d) ;; esac
      if [ "$t" = GmsMicrogTest ] || [ "$t" = GmsOffTest ]; then
        data=$(mktemp -d)
        for apk in build/gms/microg/*.apk; do
          [ -f "$apk" ] || continue
          pkg=$(basename "$apk" | sed 's/-[0-9]*\.apk$//')
          mkdir -p "$data/senses/data/$STACK/google/$pkg"
          cp -c "$apk" "$data/senses/data/$STACK/google/$pkg/base.apk" 2>/dev/null ||
            cp "$apk" "$data/senses/data/$STACK/google/$pkg/base.apk"
        done
      fi
      runs=0
      all=
      while :; do
        out=$(cd build/$VMDIR && env $tenv ARC_NODE_OPFS_DIR="$data" "$ROOT/tools/run_node.sh" 180 $VMJS \
                -cp /system/framework/arc-tests.jar "arcwasm.$cls" $targs 2>&1)
        status=$?
        all="$all$out
"
        runs=$((runs + 1))
        [ "$status" = 75 ] && [ $runs -le 3 ] || break
        echo "$t: the VM asked to be restarted (exit 75), run $((runs + 1))" >&2
      done
      [ -n "$data" ] && rm -rf "$data"
      line=$(printf '%s\n' "$out" | grep -E "^$t: " | tail -1)
      out=$all  # (the failures of every run)
      ;;
    safari|chrome)
      : > "$LOG"
      qenv=
      for e in $tenv; do qenv="$qenv&env=$e"; done
      app=Safari
      [ "$mode" = chrome ] && app="Google Chrome"
      url="http://localhost:$PORT/$WEB/index.html?arg=-cp&arg=/system/framework/arc-tests.jar&arg=arcwasm.$cls${targs:+&arg=$(printf %s "$targs" | sed "s/ /\&arg=/g")}$qenv"
      # BROWSER_SSH="-p 2222 user@host": the browser of another Mac (its
      # localhost:PORT forwarded here: ssh -R PORT:localhost:PORT), e.g. a
      # newer Safari; its tab is closed after each test.
      if [ -n "$BROWSER_SSH" ]; then
        ssh -o BatchMode=yes $BROWSER_SSH "open -a '$app' '$url'"
      else
        open -a "$app" "$url"
      fi
      i=0
      until grep -q "\[$mode\] \(EXIT\|ABORT\)" "$LOG" || [ $i -ge 120 ]; do
        sleep 1; i=$((i + 1))
      done
      line=$(grep "\[$mode\] $t: " "$LOG" | tail -1 | sed "s/^.*\[$mode\] //")
      status=$(grep -o "\[$mode\] EXIT [0-9]*" "$LOG" | awk '{print $3}')
      [ -n "$status" ] || status=124
      out=$(grep '\[FAIL\]\|ABORT' "$LOG")
      if [ -n "$BROWSER_SSH" ] && [ "$app" = Safari ]; then
        ssh -o BatchMode=yes $BROWSER_SSH "osascript -e 'tell application \"Safari\" to close (every tab of every window whose URL contains \"localhost:$PORT\")'" >/dev/null 2>&1
      fi
      ;;
    *) echo "usage: $0 node|safari|chrome [TEST...]" >&2; exit 2 ;;
  esac
  printf '%-18s exit=%-3s %s\n' "$t" "$status" "$line"
  [ "$status" = 0 ] || { bad=$((bad + 1)); printf '%s\n' "$out" | grep '\[FAIL\]\|ABORT' | head -5; }
done
[ "$mode" = safari ] && osascript -e "tell application \"Safari\" to close (every tab of every window whose URL starts with \"http://localhost:$PORT\")" >/dev/null 2>&1
[ "$mode" = chrome ] && osascript -e "tell application \"Google Chrome\" to close (every tab of every window whose URL starts with \"http://localhost:$PORT\")" >/dev/null 2>&1
exit $bad
