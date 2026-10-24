#!/bin/sh
# Builds the two boot class path jars of topic "activity"
# (docs/topics/activity.md), after framework2.jar as in AOSP 4.4.4's
# BOOTCLASSPATH, the same way tools/build_framework_jar.sh builds the others
# (javac -source/-target 8 against core and the framework -> d8 --min-api 19
# -> jar with one classes.dex):
#
#   build_services_jar.sh OUT_DIR policy FW_CLASSES...
#       android.policy.jar  frameworks/base/policy/src (PhoneWindow,
#                           PhoneLayoutInflater, ...; module android.policy)
#   build_services_jar.sh OUT_DIR services FW_CLASSES...
#       services.jar        the in-process system_server:
#                           framework/java-arcwasm/arcwasm/server, plus the
#                           generated arcwasm.server.base.* (every method of
#                           a binder interface, unimplemented:
#                           tools/gen_binder_base.py)
#
#   build_services_jar.sh OUT_DIR services6 FW_CLASSES...
#       services.jar        the same for Android 6.0.1 on ART (topic fw6-system,
#                           docs/topics/fw6-system.md): framework/java-arcwasm6
#                           (arcwasm.server, arcwasm.input, arcwasm.graphics)
#                           with the bases generated from the 6.0.1 interfaces,
#                           against core-libart.jar, d8 --min-api 23 (no
#                           android.policy.jar: 6.0's PhoneWindow is in
#                           framework.jar)
#
# FW_CLASSES: the javac output jars of the framework and its libraries
# ($(FW_CLASSES) in mk/framework.mk, $(F6_CLASSES) in mk/topic-fw6-core.mk).
# OUT_DIR holds core-java/ (core-libart-java/ for services6) already;
# "services" needs "policy".
set -e
mkdir -p "$1"
OUT=$(cd "$1" && pwd)
what=$2
shift 2
ROOT=$(cd "$(dirname "$0")/.." && pwd)
FWB=$ROOT/third_party/aosp44/frameworks/base
SDK=${ANDROID_SDK:-$HOME/Library/Android/sdk}
D8=${D8:-$(ls -d "$SDK"/build-tools/*/d8 | sort -V | tail -1)}
MIN_API=19
CORE=$OUT/core-java/core-classes.jar
if [ "$what" = services6 ]; then
  MIN_API=23
  CORE=$OUT/core-libart-java/core-libart-classes.jar
elif [ "$what" = services10 ]; then
  # Topic fw10-system: against topic art10's core (CORE10_CLASSES, from
  # mk/topic-fw10-system.mk), dex 039 as the rest of the 10 boot class path.
  MIN_API=29
  CORE=$CORE10_CLASSES
elif [ "$what" = services8 ]; then
  # Topic fw8-system: against topic art8's core (CORE8_CLASSES, exported by
  # mk/topic-fw8-core.mk), dex 038 as the rest of the 8.1 boot class path.
  MIN_API=27
  CORE=$CORE8_CLASSES
fi
[ -f "$CORE" ] || { echo "$CORE missing; build core.jar first" >&2; exit 1; }
FW=
for j in "$@"; do FW=$FW${FW:+:}$(cd "$(dirname "$j")" && pwd)/$(basename "$j"); done

# build NAME CLASSPATH: javac $W/srcs.txt -> $W/NAME-classes.jar -> OUT/NAME.jar.
build() {
  name=$1 cp=$2
  rm -rf "$W/classes" "$W/empty" "$W/dex" "$W/jar"
  mkdir -p "$W/classes" "$W/empty" "$W/dex" "$W/jar"
  javac -J-Xmx2g -source 8 -target 8 -bootclasspath "$CORE" -sourcepath "$W/empty" \
    -cp "$cp" -encoding UTF-8 -nowarn -Xlint:-options -Xmaxerrs 200 \
    -d "$W/classes" @"$W/srcs.txt" > "$W/javac.log" 2>&1 || {
      grep -v '^Note:' "$W/javac.log" >&2; echo "javac failed for $name" >&2; exit 1; }
  grep -v '^Note:' "$W/javac.log" || true
  (cd "$W/classes" && jar cf "$W/$name-classes.jar" .)
  set -- --lib "$CORE"
  old=$IFS; IFS=:
  for l in $cp; do set -- "$@" --lib "$l"; done
  IFS=$old
  "$D8" --release --min-api $MIN_API "$@" --output "$W/dex" "$W/$name-classes.jar"
  [ -f "$W/dex/classes2.dex" ] && { echo "$name: more than one dex" >&2; exit 1; }
  cp "$W/dex/classes.dex" "$W/jar/"
  rm -f "$OUT/$name.jar"
  (cd "$W/jar" && zip -qr "$OUT/$name.jar" .)
  echo "built $OUT/$name.jar ($(wc -c < "$W/dex/classes.dex" | tr -d ' ') bytes of dex)"
}

srcs() { find "$@" -name '*.java' ! -name '.*' | sort | sed 's|.*|"&"|'; }

case $what in
  policy)
    W=$OUT/android.policy-java; rm -rf "$W"; mkdir -p "$W"
    srcs "$FWB/policy/src" > "$W/srcs.txt"
    build android.policy "$FW"
    ;;
  services)
    W=$OUT/services-java; rm -rf "$W"; mkdir -p "$W/gen/arcwasm/server/base"
    G=$W/gen/arcwasm/server/base
    # INTERFACE SUPERCLASS BASE [transact]: the services callers reach
    # through real transactions, and the window stand-in, called directly
    # (its relayout() hands back a Surface, whose Parcel natives need libgui).
    while read -r iface super base mode; do
      python3 "$ROOT/tools/gen_binder_base.py" "$FW" "$iface" "$super" \
        "arcwasm.server.base.$base" "$G/$base.java" $mode
    done <<EOF
android.app.IActivityManager android.app.ActivityManagerNative ActivityManagerBase transact
android.content.pm.IPackageManager android.content.pm.IPackageManager\$Stub PackageManagerBase transact
android.os.IPowerManager android.os.IPowerManager\$Stub PowerManagerBase transact
android.location.ILocationManager android.location.ILocationManager\$Stub LocationManagerBase transact
android.net.IConnectivityManager android.net.IConnectivityManager\$Stub ConnectivityManagerBase transact
android.hardware.display.IDisplayManager android.hardware.display.IDisplayManager\$Stub DisplayManagerBase transact
com.android.internal.view.IInputMethodManager com.android.internal.view.IInputMethodManager\$Stub InputMethodManagerBase transact
android.view.accessibility.IAccessibilityManager android.view.accessibility.IAccessibilityManager\$Stub AccessibilityManagerBase transact
android.content.IContentProvider android.content.ContentProviderNative ContentProviderBase transact
android.view.IWindowManager android.view.IWindowManager\$Stub WindowManagerBase
android.view.IWindowSession android.view.IWindowSession\$Stub WindowSessionBase
android.media.IAudioService android.media.IAudioService\$Stub AudioServiceBase
android.os.storage.IMountService android.os.storage.IMountService\$Stub MountServiceBase transact
android.content.IContentService android.content.IContentService\$Stub ContentServiceBase transact
android.accounts.IAccountManager android.accounts.IAccountManager\$Stub AccountManagerBase transact
android.os.INetworkManagementService android.os.INetworkManagementService\$Stub NetworkManagementBase transact
android.os.IUserManager android.os.IUserManager\$Stub UserManagerBase transact
android.net.wifi.IWifiManager android.net.wifi.IWifiManager\$Stub WifiManagerBase transact
com.android.internal.appwidget.IAppWidgetService com.android.internal.appwidget.IAppWidgetService\$Stub AppWidgetServiceBase transact
android.app.IWallpaperManager android.app.IWallpaperManager\$Stub WallpaperManagerBase transact
android.app.ISearchManager android.app.ISearchManager\$Stub SearchManagerBase transact
com.android.internal.statusbar.IStatusBarService com.android.internal.statusbar.IStatusBarService\$Stub StatusBarManagerBase transact
android.app.INotificationManager android.app.INotificationManager\$Stub NotificationManagerBase transact
com.android.internal.app.IAppOpsService com.android.internal.app.IAppOpsService\$Stub AppOpsBase transact
android.media.IMediaRouterService android.media.IMediaRouterService\$Stub MediaRouterBase transact
android.app.admin.IDevicePolicyManager android.app.admin.IDevicePolicyManager\$Stub DevicePolicyBase transact
com.android.internal.app.IBatteryStats com.android.internal.app.IBatteryStats\$Stub BatteryStatsBase transact
com.android.internal.app.IProcessStats com.android.internal.app.IProcessStats\$Stub ProcessStatsBase transact
android.security.IKeystoreService android.security.IKeystoreService\$Stub KeystoreBase transact
android.os.IVibratorService android.os.IVibratorService\$Stub VibratorServiceBase transact
com.android.internal.app.IUsageStats com.android.internal.app.IUsageStats\$Stub UsageStatsBase transact
com.android.internal.telephony.ITelephony com.android.internal.telephony.ITelephony\$Stub TelephonyBase transact
com.android.internal.telephony.IPhoneSubInfo com.android.internal.telephony.IPhoneSubInfo\$Stub PhoneSubInfoBase transact
com.android.internal.telephony.ISms com.android.internal.telephony.ISms\$Stub SmsBase transact
com.android.internal.telephony.ITelephonyRegistry com.android.internal.telephony.ITelephonyRegistry\$Stub TelephonyRegistryBase transact
com.android.internal.widget.ILockSettings com.android.internal.widget.ILockSettings\$Stub LockSettingsBase transact
android.hardware.usb.IUsbManager android.hardware.usb.IUsbManager\$Stub UsbManagerBase transact
android.hardware.ISerialManager android.hardware.ISerialManager\$Stub SerialManagerBase transact
android.net.nsd.INsdManager android.net.nsd.INsdManager\$Stub NsdManagerBase transact
android.net.INetworkPolicyManager android.net.INetworkPolicyManager\$Stub NetworkPolicyBase transact
android.net.INetworkStatsService android.net.INetworkStatsService\$Stub NetworkStatsBase transact
android.service.dreams.IDreamManager android.service.dreams.IDreamManager\$Stub DreamManagerBase transact
com.android.internal.os.IDropBoxManagerService com.android.internal.os.IDropBoxManagerService\$Stub DropBoxBase transact
android.os.IUpdateLock android.os.IUpdateLock\$Stub UpdateLockBase transact
android.print.IPrintManager android.print.IPrintManager\$Stub PrintManagerBase transact
android.hardware.IConsumerIrService android.hardware.IConsumerIrService\$Stub ConsumerIrBase transact
android.app.backup.IBackupManager android.app.backup.IBackupManager\$Stub BackupManagerBase transact
android.location.ICountryDetector android.location.ICountryDetector\$Stub CountryDetectorBase transact
com.android.internal.textservice.ITextServicesManager com.android.internal.textservice.ITextServicesManager\$Stub TextServicesManagerBase transact
android.app.IAlarmManager android.app.IAlarmManager\$Stub AlarmManagerBase transact
android.content.IClipboard android.content.IClipboard\$Stub ClipboardBase transact
android.app.IUiModeManager android.app.IUiModeManager\$Stub UiModeManagerBase transact
android.bluetooth.IBluetoothManager android.bluetooth.IBluetoothManager\$Stub BluetoothManagerBase transact
EOF
    # The stub WebView provider's settings, cookie manager and databases
    # (arcwasm/webview/StubWebViewFactory.java).
    python3 "$ROOT/tools/gen_webview_stub.py" \
      "$ROOT/third_party/aosp44/frameworks/base/core/java/android/webkit" \
      "$W/gen/arcwasm/webview" arcwasm.webview WebSettings:StubWebSettings \
      CookieManager:StubCookieManagerBase WebIconDatabase:StubWebIconDatabase \
      WebViewDatabase:StubWebViewDatabase WebBackForwardList:StubBackForwardList
    srcs "$ROOT/framework/java-arcwasm/arcwasm/server" \
         "$ROOT/framework/java-arcwasm/arcwasm/webview" "$W/gen" > "$W/srcs.txt"
    build services "$FW:$OUT/android.policy-java/android.policy-classes.jar"
    ;;
  services6)
    W=$OUT/services-java; rm -rf "$W"; mkdir -p "$W/gen/arcwasm/server/base"
    G=$W/gen/arcwasm/server/base
    # As for "services": transact = callers get the Proxy (real
    # transactions); the window manager and its sessions are called
    # directly (relayout's out Surface is the caller's own object).
    while read -r iface super base mode; do
      python3 "$ROOT/tools/gen_binder_base.py" "$FW" "$iface" "$super" \
        "arcwasm.server.base.$base" "$G/$base.java" $mode
    done <<EOF6
android.app.IActivityManager android.app.ActivityManagerNative ActivityManagerBase transact
android.content.pm.IPackageManager android.content.pm.IPackageManager\$Stub PackageManagerBase transact
android.os.IPowerManager android.os.IPowerManager\$Stub PowerManagerBase transact
android.location.ILocationManager android.location.ILocationManager\$Stub LocationManagerBase transact
android.net.IConnectivityManager android.net.IConnectivityManager\$Stub ConnectivityManagerBase transact
android.hardware.display.IDisplayManager android.hardware.display.IDisplayManager\$Stub DisplayManagerBase transact
com.android.internal.view.IInputMethodManager com.android.internal.view.IInputMethodManager\$Stub InputMethodManagerBase transact
android.view.accessibility.IAccessibilityManager android.view.accessibility.IAccessibilityManager\$Stub AccessibilityManagerBase transact
android.content.IContentProvider android.content.ContentProviderNative ContentProviderBase transact
android.media.IAudioService android.media.IAudioService\$Stub AudioServiceBase transact
com.android.internal.app.IAppOpsService com.android.internal.app.IAppOpsService\$Stub AppOpsBase transact
android.app.INotificationManager android.app.INotificationManager\$Stub NotificationManagerBase transact
android.app.IAlarmManager android.app.IAlarmManager\$Stub AlarmManagerBase transact
android.os.IUserManager android.os.IUserManager\$Stub UserManagerBase transact
android.content.IClipboard android.content.IClipboard\$Stub ClipboardBase transact
android.app.usage.IUsageStatsManager android.app.usage.IUsageStatsManager\$Stub UsageStatsManagerBase transact
android.app.job.IJobScheduler android.app.job.IJobScheduler\$Stub JobSchedulerBase transact
android.os.storage.IMountService android.os.storage.IMountService\$Stub MountServiceBase transact
com.android.internal.textservice.ITextServicesManager com.android.internal.textservice.ITextServicesManager\$Stub TextServicesManagerBase transact
android.content.IContentService android.content.IContentService\$Stub ContentServiceBase transact
android.os.IVibratorService android.os.IVibratorService\$Stub VibratorServiceBase transact
android.os.IDeviceIdleController android.os.IDeviceIdleController\$Stub DeviceIdleControllerBase transact
com.android.internal.appwidget.IAppWidgetService com.android.internal.appwidget.IAppWidgetService\$Stub AppWidgetServiceBase transact
android.app.IWallpaperManager android.app.IWallpaperManager\$Stub WallpaperManagerBase transact
android.content.pm.ILauncherApps android.content.pm.ILauncherApps\$Stub LauncherAppsBase transact
com.android.internal.statusbar.IStatusBarService com.android.internal.statusbar.IStatusBarService\$Stub StatusBarManagerBase transact
android.app.ISearchManager android.app.ISearchManager\$Stub SearchManagerBase transact
android.accounts.IAccountManager android.accounts.IAccountManager\$Stub AccountManagerBase transact
android.content.pm.IPackageInstaller android.content.pm.IPackageInstaller\$Stub PackageInstallerBase transact
android.media.IMediaRouterService android.media.IMediaRouterService\$Stub MediaRouterBase transact
android.app.trust.ITrustManager android.app.trust.ITrustManager\$Stub TrustManagerBase transact
com.android.internal.widget.ILockSettings com.android.internal.widget.ILockSettings\$Stub LockSettingsBase transact
android.service.dreams.IDreamManager android.service.dreams.IDreamManager\$Stub DreamManagerBase transact
com.android.internal.telephony.ITelephonyRegistry com.android.internal.telephony.ITelephonyRegistry\$Stub TelephonyRegistryBase transact
android.media.session.ISessionManager android.media.session.ISessionManager\$Stub SessionManagerBase transact
android.net.wifi.IWifiManager android.net.wifi.IWifiManager\$Stub WifiManagerBase transact
android.net.INetworkPolicyManager android.net.INetworkPolicyManager\$Stub NetworkPolicyBase transact
android.media.projection.IMediaProjectionManager android.media.projection.IMediaProjectionManager\$Stub MediaProjectionBase transact
android.app.admin.IDevicePolicyManager android.app.admin.IDevicePolicyManager\$Stub DevicePolicyBase transact
android.os.INetworkManagementService android.os.INetworkManagementService\$Stub NetworkManagementBase transact
android.net.INetworkStatsService android.net.INetworkStatsService\$Stub NetworkStatsBase transact
android.hardware.usb.IUsbManager android.hardware.usb.IUsbManager\$Stub UsbManagerBase transact
android.location.ICountryDetector android.location.ICountryDetector\$Stub CountryDetectorBase transact
android.app.backup.IBackupManager android.app.backup.IBackupManager\$Stub BackupManagerBase transact
android.hardware.fingerprint.IFingerprintService android.hardware.fingerprint.IFingerprintService\$Stub FingerprintBase transact
android.net.nsd.INsdManager android.net.nsd.INsdManager\$Stub NsdManagerBase transact
com.android.internal.telephony.ISub com.android.internal.telephony.ISub\$Stub SubBase transact
com.android.internal.telephony.ITelephony com.android.internal.telephony.ITelephony\$Stub TelephonyBase transact
com.android.internal.telephony.IPhoneSubInfo com.android.internal.telephony.IPhoneSubInfo\$Stub PhoneSubInfoBase transact
com.android.internal.telephony.ICarrierConfigLoader com.android.internal.telephony.ICarrierConfigLoader\$Stub CarrierConfigBase transact
android.print.IPrintManager android.print.IPrintManager\$Stub PrintManagerBase transact
com.android.internal.app.IBatteryStats com.android.internal.app.IBatteryStats\$Stub BatteryStatsBase transact
com.android.internal.app.IProcessStats com.android.internal.app.IProcessStats\$Stub ProcessStatsBase transact
android.security.IKeystoreService android.security.IKeystoreService\$Stub KeystoreBase transact
android.content.IRestrictionsManager android.content.IRestrictionsManager\$Stub RestrictionsManagerBase transact
android.service.persistentdata.IPersistentDataBlockService android.service.persistentdata.IPersistentDataBlockService\$Stub PersistentDataBlockBase transact
com.android.internal.app.IVoiceInteractionManagerService com.android.internal.app.IVoiceInteractionManagerService\$Stub VoiceInteractionBase transact
android.hardware.IConsumerIrService android.hardware.IConsumerIrService\$Stub ConsumerIrBase transact
android.hardware.ISerialManager android.hardware.ISerialManager\$Stub SerialManagerBase transact
android.net.IEthernetManager android.net.IEthernetManager\$Stub EthernetManagerBase transact
android.media.midi.IMidiManager android.media.midi.IMidiManager\$Stub MidiManagerBase transact
com.android.internal.os.IDropBoxManagerService com.android.internal.os.IDropBoxManagerService\$Stub DropBoxBase transact
android.os.IBatteryPropertiesRegistrar android.os.IBatteryPropertiesRegistrar\$Stub BatteryPropertiesBase transact
android.net.wifi.p2p.IWifiP2pManager android.net.wifi.p2p.IWifiP2pManager\$Stub WifiP2pBase transact
android.net.INetworkScoreService android.net.INetworkScoreService\$Stub NetworkScoreBase transact
android.hardware.hdmi.IHdmiControlService android.hardware.hdmi.IHdmiControlService\$Stub HdmiControlBase transact
android.os.IUpdateLock android.os.IUpdateLock\$Stub UpdateLockBase transact
android.app.IUiModeManager android.app.IUiModeManager\$Stub UiModeManagerBase transact
android.view.IWindowManager android.view.IWindowManager\$Stub WindowManagerBase
android.view.IWindowSession android.view.IWindowSession\$Stub WindowSessionBase
EOF6
    # The stub WebView provider's settings, cookie manager and databases
    # (arcwasm/webview/StubWebViewFactory.java).
    python3 "$ROOT/tools/gen_webview_stub.py" \
      "$ROOT/third_party/aosp60/frameworks/base/core/java/android/webkit" \
      "$W/gen/arcwasm/webview" arcwasm.webview WebSettings:StubWebSettings \
      CookieManager:StubCookieManagerBase WebIconDatabase:StubWebIconDatabase \
      WebViewDatabase:StubWebViewDatabase WebBackForwardList:StubBackForwardList
    # Shared with the 4.4 build: Unimplemented, Trace, AudioStats, and the
    # HOME/APP_SWITCH keys, hang watchdog, crash recovery and UI exerciser of
    # topic userspace.
    srcs "$ROOT/framework/java-arcwasm6/arcwasm" "$W/gen" > "$W/srcs.txt"
    srcs "$ROOT/framework/java-arcwasm/arcwasm/server/Unimplemented.java" \
         "$ROOT/framework/java-arcwasm/arcwasm/server/Trace.java" \
         "$ROOT/framework/java-arcwasm/arcwasm/audio/AudioStats.java" \
         "$ROOT/framework/java-arcwasm/arcwasm/server/SystemKeys.java" \
         "$ROOT/framework/java-arcwasm/arcwasm/server/HangWatchdog.java" \
         "$ROOT/framework/java-arcwasm/arcwasm/server/CrashRecovery.java" \
         "$ROOT/framework/java-arcwasm/arcwasm/server/UserDictionaryProvider.java" \
         "$ROOT/framework/java-arcwasm/arcwasm/server/ProcessStatsService.java" \
         "$ROOT/framework/java-arcwasm/arcwasm/server/Monkey.java" \
         "$ROOT/framework/java-arcwasm/arcwasm/server/AppLibrary.java" \
         "$ROOT/framework/java-arcwasm/arcwasm/server/Senses.java" \
         "$ROOT/framework/java-arcwasm/arcwasm/server/Gms.java" >> "$W/srcs.txt"
    build services "$FW"
    ;;
  services8)
    W=$OUT/services-java; rm -rf "$W"; mkdir -p "$W/gen/arcwasm/server/base"
    G=$W/gen/arcwasm/server/base
    # As for "services6", on the 8.1 interfaces (IActivityManager is AIDL
    # in 8.1, IMountService became IStorageManager) and the services API 25-27
    # apps ask for besides (shortcut, storagestats, autofill, trust for
    # KeyguardManager, uimode, appwidget, account, wifi, battery, dropbox).
    while read -r iface super base mode; do
      python3 "$ROOT/tools/gen_binder_base.py" "$FW" "$iface" "$super" \
        "arcwasm.server.base.$base" "$G/$base.java" $mode
    done <<EOF8
android.app.IActivityManager android.app.IActivityManager\$Stub ActivityManagerBase transact
android.content.pm.IPackageManager android.content.pm.IPackageManager\$Stub PackageManagerBase transact
android.content.pm.IPackageInstaller android.content.pm.IPackageInstaller\$Stub PackageInstallerBase transact
android.app.ISearchManager android.app.ISearchManager\$Stub SearchManagerBase transact
android.content.IRestrictionsManager android.content.IRestrictionsManager\$Stub RestrictionsManagerBase transact
android.os.IPowerManager android.os.IPowerManager\$Stub PowerManagerBase transact
android.location.ILocationManager android.location.ILocationManager\$Stub LocationManagerBase transact
android.net.IConnectivityManager android.net.IConnectivityManager\$Stub ConnectivityManagerBase transact
android.hardware.display.IDisplayManager android.hardware.display.IDisplayManager\$Stub DisplayManagerBase transact
com.android.internal.view.IInputMethodManager com.android.internal.view.IInputMethodManager\$Stub InputMethodManagerBase transact
android.view.accessibility.IAccessibilityManager android.view.accessibility.IAccessibilityManager\$Stub AccessibilityManagerBase transact
android.content.IContentProvider android.content.ContentProviderNative ContentProviderBase transact
android.media.IAudioService android.media.IAudioService\$Stub AudioServiceBase transact
com.android.internal.app.IAppOpsService com.android.internal.app.IAppOpsService\$Stub AppOpsBase transact
android.app.INotificationManager android.app.INotificationManager\$Stub NotificationManagerBase transact
android.app.IAlarmManager android.app.IAlarmManager\$Stub AlarmManagerBase transact
android.os.IUserManager android.os.IUserManager\$Stub UserManagerBase transact
android.content.IClipboard android.content.IClipboard\$Stub ClipboardBase transact
android.app.usage.IUsageStatsManager android.app.usage.IUsageStatsManager\$Stub UsageStatsManagerBase transact
android.app.job.IJobScheduler android.app.job.IJobScheduler\$Stub JobSchedulerBase transact
android.os.storage.IStorageManager android.os.storage.IStorageManager\$Stub StorageManagerBase transact
com.android.internal.textservice.ITextServicesManager com.android.internal.textservice.ITextServicesManager\$Stub TextServicesManagerBase transact
android.content.IContentService android.content.IContentService\$Stub ContentServiceBase transact
android.os.IVibratorService android.os.IVibratorService\$Stub VibratorServiceBase transact
android.os.IDeviceIdleController android.os.IDeviceIdleController\$Stub DeviceIdleControllerBase transact
android.content.pm.IShortcutService android.content.pm.IShortcutService\$Stub ShortcutServiceBase transact
android.app.usage.IStorageStatsManager android.app.usage.IStorageStatsManager\$Stub StorageStatsManagerBase transact
android.view.autofill.IAutoFillManager android.view.autofill.IAutoFillManager\$Stub AutoFillManagerBase transact
android.app.trust.ITrustManager android.app.trust.ITrustManager\$Stub TrustManagerBase transact
android.app.IUiModeManager android.app.IUiModeManager\$Stub UiModeManagerBase transact
android.app.IWallpaperManager android.app.IWallpaperManager\$Stub WallpaperManagerBase transact
android.media.IMediaRouterService android.media.IMediaRouterService\$Stub MediaRouterServiceBase transact
android.media.midi.IMidiManager android.media.midi.IMidiManager\$Stub MidiManagerBase transact
android.hardware.usb.IUsbManager android.hardware.usb.IUsbManager\$Stub UsbManagerBase transact
com.android.internal.appwidget.IAppWidgetService com.android.internal.appwidget.IAppWidgetService\$Stub AppWidgetServiceBase transact
android.accounts.IAccountManager android.accounts.IAccountManager\$Stub AccountManagerBase transact
android.net.wifi.IWifiManager android.net.wifi.IWifiManager\$Stub WifiManagerBase transact
com.android.internal.app.IBatteryStats com.android.internal.app.IBatteryStats\$Stub BatteryStatsBase transact
android.os.IBatteryPropertiesRegistrar android.os.IBatteryPropertiesRegistrar\$Stub BatteryPropertiesBase transact
com.android.internal.os.IDropBoxManagerService com.android.internal.os.IDropBoxManagerService\$Stub DropBoxManagerBase transact
android.media.session.ISessionManager android.media.session.ISessionManager\$Stub SessionManagerBase transact
android.media.session.ISession android.media.session.ISession\$Stub SessionBase transact
android.media.session.ISessionController android.media.session.ISessionController\$Stub SessionControllerBase transact
android.net.INetworkPolicyManager android.net.INetworkPolicyManager\$Stub NetworkPolicyManagerBase transact
android.view.IWindowManager android.view.IWindowManager\$Stub WindowManagerBase
android.view.IWindowSession android.view.IWindowSession\$Stub WindowSessionBase
android.hardware.input.IInputManager android.hardware.input.IInputManager\$Stub InputManagerBase
EOF8
    python3 "$ROOT/tools/gen_webview_stub.py" \
      "$ROOT/third_party/aosp81/frameworks/base/core/java/android/webkit" \
      "$W/gen/arcwasm/webview" arcwasm.webview WebSettings:StubWebSettings \
      CookieManager:StubCookieManagerBase WebIconDatabase:StubWebIconDatabase \
      WebViewDatabase:StubWebViewDatabase WebBackForwardList:StubBackForwardList
    srcs "$ROOT/framework/java-arcwasm8/arcwasm" "$W/gen" > "$W/srcs.txt"
    # Shared with 4.4 and 6.0.1 besides: the HOME key, SENSES Web Apps'
    # library and one-app mode (topic install8).
    srcs "$ROOT/framework/java-arcwasm/arcwasm/server/Unimplemented.java" \
         "$ROOT/framework/java-arcwasm/arcwasm/server/Trace.java" \
         "$ROOT/framework/java-arcwasm/arcwasm/audio/AudioStats.java" \
         "$ROOT/framework/java-arcwasm/arcwasm/server/SystemKeys.java" \
         "$ROOT/framework/java-arcwasm/arcwasm/server/AppLibrary.java" \
         "$ROOT/framework/java-arcwasm/arcwasm/server/Senses.java" \
         "$ROOT/framework/java-arcwasm/arcwasm/server/Gms.java" \
         "$ROOT/framework/java-multiproc/arcwasm/server/LightProcess.java" >> "$W/srcs.txt"
    build services "$FW"
    ;;
  services10)
    W=$OUT/services-java; rm -rf "$W"; mkdir -p "$W/gen/arcwasm/server/base"
    G=$W/gen/arcwasm/server/base
    # As for "services8", on the 10 interfaces: activities moved to
    # IActivityTaskManager, and what API 28-29 apps ask for besides (role,
    # URI grants, thermal, cross-profile apps, launcher apps).
    while read -r iface super base mode; do
      python3 "$ROOT/tools/gen_binder_base.py" "$FW" "$iface" "$super" \
        "arcwasm.server.base.$base" "$G/$base.java" $mode
    done <<EOF10
android.app.IActivityManager android.app.IActivityManager\$Stub ActivityManagerBase transact
android.app.IActivityTaskManager android.app.IActivityTaskManager\$Stub ActivityTaskManagerBase transact
android.content.pm.IPackageManager android.content.pm.IPackageManager\$Stub PackageManagerBase transact
android.content.pm.IPackageInstaller android.content.pm.IPackageInstaller\$Stub PackageInstallerBase transact
android.app.ISearchManager android.app.ISearchManager\$Stub SearchManagerBase transact
android.content.IRestrictionsManager android.content.IRestrictionsManager\$Stub RestrictionsManagerBase transact
android.os.IPowerManager android.os.IPowerManager\$Stub PowerManagerBase transact
android.location.ILocationManager android.location.ILocationManager\$Stub LocationManagerBase transact
android.net.IConnectivityManager android.net.IConnectivityManager\$Stub ConnectivityManagerBase transact
android.hardware.display.IDisplayManager android.hardware.display.IDisplayManager\$Stub DisplayManagerBase transact
com.android.internal.view.IInputMethodManager com.android.internal.view.IInputMethodManager\$Stub InputMethodManagerBase transact
android.view.accessibility.IAccessibilityManager android.view.accessibility.IAccessibilityManager\$Stub AccessibilityManagerBase transact
android.content.IContentProvider android.content.ContentProviderNative ContentProviderBase transact
android.media.IAudioService android.media.IAudioService\$Stub AudioServiceBase transact
com.android.internal.app.IAppOpsService com.android.internal.app.IAppOpsService\$Stub AppOpsBase transact
android.app.INotificationManager android.app.INotificationManager\$Stub NotificationManagerBase transact
android.app.IAlarmManager android.app.IAlarmManager\$Stub AlarmManagerBase transact
android.os.IUserManager android.os.IUserManager\$Stub UserManagerBase transact
android.content.IClipboard android.content.IClipboard\$Stub ClipboardBase transact
android.app.usage.IUsageStatsManager android.app.usage.IUsageStatsManager\$Stub UsageStatsManagerBase transact
android.app.job.IJobScheduler android.app.job.IJobScheduler\$Stub JobSchedulerBase transact
android.os.storage.IStorageManager android.os.storage.IStorageManager\$Stub StorageManagerBase transact
com.android.internal.textservice.ITextServicesManager com.android.internal.textservice.ITextServicesManager\$Stub TextServicesManagerBase transact
android.content.IContentService android.content.IContentService\$Stub ContentServiceBase transact
android.os.IVibratorService android.os.IVibratorService\$Stub VibratorServiceBase transact
android.os.IDeviceIdleController android.os.IDeviceIdleController\$Stub DeviceIdleControllerBase transact
android.content.pm.IShortcutService android.content.pm.IShortcutService\$Stub ShortcutServiceBase transact
android.app.usage.IStorageStatsManager android.app.usage.IStorageStatsManager\$Stub StorageStatsManagerBase transact
android.view.autofill.IAutoFillManager android.view.autofill.IAutoFillManager\$Stub AutoFillManagerBase transact
android.app.trust.ITrustManager android.app.trust.ITrustManager\$Stub TrustManagerBase transact
android.app.IUiModeManager android.app.IUiModeManager\$Stub UiModeManagerBase transact
android.app.IWallpaperManager android.app.IWallpaperManager\$Stub WallpaperManagerBase transact
android.media.IMediaRouterService android.media.IMediaRouterService\$Stub MediaRouterServiceBase transact
android.media.midi.IMidiManager android.media.midi.IMidiManager\$Stub MidiManagerBase transact
android.hardware.usb.IUsbManager android.hardware.usb.IUsbManager\$Stub UsbManagerBase transact
com.android.internal.appwidget.IAppWidgetService com.android.internal.appwidget.IAppWidgetService\$Stub AppWidgetServiceBase transact
android.accounts.IAccountManager android.accounts.IAccountManager\$Stub AccountManagerBase transact
android.net.wifi.IWifiManager android.net.wifi.IWifiManager\$Stub WifiManagerBase transact
com.android.internal.app.IBatteryStats com.android.internal.app.IBatteryStats\$Stub BatteryStatsBase transact
android.os.IBatteryPropertiesRegistrar android.os.IBatteryPropertiesRegistrar\$Stub BatteryPropertiesBase transact
com.android.internal.os.IDropBoxManagerService com.android.internal.os.IDropBoxManagerService\$Stub DropBoxManagerBase transact
android.media.session.ISessionManager android.media.session.ISessionManager\$Stub SessionManagerBase transact
android.media.session.ISession android.media.session.ISession\$Stub SessionBase transact
android.media.session.ISessionController android.media.session.ISessionController\$Stub SessionControllerBase transact
android.net.INetworkPolicyManager android.net.INetworkPolicyManager\$Stub NetworkPolicyManagerBase transact
android.app.role.IRoleManager android.app.role.IRoleManager\$Stub RoleManagerBase transact
android.app.IUriGrantsManager android.app.IUriGrantsManager\$Stub UriGrantsManagerBase transact
android.os.IThermalService android.os.IThermalService\$Stub ThermalServiceBase transact
android.content.pm.ICrossProfileApps android.content.pm.ICrossProfileApps\$Stub CrossProfileAppsBase transact
android.content.pm.ILauncherApps android.content.pm.ILauncherApps\$Stub LauncherAppsBase transact
android.view.IWindowManager android.view.IWindowManager\$Stub WindowManagerBase
android.view.IWindowSession android.view.IWindowSession\$Stub WindowSessionBase
EOF10
    python3 "$ROOT/tools/gen_webview_stub.py" \
      "$ROOT/third_party/aosp10/frameworks/base/core/java/android/webkit" \
      "$W/gen/arcwasm/webview" arcwasm.webview WebSettings:StubWebSettings \
      CookieManager:StubCookieManagerBase WebIconDatabase:StubWebIconDatabase \
      WebViewDatabase:StubWebViewDatabase WebBackForwardList:StubBackForwardList
    srcs "$ROOT/framework/java-arcwasm10/arcwasm" "$W/gen" > "$W/srcs.txt"
    # Shared with 4.4, 6.0.1 and 8.1 besides: SENSES Web Apps' library and
    # one-app mode (topic install8). SystemKeys has its own 10 copy (input
    # through arcwasm.server.Input).
    srcs "$ROOT/framework/java-arcwasm/arcwasm/server/Unimplemented.java" \
         "$ROOT/framework/java-arcwasm/arcwasm/server/Trace.java" \
         "$ROOT/framework/java-arcwasm/arcwasm/audio/AudioStats.java" \
         "$ROOT/framework/java-arcwasm/arcwasm/server/AppLibrary.java" \
         "$ROOT/framework/java-arcwasm/arcwasm/server/Senses.java" \
         "$ROOT/framework/java-arcwasm/arcwasm/server/Gms.java" \
         "$ROOT/framework/java-multiproc/arcwasm/server/LightProcess.java" >> "$W/srcs.txt"
    build services "$FW"
    ;;
  *) echo "usage: $0 OUT_DIR policy|services|services6|services8|services10 FW_CLASSES..." >&2; exit 2 ;;
esac
