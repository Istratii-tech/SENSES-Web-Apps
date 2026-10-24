// Translation-shaped wasm modules for tools/jitmem (topic shared-translations,
// docs/topics/shared-translations.md): like shim/ndk/cpu/jit.cc, a function
// (cpu) -> next pc with 26 i32, 3 i64, 3 f32 and 3 f64 locals, importing the
// shared memory "e" "m" and a helper "e" "x". Function |i| has the same bytes
// in every thread (a block translated once), different from function i+1.
(function (root) {
  function u32(out, v) { do { var x = v & 0x7f; v >>>= 7; if (v) x |= 0x80; out.push(x); } while (v); }
  function s32(out, v) {
    for (;;) {
      var x = v & 0x7f; v >>= 7;
      var done = (v === 0 && !(x & 0x40)) || (v === -1 && (x & 0x40));
      if (!done) x |= 0x80;
      out.push(x);
      if (done) break;
    }
  }
  function section(out, id, body) { out.push(id); u32(out, body.length); for (var i = 0; i < body.length; i++) out.push(body[i]); }

  // One function body (locals + code), ~|ops| guest instructions' worth.
  function body(seed, ops) {
    var c = [];
    var rnd = seed * 2654435761 >>> 0;
    function r(n) { rnd = (rnd * 1103515245 + 12345) >>> 0; return (rnd >>> 8) % n; }
    function get(l) { c.push(0x20); u32(c, l); }
    function set(l) { c.push(0x21); u32(c, l); }
    function i32(v) { c.push(0x41); s32(c, v); }
    for (var reg = 1; reg <= 8; reg++) { get(0); c.push(0x28, 2); u32(c, 4 * reg); set(reg); }
    c.push(0x02, 0x40);   // block
    for (var k = 0; k < ops; k++) {
      var a = 1 + r(8), b = 1 + r(8), d = 1 + r(8);
      switch (r(5)) {
        case 0: get(a); get(b); c.push(0x6a); set(d); break;                        // add
        case 1: get(a); i32(r(4096) + seed); c.push(0x73); set(d); break;            // eor imm
        case 2: get(a); i32(r(31) + 1); c.push(0x74); set(d); break;                 // lsl
        case 3: get(0); get(a); i32(0xfff); c.push(0x71, 0x6a); c.push(0x28, 2, 64); set(d); break;   // ldr [cpu + (a & 0xfff) + 64]
        default: get(0); get(a); i32(0xfff); c.push(0x71, 0x6a); get(b); c.push(0x36, 2, 64); break; // str
      }
      if (r(6) === 0) { get(d); c.push(0x45, 0x0d, 0); }   // cbz: exit
    }
    c.push(0x0b);
    for (reg = 1; reg <= 8; reg++) { get(0); get(reg); c.push(0x36, 2); u32(c, 4 * reg); }
    i32(seed * 4 + 0x10000);
    c.push(0x0b);
    var b2 = [];
    u32(b2, 4); u32(b2, 25); b2.push(0x7f); u32(b2, 3); b2.push(0x7e); u32(b2, 3); b2.push(0x7d); u32(b2, 3); b2.push(0x7c);
    return b2.concat(c);
  }

  // A module with functions first .. first+n-1, exported "f0".., "f1"..;
  // |tbl|: none exported, an active element segment puts them into the
  // imported table "e" "t" at the imported global "e" "o" instead.
  function module(first, n, ops, shared, tbl) {
    var m = [0, 0x61, 0x73, 0x6d, 1, 0, 0, 0];
    var t = []; u32(t, 2);
    t.push(0x60, 1, 0x7f, 1, 0x7f);
    t.push(0x60, 2, 0x7f, 0x7f, 0);
    section(m, 1, t);
    var imp = []; u32(imp, tbl ? 4 : 2);
    u32(imp, 1); imp.push(101); u32(imp, 1); imp.push(109); imp.push(2, shared ? 3 : 1); u32(imp, 0); u32(imp, 65536);
    u32(imp, 1); imp.push(101); u32(imp, 1); imp.push(120); imp.push(0); u32(imp, 1);
    if (tbl) {
      u32(imp, 1); imp.push(101); u32(imp, 1); imp.push(116); imp.push(1, 0x70, 0); u32(imp, 0);   // table funcref min 0
      u32(imp, 1); imp.push(101); u32(imp, 1); imp.push(111); imp.push(3, 0x7f, 0);                // global i32 const
    }
    section(m, 2, imp);
    var f = []; u32(f, n); for (var i = 0; i < n; i++) u32(f, 0);
    section(m, 3, f);
    var e = [];
    if (!tbl) {
      u32(e, n);
      for (i = 0; i < n; i++) {
        var name = 'f' + i;
        u32(e, name.length); for (var j = 0; j < name.length; j++) e.push(name.charCodeAt(j));
        e.push(0); u32(e, i + 1);
      }
      section(m, 7, e);
    } else {
      e = []; u32(e, 1); e.push(0, 0x23, 0, 0x0b); u32(e, n);   // segment 0: table 0, offset global.get 0
      for (i = 0; i < n; i++) u32(e, i + 1);
      section(m, 9, e);
    }
    var code = []; u32(code, n);
    for (i = 0; i < n; i++) { var bd = body(first + i, ops); u32(code, bd.length); for (j = 0; j < bd.length; j++) code.push(bd[j]); }
    section(m, 10, code);
    return new Uint8Array(m);
  }

  // Driver: run(start, count, calls, cpu) call_indirects table slots
  // start..start+count-1, |calls| rounds (the interpreter's chaining).
  function driver(shared) {
    var m = [0, 0x61, 0x73, 0x6d, 1, 0, 0, 0];
    var t = []; u32(t, 2);
    t.push(0x60, 1, 0x7f, 1, 0x7f);
    t.push(0x60, 4, 0x7f, 0x7f, 0x7f, 0x7f, 0);
    section(m, 1, t);
    var imp = []; u32(imp, 1);
    u32(imp, 1); imp.push(101); u32(imp, 1); imp.push(116); imp.push(1, 0x70, 0); u32(imp, 0);
    section(m, 2, imp);
    section(m, 3, [1, 1]);
    section(m, 7, [1, 3, 114, 117, 110, 0, 0]);
    // locals: 4 params; 4: i, 5: c
    var c = [1, 2, 0x7f];
    c.push(0x02, 0x40, 0x03, 0x40);                 // block loop (rounds)
    c.push(0x20, 2, 0x45, 0x0d, 1);                 // calls == 0 -> exit
    c.push(0x41, 0, 0x21, 4);                       // i = 0
    c.push(0x02, 0x40, 0x03, 0x40);                 // block loop (slots)
    c.push(0x20, 4, 0x20, 1, 0x4f, 0x0d, 1);        // i >= count -> exit
    c.push(0x20, 3, 0x20, 0, 0x20, 4, 0x6a, 0x11, 0, 0, 0x1a);   // call_indirect (cpu) at start + i; drop
    c.push(0x20, 4, 0x41, 1, 0x6a, 0x21, 4, 0x0c, 0, 0x0b, 0x0b);
    c.push(0x20, 2, 0x41, 1, 0x6b, 0x21, 2, 0x0c, 0, 0x0b, 0x0b, 0x0b);
    var code = []; u32(code, 1); u32(code, c.length); code = code.concat(c);
    section(m, 10, code);
    return new Uint8Array(m);
  }

  root.jitmemModule = module;
  root.jitmemDriver = driver;
})(typeof globalThis !== 'undefined' ? globalThis : this);
