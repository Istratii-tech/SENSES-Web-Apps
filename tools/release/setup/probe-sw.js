// The setup page's probe (tools/release/setup/index.html): a service worker
// that does nothing. Over https Safari registers one only when the phone
// trusts the computer's certificate -- what SENSES Web Apps needs.
self.addEventListener('install', function () { self.skipWaiting(); });
