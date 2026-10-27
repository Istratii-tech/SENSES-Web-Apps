// SENSES Web Apps: what an APK is, read in the browser (topic senses-shell,
// docs/topics/senses-shell.md).
//
//   SensesApk.read(blob, {locales, splits}) -> {package, label, versionName, versionCode,
//       minSdk, targetSdk, abis, launcher, orientation, icon: {kind, ...} | null,
//       warnings, size} (orientation: 'landscape' | 'portrait' | null, the
//       launcher activity's android:screenOrientation; topic install8)
//       (splits: Blobs of the app's parts whose resources count too, e.g. the
//       icon in a density part, the label in a language part)
//   SensesApk.readSplit(blob) -> {package, versionCode, split, configForSplit,
//       isFeatureSplit, hasCode, abis, size}: a part's manifest
//   SensesApk.readBundle(blob) -> {kind: 'apk'} for an APK, else the parts and
//       OBB files of an .apks/.xapk/.apkm (topic apks-split, docs/topics/apks-split.md)
//   SensesApk.choose(bundle, {abis, dpi, locales}) -> the parts for this device
//   SensesApk.streamEntry(blob, entry, sink) -> an entry's bytes in pieces to
//       sink (may return a promise), its CRC checked: parts are never whole in memory
//
// - ZIP: the central directory and only the entries needed (the manifest,
//   resources.arsc, the icon) are read, by Blob.slice: a 130 MB APK is never
//   in memory as a whole (iPhones). Deflate through DecompressionStream
//   ('deflate-raw'), or the small inflate below where a browser has none
//   (Safari before 16.4).
// - AndroidManifest.xml: Android's binary XML (ResXMLTree): package,
//   versions, uses-sdk, the application's label and icon, the launcher
//   activity (MAIN/LAUNCHER, also through an activity-alias) and the
//   orientation it asks for.
// - resources.arsc (ResTable): the label in the user's language, the icon's
//   file at the best density; an adaptive icon (mipmap-anydpi-v26 XML) is
//   given as its layers (images, colours or vector drawables, turned into
//   SVG), drawn by the page (icon.js).
// - ABIs: the lib/<abi>/ directories.
// Plain functions, no DOM: also runs in workers and in Node (tests).
(function (root) {
  'use strict';

  function ApkError(code, message) {
    var e = new Error(message);
    e.code = code;
    return e;
  }

  // --- bytes ------------------------------------------------------------------
  function u16(b, o) { return b[o] | (b[o + 1] << 8); }
  function u32(b, o) { return (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16)) + b[o + 3] * 16777216; }
  function i32(b, o) { return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24); }
  function readSlice(blob, start, end) {
    var part = blob.slice(start, end);
    if (part.arrayBuffer) return part.arrayBuffer().then(function (a) { return new Uint8Array(a); });
    return new Promise(function (resolve, reject) {  // Safari before 14
      var r = new FileReader();
      r.onload = function () { resolve(new Uint8Array(r.result)); };
      r.onerror = function () { reject(r.error); };
      r.readAsArrayBuffer(part);
    });
  }
  var utf8 = typeof TextDecoder !== 'undefined' ? new TextDecoder('utf-8') : null;
  function decodeUtf8(b) {
    if (utf8) return utf8.decode(b);
    var s = '';
    for (var i = 0; i < b.length; i++) s += String.fromCharCode(b[i]);
    try { return decodeURIComponent(escape(s)); } catch (e) { return s; }
  }

  // --- inflate (RFC 1951), for browsers without DecompressionStream ---------------------
  // A plain canonical-Huffman decoder (after tinf), streaming: a generator
  // over io = {src, pos, end} that yields NEED when it is short of input
  // (the driver gives it more in io.src/io.pos; io.end: no more to come) and
  // the output in pieces of about OUT_PIECE bytes, keeping only the last
  // 32 KB (the window) besides: an app's part of 100 MB is never whole in
  // memory (iPhones with Safari before 16.4, iOS 16.1).
  var LEN_BASE = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99,
                  115, 131, 163, 195, 227, 258];
  var LEN_EXTRA = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0];
  var DIST_BASE = [1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025,
                   1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577];
  var DIST_EXTRA = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12,
                    12, 13, 13];
  var CL_ORDER = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15];
  var NEED = {need: true}, WINDOW = 32768, OUT_PIECE = 1 << 20;
  var LOW_INPUT = 4096;  // more than a block header or a symbol can take
  function Tree(lengths, n) {
    this.counts = new Uint16Array(16);
    this.symbols = new Uint16Array(n);
    for (var i = 0; i < n; i++) this.counts[lengths[i]]++;
    this.counts[0] = 0;
    var offs = new Uint16Array(16);
    for (var s = 0, l = 0; l < 16; l++) { offs[l] = s; s += this.counts[l]; }
    for (i = 0; i < n; i++) if (lengths[i]) this.symbols[offs[lengths[i]]++] = i;
  }
  var fixedTrees = null;
  function fixed() {
    if (!fixedTrees) {
      var l = new Uint8Array(288);
      for (var i = 0; i < 288; i++) l[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
      var d = new Uint8Array(30);
      d.fill(5);
      fixedTrees = [new Tree(l, 288), new Tree(d, 30)];
    }
    return fixedTrees;
  }
  function* inflateSteps(io) {
    var src = io.src, pos = io.pos, bitBuf = 0, bitCnt = 0;
    // out[0, kept) was given already (the window), out[kept, op) not yet.
    var out = new Uint8Array(WINDOW + OUT_PIECE + 512), op = 0, kept = 0;
    function need(n) {
      while (bitCnt < n) {
        if (pos >= src.length) throw ApkError('corrupt', 'inflate: unexpected end');
        bitBuf |= src[pos++] << bitCnt;
        bitCnt += 8;
      }
    }
    function bits(n) {
      if (!n) return 0;
      need(n);
      var v = bitBuf & ((1 << n) - 1);
      bitBuf >>>= n;
      bitCnt -= n;
      return v;
    }
    function decode(t) {
      var code = 0, first = 0, index = 0;
      for (var len = 1; len < 16; len++) {
        code |= bits(1);
        var count = t.counts[len];
        if (code - first < count) return t.symbols[index + code - first];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
      }
      throw ApkError('corrupt', 'inflate: bad code');
    }
    function dynamic() {
      var hlit = bits(5) + 257, hdist = bits(5) + 1, hclen = bits(4) + 4;
      var cl = new Uint8Array(19);
      for (var i = 0; i < hclen; i++) cl[CL_ORDER[i]] = bits(3);
      var clTree = new Tree(cl, 19);
      var lengths = new Uint8Array(hlit + hdist);
      for (var n = 0; n < hlit + hdist;) {
        var sym = decode(clTree);
        if (sym < 16) { lengths[n++] = sym; continue; }
        var rep, val = 0;
        if (sym === 16) { val = lengths[n - 1]; rep = 3 + bits(2); }
        else if (sym === 17) rep = 3 + bits(3);
        else rep = 11 + bits(7);
        if (n + rep > hlit + hdist) throw ApkError('corrupt', 'inflate: bad lengths');
        while (rep--) lengths[n++] = val;
      }
      return [new Tree(lengths.subarray(0, hlit), hlit), new Tree(lengths.subarray(hlit), hdist)];
    }
    // The output not given yet; the window moves to the front.
    function piece() {
      var p = out.slice(kept, op);
      if (op > WINDOW) {
        out.copyWithin(0, op - WINDOW, op);
        op = WINDOW;
      }
      kept = op;
      return p;
    }
    for (var last = 0; !last;) {
      if (!io.end && src.length - pos < LOW_INPUT) {
        io.pos = pos; yield NEED; src = io.src; pos = io.pos;
      }
      last = bits(1);
      var type = bits(2);
      if (type === 0) {
        bitBuf = 0; bitCnt = 0;
        if (pos + 4 > src.length) throw ApkError('corrupt', 'inflate: unexpected end');
        var len = u16(src, pos), nlen = u16(src, pos + 2);
        if ((len ^ 0xffff) !== nlen) throw ApkError('corrupt', 'inflate: bad stored block');
        pos += 4;
        while (len > 0) {
          if (op >= WINDOW + OUT_PIECE) yield piece();
          if (pos >= src.length) {
            if (io.end) throw ApkError('corrupt', 'inflate: unexpected end');
            io.pos = pos; yield NEED; src = io.src; pos = io.pos;
            continue;
          }
          var n = Math.min(len, src.length - pos, out.length - op);
          out.set(src.subarray(pos, pos + n), op);
          op += n; pos += n; len -= n;
        }
        continue;
      }
      if (type === 3) throw ApkError('corrupt', 'inflate: bad block');
      var trees = type === 1 ? fixed() : dynamic();
      for (;;) {
        if (op >= WINDOW + OUT_PIECE) yield piece();
        if (!io.end && src.length - pos < LOW_INPUT) {
          io.pos = pos; yield NEED; src = io.src; pos = io.pos;
        }
        var s = decode(trees[0]);
        if (s < 256) { out[op++] = s; continue; }
        if (s === 256) break;
        s -= 257;
        if (s >= 29) throw ApkError('corrupt', 'inflate: bad length');
        var length = LEN_BASE[s] + bits(LEN_EXTRA[s]);
        var ds = decode(trees[1]);
        if (ds >= 30) throw ApkError('corrupt', 'inflate: bad distance');
        var dist = DIST_BASE[ds] + bits(DIST_EXTRA[ds]);
        if (dist > op) throw ApkError('corrupt', 'inflate: bad distance');
        for (var k = 0; k < length; k++, op++) out[op] = out[op - dist];
      }
    }
    io.pos = pos;
    if (op > kept) yield piece();
  }
  // Inflates all of |src| at once (a manifest, resources.arsc, an icon).
  function inflateJs(src, size) {
    var io = {src: src, pos: 0, end: true}, pieces = [], total = 0;
    for (var it = inflateSteps(io), r = it.next(); !r.done; r = it.next()) {
      pieces.push(r.value);
      total += r.value.length;
    }
    if (pieces.length === 1) return pieces[0];
    var out = new Uint8Array(total);
    for (var i = 0, at = 0; i < pieces.length; at += pieces[i].length, i++) out.set(pieces[i], at);
    return out;
  }
  var nativeInflate = (function () {
    if (typeof DecompressionStream === 'undefined' || typeof Response === 'undefined') return false;
    try { new DecompressionStream('deflate-raw'); return true; } catch (e) { return false; }
  })();
  function inflate(data, size) {
    if (!nativeInflate) return Promise.resolve(inflateJs(data, size));
    var stream = new Blob([data]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
    return new Response(stream).arrayBuffer().then(function (a) { return new Uint8Array(a); },
        function () { return inflateJs(data, size); });
  }

  // --- ZIP ---------------------------------------------------------------------------------
  // The central directory: name -> {method, csize, size, offset}.
  function readZip(blob) {
    var size = blob.size;
    if (size < 22) return Promise.reject(ApkError('notzip', 'too small for a ZIP'));
    var tail = Math.min(size, 65557);
    return readSlice(blob, size - tail, size).then(function (b) {
      var eocd = -1;
      for (var i = b.length - 22; i >= 0; i--) {
        if (b[i] === 0x50 && b[i + 1] === 0x4b && b[i + 2] === 5 && b[i + 3] === 6) { eocd = i; break; }
      }
      if (eocd < 0) throw ApkError('notzip', 'no ZIP end record');
      var count = u16(b, eocd + 10), cdSize = u32(b, eocd + 12), cdOff = u32(b, eocd + 16);
      if (cdOff === 0xffffffff || count === 0xffff) {
        // ZIP64: its locator just before the end record.
        var loc = eocd - 20;
        if (loc < 0 || u32(b, loc) !== 0x07064b50) throw ApkError('notzip', 'bad ZIP64');
        var z64 = u32(b, loc + 8) + u32(b, loc + 12) * 4294967296;
        return readSlice(blob, z64, z64 + 56).then(function (z) {
          if (u32(z, 0) !== 0x06064b50) throw ApkError('notzip', 'bad ZIP64 record');
          var n = u32(z, 32), cs = u32(z, 40), co = u32(z, 48) + u32(z, 52) * 4294967296;
          return central(blob, co, cs, n);
        });
      }
      return central(blob, cdOff, cdSize, count);
    });
  }
  function central(blob, off, len, count) {
    if (off + len > blob.size) return Promise.reject(ApkError('corrupt', 'ZIP directory past the end (file cut short?)'));
    return readSlice(blob, off, off + len).then(function (b) {
      var entries = Object.create(null), p = 0;
      for (var n = 0; n < count && p + 46 <= b.length; n++) {
        if (u32(b, p) !== 0x02014b50) throw ApkError('corrupt', 'bad ZIP directory');
        var e = {method: u16(b, p + 10), csize: u32(b, p + 20), size: u32(b, p + 24),
                 offset: u32(b, p + 42), crc: u32(b, p + 16), flags: u16(b, p + 8)};
        var nameLen = u16(b, p + 28), extraLen = u16(b, p + 30), commentLen = u16(b, p + 32);
        var name = decodeUtf8(b.subarray(p + 46, p + 46 + nameLen));
        // ZIP64 sizes/offset in the extra field.
        for (var x = p + 46 + nameLen, xe = x + extraLen; x + 4 <= xe;) {
          var id = u16(b, x), sz = u16(b, x + 2), q = x + 4;
          if (id === 1) {
            if (e.size === 0xffffffff) { e.size = u32(b, q) + u32(b, q + 4) * 4294967296; q += 8; }
            if (e.csize === 0xffffffff) { e.csize = u32(b, q) + u32(b, q + 4) * 4294967296; q += 8; }
            if (e.offset === 0xffffffff) e.offset = u32(b, q) + u32(b, q + 4) * 4294967296;
          }
          x += 4 + sz;
        }
        entries[name] = e;
        p += 46 + nameLen + extraLen + commentLen;
      }
      return entries;
    });
  }
  // Where an entry's data starts (after its local header).
  function dataStart(blob, e) {
    return readSlice(blob, e.offset, e.offset + 30).then(function (h) {
      if (u32(h, 0) !== 0x04034b50) throw ApkError('corrupt', 'bad ZIP entry');
      if (e.flags & 1) throw ApkError('encrypted', 'Файл в архиве зашифрован — такой архив не открыть.');
      return e.offset + 30 + u16(h, 26) + u16(h, 28);
    });
  }
  // An entry's bytes (uncompressed). An entry may say which Blob it is in
  // (e.blob: a part of the app, read() with splits).
  function readEntry(blob, e) {
    blob = e.blob || blob;
    return dataStart(blob, e).then(function (start) {
      return readSlice(blob, start, start + e.csize);
    }).then(function (data) {
      if (e.method === 0) return data;
      if (e.method === 8) return inflate(data, e.size);
      throw ApkError('corrupt', 'ZIP method ' + e.method);
    });
  }

  // --- an entry in pieces (an app's part out of its .apks/.xapk) -----------------------
  var CRC_TABLE = null;
  // CRC-32 (ZIP's) of |b|, continuing |crc| (0 to start).
  function crc32(b, crc) {
    if (!CRC_TABLE) {
      CRC_TABLE = new Int32Array(256);
      for (var n = 0; n < 256; n++) {
        var c = n;
        for (var k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
        CRC_TABLE[n] = c;
      }
    }
    var x = ~crc;
    for (var i = 0; i < b.length; i++) x = CRC_TABLE[(x ^ b[i]) & 255] ^ (x >>> 8);
    return ~x >>> 0;
  }
  // Pieces of about OUT_PIECE bytes from DecompressionStream's smaller ones.
  function Gather(sink) {
    var parts = [], size = 0;
    this.add = function (b) {
      parts.push(b);
      size += b.length;
      return size >= OUT_PIECE ? this.flush() : null;
    };
    this.flush = function () {
      if (!size) return null;
      var all = parts.length === 1 ? parts[0] : new Uint8Array(size);
      if (parts.length > 1) for (var i = 0, at = 0; i < parts.length; at += parts[i].length, i++) all.set(parts[i], at);
      parts = []; size = 0;
      return sink(all);
    };
  }
  // The uncompressed bytes of entry |e| of |blob| (or of e.blob) to
  // sink(Uint8Array) in pieces of up to 1 MB, one at a time (sink may return
  // a promise: written before the next); the size and CRC are checked.
  function streamEntry(blob, e, sink) {
    blob = e.blob || blob;
    var crc = 0, total = 0;
    function put(b) {
      crc = crc32(b, crc);
      total += b.length;
      return Promise.resolve(sink(b));
    }
    return dataStart(blob, e).then(function (start) {
      var end = start + e.csize;
      if (end > blob.size) throw ApkError('corrupt', 'ZIP entry past the end (file cut short?)');
      if (e.method === 0) {
        var at = start;
        var stored = function () {
          if (at >= end) return null;
          var to = Math.min(end, at + OUT_PIECE);
          return readSlice(blob, at, to).then(function (b) { at = to; return put(b); }).then(stored);
        };
        return stored();
      }
      if (e.method !== 8) throw ApkError('corrupt', 'ZIP method ' + e.method);
      if (nativeInflate && blob.stream) {
        var reader = blob.slice(start, end).stream().pipeThrough(new DecompressionStream('deflate-raw')).getReader();
        var g = new Gather(put);
        var next = function () {
          return reader.read().then(function (r) {
            if (r.done) return g.flush();
            return Promise.resolve(g.add(new Uint8Array(r.value.buffer, r.value.byteOffset, r.value.byteLength))).then(next);
          });
        };
        return next();
      }
      var io = {src: new Uint8Array(0), pos: 0, end: false}, it = inflateSteps(io), read = start;
      var step = function () {
        for (;;) {
          var r = it.next();
          if (r.done) return null;
          if (r.value !== NEED) return put(r.value).then(step);
          var to = Math.min(end, read + OUT_PIECE);
          return readSlice(blob, read, to).then(function (b) {
            var rest = io.src.length - io.pos;
            var src = new Uint8Array(rest + b.length);
            src.set(io.src.subarray(io.pos));
            src.set(b, rest);
            io.src = src; io.pos = 0;
            read = to;
            io.end = read >= end;
            return step();
          });
        }
      };
      return step();
    }).then(function () {
      if (total !== e.size || crc !== e.crc)
        throw ApkError('corrupt', 'Часть архива повреждена (CRC/размер не сходятся): файл скачан не полностью?');
      return total;
    });
  }

  // --- string pools (ResStringPool) ---------------------------------------------------
  function StringPool(b, o) {
    this.b = b;
    var count = u32(b, o + 8), flags = u32(b, o + 16);
    this.utf8 = !!(flags & 0x100);
    this.stringsStart = o + u32(b, o + 20);
    this.offsets = [];
    for (var i = 0; i < count; i++) this.offsets.push(u32(b, o + 28 + i * 4));
    this.cache = [];
  }
  StringPool.prototype.get = function (i) {
    if (i < 0 || i >= this.offsets.length) return null;
    if (this.cache[i] !== undefined) return this.cache[i];
    var b = this.b, p = this.stringsStart + this.offsets[i], s;
    if (this.utf8) {
      var n = b[p++]; if (n & 0x80) p++;                       // UTF-16 length
      var len = b[p++]; if (len & 0x80) len = ((len & 0x7f) << 8) | b[p++];
      s = decodeUtf8(b.subarray(p, p + len));
    } else {
      var l = u16(b, p); p += 2;
      if (l & 0x8000) { l = ((l & 0x7fff) << 16) | u16(b, p); p += 2; }
      s = '';
      for (var k = 0; k < l; k++) s += String.fromCharCode(u16(b, p + k * 2));
    }
    return (this.cache[i] = s);
  };

  // --- binary XML (ResXMLTree) --------------------------------------------------------
  // -> a tree of {tag, attrs: [{name, id, ns, value}], children}; value is
  // {type, data, string} (Res_value: 0x01 reference, 0x03 string,
  // 0x10/0x11 int, 0x12 boolean, 0x1c..0x1f colour).
  function parseXml(b) {
    if (u16(b, 0) !== 0x0003) throw ApkError('corrupt', 'not binary XML');
    var pool = null, resIds = [], top = {tag: '#document', attrs: [], children: []}, stack = [top];
    for (var p = u16(b, 2); p + 8 <= b.length;) {
      var type = u16(b, p), headerSize = u16(b, p + 2), size = u32(b, p + 4);
      if (size < 8) break;
      if (type === 0x0001) pool = new StringPool(b, p);
      else if (type === 0x0180) {
        for (var i = p + headerSize; i < p + size; i += 4) resIds.push(u32(b, i));
      } else if (type === 0x0102) {
        var ext = p + headerSize;
        var nameIdx = i32(b, ext + 4), attrStart = u16(b, ext + 8), attrSize = u16(b, ext + 10),
            attrCount = u16(b, ext + 12);
        var el = {tag: pool.get(nameIdx), attrs: [], children: []};
        for (var a = 0; a < attrCount; a++) {
          var q = ext + attrStart + a * attrSize;
          var an = i32(b, q + 4), raw = i32(b, q + 8);
          var vtype = b[q + 15], data = u32(b, q + 16);
          el.attrs.push({
            name: pool.get(an) || '', id: an >= 0 && an < resIds.length ? resIds[an] : 0,
            value: {type: vtype, data: data, string: raw >= 0 ? pool.get(raw) : (vtype === 3 ? pool.get(data) : null)},
          });
        }
        stack[stack.length - 1].children.push(el);
        stack.push(el);
      } else if (type === 0x0103) {
        if (stack.length > 1) stack.pop();
      }
      p += size;
    }
    return top;
  }
  // Attribute of an element by its framework resource id (names can be
  // stripped by obfuscators) or name.
  var A = {label: 0x01010001, icon: 0x01010002, name: 0x01010003, enabled: 0x0101000e,
           screenOrientation: 0x0101001e,
           hasCode: 0x0101000c, isFeatureSplit: 0x0101055b,
           targetActivity: 0x01010202, minSdkVersion: 0x0101020c, versionCode: 0x0101021b,
           versionName: 0x0101021c, targetSdkVersion: 0x01010270, roundIcon: 0x0101052c,
           drawable: 0x01010199, viewportWidth: 0x01010402, viewportHeight: 0x01010403,
           pathData: 0x01010405, fillColor: 0x01010404, strokeColor: 0x01010406,
           strokeWidth: 0x01010407, fillAlpha: 0x010104cc, strokeAlpha: 0x010104cb,
           rotation: 0x01010326, pivotX: 0x010101b5, pivotY: 0x010101b6,
           scaleX: 0x01010324, scaleY: 0x01010325, translateX: 0x010104ca, translateY: 0x010104c9,
           fillType: 0x0101051e, alpha: 0x0101031f, inset: 0x010104b5};
  function attr(el, key) {
    var id = A[key];
    for (var i = 0; i < el.attrs.length; i++) if (id && el.attrs[i].id === id) return el.attrs[i].value;
    for (i = 0; i < el.attrs.length; i++) if (el.attrs[i].name === key) return el.attrs[i].value;
    return null;
  }
  function find(el, tag) {
    for (var i = 0; i < el.children.length; i++) if (el.children[i].tag === tag) return el.children[i];
    return null;
  }
  function all(el, tag) { return el.children.filter(function (c) { return c.tag === tag; }); }
  function intOf(v) {
    if (!v) return null;
    if (v.type >= 0x10 && v.type <= 0x12) return v.data | 0;
    if (v.string != null && /^-?\d+$/.test(v.string)) return +v.string;
    return null;
  }
  // Complex dimension (TYPE_DIMENSION 0x05) / float (0x04) to a number.
  function floatOf(v) {
    if (!v) return null;
    if (v.type === 0x04) { var f = new DataView(new ArrayBuffer(4)); f.setUint32(0, v.data, true); return f.getFloat32(0, true); }
    if (v.type === 0x05 || v.type === 0x06) {
      var mant = (v.data >> 8), radix = (v.data >> 4) & 3;
      return [1, 1 / 128, 1 / 32768, 1 / 8388608][radix] * mant;
    }
    if (v.type >= 0x10 && v.type <= 0x11) return v.data | 0;
    if (v.string != null && !isNaN(parseFloat(v.string))) return parseFloat(v.string);
    return null;
  }

  // --- resources.arsc (ResTable) -------------------------------------------------------
  // resid -> [{config, value}] for the base package (0x7f, or the first one).
  // |into|: a table to add to (an app's parts: their resources.arsc have the
  // base's ids for the other configurations, strings resolved per table).
  function parseArsc(b, into) {
    if (u16(b, 0) !== 0x0002) throw ApkError('corrupt', 'bad resources.arsc');
    var table = into || {strings: null, entries: Object.create(null), packageId: 0x7f};
    var p = u16(b, 2);
    while (p + 8 <= b.length) {
      var type = u16(b, p), size = u32(b, p + 4);
      if (size < 8) break;
      if (type === 0x0001) table.strings = new StringPool(b, p);
      else if (type === 0x0200) parsePackage(b, p, table);
      p += size;
    }
    return table;
  }
  function parsePackage(b, p, table) {
    var headerSize = u16(b, p + 2), size = u32(b, p + 4), id = u32(b, p + 8);
    if (table.seenPackage && id !== 0x7f) return;
    table.seenPackage = true;
    table.packageId = id;
    var typeStrings = null, keyStrings = null;
    var tsOff = u32(b, p + 268), ksOff = u32(b, p + 276);
    if (tsOff) typeStrings = new StringPool(b, p + tsOff);
    if (ksOff) keyStrings = new StringPool(b, p + ksOff);
    table.typeStrings = typeStrings;
    for (var q = p + headerSize; q + 8 <= p + size;) {
      var t = u16(b, q), hs = u16(b, q + 2), sz = u32(b, q + 4);
      if (sz < 8) break;
      if (t === 0x0201) parseType(b, q, hs, id, table);
      q += sz;
    }
  }
  function parseConfig(b, c) {
    var cs = u32(b, c);
    var lang = '', country = '';
    if (b[c + 8]) lang = (b[c + 8] & 0x80) ? '?' : String.fromCharCode(b[c + 8], b[c + 9]);
    if (b[c + 10]) country = (b[c + 10] & 0x80) ? '?' : String.fromCharCode(b[c + 10], b[c + 11]);
    return {language: lang, country: country, density: cs >= 16 ? u16(b, c + 14) : 0,
            sdk: cs >= 24 ? u16(b, c + 20) : 0,
            night: cs >= 32 ? (b[c + 29] & 0x30) === 0x20 : false};
  }
  function parseType(b, q, hs, pkgId, table) {
    var typeId = b[q + 8], flags = b[q + 9], count = u32(b, q + 12), entriesStart = u32(b, q + 16);
    var config = parseConfig(b, q + 20);
    var typeName = table.typeStrings ? table.typeStrings.get(typeId - 1) : String(typeId);
    var idx = q + hs, base = q + entriesStart;
    function entryAt(index, off) {
      var e = base + off;
      var resid = ((pkgId << 24) | (typeId << 16) | index) >>> 0;
      var esize = u16(b, e), eflags = u16(b, e + 2), value;
      if (eflags & 0x8) {        // compact (aapt2 --enable-compact-entries)
        value = {type: eflags >> 8, data: u32(b, e + 4)};
      } else if (eflags & 0x1) { // complex (a style or bag): not needed here
        return;
      } else {
        var v = e + esize;
        value = {type: b[v + 3], data: u32(b, v + 4)};
      }
      if (value.type === 0x03 && table.strings) value.string = table.strings.get(value.data);
      var list = table.entries[resid] || (table.entries[resid] = []);
      list.push({config: config, value: value, type: typeName});
    }
    if (flags & 0x1) {           // sparse: (index u16, offset/4 u16)
      for (var i = 0; i < count; i++) entryAt(u16(b, idx + i * 4), u16(b, idx + i * 4 + 2) * 4);
    } else if (flags & 0x2) {    // offset16
      for (i = 0; i < count; i++) {
        var o16 = u16(b, idx + i * 2);
        if (o16 !== 0xffff) entryAt(i, o16 * 4);
      }
    } else {
      for (i = 0; i < count; i++) {
        var off = u32(b, idx + i * 4);
        if (off !== 0xffffffff) entryAt(i, off);
      }
    }
  }

  // --- resolving ---------------------------------------------------------------------------
  var DENSITY_ANY = 0xfffe, DENSITY_NONE = 0xffff;
  function resolveAll(table, v, depth) {
    // -> the candidate values (all configurations) of a reference, or [v].
    if (!v) return [];
    if (v.type !== 0x01 || !table) return [{config: {}, value: v}];
    if ((depth || 0) > 8) return [];
    var list = table.entries[v.data] || [];
    var out = [];
    list.forEach(function (c) {
      if (c.value.type === 0x01)
        resolveAll(table, c.value, (depth || 0) + 1).forEach(function (x) { out.push({config: c.config, value: x.value, type: x.type}); });
      else out.push(c);
    });
    return out;
  }
  // A string in the user's language: locales like ['ru', 'en'].
  function resolveString(table, v, locales) {
    if (!v) return null;
    if (v.type === 0x03 || (v.type !== 0x01 && v.string != null)) return v.string;
    var cands = resolveAll(table, v).filter(function (c) { return c.value.string != null; });
    if (!cands.length) return null;
    var tries = locales.concat(['']);
    for (var i = 0; i < tries.length; i++) {
      var hit = cands.filter(function (c) { return (c.config.language || '') === tries[i]; });
      if (hit.length) {
        var plain = hit.filter(function (c) { return !c.config.country; });
        return (plain[0] || hit[0]).value.string;
      }
    }
    return cands[0].value.string;
  }
  function colorOf(v) {
    if (!v || v.type < 0x1c || v.type > 0x1f) return null;
    var d = v.data >>> 0;
    var a = v.type === 0x1d || v.type === 0x1f ? 255 : (d >>> 24) & 255;
    if (v.type === 0x1e) { // #rgb
      return 'rgb(' + (((d >> 8) & 15) * 17) + ',' + (((d >> 4) & 15) * 17) + ',' + ((d & 15) * 17) + ')';
    }
    if (v.type === 0x1f) { // #argb4
      return 'rgba(' + (((d >> 8) & 15) * 17) + ',' + (((d >> 4) & 15) * 17) + ',' + ((d & 15) * 17) + ',' + (((d >> 12) & 15) / 15).toFixed(3) + ')';
    }
    return 'rgba(' + ((d >> 16) & 255) + ',' + ((d >> 8) & 255) + ',' + (d & 255) + ',' + (a / 255).toFixed(3) + ')';
  }
  // Files a drawable reference may be, best first: raster at the highest
  // density up to 640 (xxxhdpi), not night; XML (adaptive or vector) after
  // rasters only when it is anydpi (the adaptive icon is preferred for its
  // layers? no: a full-bleed raster is what launchers show before 8.0 and
  // is exact; the adaptive icon is drawn only when there is no raster).
  function drawableFiles(table, v) {
    var cands = resolveAll(table, v).filter(function (c) { return c.value.string != null || colorOf(c.value); });
    function score(c) {
      var d = c.config.density || 160;
      if (d === DENSITY_ANY || d === DENSITY_NONE) d = 1;
      var s = d <= 640 ? d : 640 - (d - 640) / 10;
      if (c.config.night) s -= 2000;
      if (c.config.language) s -= 4000;
      return s;
    }
    return cands.slice().sort(function (a, b) { return score(b) - score(a); });
  }

  // --- the manifest ----------------------------------------------------------------------
  function launcherOf(app, pkg) {
    function full(n) {
      if (!n) return '';
      if (n.charAt(0) === '.') return pkg + n;
      if (n.indexOf('.') < 0) return pkg + '.' + n;
      return n;
    }
    var found = '';
    app.children.forEach(function (c) {
      if (found || (c.tag !== 'activity' && c.tag !== 'activity-alias')) return;
      var en = attr(c, 'enabled');
      if (en && en.type === 0x12 && en.data === 0) return;
      all(c, 'intent-filter').forEach(function (f) {
        var main = all(f, 'action').some(function (a) { var n = attr(a, 'name'); return n && n.string === 'android.intent.action.MAIN'; });
        var launch = all(f, 'category').some(function (a) { var n = attr(a, 'name'); return n && n.string === 'android.intent.category.LAUNCHER'; });
        if (main && launch && !found) {
          var n = attr(c, 'name');
          found = full(n && n.string);
        }
      });
    });
    return found;
  }
  // The launcher activity's android:screenOrientation (an alias: its
  // target's): 'landscape' (landscape, sensorLandscape, reverseLandscape,
  // userLandscape), 'portrait' (the four portrait ones) or null.
  var ORIENTATIONS = {0: 'landscape', 6: 'landscape', 8: 'landscape', 11: 'landscape',
                      1: 'portrait', 7: 'portrait', 9: 'portrait', 12: 'portrait'};
  function orientationOf(app, pkg, launcher) {
    function full(n) {
      if (!n) return '';
      if (n.charAt(0) === '.') return pkg + n;
      if (n.indexOf('.') < 0) return pkg + '.' + n;
      return n;
    }
    function named(tag, name) {
      return app.children.filter(function (c) {
        var n = c.tag === tag && attr(c, 'name');
        return n && full(n.string) === name;
      })[0];
    }
    var act = named('activity', launcher), alias = !act && named('activity-alias', launcher);
    if (alias) {
      var t = attr(alias, 'targetActivity');
      act = t && named('activity', full(t.string));
    }
    var v = act ? intOf(attr(act, 'screenOrientation')) : null;
    return ORIENTATIONS[v] || null;
  }

  // --- icons ------------------------------------------------------------------------------
  // -> {kind: 'image', mime, bytes} | {kind: 'adaptive', background, foreground} with layers
  //    {kind: 'image', mime, bytes} | {kind: 'color', color} | {kind: 'svg', svg} | null.
  function mimeOf(path) {
    if (/\.png$/i.test(path)) return 'image/png';
    if (/\.webp$/i.test(path)) return 'image/webp';
    if (/\.jpe?g$/i.test(path)) return 'image/jpeg';
    return null;
  }
  function iconOf(ctx, refs) {
    // Raster first, across the references given (icon, roundIcon).
    var lists = refs.filter(Boolean).map(function (r) { return drawableFiles(ctx.table, r); });
    for (var i = 0; i < lists.length; i++) {
      var raster = lists[i].filter(function (c) { return c.value.string && mimeOf(c.value.string) && ctx.entries[c.value.string]; });
      if (raster.length) return image(ctx, raster[0].value.string);
    }
    for (i = 0; i < lists.length; i++) {
      // The adaptive icon (mipmap-anydpi-v26) before older XML drawables.
      var xml = lists[i].filter(function (c) { return c.value.string && /\.xml$/i.test(c.value.string) && ctx.entries[c.value.string]; })
          .sort(function (a, b) { return (b.config.density === DENSITY_ANY) - (a.config.density === DENSITY_ANY) || (b.config.sdk || 0) - (a.config.sdk || 0); });
      if (xml.length) return layer(ctx, {type: 0x03, string: xml[0].value.string}, 0).catch(function () { return null; });
    }
    return Promise.resolve(null);
  }
  function image(ctx, path) {
    return readEntry(ctx.blob, ctx.entries[path]).then(function (bytes) {
      return {kind: 'image', mime: mimeOf(path), bytes: bytes, path: path};
    });
  }
  // One drawable (a reference, a file path, a colour) as a layer.
  function layer(ctx, v, depth) {
    if (!v || depth > 4) return Promise.resolve(null);
    var c = colorOf(v);
    if (c) return Promise.resolve({kind: 'color', color: c});
    var files = v.type === 0x01 ? drawableFiles(ctx.table, v) : [{value: v, config: {}}];
    var raster = files.filter(function (f) { return f.value.string && mimeOf(f.value.string) && ctx.entries[f.value.string]; });
    if (raster.length) return image(ctx, raster[0].value.string);
    var col = files.filter(function (f) { return colorOf(f.value); });
    if (col.length) return Promise.resolve({kind: 'color', color: colorOf(col[0].value)});
    var xml = files.filter(function (f) { return f.value.string && /\.xml$/i.test(f.value.string) && ctx.entries[f.value.string]; });
    if (!xml.length) return Promise.resolve(null);
    var path = xml[0].value.string;
    return readEntry(ctx.blob, ctx.entries[path]).then(function (bytes) {
      var doc = parseXml(bytes), root = doc.children[0];
      if (!root) return null;
      if (root.tag === 'adaptive-icon') {
        var bg = find(root, 'background'), fg = find(root, 'foreground');
        return Promise.all([bg ? layerOfElement(ctx, bg, depth) : null,
                            fg ? layerOfElement(ctx, fg, depth) : null]).then(function (l) {
          if (!l[0] && !l[1]) return null;
          return {kind: 'adaptive', background: l[0], foreground: l[1]};
        });
      }
      if (root.tag === 'vector') return vectorSvg(ctx, root);
      if (root.tag === 'bitmap') return layer(ctx, attr(root, 'src') || attrByName(root, 'src'), depth + 1);
      if (root.tag === 'inset') {
        return layer(ctx, attr(root, 'drawable'), depth + 1).then(function (l) {
          if (!l) return null;
          var f = floatOf(attr(root, 'inset') || attrByName(root, 'inset'));
          l.inset = attr(root, 'inset') && attr(root, 'inset').type === 0x06 ? f : 0;
          return l;
        });
      }
      if (root.tag === 'layer-list') {
        var items = all(root, 'item');
        return items.length ? layerOfElement(ctx, items[items.length - 1], depth) : null;
      }
      if (root.tag === 'shape') return shapeSvg(ctx, root);
      return null;
    });
  }
  function attrByName(el, name) {
    for (var i = 0; i < el.attrs.length; i++) if (el.attrs[i].name === name) return el.attrs[i].value;
    return null;
  }
  function attrId(el, id) {
    for (var i = 0; i < el.attrs.length; i++) if (el.attrs[i].id === id) return el.attrs[i].value;
    return null;
  }
  // <background android:drawable="..."/> or an inline child drawable.
  function layerOfElement(ctx, el, depth) {
    var d = attr(el, 'drawable');
    if (d) return layer(ctx, d, depth + 1);
    var child = el.children[0];
    if (child && child.tag === 'vector') return Promise.resolve(vectorSvg(ctx, child));
    if (child && child.tag === 'inset') {
      return layer(ctx, attr(child, 'drawable'), depth + 1);
    }
    return Promise.resolve(null);
  }
  function resolveColor(ctx, v) {
    if (!v) return null;
    var c = colorOf(v);
    if (c) return c;
    if (v.type === 0x01) {
      var cands = resolveAll(ctx.table, v).filter(function (x) { return colorOf(x.value) && !x.config.night; });
      if (!cands.length) cands = resolveAll(ctx.table, v).filter(function (x) { return colorOf(x.value); });
      if (cands.length) return colorOf(cands[0].value);
    }
    if (v.string && /^#[0-9a-f]{3,8}$/i.test(v.string)) return v.string;
    return null;
  }
  // A <shape> (GradientDrawable) stretched over its bounds: a solid
  // rectangle (rounded) or oval as SVG; gradients as their middle colour.
  function shapeSvg(ctx, root) {
    var kind = intOf(attrId(root, 0x0101011c) || attrByName(root, 'shape')) || 0;  // android:shape
    var solid = find(root, 'solid'), grad = find(root, 'gradient'), corners = find(root, 'corners');
    var color = solid && resolveColor(ctx, attrByName(solid, 'color') || attrId(solid, 0x010101a5));
    if (!color && grad) color = resolveColor(ctx, attrByName(grad, 'centerColor') || attrByName(grad, 'startColor') ||
                                             attrId(grad, 0x0101020b) || attrId(grad, 0x0101019d));
    if (!color) return null;
    if (kind === 0 && !corners) return {kind: 'color', color: color};
    var r = corners ? (floatOf(attrByName(corners, 'radius') || attrId(corners, 0x010101a8)) || 0) : 0;
    var body = kind === 1 ? '<ellipse cx="50" cy="50" rx="50" ry="50" fill="' + color + '"/>'
                          : '<rect width="100" height="100" rx="' + Math.min(50, r * 100 / 48) + '" fill="' + color + '"/>';
    return {kind: 'svg', svg: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100" width="400" height="400" preserveAspectRatio="none">' + body + '</svg>'};
  }

  // A VectorDrawable as SVG: paths (fill, stroke, alphas, even-odd), groups
  // (translate/scale/rotate about a pivot), clip-paths. Gradients (inline
  // aapt:attr) become a mid grey.
  function esc(s) { return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/"/g, '&quot;'); }
  function vectorSvg(ctx, root) {
    var vw = floatOf(attr(root, 'viewportWidth')) || 24, vh = floatOf(attr(root, 'viewportHeight')) || 24;
    var clipN = 0, defs = '';
    function paint(v) {
      if (!v) return null;
      var c = resolveColor(ctx, v);
      if (c) return c;
      return v.type === 0x01 ? '#888888' : null;
    }
    function num(el, key, def) { var f = floatOf(attr(el, key)); return f == null ? def : f; }
    function body(el) {
      var out = '';
      el.children.forEach(function (c) {
        if (c.tag === 'path') {
          var d = attr(c, 'pathData');
          d = d && (d.string || resolveString(ctx.table, d, []));
          if (!d) return;
          var fill = paint(attr(c, 'fillColor')), stroke = paint(attr(c, 'strokeColor'));
          var ft = intOf(attr(c, 'fillType'));
          out += '<path d="' + esc(d) + '" fill="' + (fill || 'none') + '"' +
              (fill ? ' fill-opacity="' + num(c, 'fillAlpha', 1) + '"' : '') +
              (ft === 1 ? ' fill-rule="evenodd"' : '') +
              (stroke ? ' stroke="' + stroke + '" stroke-width="' + num(c, 'strokeWidth', 0) +
                        '" stroke-opacity="' + num(c, 'strokeAlpha', 1) + '"' : '') + '/>';
        } else if (c.tag === 'group') {
          var px = num(c, 'pivotX', 0), py = num(c, 'pivotY', 0);
          var t = 'translate(' + (num(c, 'translateX', 0) + px) + ' ' + (num(c, 'translateY', 0) + py) + ') ' +
              'rotate(' + num(c, 'rotation', 0) + ') scale(' + num(c, 'scaleX', 1) + ' ' + num(c, 'scaleY', 1) + ') ' +
              'translate(' + (-px) + ' ' + (-py) + ')';
          out += '<g transform="' + t + '">' + body(c) + '</g>';
        } else if (c.tag === 'clip-path') {
          var cd = attr(c, 'pathData');
          cd = cd && (cd.string || resolveString(ctx.table, cd, []));
          if (!cd) return;
          var id = 'c' + (clipN++);
          defs += '<clipPath id="' + id + '"><path d="' + esc(cd) + '"/></clipPath>';
          // The clip applies to the following siblings: wrap the rest.
          out += '<g clip-path="url(#' + id + ')">';
          c.clipOpen = true;
        }
      });
      // Close the clip groups opened in this element.
      var opened = el.children.filter(function (c) { return c.clipOpen; }).length;
      for (var k = 0; k < opened; k++) out += '</g>';
      return out;
    }
    var inner = body(root);
    var alpha = num(root, 'alpha', 1);
    var svg = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ' + vw + ' ' + vh + '" width="' + vw * 4 +
        '" height="' + vh * 4 + '"><defs>' + defs + '</defs><g opacity="' + alpha + '">' + inner + '</g></svg>';
    return {kind: 'svg', svg: svg};
  }

  // --- the APK -----------------------------------------------------------------------------
  var KNOWN_ABIS = ['armeabi-v7a', 'armeabi', 'arm64-v8a', 'x86', 'x86_64', 'mips', 'mips64'];
  function read(blob, options) {
    options = options || {};
    var locales = options.locales || ['en'];
    var splits = options.splits || [];
    var ctx = {blob: blob};
    return readZip(blob).catch(function (e) {
      if (e.code === 'notzip') throw ApkError('notapk', 'Это не APK: файл не похож на архив приложения.');
      throw e;
    }).then(function (entries) {
      ctx.entries = entries;
      var names = Object.keys(entries);
      if (!entries['AndroidManifest.xml']) {
        var inner = names.filter(function (n) { return /\.apk$/i.test(n); });
        if (inner.length || entries['manifest.json'] || entries['toc.pb'])
          throw ApkError('bundle', 'Это набор APK (XAPK/APKS/APKM), а не одно приложение. Нужен цельный файл .apk.');
        throw ApkError('notapk', 'В архиве нет AndroidManifest.xml — это не APK.');
      }
      var abis = {};
      names.forEach(function (n) {
        var m = /^lib\/([^/]+)\/[^/]+\.so$/.exec(n);
        if (m) abis[m[1]] = true;
      });
      ctx.abis = Object.keys(abis).sort(function (a, b) {
        return (KNOWN_ABIS.indexOf(a) + 99) % 99 - (KNOWN_ABIS.indexOf(b) + 99) % 99;
      });
      return Promise.all([readEntry(blob, entries['AndroidManifest.xml']),
                          entries['resources.arsc'] ? readEntry(blob, entries['resources.arsc']) : null,
                          Promise.all(splits.map(function (b) {
                            return readZip(b).then(function (en) { return {blob: b, entries: en}; });
                          }))]);
    }).then(function (parts) {
      try { ctx.table = parts[1] ? parseArsc(parts[1]) : null; } catch (e) { ctx.table = null; }
      // The parts' files and resources (an app bundle keeps the icon at each
      // density in that density's part, translations in the language parts).
      return Promise.all(parts[2].map(function (sp) {
        Object.keys(sp.entries).forEach(function (n) {
          if (ctx.entries[n] || n === 'AndroidManifest.xml' || n === 'resources.arsc') return;
          sp.entries[n].blob = sp.blob;
          ctx.entries[n] = sp.entries[n];
        });
        return sp.entries['resources.arsc'] ? readEntry(sp.blob, sp.entries['resources.arsc']) : null;
      })).then(function (arscs) {
        arscs.forEach(function (a) {
          if (!a) return;
          try { ctx.table = parseArsc(a, ctx.table || undefined); } catch (e) { /* the part's resources unread */ }
        });
        return parts[0];
      });
    }).then(function (manifest) {
      var doc = parseXml(manifest);
      var man = doc.children[0];
      if (!man || man.tag !== 'manifest') throw ApkError('corrupt', 'В APK повреждён AndroidManifest.xml.');
      var pkgV = attrByName(man, 'package');
      var pkg = pkgV && pkgV.string;
      if (!pkg) throw ApkError('corrupt', 'В манифесте нет имени пакета.');
      var sdk = find(man, 'uses-sdk');
      var app = find(man, 'application') || {attrs: [], children: []};
      var info = {
        package: pkg,
        versionCode: intOf(attr(man, 'versionCode')) || 0,
        versionName: resolveString(ctx.table, attr(man, 'versionName'), locales) || '',
        minSdk: sdk ? (intOf(attr(sdk, 'minSdkVersion')) || 1) : 1,
        targetSdk: 0,
        abis: ctx.abis,
        launcher: launcherOf(app, pkg),
        label: resolveString(ctx.table, attr(app, 'label'), locales) || '',
        size: blob.size,
        warnings: [],
      };
      info.targetSdk = sdk ? (intOf(attr(sdk, 'targetSdkVersion')) || info.minSdk) : info.minSdk;
      if (!info.label) info.label = pkg.split('.').pop();
      var split = attrByName(man, 'split');
      if (split && split.string) throw ApkError('split', 'Это часть приложения (split APK «' + split.string + '»), а не всё приложение.');
      if (attrByName(man, 'isSplitRequired') && attrByName(man, 'isSplitRequired').data ||
          attrByName(man, 'requiredSplitTypes'))
        info.warnings.push('split');
      info.orientation = info.launcher ? orientationOf(app, pkg, info.launcher) : null;
      if (!info.launcher) info.warnings.push('nolauncher');
      return iconOf(ctx, [attr(app, 'icon'), attr(app, 'roundIcon')]).catch(function () { return null; })
          .then(function (icon) {
            info.icon = icon;
            return info;
          });
    });
  }

  // --- an app's parts: .apks, .xapk, .apkm (topic apks-split) ------------------------
  // A part's manifest: which app, which split, code or not.
  function readSplit(blob) {
    return readZip(blob).catch(function (e) {
      if (e.code === 'notzip') throw ApkError('notapk', 'Часть приложения — не APK.');
      throw e;
    }).then(function (entries) {
      if (!entries['AndroidManifest.xml']) throw ApkError('notapk', 'В части приложения нет AndroidManifest.xml.');
      var abis = {};
      Object.keys(entries).forEach(function (n) {
        var m = /^lib\/([^/]+)\/[^/]+\.so$/.exec(n);
        if (m) abis[m[1]] = true;
      });
      return readEntry(blob, entries['AndroidManifest.xml']).then(function (bytes) {
        var man = parseXml(bytes).children[0];
        if (!man || man.tag !== 'manifest') throw ApkError('corrupt', 'В части приложения повреждён AndroidManifest.xml.');
        var app = find(man, 'application') || {attrs: [], children: []};
        var str = function (v) { return v && v.string || null; };
        var hasCode = attr(app, 'hasCode'), feature = attr(man, 'isFeatureSplit');
        return {
          package: str(attrByName(man, 'package')),
          versionCode: intOf(attr(man, 'versionCode')) || 0,
          split: str(attrByName(man, 'split')),
          configForSplit: str(attrByName(man, 'configForSplit')),
          isFeatureSplit: !!(feature && feature.data),
          hasCode: hasCode ? !!hasCode.data : entries['classes.dex'] != null,
          abis: Object.keys(abis),
          size: blob.size,
        };
      });
    });
  }

  // The configuration a part is for, from its name: split_config.arm64_v8a.apk,
  // config.xxhdpi.apk, base-ru.apk (bundletool) ... -> {kind: 'base' | 'abi'
  // | 'density' | 'locale' | 'config' (another) | 'feature', value, module}.
  var ABI_TOKENS = {armeabi_v7a: 'armeabi-v7a', armeabi: 'armeabi', arm64_v8a: 'arm64-v8a',
                    x86: 'x86', x86_64: 'x86_64', mips: 'mips', mips64: 'mips64'};
  var DENSITIES = {ldpi: 120, mdpi: 160, tvdpi: 213, hdpi: 240, xhdpi: 320, xxhdpi: 480, xxxhdpi: 640};
  function configKind(token, module) {
    if (ABI_TOKENS[token]) return {kind: 'abi', value: ABI_TOKENS[token], module: module};
    if (DENSITIES[token]) return {kind: 'density', value: token, module: module};
    var m = /^(?:b\+)?([a-z]{2,3})(?:[_+-]r?[A-Za-z0-9]{2,8})*$/.exec(token);
    if (m) return {kind: 'locale', value: m[1], module: module};
    return {kind: 'config', value: token, module: module};
  }
  function partOf(path, id) {
    var name = (id || path.split('/').pop().replace(/\.apk$/i, ''));
    if (id) {
      if (id === 'base') return {kind: 'base', module: 'base'};
      var c = /^(?:(.+)\.)?config\.(.+)$/.exec(id);
      if (c) return configKind(c[2], c[1] || 'base');
      return {kind: 'feature', value: id, module: id};
    }
    if (/^splits\//.test(path)) {      // bundletool: splits/MODULE-master.apk, MODULE-CONFIG.apk
      var b = /^(.+?)-(.+)$/.exec(name);
      if (b) {
        if (b[2] === 'master') return b[1] === 'base' ? {kind: 'base', module: 'base'} : {kind: 'feature', value: b[1], module: b[1]};
        return configKind(b[2], b[1]);
      }
    }
    name = name.replace(/^split_/, '');
    if (name === 'base' || name === 'base-master' || name === 'universal') return {kind: 'base', module: 'base'};
    var cf = /^(?:(.+)\.)?config\.(.+)$/.exec(name);
    if (cf) return configKind(cf[2], cf[1] || 'base');
    if (ABI_TOKENS[name] || DENSITIES[name]) return configKind(name, 'base');
    return {kind: 'feature', value: name, module: name};
  }

  // What an archive of parts holds: {kind: 'apks' | 'xapk' | 'apkm' |
  // 'bundletool', parts: [{path, entry, kind, value, module}], obb: [{path,
  // entry, file}], package (from the archive's own manifest, if it has one)},
  // or {kind: 'apk'} for a plain APK.
  function readBundle(blob) {
    return readZip(blob).catch(function (e) {
      if (e.code === 'notzip') throw ApkError('notapk', 'Это не APK: файл не похож на архив приложения.');
      throw e;
    }).then(function (entries) {
      if (entries['AndroidManifest.xml']) return {kind: 'apk'};
      var names = Object.keys(entries);
      var apks = names.filter(function (n) { return /\.apk$/i.test(n) && !/\/$/.test(n); });
      if (!apks.length) throw ApkError('notapk', 'В архиве нет AndroidManifest.xml — это не APK.');
      var b = {kind: 'apks', parts: [], obb: [], package: null, versionCode: null, warnings: []};
      if (entries['manifest.json']) b.kind = 'xapk';
      else if (entries['info.json']) b.kind = 'apkm';
      else if (entries['toc.pb']) b.kind = 'bundletool';
      var json = function (n) {
        return entries[n] ? readEntry(blob, entries[n]).then(function (x) {
          try { return JSON.parse(decodeUtf8(x)); } catch (e) { return null; }
        }, function () { return null; }) : Promise.resolve(null);
      };
      return Promise.all([json('manifest.json'), json('info.json')]).then(function (j) {
        var xm = j[0] || {}, info = j[1] || {};
        b.package = xm.package_name || info.pname || info.package_name || null;
        b.versionCode = +(xm.version_code || info.versioncode || info.version_code) || null;
        b.label = xm.name || info.app_name || null;
        var ids = {};
        (xm.split_apks || []).forEach(function (s) { if (s && s.file) ids[s.file] = s.id; });
        // bundletool: the split APKs; the standalone ones (for Android before
        // 5.0) only if there is nothing else.
        var use = apks.filter(function (n) { return !/^(standalones|asset-slices)\//.test(n); });
        if (!use.length) use = apks.slice(0, 1);
        use.forEach(function (n) {
          var e = entries[n];
          if (e.flags & 1) throw ApkError('encrypted', 'Архив зашифрован (так бывает у .apkm новых версий) — такой файл не открыть.');
          var p = partOf(n, ids[n]);
          p.path = n;
          p.entry = e;
          b.parts.push(p);
        });
        // An .xapk names its base after the package (com.example.apk):
        // without its manifest's ids, the one part that is not a split is it.
        var bases = b.parts.filter(function (p) { return p.kind === 'base'; });
        if (!bases.length) {
          var whole = b.parts.filter(function (p) { return p.kind === 'feature'; });
          var named = whole.filter(function (p) { return b.package && p.path.split('/').pop() === b.package + '.apk'; });
          var base = named[0] || whole.sort(function (x, y) { return y.entry.size - x.entry.size; })[0];
          if (base) { base.kind = 'base'; base.module = 'base'; delete base.value; }
        }
        if (!b.parts.some(function (p) { return p.kind === 'base'; }))
          throw ApkError('nobase', 'В наборе нет основной части приложения (base.apk).');
        if (b.parts.filter(function (p) { return p.kind === 'base'; }).length > 1)
          throw ApkError('nobase', 'В наборе несколько основных частей — это не одно приложение.');
        names.forEach(function (n) {
          var m = /^Android\/obb\/([^/]+)\/([^/]+\.obb)$/i.exec(n);
          if (m && !(entries[n].flags & 1)) b.obb.push({path: n, entry: entries[n], file: m[2], dir: m[1]});
        });
        return b;
      });
    });
  }

  // The parts for this device: the base, every feature module, and of each
  // module's configuration parts the first ABI of |abis| it has, the density
  // nearest at or above |dpi|, the languages of |locales|; other
  // configurations are taken. -> {parts, skipped: [{part, why}], abi,
  // density, offered: {abis, densities, locales}, warnings}
  var ABI_ORDER = ['armeabi-v7a', 'armeabi', 'arm64-v8a'];
  function choose(bundle, options) {
    options = options || {};
    var order = options.abis || ABI_ORDER, dpi = options.dpi || 160;
    var locales = options.locales || ['ru', 'en'];
    var out = {parts: [], skipped: [], abi: null, density: null, warnings: [],
               offered: {abis: [], densities: [], locales: []}};
    var add = function (list, v) { if (list.indexOf(v) < 0) list.push(v); };
    var modules = {};
    bundle.parts.forEach(function (p) {
      (modules[p.module] || (modules[p.module] = [])).push(p);
      if (p.kind === 'abi') add(out.offered.abis, p.value);
      if (p.kind === 'density') add(out.offered.densities, p.value);
      if (p.kind === 'locale') add(out.offered.locales, p.value);
    });
    Object.keys(modules).forEach(function (m) {
      var ps = modules[m];
      var abis = ps.filter(function (p) { return p.kind === 'abi'; });
      var abi = null;
      for (var i = 0; i < order.length && !abi; i++)
        abi = abis.filter(function (p) { return p.value === order[i]; })[0] || null;
      if (abis.length && !abi)
        out.warnings.push('noabi');
      var dens = ps.filter(function (p) { return p.kind === 'density'; });
      var above = dens.filter(function (p) { return DENSITIES[p.value] >= dpi; })
          .sort(function (a, b) { return DENSITIES[a.value] - DENSITIES[b.value]; });
      var density = above[0] || dens.sort(function (a, b) { return DENSITIES[b.value] - DENSITIES[a.value]; })[0] || null;
      ps.forEach(function (p) {
        var take = p.kind === 'base' || p.kind === 'feature' || p.kind === 'config' ||
            (p.kind === 'abi' && p === abi) || (p.kind === 'density' && p === density) ||
            (p.kind === 'locale' && locales.indexOf(p.value) >= 0);
        if (take) out.parts.push(p);
        else out.skipped.push({part: p, why: p.kind === 'abi' ? 'abi' : p.kind === 'density' ? 'density' : 'locale'});
      });
      if (abi && m === 'base') out.abi = abi.value;
      if (density && m === 'base') out.density = density.value;
    });
    // The base first, then by name (the order the package manager lists them).
    out.parts.sort(function (a, b) {
      return (b.kind === 'base') - (a.kind === 'base') || (a.path < b.path ? -1 : a.path > b.path ? 1 : 0);
    });
    return out;
  }

  // Which ABIs each system runs (the NDK bridge's: ARMv7 everywhere, AArch64
  // on 8.1 and 10, topics ndk-arm64, fw10-system).
  var STACK_ABIS = {a10: ['armeabi-v7a', 'armeabi', 'arm64-v8a'], a81: ['armeabi-v7a', 'armeabi', 'arm64-v8a'],
                    a60: ['armeabi-v7a', 'armeabi'], a44: ['armeabi-v7a', 'armeabi']};
  var STACK_API = {a10: 29, a81: 27, a60: 23, a44: 19};
  // The system an app gets when it is installed: Android 10 (topic
  // a10-default: it shows the first screen of at least as many apps as 8.1,
  // docs/topics/a10-default.md); 8.1 in a build without 10. Apps already in a
  // library keep the system they have (meta.stack). |available|: the systems
  // in this build ({a10: true, ...}).
  function defaultStack(meta, available) {
    available = available || {a10: true, a81: true};
    return available.a10 ? 'a10' : 'a81';
  }
  // Can |meta| run on system |stack| ('a81' ...)? -> null, or why not.
  function stackProblem(meta, stack) {
    if (meta.parts && meta.parts.length > 1 && STACK_API[stack] < 21)
      return 'Android 4.4 не умеет приложения из частей (split APK)';
    var abis = STACK_ABIS[stack];
    if (abis && meta.abis && meta.abis.length && !meta.abis.some(function (a) { return abis.indexOf(a) >= 0; }))
      return 'нет кода для ' + abis.join('/');
    return null;
  }
  // Warnings for the card: no ARM code where there is native code, too new,
  // not on this system (meta.stack).
  function warnings(meta) {
    var w = [];
    var arm = meta.abis && meta.abis.filter(function (a) { return STACK_ABIS.a81.indexOf(a) >= 0; });
    if (meta.abis && meta.abis.length && !arm.length)
      w.push({level: 'bad', text: 'Нет кода для ARM (есть: ' + meta.abis.join(', ') + ') — приложение не запустится.'});
    else if (meta.stack && stackProblem(meta, meta.stack))
      w.push({level: 'bad', text: 'На ' + stackName(meta.stack) + ' не запустится: ' + stackProblem(meta, meta.stack) +
              '. Выберите ' + ['a10', 'a81', 'a60', 'a44'].filter(function (s) { return !stackProblem(meta, s); }).map(stackName).join(' или ') + '.'});
    else if (arm && arm.length && arm.every(function (a) { return a === 'arm64-v8a'; }))
      w.push({level: 'warn', text: 'Только 64-битный код ARM (arm64-v8a): работает на Android 10 и 8.1, транслятор arm64 новый — возможны сбои.'});
    if (meta.minSdk > STACK_API.a10)
      w.push({level: 'warn', text: 'Требует Android ' + sdkName(meta.minSdk) + ' (API ' + meta.minSdk + '), а самая новая система здесь — 10 (API 29): может не работать.'});
    return w;
  }
  function stackName(s) { return {a10: 'Android 10', a81: 'Android 8.1', a60: 'Android 6.0', a44: 'Android 4.4'}[s] || s; }
  // The card's line about an app's parts: "основа, armeabi-v7a, mdpi, ru, en, модуль feature".
  function partsText(meta) {
    if (!meta.parts || !meta.parts.length) return '';
    var t = meta.parts.map(function (p) {
      if (!p.split) return 'основа';
      if (p.kind === 'abi' || p.kind === 'density' || p.kind === 'locale') return p.value;
      if (p.kind === 'feature') return 'модуль ' + p.split;
      return p.split;
    });
    return t.join(', ');
  }

  var SDK_NAMES = {14: '4.0', 15: '4.0.3', 16: '4.1', 17: '4.2', 18: '4.3', 19: '4.4', 20: '4.4W', 21: '5.0',
                   22: '5.1', 23: '6.0', 24: '7.0', 25: '7.1', 26: '8.0', 27: '8.1', 28: '9', 29: '10', 30: '11',
                   31: '12', 32: '12L', 33: '13', 34: '14', 35: '15', 36: '16', 37: '17'};
  function sdkName(n) { return SDK_NAMES[n] || (n < 14 ? '< 4.0' : 'API ' + n); }

  var api = {read: read, readSplit: readSplit, readBundle: readBundle, choose: choose, partOf: partOf,
             streamEntry: streamEntry, crc32: crc32, stackProblem: stackProblem, defaultStack: defaultStack, partsText: partsText,
             ABI_ORDER: ABI_ORDER, DENSITIES: DENSITIES,
             warnings: warnings, sdkName: sdkName, readZip: readZip, readEntry: readEntry,
             parseXml: parseXml, parseArsc: parseArsc, inflateJs: inflateJs,
             get nativeInflate() { return nativeInflate; }, set nativeInflate(v) { nativeInflate = v; }};
  root.SensesApk = api;
  if (typeof module === 'object' && module.exports) module.exports = api;
})(typeof self !== 'undefined' ? self : this);
