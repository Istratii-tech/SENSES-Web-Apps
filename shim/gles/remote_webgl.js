// WebGL2 for threads of a browser without WebGL in workers (Safari before 17:
// iOS 15-16 phones), topic userspace.
//
// An app's GL thread (GLSurfaceView's, say) draws on a WebGL2 context of its
// own on an OffscreenCanvas (shim/graphics_translation/egl/native_webgl.cpp).
// Safari before 17 has no WebGL there: the game drew nothing (black screen).
// The page's main thread does have WebGL2, so here the thread gets a stand-in
// context (worker side, create()) whose calls run on a real WebGL2 context of
// a hidden canvas on the main thread (main side, Module.arcGlRemote):
//
//   * calls without a result are queued and sent in one message when a call
//     needs its result (a getter, readPixels at eglSwapBuffers: about one
//     round trip per frame), or when the queue grows long;
//   * a call with a result is sent with the queue, and the thread waits
//     (Atomics.wait) for the main thread to write the result into a shared
//     buffer; views of the wasm memory go as they are (the main thread reads
//     and writes the shared memory itself: readPixels lands in the layer);
//   * queued calls copy the memory they read when they are made (Emscripten
//     passes the whole heap with an offset and a length; RANGES below), or,
//     where the length is not given (texture uploads), run at once instead;
//   * WebGL objects are numbers on the thread (create* numbers them there,
//     so they are queued too) and objects on the main thread.
//
// Messages reach the main thread through Emscripten's worker message
// handler (CMD_CALL_HANDLER: Module[handler](...args)). In a pre-js, so it
// runs in the page and in every thread. Test it in a browser that has WebGL
// in workers with ?remotegl=1 on the page (the threads see it in the
// script's URL).

