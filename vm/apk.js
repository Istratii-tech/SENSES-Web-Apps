// What a page reads from an APK before it goes into SENSES Web Apps'
// library (topic install8): AndroidManifest.xml's package, versionCode,
// versionName, minSdkVersion, targetSdkVersion, and the ABIs of lib/.
// A reference for the shell (demo/apps). Needs DecompressionStream
// ('deflate-raw': Safari 16.4+, Chrome 80+) for a compressed manifest.
(function () {
  const u16 = (b, o) => b[o] | (b[o + 1] << 8);
  const u32 = (b, o) => (u16(b, o) | (u16(b, o + 2) << 16)) >>> 0;

  // name -> {method, size, csize, offset} from the central directory.
  function entries(b) {
    let eocd = -1;
    for (let i = b.length - 22; i >= Math.max(0, b.length - 65557); i--) {
      if (u32(b, i) === 0x06054b50) { eocd = i; break; }
    }
    if (eocd < 0) throw new Error('not a zip file');
    const count = u16(b, eocd + 10);
    let p = u32(b, eocd + 16);
    const out = new Map();
    const dec = new TextDecoder();
    for (let i = 0; i < count; i++) {
      if (u32(b, p) !== 0x02014b50) throw new Error('bad zip central directory');
      const nameLen = u16(b, p + 28), extraLen = u16(b, p + 30), commentLen = u16(b, p + 32);
      const name = dec.decode(b.subarray(p + 46, p + 46 + nameLen));
      out.set(name, {method: u16(b, p + 10), csize: u32(b, p + 20), size: u32(b, p + 24),
                     offset: u32(b, p + 42)});
      p += 46 + nameLen + extraLen + commentLen;
    }
    return out;
  }

  async function read(b, e) {
    const o = e.offset;
    const start = o + 30 + u16(b, o + 26) + u16(b, o + 28);
    const raw = b.subarray(start, start + e.csize);
    if (e.method === 0) return raw;
    const stream = new Blob([raw]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
    return new Uint8Array(await new Response(stream).arrayBuffer());
  }

  // Android's binary XML: the manifest element's attributes.
  function manifestAttributes(x) {
    let strings = [];
    let p = u16(x, 2);  // after the file header
    while (p < x.length) {
      const type = u16(x, p), headerSize = u16(x, p + 2), size = u32(x, p + 4);
      if (type === 0x0001) {  // string pool
        const count = u32(x, p + 8), flags = u32(x, p + 16), stringsStart = u32(x, p + 20);
        const utf8 = (flags & 0x100) !== 0;
        strings = [];
        for (let i = 0; i < count; i++) {
          let s = p + stringsStart + u32(x, p + headerSize + i * 4);
          if (utf8) {
            s += (x[s] & 0x80) ? 2 : 1;               // UTF-16 length
            let n = x[s]; s++;
            if (n & 0x80) { n = ((n & 0x7f) << 8) | x[s]; s++; }
            strings.push(new TextDecoder().decode(x.subarray(s, s + n)));
          } else {
            let n = u16(x, s); s += 2;
            if (n & 0x8000) { n = ((n & 0x7fff) << 16) | u16(x, s); s += 2; }
            let t = '';
            for (let k = 0; k < n; k++) t += String.fromCharCode(u16(x, s + 2 * k));
            strings.push(t);
          }
        }
      } else if (type === 0x0102) {  // start element
        const name = strings[u32(x, p + 20)];
        const attrStart = u16(x, p + 24), attrSize = u16(x, p + 26), attrCount = u16(x, p + 28);
        const attrs = {};
        for (let i = 0; i < attrCount; i++) {
          const a = p + 16 + attrStart + i * attrSize;
          const key = strings[u32(x, a + 4)];
          const raw = u32(x, a + 8);
          const dataType = x[a + 15], data = u32(x, a + 16);
          attrs[key] = raw !== 0xffffffff ? strings[raw]
                       : dataType === 0x03 ? strings[data] : data;
        }
        if (name === 'manifest') return {manifest: attrs, x, p: p + size, strings};
      }
      p += size;
    }
    throw new Error('no <manifest> in AndroidManifest.xml');
  }

  // The launcher activity's android:screenOrientation (attribute id
  // 0x0101001e: names may be stripped): 'landscape', 'portrait' or null.
  // An app that wants landscape draws nothing on a portrait screen (Angry
  // Birds), and the 6.0/8.1 stacks do not turn the screen: the page starts
  // them on a landscape one.
  function launcherOrientation(x) {
    var strings = [], ids = [];
    var activity = null, orientation = null, inActivity = false, main = false, launcher = false;
    var p = u16(x, 2);
    while (p < x.length) {
      var type = u16(x, p), headerSize = u16(x, p + 2), size = u32(x, p + 4);
      if (!size) break;
      if (type === 0x0001) {
        strings = [];
        var count = u32(x, p + 8), flags = u32(x, p + 16), start = u32(x, p + 20);
        var utf8 = (flags & 0x100) !== 0;
        for (var i = 0; i < count; i++) {
          var q = p + start + u32(x, p + headerSize + i * 4);
          if (utf8) {
            q += (x[q] & 0x80) ? 2 : 1;
            var n = x[q]; q++;
            if (n & 0x80) { n = ((n & 0x7f) << 8) | x[q]; q++; }
            strings.push(new TextDecoder().decode(x.subarray(q, q + n)));
          } else {
            var m = u16(x, q); q += 2;
            var t = '';
            for (var k = 0; k < m; k++) t += String.fromCharCode(u16(x, q + 2 * k));
            strings.push(t);
          }
        }
      } else if (type === 0x0180) {  // resource ids of the attribute names
        ids = [];
        for (var j = 8; j < size; j += 4) ids.push(u32(x, p + j));
      } else if (type === 0x0102 || type === 0x0103) {
        var name = strings[u32(x, p + 20)];
        if (type === 0x0103) {
          if (name === 'activity') {
            if (main && launcher && orientation === null) orientation = activity;
            inActivity = false;
          }
        } else {
          var attrStart = u16(x, p + 24), attrSize = u16(x, p + 26), attrCount = u16(x, p + 28);
          var attrs = {};
          for (var a = 0; a < attrCount; a++) {
            var at = p + 16 + attrStart + a * attrSize;
            var ni = u32(x, at + 4), raw = u32(x, at + 8);
            var value = raw !== 0xffffffff ? strings[raw] : u32(x, at + 16);
            attrs[ids[ni] === 0x0101001e ? 'screenOrientation' : strings[ni]] = value;
            if (ids[ni] === 0x01010003) attrs.name = value;  // android:name
          }
          if (name === 'activity') {
            inActivity = true; main = false; launcher = false;
            var o = attrs.screenOrientation;
            activity = [0, 6, 8, 11].indexOf(o) >= 0 ? 'landscape'
                     : [1, 7, 9, 12].indexOf(o) >= 0 ? 'portrait' : null;
          } else if (inActivity && name === 'action' && attrs.name === 'android.intent.action.MAIN') {
            main = true;
          } else if (inActivity && name === 'category' &&
                     attrs.name === 'android.intent.category.LAUNCHER') {
            launcher = true;
          }
        }
      }
      p += size;
    }
    return orientation;
  }

  function usesSdk(x, p, strings) {
    while (p < x.length) {
      const type = u16(x, p), size = u32(x, p + 4);
      if (type === 0x0102 && strings[u32(x, p + 20)] === 'uses-sdk') {
        const attrStart = u16(x, p + 24), attrSize = u16(x, p + 26), attrCount = u16(x, p + 28);
        const out = {};
        for (let i = 0; i < attrCount; i++) {
          const a = p + 16 + attrStart + i * attrSize;
          const key = strings[u32(x, a + 4)];
          const raw = u32(x, a + 8);
          out[key] = raw !== 0xffffffff ? +strings[raw] : u32(x, a + 16);
        }
        return out;
      }
      p += size;
    }
    return {};
  }

  /** {package, versionCode, versionName, minSdk, targetSdk, abis} of an APK's bytes. */
  async function inspect(bytes) {
    const all = entries(bytes);
    const m = all.get('AndroidManifest.xml');
    if (!m) throw new Error('no AndroidManifest.xml: not an APK');
    const xml = await read(bytes, m);
    const {manifest, x, p, strings} = manifestAttributes(xml);
    const sdk = usesSdk(x, p, strings);
    const abis = new Set();
    for (const name of all.keys()) {
      const a = /^lib\/([^/]+)\/[^/]+\.so$/.exec(name);
      if (a) abis.add(a[1]);
    }
    return {
      package: manifest.package, versionCode: manifest.versionCode >>> 0,
      versionName: typeof manifest.versionName === 'string' ? manifest.versionName : '',
      minSdk: sdk.minSdkVersion || 1, targetSdk: sdk.targetSdkVersion || sdk.minSdkVersion || 1,
      abis: [...abis],
      orientation: launcherOrientation(xml),
    };
  }

  self.SensesApk = {inspect};
})();
