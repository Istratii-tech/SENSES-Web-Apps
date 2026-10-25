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

Попробуйте без установки: https://istratii-tech.github.io/SENSES-Web-Apps/vm/ 
Долгая загрузка на черном экране

**Какой файл качать.** Обычно — **запускатор** `SENSES-Web-Apps-launcher-ВЕРСИЯ.zip` (~35 КБ) из Releases
этого репозитория. При каждом запуске он сам скачивает на компьютер последнюю версию SENSES Web Apps
отсюда же (Android 10 — ~58 МБ; все системы — ~170 МБ, по `--variant all`), сверяет SHA-256 и
запускает; обновления приходят сами, сертификат и приложения на телефоне сохраняются. Полные архивы
`…-android10.zip` и `…-all.zip` — для компьютера без интернета: их распаковывают и запускают так же
(или отдают запускатору: `--install ПУТЬ.zip`).

1. Скачайте launcher (или полный архив) и распакуйте его на компьютере (macOS, Windows или Linux).
2. Установите Python 3.8 или новее (https://www.python.org/downloads/; в Windows отметьте «Add python.exe to PATH»). 
   На macos скорее всего установится автоматически.
3. Запустите «Start SENSES.command» (macOS), «Start SENSES.bat» (Windows) или `./start-senses.sh` (Linux).
   Выберите систему. Лучше всего сейчас работает Андроид 10.
   Вы так же можете запустить через iSH на айфоне без компьютера.
   Запускатор скачает систему Android 10 и покажет загрузку. Затем появится окно с
   QR-кодом и адресом; не закрывайте его, пока не запустите первое приложение.
4. На iPhone в той же сети Wi-Fi наведите «Камеру» на QR-код. Один раз для телефона страница проведёт по
   шагам: «Скачать профиль» → Настройки → «Профиль загружен» → «Установить» → Основные → Об этом
   устройстве → «Доверие сертификатам» → включить сертификат SENSES. Затем вернитесь в Safari и нажмите «Открыть SENSES Web Apps».
   Если страница не предлагает профиль — отсканируйте второй QR-код «Профиль для iPhone» (в окне и на
   странице компьютера) или откройте `http://АДРЕС:8499/p`.
   Система с установкой вашего личного сертификата требуется для безопасности. 
   Вы можете установить и чужой сертификат, но так делать не рекомендуется.
5. **Предупреждение Safari.** Safari может сам открыть адрес по https и показать «Подключение не защищено».
   Это ожидаемо, пока профиль не установлен: нажмите «Подробнее» → «Посетить веб-сайт» (это сертификат
   вашего же компьютера). После установки профиля и включения доверия к нему предупреждение исчезает.
   Если после «Посетить веб-сайт» ничего не происходит — полностью закройте Safari (смахните его вверх в
   переключателе приложений) и откройте адрес снова.
6. Скачайте Android-приложение (.apk, .apks, .xapk) на телефон, в SENSES нажмите «Установить приложение»
   и выберите файл из приложения "Файлы". Совет: в Safari выберите «Поделиться» → «На экран „Домой“», на рабочем столе появится иконка.
   У иконки на рабочем столе будет своя библиотека приложений.
7. При запуске первого приложения будет загружена система Андроид с вашего компьютера.
   После успешного запуска первого приложения, устанавливать и пользоваться другими приложениями можно оффлайн.
8. Подробно — `README-ru.txt` в архиве (разделы «Как это работает» и «Обновление» — в запускаторе).
   Откат к прошлой версии: запуск с `--rollback`; другие системы: `--variant all`.

How to use SENSES Web Apps

Try it without installing: https://istratii-tech.github.io/SENSES-Web-Apps/vm/ 
Long loading time on the black screen

**Which file to download.** Usually the **launcher**, `SENSES-Web-Apps-launcher-VERSION.zip` (~35 KB), from
this repository's Releases. At each start it downloads the latest SENSES Web Apps from here to your
computer (Android 10 ~58 MB; all systems ~170 MB with `--variant all`), checks its SHA-256 and
starts it; updates come by themselves, and the certificate and the phone's apps stay. The full archives
`…-android10.zip` and `…-all.zip` are for a computer without internet: unpack and start them the same
way (or give one to the launcher: `--install PATH.zip`).

1. Download the launcher (or the full archive) and extract it on your computer (macOS, Windows or Linux).
2. Install Python 3.8 or newer (https://www.python.org/downloads/; on Windows, check “Add python.exe to PATH”).
On macOS it will most likely be installed automatically.
3. Run “Start SENSES.command” (macOS), “Start SENSES.bat” (Windows) or ./start-senses.sh (Linux).
You can also run it via iSH on your iPhone without a computer.
Choose a system. Android 10 currently works best.
The launcher will download Android 10 and show the download progress. A window with a
QR code and an address will then appear. Keep it open until you have launched your first app.
4. OOn an iPhone connected to the same Wi-Fi network, point the Camera at the QR code. The first time on each phone, the page walks you through
these steps: “Download profile” → Settings → “Profile Downloaded” → “Install” → General → About →
“Certificate Trust Settings” → turn on the SENSES certificate. Then go back to Safari and tap “Open SENSES Web Apps”.
If the page doesn’t offer the profile, scan the second QR code, “Profile for iPhone” (shown in the window and on the
computer’s page), or open http://ADDRESS:8499/p.
For security, the system needs you to install your own personal certificate.
You can install someone else’s certificate too, but we don’t recommend it.
5. **Safari's warning.** Safari may switch the address to https by itself and show “This Connection Is Not Private”.
This is expected until the profile is installed. Tap “Show Details” → “visit this website” (the certificate
belongs to your own computer). The warning goes away once you install the profile and turn on trust for it.
If nothing happens after “visit this website”, fully close Safari (swipe it up in the
app switcher) and open the address again.
6. Download an Android app (.apk, .apks, .xapk) to your phone. In SENSES, tap “Install app”
and pick the file in the Files app. Tip: in Safari, tap Share → “Add to Home Screen” to put an icon on your Home Screen.
That Home Screen icon gets its own app library.
7. When you launch your first app, the Android system is loaded from your computer.
Once the first app has launched, you can install and use other apps offline.
8. Details: `README.txt` in the archive (the launcher's has "How it works" and "Updating").
   Back to the previous version: start with `--rollback`; other systems: `--variant all`.

## Запуск эмулятора в браузере / Run the emulator in a browser

[Открыть SENSES Web Apps на GitHub Pages](https://istratii-tech.github.io/SENSES-Web-Apps/). Здесь работает Android 10: выберите APK, и он запустится в браузере. APK и данные остаются в локальном хранилище браузера и не загружаются на сайт. Первый запуск скачивает около 125 МБ системных файлов.

GitHub Pages не предоставляет сетевой мост, поэтому приложения внутри эмулятора не имеют доступа к интернету. Для приложений с сетью используйте запускатор и локальный сервер из инструкций выше.

[Open SENSES Web Apps on GitHub Pages](https://istratii-tech.github.io/SENSES-Web-Apps/). It runs Android 10 in the browser. Choose an APK to launch it; the APK and app data stay in the browser's local storage and are not uploaded to the site. The first launch downloads about 125 MB of system files.

GitHub Pages has no SENSES network relay, so Android apps on the page cannot access the internet. Use the launcher and local server above for apps that need networking.

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
make release release-all release-launcher          # the release archives: build/release
make release-test                                   # the server, certificates, QR, mDNS, the launcher
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
