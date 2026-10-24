// The browser half of arc-wasm's media decoders (shim/media/webcodecs.cc), a
// --pre-js of every artvm build: WebCodecs' VideoDecoder and AudioDecoder
// behind android::MediaCodec (shim/media/media_codec.cc).
//
// Everything here runs on one pthread, the "media" thread, which returns to
// its event loop (emscripten_exit_with_live_runtime) instead of running a C
// loop: WebCodecs delivers frames, samples and errors through callbacks and
// promises, and only a worker that goes back to its event loop gets them.
// App threads (MediaCodec's callers, ART threads that never return to
// theirs) proxy each call here (emscripten_proxy_async); decoded output goes
// back into wasm memory through the exported arc_media_* functions, which
// take the codec's lock for a moment and wake its waiters.
//
// Per decoder (keyed by its C++ object's address):
//   decode   EncodedVideoChunk/EncodedAudioChunk from a copy of the input
//   output   frames and samples join one queue (items) in decoding order;
//            pump() moves them into the codec's output buffers one at a time:
//            a VideoFrame is copied (copyTo, asynchronous) into a staging
//            buffer and converted there by C (arc_media_video_frame: I420 for
//            ByteBuffer output, kept as is for a Surface); AudioData becomes
//            16-bit interleaved PCM (synchronous). When the codec has no free
//            output buffer, pump stops and C calls it again on release.
//   flush    end of stream: decoder.flush(), then an end-of-stream item
//   reset    MediaCodec.flush(): decoder.reset() and a new epoch; items of an
//            older epoch are dropped (C configures the decoder again)
// A frame with no CPU-readable format (VideoFrame.format null) is drawn into
// an OffscreenCanvas and read back as RGBA.
//
// ArcMedia.probe answers MediaCodecList: isConfigSupported for each
// candidate configuration, in this browser.
// Part of the arc-wasm shim layer (topic mediacodec).

