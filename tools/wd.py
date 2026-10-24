#!/usr/bin/env python3
"""Tiny WebDriver client for Safari on another Mac (safaridriver there,
forwarded here with ssh -L; Safari needs Develop > Allow Remote Automation).
  tools/wd.py start                 open a session (id kept in $WD_STATE)
  tools/wd.py go URL                navigate
  tools/wd.py shot FILE.png         screenshot of the viewport
  tools/wd.py tap X Y               tap at viewport CSS pixels
  tools/wd.py drag X1 Y1 X2 Y2      swipe
  tools/wd.py ptap X Y              a touch sent as PointerEvents to the VM page's
  tools/wd.py pdrag X1 Y1 X2 Y2       canvas (in the page or its iframe; Safari's
                                    WebDriver mouse does not reach the iframe)
  tools/wd.py js 'return ...'       run a script, print the result
  tools/wd.py size                  viewport size
  tools/wd.py end                   close the session (and Safari's window)
WD_URL (default http://localhost:4444)."""
import base64, json, os, sys, urllib.request

WD = os.environ.get('WD_URL', 'http://localhost:4444')
STATE = os.environ.get('WD_STATE', os.path.join(os.environ.get('TMPDIR', '/tmp'), 'wd-session'))


def call(method, path, body=None):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(WD + path, data=data, method=method,
                                 headers={'Content-Type': 'application/json'})
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            return json.load(r).get('value')
    except urllib.error.HTTPError as e:
        sys.exit('webdriver %s %s: %s' % (method, path, e.read().decode()[:400]))


def sid():
    return open(STATE).read().strip()


def pointer(actions):
    call('POST', '/session/%s/actions' % sid(), {'actions': [{
        'type': 'pointer', 'id': 'finger', 'parameters': {'pointerType': 'mouse'},
        'actions': actions}]})


cmd, args = sys.argv[1], sys.argv[2:]
if cmd == 'start':
    v = call('POST', '/session', {'capabilities': {'alwaysMatch': {'browserName': 'safari'}}})
    open(STATE, 'w').write(v['sessionId'])
    print(v['sessionId'])
elif cmd == 'go':
    call('POST', '/session/%s/url' % sid(), {'url': args[0]})
elif cmd == 'shot':
    open(args[0], 'wb').write(base64.b64decode(call('GET', '/session/%s/screenshot' % sid())))
elif cmd == 'tap':
    x, y = int(args[0]), int(args[1])
    pointer([{'type': 'pointerMove', 'x': x, 'y': y, 'origin': 'viewport'},
             {'type': 'pointerDown', 'button': 0}, {'type': 'pause', 'duration': 80},
             {'type': 'pointerUp', 'button': 0}])
elif cmd == 'drag':
    x1, y1, x2, y2 = map(int, args[:4])
    steps = [{'type': 'pointerMove', 'x': x1, 'y': y1, 'origin': 'viewport'},
             {'type': 'pointerDown', 'button': 0}]
    for i in range(1, 9):
        steps.append({'type': 'pointerMove', 'duration': 25, 'origin': 'viewport',
                      'x': x1 + (x2 - x1) * i // 8, 'y': y1 + (y2 - y1) * i // 8})
    pointer(steps + [{'type': 'pointerUp', 'button': 0}])
elif cmd in ('ptap', 'pdrag'):
    pts = list(map(int, args[:4]))
    if cmd == 'ptap':
        pts = pts[:2] * 2
    call('POST', '/session/%s/execute/async' % sid(), {'script': """
const [x1, y1, x2, y2] = arguments[0], done = arguments[1];
let doc = document, off = [0, 0];
const f = document.querySelector('iframe');
if (f && f.contentDocument && f.contentDocument.querySelector('canvas')) {
  const r = f.getBoundingClientRect(); doc = f.contentDocument; off = [r.left, r.top];
}
const c = doc.querySelector('canvas'), V = doc.defaultView;
const ev = (type, x, y, b) => c.dispatchEvent(new V.PointerEvent(type, {pointerId: 9,
    pointerType: 'touch', clientX: x - off[0], clientY: y - off[1], pressure: b, buttons: b, bubbles: true}));
const n = (x1 === x2 && y1 === y2) ? 1 : 10;
ev('pointerdown', x1, y1, 1);
let i = 0;
const step = () => {
  if (++i <= n && n > 1) { ev('pointermove', x1 + (x2 - x1) * i / n, y1 + (y2 - y1) * i / n, 1); setTimeout(step, 30); }
  else setTimeout(() => { ev('pointerup', x2, y2, 0); done(1); }, 80);
};
setTimeout(step, 30);""", 'args': [pts]})
elif cmd == 'js':
    print(json.dumps(call('POST', '/session/%s/execute/sync' % sid(),
                          {'script': args[0], 'args': []}), ensure_ascii=False))
elif cmd == 'size':
    print(json.dumps(call('POST', '/session/%s/execute/sync' % sid(),
                          {'script': 'return [innerWidth, innerHeight, devicePixelRatio]', 'args': []})))
elif cmd == 'end':
    call('DELETE', '/session/%s' % sid())
else:
    sys.exit(__doc__)
