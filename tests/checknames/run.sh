#!/bin/bash
# Проверка заставы имён (tools/checknames.sh; перенесена из SENSES). Код возврата 0 — застава работает.
#
# ⚠️★ Грязный образец делается ИЗ САМОГО СПИСКА и во временном каталоге за
# пределами проекта. Положить запрещённые слова в файл под учётом git — значит
# провалить ровно ту проверку, которую этот тест проверяет.
set -uo pipefail
cd "$(dirname "$0")/../.."
GUARD=tools/checknames.sh
LIST=tools/badwords.b64
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
FAIL=0

say() { printf '\n%s\n' "$*"; }
ok()  { printf '  ✓ %s\n' "$*"; }
bad() { FAIL=1; printf '  ⛔ %s\n' "$*"; }

[ -x "$GUARD" ] || { echo "⛔ нет заставы $GUARD"; exit 1; }
[ -f "$LIST"  ] || { echo "⛔ нет списка $LIST"; exit 1; }

say "== 1. чистый файл проходит"
printf 'обычный текст без запрещённых слов\n' > "$TMP/clean.txt"
if "$GUARD" "$TMP/clean.txt" >/dev/null 2>&1
then ok "чистый файл принят"
else bad "чистый файл отвергнут — застава ругается на пустом месте"; fi

say "== 2. каждое запрещённое слово ловится по отдельности"
n=0; miss=0
while IFS= read -r w; do
  [ -n "$w" ] || continue
  n=$((n+1))
  printf 'строка с %s внутри\n' "$w" > "$TMP/dirty.txt"
  if "$GUARD" "$TMP/dirty.txt" >/dev/null 2>&1; then
    bad "слово №$n пропущено заставой"; miss=$((miss+1))
  fi
done < <(base64 -d < "$LIST")
if [ "$n" = 0 ]; then bad "список пуст — застава ничего не ищет"
elif [ "$miss" = 0 ]; then ok "все $n слов пойманы"; fi

say "== 3. регистр не спасает"
first=$(base64 -d < "$LIST" | head -1)
printf 'СТРОКА С %s ВНУТРИ\n' "$(printf '%s' "$first" | tr '[:lower:]' '[:upper:]')" > "$TMP/dirty.txt"
if "$GUARD" "$TMP/dirty.txt" >/dev/null 2>&1
then bad "верхний регистр проходит мимо заставы"
else ok "регистр не спасает"; fi

say "== 4. запрещённое слово в ДВОИЧНОМ файле тоже ловится"
# ⚠️ Ради этого случая застава и зовёт grep с ключом -a. Без него grep молча
# бросает файл, где встретился нулевой байт, — и слово, вкомпилированное в
# библиотеку или уехавшее в ресурсы пакета, осталось бы незамеченным. А
# проверять надо в первую очередь именно готовый пакет.
#
# ⚠️★ Образец делаем ПРАВДОПОДОБНЫМ: строковая константа в бинаре лежит между
# нулевыми байтами. Первая редакция этого теста окружала слово буквами `x` — и
# отказала, когда застава научилась требовать границу лексемы. Отказал ТЕСТ, а
# не застава: слова, зажатого между буквами, в настоящем бинаре не бывает.
{ head -c 512 /dev/urandom; printf '\0%s\0' "$first"; head -c 512 /dev/urandom; } > "$TMP/blob.bin"
if "$GUARD" "$TMP/blob.bin" >/dev/null 2>&1
then bad "в двоичном файле слово не найдено — проверка готового пакета бесполезна"
else ok "в двоичном файле слово найдено"; fi

say "== 4б. короткое слово в двоичном: ловится с продолжением, но не в чистом поле"
short=$(base64 -d < "$LIST" | awk 'length($0)<=3' | head -1)
if [ -z "$short" ]; then ok "коротких слов в списке нет — правило не нужно"
else
  # Настоящая утечка: слово плюс продолжение.
  { head -c 256 /dev/urandom; printf '\0%s_binder\0' "$short"; head -c 256 /dev/urandom; } > "$TMP/s1.bin"
  if "$GUARD" "$TMP/s1.bin" >/dev/null 2>&1
  then bad "короткое слово С ПРОДОЛЖЕНИЕМ пропущено — правило слишком узкое"
  else ok "короткое слово с продолжением поймано"; fi
  # Случайный мусор: те же буквы, но продолжения нет. Ловить это нельзя —
  # иначе застава кричит на каждой таблице и её перестают слушать.
  printf '\0%s\0' "$short" > "$TMP/s2.bin"
  printf '\x01\x02\x03' >> "$TMP/s2.bin"
  if "$GUARD" "$TMP/s2.bin" >/dev/null 2>&1
  then ok "три буквы в чистом поле не считаются находкой"
  else bad "ложное срабатывание на трёх буквах без продолжения"; fi
fi

