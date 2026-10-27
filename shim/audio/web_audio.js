// The page side of arc-wasm's sound output (shim/audio/audio_flinger.cc), a
// --pre-js of the browser builds. The mixer thread asks the main thread
// (MAIN_THREAD_EM_ASM) to open the output: an AudioContext at the mixer's
// rate with an AudioWorklet that plays a ring of 16-bit stereo frames in
// wasm memory (a SharedArrayBuffer), read and written with Atomics:
//
//   ctl[0] frames written (mixer)   ctl[1] frames played (worklet)
//   ctl[2] 1 while the context runs ctl[3] the context's sample rate
//   ctl[4] worklet underruns        ctl[5] peak of the last quantum
//   (an underrun: the ring ran dry in the middle of a quantum)
//
// Autoplay: browsers start an AudioContext only from a user gesture. The
// context is made and started inside the page's first gesture (touchend,
// click, keydown: iOS takes no pointerdown), or, when an app plays before
// any, made suspended and started by the next one; every later gesture
// resumes it if it stopped (iOS "interrupted": a call, Siri, another app).
// Until it runs ctl[2] is 0 and the mixer lets a clock consume the output.
// Without a sound output open the started context is suspended again (no
// audio thread for apps that play nothing); a started context resumes later
// without a gesture.
//
// iPhones: Web Audio is "ambient" sound there, silenced by the ring/silent
// switch. Once an app plays, the page asks for playback sound, as Android
// media does (topic ios-audio-keyboard): navigator.audioSession.type =
// "playback" where it exists (iOS 16.4+), else a looping silent <audio>
// element started in a gesture (what makes WebKit use the playback session).
// It pauses while the page is hidden.
//
// The worklet plays the ring at the context's rate; where the browser gave
// another rate than the mixer's (no sampleRate option), it resamples
// (linear).
//
// Module.arcwasmAudioResume() resumes from a page's own handler (a "play"
// button); Module.arcwasmAudioStats() reports the context state, frames
// played, underruns, and the level and dominant frequency the page actually
// outputs (an AnalyserNode after the worklet) -- how tests and demos check
// that sound is produced without listening to it.
// Part of the arc-wasm shim layer (topic gles-audio).

