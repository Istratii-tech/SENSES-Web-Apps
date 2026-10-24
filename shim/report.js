// Sends page output to tools/serve.py (POST /__log), batched, so results
// from browsers that cannot be automated (Safari) reach the console.
var arcReport = (function () {
  var queue = [], timer = 0;
  function flush() {
    timer = 0;
    if (!queue.length) return;
    var body = queue.join('\n');
    queue = [];
    fetch('/__log', { method: 'POST', body: body, keepalive: true }).catch(function () {});
  }
  window.addEventListener('error', function (e) { arcReport('window.onerror: ' + e.message); });
  window.addEventListener('unhandledrejection', function (e) { arcReport('unhandledrejection: ' + e.reason); });
  return function (line) {
    queue.push(String(line));
    if (!timer) timer = setTimeout(flush, 300);
  };
})();
