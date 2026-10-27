(function () {
  function setStatus(text, isError) {
    var status = document.getElementById('storage');
    if (!status) return;
    status.textContent = text;
    if (isError) status.classList.add('bad');
  }

  function waitForController(timeoutMs) {
    if (navigator.serviceWorker.controller) return Promise.resolve(true);
    return new Promise(function (resolve) {
      var finished = false;
      var timer;
      function finish() {
        if (finished) return;
        finished = true;
        clearTimeout(timer);
        navigator.serviceWorker.removeEventListener('controllerchange', onChange);
        resolve(!!navigator.serviceWorker.controller);
      }
      function onChange() { finish(); }
      timer = setTimeout(finish, timeoutMs);
      navigator.serviceWorker.addEventListener('controllerchange', onChange, { once: true });
      if (navigator.serviceWorker.controller) finish();
    });
  }

  function reloadForIsolation() {
    var url = new URL(location.href);
    var retries = Number(url.searchParams.get('__senses_isolation_retry') || 0);
    if (retries >= 1) {
      setStatus('Не удалось включить изоляцию WebAssembly. Обновите страницу или откройте сайт в актуальном Safari, Chrome или Edge.', true);
      return false;
    }
    url.searchParams.set('__senses_isolation_retry', String(retries + 1));
    location.replace(url.href);
    return false;
  }

  window.SENSES_PAGES_READY = (async function () {
    if (!window.isSecureContext || !('serviceWorker' in navigator)) {
      setStatus('Для запуска нужен браузер с поддержкой защищённого соединения и Service Worker.', true);
      return false;
    }

    try {
      setStatus('Подготавливаю безопасный режим WebAssembly…');
      await navigator.serviceWorker.register('./sw.js', { scope: './', updateViaCache: 'none' });
      await navigator.serviceWorker.ready;
      if (!(await waitForController(6000))) {
        setStatus('Не удалось запустить изоляцию WebAssembly. Обновите страницу и попробуйте ещё раз.', true);
        return false;
      }
      if (!window.crossOriginIsolated) return reloadForIsolation();

      var url = new URL(location.href);
      if (url.searchParams.has('__senses_isolation_retry')) {
        url.searchParams.delete('__senses_isolation_retry');
        history.replaceState(null, '', url.href);
      }
      return true;
    } catch (error) {
      console.error('SENSES Web Apps browser setup failed', error);
      setStatus('Не удалось подготовить браузер для WebAssembly. Проверьте настройки приватности и обновите страницу.', true);
      return false;
    }
  })();
})();