(function () {
  var CMD_CALL_HANDLER = 9;  // Emscripten's src/lib/libpthread.js
  var RESULT_CHARS = 1 << 20;
  var QUEUE_MAX = 4096;

  // Queued calls travel in a SharedArrayBuffer of the context (the stream),
  // not as an array per call in a message: at ~5000 calls a second (Angry
  // Birds' menu) the arrays, their structured clones on the page and a
  // copy (ArrayBuffer) per uniform were garbage the thread's and the
  // page's GC let grow by ~75 MB a minute on an iPhone X (docs/topics/
  // ios-mem.md). In 8-byte slots, per call: [name id (int32), argc
  // (int32)], target id (f64), id of a create* (f64, 0: none), then per
  // argument [tag (int32), kind (int32)], value (f64), and for data and
  // text their bytes (UTF-16 units for text), to the next slot. A batch's
  // message says how many calls and names any new method names (id: their
  // order); the page answers every batch, so the stream is written again
  // from its start. A call the thread waits for goes last in the stream,
  // its views of the wasm memory as [TAG_HEAP, kind], byte offset, length
  // (the page reads and writes the memory itself); a view of other memory
  // it reads into (getBufferSubData, readPixels) as [TAG_OUT, kind], count
  // and the view's bytes, which the page overwrites and the thread copies
  // back. Arrays of numbers (drawBuffers, invalidateFramebuffer) as
  // [TAG_NUMS], count, the numbers.
  var STREAM_BYTES = 256 << 10;
  var MAX_ARGS = 12;  // WebGL 2 methods take at most 12 (texSubImage3D)
  var SMALL_COPY = 1024;  // bytes copied in a loop, not through a view
  var TAG_UNDEFINED = 0, TAG_NULL = 1, TAG_NUMBER = 2, TAG_BOOL = 3, TAG_REF = 4,
      TAG_DATA = 5, TAG_STRING = 6, TAG_JSON = 7, TAG_HEAP = 8, TAG_OUT = 9, TAG_NUMS = 10;
  var KINDS = [Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array, Int32Array,
               Uint32Array, Float32Array, Float64Array];
  var CALL_SYNC = 0, CALL_QUEUED = 1, CALL_CREATE = 2, CALL_DELETE = 3;  // DELETE: queued too
  // A result in ctl[2] (else JSON in the result buffer): a number or a
  // boolean in the f64 after ctl, or null; a WebGL object by its id in that
  // f64; a typed array (kind in ctl[3], length in ctl[1]) as f64s in the
  // result buffer.
  var ANSWER_JSON = 0, ANSWER_NUMBER = 1, ANSWER_NULL = 2, ANSWER_BOOL = 3, ANSWER_REF = 4,
      ANSWER_VIEW = 5;

  // Data views among the arguments: [view, offset, length] argument indices
  // (length: a number of elements, or -N: N elements).
  var RANGES = {
    bufferData: [1, 3, 4],
    bufferSubData: [2, 3, 4],
    compressedTexImage2D: [6, 7, 8],
    compressedTexSubImage2D: [7, 8, 9],
    compressedTexImage3D: [7, 8, 9],
    compressedTexSubImage3D: [9, 10, 11],
    clearBufferfv: [2, 3, -4],
    clearBufferiv: [2, 3, -4],
    clearBufferuiv: [2, 3, -4],
  };
  ['1', '2', '3', '4'].forEach(function (n) {
    ['f', 'i', 'ui'].forEach(function (t) { RANGES['uniform' + n + t + 'v'] = [1, 2, 3]; });
  });
  ['2', '3', '4', '2x3', '2x4', '3x2', '3x4', '4x2', '4x3'].forEach(function (n) {
    RANGES['uniformMatrix' + n + 'fv'] = [2, 3, 4];
  });
  var CREATES = /^(create(Buffer|Framebuffer|Program|Renderbuffer|Shader|Texture|Query|Sampler|TransformFeedback|VertexArray)|fenceSync)$/;
  var NO_RESULT = /^(delete|bind|attach|detach|active|blend|blit|buffer|clear|color|compile|copy|cull|depth|disable|draw|enable|framebuffer|front|generate|hint|invalidate|line|link|pixel|polygon|read[A-Z]|render|sample|scissor|shader|stencil|tex|transform|uniform|use|validate|vertex|viewport|begin|end|flush|pause|resume|compressed|beginQuery|endQuery)/;

  function isShared(v) {
    return typeof SharedArrayBuffer != 'undefined' && v.buffer instanceof SharedArrayBuffer;
  }

  // WebGL2's (data, offset[, length]) forms: [data, offset, length] argument
  // indices (length -1: none, to the end). On the main thread such a call
  // gets the part itself (data.subarray) and no offset: Safari 16 is not to
  // be trusted with the offset (a readPixels into the heap at the wrong
  // place corrupts malloc's own data).
  var OFFSET_FORMS = {
    readPixels: [6, 7, -1],
    texImage2D: [8, 9, -1],
    texSubImage2D: [8, 9, -1],
    texImage3D: [9, 10, -1],
    texSubImage3D: [10, 11, -1],
    compressedTexImage2D: [6, 7, 8],
    compressedTexSubImage2D: [7, 8, 9],
    compressedTexImage3D: [7, 8, 9],
    compressedTexSubImage3D: [9, 10, 11],
    bufferData: [1, 3, 4],
    bufferSubData: [2, 3, 4],
    getBufferSubData: [2, 3, 4],
    clearBufferfv: [2, 3, -1],
    clearBufferiv: [2, 3, -1],
    clearBufferuiv: [2, 3, -1],
  };
  ['1', '2', '3', '4'].forEach(function (n) {
    ['f', 'i', 'ui'].forEach(function (t) { OFFSET_FORMS['uniform' + n + t + 'v'] = [1, 2, 3]; });
  });
  ['2', '3', '4', '2x3', '2x4', '3x2', '3x4', '4x2', '4x3'].forEach(function (n) {
    OFFSET_FORMS['uniformMatrix' + n + 'fv'] = [2, 3, 4];
  });

  // |args| of |name| with an (offset[, length]) form turned into a subarray.
  function withoutOffset(name, args) {
    var form = OFFSET_FORMS[name];
    if (!form || args.length <= form[1] || !ArrayBuffer.isView(args[form[0]])) return args;
    var off = args[form[1]];
    if (typeof off != 'number') return args;
    var len = form[2] >= 0 && typeof args[form[2]] == 'number' && args[form[2]] > 0 ? args[form[2]] : -1;
    var data = args[form[0]];
    var out = args.slice(0, form[1]);
    out[form[0]] = len >= 0 ? data.subarray(off, off + len) : data.subarray(off);
    return out;
  }

  // ---- The page's main thread: the real contexts. -------------------------

  if (typeof window != 'undefined') {
    var contexts = {};
    var warned = 0;
    // To the page's output too (Module.printErr: a phone's log reaches
    // tools/serve.py that way; console.warn does not).
    var warn = function (text) {
      if (Module['printErr']) Module['printErr'](text); else console.warn(text);
    };

    function encode(c, v, ext) {
      if (v === null || v === undefined) return null;
      var t = typeof v;
      if (t == 'number' || t == 'boolean' || t == 'string') return v;
      if (ArrayBuffer.isView(v)) return { T: v.constructor.name, v: Array.prototype.slice.call(v) };
      if (Array.isArray(v)) return v.map(function (x) { return encode(c, x); });
      if ((typeof WebGLActiveInfo != 'undefined' && v instanceof WebGLActiveInfo) ||
          (typeof WebGLShaderPrecisionFormat != 'undefined' && v instanceof WebGLShaderPrecisionFormat) ||
          Object.getPrototypeOf(v) === Object.prototype) {
        var o = {};
        for (var k in v) if (typeof v[k] != 'function') o[k] = v[k];
        return { O: o };
      }
      var id = idOf(c, v);
      return ext ? { $: id, x: methodsOf(v) } : { $: id };
    }

    // A WebGL object's number (the thread's, or one given here).
    function idOf(c, v) {
      var id = c.ids.get(v);
      if (id === undefined) {
        id = --c.lastMainId;
        c.ids.set(v, id);
        c.objs.set(id, v);
      }
      return id;
    }

    // Whether |v| is a WebGL object (goes to the thread by its number).
    function isObject(v) {
      return !Array.isArray(v) && !ArrayBuffer.isView(v) && Object.getPrototypeOf(v) !== Object.prototype &&
          !(typeof WebGLActiveInfo != 'undefined' && v instanceof WebGLActiveInfo) &&
          !(typeof WebGLShaderPrecisionFormat != 'undefined' && v instanceof WebGLShaderPrecisionFormat);
    }

    // The names of an object's methods (what the thread's stand-in calls on).
    function methodsOf(o) {
      var names = [];
      for (var k in o) if (typeof o[k] == 'function') names.push(k);
      return names;
    }

    // --- views of the shared wasm memory -------------------------------------------
    // Into the memory (readPixels, getBufferSubData): always through a buffer
    // of the page, then exactly the bytes asked for are copied in. Safari 16
    // given a view of the shared wasm memory wrote back the whole view (from
    // the destination to the end of the memory) as it was when the call
    // began, so what other threads wrote above it in the meantime was lost
    // (malloc's headers back to older states: Angry Birds 2.1.0 on an iPhone
    // X; topic userspace).
    // From the memory (texture uploads the thread waits for): the view itself
    // where WebGL takes views of shared memory; where it refuses them (older
    // WebKit: a TypeError; ?remotegl=noshared on the page tests it), private
    // copies of only the bytes the call reads (topic install8). Calls arrive
    // here with subarrays, not offsets (withoutOffset), so the image's size
    // says how many bytes.
    var noSharedViews = /[?&]remotegl=noshared\b/.test(location.search);

    function hasShared(args) {
      for (var i = 0; i < args.length; i++) {
        if (ArrayBuffer.isView(args[i]) && isShared(args[i])) return true;
      }
      return false;
    }

    // Bytes per pixel of a format/type pair (WebGL 2's).
    function pixelBytes(format, type) {
      var G = WebGL2RenderingContext;
      switch (type) {
        case G.UNSIGNED_SHORT_5_6_5: case G.UNSIGNED_SHORT_4_4_4_4: case G.UNSIGNED_SHORT_5_5_5_1:
          return 2;
        case G.UNSIGNED_INT_2_10_10_10_REV: case G.UNSIGNED_INT_10F_11F_11F_REV:
        case G.UNSIGNED_INT_5_9_9_9_REV: case G.UNSIGNED_INT_24_8:
          return 4;
        case G.FLOAT_32_UNSIGNED_INT_24_8_REV:
          return 8;
      }
      var comps = 4;
      switch (format) {
        case G.RGB: case G.RGB_INTEGER: comps = 3; break;
        case G.RG: case G.RG_INTEGER: case G.LUMINANCE_ALPHA: comps = 2; break;
        case G.RED: case G.RED_INTEGER: case G.LUMINANCE: case G.ALPHA:
        case G.DEPTH_COMPONENT: comps = 1; break;
      }
      var size = 1;
      switch (type) {
        case G.SHORT: case G.UNSIGNED_SHORT: case G.HALF_FLOAT: size = 2; break;
        case G.INT: case G.UNSIGNED_INT: case G.FLOAT: size = 4; break;
      }
      return comps * size;
    }

    // Where a w x h x d image lies in client memory, by the context's pack
    // (readPixels) or unpack (uploads) parameters: {pixel, row, image,
    // first, bytes}: bytes per pixel, row and image strides, the first
    // pixel's byte, the bytes from the start to the last pixel's end.
    function imageLayout(gl, pack, w, h, d, format, type) {
      var G = WebGL2RenderingContext;
      var p = function (unpackName, packName) {
        return gl.getParameter(G[pack ? packName : unpackName]) || 0;
      };
      var pixel = pixelBytes(format, type);
      var align = p('UNPACK_ALIGNMENT', 'PACK_ALIGNMENT') || 4;
      var rowLength = p('UNPACK_ROW_LENGTH', 'PACK_ROW_LENGTH') || w;
      var row = Math.ceil(rowLength * pixel / align) * align;
      var image = row * ((pack ? 0 : p('UNPACK_IMAGE_HEIGHT')) || h);
      var first = (pack ? 0 : p('UNPACK_SKIP_IMAGES')) * image +
          p('UNPACK_SKIP_ROWS', 'PACK_SKIP_ROWS') * row + p('UNPACK_SKIP_PIXELS', 'PACK_SKIP_PIXELS') * pixel;
      var bytes = w <= 0 || h <= 0 || d <= 0 ? 0 : first + (d - 1) * image + (h - 1) * row + w * pixel;
      return {pixel: pixel, row: row, image: image, first: first, bytes: bytes};
    }

    // Every 5 s to the log: frames read back, the canvas, how much of the
    // last frame was not black (a phone's log says where frames go black).
    function noteFrame(c, w, h, px, L) {
      c.frames = (c.frames || 0) + 1;
      var now = Date.now();
      if (c.noted && now - c.noted < 5000) return;
      c.noted = now;
      var lit = 0, n = 0;
      if (L.pixel == 4) {
        for (var i = 0; i + 3 < L.bytes; i += 4 * 97, n++) if (px[i] + px[i + 1] + px[i + 2] > 48) lit++;
      }
      var calls = [], waits = [];
      for (var k in (c.calls || {})) calls.push(k + ':' + c.calls[k]);
      for (var k in (c.waits || {})) if (c.waits[k]) waits.push(k + ':' + c.waits[k]), c.waits[k] = 0;
      c.calls = {};
      warn('remote WebGL: ' + c.frames + ' frames read, ' + w + 'x' + h + ' of a ' +
           c.canvas.width + 'x' + c.canvas.height + ' canvas, not black ' +
           (n ? Math.round(100 * lit / n) : '?') + '%' + (c.gl.isContextLost() ? ', context lost' : '') +
           ', error ' + c.gl.getError() + ', calls ' + calls.sort().join(' ') +
           ', copied in ' + ((c.bytesIn || 0) / 1048576).toFixed(1) + ' MB, ' + (c.queued || 0) +
           ' queued calls in ' + (c.batches || 0) + ' batches, waited for ' + waits.sort().join(' ') +
           ' since the last line');
      c.bytesIn = c.queued = c.batches = 0;
    }

    // The context's buffer of the page, at least |bytes| long, kept (a new
    // one per frame read back was ~25 MB a second for the page's GC), and
    // views of it by kind and length (uniforms: a few lengths).
    function scratch(c, bytes) {
      if (!c.scratch || c.scratch.byteLength < bytes) {
        c.scratch = new ArrayBuffer(Math.max((bytes + 7) & ~7, 2 * (c.scratch ? c.scratch.byteLength : 32768)));
        c.scratch8 = new Uint8Array(c.scratch);
        c.scratch32 = new Int32Array(c.scratch);
        c.views = KINDS.map(function () { return new Map(); });
      }
      return c.scratch;
    }

    function scratchView(c, kind, count) {
      var K = KINDS[kind];
      scratch(c, count * K.BYTES_PER_ELEMENT);
      if (count > 4096) return new K(c.scratch, 0, count);
      var v = c.views[kind].get(count);
      if (!v) c.views[kind].set(count, v = new K(c.scratch, 0, count));
      return v;
    }

    // readPixels(x, y, w, h, format, type, view[, offset]) into the thread's
    // memory through a buffer of the page: only the image's rows are copied
    // in (gaps between them are left as they are).
    function readPixelsInto(c, args) {
      var view = args[6], off = typeof args[7] == 'number' ? args[7] : 0;
      var w = args[2], h = args[3];
      var L = imageLayout(c.gl, true, w, h, 1, args[4], args[5]);
      if (!L.bytes) return;
      var tmp = scratchView(c, 1, L.bytes);
      c.gl.readPixels(args[0], args[1], w, h, args[4], args[5], tmp);
      noteFrame(c, w, h, tmp, L);
      var start = view.byteOffset + off * view.BYTES_PER_ELEMENT;
      if (start + L.bytes > view.buffer.byteLength) return;
      var dst = new Uint8Array(view.buffer, start, L.bytes);
      if (L.row == w * L.pixel) {  // rows without gaps: at once
        dst.set(L.first ? tmp.subarray(L.first) : tmp, L.first);
        return;
      }
      for (var y = 0; y < h; y++) {
        var at = L.first + y * L.row;
        dst.set(tmp.subarray(at, at + w * L.pixel), at);
      }
    }

    // arcReadPixels(x, y, w, h, view, offset): an RGBA frame of the default
    // framebuffer into the thread's memory, as shim/gles/egl_layer_surface.cc
    // reads a layer: the read framebuffer, pack buffer and pack alignment
    // set for it here and put back, where the thread asked for each with a
    // call it waited for (four round trips a frame, now one).
    var READ_FRAMEBUFFER = 0x8CA8, READ_FRAMEBUFFER_BINDING = 0x8CAA;
    var PIXEL_PACK_BUFFER = 0x88EB, PIXEL_PACK_BUFFER_BINDING = 0x88ED, PACK_ALIGNMENT = 0x0D05;
    function readFrame(c, a) {
      var gl = c.gl;
      var fb = gl.getParameter(READ_FRAMEBUFFER_BINDING), pack = gl.getParameter(PIXEL_PACK_BUFFER_BINDING);
      var align = gl.getParameter(PACK_ALIGNMENT);
      if (fb) gl.bindFramebuffer(READ_FRAMEBUFFER, null);
      if (pack) gl.bindBuffer(PIXEL_PACK_BUFFER, null);
      if (align != 4) gl.pixelStorei(PACK_ALIGNMENT, 4);
      var args = c.frameArgs || (c.frameArgs = [0, 0, 0, 0, gl.RGBA, gl.UNSIGNED_BYTE, null, 0]);
      args[0] = a[0]; args[1] = a[1]; args[2] = a[2]; args[3] = a[3]; args[6] = a[4]; args[7] = a[5];
      try {
        readPixelsInto(c, args);
      } finally {
        args[6] = null;
        if (align != 4) gl.pixelStorei(PACK_ALIGNMENT, align);
        if (pack) gl.bindBuffer(PIXEL_PACK_BUFFER, pack);
        if (fb) gl.bindFramebuffer(READ_FRAMEBUFFER, fb);
      }
    }

    // getBufferSubData(target, srcByteOffset, view[, dstOffset[, length]])
    // the same way.
    function getBufferSubDataInto(c, args) {
      var view = args[2];
      var off = typeof args[3] == 'number' ? args[3] : 0;
      var E = view.BYTES_PER_ELEMENT;
      var n = typeof args[4] == 'number' && args[4] > 0 ? args[4] * E : view.byteLength - off * E;
      var size = c.gl.getBufferParameter(args[0], c.gl.BUFFER_SIZE) - args[1];
      n = Math.max(0, Math.min(n, size));
      if (!n) return;
      var tmp = scratchView(c, 1, n);
      c.gl.getBufferSubData(args[0], args[1], tmp);
      new Uint8Array(view.buffer, view.byteOffset + off * E, n).set(tmp);
    }

    // [view index, w, h, d, format, type] of the uploads, as withoutOffset
    // leaves them (the view the image's first byte on).
    function uploadArgs(name, a) {
      switch (name) {
        case 'texImage2D': return a.length == 9 ? [8, a[3], a[4], 1, a[6], a[7]] : null;
        case 'texSubImage2D': return a.length == 9 ? [8, a[4], a[5], 1, a[6], a[7]] : null;
        case 'texImage3D': return a.length == 10 ? [9, a[3], a[4], a[5], a[7], a[8]] : null;
        case 'texSubImage3D': return a.length == 11 ? [10, a[5], a[6], a[7], a[8], a[9]] : null;
      }
      return null;
    }

    // The call with private copies of the shared views it reads: an image's
    // bytes only, other views (bounded by their length) whole; in the
    // context's buffer (scratch), all but the first view in new ones.
    function callUnshared(c, target, name, args) {
      var a = args.slice();
      var img = target === c.gl ? uploadArgs(name, a) : null;
      var first = true;
      for (var i = 0; i < a.length; i++) {
        var v = a[i];
        if (!ArrayBuffer.isView(v) || !isShared(v)) continue;
        var count = v.length;
        if (img && img[0] == i) {
          var bytes = imageLayout(c.gl, false, img[1], img[2], img[3], img[4], img[5]).bytes;
          count = Math.min(v.length, Math.ceil(bytes / v.BYTES_PER_ELEMENT));
        }
        var kind = KINDS.indexOf(v.constructor);
        var copy = first && kind >= 0 ? scratchView(c, kind, count) : new v.constructor(count);
        first = false;
        copy.set(v.subarray(0, count));
        a[i] = copy;
      }
      return target[name].apply(target, a);
    }

    function resolve(c, a) {
      return a !== null && typeof a == 'object' && a.$ !== undefined && !ArrayBuffer.isView(a) ? c.objs.get(a.$) : a;
    }

    function run(c, cmd) {
      var name = cmd[1];
      var args = cmd[2];
      if (name == '#size') {
        if (c.canvas.width != args[0]) c.canvas.width = args[0];
        if (c.canvas.height != args[1]) c.canvas.height = args[1];
        return undefined;
      }
      for (var i = 0; i < args.length; i++) {
        args[i] = resolve(c, args[i]);
        // Bytes the threads copied out of the wasm memory for the call
        // (textures, buffers: ArrayBuffers this page frees when it collects).
        if (ArrayBuffer.isView(args[i]) && !isShared(args[i])) c.bytesIn = (c.bytesIn || 0) + args[i].byteLength;
      }
      if (/^(draw|tex|clear|useProgram|bindFramebuffer|viewport)/.test(name)) {
        c.calls = c.calls || {};
        c.calls[name] = (c.calls[name] || 0) + 1;
      }
      if (!cmd[0] && name == 'arcReadPixels') {
        try {
          readFrame(c, args);
        } catch (e) {
          if (warned++ < 20) warn('remote WebGL: ' + name + ': ' + e);
        }
        return undefined;
      }
      if (!cmd[0] && ((name == 'readPixels' && ArrayBuffer.isView(args[6])) ||
                      (name == 'getBufferSubData' && ArrayBuffer.isView(args[2])))) {
        try {
          if (name == 'readPixels') readPixelsInto(c, args); else getBufferSubDataInto(c, args);
        } catch (e) {
          if (warned++ < 20) warn('remote WebGL: ' + name + ': ' + e);
        }
        return undefined;
      }
      if (!cmd[0]) args = withoutOffset(name, args);
      var target = cmd[0] ? c.objs.get(cmd[0]) : c.gl;
      var result;
      try {
        result = noSharedViews && hasShared(args) ? callUnshared(c, target, name, args)
                                                  : target[name].apply(target, args);
      } catch (e) {
        if (e instanceof TypeError && !noSharedViews && hasShared(args)) {
          // A WebGL without views of shared memory (older WebKit): copies from
          // now on.
          noSharedViews = true;
          warn('remote WebGL: ' + name + ' takes no views of shared memory (' + e +
               '): copies from now on');
          try {
            result = callUnshared(c, target, name, args);
          } catch (e2) {
            if (warned++ < 20) warn('remote WebGL: ' + name + ': ' + e2);
            return undefined;
          }
        } else {
          if (warned++ < 20) warn('remote WebGL: ' + name + ': ' + e);
          return undefined;
        }
      }
      // Shaders that do not compile or link: to the log (a phone's WebGL may
      // refuse what the Mac's takes; the app then draws nothing).
      if (name == 'compileShader' && c.gl && !c.gl.getShaderParameter(args[0], c.gl.COMPILE_STATUS) &&
          warned++ < 20) {
        warn('remote WebGL: shader not compiled: ' + c.gl.getShaderInfoLog(args[0]) + ' | ' +
             String(c.gl.getShaderSource(args[0])).slice(0, 300).replace(/\s+/g, ' '));
      } else if (name == 'linkProgram' && c.gl &&
                 !c.gl.getProgramParameter(args[0], c.gl.LINK_STATUS) && warned++ < 20) {
        warn('remote WebGL: program not linked: ' + c.gl.getProgramInfoLog(args[0]));
      }
      if (cmd[3]) {  // numbered on the thread (create*)
        c.objs.set(cmd[3], result);
        if (result) c.ids.set(result, cmd[3]);
      } else if (/^delete/.test(name) && cmd[2][0] && cmd[2][0].$ !== undefined) {
        c.objs.delete(cmd[2][0].$);
      }
      return result;
    }

    // Queued calls from the stream (STREAM above): |n| of them, arguments in
    // arrays kept per count, data in the context's buffer.
    var FLAG_COUNT = 1, FLAG_DELETE = 2, FLAG_COMPILE = 4, FLAG_LINK = 8, FLAG_SIZE = 16;

    function setStream(c, sab) {
      c.s32 = new Int32Array(sab);
      c.s64 = new Float64Array(sab);
      c.s16 = new Uint16Array(sab);
      c.s8 = new Uint8Array(sab);
    }

    function addName(c, name) {
      c.names.push(name);
      c.flags.push((/^(draw|tex|clear|useProgram|bindFramebuffer|viewport)/.test(name) ? FLAG_COUNT : 0) |
                   (/^delete/.test(name) ? FLAG_DELETE : 0) | (name == 'compileShader' ? FLAG_COMPILE : 0) |
                   (name == 'linkProgram' ? FLAG_LINK : 0) | (name == '#size' ? FLAG_SIZE : 0));
    }

    function textAt(c, slot, len) {
      var s = '', base = slot << 2;
      for (var i = 0; i < len; i += 8192) {
        s += String.fromCharCode.apply(null, c.s16.subarray(base + i, base + Math.min(len, i + 8192)));
      }
      return s;
    }

    // The arguments of the call at slot |p| into |av|; the slot after them.
    // c.ref0: the id of a first argument that is an object (delete*).
    function decodeArgs(c, p, argc, av) {
      var s32 = c.s32, s64 = c.s64, data = 0;
      c.ref0 = 0;
      c.numsUsed = 0;
      for (var j = 0; j < argc; j++) {
        var tag = s32[p << 1], v;
        switch (tag) {
          case TAG_NUMBER: v = s64[p + 1]; p += 2; break;
          case TAG_BOOL: v = s64[p + 1] != 0; p += 2; break;
          case TAG_NULL: v = null; p += 2; break;
          case TAG_REF:
            if (j == 0) c.ref0 = s64[p + 1];
            v = c.objs.get(s64[p + 1]);
            p += 2;
            break;
          case TAG_HEAP:  // a view of the wasm memory (a call the thread waits for)
            v = new KINDS[s32[(p << 1) + 1]](wasmMemory.buffer, s64[p + 1], s64[p + 2]);
            p += 3;
            break;
          case TAG_DATA: {
            var kind = s32[(p << 1) + 1], count = s64[p + 1];
            var bytes = count * KINDS[kind].BYTES_PER_ELEMENT;
            if (data++) {  // a second view in one call (none known): its own
              v = new KINDS[kind](count);
              new Uint8Array(v.buffer).set(c.s8.subarray((p + 2) << 3, ((p + 2) << 3) + bytes));
              p += 2 + ((bytes + 7) >> 3);
              break;
            }
            v = scratchView(c, kind, count);
            if (bytes <= SMALL_COPY) {
              var d = c.scratch32, from = (p + 2) << 1;
              for (var w = 0, nw = (bytes + 3) >> 2; w < nw; w++) d[w] = s32[from + w];
            } else {
              c.scratch8.set(c.s8.subarray((p + 2) << 3, ((p + 2) << 3) + bytes));
            }
            c.bytesIn = (c.bytesIn || 0) + bytes;
            p += 2 + ((bytes + 7) >> 3);
            break;
          }
          case TAG_OUT: {  // the call writes into it; the thread copies it back
            var okind = s32[(p << 1) + 1], ocount = s64[p + 1];
            v = new KINDS[okind](c.s8.buffer, (p + 2) << 3, ocount);
            p += 2 + ((ocount * KINDS[okind].BYTES_PER_ELEMENT + 7) >> 3);
            break;
          }
          case TAG_NUMS: {  // in an array kept per length (WebGL keeps none)
            var nn = s64[p + 1], arr = c.nums[nn] || (c.nums[nn] = new Array(nn));
            if (c.numsUsed++) arr = new Array(nn);  // a second one in the call
            for (var q = 0; q < nn; q++) arr[q] = s64[p + 2 + q];
            v = arr;
            p += 2 + nn;
            break;
          }
          case TAG_STRING: case TAG_JSON: {
            var len = s64[p + 1];
            v = textAt(c, p + 2, len);
            if (tag == TAG_JSON) v = JSON.parse(v);
            p += 2 + ((len * 2 + 7) >> 3);
            break;
          }
          default: v = undefined; p += 2;
        }
        av[j] = v;
      }
      return p;
    }

    // Runs |n| queued calls from the stream's start; the slot after them.
    function runStream(c, n, timing) {
      var s32 = c.s32, s64 = c.s64, p = 0;
      for (var k = 0; k < n; k++) {
        var t1 = performance.now();
        var id = s32[p << 1], argc = s32[(p << 1) + 1], tid = s64[p + 1], cid = s64[p + 2];
        var av = c.argvs[argc] || (c.argvs[argc] = new Array(argc));
        p = decodeArgs(c, p + 3, argc, av);
        var ref0 = c.ref0;
        var name = c.names[id], flags = c.flags[id];
        if (flags & FLAG_SIZE) {
          if (c.canvas.width != av[0]) c.canvas.width = av[0];
          if (c.canvas.height != av[1]) c.canvas.height = av[1];
          continue;
        }
        if (flags & FLAG_COUNT) {
          c.calls = c.calls || {};
          c.calls[name] = (c.calls[name] || 0) + 1;
        }
        var target = tid ? c.objs.get(tid) : c.gl, result;
        try {
          result = target[name].apply(target, av);
        } catch (e) {
          if (warned++ < 20) warn('remote WebGL: ' + name + ': ' + e);
          continue;
        }
        if ((flags & FLAG_COMPILE) && !c.gl.getShaderParameter(av[0], c.gl.COMPILE_STATUS) && warned++ < 20) {
          warn('remote WebGL: shader not compiled: ' + c.gl.getShaderInfoLog(av[0]) + ' | ' +
               String(c.gl.getShaderSource(av[0])).slice(0, 300).replace(/\s+/g, ' '));
        } else if ((flags & FLAG_LINK) && !c.gl.getProgramParameter(av[0], c.gl.LINK_STATUS) && warned++ < 20) {
          warn('remote WebGL: program not linked: ' + c.gl.getProgramInfoLog(av[0]));
        }
        if (cid) {  // numbered on the thread (create*)
          c.objs.set(cid, result);
          if (result) c.ids.set(result, cid);
        } else if ((flags & FLAG_DELETE) && ref0) {
          c.objs.delete(ref0);
        }
        var dt = performance.now() - t1;
        if (dt > timing.slowest) { timing.slowest = dt; timing.name = name; }
      }
      return p;
    }

    // The call the thread waits for, after the queued ones at slot |p|, as
    // run() takes it ([target id, name, arguments]; arrays kept).
    function syncCall(c, p) {
      var s32 = c.s32, s64 = c.s64;
      var id = s32[p << 1], argc = s32[(p << 1) + 1];
      var av = c.sargvs[argc] || (c.sargvs[argc] = new Array(argc));
      decodeArgs(c, p + 3, argc, av);
      var cmd = c.syncCmd;
      cmd[0] = s64[p + 1];
      cmd[1] = c.names[id];
      cmd[2] = av;
      return cmd;
    }

    // A call's result to the thread: numbers, booleans, null, WebGL objects
    // and typed arrays without JSON (each a string and objects on both sides).
    function answerResult(c, v, ext) {
      if (v !== null && typeof v == 'object' && !ext) {
        var kind = ArrayBuffer.isView(v) ? KINDS.indexOf(v.constructor) : -1;
        if (kind >= 0 && v.length <= c.resF.length) {
          for (var i = 0; i < v.length; i++) c.resF[i] = v[i];
          Atomics.store(c.ctl, 1, v.length);
          Atomics.store(c.ctl, 3, kind);
          Atomics.store(c.ctl, 2, ANSWER_VIEW);
          Atomics.store(c.ctl, 0, 1);
          Atomics.notify(c.ctl, 0);
          return;
        }
        if (kind < 0 && isObject(v)) {
          c.num[0] = idOf(c, v);
          Atomics.store(c.ctl, 2, ANSWER_REF);
          Atomics.store(c.ctl, 0, 1);
          Atomics.notify(c.ctl, 0);
          return;
        }
      }
      answer(c, v === null || typeof v != 'object' ? v : encode(c, v, ext));
    }

    function answer(c, value) {
      var t = typeof value;
      if (value === undefined || value === null || t == 'number' || t == 'boolean') {
        if (t == 'number') c.num[0] = value; else if (t == 'boolean') c.num[0] = value ? 1 : 0;
        Atomics.store(c.ctl, 2, t == 'number' ? ANSWER_NUMBER : t == 'boolean' ? ANSWER_BOOL : ANSWER_NULL);
        Atomics.store(c.ctl, 0, 1);
        Atomics.notify(c.ctl, 0);
        return;
      }
      Atomics.store(c.ctl, 2, ANSWER_JSON);
      var s = JSON.stringify(value);
      if (s.length > RESULT_CHARS) {
        warn('remote WebGL: result of ' + s.length + ' chars cut');
        s = 'null';
      }
      for (var i = 0; i < s.length; i++) c.res[i] = s.charCodeAt(i);
      Atomics.store(c.ctl, 1, s.length);
      Atomics.store(c.ctl, 0, 1);
      Atomics.notify(c.ctl, 0);
    }

    // Does an upload from a view of shared memory reach the texture? Safari 16
    // takes such a view without an error and uploads nothing (Angry Birds on an
    // iPhone X: every texture empty, every frame black). A 1x1 texture from a
    // SharedArrayBuffer, read back through a framebuffer.
    var sharedUploadsChecked = false;
    function checkSharedUploads(gl) {
      if (sharedUploadsChecked || noSharedViews || typeof SharedArrayBuffer == 'undefined') return;
      sharedUploadsChecked = true;
      var ok = false, tex = gl.createTexture(), fb = gl.createFramebuffer();
      try {
        var src = new Uint8Array(new SharedArrayBuffer(4));
        src.set([200, 100, 50, 255]);
        gl.bindTexture(gl.TEXTURE_2D, tex);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, src);
        gl.bindFramebuffer(gl.FRAMEBUFFER, fb);
        gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, tex, 0);
        var out = new Uint8Array(4);
        gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, out);
        ok = out[0] == 200 && out[1] == 100 && out[2] == 50;
      } catch (e) {
        ok = false;
      }
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.bindTexture(gl.TEXTURE_2D, null);
      gl.deleteFramebuffer(fb);
      gl.deleteTexture(tex);
      while (gl.getError() != gl.NO_ERROR) {}
      if (!ok) {
        noSharedViews = true;
        warn('remote WebGL: uploads from shared memory do not reach textures here: copies');
      }
    }

    Module['arcGlRemote'] = function (m) {
      if (m.op == 'create') {
        var canvas = document.createElement('canvas');
        canvas.width = canvas.height = 1;
        // The drawing buffer is kept between tasks: the thread's calls come in
        // several messages (each a task), and Safari 16 clears an unkept buffer
        // between them -- readPixels at eglSwapBuffers then read black (games
        // on an iPhone X: frames came, all black).
        var gl = canvas.getContext('webgl2', Object.assign({}, m.attrs, { preserveDrawingBuffer: true }));
        var c = contexts[m.ch] = {
          gl: gl, canvas: canvas, objs: new Map(), ids: new WeakMap(), lastMainId: 0,
          ctl: new Int32Array(m.ctl), num: new Float64Array(m.ctl, 16, 1), res: new Uint16Array(m.res),
          resF: new Float64Array(m.res), nums: [], numsUsed: 0,
          names: [], flags: [], argvs: [], sargvs: [], syncCmd: [0, '', null],
          timing: { slowest: 0, name: '' },
        };
        setStream(c, m.stream);
        var consts = {};
        if (gl) checkSharedUploads(gl);
        if (gl) {
          for (var k in WebGL2RenderingContext.prototype) {
            if (/^[A-Z0-9_]+$/.test(k)) consts[k] = WebGL2RenderingContext.prototype[k];
          }
        }
        warn('remote WebGL: context ' + m.ch + (gl ? ' on the page' : ': no WebGL2 on the page either'));
        answer(c, gl ? { O: { consts: consts, methods: methodsOf(gl) } } : null);  // data as it is
        return;
      }
      var c = contexts[m.ch];
      if (!c) return;
      if (m.op == 'note') {  // from the thread (its console does not reach a phone's log)
        if (warned++ < 40) warn('remote WebGL ' + m.ch + ': ' + m.text);
        return;
      }
      if (m.op == 'destroy') {
        delete contexts[m.ch];
        var lose = c.gl && c.gl.getExtension('WEBGL_lose_context');
        if (lose) lose.loseContext();  // its textures and buffers go now, not at GC
        return;
      }
      try {
        if (m.buf) setStream(c, m.buf);
        if (m.names) for (var i = 0; i < m.names.length; i++) addName(c, m.names[i]);
        var t0 = performance.now(), timing = c.timing;
        timing.slowest = 0;
        var end = runStream(c, m.n, timing);
        // The call the thread waits for: in the stream after the others
        // (sync true), or in the message (views of other buffers).
        if (m.sync === true) m.sync = syncCall(c, end);
        c.queued = (c.queued || 0) + m.n;
        c.batches = (c.batches || 0) + 1;
        // A long batch holds the page's main thread (iOS kills a page that
        // stops answering): to the log, with its slowest call.
        var took = performance.now() - t0;
        if (took > 300 && warned++ < 40) {
          warn('remote WebGL: ' + m.n + ' calls took ' + Math.round(took) + ' ms (' +
               timing.name + ': ' + Math.round(timing.slowest) + ' ms)');
        }
      } finally {
        // The thread waits for this answer whatever happens.
        if (m.sync) {
          c.waits = c.waits || {};
          var what = m.sync[1] == 'getParameter' ? 'getParameter(0x' + (+m.sync[2][0]).toString(16) + ')' : m.sync[1];
          c.waits[what] = (c.waits[what] || 0) + 1;
          var value = null;
          try {
            value = run(c, m.sync);
          } finally {
            answerResult(c, value === undefined ? null : value, m.sync[1] == 'getExtension');
          }
        } else if (m.ack) {
          answer(c, null);
        }
      }
    };
    return;
  }

  // ---- A thread: stand-in contexts. ----------------------------------------

  if (typeof SharedArrayBuffer == 'undefined' || typeof Atomics == 'undefined') return;
  var lastChannel = 0;
  var KIND_OF = new Map();
  KINDS.forEach(function (K, i) { KIND_OF.set(K, i); });

  // ---- State the thread answers itself (topic gl-heap). ---------------------
  // Every call that changes a binding or a pixel store parameter goes through
  // the stand-in, so the thread knows what the page's context would answer
  // for them without asking. Asked, each was a wait for the page and a
  // message to it: Subway Surfers (Unity) asked ~60 times a frame -- the
  // buffer bound to a target and its size at each of ~12 glMapBufferRange a
  // frame (the NDK bridge's BufferBinding), six bindings and pack
  // parameters around the frame's readPixels, two glClientWaitSync -- each
  // a MessageEvent with its data on the page (WebKit malloc that the page's
  // GC does not see grew with the frames on an iPhone X). With ?remotegl=check
  // every answer given here is checked against the page's (to its log).
  var MISS = {};  // not known here: ask the page
  var BUFFER_BINDING = { 0x8892: 0x8894, 0x8F36: 0x8F36, 0x8F37: 0x8F37, 0x88EB: 0x88ED,
                         0x88EC: 0x88EF, 0x8A11: 0x8A28 };  // target -> its binding's pname
  var ELEMENT_ARRAY_BUFFER = 0x8893, ELEMENT_ARRAY_BUFFER_BINDING = 0x8895;
  var TEXTURE_BINDING = { 0x0DE1: 0x8069, 0x8513: 0x8514, 0x806F: 0x806A, 0x8C1A: 0x8C1D };
  var FRAMEBUFFER = 0x8D40, DRAW_FRAMEBUFFER = 0x8CA9, READ_FRAMEBUFFER_ = 0x8CA8;
  var DRAW_FRAMEBUFFER_BINDING = 0x8CA6, READ_FRAMEBUFFER_BINDING_ = 0x8CAA;
  var RENDERBUFFER_BINDING = 0x8CA7, VERTEX_ARRAY_BINDING = 0x85B5, CURRENT_PROGRAM = 0x8B8D;
  var ACTIVE_TEXTURE = 0x84E0, TEXTURE0 = 0x84C0, SAMPLER_BINDING = 0x8919;
  var BUFFER_SIZE = 0x8764, BUFFER_USAGE = 0x8765, STATIC_DRAW = 0x88E4;
  // The numeric pixel store parameters and their initial values.
  var PIXEL_STORE = { 0x0D05: 4, 0x0CF5: 4, 0x0D02: 0, 0x0D03: 0, 0x0D04: 0, 0x0CF2: 0,
                      0x0CF3: 0, 0x0CF4: 0, 0x806E: 0, 0x806D: 0 };
  // Syncs: WebGL waits on the client with no timeout anyway, and the page
  // runs every call in order, so what the thread does after a fence sees its
  // work done (data it reads back waits for it on the page).
  var ALREADY_SIGNALED = 0x911A, SYNC_STATUS = 0x9114, SIGNALED = 0x9119;
  var SYNC_ANSWERS = { 0x9112: 0x9116, 0x9113: 0x9117, 0x9114: SIGNALED, 0x9115: 0 };

  function Shadow() {
    this.p = new Map();  // pname -> value (a stand-in object, null or a number)
    var p = this.p;
    for (var t in BUFFER_BINDING) p.set(BUFFER_BINDING[t], null);
    [DRAW_FRAMEBUFFER_BINDING, READ_FRAMEBUFFER_BINDING_, RENDERBUFFER_BINDING, VERTEX_ARRAY_BINDING,
     CURRENT_PROGRAM].forEach(function (n) { p.set(n, null); });
    p.set(ACTIVE_TEXTURE, TEXTURE0);
    for (var k in PIXEL_STORE) p.set(+k, PIXEL_STORE[k]);
    this.element = null;  // the default vertex array's (a bound one keeps its own: __element)
    this.units = [];      // per texture unit: {pname of a target's binding: texture, sampler}
  }

  Shadow.prototype.unit = function (i) {
    return this.units[i] || (this.units[i] = { 0x8069: null, 0x8514: null, 0x806A: null, 0x8C1D: null,
                                                sampler: null });
  };

  Shadow.prototype.bufferAt = function (target) {
    if (target == ELEMENT_ARRAY_BUFFER) {
      var vao = this.p.get(VERTEX_ARRAY_BINDING);
      return vao ? (vao.__element || null) : this.element;
    }
    var n = BUFFER_BINDING[target];
    return n === undefined ? MISS : this.p.get(n);
  };

  Shadow.prototype.get = function (pname) {
    var v = this.p.get(pname);
    if (v !== undefined) return v;
    if (pname == ELEMENT_ARRAY_BUFFER_BINDING) return this.bufferAt(ELEMENT_ARRAY_BUFFER);
    if (pname == SAMPLER_BINDING) return this.unit(this.p.get(ACTIVE_TEXTURE) - TEXTURE0).sampler;
    var u = this.unit(this.p.get(ACTIVE_TEXTURE) - TEXTURE0)[pname];
    return u === undefined ? MISS : u;
  };

  // Each tracked method of the context, after the call: (shadow, arguments).
  var TRACK = {
    bindBuffer: function (s, a) {
      if (a[0] == ELEMENT_ARRAY_BUFFER) {
        var vao = s.p.get(VERTEX_ARRAY_BINDING);
        if (vao) vao.__element = a[1] || null; else s.element = a[1] || null;
      } else if (BUFFER_BINDING[a[0]] !== undefined) {
        s.p.set(BUFFER_BINDING[a[0]], a[1] || null);
      }
    },
    // The indexed bindings also set the generic one (uniform buffers here).
    bindBufferBase: function (s, a) { if (a[0] == 0x8A11) s.p.set(0x8A28, a[2] || null); },
    bindBufferRange: function (s, a) { if (a[0] == 0x8A11) s.p.set(0x8A28, a[2] || null); },
    bufferData: function (s, a) {
      var b = s.bufferAt(a[0]);
      if (!b || b === MISS) return;
      var d = a[1], size;
      if (typeof d == 'number') size = d;
      else if (ArrayBuffer.isView(d)) size = d.BYTES_PER_ELEMENT * (a[4] > 0 ? a[4] : d.length - (a[3] || 0));
      else if (d && typeof d.byteLength == 'number') size = d.byteLength;
      else size = 0;
      b.__size = size;
      b.__usage = a[2];
    },
    bindFramebuffer: function (s, a) {
      if (a[0] == FRAMEBUFFER || a[0] == DRAW_FRAMEBUFFER) s.p.set(DRAW_FRAMEBUFFER_BINDING, a[1] || null);
      if (a[0] == FRAMEBUFFER || a[0] == READ_FRAMEBUFFER_) s.p.set(READ_FRAMEBUFFER_BINDING_, a[1] || null);
    },
    bindRenderbuffer: function (s, a) { s.p.set(RENDERBUFFER_BINDING, a[1] || null); },
    bindVertexArray: function (s, a) { s.p.set(VERTEX_ARRAY_BINDING, a[0] || null); },
    useProgram: function (s, a) { s.p.set(CURRENT_PROGRAM, a[0] || null); },
    activeTexture: function (s, a) { s.p.set(ACTIVE_TEXTURE, a[0]); },
    bindTexture: function (s, a) {
      var n = TEXTURE_BINDING[a[0]];
      if (n !== undefined) s.unit(s.p.get(ACTIVE_TEXTURE) - TEXTURE0)[n] = a[1] || null;
    },
    bindSampler: function (s, a) { s.unit(a[0]).sampler = a[1] || null; },
    pixelStorei: function (s, a) { if (PIXEL_STORE[a[0]] !== undefined) s.p.set(a[0], a[1]); },
    // A deleted object is unbound from the context (and from the bound
    // vertex array's element buffer).
    deleteBuffer: function (s, a) {
      var b = a[0];
      if (!b) return;
      for (var t in BUFFER_BINDING) if (s.p.get(BUFFER_BINDING[t]) === b) s.p.set(BUFFER_BINDING[t], null);
      var vao = s.p.get(VERTEX_ARRAY_BINDING);
      if (vao && vao.__element === b) vao.__element = null;
      if (!vao && s.element === b) s.element = null;
    },
    deleteFramebuffer: function (s, a) {
      if (!a[0]) return;
      if (s.p.get(DRAW_FRAMEBUFFER_BINDING) === a[0]) s.p.set(DRAW_FRAMEBUFFER_BINDING, null);
      if (s.p.get(READ_FRAMEBUFFER_BINDING_) === a[0]) s.p.set(READ_FRAMEBUFFER_BINDING_, null);
    },
    deleteRenderbuffer: function (s, a) {
      if (a[0] && s.p.get(RENDERBUFFER_BINDING) === a[0]) s.p.set(RENDERBUFFER_BINDING, null);
    },
    deleteVertexArray: function (s, a) {
      if (a[0] && s.p.get(VERTEX_ARRAY_BINDING) === a[0]) s.p.set(VERTEX_ARRAY_BINDING, null);
    },
    deleteTexture: function (s, a) {
      if (!a[0]) return;
      for (var i = 0; i < s.units.length; i++) {
        var u = s.units[i];
        if (!u) continue;
        for (var n in u) if (u[n] === a[0]) u[n] = null;
      }
    },
    deleteSampler: function (s, a) {
      if (!a[0]) return;
      for (var i = 0; i < s.units.length; i++) if (s.units[i] && s.units[i].sampler === a[0]) s.units[i].sampler = null;
    },
  };

  // Each method the thread may answer itself: (shadow, arguments) -> the
  // answer, or MISS.
  var LOCAL = {
    getParameter: function (s, a) { return typeof a[0] == 'number' ? s.get(a[0]) : MISS; },
    getBufferParameter: function (s, a) {
      if (a[1] != BUFFER_SIZE && a[1] != BUFFER_USAGE) return MISS;
      var b = s.bufferAt(a[0]);
      if (b === MISS || !b || b.__size === undefined) return MISS;
      return a[1] == BUFFER_SIZE ? b.__size : b.__usage;
    },
    clientWaitSync: function (s, a) { return a[0] ? ALREADY_SIGNALED : MISS; },
    getSyncParameter: function (s, a) {
      return a[0] && SYNC_ANSWERS[a[1]] !== undefined ? SYNC_ANSWERS[a[1]] : MISS;
    },
  };

  function RemoteContext(attrs) {
    var self_ = this;
    this.ch = ++lastChannel + (Math.random() * 1e6 | 0) * 1000;  // unique per thread
    this.ctl = new Int32Array(new SharedArrayBuffer(24));
    this.num = new Float64Array(this.ctl.buffer, 16, 1);
    this.res = new Uint16Array(new SharedArrayBuffer(RESULT_CHARS * 2));
    this.resF = new Float64Array(this.res.buffer);
    this.setStream(new SharedArrayBuffer(STREAM_BYTES));
    this.shadow = new Shadow();
    this.outView = null;  // a view the waited-for call writes into (TAG_OUT), and its slot
    this.outAt = 0;
    this.mismatches = 0;
    this.newBuf = null;  // a grown stream, for the page with the next batch
    this.ncmd = 0;
    this.nameIds = new Map();
    this.newNames = [];
    this.av = new Array(MAX_ARGS);
    this.srcBufs = [null, null];  // buffers copied from, and Int32Arrays of them
    this.src32s = [null, null];
    this.tokens = new Map();
    this.lastId = 0;
    this.width = 1;
    this.height = 1;
    this.stats = { sync: 0, async: 0, waitMs: 0, since: Date.now(), names: {} };
    // ?remotegl=1 (a browser with WebGL in workers, testing): counts to the
    // console; ?remotegl=check: and every answer of the shadow checked
    // against the page's.
    this.log = /[?&]remotegl=(1|check)\b/.test(self.location.search);
    this.verify = /[?&]remotegl=check\b/.test(self.location.search);
    // The message of every batch, kept (filled in by flush).
    this.runMsg = { op: 'run', ch: this.ch, n: 0, sync: null, ack: false, names: null, buf: null };
    this.runPost = { cmd: CMD_CALL_HANDLER, handler: 'arcGlRemote', args: [this.runMsg] };

    this.post({ op: 'create', ch: this.ch, attrs: attrs, ctl: this.ctl.buffer, res: this.res.buffer,
                stream: this.sbuf });
    var info = this.wait();
    if (!info) return;
    this.ok = true;

    var canvas = {
      get width() { return self_.width; },
      set width(w) { self_.width = w; self_.size(); },
      get height() { return self_.height; },
      set height(h) { self_.height = h; self_.size(); },
    };
    var own = {
      canvas: canvas,
      get drawingBufferWidth() { return self_.width; },
      get drawingBufferHeight() { return self_.height; },
      isContextLost: function () { return false; },
      arcRemote: true,  // for shim/gles/library_remote_webgl.js
      // The thread is ending (native_webgl.cpp): the main thread drops the context.
      arcRemoteRelease: function () {
        self_.flush(null);
        self_.post({ op: 'destroy', ch: self_.ch });
      },
      // An RGBA frame of the default framebuffer into |view| at |offset|
      // with the read state set for it on the page (readFrame there).
      arcReadPixels: function (x, y, w, h, view, offset) {
        var av = self_.av;
        av[0] = x; av[1] = y; av[2] = w; av[3] = h; av[4] = view; av[5] = offset;
        self_.call(0, 'arcReadPixels', self_.nameId('arcReadPixels'), CALL_SYNC, 6);
        av[0] = av[1] = av[2] = av[3] = av[4] = av[5] = undefined;
      },
    };
    this.gl = this.facade(0, own, info.consts, info.methods);
  }

  RemoteContext.prototype.setStream = function (sab) {
    this.sbuf = sab;
    this.s32 = new Int32Array(sab);
    this.s64 = new Float64Array(sab);
    this.s8 = new Uint8Array(sab);
    this.s16 = new Uint16Array(sab);
    this.cap = sab.byteLength >> 3;  // slots
    this.pos = 0;
  };

  RemoteContext.prototype.post = function (m) {
    self.postMessage({ cmd: CMD_CALL_HANDLER, handler: 'arcGlRemote', args: [m] });
  };

  RemoteContext.prototype.wait = function () {
    var t0 = performance.now();
    // Until the flag is up: a wait can end without it. The page raises the
    // flag, then notifies; in between the thread may take that answer (by
    // the flag), post the next call and wait again, and the late notify
    // ends this wait. Taken as this call's answer, the previous one made
    // every answer after it one call late, and the next batch went into the
    // stream while the page still ran this one (calls of no name on no
    // object, then the GL thread stopped: Subway Surfers in Chrome with
    // ?remotegl=1; "4ull" in a JSON answer, Angry Birds 2.1.0 on an iPhone
    // X, topic ios-gl-json).
    while (Atomics.load(this.ctl, 0) == 0) Atomics.wait(this.ctl, 0, 0);
    this.stats.waitMs += performance.now() - t0;
    var n = this.ctl[1], kind = this.ctl[2];
    Atomics.store(this.ctl, 0, 0);
    switch (kind) {
      case ANSWER_NUMBER: return this.num[0];
      case ANSWER_NULL: return null;
      case ANSWER_BOOL: return this.num[0] != 0;
      case ANSWER_REF: return this.token(this.num[0], null);
      case ANSWER_VIEW: {
        var out = new KINDS[this.ctl[3]](n);
        for (var j = 0; j < n; j++) out[j] = this.resF[j];
        return out;
      }
    }
    var s = '';
    for (var i = 0; i < n; i += 8192) {
      s += String.fromCharCode.apply(null, this.res.subarray(i, Math.min(n, i + 8192)));
    }
    return this.decode(JSON.parse(s));
  };

  RemoteContext.prototype.decode = function (v) {
    if (v === null || typeof v != 'object') return v;
    if (Array.isArray(v)) return v.map(this.decode, this);
    if (v.T) return new globalThis[v.T](v.v);
    if (v.O) return v.O;
    return this.token(v.$, v.x);
  };

  // The stand-in of the page's object |id| (an extension's: with |methods|).
  RemoteContext.prototype.token = function (id, methods) {
    var t = this.tokens.get(id);
    if (!t) {
      t = methods ? this.facade(id, {}, {}, methods) : { __rid: id };
      this.tokens.set(id, t);
    }
    return t;
  };

  RemoteContext.prototype.nameId = function (name) {
    var id = this.nameIds.get(name);
    if (id === undefined) {
      id = this.nameIds.size;
      this.nameIds.set(name, id);
      this.newNames.push(name);
    }
    return id;
  };

  // The object whose |names| methods go to |target| on the main thread (0:
  // the context, else an extension); |own| holds what is answered here and
  // what Emscripten keeps on the object (currentPixelUnpackBufferBinding...).
  // A method puts its arguments in the context's kept array (an arguments
  // object or an array per call was garbage for the thread's GC: thousands
  // of calls a second). A plain object, not a Proxy: JavaScriptCore made a
  // string of the property's name for every read through a get trap, one
  // per GL call.
  // The context's own methods also keep the shadow (TRACK) or are answered
  // from it (LOCAL).
  RemoteContext.prototype.facade = function (target, own, consts, names) {
    var rc = this;
    var f = {};
    for (var k in consts) f[k] = consts[k];
    names.forEach(function (p) {
      var kind = CREATES.test(p) ? CALL_CREATE : !NO_RESULT.test(p) || p == 'readPixels' ? CALL_SYNC :
                 /^delete/.test(p) ? CALL_DELETE : CALL_QUEUED;
      var id = -1;
      var track = !target && TRACK.hasOwnProperty(p) ? TRACK[p] : null;
      var local = !target && LOCAL.hasOwnProperty(p) ? LOCAL[p] : null;
      f[p] = function (a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11) {
        var av = rc.av;
        av[0] = a0; av[1] = a1; av[2] = a2; av[3] = a3; av[4] = a4; av[5] = a5;
        av[6] = a6; av[7] = a7; av[8] = a8; av[9] = a9; av[10] = a10; av[11] = a11;
        if (id < 0) id = rc.nameId(p);
        var n = Math.min(arguments.length, MAX_ARGS), r;
        if (local === null || (r = local(rc.shadow, av)) === MISS) {
          r = rc.call(target, p, id, kind, n);
        } else if (rc.verify && kind == CALL_SYNC) {
          rc.check(p, id, n, r);
        }
        if (track !== null) track(rc.shadow, av);
        av[0] = av[1] = av[2] = av[3] = av[4] = av[5] = av[6] = av[7] = av[8] = av[9] = av[10] = av[11] = undefined;
        return r;
      };
    });
    Object.getOwnPropertyNames(own).forEach(function (k) {
      Object.defineProperty(f, k, Object.getOwnPropertyDescriptor(own, k));
    });
    if (target) f.__rid = target;
    return f;
  };

  // ?remotegl=check: the page's answer to a call the shadow answered (|mine|);
  // a difference to the page's log. Syncs differ by design.
  RemoteContext.prototype.check = function (name, id, n, mine) {
    if (name == 'clientWaitSync' || name == 'getSyncParameter') return;
    var what = name + '(0x' + (+this.av[0]).toString(16) + (n > 1 ? ', 0x' + (+this.av[1]).toString(16) : '') + ')';
    var theirs = this.call(0, name, id, CALL_SYNC, n);
    if (theirs !== mine && this.mismatches++ < 20) {
      var show = function (v) { return v && v.__rid !== undefined ? 'object ' + v.__rid : String(v); };
      this.post({ op: 'note', ch: this.ch, text: 'shadow: ' + what + ' is ' + show(mine) + ' here, ' +
                  show(theirs) + ' on the page' });
    }
  };

  // Arguments of a call the thread waits for: views go as they are (the
  // main thread reads and writes the shared memory itself).
  RemoteContext.prototype.args = function (n) {
    var out = new Array(n);
    for (var i = 0; i < n; i++) {
      var v = this.av[i];
      out[i] = v !== null && typeof v == 'object' && v.__rid !== undefined ? { $: v.__rid } : v;
    }
    return out;
  };

  // The canvas' size: a pseudo-call in the stream.
  RemoteContext.prototype.size = function () {
    var av = this.av;
    av[0] = this.width;
    av[1] = this.height;
    this.queue(0, this.nameId('#size'), '#size', 2, 0);
    av[0] = av[1] = undefined;
  };

  // Copies |bytes| bytes of |buf| from byte |from| into the stream at slot |at|.
  RemoteContext.prototype.copyIn = function (buf, from, bytes, at) {
    if (bytes <= SMALL_COPY && !(from & 3)) {
      // Through an Int32Array of the buffer, kept (the heap's, and a
      // temporary of Emscripten's): a view per call was garbage too.
      var b = this.srcBufs, j = b[0] === buf ? 0 : b[1] === buf ? 1 : -1;
      if (j < 0) {
        j = buf instanceof SharedArrayBuffer ? 0 : 1;
        b[j] = buf;
        this.src32s[j] = new Int32Array(buf, 0, buf.byteLength >> 2);
      }
      var src = this.src32s[j], dst = this.s32, d = at << 1, s = from >> 2;
      for (var k = 0, w = (bytes + 3) >> 2; k < w; k++) dst[d + k] = src[s + k];
      return;
    }
    this.s8.set(new Uint8Array(buf, from, bytes), at << 3);
  };

  // A string (UTF-16 units) into the stream at slot |at|; slots taken.
  RemoteContext.prototype.stringIn = function (s, at) {
    var u = this.s16, base = at << 2;
    for (var k = 0; k < s.length; k++) u[base + k] = s.charCodeAt(k);
    return (s.length * 2 + 7) >> 3;
  };

  // A queued call into the stream (layout: STREAM above). Data views among
  // the arguments are copied in: the part (data, offset[, length]) names
  // (the whole heap needs RANGES), and those arguments are dropped. false:
  // the stream is full (flush and call again), or, with |this.cannot| set,
  // a call that cannot be queued (it runs at once).
  RemoteContext.prototype.enqueue = function (target, id, name, n, createId, sync) {
    var av = this.av, s32 = this.s32, s64 = this.s64, cap = this.cap;
    var start = this.pos, p = start + 3, argc = n;
    this.cannot = false;
    if (p + 2 * n > cap) return false;
    for (var i = 0; i < argc; i++) {
      var v = av[i], t = typeof v;
      if (p + 2 > cap) return false;
      if (t == 'number') {
        s32[p << 1] = TAG_NUMBER;
        s64[p + 1] = v;
        p += 2;
        continue;
      }
      if (t == 'boolean') {
        s32[p << 1] = TAG_BOOL;
        s64[p + 1] = v ? 1 : 0;
        p += 2;
        continue;
      }
      if (v === undefined || v === null) {
        s32[p << 1] = v === null ? TAG_NULL : TAG_UNDEFINED;
        p += 2;
        continue;
      }
      if (t == 'object' && v.__rid !== undefined) {
        s32[p << 1] = TAG_REF;
        s64[p + 1] = v.__rid;
        p += 2;
        continue;
      }
      if (t == 'object' && ArrayBuffer.isView(v)) {
        var kind = KIND_OF.get(v.constructor);
        if (kind === undefined) { this.cannot = true; return false; }
        var E = v.BYTES_PER_ELEMENT, off = 0, count = v.length;
        var range = RANGES[name], form = OFFSET_FORMS[name];
        if (sync) {
          // The page takes the arguments as they are (run(): withoutOffset).
          if (isShared(v)) {
            if (v.buffer !== wasmMemory.buffer) { this.cannot = true; return false; }
            if (p + 3 > cap) return false;
            s32[p << 1] = TAG_HEAP;
            s32[(p << 1) + 1] = kind;
            s64[p + 1] = v.byteOffset;
            s64[p + 2] = v.length;
            p += 3;
            continue;
          }
          range = form = null;
          if ((name == 'getBufferSubData' && i == 2) || (name == 'readPixels' && i == 6)) {
            // It writes into the view: through the stream and back (call()).
            if (p + 2 + ((v.byteLength + 7) >> 3) > cap) return false;
            s32[p << 1] = TAG_OUT;
            s32[(p << 1) + 1] = kind;
            s64[p + 1] = v.length;
            this.copyIn(v.buffer, v.byteOffset, v.byteLength, p + 2);
            this.outView = v;
            this.outAt = p + 2;
            p += 2 + ((v.byteLength + 7) >> 3);
            continue;
          }
        } else if (isShared(v) && v.byteLength == v.buffer.byteLength) {
          // The whole heap: the range is in the next arguments.
          if (!range || range[0] != i) { this.cannot = true; return false; }
          off = av[range[1]] || 0;
          count = range[2] < 0 ? -range[2] : av[range[2]];
          if (count === undefined) { this.cannot = true; return false; }
          argc = range[1];
        } else if (form && form[0] == i && typeof av[form[1]] == 'number') {
          // A view of a part, or a reused temporary, with an offset.
          off = av[form[1]];
          count = form[2] >= 0 && typeof av[form[2]] == 'number' && av[form[2]] > 0 ? av[form[2]] : v.length - off;
          argc = form[1];
        }
        count = Math.max(0, Math.min(count, v.length - off));
        var bytes = count * E;
        if (p + 2 + ((bytes + 7) >> 3) > cap) return false;
        s32[p << 1] = TAG_DATA;
        s32[(p << 1) + 1] = kind;
        s64[p + 1] = count;
        this.copyIn(v.buffer, v.byteOffset + off * E, bytes, p + 2);
        p += 2 + ((bytes + 7) >> 3);
        continue;
      }
      if (Array.isArray(v) && v.length <= 64 && this.numbers(v)) {
        if (p + 2 + v.length > cap) return false;
        s32[p << 1] = TAG_NUMS;
        s64[p + 1] = v.length;
        for (var q = 0; q < v.length; q++) s64[p + 2 + q] = v[q];
        p += 2 + v.length;
        continue;
      }
      // Other arrays, strings, anything else: as text, JSON but for strings.
      var text = t == 'string' ? v : JSON.stringify(v);
      if (p + 2 + ((text.length * 2 + 7) >> 3) > cap) return false;
      s32[p << 1] = t == 'string' ? TAG_STRING : TAG_JSON;
      s64[p + 1] = text.length;
      p += 2 + this.stringIn(text, p + 2);
    }
    s32[start << 1] = id;
    s32[(start << 1) + 1] = argc;
    s64[start + 1] = target;
    s64[start + 2] = createId;
    this.pos = p;
    this.ncmd++;
    return true;
  };

  RemoteContext.prototype.numbers = function (a) {
    for (var i = 0; i < a.length; i++) if (typeof a[i] != 'number') return false;
    return true;
  };

  // Queues the call (|sync|: the one the thread waits for, last); false if
  // it cannot be queued.
  RemoteContext.prototype.queue = function (target, id, name, n, createId, sync) {
    if (this.enqueue(target, id, name, n, createId, sync)) return true;
    if (this.cannot) return false;
    this.flush(null);
    if (this.enqueue(target, id, name, n, createId, sync)) return true;
    if (this.cannot) return false;
    // Larger than the whole stream: a larger one, for the page with the
    // next batch.
    this.setStream(new SharedArrayBuffer(this.sbuf.byteLength * 2));
    this.newBuf = this.sbuf;
    return this.enqueue(target, id, name, n, createId, sync) || this.queue(target, id, name, n, createId, sync);
  };

  RemoteContext.prototype.call = function (target, name, id, kind, n) {
    if (kind == CALL_CREATE) {
      var cid = ++this.lastId;
      var token = { __rid: cid };
      this.tokens.set(cid, token);
      if (this.queue(target, id, name, n, cid)) {
        this.stats.async++;
        return token;
      }
      // Cannot be queued (never seen): numbered on the main thread instead.
      this.tokens.delete(cid);
    } else if (kind != CALL_SYNC && this.queue(target, id, name, n, 0)) {
      if (kind == CALL_DELETE && this.av[0] && this.av[0].__rid) this.tokens.delete(this.av[0].__rid);
      this.stats.async++;
      if (this.ncmd >= QUEUE_MAX) this.flush(null);
      return undefined;
    }
    this.stats.sync++;
    if (this.log) this.stats.names[name] = (this.stats.names[name] || 0) + 1;
    this.outView = null;
    if (this.queue(target, id, name, n, 0, true)) {
      this.ncmd--;  // not one of the queued calls (sync: true)
      this.flush(true);
    } else {
      this.outView = null;
      this.flush([target, name, this.args(n)]);  // in the message
    }
    var result = this.wait();
    if (this.outView) {  // what the page wrote into the view (TAG_OUT)
      var o = this.outView;
      this.outView = null;
      new Uint8Array(o.buffer, o.byteOffset, o.byteLength).set(new Uint8Array(this.sbuf, this.outAt << 3, o.byteLength));
    }
    if (this.log && Date.now() - this.stats.since > 5000) {
      var s = this.stats;
      console.log('remote WebGL ' + this.ch + ': ' + s.sync + ' waits (' + s.waitMs.toFixed(0) +
                  ' ms), ' + s.async + ' queued calls in ' + (Date.now() - s.since) + ' ms; waits: ' +
                  JSON.stringify(s.names));
      this.stats = { sync: 0, async: 0, waitMs: 0, since: Date.now(), names: {} };
    }
    return result;
  };

  // The queued calls (and |sync|, a call whose result the thread waits for:
  // true when it is last in the stream, else [target, name, arguments]) to
  // the main thread. Without |sync| the main thread still answers once it
  // has run them: the stream is written again from its start.
  RemoteContext.prototype.flush = function (sync) {
    if (!this.ncmd && !sync) return;
    var m = this.runMsg;
    m.n = this.ncmd;
    m.sync = sync;
    m.ack = !sync;
    m.names = this.newNames.length ? this.newNames : null;
    m.buf = this.newBuf;
    self.postMessage(this.runPost);
    m.sync = m.names = m.buf = null;
    if (this.newNames.length) this.newNames = [];
    this.newBuf = null;
    this.pos = 0;
    this.ncmd = 0;
    if (!sync) this.wait();
  };

  // A WebGL2 context for the calling thread, or null.
  globalThis.arcRemoteWebGL = {
    forced: function () { return /[?&]remotegl=(1|noshared|check)\b/.test(self.location.search); },
    create: function (attrs) {
      var rc = new RemoteContext(attrs);
      return rc.ok ? rc.gl : null;
    },
  };
})();