say "== 5. верные ссылки и название продукта проходят"
# ⚠️★ Ради этого случая список и переделывался. Сперва в нём лежало имя автора
# с подчёркиванием — чтобы ловить неверную ссылку на GitHub, где подчёркивание
# недопустимо. Но та же строка законно стоит и в НАЗВАНИИ продукта, и в ссылке
# на Telegram: застава завалила собственный замысел. Запрещать надо не строку,
# а ровно то, что вредно.
cat > "$TMP/allow.kt" <<'KT'
const val NAME = "SENSES Web Apps"
const val TG = "https://t.me/istratii_tech"
const val GH = "https://github.com/istratiitech"
const val ORG = "https://github.com/Istratii-tech"
const val SIGN = "istratiitech <istratiitech@users.noreply.github.com>"
KT
if "$GUARD" "$TMP/allow.kt" >/dev/null 2>&1
then ok "название, Telegram и верный GitHub пропущены"
else bad "застава ругается на законное:"; "$GUARD" "$TMP/allow.kt" 2>&1 | sed 's/^/      /'; fi

say "== 6. неверная ссылка на GitHub ловится"
# ★ Организация с дефисом (Istratii-tech) — законна: в ней лежат исходники переводчика (решение человека при выпуске).
# Неверна только ссылка с подчёркиванием — на GitHub такого имени быть не может.
miss6=0
for bad_url in 'https://github.com/istratii_tech'; do
  printf 'const val GH = "%s"\n' "$bad_url" > "$TMP/deny.kt"
  if "$GUARD" "$TMP/deny.kt" >/dev/null 2>&1; then
    bad "прошла неверная ссылка: $bad_url"; miss6=1
  fi
done
[ "$miss6" = 0 ] && ok "неверная ссылка поймана"

say "== 7. без аргументов проверяется то, что под учётом git"
# ⚠️ Именно под учётом, а не всё подряд: опубликовано будет только оно, а
# перебор всего дерева упёрся бы в распакованные исходники QEMU и в сборку.
out=$("$GUARD" 2>&1); rc=$?
if [ "$rc" = 0 ] && printf '%s' "$out" | grep -q 'чисто'
then ok "проект чист"
else bad "проект не проходит собственную заставу:"; printf '%s\n' "$out" | head -20 | sed 's/^/      /'; fi

say "== 8. файл с РУССКИМ ИМЕНЕМ под учётом git тоже проверяется"
# ⚠️★★★★★ Ради этого случая застава и чинилась 21 сентября. `git ls-files`
# печатает имена с не-ASCII буквами В КАВЫЧКАХ и восьмеричными escape'ами:
# такого пути на диске нет, grep молча отказывал, находок не было — и застава
# отвечала «чисто», НЕ ЗАГЛЯНУВ НИ В ОДИН документ с русским именем. Весь
# `docs/` был вне проверки, и запрещённые слова там лежали.
#
# Проверяем именно ту дорогу, где была дыра: обход БЕЗ АРГУМЕНТОВ по git.
# Для этого нужен отдельный репозиторий — свой, временный, со своей копией
# заставы (она всегда переходит в каталог над собой).
REPO="$TMP/репо"
mkdir -p "$REPO/tools"
cp "$GUARD" "$REPO/tools/" && cp "$LIST" "$REPO/tools/" && cp "$(dirname "$GUARD")/checkpatterns.py" "$(dirname "$GUARD")/unpackimage.py" "$REPO/tools/"
printf 'строка с %s внутри\n' "$first" > "$REPO/ГРЯЗНЫЙ-ДОКУМЕНТ.md"
(
  cd "$REPO" || exit 1
  git init -q . >/dev/null 2>&1
  git add -A >/dev/null 2>&1
)
if [ ! -d "$REPO/.git" ]; then
  bad "временный репозиторий не завёлся — случай не проверен"
elif "$REPO/tools/checknames.sh" >/dev/null 2>&1; then
  bad "документ с русским именем ПРОПУЩЕН — застава снова смотрит не туда"
else
  ok "документ с русским именем проверен и слово поймано"
fi

say "== 9. образ (.data): слово в СЖАТОЙ записи zip внутри образа ловится"
# Образ /system несёт jar и APK подряд; сжатая запись (ресурсы, assets) снаружи
# выглядит шумом. tools/unpackimage.py распаковывает каждую такую запись.
python3 - "$TMP/sys.data" "$first" <<'PY'
import io, sys, zipfile, os
buf = io.BytesIO()
with zipfile.ZipFile(buf, 'w', zipfile.ZIP_DEFLATED) as z:
    z.writestr('res/raw/note.txt', ('padding ' * 50 + sys.argv[2] + ' ' + 'padding ' * 50).encode())
data = os.urandom(4096) + buf.getvalue() + os.urandom(4096)
open(sys.argv[1], 'wb').write(data)
PY
if LC_ALL=C grep -aqF "$first" "$TMP/sys.data"; then bad "образец негодный: слово видно и без распаковки"
elif "$GUARD" "$TMP/sys.data" >/dev/null 2>&1; then bad "слово в сжатой записи образа пропущено"
else ok "слово в сжатой записи образа поймано"; fi
out9=$("$GUARD" "$TMP/sys.data" 2>&1)
printf '%s' "$out9" | grep -q 'из образа' && ok "находка названа с образом, откуда она" || bad "не сказано, из какого образа находка"

printf '\n'
[ "$FAIL" = 0 ] && echo "ВСЁ ХОРОШО" || echo "ЕСТЬ ОТКАЗЫ"
exit "$FAIL"
