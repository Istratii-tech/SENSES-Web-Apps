// The page side of the browser input devices (shim/input/browser_input.h),
// a --pre-js of the browser builds. On the page's main thread it listens to
// the display canvas (Module.canvas) and hands every DOM pointer and
// keyboard event to the dispatcher in wasm with one call that only queues
// (arcwasm_input_pointer / arcwasm_input_key): the main thread never blocks,
// the dispatcher thread turns the records into Android input events.
//
//   pointerdown/move/up/cancel  -> touchscreen MotionEvents; coordinates
//                                  mapped to the canvas' pixel size (the
//                                  display), pointerId -> Android pointer id,
//                                  coalesced moves kept (they become history)
//   keydown/keyup               -> KeyEvents; KeyboardEvent.code is mapped to
//                                  a Linux scan code and Generic.kl in wasm
//
// The canvas gets keyboard focus (tabindex) when it is touched. Pages can
// call Module.arcwasmInput.attach(canvas) themselves for another element.
//
// Text (topic ios-audio-keyboard, docs/topics/ios-audio-keyboard.md): the
// browser's own keyboard -- an iPhone's on-screen one, or a computer's with
// its layouts and input methods -- types into a hidden <textarea> that takes
// the keyboard focus instead of the canvas. While Android serves an editor
// (the "input_method" service says so: imeChanged), what changed in the
// textarea becomes input method calls on that editor (commit, compose,
// delete, editor action, key); other keys stay KeyEvents. iOS opens its
// keyboard only for an element focused in a touch's handler, and Android
// asks for the keyboard later (showSoftInput, after the app had the touch):
// every touch on the display focuses the textarea with inputmode "none" (no
// keyboard), and the keyboard comes up when Android wants it by switching
// that to the editor's mode. How much of the display the keyboard covers
// (visualViewport) goes back to Android, whose windows then pan or resize
// (adjustPan/adjustResize). "Back" with the keyboard up closes the keyboard.
// Part of the arc-wasm shim layer (topic "input").
if (typeof window !== 'undefined' && typeof document !== 'undefined') (function () {
  var POINTER_DOWN = 1, POINTER_MOVE = 2, POINTER_UP = 3, POINTER_CANCEL = 4;
  var POINTER_TYPES = { mouse: 0, touch: 1, pen: 2 };
  var MOD_SHIFT = 1, MOD_CTRL = 2, MOD_ALT = 4, MOD_META = 8,
      MOD_CAPS_LOCK = 16, MOD_NUM_LOCK = 32, MOD_SCROLL_LOCK = 64;

  var api = Module['arcwasmInput'] = {
    attached: null,
    // Records handed to wasm, and DOM events seen before wasm was ready.
    queued: 0,
    early: 0,
    attach: attach,
    sendFile: sendFile,
    // The page turns the display element when the phone is held sideways for
    // a turned Android display (demo/pwa/index.html): -90 or 90 degrees
    // (CSS rotate) or 0. Pointer coordinates are mapped back through it.
    turn: 0,
    localPoint: localPoint,
  };

  // (clientX, clientY) in |el|'s own pixels (pw x ph), with |el| turned by
  // api.turn about its center (its bounding rectangle is the turned one).
  function localPoint(el, clientX, clientY, pw, ph) {
    var r = el.getBoundingClientRect();
    if (api.turn === -90) {  // its right edge is up
      return [(r.bottom - clientY) * pw / (r.height || 1), (clientX - r.left) * ph / (r.width || 1)];
    }
    if (api.turn === 90) {   // its right edge is down
      return [(clientY - r.top) * pw / (r.height || 1), (r.right - clientX) * ph / (r.width || 1)];
    }
    return [(clientX - r.left) * pw / (r.width || 1), (clientY - r.top) * ph / (r.height || 1)];
  }

  // A file for Android (topic userspace: an APK to install; arcwasm.server.
  // ApkInbox takes it): NAME and its bytes (Uint8Array) are copied into wasm
  // memory and queued. False if the runtime is not up or memory is short.
  function sendFile(name, bytes) {
    var buffer = exported('arcwasm_page_file_buffer');
    var nameBuffer = exported('arcwasm_page_file_name_buffer');
    var ready = exported('arcwasm_page_file_ready');
    if (!buffer || !nameBuffer || !ready) return false;
    var ptr = buffer(bytes.length);
    if (!ptr) return false;
    // A fresh view: malloc may have grown memory (HEAPU8 may be stale).
    var heap = new Uint8Array(wasmMemory.buffer);
    heap.set(bytes, ptr);
    var np = nameBuffer();
    var utf8 = new TextEncoder().encode(String(name)).subarray(0, 255);
    heap.set(utf8, np);
    heap[np + utf8.length] = 0;
    ready(ptr, bytes.length);
    return true;
  }

  // The wasm exports, once the runtime is up (before that they throw).
  function exported(name) {
    if (typeof runtimeInitialized === 'undefined' || !runtimeInitialized) return null;
    var f = Module['_' + name];
    return typeof f === 'function' ? f : null;
  }

  function age(e) {
    var a = performance.now() - e.timeStamp;
    return a > 0 && a < 60000 ? a : 0;
  }

  function modifiers(e) {
    var m = (e.shiftKey ? MOD_SHIFT : 0) | (e.ctrlKey ? MOD_CTRL : 0) |
            (e.altKey ? MOD_ALT : 0) | (e.metaKey ? MOD_META : 0);
    if (e.getModifierState) {
      if (e.getModifierState('CapsLock')) m |= MOD_CAPS_LOCK;
      if (e.getModifierState('NumLock')) m |= MOD_NUM_LOCK;
      if (e.getModifierState('ScrollLock')) m |= MOD_SCROLL_LOCK;
    }
    return m;
  }

  function attach(canvas) {
    if (!canvas || api.attached === canvas) return;
    api.attached = canvas;
    var active = {};  // pointerIds that are down
    var sizeSent = false;
    canvas.style.touchAction = 'none';
    if (canvas.tabIndex < 0) canvas.tabIndex = 0;

    function sendSize(setSize) {
      if (sizeSent) return;
      setSize(canvas.width, canvas.height);
      sizeSent = true;
    }

    function pointer(type, e) {
      var push = exported('arcwasm_input_pointer');
      if (!push) { api.early++; return; }
      var setSize = exported('arcwasm_input_display_size');
      if (setSize) sendSize(setSize);
      var kind = POINTER_TYPES[e.pointerType];
      if (kind === undefined) kind = 1;
      var samples = [e];
      if (type === POINTER_MOVE && e.getCoalescedEvents) {
        var coalesced = e.getCoalescedEvents();
        if (coalesced && coalesced.length) samples = coalesced;
      }
      for (var i = 0; i < samples.length; i++) {
        var s = samples[i];
        var pressure = s.pressure || (type === POINTER_UP ? 0 : 1);
        var at = localPoint(canvas, s.clientX, s.clientY, canvas.width, canvas.height);
        push(type, e.pointerId, kind, at[0], at[1], pressure, s.buttons | 0, age(s));
        api.queued++;
      }
    }

    canvas.addEventListener('pointerdown', function (e) {
      active[e.pointerId] = true;
      try { canvas.setPointerCapture(e.pointerId); } catch (err) {}
      // Keyboard focus, so typing reaches the app (a finger: on its
      // touchend, where iOS lets the focus open its keyboard).
      if (e.pointerType !== 'touch') ime.focus(false);
      pointer(POINTER_DOWN, e);
      e.preventDefault();
    });
    canvas.addEventListener('pointermove', function (e) {
      if (!active[e.pointerId]) return;  // hover: not a touch
      pointer(POINTER_MOVE, e);
      e.preventDefault();
    });
    function end(type) {
      return function (e) {
        if (!active[e.pointerId]) return;
        delete active[e.pointerId];
        pointer(type, e);
        e.preventDefault();
      };
    }
    canvas.addEventListener('pointerup', end(POINTER_UP));
    canvas.addEventListener('pointercancel', end(POINTER_CANCEL));
    // (touchend: a user gesture for iOS's keyboard; pointerup is not)
    canvas.addEventListener('touchend', function () { ime.focus(true); });
    // The focus stays with the textarea (a mousedown would move it here).
    canvas.addEventListener('mousedown', function (e) { e.preventDefault(); });

    canvas.addEventListener('keydown', function (e) {
      if (ime.canvasKey(e)) return;
      sendKey(e, true);
    });
    canvas.addEventListener('keyup', function (e) {
      if (ime.canvasKey(e)) return;
      sendKey(e, false);
    });
    ime.create();
  }

  function log(line) {
    (Module['printErr'] || console.log)('ime: ' + line);
  }

  // A DOM key event as an Android KeyEvent (the code through Generic.kl).
  function sendKey(e, down) {
    var push = exported('arcwasm_input_key');
    var buffer = exported('arcwasm_input_code_buffer');
    if (!push || !buffer) { api.early++; return; }
    var code = String(e.code || '');
    var ptr = buffer() >>> 0;  // unsigned: wasm memory may pass 2 GB
    var n = Math.min(code.length, 23);
    for (var i = 0; i < n; i++) HEAPU8[ptr + i] = code.charCodeAt(i) & 0x7f;
    HEAPU8[ptr + n] = 0;
    push(down ? 1 : 0, modifiers(e), e.repeat ? 1 : 0, age(e));
    api.queued++;
    // Keep keys for the app, except the browser's own shortcuts (Cmd+...).
    if (!e.metaKey) e.preventDefault();
  }

  // --- the browser's keyboard as Android's input method ------------------------------
  // (shim/input/browser_input.h: ImeOp, ImeState; arcwasm.input.BrowserIme)
  var IME_COMMIT = 99, IME_COMPOSE = 112, IME_FINISH = 102, IME_DELETE = 100, IME_KEY = 107,
      IME_ACTION = 97, IME_BATCH = 98, IME_KEYBOARD = 104, IME_RESEED = 114;
  var KEYCODE_DEL = 67, KEYCODE_ENTER = 66;
  // EditorInfo / InputType bits.
  var CLASS_MASK = 0xf, CLASS_TEXT = 1, CLASS_NUMBER = 2, CLASS_PHONE = 3, VARIATION_MASK = 0xff0,
      FLAG_CAP_CHARACTERS = 0x1000, FLAG_CAP_WORDS = 0x2000, FLAG_CAP_SENTENCES = 0x4000,
      FLAG_AUTO_COMPLETE = 0x10000, FLAG_MULTI_LINE = 0x20000, FLAG_NO_SUGGESTIONS = 0x80000,
      NUMBER_FLAG_DECIMAL = 0x2000;
  var V_URI = 0x10, V_EMAIL = 0x20, V_PASSWORD = 0x80, V_VISIBLE_PASSWORD = 0x90,
      V_FILTER = 0xb0, V_WEB_EMAIL = 0xd0, V_WEB_PASSWORD = 0xe0;
  var ACTION_MASK = 0xff, ACTION_NONE = 1, FLAG_NO_ENTER_ACTION = 0x40000000;
  var ENTER_HINTS = { 2: 'go', 3: 'search', 4: 'send', 5: 'next', 6: 'done', 7: 'previous' };
  var SHADOW_MAX = 400;  // the textarea keeps this much of the editor's text

  var ime = api.ime = {
    el: null,
    // The served editor (none: editor false), whether Android wants the keyboard.
    editor: false, show: false, inputType: 0, imeOptions: 0, actionId: 0, serial: 0,
    // What the textarea held when Android was last told (the editor's text
    // before its cursor, as far as known; the textarea's caret stays at its end).
    sent: '',
    dirty: false,      // typed into since the last reset (a seed would be stale)
    composing: false, compStart: 0, composed: '',
    forwarded: {},     // KeyboardEvent.code -> true: its keydown went as a KeyEvent
    shown: false,      // the on-screen keyboard is up (visualViewport)
    covered: 0,        // display pixels it covers, as last told
    swallow: null,     // the code whose keyup is ours (BrowserBack closing the keyboard)
    refocusing: false,
    reseedTimer: 0,
  };

  ime.create = function () {
    if (ime.el || !document.body) return;
    var el = ime.el = document.createElement('textarea');
    el.setAttribute('aria-hidden', 'true');
    el.setAttribute('autocomplete', 'off');
    el.setAttribute('inputmode', 'none');
    el.spellcheck = false;
    el.tabIndex = -1;
    // Invisible but focusable, at the top (where iOS has no reason to
    // scroll the page for it); 16 px so that iOS does not zoom in.
    el.style.cssText = 'position:fixed;left:0;top:0;width:1px;height:1px;margin:0;padding:0;border:0;' +
        'opacity:0;color:transparent;background:transparent;caret-color:transparent;resize:none;' +
        'overflow:hidden;white-space:pre;font-size:16px;pointer-events:none;z-index:0';
    document.body.appendChild(el);
    el.addEventListener('keydown', onKeyDown);
    el.addEventListener('keyup', onKeyUp);
    el.addEventListener('beforeinput', onBeforeInput);
    el.addEventListener('input', onInput);
    el.addEventListener('compositionstart', onCompositionStart);
    el.addEventListener('compositionend', onCompositionEnd);
    el.addEventListener('blur', onBlur);
    el.addEventListener('focus', measure);
    if (window.visualViewport) {
      visualViewport.addEventListener('resize', measure);
      visualViewport.addEventListener('scroll', measure);
    }
    window.addEventListener('resize', measure);
  };

  // To Android: one record (texts longer than the buffer in pieces).
  function send(op, a, b, text) {
    var push = exported('arcwasm_ime_op'), buffer = exported('arcwasm_ime_text_buffer');
    if (!push || !buffer) { api.early++; return; }
    text = text || '';
    var i = 0;
    do {
      var end = Math.min(text.length, i + 1000);
      var c = text.charCodeAt(end - 1);
      if (end < text.length && c >= 0xd800 && c < 0xdc00) end--;  // not inside a pair
      var ptr = buffer() >>> 0;
      var units = new Uint16Array(wasmMemory.buffer, ptr, end - i);
      for (var j = i; j < end; j++) units[j - i] = text.charCodeAt(j);
      push(op, a | 0, b | 0, end - i);
      i = end;
    } while (i < text.length);
    api.queued++;
  }

  // The keyboard's mode for the editor (inputmode), "none" for no keyboard.
  function mode() {
    if (!ime.editor || !ime.show) return 'none';
    var t = ime.inputType, klass = t & CLASS_MASK, v = t & VARIATION_MASK;
    if (klass === CLASS_NUMBER) return t & NUMBER_FLAG_DECIMAL ? 'decimal' : 'numeric';
    if (klass === CLASS_PHONE) return 'tel';
    if (klass === CLASS_TEXT && (v === V_EMAIL || v === V_WEB_EMAIL)) return 'email';
    if (klass === CLASS_TEXT && v === V_URI) return 'url';
    return 'text';
  }

  function setMode(m) {
    var el = ime.el;
    if (el && el.getAttribute('inputmode') !== m) el.setAttribute('inputmode', m);
  }

  // The editor's wishes as the textarea's attributes: capitals, corrections,
  // the return key's label.
  function applyEditor() {
    var el = ime.el, t = ime.inputType, klass = t & CLASS_MASK, v = t & VARIATION_MASK;
    var secret = (klass === CLASS_TEXT && (v === V_PASSWORD || v === V_VISIBLE_PASSWORD ||
                                           v === V_WEB_PASSWORD)) ||
                 (klass === CLASS_NUMBER && v === 0x10);
    var plain = klass === CLASS_TEXT && !secret && v !== V_EMAIL && v !== V_WEB_EMAIL &&
                v !== V_URI && v !== V_FILTER && !(t & (FLAG_NO_SUGGESTIONS | FLAG_AUTO_COMPLETE));
    el.setAttribute('autocapitalize', klass !== CLASS_TEXT || secret ? 'none' :
                    t & FLAG_CAP_CHARACTERS ? 'characters' : t & FLAG_CAP_WORDS ? 'words' :
                    t & FLAG_CAP_SENTENCES ? 'sentences' : 'none');
    el.setAttribute('autocorrect', plain ? 'on' : 'off');
    el.spellcheck = false;
    var o = ime.imeOptions, hint = '';
    if (t & FLAG_MULTI_LINE || o & FLAG_NO_ENTER_ACTION) hint = 'enter';
    else hint = ENTER_HINTS[o & ACTION_MASK] || '';
    if (hint) el.setAttribute('enterkeyhint', hint); else el.removeAttribute('enterkeyhint');
  }

  // Forget what the textarea knew (the editor's cursor may have moved);
  // Android sends the text before its cursor again soon (a seed).
  function reset(reseed) {
    var el = ime.el;
    if (el) el.value = '';
    ime.sent = '';
    ime.dirty = false;
    ime.composing = false;
    clearTimeout(ime.reseedTimer);
    if (reseed && ime.editor) {
      // After the app had the touch or key that moved its cursor.
      ime.reseedTimer = setTimeout(function () { if (!ime.dirty) send(IME_RESEED); }, 250);
    }
  }

  // Android's editor state changed (arcwasm::SetImeState).
  api.imeChanged = function () {
    var get = exported('arcwasm_ime_state');
    if (!get || !ime.el) return;
    var ptr = get() >>> 0, w = new Int32Array(wasmMemory.buffer, ptr, 7);
    var serial = w[0], editor = !!w[1], show = !!w[2], t = w[3], o = w[4], action = w[5], n = w[6];
    var seed = n > 0 ? String.fromCharCode.apply(null, new Uint16Array(wasmMemory.buffer, ptr + 28, n)) : '';
    if (serial === ime.serial) return;
    ime.serial = serial;
    var changed = editor !== ime.editor || t !== ime.inputType || o !== ime.imeOptions;
    if (changed || show !== ime.show) {
      log(editor ? 'editor type 0x' + t.toString(16) + ' options 0x' + o.toString(16) +
                   (show ? ', keyboard wanted' : '') : 'no editor');
    }
    ime.editor = editor; ime.show = show; ime.inputType = t; ime.imeOptions = o; ime.actionId = action;
    if (changed) reset(false);
    if (editor) {
      applyEditor();
      if (seed && !ime.dirty && !ime.composing && ime.el.value !== seed) {
        ime.el.value = seed.slice(-SHADOW_MAX);
        ime.sent = ime.el.value;
        caretToEnd();
      }
    }
    setMode(mode());
    // A computer's keyboard: the textarea takes it at once (iOS opens its
    // keyboard only from a touch: there the next one does it).
    if (editor && document.activeElement !== ime.el && document.activeElement !== document.body &&
        document.activeElement !== api.attached) {
      return;  // the page's own field has the focus
    }
    if (editor && document.activeElement !== ime.el) focusTextarea();
    if (editor && show && navigator.maxTouchPoints > 0) {
      setTimeout(function () {
        if (ime.editor && ime.show && !ime.shown) log('keyboard wanted, not up (the next touch opens it)');
      }, 800);
    }
    if (!(editor && show) && ime.shown) {
      // Put away (inputmode "none" should do; else the focus goes).
      setTimeout(function () {
        if (ime.shown && mode() === 'none') {
          ime.refocusing = true;
          ime.el.blur();
          ime.refocusing = false;
        }
      }, 300);
    }
    measure();
  };

  function focusTextarea() {
    setMode(mode());
    if (document.activeElement !== ime.el) ime.el.focus({ preventScroll: true });
  }

  // Focus for typing (touch: in a touchend handler, so that iOS may open
  // its keyboard for it).
  ime.focus = function (touch) {
    if (!ime.el) ime.create();
    var el = ime.el;
    if (!el) { if (api.attached) api.attached.focus({ preventScroll: true }); return; }
    var m = mode();
    setMode(m);
    if (document.activeElement !== el) {
      el.focus({ preventScroll: true });
    } else if (touch && m !== 'none' && !ime.shown) {
      // Focused, the keyboard wanted but not up (asked for outside a
      // touch): a focus inside this touch opens it.
      ime.refocusing = true;
      el.blur();
      el.focus({ preventScroll: true });
      ime.refocusing = false;
    }
    if (ime.editor && !ime.composing) reset(true);
  };

  function caretToEnd() {
    var el = ime.el, n = el.value.length;
    if (el.selectionStart !== n || el.selectionEnd !== n) el.setSelectionRange(n, n);
  }

  // What changed in the textarea since Android was told, as an input
  // method's edit at the editor's cursor: delete back to the first change,
  // commit the rest.
  function sync() {
    var el = ime.el, now = el.value, was = ime.sent;
    var i = 0, n = Math.min(now.length, was.length);
    while (i < n && now.charCodeAt(i) === was.charCodeAt(i)) i++;
    if (i > 0 && i < now.length) {
      var c = now.charCodeAt(i - 1);
      if (c >= 0xd800 && c < 0xdc00) i--;  // a pair changed half: the whole pair
    }
    var del = was.length - i, text = now.slice(i);
    if ((ime.inputType & CLASS_MASK) === 0) {
      // A view without text (TYPE_NULL, a terminal): keys, as LatinIME
      // sends there; the commit arrives as key events.
      for (var d = 0; d < del; d++) send(IME_KEY, KEYCODE_DEL, 0);
      if (text) send(IME_COMMIT, 0, 0, text);
      el.value = '';
      ime.sent = '';
      return;
    }
    if (del && text) send(IME_BATCH, 1);
    if (del) send(IME_DELETE, del, 0);
    if (text) send(IME_COMMIT, 0, 0, text);
    if (del && text) send(IME_BATCH, 0);
    if (now.length > SHADOW_MAX * 2) {
      el.value = now.slice(-SHADOW_MAX);
    }
    caretToEnd();
    ime.sent = el.value;
    if (del || text) ime.dirty = true;
  }

  // The return key: as Android's keyboard does (LatinIME): the editor's
  // action, or a new line where there is none.
  function enter() {
    var o = ime.imeOptions, action = o & FLAG_NO_ENTER_ACTION ? ACTION_NONE : o & ACTION_MASK;
    if ((ime.inputType & CLASS_MASK) === 0) send(IME_KEY, KEYCODE_ENTER, 0);
    else if (ime.actionId) send(IME_ACTION, ime.actionId);
    else if (action !== ACTION_NONE) send(IME_ACTION, action);
    else send(IME_COMMIT, 0, 0, '\n');
    reset(true);
  }

  function isText(e) {
    var k = e.key;
    if (e.ctrlKey || e.metaKey) return false;
    return k === 'Dead' || k === 'Unidentified' || k === 'Process' ||
           (typeof k === 'string' && k.length > 0 && (k.length === 1 || k.codePointAt(0) > 0xffff &&
                                                     k.length === 2));
  }

  function onKeyDown(e) {
    if (e.isComposing || e.keyCode === 229) return;  // the input method's
    if (!ime.editor) {  // games: every key a KeyEvent, as on the canvas
      if (closeKeyboard(e, true)) return;
      ime.forwarded[e.code] = true;
      sendKey(e, true);
      return;
    }
    var el = ime.el, k = e.key;
    if (e.metaKey) {
      // The browser's own (paste...), not those that would move the
      // textarea away from the editor: select all, undo.
      if (/^[aAzZyY]$/.test(k)) e.preventDefault();
      return;
    }
    if (k === 'Enter') { e.preventDefault(); enter(); return; }
    if (k === 'Backspace' && el.selectionStart === 0 && el.selectionEnd === 0) {
      // Nothing known before the cursor: the key itself (as LatinIME).
      e.preventDefault();
      send(IME_KEY, KEYCODE_DEL, 0);
      return;
    }
    if (k === 'Backspace' || isText(e)) return;  // the textarea's (then sync)
    if (closeKeyboard(e, true)) return;
    ime.forwarded[e.code] = true;
    sendKey(e, true);
    if (!/^(Shift|Control|Alt|Meta|CapsLock)/.test(e.code)) reset(true);
  }

  function onKeyUp(e) {
    if (ime.swallow === e.code) { ime.swallow = null; e.preventDefault(); return; }
    if (!ime.forwarded[e.code]) return;
    delete ime.forwarded[e.code];
    sendKey(e, false);
  }

  // Keys sent to the canvas itself (the page's buttons; the canvas still
  // focused): "back" closes the keyboard; text for a served editor is
  // committed (the textarea takes the focus for what follows).
  ime.canvasKey = function (e) {
    var down = e.type === 'keydown';
    if (!down && ime.swallow === e.code) { ime.swallow = null; return true; }
    if (down && closeKeyboard(e, false)) return true;
    if (!ime.editor || !e.isTrusted) return false;
    if (down && isText(e) && e.key.length <= 2 && e.key !== 'Dead') {
      e.preventDefault();
      send(IME_COMMIT, 0, 0, e.key);
      ime.focus(false);
      return true;
    }
    return false;
  };

  // "Back" while the on-screen keyboard is up: the keyboard goes, as an
  // input method takes the back key first.
  function closeKeyboard(e, viaTextarea) {
    if (e.code !== 'BrowserBack' && e.code !== 'GoBack' || !ime.shown) return false;
    ime.swallow = e.code;
    e.preventDefault();
    ime.show = false;
    setMode('none');
    send(IME_KEYBOARD, 0, 2);
    ime.covered = 0;
    return true;
  }

  function onBeforeInput(e) {
    var t = e.inputType;
    if (t === 'insertLineBreak' || t === 'insertParagraph') {
      e.preventDefault();
      if (ime.editor) enter();
    } else if (t === 'historyUndo' || t === 'historyRedo') {
      e.preventDefault();
    }
  }

  function onInput() {
    var el = ime.el;
    if (!ime.editor) { el.value = ''; ime.sent = ''; return; }
    if (ime.composing) {
      var text = el.value.slice(ime.compStart);
      if (text !== ime.composed) { ime.composed = text; send(IME_COMPOSE, 0, 0, text); }
      ime.dirty = true;
      return;
    }
    sync();
  }

  function onCompositionStart() {
    if (!ime.editor) return;
    // Anything typed before it first (the textarea's selection is what the
    // composition replaces: collapsed at the end).
    if (ime.el.value !== ime.sent) sync();
    ime.composing = true;
    ime.compStart = ime.el.selectionStart;
    ime.composed = '';
  }

  function onCompositionEnd() {
    if (!ime.composing) return;
    ime.composing = false;
    var text = ime.el.value.slice(ime.compStart);
    if (text !== ime.composed) send(IME_COMPOSE, 0, 0, text);
    send(IME_FINISH);
    ime.composed = '';
    ime.sent = ime.el.value;
    ime.dirty = true;
    caretToEnd();
  }

  function onBlur() {
    if (ime.refocusing) return;
    if (ime.composing) onCompositionEnd();
    setTimeout(function () {
      // The user put the keyboard away (iOS "Done"; the focus did not come
      // back): Android forgets its wish.
      if (document.activeElement !== ime.el && ime.editor && ime.show) {
        ime.show = false;
        setMode('none');
        send(IME_KEYBOARD, 0, 2);
        ime.covered = 0;
      }
      measure();
    }, 100);
  }

  // How much of the display the on-screen keyboard covers, for Android.
  function measure() {
    var el = ime.el, vv = window.visualViewport, canvas = api.attached;
    if (!el || !canvas) return;
    var px = 0, shown = false;
    if (vv && document.activeElement === el && el.getAttribute('inputmode') !== 'none' &&
        vv.scale < 1.05 && innerHeight - vv.height > 100) {
      shown = true;
      var r = canvas.getBoundingClientRect();
      var cover = r.bottom - (vv.offsetTop + vv.height);
      if (cover > 0 && api.turn === 0 && r.height > 0) {
        px = Math.min(canvas.height, Math.round(cover * canvas.height / r.height));
      }
    }
    if (shown !== ime.shown) log(shown ? 'keyboard up, ' + px + ' px of the display under it' : 'keyboard down');
    ime.shown = shown;
    if (px !== ime.covered) {
      ime.covered = px;
      send(IME_KEYBOARD, px, shown ? 1 : 0);
    }
  }

  function attachDefault() {
    if (Module['canvas']) attach(Module['canvas']);
  }
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', attachDefault);
  } else {
    attachDefault();
  }
})();
