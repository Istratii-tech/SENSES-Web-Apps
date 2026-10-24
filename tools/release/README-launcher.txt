SENSES Web Apps — Android apps on your iPhone, from your own computer
=====================================================================

(По-русски: README-ru.txt)

This is the launcher: a small download that fetches SENSES Web Apps itself
and keeps it up to date. Your computer runs a small private server; your
iPhone (or any phone or browser) on the same Wi-Fi opens it in Safari and
runs Android apps you install yourself. Nothing goes to the internet except
the updates and what the apps themselves connect to.

You need
--------
* A computer with macOS, Windows 10/11 or Linux, on the same Wi-Fi as the phone
  (a cable to the same router is fine; not a guest network), with internet for
  the first start (about 60 MB for Android 10, 170 MB for all systems).
* Python 3.8 or newer (free): https://www.python.org/downloads/
  On Windows tick "Add python.exe to PATH" in the installer.
* iPhone with iOS 16 or newer (Safari), or Android with Chrome, or a desktop
  browser. 4 GB of free space on the phone is a good start.

1. Start SENSES
---------------
Unpack the zip anywhere and:
* macOS: double-click "Start SENSES.command".
  If macOS says it cannot verify the file: System Settings > Privacy & Security,
  scroll down, "Open Anyway". Or in Terminal:  sh "path/to/Start SENSES.command"
* Windows: double-click "Start SENSES.bat". If SmartScreen warns: "More info" >
  "Run anyway". When Windows asks whether Python may use networks, allow
  "Private networks".
* Linux: ./start-senses.sh

The first time it asks which systems to download: 1 — Android 10 only
(enough for most apps), 2 — all systems (Android 4.4, 6.0.1, 8.1 and 10).
It downloads SENSES Web Apps with a progress bar, checks it and starts it.

A window shows a QR code and an address like http://192.168.x.y:8499/ (the
setup page, plain http; SENSES itself is on https, port 8500), and a
page with the same opens in your browser. Keep the window open while you use
the apps; close it (or press Ctrl+C) to stop.

How it works
------------
* At each start the launcher asks GitHub Releases
  (https://github.com/Istratii-tech/SENSES-Web-Apps/releases) for the latest
  version. When it is newer than the one on this computer, it downloads the
  full release archive (the same SENSES-Web-Apps-VERSION-android10.zip or
  -all.zip that is published there), checks its SHA-256 against the
  published sum, unpacks it into your settings folder (versions/, see
  "Settings and logs") and starts it. The new version brings its own server
  too, so the launcher itself rarely changes.
* The computer downloads, not the phone: the phone gets everything from your
  computer over your Wi-Fi, as before.
* HTTPS is always checked. If this Python cannot check certificates (on macOS
  the python.org Python before "Install Certificates.command" was run), the
  launcher downloads with the system's curl, which uses the system's
  certificates. It never downloads without the check.
* No internet, or GitHub does not answer? The launcher starts the version it
  downloaded last and says so.

Updating
--------
Nothing to do: start SENSES as usual and it updates itself when a new version
is out. It keeps the same certificate, port and name (in your settings
folder), so the phone keeps its apps and their data; the phone updates SENSES
itself the next time you open it.
* The current version and the one before it stay on the computer; older ones
  are removed.
* Back to the previous version: start with --rollback (macOS, in Terminal:
  sh "Start SENSES.command" --rollback; Windows: "Start SENSES.bat" --rollback
  in a command prompt in the folder). Updates then pause; --update resumes
  them (and --rollback again goes forward).
* Other systems: --variant all (Android 4.4, 6.0.1, 8.1 and 10) or
  --variant android10. The choice is kept.
* Start without checking for updates: --no-update.
* A new launcher (rarely needed): download it again and unpack over the old
  folder; your settings and the downloaded versions stay.

Without internet
----------------
After the first start SENSES works without internet (the apps that need the
network need it, of course). For a computer that never has internet, download
the full archive SENSES-Web-Apps-VERSION-android10.zip (or -all.zip) on
another computer, from the Releases page above, and either unpack it and
start its own "Start SENSES", or start the launcher with
--install path/to/SENSES-Web-Apps-VERSION-android10.zip
(with its .sha256 file beside it, the launcher checks it too).

@@include 2. Once per phone: trust the computer | On this computer@@
@@include On this computer | Updating@@
@@include If the phone cannot connect | Settings and logs@@
Settings and logs
-----------------
macOS:   ~/Library/Application Support/SENSES Web Apps/
Windows: %APPDATA%\SENSES Web Apps\
Linux:   ~/.config/senses-web-apps/
(config.json: port, name, the chosen systems and versions; ca.* and
server.*: the certificates — keep ca.key private; versions/: the downloaded
SENSES Web Apps, the current and the previous; logs/phone-log.txt: what the
phone reports.)
ca.key and server.key are readable only by you. Deleting the folder makes a
new certificate (as --reset-cert): phones then need step 2 again.

Options: --variant android10|all, --rollback, --update, --no-update,
--install ZIP, and the server's: --port N, --no-browser, --lang en|ru,
--verbose, --reset-cert (python3 launcher/launcher.py --help).

Licences: SENSES Web Apps itself is under the GNU AGPL v3 (LICENSE.txt); the
components it includes are listed in NOTICE.txt and licenses/ of each version
it downloads. Source: https://github.com/Istratii-tech/SENSES-Web-Apps