var ArcMedia = (function () {
  var hasVideo = typeof VideoDecoder == 'function';
  var hasAudio = typeof AudioDecoder == 'function';
  var decoders = new Map();
  var copyToShared = true;  // VideoFrame.copyTo straight into wasm memory

  var VFMT = {I420: 1, I420A: 2, I422: 3, I444: 4, NV12: 5, RGBA: 6, RGBX: 7, BGRA: 8, BGRX: 9};
  var MATRIX = {bt709: 1, bt470bg: 2, smpte170m: 2, bt2020: 3, 'bt2020-ncl': 3, rgb: 4};

  function heap() { return HEAPU8; }

  // Decoded frames held outside the wasm memory (decoder memory, often the
  // GPU's), counted for the memory budget (shim/membudget/mem_budget_pre.js):
  // every VideoFrame is closed as soon as it is copied out or dropped.
  // (Looked up when used: the budget's pre-js may come after this one.)
  function mem() { return typeof ArcMemBudget != 'undefined' ? ArcMemBudget : null; }
  function frameBytes(f) {
    try { return f.allocationSize(); } catch (e) {}
    return (f.codedWidth * f.codedHeight * 3) >> 1;
  }
  function closeFrame(it) {
    if (!it.frame) return;
    it.frame.close();
    it.frame = null;
    if (mem()) mem().frameClosed(it.bytes);
  }
  function str(p, n) { return UTF8ToString(p, n); }

  function errorCode(e) {
    // MediaCodec reports these as CodecException error codes.
    switch (e && e.name) {
      case 'NotSupportedError': return 1;
      case 'DataError': case 'EncodingError': return 2;
      case 'QuotaExceededError': return 3;
      default: return 4;
    }
  }

  function fail(st, e, what) {
    if (st.closed) return;
    err('MediaCodec: ' + (st.video ? 'VideoDecoder' : 'AudioDecoder') + ' ' + what + ': ' +
        (e && e.name ? e.name + ': ' + e.message : e));
    _arc_media_error(st.h, st.epoch, errorCode(e));
  }

  // decodeQueueSize goes down as the decoder takes chunks; without 'dequeue'
  // events (older browsers) it is polled while chunks wait.
  function setQueue(st) {
    if (st.closed || !st.dec) return;
    var n = st.dec.decodeQueueSize | 0;
    _arc_media_queue(st.h, n);
    if (n > 0 && !st.dequeueEvents && !st.polling) {
      st.polling = true;
      setTimeout(function () { st.polling = false; setQueue(st); }, 4);
    }
  }

  function create(h, video, epoch) {
    if (decoders.has(h)) close(h);  // Recreate after an error
    var st = {h: h, video: !!video, epoch: epoch, items: [], busy: false, closed: false,
              dec: null, canvas: null, ctx: null, tmp: null};
    var init = {
      output: function (o) {
        if (st.closed) { o.close(); return; }
        if (st.video) {
          var b = frameBytes(o);
          if (mem()) mem().frameOpened(b);
          st.items.push({frame: o, bytes: b, epoch: st.epoch});
        } else {
          st.items.push({audio: o, epoch: st.epoch});
        }
        setQueue(st);
        pump(st);
      },
      error: function (e) { fail(st, e, 'error'); }
    };
    try {
      st.dec = video ? new VideoDecoder(init) : new AudioDecoder(init);
    } catch (e) {
      fail(st, e, 'constructor');
      return -1;
    }
    st.dequeueEvents = 'ondequeue' in st.dec;
    if (st.dequeueEvents) st.dec.addEventListener('dequeue', function () { setQueue(st); });
    decoders.set(h, st);
    if (mem()) mem().decoders(1);
    return 0;
  }

  function configure(h, codec, desc, descLen, width, height, rate, channels) {
    var st = decoders.get(h);
    if (!st || st.closed) return -1;
    var cfg = {codec: codec};
    if (descLen > 0) cfg.description = heap().slice(desc, desc + descLen);
    if (st.video) {
      if (width > 0 && height > 0) { cfg.codedWidth = width; cfg.codedHeight = height; }
      cfg.optimizeForLatency = true;
    } else {
      cfg.sampleRate = rate;
      cfg.numberOfChannels = channels;
    }
    try {
      st.dec.configure(cfg);
    } catch (e) {
      fail(st, e, 'configure(' + JSON.stringify({codec: codec, w: width, h: height, rate: rate, ch: channels}) + ')');
      return -1;
    }
    return 0;
  }

  function decode(h, p, n, pts, key) {
    var st = decoders.get(h);
    if (!st || st.closed || st.dec.state != 'configured') return;
    var data = heap().slice(p, p + n);
    try {
      st.dec.decode(st.video
          ? new EncodedVideoChunk({type: key ? 'key' : 'delta', timestamp: pts, data: data})
          : new EncodedAudioChunk({type: 'key', timestamp: pts, data: data}));
    } catch (e) {
      fail(st, e, 'decode');
    }
    setQueue(st);
  }

  function flush(h, epoch) {
    var st = decoders.get(h);
    if (!st || st.closed) return;
    var eos = function () {
      if (st.closed || st.epoch != epoch) return;
      st.items.push({eos: true, epoch: epoch});
      pump(st);
    };
    if (st.dec.state != 'configured') { eos(); return; }
    st.dec.flush().then(eos, function (e) {
      // A reset or close aborts the flush: no end of stream for that epoch.
      if (st.closed || st.epoch != epoch) return;
      if (e && e.name == 'AbortError') return;
      fail(st, e, 'flush');
    });
  }

  function dropItems(st) {
    st.items.forEach(function (it) {
      closeFrame(it);
      if (it.audio) it.audio.close();
    });
    st.items = [];
  }

  function reset(h, epoch) {
    var st = decoders.get(h);
    if (!st || st.closed) return;
    st.epoch = epoch;
    dropItems(st);
    try { if (st.dec.state != 'closed') st.dec.reset(); } catch (e) {}
    setQueue(st);
    _arc_media_backlog(st.h, 0);
  }

  function close(h) {
    var st = decoders.get(h);
    if (!st) return;
    st.closed = true;
    dropItems(st);
    try { if (st.dec.state != 'closed') st.dec.close(); } catch (e) {}
    decoders.delete(h);
    if (mem()) mem().decoders(-1);
  }

  // A video frame into the codec's staging buffer: [format, w, h, planes,
  // offset0, stride0, ..., offset3, stride3, visible x, y, w, h, matrix, full range]
  function frameInfo(st, f, format, layout, w, h) {
    var info = _arc_media_frame_info(st.h) >>> 2;
    var I = HEAP32;
    I[info] = format; I[info + 1] = w; I[info + 2] = h; I[info + 3] = layout.length;
    for (var i = 0; i < 4; i++) {
      I[info + 4 + 2 * i] = i < layout.length ? layout[i].offset : 0;
      I[info + 5 + 2 * i] = i < layout.length ? layout[i].stride : 0;
    }
    var v = f.visibleRect || {x: 0, y: 0, width: w, height: h};
    I[info + 12] = v.x; I[info + 13] = v.y; I[info + 14] = v.width; I[info + 15] = v.height;
    var cs = f.colorSpace || {};
    I[info + 16] = MATRIX[cs.matrix] || 0;
    I[info + 17] = cs.fullRange ? 1 : 0;
  }

  // Copies frame f of item it into the staging buffer; pump continues when done.
  function copyFrame(st, it) {
    var f = it.frame;
    var format = VFMT[f.format] || 0;
    if (!format) {
      // No CPU-readable layout: draw it and read RGBA back (synchronous).
      var w = f.displayWidth, hh = f.displayHeight;
      if (!st.canvas || st.canvas.width != w || st.canvas.height != hh) {
        st.canvas = new OffscreenCanvas(w, hh);
        st.ctx = st.canvas.getContext('2d', {willReadFrequently: true});
      }
      st.ctx.drawImage(f, 0, 0, w, hh);
      var img = st.ctx.getImageData(0, 0, w, hh);
      var p = _arc_media_staging(st.h, img.data.length) >>> 0;
      if (!p) { fail(st, 'out of memory', 'staging'); return false; }
      heap().set(img.data, p);
      it.ts = f.timestamp; it.format = VFMT.RGBA;
      it.layout = [{offset: 0, stride: w * 4}]; it.w = w; it.h = hh;
      it.vis = {x: 0, y: 0, width: w, height: hh}; it.cs = {};
      frameInfo(st, {visibleRect: it.vis, colorSpace: f.colorSpace}, it.format, it.layout, w, hh);
      it.infoDone = true;
      closeFrame(it); it.copied = true;
      return true;
    }
    var size = f.allocationSize();
    var p = _arc_media_staging(st.h, size) >>> 0;
    if (!p) { fail(st, 'out of memory', 'staging'); return false; }
    st.busy = true;
    var epoch = it.epoch;
    var done = function (layout, tmp) {
      st.busy = false;
      if (st.closed || st.epoch != epoch) { closeFrame(it); return; }
      if (tmp) heap().set(tmp, p);
      it.ts = f.timestamp; it.format = format; it.layout = layout;
      it.w = f.codedWidth; it.h = f.codedHeight;
      frameInfo(st, f, format, layout, f.codedWidth, f.codedHeight);
      it.infoDone = true;
      closeFrame(it); it.copied = true;
      pump(st);
    };
    var viaCopy = function () {
      var tmp = new Uint8Array(size);
      f.copyTo(tmp).then(function (layout) { done(layout, tmp); },
                         function (e) { st.busy = false; fail(st, e, 'VideoFrame.copyTo'); });
    };
    if (copyToShared) {
      try {
        f.copyTo(heap().subarray(p, p + size)).then(function (layout) { done(layout, null); },
            function (e) { copyToShared = false; viaCopy(); });
      } catch (e) {
        copyToShared = false;
        viaCopy();
      }
    } else {
      viaCopy();
    }
    return false;
  }

  // AudioData a as 16-bit interleaved PCM at p.
  function pcm16(st, a, p) {
    var n = a.numberOfFrames, ch = a.numberOfChannels, out = p >>> 1, H = HEAP16;
    var planar = null;
    try {
      planar = [];
      for (var c = 0; c < ch; c++) {
        var f = new Float32Array(n);
        a.copyTo(f, {planeIndex: c, format: 'f32-planar'});
        planar.push(f);
      }
    } catch (e) {
      planar = null;
    }
    if (planar) {
      for (var i = 0; i < n; i++) {
        for (var c = 0; c < ch; c++) {
          var s = planar[c][i];
          H[out++] = s >= 1 ? 32767 : s <= -1 ? -32768 : (s * 32767) | 0;
        }
      }
      return;
    }
    // No format conversion in copyTo: the data's own format.
    var fmt = a.format;
    if (fmt == 's16') {
      var t = new Int16Array(n * ch);
      a.copyTo(t, {planeIndex: 0});
      H.set(t, out);
      return;
    }
    var interleaved = fmt.indexOf('-planar') < 0;
    var planes = interleaved ? 1 : ch;
    var get = [];
    for (var c = 0; c < planes; c++) {
      var bytes = a.allocationSize({planeIndex: c});
      var buf = new ArrayBuffer(bytes);
      a.copyTo(buf, {planeIndex: c});
      var v = fmt.indexOf('f32') == 0 ? new Float32Array(buf) : fmt.indexOf('s32') == 0 ? new Int32Array(buf)
            : fmt.indexOf('s16') == 0 ? new Int16Array(buf) : new Uint8Array(buf);
      get.push(v);
    }
    var scale = fmt.indexOf('f32') == 0 ? 32767 : fmt.indexOf('s32') == 0 ? 1 / 65536 : 1;
    for (var i = 0; i < n; i++) {
      for (var c = 0; c < ch; c++) {
        var x = interleaved ? get[0][i * ch + c] : get[c][i];
        var s = fmt.indexOf('u8') == 0 ? (x - 128) << 8 : x * scale;
        H[out++] = s > 32767 ? 32767 : s < -32768 ? -32768 : s | 0;
      }
    }
  }

  function pump(st) {
    while (!st.closed && !st.busy && st.items.length) {
      var it = st.items[0];
      if (it.epoch != st.epoch) {
        closeFrame(it);
        if (it.audio) it.audio.close();
        st.items.shift();
        continue;
      }
      if (it.eos) {
        if (!_arc_media_eos(st.h, it.epoch)) break;  // no free output buffer yet
        st.items.shift();
        continue;
      }
      if (it.audio) {
        var a = it.audio;
        var p = _arc_media_audio_slot(st.h, it.epoch, a.numberOfFrames, a.numberOfChannels,
                                      a.sampleRate, a.timestamp) >>> 0;
        if (!p) break;
        try {
          pcm16(st, a, p);
        } catch (e) {
          fail(st, e, 'AudioData.copyTo');
        }
        _arc_media_audio_done(st.h);
        a.close();
        st.items.shift();
        continue;
      }
      if (!it.copied) {
        if (!copyFrame(st, it)) break;  // copying (pump runs again when done) or failed
      }
      // Copied into staging: C converts it into an output buffer (0: none free).
      if (!_arc_media_video_frame(st.h, it.epoch, it.ts)) break;
      st.items.shift();
    }
    if (!st.closed) _arc_media_backlog(st.h, st.items.length);
  }

  function pumpNow(h) {
    var st = decoders.get(h);
    if (st) pump(st);
  }

  // MediaCodecList: lines "V codec width height" / "A codec rate channels";
  // writes '1'/'0' per line at out, then calls arc_media_probe_done(token).
  function probe(list, out, token) {
    var lines = str(list).split('\n').filter(function (l) { return l.length; });
    var ps = lines.map(function (l, i) {
      var f = l.split(' ');
      var put = function (ok) { HEAPU8[out + i] = ok ? 49 : 48; };
      try {
        if (f[0] == 'V') {
          if (!hasVideo) return put(false);
          return VideoDecoder.isConfigSupported({codec: f[1], codedWidth: +f[2], codedHeight: +f[3]})
              .then(function (r) { put(r.supported); }, function () { put(false); });
        }
        if (!hasAudio) return put(false);
        return AudioDecoder.isConfigSupported({codec: f[1], sampleRate: +f[2], numberOfChannels: +f[3]})
            .then(function (r) { put(r.supported); }, function () { put(false); });
      } catch (e) {
        put(false);
      }
    });
    Promise.all(ps).then(function () { _arc_media_probe_done(token); });
  }

  return {
    available: function () { return (hasVideo ? 1 : 0) | (hasAudio ? 2 : 0); },
    create: create, configure: configure, decode: decode, flush: flush,
    reset: reset, close: close, pump: pumpNow, probe: probe
  };
})();
