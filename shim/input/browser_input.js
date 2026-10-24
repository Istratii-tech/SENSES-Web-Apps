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
      // Keyboard focus, so typing reaches the app.
      if (document.activeElement !== canvas) canvas.focus({ preventScroll: true });
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

    function key(down) {
      return function (e) {
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
      };
    }
    canvas.addEventListener('keydown', key(true));
    canvas.addEventListener('keyup', key(false));
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
