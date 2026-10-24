SENSES Web Apps — Android apps on your iPhone, from your own computer
=====================================================================

(По-русски: README-ru.txt)

Your computer runs a small private server; your iPhone (or any phone or
browser) on the same Wi-Fi opens it in Safari and runs Android apps you
install yourself. Nothing goes to the internet except what the apps
themselves connect to.

You need
--------
* A computer with macOS, Windows 10/11 or Linux, on the same Wi-Fi as the phone
  (a cable to the same router is fine; not a guest network).
* Python 3.8 or newer (free): https://www.python.org/downloads/
  On Windows tick "Add python.exe to PATH" in the installer.
* iPhone with iOS 16 or newer (Safari), or Android with Chrome, or a desktop
  browser. 4 GB of free space on the phone is a good start.

1. Start the server
-------------------
Unpack the zip anywhere and:
* macOS: double-click "Start SENSES.command".
  If macOS says it cannot verify the file: System Settings > Privacy & Security,
  scroll down, "Open Anyway". Or in Terminal:  sh "path/to/Start SENSES.command"
* Windows: double-click "Start SENSES.bat". If SmartScreen warns: "More info" >
  "Run anyway". When Windows asks whether Python may use networks, allow
  "Private networks".
* Linux: ./start-senses.sh

A window shows a QR code and an address like http://192.168.x.y:8500/, and a
page with the same opens in your browser. Keep the window open while you use
the apps; close it (or press Ctrl+C) to stop.

2. Once per phone: trust the computer
-------------------------------------
Scan the QR code with the iPhone Camera (or type the address in Safari).
The page walks you through it:
  a. "Download the profile" > Allow > Close.
  b. Settings > Profile Downloaded > Install > passcode > Install > Done.
  c. Settings > General > About > Certificate Trust Settings > turn on
     "SENSES Web Apps CA (your computer)" > Continue.
  d. Back in Safari the page shows "Open SENSES Web Apps".
The certificate was made on your computer at the first start; it never leaves
it except as this profile, and no one else has its key. It is valid only for
home network addresses (10.x, 172.16-31.x, 192.168.x) and .local names, never
for internet sites, and lasts 3 years (the window warns two months ahead).
To remove it from the iPhone: Settings > General > VPN & Device Management >
SENSES Web Apps > Remove Profile.

A new certificate (lost, leaked, or about to end): stop SENSES and start it
with --reset-cert, e.g. on macOS in Terminal:
  sh "Start SENSES.command" --reset-cert
(Windows: "Start SENSES.bat" --reset-cert in a command prompt in the folder.)
Then remove the old profile on each phone as above and install the new one
from the page. The apps and their data stay.

Android: the page offers the certificate file instead; install it in
Settings > Security > Encryption & credentials > Install a certificate > CA
certificate.

3. Apps
-------
In SENSES tap "Install app" (Установить приложение) and pick an .apk (or
.apks/.xapk/.apkm) file you have on the phone (Files). Tip: in Safari, Share >
Add to Home Screen gives SENSES its own full-screen icon.
No apps and nothing from Google come with SENSES; Google services (microG or
Google's own) can be added from your own files in the menu.

On this computer
----------------
Open http://localhost:8500/ (the address the window shows) in Chrome, Edge,
Safari or Firefox: it works there without any certificate.

Updating
--------
Unpack the new release (anywhere, even over the old folder) and start it the
same way. It keeps the same certificate, port and name (kept in your user
settings folder, see below), so the phone keeps its apps and their data; the
phone updates SENSES itself the next time you open it.

If the phone cannot connect
---------------------------
* Same Wi-Fi? Guest networks and "client isolation" keep devices apart.
* Firewall: allow Python (macOS: System Settings > Network > Firewall >
  Options; Windows: the network must be "Private").
* A VPN on the phone or the computer can hide the home network.
* The window prints "Connected: <phone address>" when the phone reaches it.
* The computer must stay awake (SENSES keeps it from sleeping while it runs;
  closing a laptop's lid still sends it to sleep).
* Tip: reserve the computer's address in the router, or use the
  https://senses-NAME.local:8500/ address the page offers: the apps live at
  that address on the phone.

Settings and logs
-----------------
macOS:   ~/Library/Application Support/SENSES Web Apps/
Windows: %APPDATA%\SENSES Web Apps\
Linux:   ~/.config/senses-web-apps/
(config.json: port and name; ca.* and server.*: the certificates — keep
ca.key private; logs/phone-log.txt: what the phone reports.)
ca.key and server.key are readable only by you. Deleting the folder makes a
new certificate (as --reset-cert): phones then need step 2 again.

Options: --port N, --no-browser, --lang en|ru, --verbose (python3 server/senses.py --help).

Licences: SENSES Web Apps itself is under the GNU AGPL v3 (LICENSE.txt); the components
it includes — NOTICE.txt and licenses/. Source: https://github.com/Istratii-tech/SENSES-Web-Apps
