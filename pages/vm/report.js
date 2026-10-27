(function () {
  function report(line) {
    if (window.console && typeof console.info === 'function') console.info(String(line));
  }

  window.arcReport = report;
  window.addEventListener('error', function (event) {
    report('window.onerror: ' + event.message);
  });
  window.addEventListener('unhandledrejection', function (event) {
    report('unhandledrejection: ' + event.reason);
  });
})();
