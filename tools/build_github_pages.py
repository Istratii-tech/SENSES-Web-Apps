#!/usr/bin/env python3
import argparse
import hashlib
import json
import os
import re
import shutil
import zipfile
from pathlib import Path, PurePosixPath
from typing import Mapping

ROOT = Path(__file__).resolve().parents[1]
REQUIRED_VM_FILES = {
    'artvm10.js',
    'artvm10.wasm',
    'artvm10.data',
    'arc_tint.js',
    'arc_tint.wasm',
}
REQUIRED_SHELL_FILES = (
    'bootstrap.js',
    'report.js',
    'site.css',
)
DATA_PART_SIZE = 32 * 1024 * 1024


def fail(message: str) -> None:
    raise SystemExit(message)


def safe_relative_path(value: str) -> Path:
    path = PurePosixPath(value)
    if path.is_absolute() or not path.parts or any(part in ('', '.', '..') for part in path.parts):
        fail(f'Unsafe archive path: {value}')
    return Path(*path.parts)


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        fail(f'Expected one {label} in the VM page, found {count}')
    return text.replace(old, new, 1)


def archive_root(archive: zipfile.ZipFile) -> str:
    roots = {
        PurePosixPath(info.filename).parts[0]
        for info in archive.infolist()
        if info.filename and not info.is_dir()
    }
    if len(roots) != 1:
        fail('Expected one top-level directory in the Android 10 release archive')
    root = roots.pop()
    if not root.startswith('SENSES-Web-Apps-'):
        fail(f'Unexpected release directory: {root}')
    return root


def read_member(archive: zipfile.ZipFile, names: Mapping[str, zipfile.ZipInfo], name: str) -> bytes:
    if name not in names:
        fail(f'Missing release file: {name}')
    with archive.open(names[name]) as source:
        return source.read()