if (typeof window != 'undefined' && typeof document != 'undefined') (function () {
  var audio = null;  // {ctx, node, analyser, ctl}: the app's output
  var ctx = null;    // the page's AudioContext (made in a gesture if it can)
  var RATE = 44100;  // the mixer's (shim/audio/audio_flinger.h kOutputRate)
  var gestures = 0;
  var started = false;  // the context ran once (resume needs no gesture since)
  var media = null;  // the silent <audio> keeping iOS's playback session

  var PROCESSOR = [
    "class ArcWasmAudio extends AudioWorkletProcessor {",
    "  constructor(options) {",
    "    super();",
    "    var o = options.processorOptions;",
    "    this.ctl = new Int32Array(o.buffer, o.ctl, 8);",
    "    this.ring = new Int16Array(o.buffer, o.ring, o.frames * 2);",
    "    this.mask = o.frames - 1;",
    "    this.step = o.step || 1;  // ring frames per output frame",
    "    this.frac = 0;",
    "  }",
    "  process(inputs, outputs) {",
    "    var out = outputs[0], left = out[0], right = out[1] || out[0];",
    "    var ctl = this.ctl, ring = this.ring, mask = this.mask, step = this.step;",
    "    var r = Atomics.load(ctl, 1), w = Atomics.load(ctl, 0);",
    "    var peak = 0, n = left.length, had = false, dry = false;",
    "    for (var i = 0; i < n; i++) {",
    "      if (step !== 1 && ((w - r) | 0) > 1) {",
    "        // between frames r and r + 1",
    "        var j0 = (r & mask) * 2, j1 = ((r + 1) & mask) * 2, f = this.frac;",
    "        var a = ring[j0] + (ring[j1] - ring[j0]) * f, b = ring[j0 + 1] + (ring[j1 + 1] - ring[j0 + 1]) * f;",
    "        left[i] = a / 32768; right[i] = b / 32768;",
    "        var m = a < 0 ? -a : a; if (m > peak) peak = m;",
    "        m = b < 0 ? -b : b; if (m > peak) peak = m;",
    "        f += step; var whole = Math.floor(f); this.frac = f - whole;",
    "        r = (r + whole) | 0; had = true;",
    "      } else if (step === 1 && ((w - r) | 0) > 0) {",
    "        var j = (r & mask) * 2, a = ring[j], b = ring[j + 1];",
    "        left[i] = a / 32768; right[i] = b / 32768;",
    "        var m = a < 0 ? -a : a; if (m > peak) peak = m;",
    "        m = b < 0 ? -b : b; if (m > peak) peak = m;",
    "        r = (r + 1) | 0; had = true;",
    "      } else {",
    "        // Ran dry in the middle of a quantum: an underrun (an empty",
    "        // ring at its start is the mixer in standby).",
    "        dry = had;",
    "        left[i] = 0; right[i] = 0;",
    "      }",
    "    }",
    "    if (dry) Atomics.add(ctl, 4, 1);",
    "    Atomics.store(ctl, 1, r);",
    "    Atomics.store(ctl, 5, peak);",
    "    return true;",
    "  }",
    "}",
    "registerProcessor('arcwasm-audio', ArcWasmAudio);"
  ].join('\n');

  function log(line) {
    (Module['printErr'] || console.log)('audio: ' + line);
  }

  function update() {
    if (!audio) return;
    var running = audio.ctx.state == 'running' && audio.node ? 1 : 0;
    Atomics.store(audio.ctl, 2, running);
    Atomics.store(audio.ctl, 3, audio.ctx.sampleRate | 0);
  }

  function supported() {
    return !!(window.AudioContext || window.webkitAudioContext) &&
           typeof AudioWorkletNode != 'undefined' && typeof SharedArrayBuffer != 'undefined';
  }

  // The page's context (one: iOS allows few), at the mixer's rate if the
  // browser takes it.
  function context() {
    if (ctx) return ctx;
    var AC = window.AudioContext || window.webkitAudioContext;
    try {
      ctx = new AC({ sampleRate: RATE, latencyHint: 'interactive' });
    } catch (e) {
      ctx = new AC();  // the worklet resamples
    }
    ctx.onstatechange = function () {
      update();
      log('AudioContext ' + ctx.state + ' at ' + ctx.sampleRate + ' Hz');
      if (ctx.state == 'running') started = true;
      if (ctx.state == 'running' && audio) playbackSession(false);
    };
    return ctx;
  }

  // A user gesture: start (or restart) sound. Made here, the context starts
  // at once; without an app's output it waits suspended for one.
  function gesture(e) {
    if (!supported() || (e && e.isTrusted === false)) return;
    gestures++;
    var c = context();
    if (!audio && started) return;  // no output: stays suspended
    if (c.state != 'running') {
      c.resume().then(function () {
        update();
        if (!audio) c.suspend();  // started; nothing to play yet
      }, function (err) { log('resume failed: ' + err); });
    }
    if (audio) playbackSession(true);
  }

  ['touchend', 'click', 'keydown', 'pointerup', 'mousedown'].forEach(function (type) {
    document.addEventListener(type, gesture, true);
  });

  // The page comes back (iOS stops sound while it is hidden).
  document.addEventListener('visibilitychange', function () {
    if (document.hidden) {
      if (media) media.pause();
      return;
    }
    if (audio && ctx && ctx.state != 'running') {
      ctx.resume().then(update, function () { /* the next gesture */ });
    }
    if (audio) playbackSession(false);
  });

  // iOS: sound past the silent switch (see the top). |inGesture|: may start
  // the silent media element.
  function playbackSession(inGesture) {
    var session = navigator.audioSession;
    if (session && 'type' in session) {
      if (session.type != 'playback') {
        try {
          session.type = 'playback';
          log('audio session: playback');
        } catch (e) {
          log('audio session: ' + e);
        }
      }
      return;
    }
    if (!/iPhone|iPad|iPod/.test(navigator.userAgent) &&
        !(navigator.platform == 'MacIntel' && navigator.maxTouchPoints > 1)) {
      return;  // only iOS has the switch
    }
    if (!media) {
      if (!inGesture) return;
      media = document.createElement('audio');
      media.setAttribute('x-webkit-airplay', 'deny');
      media.setAttribute('playsinline', '');
      media.disableRemotePlayback = true;
      media.preload = 'auto';
      media.loop = true;
      media.src = URL.createObjectURL(silentWav());
    }
    if (media.paused && !document.hidden) {
      var p = media.play();
      if (p && p.then) {
        p.then(function () { log('audio session: playback (silent media element)'); },
               function (err) { log('silent media element: ' + err); });
      }
    }
  }

  // 1 s of silence, 8 kHz mono 8-bit (0x80 is zero).
  function silentWav() {
    var n = 8000, b = new Uint8Array(44 + n), v = new DataView(b.buffer);
    var text = function (at, t) { for (var i = 0; i < t.length; i++) b[at + i] = t.charCodeAt(i); };
    text(0, 'RIFF'); v.setUint32(4, 36 + n, true); text(8, 'WAVE');
    text(12, 'fmt '); v.setUint32(16, 16, true); v.setUint16(20, 1, true); v.setUint16(22, 1, true);
    v.setUint32(24, 8000, true); v.setUint32(28, 8000, true); v.setUint16(32, 1, true);
    v.setUint16(34, 8, true); text(36, 'data'); v.setUint32(40, n, true);
    b.fill(0x80, 44);
    return new Blob([b], { type: 'audio/wav' });
  }

  Module['arcwasmAudioOpen'] = function (ctlPtr, ringPtr, frames, rate) {
    if (audio) return 1;
    if (!supported()) return 0;
    RATE = rate;
    context();
    audio = { ctx: ctx, node: null, analyser: null, ctl: new Int32Array(HEAP8.buffer, ctlPtr, 8) };
    // Started by an earlier gesture: on again (no gesture needed now).
    if (started && ctx.state != 'running') ctx.resume().then(update, function () {});
    var url = URL.createObjectURL(new Blob([PROCESSOR], { type: 'application/javascript' }));
    ctx.audioWorklet.addModule(url).then(function () {
      var node = new AudioWorkletNode(ctx, 'arcwasm-audio', {
        numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2],
        processorOptions: { buffer: HEAP8.buffer, ctl: ctlPtr, ring: ringPtr, frames: frames,
                            step: rate / ctx.sampleRate }
      });
      var analyser = ctx.createAnalyser();
      analyser.fftSize = 4096;
      node.connect(ctx.destination);
      node.connect(analyser);
      audio.node = node;
      audio.analyser = analyser;
      update();
      if (ctx.state == 'running') playbackSession(false);
      log('output ready (' + ctx.sampleRate + ' Hz' + (ctx.sampleRate != rate ? ', resampled from ' + rate : '') +
          ', ' + ctx.state + (ctx.state == 'running' ? '' : ': starts with the first touch or key') + ')');
    }, function (e) {
      log('AudioWorklet failed: ' + e);
    });
    return 1;
  };

  Module['arcwasmAudioResume'] = gesture;

  // The same worklet on a ring of a 440 Hz tone, rendered by an
  // OfflineAudioContext (no autoplay policy applies): whether this browser
  // runs an AudioWorklet over a SharedArrayBuffer at all. Resolves to
  // {peakHz, rms}.
  Module['arcwasmAudioSelfCheck'] = function () {
    var OAC = window.OfflineAudioContext || window.webkitOfflineAudioContext;
    if (!OAC || typeof AudioWorkletNode == 'undefined' || typeof SharedArrayBuffer == 'undefined') {
      return Promise.reject(new Error('no OfflineAudioContext/AudioWorklet/SharedArrayBuffer'));
    }
    var frames = 8192, rate = 44100;
    var sab = new SharedArrayBuffer(32 + frames * 4);
    var ctl = new Int32Array(sab, 0, 8), ring = new Int16Array(sab, 32, frames * 2);
    for (var i = 0; i < frames; i++) {
      ring[2 * i] = ring[2 * i + 1] = Math.round(Math.sin(2 * Math.PI * 440 * i / rate) * 12000);
    }
    Atomics.store(ctl, 0, frames);
    var ctx = new OAC(2, frames, rate);
    var url = URL.createObjectURL(new Blob([PROCESSOR], { type: 'application/javascript' }));
    return ctx.audioWorklet.addModule(url).then(function () {
      var node = new AudioWorkletNode(ctx, 'arcwasm-audio', {
        numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2],
        processorOptions: { buffer: sab, ctl: 0, ring: 32, frames: frames }
      });
      node.connect(ctx.destination);
      return ctx.startRendering();
    }).then(function (buffer) {
      var d = buffer.getChannelData(0), sum = 0, crossings = 0;
      for (var i = 0; i < d.length; i++) {
        sum += d[i] * d[i];
        if (i && d[i - 1] < 0 && d[i] >= 0) crossings++;
      }
      return { rms: Math.sqrt(sum / d.length), peakHz: Math.round(crossings * rate / d.length),
               played: Atomics.load(ctl, 1) };
    });
  };

  // What the page outputs right now.
  Module['arcwasmAudioStats'] = function () {
    if (!audio) return { state: 'none' };
    var s = {
      state: audio.ctx.state, rate: audio.ctx.sampleRate,
      written: Atomics.load(audio.ctl, 0) >>> 0, played: Atomics.load(audio.ctl, 1) >>> 0,
      underruns: Atomics.load(audio.ctl, 4), rms: 0, peakHz: 0
    };
    var a = audio.analyser;
    if (a) {
      var t = new Float32Array(a.fftSize);
      a.getFloatTimeDomainData(t);
      var sum = 0;
      for (var i = 0; i < t.length; i++) sum += t[i] * t[i];
      s.rms = Math.sqrt(sum / t.length);
      var f = new Float32Array(a.frequencyBinCount);
      a.getFloatFrequencyData(f);
      var best = 1;
      for (var k = 2; k < f.length; k++) if (f[k] > f[best]) best = k;
      s.peakHz = Math.round(best * audio.ctx.sampleRate / a.fftSize);
    }
    return s;
  };
})();
