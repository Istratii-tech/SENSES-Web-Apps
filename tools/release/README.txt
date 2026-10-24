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

A window shows a QR code and an address like http://192.168.x.y:8499/ (the
setup page, plain http; SENSES itself is on https, port 8500), and a
page with the same opens in your browser. Keep the window open while you use
the apps; close it (or press Ctrl+C) to stop.

2. Once per phone: trust the computer
-------------------------------------
Scan the QR code with the iPhone Camera (or type the address in Safari).
The page walks you through it:
  a. Safari's warning. Safari may open the address over https by itself and
     show "This Connection Is Not Private". That is expected until the
     profile is installed: tap Show Details > visit this website (it is
     your own computer's certificate). Once the profile is installed and
     trusted, the warning goes away.
  b. "Download the profile" > Allow > Close. If the page does not offer the
     profile: scan the second QR code ("The iPhone profile", in the window
     and on the computer's page) or open http://ADDRESS:8499/p.
  c. Settings > Profile Downloaded > Install > passcode > Install > Done.
  d. Settings > General > About > Certificate Trust Settings > turn on
     "SENSES Web Apps CA (your computer)" > Continue.
  e. Back in Safari the page shows "Open SENSES Web Apps".
(What this certificate is: "About the certificate" below.)

Android: the page offers the certificate file instead; install it in
Settings > Security > Encryption & credentials > Install a certificate > CA
certificate.

About the certificate
=====================
* It is made on YOUR computer, by this program, at its first start. There is
  no certificate and no key in the release or in the project's repository.
* The private key stays on this computer only, in your settings folder (see
  "Settings and logs" below), readable only by your account. Neither the
  authors of SENSES Web Apps nor anyone else has access to it; it is never
  sent anywhere.
* Only the public part goes to the phone: the profile, from the setup page on
  your home network.
* It works only for home network addresses (10.x, 172.16-31.x, 192.168.x) and
  .local names. Even if the key were stolen, it could not be used to pass as
  any internet site: the limit is written into the certificate itself.
* It lasts 3 years; the window warns two months ahead.
* A new certificate (lost, leaked, or about to end): stop SENSES and start it
  with --reset-cert, e.g. on macOS in Terminal:
    sh "Start SENSES.command" --reset-cert
  (Windows: "Start SENSES.bat" --reset-cert in a command prompt in the
  folder.) Then remove the old profile on each phone and install the new one
  from the setup page. The apps and their data stay.
* To remove it from an iPhone: Settings > General > VPN & Device Management >
  SENSES Web Apps > Remove Profile.

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
* Safari says "This Connection Is Not Private"? On the setup page (port 8499)
  that is expected before the profile: go on (Show Details > visit this
  website). On SENSES itself (port 8500) it means the phone does not trust the
  computer yet and apps would not work: open the setup page and do step 2. If
  you went on there anyway, SENSES shows a red bar with the link to it.
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
