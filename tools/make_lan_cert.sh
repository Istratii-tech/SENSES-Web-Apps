#!/bin/sh
# HTTPS for testing the PWA on a phone over the local network (topic
# userspace, docs/topics/userspace.md "PWA"): service workers and wasm
# threads need a secure context, which a phone only gives a LAN address over
# trusted HTTPS. Makes, in OUT (default build/tls):
#   ca.key, ca.crt          a private root CA (kept between runs, so a phone
#                           that trusts it keeps trusting new server certs)
#   server.key, server.crt  a server certificate for this Mac's LAN address
#                           and NAME.local, signed by that CA
#   public/ca.crt           what the phone downloads and trusts
#
# Usage: tools/make_lan_cert.sh [IP [OUT]]   (IP: en0's address by default)
#
# Then: ARC_BIND=0.0.0.0 ARC_TLS_CERT=build/tls/server.crt
#       ARC_TLS_KEY=build/tls/server.key tools/serve.py 8443 build/pwa
# Real hosting needs none of this: any HTTPS static host will do (sw.js adds
# the isolation headers).
set -eu
ip=${1:-$(ipconfig getifaddr en0)}
out=${2:-build/tls}
host=$(scutil --get LocalHostName 2>/dev/null || hostname -s)
mkdir -p "$out/public"
cd "$out"

if [ ! -f ca.key ]; then
  openssl req -x509 -new -newkey rsa:2048 -nodes -sha256 -days 3650 \
    -keyout ca.key -out ca.crt -subj "/CN=arc-wasm local CA ($host)" \
    -addext "basicConstraints=critical,CA:TRUE" \
    -addext "keyUsage=critical,keyCertSign,cRLSign"
fi

# iOS accepts server certificates of at most 825 days, with the names in
# subjectAltName and serverAuth in extendedKeyUsage.
cat > server.ext <<EOF
basicConstraints=CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=IP:$ip,DNS:$host.local,DNS:localhost,IP:127.0.0.1
EOF
openssl req -new -newkey rsa:2048 -nodes -sha256 -keyout server.key -out server.csr \
  -subj "/CN=$ip"
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
  -days 397 -sha256 -extfile server.ext -out server.crt
rm -f server.csr server.ext
cp ca.crt public/ca.crt
# The phone's first stop, served over plain HTTP (e.g. tools/serve.py 8080
# build/tls/public): the CA to install, then the HTTPS link.
cat > public/index.html <<EOF
<!doctype html>
<html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Сертификат для Android 4.4</title>
<style>
  body { margin: 0; padding: 24px 20px; background: #000; color: #e6e9f0;
         font: 16px/1.5 -apple-system, system-ui, sans-serif; }
  main { max-width: 420px; margin: 0 auto; }
  h1 { font-size: 22px; margin: 0 0 12px; }
  li { margin: 10px 0; }
  a.button { display: block; margin: 16px 0; padding: 12px; border-radius: 10px; text-align: center;
             background: #3ddc84; color: #062b16; font-weight: 600; text-decoration: none; }
  small { color: #9aa1b2; }
</style></head>
<body><main>
<h1>Шаг 1. Сертификат</h1>
<p>Чтобы iPhone открыл Android с этого Mac по HTTPS, один раз доверьтесь его сертификату:</p>
<a class="button" href="ca.crt">Скачать сертификат</a>
<ol>
  <li>Нажмите «Разрешить», затем «Закрыть».</li>
  <li>Настройки → «Профиль загружен» → «Установить».</li>
  <li>Настройки → Основные → Об этом устройстве → Доверие сертификатам →
      включите «arc-wasm local CA».</li>
</ol>
<h1>Шаг 2. Android</h1>
<a class="button" href="https://$ip:8443/">Открыть Android</a>
<p><small>Нужно только для проверки с Mac по Wi-Fi. На настоящем HTTPS-хостинге
сертификат не нужен.</small></p>
</main></body></html>
EOF
echo "server certificate for $ip and $host.local in $out/server.crt"