def copy_member(archive: zipfile.ZipFile, info: zipfile.ZipInfo, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    with archive.open(info) as source, destination.open('wb') as target:
        shutil.copyfileobj(source, target, length=1024 * 1024)


def split_data_file(data_path: Path, version: str) -> None:
    total_size = data_path.stat().st_size
    total_written = 0
    parts = []
    part_index = 0
    with data_path.open('rb') as source:
        while True:
            part_name = f'{data_path.name}.part-{part_index:03d}'
            part_path = data_path.with_name(part_name)
            part_size = 0
            with part_path.open('wb') as target:
                while part_size < DATA_PART_SIZE:
                    block = source.read(min(1024 * 1024, DATA_PART_SIZE - part_size))
                    if not block:
                        break
                    target.write(block)
                    part_size += len(block)
                    total_written += len(block)
            if part_size == 0:
                part_path.unlink(missing_ok=True)
                break
            parts.append(part_name)
            part_index += 1
    if total_written != total_size:
        fail(f'Incomplete Android data split: expected {total_size} bytes, wrote {total_written}')
    manifest = {'version': version, 'size': total_size, 'parts': parts}
    data_path.with_name(data_path.name + '.parts.json').write_text(
        json.dumps(manifest, separators=(',', ':')) + '\n', encoding='utf-8'
    )
    data_path.unlink()


def verify_release(zip_path: Path, checksum_path: Path) -> None:
    fields = checksum_path.read_text(encoding='utf-8').strip().split()
    if len(fields) < 2 or not re.fullmatch(r'[0-9a-fA-F]{64}', fields[0]):
        fail(f'Invalid checksum file: {checksum_path}')
    if Path(fields[-1]).name != zip_path.name:
        fail('The checksum file does not name the supplied release archive')
    digest = hashlib.sha256()
    with zip_path.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
    if digest.hexdigest().lower() != fields[0].lower():
        fail('Android 10 release SHA-256 verification failed')


def patch_vm_page(source: str, version: str) -> str:
    text = source
    text, comment_count = re.subn(
        r'<!-- A test page for the VM side of SENSES Web Apps.*?-->\n',
        '',
        text,
        count=1,
        flags=re.S,
    )
    if comment_count != 1:
        fail('Expected the upstream VM test page comment')
    text = replace_once(text, '<html lang="ru">', '<html lang="ru" data-theme="dark">', 'dark page theme')
    text = replace_once(text, '<title>SENSES VM test</title>', '<title>SENSES Web Apps — Android 10</title>', 'page title')
    text = replace_once(
        text,
        '</style>\n</head>',
        '</style>\n<link rel="stylesheet" href="site.css">\n'
        '<link rel="icon" type="image/svg+xml" href="../favicon.svg">\n</head>',
        'stylesheet link',
    )
    text = replace_once(
        text,
        '<h1>SENSES — проверка VM</h1>\n  <div class="muted" id="storage">Хранилище…</div>',
        '  <div class="topbar"><a class="home-link" href="../">← SENSES Web Apps</a>\n'
        f'    <span class="build-tag">Android 10 · {version}</span></div>\n'
        '  <h1>Запуск Android 10</h1>\n'
        '  <div class="muted" id="storage">Проверяю браузер…</div>\n'
        '  <div class="notice"><strong>Первый запуск загрузит около 125 МБ.</strong> APK и данные приложений '
        'останутся в хранилище этого браузера. На GitHub Pages нет сетевого моста: приложения Android здесь '
        'работают без интернета.</div>',
        'page heading and local-use notice',
    )
    text = replace_once(
        text,
        '    <select id="stack">\n'
        '      <option value="a81">Android 8.1</option>\n'
        '      <option value="a10">Android 10</option>\n'
        '      <option value="a60">Android 6.0.1</option>\n'
        '      <option value="a44">Android 4.4</option>\n'
        '    </select>',
        '    <input type="hidden" id="stack" value="a10">',
        'Android version selector',
    )
    text = replace_once(
        text,
        '<label class="file">Добавить APK<input type="file" id="file"></label>',
        '<label class="file">Добавить APK<input type="file" id="file" accept=".apk,application/vnd.android.package-archive"></label>',
        'APK file picker',
    )
    text = replace_once(
        text,
        'данные senses/data/{a44,a60,a81,a10}',
        'данные senses/data/a10',
        'Android 10 storage label',
    )
    text = replace_once(
        text,
        "    log('storage ' + kind);\n",
        '',
        'remove low-value startup debug log',
    )
    text = replace_once(
        text,
        '<script src="apk.js"></script>',
        '<script src="bootstrap.js"></script>\n<script src="apk.js"></script>',
        'isolation bootstrap script',
    )
    text = replace_once(
        text,
        '<script>\n  var params = new URLSearchParams(location.search);',
        '<script>\n(async function () {\n'
        '  if (!(await window.SENSES_PAGES_READY)) return;\n'
        '  var params = new URLSearchParams(location.search);',
        'application bootstrap',
    )
    text = replace_once(
        text,
        '  var params = new URLSearchParams(location.search);',
        '  var params = new URLSearchParams();',
        'disable URL-driven test controls on the public page',
    )
    text = replace_once(
        text,
        "  if (params.get('stack')) $('stack').value = params.get('stack');",
        "  $('stack').value = 'a10';",
        'Android 10 default',
    )
    text = replace_once(
        text,
        "    params.getAll('env').forEach(function (e) { q.append('env', e); });",
        "    params.getAll('env').forEach(function (e) { q.append('env', e); });\n"
        "    q.append('env', 'ARC_NET=0');",
        'offline Android network setting',
    )
    text = replace_once(
        text,
        "  }).catch(function (e) { log('Хранилище: ' + e, true); });\n</script>",
        "  }).catch(function (e) { log('Хранилище: ' + e, true); });\n"
        "})();\n</script>",
        'application bootstrap closure',
    )
    text = replace_once(
        text,
        '<div id="vm"></div>\n<div id="keys">',
        '<div id="vm"></div>\n'
        '<div id="startup" class="indeterminate" aria-live="off">\n'
        '  <div class="loading-card">\n'
        '    <div class="loading-kicker">ANDROID 10</div>\n'
        '    <h2 id="loading-title" aria-live="polite">Запуск приложения…</h2>\n'
        '    <div id="loading-bar" class="loading-bar" role="progressbar" aria-label="Загрузка Android 10" aria-valuemin="0" aria-valuemax="100" aria-valuetext="Подготовка Android 10"><span id="loading-fill"></span></div>\n'
        '    <div class="loading-row"><span id="loading-detail">Подготовка системы…</span><span id="loading-percent">…</span></div>\n'
        '    <div id="loading-count" class="loading-count">Подготовка Android · 0 с</div>\n'
        '  </div>\n'
        '</div>\n'
        '<div id="keys">',
        'startup progress overlay',
    )
    text = replace_once(
        text,
        '  <pre id="log"></pre>\n</main>',
        '  <pre id="log"></pre>\n'
        '  <div class="site-foot"><a href="../">О проекте</a><a href="../LICENSE.txt">Лицензия</a></div>\n'
        '</main>',
        'page footer',
    )
    text = replace_once(
        text,
        '  var orientations = {};  // package -> meta.orientation (refresh())\n  function start(pkg) {',
        '  var orientations = {};  // package -> meta.orientation (refresh())\n'
        '  var loadParts = { system: { loaded: 0, total: 0 }, wasm: { loaded: 0, total: 0 } };\n'
        "  var startupStage = 'prepare', startupAt = 0, startupTimer = 0, startupPackage = '';\n"
        '  function formatMB(bytes) {\n'
        "    return (bytes / 1000000).toLocaleString('ru-RU', { minimumFractionDigits: 1, maximumFractionDigits: 1 });\n"
        '  }\n'
        '  function showStartup(pkg) {\n'
        '    startupPackage = pkg;\n'
        "    startupStage = 'prepare';\n"
        '    startupAt = performance.now();\n'
        '    loadParts = { system: { loaded: 0, total: 0 }, wasm: { loaded: 0, total: 0 } };\n'
        "    $('startup').classList.add('on', 'indeterminate');\n"
        "    $('loading-title').textContent = 'Запуск ' + pkg;\n"
        "    $('loading-detail').textContent = 'Подготовка Android 10…';\n"
        "    $('loading-count').textContent = 'Подготовка Android · 0 с';\n"
        "    $('loading-percent').textContent = '…';\n"
        "    $('loading-fill').style.width = '0%';\n"
        "    $('loading-bar').removeAttribute('aria-valuenow');\n"
        "    $('loading-bar').setAttribute('aria-valuetext', 'Подготовка Android 10');\n"
        '    if (startupTimer) clearInterval(startupTimer);\n'
        '    startupTimer = setInterval(function () {\n'
        "      if (!$('startup').classList.contains('on')) return;\n"
        '      var seconds = Math.floor((performance.now() - startupAt) / 1000);\n'
        "      if (startupStage === 'download') {\n"
        '        var loaded = loadParts.system.loaded + loadParts.wasm.loaded;\n'
        '        var total = loadParts.system.total + loadParts.wasm.total;\n'
        "        $('loading-count').textContent = 'Загружено ' + formatMB(loaded) + (total ? ' / ' + formatMB(total) + ' МБ' : ' МБ') + ' · ' + seconds + ' с';\n"
        '        return;\n'
        '      }\n'
        "      $('loading-count').textContent = (startupStage === 'runtime' ? 'Запуск приложения' : 'Подготовка Android') + ' · ' + seconds + ' с';\n"
        '    }, 1000);\n'
        '  }\n'
        '  function hideStartup() {\n'
        "    $('startup').classList.remove('on', 'indeterminate');\n"
        '    if (startupTimer) clearInterval(startupTimer);\n'
        '    startupTimer = 0;\n'
        '  }\n'
        '  function updateStartup(info) {\n'
        '    if (!info || !loadParts[info.phase]) return;\n'
        "    startupStage = 'download';\n"
        '    var part = loadParts[info.phase];\n'
        '    part.loaded = Math.max(part.loaded, Number(info.loaded) || 0);\n'
        '    part.total = Math.max(part.total, Number(info.total) || 0);\n'
        '    var loaded = loadParts.system.loaded + loadParts.wasm.loaded;\n'
        '    var total = loadParts.system.total + loadParts.wasm.total;\n'
        '    var determinate = loadParts.system.total > 0 && loadParts.wasm.total > 0;\n'
        '    var percent = determinate ? Math.min(99, Math.floor(loaded * 100 / total)) : 0;\n'
        "    $('loading-title').textContent = 'Запуск ' + startupPackage;\n"
        "    $('loading-detail').textContent = 'Система ' + formatMB(loadParts.system.loaded) + ' МБ · WebAssembly ' + formatMB(loadParts.wasm.loaded) + ' МБ';\n"
        "    $('loading-count').textContent = determinate ? formatMB(loaded) + ' / ' + formatMB(total) + ' МБ загружено' : 'Загружено ' + formatMB(loaded) + ' МБ';\n"
        "    $('loading-percent').textContent = determinate ? percent + '%' : '…';\n"
        "    $('loading-fill').style.width = percent + '%';\n"
        '    if (determinate) {\n'
        "      $('startup').classList.remove('indeterminate');\n"
        "      $('loading-bar').setAttribute('aria-valuenow', String(percent));\n"
        "      $('loading-bar').setAttribute('aria-valuetext', percent + '% · ' + formatMB(loaded) + ' из ' + formatMB(total) + ' МБ');\n"
        '    } else {\n'
        "      $('startup').classList.add('indeterminate');\n"
        "      $('loading-bar').removeAttribute('aria-valuenow');\n"
        "      $('loading-bar').setAttribute('aria-valuetext', formatMB(loaded) + ' МБ загружено');\n"
        '    }\n'
        '  }\n'
        '  function appStarted() {\n'
        "    startupStage = 'runtime';\n"
        "    $('loading-title').textContent = 'Запускается приложение…';\n"
        "    $('loading-detail').textContent = 'Файлы загружены — запускается Android';\n"
        "    $('loading-percent').textContent = '100%';\n"
        "    $('loading-fill').style.width = '100%';\n"
        "    $('startup').classList.remove('indeterminate');\n"
        "    $('loading-count').textContent = 'Запуск приложения · 0 с';\n"
        "    $('loading-bar').setAttribute('aria-valuenow', '100');\n"
        "    $('loading-bar').setAttribute('aria-valuetext', 'Файлы загружены, запускается приложение');\n"
        '  }\n'
        '  function start(pkg) {\n'
        '    showStartup(pkg);',
        'startup progress state',
    )
    text = replace_once(
        text,
        '  function stop(why) {\n    if (!frame) return;',
        '  function stop(why) {\n    hideStartup();\n    if (!frame) return;',
        'hide startup progress when closing the VM',
    )
    text = replace_once(
        text,
        "    var t = ((performance.now() - startedAt) / 1000).toFixed(1) + ' с';\n    if (m.event) {\n      log(t + ' SENSES: ' + m.event);",
        "    var t = ((performance.now() - startedAt) / 1000).toFixed(1) + ' с';\n"
        "    if (m.progress) { updateStartup(m.progress); return; }\n"
        "    if (m.event) {\n"
        "      log(t + ' SENSES: ' + m.event);\n"
        "      if (/^app started /.test(m.event)) appStarted();\n"
        "      if (/^app shown /.test(m.event)) hideStartup();",
        'handle VM download and launch progress',
    )
    if '__VERSION__' in text:
        text = text.replace('__VERSION__', version)
    return text


def patch_run_page(source: str) -> str:
    text = replace_once(
        source,
        """  function post(msg) {
    try { parent.postMessage(Object.assign({ senses: true }, msg), location.origin); } catch (e) {}
  }""",
        """  function post(msg) {
    try { parent.postMessage(Object.assign({ senses: true }, msg), location.origin); } catch (e) {}
  }
  function reportProgress(phase, loaded, total) {
    post({ progress: { phase: phase, loaded: loaded, total: total } });
  }
  function progressResponse(response, phase) {
    if (!response.ok) return response;
    var total = Number(response.headers.get('Content-Length')) || 0;
    reportProgress(phase, 0, total);
    if (!response.body) {
      reportProgress(phase, total, total);
      return response;
    }
    var reader = response.body.getReader(), loaded = 0, reported = 0;
    var body = new ReadableStream({
      pull: function (controller) {
        return reader.read().then(function (chunk) {
          if (chunk.done) {
            if (loaded !== reported) reportProgress(phase, loaded, total || loaded);
            controller.close();
            return;
          }
          loaded += chunk.value.byteLength;
          if (loaded - reported >= 1048576 || (total && loaded >= total)) {
            reported = loaded;
            reportProgress(phase, loaded, total);
          }
          controller.enqueue(chunk.value);
        }).catch(function (error) { controller.error(error); });
      },
      cancel: function (reason) { return reader.cancel(reason); }
    });
    return new Response(body, {
      status: response.status,
      statusText: response.statusText,
      headers: new Headers(response.headers)
    });
  }""",
        'VM download progress messaging',
    )
    text = replace_once(
        text,
        """  function blobOf(r) {
    if (!r.body) return r.blob();
    var reader = r.body.getReader(), blob = new Blob([]), parts = [], inParts = 0;
    function flush() {
      blob = new Blob([blob].concat(parts));
      parts.forEach(function (p) { drop(p.buffer); });
      parts = []; inParts = 0;
    }
    return (function pump() {
      return reader.read().then(function (c) {
        if (c.done) { flush(); return blob; }
        parts.push(c.value);
        inParts += c.value.length;
        if (inParts >= 8 << 20) flush();
        return pump();
      });
    })();
  }""",
        """  function blobOf(r, phase) {
    var total = Number(r.headers.get('Content-Length')) || 0;
    if (!r.body) return r.blob().then(function (blob) {
      if (phase) reportProgress(phase, blob.size, total || blob.size);
      return blob;
    });
    var reader = r.body.getReader(), blob = new Blob([]), parts = [], inParts = 0, loaded = 0, reported = 0;
    if (phase) reportProgress(phase, 0, total);
    function flush() {
      blob = new Blob([blob].concat(parts));
      parts.forEach(function (p) { drop(p.buffer); });
      parts = []; inParts = 0;
    }
    return (function pump() {
      return reader.read().then(function (c) {
        if (c.done) {
          flush();
          if (phase && loaded !== reported) reportProgress(phase, loaded, total || loaded);
          return blob;
        }
        parts.push(c.value);
        inParts += c.value.byteLength;
        loaded += c.value.byteLength;
        if (phase && (loaded - reported >= 1048576 || (total && loaded >= total))) {
          reported = loaded;
          reportProgress(phase, loaded, total);
        }
        if (inParts >= 8 << 20) flush();
        return pump();
      });
    })();
  }""",
        'system image download progress',
    )
    text = replace_once(
        text,
        r"    var compiled = WebAssembly.compileStreaming(fetch(s.script.replace(/\.js$/, '.wasm')));",
        r"    var compiled = WebAssembly.compileStreaming(fetch(s.script.replace(/\.js$/, '.wasm')).then(function (response) { return progressResponse(response, 'wasm'); }));",
        'wasm download progress',
    )
    text = replace_once(
        text,
        "      return blobOf(r).then(function (b) {",
        "      return blobOf(r, 'system').then(function (b) {",
        'cached system image progress',
    )
    text = replace_once(
        text,
        '      return asBlob ? blobOf(r) : r.arrayBuffer();',
        "      return asBlob ? blobOf(r, 'system') : r.arrayBuffer();",
        'system image progress',
    )
    return text


def build(zip_path: Path, checksum_path: Path, output: Path) -> None:
    zip_path = zip_path.resolve()
    checksum_path = checksum_path.resolve()
    output = output.resolve()
    if output == ROOT or output in ROOT.parents:
        fail('Refusing to use the repository or one of its parent directories as the output path')
    verify_release(zip_path, checksum_path)
    marker = output / '.senses-pages-output'
    if output.exists():
        if not marker.is_file():
            fail(f'Refusing to replace an unrecognized output directory: {output}')
        shutil.rmtree(output)
    output.mkdir(parents=True)
    marker.touch()

    with zipfile.ZipFile(zip_path) as archive:
        infos = {info.filename: info for info in archive.infolist() if not info.is_dir()}
        root = archive_root(archive)
        release_prefix = root + '/'
        version = read_member(archive, infos, release_prefix + 'site/VERSION').decode('utf-8').strip()
        stacks = json.loads(read_member(archive, infos, release_prefix + 'site/stacks.json'))
        if not version or 'a10' not in stacks.get('stacks', []):
            fail('The selected release does not contain the Android 10 web runtime')

        copied_vm_files = set()
        vm_prefix = release_prefix + 'site/a10/'
        for name, info in infos.items():
            if not name.startswith(vm_prefix):
                continue
            relative = safe_relative_path(name[len(vm_prefix):])
            copy_member(archive, info, output / 'vm' / 'a10' / relative)
            copied_vm_files.add(relative.as_posix())
        missing = REQUIRED_VM_FILES - copied_vm_files
        if missing:
            fail('The Android 10 release is missing: ' + ', '.join(sorted(missing)))

        for name in ('LICENSE.txt', 'NOTICE.txt'):
            copy_member(archive, infos[release_prefix + name], output / name)
        license_prefix = release_prefix + 'licenses/'
        for name, info in infos.items():
            if name.startswith(license_prefix):
                relative = safe_relative_path(name[len(release_prefix):])
                copy_member(archive, info, output / relative)
        (output / 'vm' / 'VERSION').write_text(version + '\n', encoding='utf-8')
        shutil.copy2(ROOT / 'pages' / 'favicon.svg', output / 'favicon.svg')

    split_data_file(output / 'vm' / 'a10' / 'artvm10.data', version)

    vm_dir = output / 'vm'
    for name in ('apk.js', 'library-worker.js'):
        shutil.copy2(ROOT / 'demo' / 'senses-apps-vm' / name, vm_dir / name)
    run_source = (ROOT / 'demo' / 'senses-apps-vm' / 'run.html').read_text(encoding='utf-8')
    (vm_dir / 'run.html').write_text(patch_run_page(run_source), encoding='utf-8')
    shutil.copy2(ROOT / 'shim' / 'opfs' / 'idb_opfs.js', vm_dir / 'idb_opfs.js')
    for name in REQUIRED_SHELL_FILES:
        source = ROOT / 'pages' / 'vm' / name
        shutil.copy2(source, vm_dir / name)

    index_source = (ROOT / 'demo' / 'senses-apps-vm' / 'index.html').read_text(encoding='utf-8')
    (vm_dir / 'index.html').write_text(patch_vm_page(index_source, version), encoding='utf-8')

    landing = (ROOT / 'pages' / 'index.html').read_text(encoding='utf-8')
    landing = replace_once(landing, '__VERSION__', version, 'landing page release version')
    (output / 'index.html').write_text(landing, encoding='utf-8')

    worker = (ROOT / 'pages' / 'vm' / 'sw.js').read_text(encoding='utf-8')
    build_id = os.environ.get('GITHUB_RUN_ID') or os.environ.get('GITHUB_SHA', 'local')[:12]
    worker = replace_once(worker, '__BUILD_VERSION__', f'{version}-{build_id}', 'service worker build version')
    (vm_dir / 'sw.js').write_text(worker, encoding='utf-8')

    (output / '.nojekyll').write_text('', encoding='utf-8')
    marker.unlink()
    total_size = sum(path.stat().st_size for path in output.rglob('*') if path.is_file())
    if total_size >= 1_000_000_000:
        fail(f'Prepared Pages site exceeds 1 GB: {total_size} bytes')
    print(f'Prepared Android {version} GitHub Pages site: {total_size / 1_000_000:.1f} MB at {output}')


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('release_zip', type=Path)
    parser.add_argument('checksum_file', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    build(args.release_zip, args.checksum_file, args.out)


if __name__ == '__main__':
    main()
