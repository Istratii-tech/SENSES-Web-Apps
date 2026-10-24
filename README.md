# SENSES Web Apps

[![Telegram](https://img.shields.io/badge/Telegram-2CA5E0?style=for-the-badge&logo=telegram&logoColor=white)](https://t.me/istratii_tech ) [![Discord](https://img.shields.io/badge/Discord-5865F2?style=for-the-badge&logo=discord&logoColor=white)](https://discord.gg/5HWuRWdKwt)

**Android apps in a web browser** — on an iPhone (iOS 16 and newer, Safari), on
Android and on any desktop browser. Each app runs on an Android system of its own
(Android 10 by default; 8.1, 6.0.1 and 4.4 too), compiled to WebAssembly: the
Android framework and ART/Dalvik from the Android Open Source Project, ARC's
`posix_translation` as the kernel, ARM and arm64 native code translated to wasm,
OpenGL ES on WebGL 2, Vulkan on WebGPU. A small server on your own computer
serves it to your phone over your Wi-Fi and gives the apps the network.

MADE WITH CLAUDE CODE

Repository: `https://github.com/Istratii-tech/SENSES-Web-Apps`.
Requests and questions: issues of this repository or `https://t.me/istratii_tech`.

## Instructions

Инструкция: как пользоваться SENSES Web Apps

1. Скачайте архив выпуска (Releases этого репозитория): `…-android10.zip` (Android 10, ~58 МБ) или
   `…-all.zip` (все системы, ~170 МБ) — и распакуйте его на компьютере (macOS, Windows или Linux).
2. Нужен Python 3.8 или новее (https://www.python.org/downloads/; в Windows отметьте «Add python.exe to PATH»).
3. Запустите «Start SENSES.command» (macOS), «Start SENSES.bat» (Windows) или `./start-senses.sh` (Linux).
   Появится окно с QR-кодом и адресом; не закрывайте его, пока пользуетесь приложениями.
4. На iPhone в той же сети Wi-Fi наведите «Камеру» на QR-код. Один раз для телефона страница проведёт по
   шагам: «Скачать профиль» → Настройки → «Профиль загружен» → «Установить» → Основные → Об этом
   устройстве → «Доверие сертификатам» → включить сертификат SENSES. Затем — «Открыть SENSES Web Apps».
5. Скачайте Android-приложение (.apk, .apks, .xapk) на телефон, в SENSES нажмите «Установить приложение»
   и выберите файл. Совет: «Поделиться» → «На экран „Домой“».
6. Подробно — `README-ru.txt` в архиве выпуска.

How to use SENSES Web Apps

1. Download a release archive (Releases of this repository): `…-android10.zip` (Android 10, ~58 MB) or
   `…-all.zip` (all systems, ~170 MB), and unpack it on your computer (macOS, Windows or Linux).
2. You need Python 3.8 or newer (https://www.python.org/downloads/; on Windows tick "Add python.exe to PATH").
3. Start "Start SENSES.command" (macOS), "Start SENSES.bat" (Windows) or `./start-senses.sh` (Linux).
   A window shows a QR code and an address; keep it open while you use the apps.
4. On your iPhone, on the same Wi-Fi, point the Camera at the QR code. Once per phone the page walks you
   through it: Download the profile → Settings → Profile Downloaded → Install → General → About →
   Certificate Trust Settings → turn on the SENSES certificate. Then "Open SENSES Web Apps".
5. Download an Android app (.apk, .apks, .xapk) to the phone, tap "Install app" in SENSES and pick the
   file. Tip: Share → Add to Home Screen.
6. Details: `README.txt` in the release archive.

## О сертификате / About the certificate

Сертификат для телефона простыми словами

* **Создаётся у вас.** Сертификат делает программа из выпуска на вашем компьютере при первом запуске.
  Ни в выпуске, ни в этом репозитории нет ни сертификатов, ни ключей.
* **Закрытый ключ остаётся на вашем компьютере** — в папке настроек пользователя (macOS:
  `~/Library/Application Support/SENSES Web Apps/`, Windows: `%APPDATA%\SENSES Web Apps\`,
  Linux: `~/.config/senses-web-apps/`), доступ только у вашей учётной записи. Ни у авторов SENSES Web
  Apps, ни у кого-либо ещё доступа к нему нет; никуда он не отправляется.
* **На телефон уходит только открытая часть** — профиль, со страницы установки в вашей домашней сети.
* **Действует только в домашней сети**: для адресов 10.x, 172.16–31.x, 192.168.x и имён `.local`. Выдать
  себя им за сайт в интернете нельзя, даже если ключ украдут: это ограничение записано в самом сертификате,
  и телефон его проверяет.
* **Срок — 3 года.** За два месяца до конца окно сервера предупредит.
* **Сбросить** (потерян, утёк, кончается срок): остановите SENSES и запустите с `--reset-cert`
  (macOS: `sh "Start SENSES.command" --reset-cert`; Windows: `"Start SENSES.bat" --reset-cert`), затем
  поставьте на телефоны новый профиль со страницы установки. Приложения и их данные останутся.
* **Удалить с iPhone**: Настройки → Основные → VPN и управление устройством → SENSES Web Apps →
  Удалить профиль.

The phone's certificate in plain words

* **It is made on your computer.** The program from the release makes it on your computer at its first
  start. Neither the release nor this repository contains any certificate or key.
* **The private key stays on your computer**, in your user's settings folder (macOS:
  `~/Library/Application Support/SENSES Web Apps/`, Windows: `%APPDATA%\SENSES Web Apps\`,
  Linux: `~/.config/senses-web-apps/`), readable only by your account. Neither the authors of SENSES Web
  Apps nor anyone else has access to it; it is never sent anywhere.
* **Only the public part goes to the phone**: the profile, from the setup page on your home network.
* **It works only on your home network**: for the addresses 10.x, 172.16–31.x, 192.168.x and `.local`
  names. Even if the key were stolen, it could not be used to pass as any internet site: the limit is
  written into the certificate itself, and the phone checks it.
* **It lasts 3 years.** The server's window warns two months ahead.
* **To reset it** (lost, leaked, about to end): stop SENSES and start it with `--reset-cert` (macOS:
  `sh "Start SENSES.command" --reset-cert`; Windows: `"Start SENSES.bat" --reset-cert`), then install the
  new profile on the phones from the setup page. The apps and their data stay.
* **To remove it from an iPhone**: Settings → General → VPN & Device Management → SENSES Web Apps →
  Remove Profile.

## What is *not* here

* **No Android apps.** Neither the release nor this repository ships any APK. You install your own files.
* **Nothing from Google.** No Google Play services, no Play Store, no Google apps. Google services
  (microG or Google's own) can be added by you, from your own files, in the app's menu.
* **No certificate of ours.** The release's server makes its own root certificate on your computer at the
  first start; it is valid only for home-network addresses (10/8, 172.16/12, 192.168/16) and `.local`
  names, never for internet sites, and its key never leaves your computer.
* **No telemetry.** Nothing is sent anywhere except what the apps themselves connect to.

## Building

On macOS with Xcode's command line tools, emsdk (Emscripten), Python 3, a JDK and the Android SDK
build tools (`d8`, `aapt2`):

```bash
source ~/emsdk/emsdk_env.sh
tools/fetch_aosp44.sh; tools/fetch_aosp60.sh; tools/fetch_aosp81.sh; tools/fetch_aosp10.sh
make fw8-host && make fw8-jars
make -j4 fw8-libs dalvik dalvik-web app-web art art-web fw6-system-web
make -j4 art8 art8-web fw8-graphics-web fw8-system-web
make -j4 fw10-host && make fw10-jars && make -j4 fw10-libs art10 art10-web fw10-graphics-web
tools/vulkan/build_tint.sh WORK build/vulkan/tint   # once; WORK: a folder with ~2 GB free
make -j4 senses-apps-vm senses-apps-web             # the web app: build/senses-apps
make release release-all                            # the release archives: build/release
make release-test                                   # the release's server, certificates, QR, mDNS
```

The `fetch_aosp*.sh` scripts download the AOSP sources at the tags below and apply `patches/`.
Big inputs may live outside the checkout: `ARC_WORKSPACE` (environment) or the folder next to it.

## Layout

```
upstream/       ARC (App Runtime for Chrome) posix_translation and graphics_translation, as imported
shim/           the kernel, NDK bridge, ARM/arm64 translation, GLES/Vulkan, audio, storage for the web
art/ art8/ art10/ dalvik/   the VMs' entry points          framework/   Java parts of the framework
patches/        changes to AOSP (per Android version)       mk/          the build
demo/apps/      the web app (library, app page, service worker)
tools/          build and test tools; tools/release/ — the release's server (Python, standard library)
tests/          Java, native and browser tests
TECHNICAL.md    how the pieces fit together
```

## Licences

* **Android Open Source Project** (`android-10.0.0_r47`, `android-8.1.0_r81`, `android-6.0.1_r81`,
  `android-4.4.4_r2`) — Apache License 2.0, except as noted in its own NOTICE files (the release has them
  all under `licenses/`). **libcore's OpenJDK-derived part (`ojluni`) is under the GNU GPL version 2 with the
  Classpath Exception**: its complete source, as built, is the release asset
  `SENSES-Web-Apps-VERSION-gpl-sources.tar.gz` (and our changes to it are in `patches/`).
* **ARC** (`upstream/`) — BSD-style licence of The Chromium Authors (`upstream/src/LICENSE`).
* **Emscripten runtime, musl, libc++, compiler-rt** — MIT, NCSA, Apache 2.0 with LLVM exception.
* **Tint** (Dawn) — BSD 3-Clause; **SPIRV-Tools** — Apache 2.0; **SPIR-V and Vulkan headers** — MIT/Apache 2.0.
* **SENSES Web Apps' own code** — **GNU Affero General Public License v3.0** (`LICENSE`): if you change it and
  distribute it or let people use it over a network, you must publish your changes under the same licence.

Details: `NOTICE`.

## Trademarks

Android is a trademark of Google LLC. All other product names, logos and brands are property of their
respective owners. This project is independent and is not affiliated with, endorsed by, or sponsored by
any of them.

## Contact

`https://github.com/Istratii-tech/SENSES-Web-Apps` (issues), `https://t.me/istratii_tech`
