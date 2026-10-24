#!/bin/bash
# (Перенесено из SENSES.) Проверка второй половины заставы имён — личных данных ПО ФОРМЕ (tools/checkpatterns.py через tools/checknames.sh).
# Код возврата 0 — застава работает. Отдельный файл рядом с run.sh (проверка первой половины, по списку слов).
#
# ⚠️★ Образцы строятся во временном каталоге ИЗ КУСКОВ: готовых значений в тексте теста нет, и сам этот файл заставу
# проходит. Положительные образцы (`pos-*`) обязаны ловиться, отрицательные (`neg-*`) — нет: застава, которая кричит
# на законном, перестаёт быть слышимой.
set -uo pipefail
cd "$(dirname "$0")/../.."
GUARD=tools/checknames.sh
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
FAIL=0

say() { printf '\n%s\n' "$*"; }
ok()  { printf '  ✓ %s\n' "$*"; }
bad() { FAIL=1; printf '  ⛔ %s\n' "$*"; }

[ -x "$GUARD" ] || { echo "⛔ нет заставы $GUARD"; exit 1; }
[ -f tools/checkpatterns.py ] || { echo "⛔ нет tools/checkpatterns.py"; exit 1; }

say "== 1. личные данные ПО ФОРМЕ (tools/checkpatterns.py): образцы строятся из кусков, готовых значений в тексте теста нет"
# Что должно ловиться: имя файла образца = вид. Значения собраны из частей, чтобы сам этот файл не содержал находок.
mkdir -p "$TMP/pat"
P="$TMP/pat"
net1="19"; net2="2.1"; net3="68"
imei_a="35"; imei_b="2099"; imei_c="001761481"          # 15 цифр, контроль Луна сходится
mac1="a4:5e"; mac2="60:c8:1d:3f"                            # шесть пар через ':' — собираются из двух кусков
printf 'mac %s:%s end\n' "$mac1" "$mac2"                       > "$P/pos-mac.txt"
printf 'mac-dash %s end\n' "$(printf '%s:%s' "$mac1" "$mac2" | tr ':' '-')" > "$P/pos-mac-dash.txt"
printf 'IMEI %s%s%s\n' "$imei_a" "$imei_b" "$imei_c"          > "$P/pos-imei.txt"
printf 'iccid %s%s%s%s\n' "89" "7010" "123456789012" "3"       > "$P/pos-iccid.txt"
printf 'addr %s%s%s.0.151\n' "$net1" "$net2" "$net3"           > "$P/pos-ip-home.txt"
printf 'subnet %s%s%s.0.* here\n' "$net1" "$net2" "$net3"      > "$P/pos-ip-subnet.txt"
printf 'x 17%s16.5.4 y\n' "2."                                 > "$P/pos-ip-172-16.txt"
printf 'x 17%s31.0.9 y\n' "2."                                 > "$P/pos-ip-172-31.txt"
printf 'x 1%s1.2.3 y\n' "0.0."                                 > "$P/pos-ip-10.txt"
printf 'mail %s@%s.%s\n' "john.smith" "corp-example" "com"      > "$P/pos-mail.txt"
printf 'path /Us%s/%s/work\n' "ers" "ivanov"                    > "$P/pos-home-users.txt"
printf 'path /ho%s/%s/x\n' "me" "petrov"                        > "$P/pos-home-home.txt"
printf 'x \0\0 %s:%s \0\0\n' "$mac1" "$mac2"                    > "$P/pos-mac-binary.bin"
missp=0
for f in "$P"/pos-*; do
  if "$GUARD" "$f" >/dev/null 2>&1; then bad "не поймано: $(basename "$f")"; missp=1; fi
done
[ "$missp" = 0 ] && ok "все $(ls "$P"/pos-* | wc -l | tr -d ' ') видов находок пойманы"

# Чего ловить нельзя: иначе заставу перестанут слушать.
printf 'zero 00:00:00:00:00:00 time 12:34:56\n'                > "$P/neg-mac.txt"
printf 'emu 1%s2.15 and 1%s2.2 and 10.15.7\n' "0.0." "0.0."    > "$P/neg-ip-emulator.txt"
printf 'sample %s%s%s.x.y\n' "$net1" "$net2" "$net3"           > "$P/neg-ip-sample.txt"
printf 'out of range 17%s15.0.1 and 17%s32.0.1\n' "2." "2."    > "$P/neg-ip-range.txt"
printf 'num 000000000000000 and 111111111111111 and 123456789012345\n' > "$P/neg-imei.txt"
printf 'mails istratiitech@users.noreply.github.com noreply@anthropic.com this@MainActivity.window data@hello.jar framework@boot.art\n' > "$P/neg-mail.txt"
printf 'ranges 1%s/8 and 172.16.0.0/12 and 19%s.0.0/16\n' "0.0.0.0" "2.168" > "$P/neg-ip-ranges.txt"
# ⚠️ Домашний каталог macOS в любом виде (в том числе шаблоном) ловит уже список слов — поэтому образец безымянных шаблонов только для Linux.
printf 'tmpl /ho%s/<name>/x /ho%s/$USER/y a/home/x /home/ ~/work /home/user/z\n' "me" "me" > "$P/neg-home.txt"
printf 'ver 1.10.1.2.3 and 4.4.4.4 and 8.8.8.8\n'              > "$P/neg-misc.txt"
missn=0
for f in "$P"/neg-*; do
  if ! "$GUARD" "$f" >/dev/null 2>&1; then bad "ложное срабатывание: $(basename "$f")"; "$GUARD" "$f" 2>&1 | sed 's/^/      /'; missn=1; fi
done
[ "$missn" = 0 ] && ok "все $(ls "$P"/neg-* | wc -l | tr -d ' ') образцов без находок прошли"

# Вывод замаскирован: сырое значение в журнал не попадает.
out=$("$GUARD" "$P/pos-mac.txt" 2>&1)
if printf '%s' "$out" | grep -qF "$mac1:$mac2"
then bad "значение напечатано целиком — вывод не замаскирован"
else ok "значение в выводе замаскировано"; fi
# Вторая половина заставы действительно работала (число файлов в итоге).
"$GUARD" "$P/neg-mac.txt" 2>&1 | grep -q 'шаблонами: 1' && ok "итог называет число файлов, проверенных шаблонами" || bad "в итоге нет числа файлов, проверенных шаблонами"

# Вторая половина не запустилась (нет скрипта) — это ОТКАЗ заставы, а не «чисто»: молчащий прибор неотличим от нашедшего ничего.
say "== 2. нет tools/checkpatterns.py — застава отказывает, а не молчит"
R="$TMP/без-шаблонов"; mkdir -p "$R/tools"
cp "$GUARD" tools/badwords.b64 tools/unpackimage.py "$R/tools/"
printf 'чистый текст\n' > "$R/clean.txt"
out2=$("$R/tools/checknames.sh" "$R/clean.txt" 2>&1); rc2=$?
if [ "$rc2" != 0 ] && printf '%s' "$out2" | grep -q 'checkpatterns.py'
then ok "без checkpatterns.py застава отказала и назвала причину"
else bad "без checkpatterns.py застава ответила rc=$rc2: $out2"; fi

printf '\n'
[ "$FAIL" = 0 ] && echo "ВСЁ ХОРОШО" || echo "ЕСТЬ ОТКАЗЫ"
exit "$FAIL"
