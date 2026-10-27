// SENSES Web Apps: «Сервисы Google» (topic gms, docs/topics/gms.md). SENSES
// ships nothing of Google's: by default the systems are phones without Google
// Play. Here the user adds, per system (8.1 and 10), what they obtained
// themselves from «Файлы»: microG (GmsCore com.google.android.gms, its
// companion com.android.vending) or Google's own Google Play services, Play
// Store and Services Framework (.apk, or .apks/.xapk/.apkm of parts). They go
// to that system's /data as senses/data/STACK/google/PACKAGE/ (store-worker.js),
// which the VM installs as privileged system apps; microG's signature
// spoofing is honoured for them only. Replace: add the file again; remove:
// the row's button (the package's data in that system goes with it).
//
//   SensesGoogle.open({locales: () => [...], dpi})   the sheet (from the menu)
//   SensesGoogle.addFiles(stack, [File])           (tests) as if picked there
(function (root) {
  'use strict';
  var S = root.Senses, $ = S.$, store = root.SensesStore;
  var STACKS = ['a81', 'a10'];
  var NAMES = {
    'com.google.android.gms': 'Сервисы Google Play',
    'com.android.vending': 'Google Play Маркет',
    'com.google.android.gsf': 'Google Services Framework',
  };
  var opts = {}, busy = false, target = null;

  function el(tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  }

  // The sheet, made once.
  function sheet() {
    if ($('gms')) return $('gms');
    var s = el('div', 'sheet');
    s.id = 'gms';
    s.hidden = true;
    var box = el('div', 'box');
    box.appendChild(el('h2', null, 'Сервисы Google'));
    box.appendChild(el('p', 'gms-note',
        'В SENSES нет сервисов Google: приложения видят телефон без Google Play. Если приложению они ' +
        'нужны, добавьте в систему файлы, которые вы скачали сами: microG (Services и Companion) или ' +
        'оригинальные «Сервисы Google Play» и «Google Play Маркет» (.apk или .apks). Подмена подписи ' +
        'включается только для microG. Вход в аккаунт Google не поддерживается.'));
    STACKS.forEach(function (stack) {
      var sec = el('div', 'gms-stack');
      sec.appendChild(el('h3', null, S.STACKS[stack].name));
      var ul = el('ul', 'gms-list');
      ul.id = 'gms-' + stack;
      sec.appendChild(ul);
      var add = el('button', null, 'Добавить файлы…');
      add.onclick = function () { pick(stack); };
      sec.appendChild(add);
      box.appendChild(sec);
    });
    var status = el('div', 'gms-status');
    status.id = 'gmsstatus';
    box.appendChild(status);
    var bar = el('div', 'bar');
    bar.id = 'gmsbar';
    bar.hidden = true;
    bar.appendChild(el('div'));
    box.appendChild(bar);
    var close = el('button', 'close', 'Закрыть');
    close.setAttribute('data-close', '');
    box.appendChild(close);
    s.appendChild(box);
    // No accept= (iOS greys .apk out); several files at once (Services and Companion).
    var input = el('input');
    input.type = 'file';
    input.multiple = true;
    input.hidden = true;
    input.id = 'gmsfile';
    input.onchange = function () {
      var files = Array.prototype.slice.call(this.files || []);
      if (files.length && target) addFiles(target, files);
    };
    s.appendChild(input);
    document.body.appendChild(s);
    return s;
  }

  function status(text, fraction, bad) {
    $('gmsstatus').textContent = text || '';
    $('gmsstatus').classList.toggle('error', !!bad);
    if (fraction === undefined) $('gmsbar').hidden = true;
    else S.bar($('gmsbar'), fraction);
  }

  function kindOf(meta) {
    return /microg/i.test((meta.label || '') + ' ' + (meta.versionName || '')) ? 'microG' : 'Google';
  }

  function render(list) {
    STACKS.forEach(function (stack) {
      var ul = $('gms-' + stack);
      ul.textContent = '';
      var mine = list.filter(function (x) { return x.stack === stack; });
      if (!mine.length) ul.appendChild(el('li', 'gms-none', 'Не установлены — приложения видят телефон без Google Play'));
      mine.forEach(function (x) {
        var li = el('li');
        var name = el('div', 'name');
        name.appendChild(el('div', null, (NAMES[x.meta.package] || x.meta.package) + ' · ' + kindOf(x.meta)));
        name.appendChild(el('div', 'pkg', x.meta.package + ' · ' + (x.meta.versionName || x.meta.versionCode) +
                            ' · ' + S.size(x.meta.size || 0)));
        li.appendChild(name);
        var rm = el('button', 'danger', 'Удалить');
        rm.onclick = function () { remove(stack, x.meta); };
        li.appendChild(rm);
        ul.appendChild(li);
      });
    });
  }

  function refresh() {
    return store.googleList().then(function (list) {
      render(list);
      return list;
    }, function (e) {
      status('Хранилище недоступно: ' + e.message, undefined, true);
      return [];
    });
  }

  function pick(stack) {
    if (busy) return;
    target = stack;
    $('gmsfile').value = '';
    $('gmsfile').click();
  }

  function addFiles(stack, files) {
    busy = true;
    var installed = [];
    var chain = refresh().then(function (list) {
      return files.reduce(function (p, file) {
        return p.then(function () { return addOne(stack, file, list); }).then(function (pkg) {
          if (pkg) installed.push(pkg);
        });
      }, Promise.resolve());
    });
    return chain.then(function () {
      busy = false;
      status(installed.length ? 'Готово. Изменения вступят в силу при следующем запуске приложения на ' +
             S.STACKS[stack].name + '.' : '', undefined);
      return refresh();
    }, function (e) {
      busy = false;
      S.log('Google: failed: ' + (e && e.message || e));
      status(e && e.name === 'QuotaExceededError' ? 'Не хватает места.' : 'Не удалось: ' + (e && e.message || e),
             undefined, true);
      return refresh();
    });
  }

  // One file: read it, accept Google's package names only, put it in the stack.
  function addOne(stack, file, list) {
    var rootDir = store.googleRoot(stack);
    var scoped = Object.create(store);
    scoped.install = function (args, onProgress) { return store.install(Object.assign({root: rootDir}, args), onProgress); };
    scoped.commitBundle = function (args) { return store.commitBundle(Object.assign({root: rootDir}, args)); };
    status('Читаю ' + file.name + '…', null);
    S.log('Google: ' + file.name + ' ' + file.size + ' bytes for ' + stack);
    var prep;
    return SensesInstall.prepare(file, {
      store: scoped, apps: [], locales: opts.locales ? opts.locales() : [], dpi: opts.dpi || 160, log: S.log,
      status: function (t, f) { status(t, f); },
    }).then(function (p) {
      prep = p;
      var info = p.info;
      if (!NAMES[info.package]) {
        prep.cancel();
        throw new Error(file.name + ' — это ' + info.package + ', а не сервисы Google');
      }
      var old = list.filter(function (x) { return x.stack === stack && x.meta.package === info.package; })[0];
      var meta = Object.assign({
        package: info.package, label: info.label, versionName: info.versionName,
        versionCode: info.versionCode, minSdk: info.minSdk, abis: info.abis, stack: stack,
        installedAt: Date.now(), size: file.size,
      }, p.meta);
      meta.kind = kindOf(meta);
      if (meta.minSdk > S.STACKS[stack].api) {
        prep.cancel();
        throw new Error(info.label + ' требует Android API ' + meta.minSdk + ', а ' + S.STACKS[stack].name +
                        ' — ' + S.STACKS[stack].api);
      }
      status('Копирую ' + info.label + '…', meta.parts ? null : 0);
      return prep.commit(meta, null, old, function (done, total) {
        status('Копирую ' + info.label + '…', done / total);
      }).then(function () {
        S.log('Google: installed ' + meta.package + ' ' + meta.versionName + ' (' + meta.kind + ') on ' + stack);
        return meta.package;
      });
    }, function (e) {
      if (prep) prep.cancel();
      throw e;
    });
  }

  function remove(stack, meta) {
    if (busy || !confirm('Удалить ' + (NAMES[meta.package] || meta.package) + ' из ' + S.STACKS[stack].name +
                         ' вместе с его данными?')) return;
    busy = true;
    store.googleRemove(stack, meta.package).then(function () {
      busy = false;
      S.log('Google: removed ' + meta.package + ' from ' + stack);
      status('Удалено.', undefined);
      return refresh();
    }, function (e) {
      busy = false;
      status('Не удалось удалить: ' + e.message, undefined, true);
    });
  }

  root.SensesGoogle = {
    open: function (o) {
      opts = o || {};
      sheet();
      status('', undefined);
      S.open('gms');
      return refresh();
    },
    list: function () { return store.googleList(); },
    kind: function (meta) { return meta.kind || kindOf(meta); },  // 'microG' or 'Google'
    // For tests (browsers that cannot pick a file): files into a system.
    addFiles: function (stack, files) { sheet(); return addFiles(stack, files); },
  };
})(self);
