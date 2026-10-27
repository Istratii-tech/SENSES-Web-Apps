// SENSES Web Apps: files between apps and the phone (topic app-files,
// docs/topics/app-files.md). The VM has no file picker, gallery or share
// sheet: arcwasm.server.PageFiles asks the page, on stdout, and this is the
// page's side, the same in Safari on iPhones and in desktop browsers.
//
//   SensesFiles.app({send, dataDir, name})   run.html: -> {watch(line) -> bool}
//     open   "SENSES: file pick ID MULTIPLE MIMES": a sheet with its own
//            button (iOS opens a file picker only from a tap), the browser's
//            picker (on an iPhone: Files, or Photos for images) with the
//            MIME filter, the files sent into the VM in pieces through
//            send(NAME, Uint8Array) (Module.arcwasmInput.sendFile), each piece
//            once the VM has written the one before ("file got")
//     save   "SENSES: file save ID SIZE MIME PATH": the file read from the
//            VM's storage (PATH under /data is dataDir() + the rest) and
//            offered: the share sheet (iPhone: «Сохранить в Файлы»), a save
//            dialog (showSaveFilePicker: Chrome, Edge) or a download
//     text   "SENSES: file text ID TEXT": shared text, to share on or copy
//   SensesFiles.library({remote})           index.html: «Файлы», each system's
//            shared storage (/sdcard = senses/data/STACK/media/0): look,
//            add files from the phone, save one out, remove
//
// Space: a copy into the VM is checked against the origin's free space first
// (Safari 16 gives a site about 1000 MB for everything, and stops at the
// quota: docs/topics/opfs-after-kill.md).
(function (root) {
  'use strict';
  var S = root.Senses, $ = S.$, store = root.SensesStore;
  var MB = 1048576, PIECE = MB, WINDOW = 3, MARGIN = 64 * MB, STALL_MS = 30000;

  function el(tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  }
  function button(text, cls, onclick) {
    var b = el('button', cls || null, text);
    b.type = 'button';
    if (onclick) b.onclick = onclick;
    return b;
  }

  // --- what the browser can do -------------------------------------------------------
  // A phone or tablet (touch, no fine pointer): the share sheet saves to Files.
  function touch() {
    return (navigator.maxTouchPoints || 0) > 1 && !(window.matchMedia && matchMedia('(pointer: fine)').matches);
  }
  function canShareFiles(files) {
    try { return !!(navigator.canShare && navigator.share && navigator.canShare({files: files})); } catch (e) { return false; }
  }
  // How a file goes out: 'share', 'picker' (save dialog) or 'download'.
  function saveWay(files) {
    if (touch() && canShareFiles(files)) return 'share';
    if (window.showSaveFilePicker && files.length === 1) return 'picker';
    return 'download';
  }
  var SAVE_LABEL = {share: 'Сохранить в Файлы', picker: 'Сохранить…', download: 'Скачать'};
  function download(file) {
    var a = el('a');
    a.href = URL.createObjectURL(file);
    a.download = file.name;
    a.rel = 'noopener';
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(function () { URL.revokeObjectURL(a.href); }, 60000);
  }
  // -> a promise; rejects with AbortError when the user backs out.
  function save(files, way) {
    if (way === 'share') return navigator.share({files: files});
    if (way === 'picker') {
      var f = files[0], ext = (/\.[^.]{1,10}$/.exec(f.name) || [''])[0];
      var types = f.type && ext ? [{description: f.type, accept: (function (o) { o[f.type] = [ext]; return o; })({})}] : undefined;
      return window.showSaveFilePicker({suggestedName: f.name, types: types}).then(function (h) {
        return h.createWritable();
      }).then(function (w) {
        return w.write(f).then(function () { return w.close(); });
      });
    }
    files.forEach(download);
    return Promise.resolve();
  }

  // Free space of the origin (null: unknown). navigator.storage.estimate where
  // there is one (Chrome, Safari 17+); Safari 16 has none: the store measures
  // (its quota assumed 1000 MB).
  function freeSpace() {
    return store.usage().then(function (u) {
      if (u && u.quota) return Math.max(0, u.quota - (u.usage || 0));
      return Promise.race([store.storage().then(function (st) { return st.free; }),
                           new Promise(function (ok) { setTimeout(ok, 8000, null); })]);
    }).catch(function () { return null; });
  }
  // -> null if |bytes| fit, else the text to show.
  function spaceFor(bytes) {
    return freeSpace().then(function (free) {
      if (free == null || bytes + MARGIN <= free) return null;
      return 'Не хватает места: нужно ' + S.size(bytes) + ', свободно ' + S.size(Math.max(0, free - MARGIN)) +
             '. Браузер даёт SENSES ограниченное место (Safari — около 1000 МБ на всё): удалите ненужные ' +
             'файлы, приложения или их данные в библиотеке.';
    });
  }

  // --- MIME types -> the picker's accept ---------------------------------------------------
  var EXT = {
    'application/pdf': '.pdf', 'application/zip': '.zip', 'application/json': '.json', 'application/xml': '.xml',
    'application/epub+zip': '.epub', 'application/vnd.android.package-archive': '.apk', 'text/plain': '.txt',
    'text/html': '.html,.htm', 'text/csv': '.csv', 'text/xml': '.xml', 'text/markdown': '.md',
    'application/msword': '.doc', 'application/rtf': '.rtf', 'application/x-sqlite3': '.db,.sqlite',
    'application/vnd.openxmlformats-officedocument.wordprocessingml.document': '.docx',
    'application/vnd.openxmlformats-officedocument.spreadsheetml.sheet': '.xlsx',
    'application/vnd.openxmlformats-officedocument.presentationml.presentation': '.pptx',
    'application/vnd.ms-excel': '.xls', 'application/vnd.oasis.opendocument.text': '.odt',
    'application/x-zip-compressed': '.zip', 'application/gzip': '.gz', 'application/x-tar': '.tar',
  };
  // '' (any file) when a type is a wildcard or one the browser might not map:
  // an iPhone greys out files whose type it does not know.
  function acceptFor(mimes) {
    var out = [];
    for (var i = 0; i < mimes.length; i++) {
      var t = String(mimes[i]).toLowerCase();
      if (t === '*/*' || t === 'application/*' || t === 'application/octet-stream') return '';
      if (/^(image|video|audio|text)\/\*$/.test(t)) out.push(t);
      else if (/^(image|video|audio)\//.test(t)) out.push(t);
      else if (EXT[t]) out.push(t, EXT[t]);
      else return '';
    }
    return out.join(',');
  }
  function describe(mimes) {
    // (an app that also takes any file: no list of what it prefers)
    if (!acceptFor(mimes)) return 'любой файл';
    var words = {image: 'изображение', video: 'видео', audio: 'аудио', text: 'текст'};
    var names = mimes.map(function (t) {
      if (t === '*/*') return 'любой файл';
      var m = /^(\w+)\/(.+)$/.exec(t);
      if (!m) return t;
      if (m[2] === '*') return words[m[1]] || t;
      return (EXT[t] ? EXT[t].split(',')[0].slice(1).toUpperCase() : m[2].replace(/^x-|^vnd\./, '')) +
             (words[m[1]] ? ' (' + words[m[1]] + ')' : '');
    });
    names = names.filter(function (n, i) { return names.indexOf(n) === i; });
    return names.length > 4 ? names.slice(0, 4).join(', ') + ' и другие' : names.join(', ');
  }

  // --- the app's page ------------------------------------------------------------------
  function app(opts) {
    var pick = null;   // {id, multiple, mimes, files, sent, acked, ...}
    var saving = null; // {files: [File], ids: {}}

    function pickSheet() {
      if ($('filepick')) return $('filepick');
      var s = el('div', 'sheet files-sheet');
      s.id = 'filepick';
      s.hidden = true;
      var box = el('div', 'box');
      box.appendChild(el('h2', null, 'Выбор файла'));
      var note = el('p', 'files-note');
      note.id = 'fpnote';
      box.appendChild(note);
      var bar = el('div', 'bar');
      bar.id = 'fpbar';
      bar.hidden = true;
      bar.appendChild(el('div'));
      box.appendChild(bar);
      var st = el('p', 'files-status');
      st.id = 'fpstatus';
      box.appendChild(st);
      var go = button('Выбрать файл', 'primary', choose);
      go.id = 'fpchoose';
      box.appendChild(go);
      var no = button('Отмена', 'close', cancel);
      no.id = 'fpcancel';
      box.appendChild(no);
      s.appendChild(box);
      document.body.appendChild(s);
      return s;
    }
    function status(text, error) {
      $('fpstatus').textContent = text || '';
      $('fpstatus').classList.toggle('error', !!error);
    }

    function asked(id, multiple, mimes) {
      if (pick && !pick.done) opts.send('arcwasm-files:cancel:' + pick.id, new Uint8Array(1));
      pick = {id: id, multiple: multiple, mimes: mimes, accept: acceptFor(mimes)};
      pickSheet();
      var label = typeof opts.name === 'function' ? opts.name() : opts.name;
      $('fpnote').textContent = (label ? '«' + label + '»' : 'Приложение') + ' просит ' +
          (multiple ? 'файлы' : 'файл') + ': ' + describe(mimes) + '.';
      $('fpchoose').textContent = multiple ? 'Выбрать файлы' : 'Выбрать файл';
      $('fpchoose').hidden = false;
      $('fpcancel').textContent = 'Отмена';
      $('fpbar').hidden = true;
      status('');
      S.open('filepick', true);
      S.log('Files: the app asks for ' + (multiple ? 'files' : 'a file') + ' (' + mimes.join(',') + ')');      // (tests on phones nobody holds: ?testpick=NAME answers with
      // tools/serve.py's /__test/NAME, as if picked there)
      if (opts.testPick) testFile(opts.testPick).then(function (f) { take([f]); }, function (e) {
        S.log('Files: test file: ' + e);
      });
    }
    // From the button's tap: the browser's picker, a fresh input each time.
    function choose() {
      if (!pick || pick.busy) return;
      var input = el('input');
      input.type = 'file';
      input.multiple = !!pick.multiple;
      if (pick.accept) input.accept = pick.accept;
      input.style.display = 'none';
      input.onchange = function () {
        var files = Array.prototype.slice.call(input.files || []);
        input.remove();
        if (files.length) take(files);
      };
      $('filepick').appendChild(input);
      input.click();
    }
    function cancel() {
      if (pick && !pick.done) {
        pick.done = true;
        opts.send('arcwasm-files:cancel:' + pick.id, new Uint8Array(1));
        S.log('Files: cancelled');
      }
      S.close('filepick');
    }

    function take(files) {
      var p = pick, total = files.reduce(function (n, f) { return n + f.size; }, 0);
      p.busy = true;
      $('fpchoose').hidden = true;
      status('Проверяю место…');
      spaceFor(total).then(function (problem) {
        if (problem) {
          p.busy = false;
          $('fpchoose').hidden = false;
          status(problem, true);
          S.log('Files: no space for ' + Math.round(total / MB) + ' MB');
          return;
        }
        S.log('Files: sending ' + files.length + ' file(s), ' + total + ' bytes');
        p.files = files;
        p.total = total;
        p.pieces = [];
        files.forEach(function (f, i) {
          for (var at = 0; at < f.size; at += PIECE) p.pieces.push({i: i, at: at, end: Math.min(f.size, at + PIECE)});
        });
        p.sent = 0;
        p.acked = 0;
        p.ackAt = Date.now();
        var meta = files.map(function (f) { return {name: f.name, type: f.type || '', size: f.size}; });
        S.bar($('fpbar'), 0);
        status('Копирую в приложение…');
        send(p, 'arcwasm-files:begin:' + p.id, new TextEncoder().encode(JSON.stringify(meta))).then(function () {
          pump(p);
          watchStall(p);
        }, function (e) { failed(p, e); });
      });
    }
    // send() of the runtime, again while it is not taking (memory short, not up).
    function send(p, name, bytes) {
      return new Promise(function (ok, no) {
        var tries = 0;
        (function again() {
          if (p.done) return no(new Error('cancelled'));
          if (opts.send(name, bytes)) return ok();
          if (++tries > 40) return no(new Error('the system does not take the file'));
          setTimeout(again, 250);
        })();
      });
    }
    // Up to WINDOW pieces beyond what the VM has written.
    function pump(p) {
      if (p.done || p.pumping) return;
      if (!p.pieces.length) {
        if (p.acked >= p.total && !p.ended) {
          p.ended = true;
          send(p, 'arcwasm-files:end:' + p.id, new Uint8Array(1)).catch(function (e) { failed(p, e); });
        }
        return;
      }
      if (p.sent - p.acked >= WINDOW * PIECE) return;
      var piece = p.pieces.shift(), f = p.files[piece.i];
      p.pumping = true;
      f.slice(piece.at, piece.end).arrayBuffer().then(function (buf) {
        return send(p, 'arcwasm-files:part:' + p.id + ':' + piece.i + ':' + piece.at, new Uint8Array(buf));
      }).then(function () {
        p.pumping = false;
        p.sent += piece.end - piece.at;
        pump(p);
      }, function (e) { p.pumping = false; failed(p, e); });
    }
    function watchStall(p) {
      var timer = setInterval(function () {
        if (p.done) { clearInterval(timer); return; }
        if (Date.now() - p.ackAt > STALL_MS) { clearInterval(timer); failed(p, new Error('the system stopped taking it')); }
      }, 2000);
    }
    function got(id, bytes) {
      var p = pick;
      if (!p || p.id !== id) return;
      p.acked = bytes;
      p.ackAt = Date.now();
      S.bar($('fpbar'), p.total ? Math.min(1, bytes / p.total) : 1);
      status('Копирую в приложение… ' + S.size(bytes) + ' из ' + S.size(p.total));
      pump(p);
    }
    function failed(p, e) {
      if (p.done) return;
      p.done = true;
      S.log('Files: copy failed: ' + (e && e.message || e));
      opts.send('arcwasm-files:cancel:' + p.id, new Uint8Array(1));
      $('fpbar').hidden = true;
      $('fpchoose').hidden = true;
      $('fpcancel').textContent = 'Закрыть';
      status('Не удалось передать файл: ' + (e && e.message || e), true);
    }
    function done(id, count) {
      if (!pick || pick.id !== id) return;
      var p = pick;
      if (p.failedText) return;
      p.done = true;
      if (count > 0) S.log('Files: the app has ' + count + ' file(s)');
      if (!$('filepick').hidden && !$('fpstatus').classList.contains('error')) S.close('filepick');
    }
    function vmFailed(id, reason) {
      if (!pick || pick.id !== id) {
        S.log('Files: ' + reason);
        return;
      }
      pick.failedText = reason;
      var p = pick;
      p.done = true;
      $('fpbar').hidden = true;
      $('fpchoose').hidden = true;
      $('fpcancel').textContent = 'Закрыть';
      status('Система не смогла записать файл: ' + (/ENOSPC|No space/i.test(reason) ? 'нет места.' : reason), true);
    }

    // --- out of the app ---
    function saveSheet() {
      if ($('filesave')) return $('filesave');
      var s = el('div', 'sheet files-sheet');
      s.id = 'filesave';
      s.hidden = true;
      var box = el('div', 'box');
      var h = el('h2', null, 'Файл из приложения');
      h.id = 'fstitle';
      box.appendChild(h);
      var list = el('ul', 'files-list');
      list.id = 'fslist';
      box.appendChild(list);
      var text = el('pre', 'files-text');
      text.id = 'fstext';
      text.hidden = true;
      box.appendChild(text);
      var st = el('p', 'files-status');
      st.id = 'fsstatus';
      box.appendChild(st);
      var go = button('', 'primary');
      go.id = 'fsgo';
      box.appendChild(go);
      var alt = button('Скачать');
      alt.id = 'fsalt';
      box.appendChild(alt);
      box.appendChild(button('Закрыть', 'close', function () { S.close('filesave'); saving = null; }));
      s.appendChild(box);
      document.body.appendChild(s);
      return s;
    }
    function offered(id, size, type, path) {
      var dir = opts.dataDir && opts.dataDir();
      var name = path.slice(path.lastIndexOf('/') + 1);
      S.log('Files: the app offers ' + name + ' (' + size + ' bytes, ' + type + ')');
      if (!dir || path.indexOf('/data/') !== 0) return;
      saveSheet();
      if ($('filesave').hidden || !saving || saving.text != null) saving = {files: [], names: {}};
      var s = saving;
      $('fstitle').textContent = 'Файл из приложения';
      $('fstext').hidden = true;
      $('fsstatus').textContent = 'Готовлю…';
      $('fsstatus').classList.remove('error');
      S.open('filesave');
      store.fileAt(dir + path.slice(5), name, type === 'application/octet-stream' ? '' : type).then(function (file) {
        if (saving !== s) return;
        if (s.names[path] != null) s.files[s.names[path]] = file;
        else { s.names[path] = s.files.length; s.files.push(file); }
        showFiles(s);
      }, function (e) {
        S.log('Files: cannot read ' + path + ': ' + e);
        if (saving === s) { $('fsstatus').textContent = 'Файл не прочитать: ' + e.message; $('fsstatus').classList.add('error'); }
      });
    }
    function showFiles(s) {
      var list = $('fslist');
      list.textContent = '';
      s.files.forEach(function (f) {
        var li = el('li');
        li.appendChild(el('span', 'name', f.name));
        li.appendChild(el('span', 'meta', S.size(f.size)));
        list.appendChild(li);
      });
      $('fstitle').textContent = s.files.length > 1 ? 'Файлы из приложения' : 'Файл из приложения';
      var way = saveWay(s.files);
      $('fsgo').textContent = SAVE_LABEL[way];
      $('fsgo').hidden = false;
      $('fsalt').hidden = way === 'download';
      $('fsstatus').textContent = '';
      $('fsgo').onclick = function () { out(s, way); };
      $('fsalt').onclick = function () { out(s, 'download'); };
    }
    function out(s, way) {
      save(s.files.slice(), way).then(function () {
        S.log('Files: saved out (' + way + ')');
        if (way !== 'download') { S.close('filesave'); saving = null; }
        else $('fsstatus').textContent = 'Загрузка начата.';
      }, function (e) {
        if (e && e.name === 'AbortError') return;
        S.log('Files: save (' + way + '): ' + e);
        $('fsstatus').textContent = 'Не получилось: ' + (e && e.message || e) + '. Попробуйте «Скачать».';
        $('fsalt').hidden = false;
      });
    }
    function text(id, encoded) {
      var t;
      try { t = decodeURIComponent(encoded); } catch (e) { t = encoded; }
      S.log('Files: the app shares text (' + t.length + ' chars)');
      saveSheet();
      saving = {text: t};
      $('fstitle').textContent = 'Текст из приложения';
      $('fslist').textContent = '';
      $('fstext').hidden = false;
      $('fstext').textContent = t.length > 600 ? t.slice(0, 600) + '…' : t;
      $('fsstatus').textContent = '';
      $('fsstatus').classList.remove('error');
      var canShare = !!navigator.share;
      $('fsgo').textContent = canShare ? 'Поделиться' : 'Скопировать';
      $('fsgo').hidden = false;
      $('fsalt').textContent = 'Скопировать';
      $('fsalt').hidden = !canShare;
      $('fsgo').onclick = function () { canShare ? shareText(t) : copy(t); };
      $('fsalt').onclick = function () { copy(t); };
      S.open('filesave');
    }
    function shareText(t) {
      navigator.share({text: t}).then(function () { S.close('filesave'); }, function (e) {
        if (!e || e.name !== 'AbortError') copy(t);
      });
    }
    function copy(t) {
      var ok = function () { $('fsstatus').textContent = 'Скопировано.'; };
      if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(t).then(ok, function () { copyOld(t) ? ok() : null; });
      } else if (copyOld(t)) ok();
    }
    function copyOld(t) {
      var a = el('textarea');
      a.value = t;
      a.style.position = 'fixed';
      a.style.opacity = '0';
      document.body.appendChild(a);
      a.select();
      var ok = false;
      try { ok = document.execCommand('copy'); } catch (e) {}
      a.remove();
      return ok;
    }

    var RE = /^SENSES: file (\w+) (\S+)(?: (.*))?$/;
    return {
      // A line of the VM's: true if it was one of these.
      watch: function (line) {
        var m = RE.exec(line);
        if (!m) return false;
        var rest = m[3] || '';
        if (m[1] === 'pick') {
          var a = rest.split(' ');
          asked(m[2], a[0] === '1', (a[1] || '*/*').split(',').filter(Boolean));
        } else if (m[1] === 'got') got(m[2], +rest);
        else if (m[1] === 'done') done(m[2], +rest);
        else if (m[1] === 'failed') vmFailed(m[2], rest);
        else if (m[1] === 'save') {
          var b = /^(\d+) (\S+) (\/.*)$/.exec(rest);
          if (b) offered(m[2], +b[1], b[2], b[3]);
        } else if (m[1] === 'text') text(m[2], rest);
        return true;
      },
      cancel: cancel,
    };
  }

  function testFile(name) {
    return fetch('/__test/' + name, {cache: 'no-store'}).then(function (r) {
      if (!r.ok) throw new Error('HTTP ' + r.status);
      return r.blob();
    }).then(function (b) { return new File([b], name, {type: b.type || ''}); });
  }

  // --- the library: each system's shared storage ---------------------------------------
  var lib = {stack: null, dir: 'Download/', busy: false};
  function rootOf(stack) { return 'senses/data/' + stack + '/media/0/'; }
  function libSheet() {
    if ($('files')) return $('files');
    var s = el('div', 'sheet files-sheet');
    s.id = 'files';
    s.hidden = true;
    var box = el('div', 'box');
    box.appendChild(el('h2', null, 'Файлы'));
    var note = el('p', 'files-note', 'Общая память (/sdcard) каждой системы: приложения видят эти файлы, например ' +
                  'в папке Download. Сюда можно положить файлы с телефона и забрать их обратно.');
    note.id = 'flnote';
    box.appendChild(note);
    var stacks = el('div', 'row files-stacks');
    stacks.id = 'flstacks';
    box.appendChild(stacks);
    var path = el('div', 'files-path');
    var up = button('‹', 'files-up', function () { go(lib.dir.replace(/[^/]+\/$/, '')); });
    up.id = 'flup';
    up.setAttribute('aria-label', 'Вверх');
    path.appendChild(up);
    var where = el('span', null, '');
    where.id = 'flwhere';
    path.appendChild(where);
    box.appendChild(path);
    var list = el('ul', 'files-list');
    list.id = 'fllist';
    box.appendChild(list);
    var bar = el('div', 'bar');
    bar.id = 'flbar';
    bar.hidden = true;
    bar.appendChild(el('div'));
    box.appendChild(bar);
    var st = el('p', 'files-status');
    st.id = 'flstatus';
    box.appendChild(st);
    box.appendChild(button('Добавить файлы с телефона', 'primary', addFiles));
    box.appendChild(button('Закрыть', 'close', function () { S.close('files'); }));
    s.appendChild(box);
    document.body.appendChild(s);
    return s;
  }
  function libStatus(text, error) {
    $('flstatus').textContent = text || '';
    $('flstatus').classList.toggle('error', !!error);
  }
  function library(opts) {
    opts = opts || {};
    libSheet();
    var row = $('flstacks');
    row.textContent = '';
    var ids = S.STACK_ORDER || Object.keys(S.STACKS);
    if (!lib.stack) lib.stack = S.DEFAULT_STACK || ids[0];
    ids.forEach(function (id) {
      var b = button(S.STACKS[id].short, lib.stack === id ? 'on' : '', function () {
        lib.stack = id;
        Array.prototype.forEach.call(row.children, function (c) { c.classList.toggle('on', c === b); });
        go('Download/');
      });
      b.title = S.STACKS[id].name;
      row.appendChild(b);
    });
    var remote = opts.remote || 0;
    $('flnote').textContent = 'Общая память (/sdcard) каждой системы: приложения видят эти файлы, например в ' +
        'папке Download. Сюда можно положить файлы с телефона и забрать их обратно.' +
        (remote ? ' Приложения в отдельных хранилищах (' + remote + ') держат файлы у себя: там файлы передаются ' +
                  'из самого приложения («Открыть», «Сохранить», «Поделиться»).' : '');
    libStatus('');
    S.open('files');
    go(lib.dir);
  }
  function go(dir) {
    lib.dir = dir;
    $('flwhere').textContent = S.STACKS[lib.stack].name + ' · /sdcard' + (dir ? '/' + dir.replace(/\/$/, '') : '');
    $('flup').disabled = !dir;
    var list = $('fllist');
    list.textContent = '';
    list.appendChild(el('li', 'files-none', 'Читаю…'));
    store.listDir(rootOf(lib.stack) + dir).then(function (entries) {
      if (lib.dir !== dir) return;
      list.textContent = '';
      entries = entries.filter(function (e) { return !/\.part$/.test(e.name); });
      entries.sort(function (a, b) {
        return (a.kind === b.kind ? 0 : a.kind === 'directory' ? -1 : 1) || a.name.localeCompare(b.name, 'ru');
      });
      if (!entries.length) list.appendChild(el('li', 'files-none', 'Пусто'));
      entries.forEach(function (e) { list.appendChild(row(e, dir)); });
    }, function (e) {
      list.textContent = '';
      libStatus('Не прочитать папку: ' + e.message, true);
    });
  }
  function row(e, dir) {
    var li = el('li', e.kind === 'directory' ? 'dir' : 'file');
    var name = el('span', 'name', e.kind === 'directory' ? e.name + '/' : e.name);
    li.appendChild(name);
    if (e.kind === 'directory') {
      li.onclick = function () { go(dir + e.name + '/'); };
      return li;
    }
    li.appendChild(el('span', 'meta', S.size(e.size) + (e.mtime ? ' · ' + new Date(e.mtime).toLocaleDateString('ru') : '')));
    var path = rootOf(lib.stack) + dir + e.name, acts = null;
    li.onclick = function (ev) {
      if (acts) { if (ev.target === li || ev.target === name) { acts.remove(); acts = null; } return; }
      acts = el('div', 'row files-acts');
      var bOut = button('…', 'primary'), bDl = button('Скачать'), bDel = button('Удалить', 'danger');
      bOut.disabled = true;
      acts.appendChild(bOut);
      acts.appendChild(bDl);
      acts.appendChild(bDel);
      li.appendChild(acts);
      var file = null;
      store.fileAt(path, e.name).then(function (f) {
        file = f;
        var way = saveWay([f]);
        bOut.textContent = SAVE_LABEL[way];
        bOut.disabled = false;
        bDl.hidden = way === 'download';
        bOut.onclick = function () {
          save([file], way).catch(function (err) { if (!err || err.name !== 'AbortError') libStatus(String(err.message || err), true); });
        };
      }, function (err) { libStatus('Не прочитать файл: ' + err.message, true); });
      bDl.onclick = function () { if (file) download(file); };
      bDel.onclick = function () {
        if (bDel.dataset.sure !== '1') { bDel.dataset.sure = '1'; bDel.textContent = 'Точно удалить?'; return; }
        store.removePath(path).then(function () {
          S.log('Files: removed ' + path);
          go(lib.dir);
        }, function (err) { libStatus('Не удалось удалить: ' + err.message, true); });
      };
    };
    return li;
  }
  function addFiles() {
    if (lib.busy) return;
    var input = el('input');
    input.type = 'file';
    input.multiple = true;
    input.style.display = 'none';
    input.onchange = function () {
      var files = Array.prototype.slice.call(input.files || []);
      input.remove();
      if (files.length) put(files);
    };
    $('files').appendChild(input);
    input.click();
  }
  function put(files) {
    var dir = rootOf(lib.stack) + lib.dir, total = files.reduce(function (n, f) { return n + f.size; }, 0), done = 0;
    lib.busy = true;
    libStatus('Проверяю место…');
    spaceFor(total).then(function (problem) {
      if (problem) { libStatus(problem, true); return; }
      S.bar($('flbar'), 0);
      var chain = Promise.resolve();
      files.forEach(function (f) {
        chain = chain.then(function () {
          libStatus('Копирую ' + f.name + '…');
          return store.putFile(dir, f, function (n) { S.bar($('flbar'), total ? Math.min(1, (done + n) / total) : 1); });
        }).then(function (name) {
          done += f.size;
          S.log('Files: added ' + dir + name + ' (' + f.size + ' bytes)');
        });
      });
      return chain.then(function () {
        libStatus(files.length > 1 ? 'Добавлено файлов: ' + files.length + '.' : 'Файл добавлен.');
      });
    }).catch(function (e) {
      libStatus('Не удалось добавить: ' + (e && e.message || e), true);
    }).then(function () {
      lib.busy = false;
      $('flbar').hidden = true;
      go(lib.dir);
    });
  }

  // (tests: ?testfiles=NAME on the library: /__test/NAME into Android 10's
  // Download through «Файлы», then read back from the storage)
  function testLibrary(name, opts) {
    lib.stack = 'a10';
    lib.dir = 'Download/';
    library(opts);
    return testFile(name).then(function (f) {
      return store.putFile(rootOf('a10') + 'Download/', f).then(function (got) {
        return store.fileAt(rootOf('a10') + 'Download/' + got, got).then(function (back) {
          return back.arrayBuffer().then(function (buf) {
            S.log('Files: test: ' + got + ' added (' + f.size + ' bytes), read back ' + buf.byteLength + ' bytes, way out: ' +
                  saveWay([back]));
            go(lib.dir);
          });
        });
      });
    }).catch(function (e) { S.log('Files: test failed: ' + e); });
  }

  root.SensesFiles = {app: app, library: library, acceptFor: acceptFor, describe: describe, saveWay: saveWay,
                      testLibrary: testLibrary};
})(self);
