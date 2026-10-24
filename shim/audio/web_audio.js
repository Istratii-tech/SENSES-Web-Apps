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
// context is created when an app first plays (suspended if there was no
// gesture yet) and resumed by the first pointerdown/keydown/touchend on the
// page (Chromium and Safari both accept resume() there). Until then ctl[2]
// is 0 and the mixer lets a clock consume the output.
//
// Module.arcwasmAudioResume() resumes from a page's own handler (a "play"
// button); Module.arcwasmAudioStats() reports the context state, frames
// played, underruns, and the level and dominant frequency the page actually
// outputs (an AnalyserNode after the worklet) -- how tests and demos check
// that sound is produced without listening to it.
// Part of the arc-wasm shim layer (topic gles-audio).

if (typeof window != 'undefined' && typeof document != 'undefined') (function () {
  var audio = null;  // {ctx, node, analyser, ctl, rate}

  var PROCESSOR = [
    "class ArcWasmAudio extends AudioWorkletProcessor {",
    "  constructor(options) {",
    "    super();",
    "    var o = options.processorOptions;",
    "    this.ctl = new Int32Array(o.buffer, o.ctl, 8);",
    "    this.ring = new Int16Array(o.buffer, o.ring, o.frames * 2);",
    "    this.mask = o.frames - 1;",
    "  }",
    "  process(inputs, outputs) {",
    "    var out = outputs[0], left = out[0], right = out[1] || out[0];",
    "    var ctl = this.ctl, ring = this.ring, mask = this.mask;",
    "    var r = Atomics.load(ctl, 1), w = Atomics.load(ctl, 0);",
    "    var peak = 0, n = left.length, had = false, dry = false;",
    "    for (var i = 0; i < n; i++) {",
    "      if (((w - r) | 0) > 0) {",
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

  function resume() {
    if (audio && audio.ctx.state != 'running') {
      audio.ctx.resume().then(update, function (e) { log('resume failed: ' + e); });
    }
  }

  ['pointerdown', 'mousedown', 'keydown', 'touchend'].forEach(function (type) {
    document.addEventListener(type, resume, true);
  });

  Module['arcwasmAudioOpen'] = function (ctlPtr, ringPtr, frames, rate) {
    if (audio) return 1;
    var AC = window.AudioContext || window.webkitAudioContext;
    if (!AC || typeof AudioWorkletNode == 'undefined' || typeof SharedArrayBuffer == 'undefined') {
      return 0;
    }
    var ctx;
    try {
      ctx = new AC({ sampleRate: rate, latencyHint: 'interactive' });
    } catch (e) {
      ctx = new AC();  // the browser resamples the worklet's output
    }
    audio = { ctx: ctx, node: null, analyser: null, ctl: new Int32Array(HEAP8.buffer, ctlPtr, 8) };
    ctx.onstatechange = function () {
      update();
      log('AudioContext ' + ctx.state + ' at ' + ctx.sampleRate + ' Hz');
    };
    var url = URL.createObjectURL(new Blob([PROCESSOR], { type: 'application/javascript' }));
    ctx.audioWorklet.addModule(url).then(function () {
      var node = new AudioWorkletNode(ctx, 'arcwasm-audio', {
        numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2],
        processorOptions: { buffer: HEAP8.buffer, ctl: ctlPtr, ring: ringPtr, frames: frames }
      });
      var analyser = ctx.createAnalyser();
      analyser.fftSize = 4096;
      node.connect(ctx.destination);
      node.connect(analyser);
      audio.node = node;
      audio.analyser = analyser;
      update();
      log('output ready (' + ctx.sampleRate + ' Hz, ' + ctx.state +
          (ctx.state == 'running' ? '' : ': starts with the first click or key') + ')');
    }, function (e) {
      log('AudioWorklet failed: ' + e);
    });
    return 1;
  };

  Module['arcwasmAudioResume'] = resume;

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
