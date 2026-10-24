// The owner of the WebGPU device for arc-wasm's Vulkan driver (topic vulkan,
// docs/topics/vulkan.md): a --pre-js of the page builds. It runs on the
// page's main thread, which has navigator.gpu, the event loop WebGPU's
// promises need and the canvas; guest threads block in wasm and send it
// batches of operations (shim/vulkan/vk_gpu.h has the format and the op
// numbers, OPS below):
//
//   Module.arcVk.adapterState()   0 no WebGPU, 1 requestAdapter() pending, 2 an adapter
//   Module.arcVk.adapterText()    the adapter's limits and features, as lines
//   Module.arcVk.run(dev, ptr, n) runs a batch of n words at ptr in the wasm memory
//   Module.arcVk.place(id, show, ...)  where an overlay goes (the SurfaceFlinger, below)
//   Module.arcVk.capture(id, ptr, w, h) an overlay's last frame into the wasm memory
//
// The driver (C++) does Vulkan's semantics; here are WebGPU's objects by id,
// and what only WebGPU's side can know:
//
//   * shaders: SPIR-V -> WGSL with Tint (arc_tint.js/.wasm next to the VM,
//     tools/vulkan/build_tint.sh), loaded when a device is first made;
//   * bind group layouts: the pipeline's plan (Vulkan's set layouts, the
//     sampler halves of combined image samplers, push constants) joined
//     with the WGSL's declarations (texture sample types, storage access);
//   * bind groups: per descriptor set version and layout, cached;
//   * push constants: a ring of storage buffer slots, one per draw;
//   * render pipeline variants: per dynamic state the pipeline bakes in.
//
// Answers (fences, REPLY, PRESENT) are 1 stored into a word of the wasm
// memory, then Atomics.notify. ?vklog=1 on the page logs batches.

(function () {
  if (typeof window == 'undefined' || typeof document == 'undefined') return;

  var OPS = {
    CREATE_BUFFER: 1, CREATE_TEXTURE: 2, CREATE_VIEW: 3, CREATE_SAMPLER: 4, CREATE_SHADER: 5,
    CREATE_PIPELINE: 6, SET_DESCRIPTORS: 7, DESTROY: 8, WRITE_BUFFER: 9, SUBMIT: 10, SIGNAL: 11,
    READBACK: 12, PRESENT: 13, REPLY: 14, CREATE_QUERY_SET: 15, WRITE_TEXTURE: 16, LABEL: 17,
    PRESENT_OVERLAY: 18,
    REQUEST_DEVICE: 0xfffe, DEVICE_GONE: 0xffff,
    BEGIN_PASS: 100, END_PASS: 101, SET_PIPELINE: 102, SET_VIEWPORT: 103, SET_SCISSOR: 104,
    SET_BLEND_CONSTANT: 105, SET_STENCIL_REF: 106, SET_VERTEX_BUFFER: 107, SET_INDEX_BUFFER: 108,
    BIND_SET: 109, PUSH_CONSTANTS: 110, DRAW: 111, DRAW_INDEXED: 112, DRAW_INDIRECT: 113,
    DRAW_INDEXED_INDIRECT: 114, DISPATCH: 115, DISPATCH_INDIRECT: 116, COPY_BUFFER: 117,
    COPY_BUFFER_TO_TEXTURE: 118, COPY_TEXTURE_TO_BUFFER: 119, COPY_TEXTURE: 120, FILL_BUFFER: 121,
    UPDATE_BUFFER: 122, CLEAR_TEXTURE: 123, CLEAR_ATTACHMENTS: 124, BLIT: 125, DYNAMIC_STATE: 126,
    DEBUG_GROUP: 127, BEGIN_QUERY: 128, END_QUERY: 129, RESOLVE_QUERIES: 130, EXECUTE: 131,
  };
  var SAMPLER_OFFSET = 500, PUSH_BINDING = 999, PUSH_SLOT = 256, PUSH_RING = 4 << 20;
  var log = /[?&]vklog=1\b/.test(location.search);
  var warned = 0;
  function warn(text) {
    if (warned++ > 200) return;
    text = 'vulkan(webgpu): ' + text;
    if (Module['printErr']) Module['printErr'](text); else console.warn(text);
  }

  // ---- the adapter -----------------------------------------------------------------
  var adapter = null, adapterState = 0, adapterText = '';
  if (navigator.gpu && !/[?&]nowebgpu=1\b/.test(location.search)) {
    adapterState = 1;
    navigator.gpu.requestAdapter({ powerPreference: 'high-performance' }).then(function (a) {
      if (!a) {
        adapterState = 0;
        warn('requestAdapter(): none');
        return;
      }
      adapter = a;
      var lines = [];
      var info = a.info || {};
      ['vendor', 'architecture', 'device', 'description'].forEach(function (k) {
        if (info[k]) lines.push('info ' + k + ' ' + String(info[k]).replace(/\n/g, ' '));
      });
      for (var k in a.limits) {
        if (typeof a.limits[k] == 'number') lines.push('limit ' + k + ' ' + a.limits[k]);
      }
      a.features.forEach(function (f) { lines.push('feature ' + f); });
      if (info.isFallbackAdapter || a.isFallbackAdapter) lines.push('fallback');
      adapterText = lines.join('\n');
      adapterState = 2;
    }, function (e) {
      adapterState = 0;
      warn('requestAdapter() failed: ' + e);
    });
  }

  // ---- the wasm memory -----------------------------------------------------------------
  var lastBuffer = null, u8 = null, u32 = null, i32 = null, f32 = null;
  function views() {
    var b = wasmMemory.buffer;
    if (b !== lastBuffer) {
      lastBuffer = b;
      u8 = new Uint8Array(b);
      u32 = new Uint32Array(b);
      i32 = new Int32Array(b);
      f32 = new Float32Array(b);
    }
  }
  function answer(addr, value) {
    views();
    Atomics.store(i32, addr >> 2, value);
    Atomics.notify(i32, addr >> 2);
  }
  var decoder = new TextDecoder();
  // A string argument at word |i|: a byte length, then the bytes.
  function str(i) {
    var n = u32[i];
    return decoder.decode(u8.slice(4 * (i + 1), 4 * (i + 1) + n));
  }
  function strWords(i) { return 1 + ((u32[i] + 3) >> 2); }
  function fbits(w) {
    f32scratch[0] = 0;
    u32scratch[0] = w;
    return f32scratch[0];
  }
  var u32scratch = new Uint32Array(1), f32scratch = new Float32Array(u32scratch.buffer);

  // ---- Tint ---------------------------------------------------------------------------
  var tint = null, tintLoading = null;
  function loadTint() {
    if (tintLoading) return tintLoading;
    var url = Module['locateFile'] ? Module['locateFile']('arc_tint.js', '') : 'arc_tint.js';
    tintLoading = new Promise(function (resolve) {
      var s = document.createElement('script');
      s.src = url;
      s.onload = function () {
        window['ArcTint']({
          locateFile: function (p) { return Module['locateFile'] ? Module['locateFile'](p, '') : p; },
        }).then(function (m) {
          tint = m;
          resolve(m);
        }, function (e) {
          warn('Tint failed to start: ' + e);
          resolve(null);
        });
      };
      s.onerror = function () {
        warn('no ' + url + ': shaders cannot be translated (tools/vulkan/build_tint.sh)');
        resolve(null);
      };
      document.head.appendChild(s);
    });
    return tintLoading;
  }
  // SPIR-V words -> WGSL text (or throws with Tint's error).
  function translate(words, samplers) {
    var m = tint;
    var p = m._malloc(words.byteLength), q = m._malloc(16 * samplers.length + 4);
    m.HEAPU8.set(new Uint8Array(words.buffer, words.byteOffset, words.byteLength), p);
    for (var i = 0; i < samplers.length; i++) {
      for (var k = 0; k < 4; k++) m.HEAPU32[(q >> 2) + 4 * i + k] = samplers[i][k];
    }
    var r = m._arc_tint_spirv_to_wgsl(p, words.length, q, samplers.length);
    var text = m.UTF8ToString(r);
    m._arc_tint_free(r);
    m._free(p);
    m._free(q);
    if (text[0] != 'W') throw new Error(text.substring(1));
    return text.substring(1);
  }

  // The resources a WGSL module declares: "group:binding" -> {space, access, type}.
  // (Tint writes the numbers as u32 literals: @group(0u) @binding(1u).)
  var DECL = /@group\((\d+)u?\)\s*@binding\((\d+)u?\)\s*var(?:<\s*(\w+)\s*(?:,\s*(\w+)\s*)?>)?\s+(\w+)\s*:\s*([^;]+);/g;
  function reflect(wgsl) {
    var out = {};
    var m;
    DECL.lastIndex = 0;
    while ((m = DECL.exec(wgsl))) {
      out[m[1] + ':' + m[2]] = { space: m[3] || '', access: m[4] || '', name: m[5], type: m[6].trim() };
    }
    return out;
  }
  function viewDimension(t) {
    if (/_cube_array/.test(t)) return 'cube-array';
    if (/_cube/.test(t)) return 'cube';
    if (/_2d_array/.test(t)) return '2d-array';
    if (/_3d/.test(t)) return '3d';
    if (/_1d/.test(t)) return '1d';
    return '2d';
  }
  // A bind group layout entry for a WGSL declaration.
  function layoutEntry(binding, decl, visibility, dynamic, minSize) {
    var t = decl.type, e = { binding: binding, visibility: visibility };
    if (t == 'sampler') e.sampler = { type: 'filtering' };
    else if (t == 'sampler_comparison') e.sampler = { type: 'comparison' };
    else if (/^texture_storage_/.test(t)) {
      var sm = /<\s*(\w+)\s*,\s*(\w+)\s*>/.exec(t);
      e.storageTexture = { format: sm[1], access: sm[2] == 'read_write' ? 'read-write' : sm[2] == 'read' ? 'read-only' : 'write-only',
                           viewDimension: viewDimension(t) };
    } else if (/^texture_depth_multisampled/.test(t)) e.texture = { sampleType: 'depth', viewDimension: '2d', multisampled: true };
    else if (/^texture_depth/.test(t)) e.texture = { sampleType: 'depth', viewDimension: viewDimension(t) };
    else if (/^texture_/.test(t)) {
      var ms = /multisampled/.test(t);
      var kind = /<\s*i32/.test(t) ? 'sint' : /<\s*u32/.test(t) ? 'uint' : ms ? 'unfilterable-float' : 'float';
      e.texture = { sampleType: kind, viewDimension: ms ? '2d' : viewDimension(t), multisampled: ms };
    } else if (decl.space == 'uniform') e.buffer = { type: 'uniform', hasDynamicOffset: !!dynamic };
    else if (decl.space == 'storage') {
      e.buffer = { type: decl.access == 'read_write' ? 'storage' : 'read-only-storage', hasDynamicOffset: !!dynamic };
    } else return null;
    return e;
  }

  // ---- devices --------------------------------------------------------------------------
  var gpu = null, gpuRequest = null;  // one GPUDevice for the page (an adapter makes one)
  var devices = {};

  function Dev(id) {
    this.id = id;
    this.objs = new Map();
    this.readbacks = [];
    this.dummies = {};
    this.push = null;  // the push constant ring
    this.pushOffset = 0;
    this.staging = [];  // mapped staging buffers of finished presents, for reuse
  }

  function requestDevice(devId, stateAddr, desc) {
    if (!gpuRequest) {
      var want = { requiredFeatures: [], requiredLimits: {} };
      (desc.features || []).forEach(function (f) { if (adapter.features.has(f)) want.requiredFeatures.push(f); });
      for (var k in desc.limits || {}) {
        if (k in adapter.limits) want.requiredLimits[k] = adapter.limits[k];
      }
      gpuRequest = adapter.requestDevice(want).then(function (d) {
        gpu = d;
        d.addEventListener('uncapturederror', function (e) { warn('GPU error: ' + e.error.message); });
        d.lost.then(function (i) { warn('device lost: ' + i.message); });
        return d;
      });
      loadTint();
    }
    Promise.all([gpuRequest, loadTint()]).then(function () {
      devices[devId] = new Dev(devId);
      answer(stateAddr, 1);
    }, function (e) {
      warn('requestDevice() failed: ' + e);
      answer(stateAddr, -1);
    });
  }

  Dev.prototype.get = function (id) { return this.objs.get(id); };

  Dev.prototype.dummy = function (kind) {
    var d = this.dummies[kind];
    if (d) return d;
    // (As big as a uniform block may be: draws check the bound size against the shader's.)
    if (kind == 'buffer') d = gpu.createBuffer({ size: 65536, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.STORAGE });
    else if (kind == 'sampler') d = gpu.createSampler();
    else if (kind == 'comparison') d = gpu.createSampler({ compare: 'less' });
    else {
      // kind: "texture:<sampleType>:<dimension>"
      var p = kind.split(':'), sampleType = p[1], dim = p[2];
      var format = sampleType == 'depth' ? 'depth32float' : sampleType == 'sint' ? 'rgba8sint' :
                   sampleType == 'uint' ? 'rgba8uint' : 'rgba8unorm';
      var layers = dim == 'cube' || dim == 'cube-array' ? 6 : 1;
      var t = gpu.createTexture({ size: [1, 1, layers], format: format, usage: GPUTextureUsage.TEXTURE_BINDING,
                                  dimension: dim == '3d' ? '3d' : dim == '1d' ? '1d' : '2d' });
      d = t.createView({ dimension: dim });
    }
    this.dummies[kind] = d;
    return d;
  };

  // A view for binding (as Vulkan described it) or as an attachment (aspect all).
  Dev.prototype.view = function (id, attachment) {
    var v = this.objs.get(id);
    if (!v) return null;
    if (attachment) {
      if (!v.attach) {
        var d = Object.assign({}, v.desc, { aspect: 'all', dimension: '2d', mipLevelCount: 1, arrayLayerCount: 1 });
        delete d.swizzle;
        v.attach = v.tex.texture.createView(d);
      }
      return v.attach;
    }
    if (!v.bind) {
      if (v.copy) {
        var cd = Object.assign({}, v.desc, { aspect: 'all' });
        delete cd.format;
        delete cd.emulate;
        v.bind = v.copy.texture.createView(cd);
      } else {
        v.bind = v.tex.texture.createView(v.desc);
      }
    }
    return v.bind;
  };

  // ---- swizzles without texture-component-swizzle (Safari 26, iOS 26) ------------------------
  // A view with a swizzle binds a copy of its image with the channels moved
  // (rgba8unorm, -srgb or rgba16float), drawn again after every write to the
  // image, in the same command list: copies, clears, blits, render passes into
  // it. (Unity's Alpha8 font atlases are R8 views with 000r: without the
  // swizzle, every glyph is a solid block.)
  function swizzleFormat(f) {
    if (!f || DEPTH_FORMAT.test(f) || /stencil|int/.test(f)) return null;
    if (/srgb/.test(f)) return 'rgba8unorm-srgb';
    if (/16|32|11b10|10a2/.test(f)) return 'rgba16float';
    return 'rgba8unorm';
  }

  Dev.prototype.addSwizzled = function (v) {
    var d = v.tex.desc, fmt = swizzleFormat(v.desc.format || d.format);
    if (!fmt || d.dimension == '3d' || (d.sampleCount || 1) > 1) {
      warn('image view swizzle ' + v.desc.emulate + ' of ' + d.format + ' ' + (d.dimension || '2d') + ': not emulated');
      return;
    }
    v.copy = { swizzle: v.desc.emulate, format: fmt,
               texture: gpu.createTexture({ size: d.size, mipLevelCount: d.mipLevelCount || 1, format: fmt,
                                            usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.RENDER_ATTACHMENT }) };
    (v.tex.swizzled = v.tex.swizzled || []).push(v);
    var enc = gpu.createCommandEncoder();
    this.drawSwizzled(enc, v, -1);
    gpu.queue.submit([enc.finish()]);
  };

  Dev.prototype.dropSwizzled = function (v) {
    var list = v.tex.swizzled, k = list ? list.indexOf(v) : -1;
    if (k >= 0) list.splice(k, 1);
    v.copy.texture.destroy();
    v.copy = null;
  };

  // Mip level |mip| (all of them: -1) of |v|'s copy from its image.
  Dev.prototype.drawSwizzled = function (enc, v, mip) {
    var c = v.copy, d = v.tex.desc, mips = d.mipLevelCount || 1, layers = d.size[2] || 1;
    var pipe = this.swizzlePipeline(c.swizzle, c.format);
    for (var m = mip < 0 ? 0 : mip; m < (mip < 0 ? mips : mip + 1); m++) {
      for (var l = 0; l < layers; l++) {
        var r = { dimension: '2d', baseMipLevel: m, mipLevelCount: 1, baseArrayLayer: l, arrayLayerCount: 1 };
        var bg = gpu.createBindGroup({ layout: pipe.getBindGroupLayout(0),
                                       entries: [{ binding: 0, resource: v.tex.texture.createView(r) }] });
        var pass = enc.beginRenderPass({ colorAttachments: [{ view: c.texture.createView(r), loadOp: 'clear', storeOp: 'store',
                                                              clearValue: { r: 0, g: 0, b: 0, a: 0 } }] });
        pass.setPipeline(pipe);
        pass.setBindGroup(0, bg);
        pass.draw(3);
        pass.end();
      }
    }
  };

  // After a write to mip level |mip| (-1: any) of image |tex| in command list |st|.
  Dev.prototype.swizzleAfter = function (st, tex, mip) {
    if (!tex || !tex.swizzled || !tex.swizzled.length) return;
    for (var k = 0; k < tex.swizzled.length; k++) this.drawSwizzled(st.enc, tex.swizzled[k], mip);
  };

  Dev.prototype.swizzlePipeline = function (swizzle, format) {
    var key = swizzle + ':' + format;
    this.swizzlePipes = this.swizzlePipes || {};
    if (this.swizzlePipes[key]) return this.swizzlePipes[key];
    var ch = swizzle.split('').map(function (s) { return s == '0' ? '0.0' : s == '1' ? '1.0' : 'c.' + s; });
    var m = gpu.createShaderModule({ code:
        '@group(0) @binding(0) var t : texture_2d<f32>;\n' +
        '@vertex fn vs(@builtin(vertex_index) i : u32) -> @builtin(position) vec4f {\n' +
        '  let p = vec2f(f32((i << 1u) & 2u), f32(i & 2u));\n' +
        '  return vec4f(p * 2.0 - 1.0, 0.0, 1.0);\n}\n' +
        '@fragment fn fs(@builtin(position) p : vec4f) -> @location(0) vec4f {\n' +
        '  let c = textureLoad(t, vec2i(p.xy), 0);\n' +
        '  return vec4f(' + ch.join(', ') + ');\n}\n' });
    return (this.swizzlePipes[key] = gpu.createRenderPipeline({
      layout: 'auto', vertex: { module: m, entryPoint: 'vs' },
      fragment: { module: m, entryPoint: 'fs', targets: [{ format: format }] }, primitive: { topology: 'triangle-list' } }));
  };

  // ---- shaders and pipelines ------------------------------------------------------------------

  function Shader(desc, words) {
    this.desc = desc;
    this.words = words;
    this.wgsl = null;
    this.error = null;
  }
  Shader.prototype.text = function () {
    if (this.wgsl || this.error) return this.wgsl;
    if (!tint) return null;
    try {
      this.wgsl = translate(this.words, this.desc.samplers || []);
      this.decls = reflect(this.wgsl);
      if (log) console.log('vulkan(webgpu): ' + this.desc.stage + ' shader\n' + this.wgsl);
    } catch (e) {
      this.error = String(e.message || e);
      warn(this.desc.stage + ' shader: ' + this.error);
    }
    return this.wgsl;
  };

  var STAGE_BITS = { vertex: 1, fragment: 2, compute: 4 };

  // Makes the WebGPU side of pipeline |p| (its desc from the driver): the
  // shader modules, bind group layouts and pipeline layout. False if a
  // shader is not translated yet (Tint loading) or failed.
  Dev.prototype.preparePipeline = function (p) {
    if (p.ready) return true;
    if (p.failed) return false;
    var self = this, desc = p.desc, stages = [];
    ['vertex', 'fragment', 'compute'].forEach(function (s) {
      if (desc[s]) stages.push({ name: s, shader: self.objs.get(desc[s].shader), entry: desc[s].entry });
    });
    for (var i = 0; i < stages.length; i++) {
      if (!stages[i].shader) { p.failed = true; return false; }
      if (!stages[i].shader.text()) {
        if (stages[i].shader.error) p.failed = true;
        return false;
      }
    }
    // Storage buffers: read-write where any stage writes them (fragment and
    // compute may), read-only where the vertex stage sees them.
    var access = {};
    stages.forEach(function (st) {
      var d = st.shader.decls;
      for (var k in d) if (d[k].space == 'storage' && d[k].access == 'read_write') access[k] = 'read_write';
    });
    stages.forEach(function (st) {
      var d = st.shader.decls, text = st.shader.wgsl;
      if (st.name == 'vertex') {
        st.module = text;
        return;
      }
      for (var k in d) {
        if (d[k].space == 'storage' && d[k].access != 'read_write' && access[k]) {
          var g = k.split(':');
          text = text.replace(new RegExp('(@group\\(' + g[0] + 'u?\\)\\s*@binding\\(' + g[1] + 'u?\\)\\s*var<\\s*storage)\\s*(,\\s*read\\s*)?>'),
                              '$1, read_write>');
        }
      }
      st.module = text;
    });
    // Bind group layouts: the driver's plan, with the types the WGSL declares.
    var groups = desc.groups || [];
    p.bgls = [];
    p.bglKeys = [];
    p.groupEntries = [];
    for (var g = 0; g < groups.length; g++) {
      var entries = [], plan = [];
      var dynIndex = 0;
      groups[g].forEach(function (b) {
        var bindings = [b.binding];
        if (b.kind == 'combined') bindings.push(b.samplerBinding);
        var dyn = b.dynamic ? dynIndex++ : -1;
        bindings.forEach(function (bn) {
          var vis = 0, decl = null;
          stages.forEach(function (st) {
            var d = st.shader.decls[g + ':' + bn];
            if (d) {
              decl = decl || d;
              vis |= STAGE_BITS[st.name];
            }
          });
          if (!decl) return;  // unused by this pipeline
          if (access[g + ':' + bn] && decl.space == 'storage') decl = Object.assign({}, decl, { access: 'read_write' });
          if (decl.space == 'storage' && decl.access == 'read_write' && (vis & 1)) {
            warn('a storage buffer the vertex stage sees is written (set ' + g + ', binding ' + bn + ')');
          }
          var e = layoutEntry(bn, decl, vis, b.dynamic || b.kind == 'push', 0);
          if (!e) return;
          // A depth image read as an ordinary texture (sampler2D on a depth buffer):
          // WebGPU binds depth formats as 'unfilterable-float' only, with
          // non-filtering samplers (unfilterable(), at the draw that found it).
          if (p.unfilterable && p.unfilterable[g + ':' + b.binding]) {
            if (e.texture && e.texture.sampleType == 'float') e.texture.sampleType = 'unfilterable-float';
            if (e.sampler && e.sampler.type == 'filtering') e.sampler.type = 'non-filtering';
          }
          entries.push(e);
          plan.push({ binding: bn, vk: b.binding, kind: b.kind, sampler: bn != b.binding, dyn: dyn, entry: e,
                      push: b.kind == 'push', size: b.size });
        });
      });
      // The push constants' binding (kind "push") is declared by the stages that use it.
      entries.sort(function (a, b) { return a.binding - b.binding; });
      plan.sort(function (a, b) { return a.binding - b.binding; });
      p.groupEntries.push(plan);
      p.bgls.push(gpu.createBindGroupLayout({ entries: entries }));
      p.bglKeys.push(JSON.stringify(entries));
    }
    p.layout = gpu.createPipelineLayout({ bindGroupLayouts: p.bgls });
    p.stages = {};
    stages.forEach(function (st) {
      var key = st.module;
      var m = self.moduleCache && self.moduleCache.get(key);
      if (!m) {
        self.moduleCache = self.moduleCache || new Map();
        m = gpu.createShaderModule({ code: st.module });
        self.moduleCache.set(key, m);
        if (m.getCompilationInfo) {
          m.getCompilationInfo().then(function (info) {
            info.messages.forEach(function (msg) {
              if (msg.type == 'error') warn(st.name + ' WGSL: ' + msg.message + ' (line ' + msg.lineNum + ')');
            });
          });
        }
      }
      p.stages[st.name] = { module: m, entryPoint: stageEntry(st.module, st.name, st.entry), text: st.module };
    });
    p.ready = true;
    return true;
  };

  // The entry point of |stage| in WGSL |text|: Tint keeps the SPIR-V name
  // |wanted| unless it is not a WGSL identifier (or another function has it).
  function stageEntry(text, stage, wanted) {
    var re = new RegExp('@' + stage + '\\b[^{]*?\\bfn\\s+(\\w+)\\s*\\(', 'g'), m, first;
    while ((m = re.exec(text))) {
      if (m[1] == wanted) return wanted;
      first = first || m[1];
    }
    return first;
  }

  // The vertex module of |p| without the y flip (made when a negative viewport
  // height first needs it), or null; its entry point in p.unflippedEntry.
  Dev.prototype.unflipped = function (p) {
    if (p.unflippedModule !== undefined) return p.unflippedModule;
    var sh = p.desc.vertex && p.desc.vertex.unflipped && this.objs.get(p.desc.vertex.unflipped);
    var text = sh && sh.text();
    p.unflippedModule = text ? gpu.createShaderModule({ code: text }) : null;
    p.unflippedEntry = text ? stageEntry(text, 'vertex', p.desc.vertex.entry) : undefined;
    if (text && p.unflippedEntry != p.desc.vertex.entry && !this.warnedEntry) {
      this.warnedEntry = true;
      warn('vertex entry point "' + p.desc.vertex.entry + '" is "' + p.unflippedEntry + '" in WGSL:\n' + text.substring(0, 1000));
    }
    return p.unflippedModule;
  };

  // The GPURenderPipeline of |p| for the current dynamic state.
  Dev.prototype.renderPipeline = function (p, st) {
    if (!this.preparePipeline(p)) return null;
    var d = p.desc, dyn = d.dynamic || 0;
    var strip = /strip/.test(d.primitive.topology) && st.indexed ? st.indexFormat : undefined;
    var flip = st.flipped && this.unflipped(p);
    var key = (flip ? 'u' : '') + (strip || '') + '|' + ((dyn & (1 << 4)) ? st.depthBias.join(',') : '') + '|' +
              ((dyn & ((1 << 6) | (1 << 7))) ? st.stencilMasks.join(',') : '');
    p.variants = p.variants || new Map();
    var rp = p.variants.get(key);
    if (rp) return rp;
    var buffers = (d.buffers || []).map(function (b) {
      return b ? { arrayStride: b.arrayStride, stepMode: b.stepMode, attributes: b.attributes } :
                 { arrayStride: 0, attributes: [] };
    });
    var desc = {
      layout: p.layout,
      vertex: { module: flip ? p.unflippedModule : p.stages.vertex.module,
                entryPoint: flip ? p.unflippedEntry : p.stages.vertex.entryPoint, buffers: buffers },
      primitive: { topology: d.primitive.topology, frontFace: d.primitive.frontFace || 'ccw',
                   cullMode: d.primitive.cullMode || 'none' },
      multisample: { count: d.multisample.count || 1, alphaToCoverageEnabled: !!d.multisample.alphaToCoverageEnabled },
    };
    if (d.multisample.mask !== undefined) desc.multisample.mask = d.multisample.mask >>> 0;
    if (strip) desc.primitive.stripIndexFormat = strip;
    if (d.primitive.unclippedDepth) desc.primitive.unclippedDepth = true;
    if (p.stages.fragment && !d.discard) {
      desc.fragment = {
        module: p.stages.fragment.module, entryPoint: p.stages.fragment.entryPoint,
        targets: (d.colors || []).map(function (f, i) {
          if (!f) return null;
          var t = d.targets[i] || {};
          var out = { format: f, writeMask: t.writeMask === undefined ? 0xf : t.writeMask };
          if (t.blend) out.blend = t.blend;
          return out;
        }),
      };
    } else if (d.colors && d.colors.length) {
      // No fragment stage (or rasterizer discard): nothing is written.
      desc.fragment = undefined;
    }
    if (d.depthStencil) {
      var ds = d.depthStencil;
      var hasStencil = /stencil/.test(ds.format);
      desc.depthStencil = { format: ds.format, depthWriteEnabled: !!ds.depthWriteEnabled,
                            depthCompare: ds.depthCompare || 'always' };
      if (hasStencil) {
        desc.depthStencil.stencilFront = ds.stencilFront;
        desc.depthStencil.stencilBack = ds.stencilBack;
        var masks = (dyn & ((1 << 6) | (1 << 7))) ? st.stencilMasks : [ds.stencilReadMask, 0, ds.stencilWriteMask, 0];
        desc.depthStencil.stencilReadMask = (dyn & (1 << 6)) ? masks[0] : ds.stencilReadMask;
        desc.depthStencil.stencilWriteMask = (dyn & (1 << 7)) ? masks[2] : ds.stencilWriteMask;
      }
      if (ds.biasEnabled || (dyn & (1 << 4))) {
        var bias = (dyn & (1 << 4)) ? st.depthBias : [ds.depthBias || 0, ds.depthBiasClamp || 0, ds.depthBiasSlopeScale || 0];
        if (/^triangle/.test(d.primitive.topology)) {
          desc.depthStencil.depthBias = Math.round(bias[0]);
          desc.depthStencil.depthBiasClamp = bias[1];
          desc.depthStencil.depthBiasSlopeScale = bias[2];
        }
      }
    }
    gpu.pushErrorScope('validation');
    rp = gpu.createRenderPipeline(desc);
    gpu.popErrorScope().then(function (e) {
      if (!e) return;
      // Which entry points the modules have (Tint renames some).
      var eps = ['vertex', 'fragment'].map(function (s) {
        var st = p.stages[s], text = st && st.text;
        var list = [], m, re = /@(vertex|fragment|compute)[^\n]*?\bfn\s+(\w+)/g;
        while (text && (m = re.exec(text))) list.push(m[1] + ' ' + m[2]);
        return s + ' [' + (st ? st.entryPoint : '-') + '] ' + list.join(',');
      });
      warn('render pipeline: ' + e.message + ' (' + eps.join('; ') + ')');
    });
    p.variants.set(key, rp);
    return rp;
  };

  Dev.prototype.computePipeline = function (p) {
    if (p.gpu) return p.gpu;
    if (!this.preparePipeline(p)) return null;
    gpu.pushErrorScope('validation');
    p.gpu = gpu.createComputePipeline({ layout: p.layout,
                                        compute: { module: p.stages.compute.module, entryPoint: p.stages.compute.entryPoint } });
    gpu.popErrorScope().then(function (e) { if (e) warn('compute pipeline: ' + e.message); });
    return p.gpu;
  };

  // The bind group of pipeline |p|'s group |g| for descriptor set |set| (may be
  // null: a group of push constants only), and its dynamic offsets.
  Dev.prototype.bindGroup = function (p, g, set, dynamic, pushOffset) {
    var key = p.bglKeys[g];
    var cache = set ? set.groups : (this.pushGroups = this.pushGroups || new Map());
    var bg = cache.get(key);
    var self = this;
    if (!bg) {
      var entries = [];
      p.groupEntries[g].forEach(function (pl) {
        var e = pl.entry, res = null;
        var d = set && set.byBinding[pl.vk];
        if (pl.push) {
          res = { buffer: self.pushRing(), offset: 0, size: Math.max(16, pl.size || 16) };
        } else if (e.buffer) {
          var b = d && d.buffer && self.objs.get(d.buffer);
          res = b ? { buffer: b, offset: d.offset, size: d.size } : { buffer: self.dummy('buffer') };
          // (VK_WHOLE_SIZE of a big buffer as a uniform buffer: WebGPU's limit.)
          if (b && e.buffer.type == 'uniform') res.size = Math.min(res.size, gpu.limits.maxUniformBufferBindingSize);
        } else if (e.sampler) {
          var s = d && d.sampler && self.objs.get(d.sampler);
          if (s && e.sampler.type == 'non-filtering') s = self.nearest(s);
          res = s || self.dummy(e.sampler.type == 'comparison' ? 'comparison' : 'sampler');
        } else if (e.texture || e.storageTexture) {
          var v = d && d.view && self.view(d.view, false);
          var t = e.texture || {};
          res = v || self.dummy('texture:' + (t.sampleType || 'float') + ':' + ((e.texture || e.storageTexture).viewDimension || '2d'));
        }
        entries.push({ binding: pl.binding, resource: res });
      });
      gpu.pushErrorScope('validation');
      bg = gpu.createBindGroup({ layout: p.bgls[g], entries: entries });
      gpu.popErrorScope().then(function (e) { if (e) warn('bind group (set ' + g + '): ' + e.message); });
      cache.set(key, bg);
    }
    var offsets = [];
    p.groupEntries[g].forEach(function (pl) {
      if (pl.push) offsets.push(pushOffset);
      else if (pl.entry.buffer && pl.entry.buffer.hasDynamicOffset) offsets.push(dynamic[pl.dyn] || 0);
    });
    return { group: bg, offsets: offsets };
  };

  Dev.prototype.pushRing = function () {
    if (!this.push) {
      this.push = gpu.createBuffer({ size: PUSH_RING, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST });
      this.pushData = new Uint8Array(PUSH_RING);
    }
    return this.push;
  };

  // ---- internal pipelines: blits and clears --------------------------------------------------------

  var BLIT_WGSL =
      'struct V { @builtin(position) p : vec4f, @location(0) uv : vec2f };\n' +
      '@group(0) @binding(0) var t : texture_2d<f32>;\n' +
      '@group(0) @binding(1) var s : sampler;\n' +
      '@group(0) @binding(2) var<uniform> box : vec4f;\n' +
      '@vertex fn vs(@builtin(vertex_index) i : u32) -> V {\n' +
      '  var q = array<vec2f, 3>(vec2f(0, 0), vec2f(2, 0), vec2f(0, 2));\n' +
      '  var o : V; o.p = vec4f(q[i].x * 2 - 1, 1 - q[i].y * 2, 0, 1);\n' +
      '  o.uv = box.xy + q[i] * (box.zw - box.xy); return o; }\n' +
      '@fragment fn fs(v : V) -> @location(0) vec4f { return textureSampleLevel(t, s, v.uv, 0); }\n';
  var CLEAR_WGSL =
      'struct C { color : array<vec4f, 8>, depth : vec4f };\n' +
      '@group(0) @binding(0) var<uniform> c : C;\n' +
      '@vertex fn vs(@builtin(vertex_index) i : u32) -> @builtin(position) vec4f {\n' +
      '  var q = array<vec2f, 3>(vec2f(-1, -1), vec2f(3, -1), vec2f(-1, 3));\n' +
      '  return vec4f(q[i], c.depth.x, 1); }\n';

  // Buffer copies at byte granularity (WebGPU copies whole words): the
  // source's words into a scratch buffer, then a compute pass writes each
  // destination word, keeping its bytes outside the range.
  var COPY_WGSL =
      '@group(0) @binding(0) var<storage, read> src : array<u32>;\n' +
      '@group(0) @binding(1) var<storage, read_write> dst : array<u32>;\n' +
      '@group(0) @binding(2) var<uniform> c : vec4u;\n' +  // src byte, dst byte, bytes, first word
      '@compute @workgroup_size(64) fn main(@builtin(global_invocation_id) g : vec3u) {\n' +
      '  let w = c.w + g.x;\n' +
      '  let first = w * 4u;\n' +
      '  if (first >= c.y + c.z) { return; }\n' +
      '  var v = dst[w];\n' +
      '  for (var k = 0u; k < 4u; k++) {\n' +
      '    let b = first + k;\n' +
      '    if (b >= c.y && b < c.y + c.z) {\n' +
      '      let i = b - c.y + c.x;\n' +
      '      let s = (src[i >> 2u] >> ((i & 3u) * 8u)) & 0xffu;\n' +
      '      v = (v & ~(0xffu << (k * 8u))) | (s << (k * 8u));\n' +
      '    }\n' +
      '  }\n' +
      '  dst[w] = v;\n' +
      '}\n';

  Dev.prototype.copyUnaligned = function (st, src, srcOff, dst, dstOff, size) {
    if (!this.copyPipe) {
      this.copyPipe = gpu.createComputePipeline({ layout: 'auto',
                                                  compute: { module: gpu.createShaderModule({ code: COPY_WGSL }), entryPoint: 'main' } });
    }
    var s0 = srcOff & ~3, s1 = Math.min((srcOff + size + 3) & ~3, src.size);
    var tmp = gpu.createBuffer({ size: Math.max(4, (s1 - s0 + 3) & ~3), usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST });
    st.enc.copyBufferToBuffer(src, s0, tmp, 0, s1 - s0);
    // The destination binding starts at a 256-byte boundary (storage offsets).
    var align = gpu.limits.minStorageBufferOffsetAlignment || 256;
    var d0 = dstOff - dstOff % align, d1 = Math.min((dstOff + size + 3) & ~3, dst.size);
    var words = (d1 - (dstOff & ~3)) >> 2;
    var first = ((dstOff & ~3) - d0) >> 2;
    var ub = gpu.createBuffer({ size: 16, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
    gpu.queue.writeBuffer(ub, 0, new Uint32Array([srcOff - s0, dstOff - d0, size, first]));
    var bg = gpu.createBindGroup({ layout: this.copyPipe.getBindGroupLayout(0), entries: [
      { binding: 0, resource: { buffer: tmp } }, { binding: 1, resource: { buffer: dst, offset: d0, size: d1 - d0 } },
      { binding: 2, resource: { buffer: ub } }] });
    var pass = st.enc.beginComputePass();
    pass.setPipeline(this.copyPipe);
    pass.setBindGroup(0, bg);
    for (var done = 0; done < words; done += 64 * 65535) {
      if (done) {
        // (more than 4M words: the next part with its own first word)
        var ub2 = gpu.createBuffer({ size: 16, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
        gpu.queue.writeBuffer(ub2, 0, new Uint32Array([srcOff - s0, dstOff - d0, size, first + done]));
        pass.setBindGroup(0, gpu.createBindGroup({ layout: this.copyPipe.getBindGroupLayout(0), entries: [
          { binding: 0, resource: { buffer: tmp } }, { binding: 1, resource: { buffer: dst, offset: d0, size: d1 - d0 } },
          { binding: 2, resource: { buffer: ub2 } }] }));
      }
      pass.dispatchWorkgroups(Math.min(65535, Math.ceil((words - done) / 64)));
    }
    pass.end();
  };

  Dev.prototype.blitPipeline = function (format) {
    this.blits = this.blits || {};
    var p = this.blits[format];
    if (p) return p;
    var m = this.blitModule || (this.blitModule = gpu.createShaderModule({ code: BLIT_WGSL }));
    p = gpu.createRenderPipeline({ layout: 'auto', vertex: { module: m, entryPoint: 'vs' },
                                   fragment: { module: m, entryPoint: 'fs', targets: [{ format: format }] } });
    this.blits[format] = p;
    return p;
  };

  Dev.prototype.clearPipeline = function (formats, depthFormat, samples, mask, depth, stencil) {
    var key = formats.join(',') + '|' + depthFormat + '|' + samples + '|' + mask + '|' + depth + '|' + stencil;
    this.clears = this.clears || new Map();
    var p = this.clears.get(key);
    if (p) return p;
    var outs = [], targets = [];
    formats.forEach(function (f, i) {
      if (!f) {
        targets.push(null);
        return;
      }
      var kind = /sint/.test(f) ? 'vec4i' : /uint/.test(f) ? 'vec4u' : 'vec4f';
      outs.push('@location(' + i + ') o' + i + ' : ' + kind);
      targets.push({ format: f, writeMask: (mask >> i) & 1 ? 0xf : 0 });
    });
    var body = 'struct O { ' + (outs.length ? outs.join(', ') : '@location(0) unused : vec4f') + ' };\n' +
               '@fragment fn fs() -> O { var o : O; ' +
               formats.map(function (f, i) {
                 if (!f) return '';
                 var kind = /sint/.test(f) ? 'vec4i' : /uint/.test(f) ? 'vec4u' : 'vec4f';
                 return 'o.o' + i + ' = ' + kind + '(c.color[' + i + ']); ';
               }).join('') + 'return o; }\n';
    var m = gpu.createShaderModule({ code: CLEAR_WGSL + body });
    var desc = { layout: 'auto', vertex: { module: m, entryPoint: 'vs' },
                 fragment: { module: m, entryPoint: 'fs', targets: targets },
                 multisample: { count: samples } };
    if (depthFormat) {
      desc.depthStencil = { format: depthFormat, depthWriteEnabled: !!depth, depthCompare: 'always' };
      if (/stencil/.test(depthFormat)) {
        var face = { compare: 'always', passOp: stencil ? 'replace' : 'keep' };
        desc.depthStencil.stencilFront = face;
        desc.depthStencil.stencilBack = face;
        desc.depthStencil.stencilWriteMask = stencil ? 0xff : 0;
      }
    }
    p = gpu.createRenderPipeline(desc);
    this.clears.set(key, p);
    return p;
  };

  // ---- batches ----------------------------------------------------------------------------

  Dev.prototype.batch = function (ptr, n) {
    views();
    var i = ptr >> 2, end = i + n;
    while (i < end) {
      var h = u32[i], op = h & 0xffff, len = h >>> 16;
      if (!len) break;
      try {
        this.op(op, i + 1, len - 1);
      } catch (e) {
        warn('op ' + op + ': ' + (e && e.stack || e));
      }
      i += len;
    }
  };

  Dev.prototype.op = function (op, a, n) {
    var objs = this.objs, self = this;
    switch (op) {
      case OPS.CREATE_BUFFER:
        objs.set(u32[a], gpu.createBuffer({ size: u32[a + 1] + u32[a + 2] * 4294967296, usage: u32[a + 3] }));
        break;
      case OPS.CREATE_TEXTURE: {
        var d = JSON.parse(str(a + 1));
        var t = gpu.createTexture({ size: d.size, mipLevelCount: d.mipLevelCount, sampleCount: d.sampleCount,
                                    dimension: d.dimension, format: d.format, usage: d.usage });
        objs.set(u32[a], { texture: t, desc: d });
        break;
      }
      case OPS.CREATE_VIEW: {
        var vd = JSON.parse(str(a + 2));
        var tex = objs.get(u32[a + 1]);
        if (!tex) break;
        if (vd.swizzle == 'rgba') delete vd.swizzle;
        else if (vd.swizzle && !gpu.features.has('texture-component-swizzle')) {
          vd.emulate = vd.swizzle;  // (a swizzled copy: addSwizzled)
          delete vd.swizzle;
        }
        if (vd.dimension == 'cube' && vd.arrayLayerCount != 6) vd.dimension = '2d-array';
        if (vd.dimension == '2d' && vd.arrayLayerCount > 1) vd.dimension = '2d-array';
        var vo = { tex: tex, desc: vd };
        objs.set(u32[a], vo);
        if (vd.emulate) this.addSwizzled(vo);
        break;
      }
      case OPS.CREATE_SAMPLER: {
        var sdesc = JSON.parse(str(a + 1));
        var smp = gpu.createSampler(sdesc);
        smp.arcDesc = sdesc;
        objs.set(u32[a], smp);
        break;
      }
      case OPS.CREATE_SHADER: {
        var sd = JSON.parse(str(a + 1));
        var at = a + 1 + strWords(a + 1);
        var bytes = u32[at];
        objs.set(u32[a], new Shader(sd, u32.slice(at + 1, at + 1 + (bytes >> 2))));
        break;
      }
      case OPS.CREATE_PIPELINE:
        objs.set(u32[a], { desc: JSON.parse(str(a + 1)), pipeline: true });
        break;
      case OPS.SET_DESCRIPTORS: {
        var entries = JSON.parse(str(a + 1)), by = {};
        entries.forEach(function (e) { by[e.binding] = e; });
        objs.set(u32[a], { byBinding: by, groups: new Map(), set: true });
        break;
      }
      case OPS.DESTROY: {
        var o = objs.get(u32[a]);
        if (o) {
          if (o instanceof GPUBuffer) o.destroy();
          else if (o.texture && o.texture.destroy) o.texture.destroy();
          else if (o.copy) this.dropSwizzled(o);
          else if (o instanceof GPUQuerySet) o.destroy();
          objs.delete(u32[a]);
        }
        break;
      }
      case OPS.WRITE_BUFFER: {
        var buf = objs.get(u32[a]);
        if (buf) this.writeBuffer(buf, u32[a + 1] + u32[a + 2] * 4294967296, u32[a + 3], u32[a + 4]);
        break;
      }
      case OPS.SUBMIT:
        this.submit(a + 1, u32[a]);
        break;
      case OPS.SIGNAL: {
        var addr = u32[a], waits = this.readbacks;
        this.readbacks = [];
        waits.push(gpu.queue.onSubmittedWorkDone());
        Promise.all(waits).then(function () { answer(addr, 1); }, function (e) {
          warn('fence: ' + e);
          answer(addr, 1);
        });
        break;
      }
      case OPS.READBACK:
        this.readback(objs.get(u32[a]), u32[a + 1] + u32[a + 2] * 4294967296, u32[a + 3], u32[a + 4]);
        break;
      case OPS.PRESENT:
        this.present(objs.get(u32[a]), u32[a + 1], u32[a + 2], u32[a + 3], u32[a + 4], u32[a + 5]);
        break;
      case OPS.PRESENT_OVERLAY:
        presentOverlay(objs.get(u32[a]), u32[a + 1], u32[a + 2], u32[a + 3], u32[a + 4]);
        break;
      case OPS.REPLY:
        answer(u32[a], 1);
        break;
      case OPS.CREATE_QUERY_SET:
        objs.set(u32[a], gpu.createQuerySet({ type: 'occlusion', count: Math.max(1, u32[a + 2]) }));
        break;
      case OPS.LABEL:
        if (log) console.log('vulkan(webgpu): ' + str(a));
        break;
      case OPS.DEVICE_GONE:
        objs.forEach(function (v) {
          if (v instanceof GPUBuffer) v.destroy();
          else if (v.texture && v.texture.destroy) v.texture.destroy();
        });
        objs.clear();
        delete devices[this.id];
        break;
      default:
        warn('unknown op ' + op);
    }
  };

  Dev.prototype.writeBuffer = function (buf, offset, ptr, size) {
    if (!size) return;
    try {
      gpu.queue.writeBuffer(buf, offset, u8, ptr, size);
    } catch (e) {
      // Some browsers refuse views of shared memory here: a copy.
      gpu.queue.writeBuffer(buf, offset, u8.slice(ptr, ptr + size));
    }
  };

  Dev.prototype.readback = function (buf, offset, size, dst) {
    if (!buf || !size) return;
    var staging = gpu.createBuffer({ size: size, usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST });
    var enc = gpu.createCommandEncoder();
    enc.copyBufferToBuffer(buf, offset, staging, 0, size);
    gpu.queue.submit([enc.finish()]);
    this.readbacks.push(staging.mapAsync(GPUMapMode.READ).then(function () {
      views();
      u8.set(new Uint8Array(staging.getMappedRange()), dst);
      staging.unmap();
      staging.destroy();
    }));
  };

  // Presents so far, for measurements (Module.arcVk.stats()).
  var stats = { readback: 0, overlay: 0 };

  // A swapchain image into the window layer's buffer: RGBA rows, top first.
  Dev.prototype.present = function (tex, w, h, dst, stride, reply) {
    stats.readback++;
    if (!tex || !w || !h) {
      answer(reply, 1);
      return;
    }
    var bpr = (w * 4 + 255) & ~255, size = bpr * h;
    var staging = null;
    for (var k = 0; k < this.staging.length; k++) {
      if (this.staging[k].size == size) {
        staging = this.staging.splice(k, 1)[0];
        break;
      }
    }
    if (!staging) staging = gpu.createBuffer({ size: size, usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST });
    var enc = gpu.createCommandEncoder();
    enc.copyTextureToBuffer({ texture: tex.texture }, { buffer: staging, bytesPerRow: bpr }, [w, h, 1]);
    gpu.queue.submit([enc.finish()]);
    var self = this;
    staging.mapAsync(GPUMapMode.READ).then(function () {
      views();
      var src = new Uint8Array(staging.getMappedRange());
      var row = w * 4;
      if (bpr == row && stride == row) u8.set(src.subarray(0, row * h), dst);
      else for (var y = 0; y < h; y++) u8.set(src.subarray(y * bpr, y * bpr + row), dst + y * stride);
      staging.unmap();
      if (self.staging.length < 4) self.staging.push(staging); else staging.destroy();
      answer(reply, 1);
    }, function (e) {
      warn('present: ' + e);
      answer(reply, 1);
    });
  };

  // ---- command lists --------------------------------------------------------------------------

  Dev.prototype.submit = function (start, words) {
    var enc = gpu.createCommandEncoder();
    var st = {
      enc: enc, pass: null, cpass: null, passInfo: null, pipe: null, cpipe: null, sentPipeline: null,
      sets: [[], []], dirty: [0, 0], push: new Uint8Array(256), pushDirty: true,
      depthBias: [0, 0, 0], stencilMasks: [0xff, 0xff, 0xff, 0xff], indexed: false, indexFormat: 'uint16',
      pushUsed: 0, pushStart: this.pushOffset,
    };
    var i = start, end = start + words;
    while (i < end) {
      var h = u32[i], op = h & 0xffff, len = h >>> 16;
      if (!len) break;
      try {
        this.cmd(st, op, i + 1, len - 1, end);
      } catch (e) {
        warn('command ' + op + ': ' + (e && e.stack || e));
      }
      i += len;
    }
    if (st.pass) st.pass.end();
    if (st.cpass) st.cpass.end();
    // This list's push constants, written before it runs.
    if (st.pushUsed) {
      var from = st.pushStart;
      if (from + st.pushUsed <= PUSH_RING) gpu.queue.writeBuffer(this.push, from, this.pushData, from, st.pushUsed);
    }
    gpu.queue.submit([enc.finish()]);
  };

  Dev.prototype.endCompute = function (st) {
    if (st.cpass) {
      st.cpass.end();
      st.cpass = null;
    }
  };

  // A slot of the push ring with the current push constants.
  Dev.prototype.pushSlot = function (st) {
    this.pushRing();
    if (this.pushOffset + PUSH_SLOT > PUSH_RING) {
      // Wrapped: what this list wrote so far goes now.
      if (st.pushUsed) gpu.queue.writeBuffer(this.push, st.pushStart, this.pushData, st.pushStart, st.pushUsed);
      this.pushOffset = 0;
      st.pushStart = 0;
      st.pushUsed = 0;
    }
    var off = this.pushOffset;
    this.pushData.set(st.push, off);
    this.pushOffset += PUSH_SLOT;
    st.pushUsed = this.pushOffset - st.pushStart;
    return off;
  };

  Dev.prototype.bindAll = function (st, bp, p, encoder) {
    var groups = p.desc.groups || [];
    var pushOff = 0;
    if (p.desc.pushGroup >= 0 && (st.pushDirty || st.lastPushOff === undefined)) {
      st.lastPushOff = this.pushSlot(st);
      st.pushDirty = false;
    }
    pushOff = st.lastPushOff || 0;
    for (var g = 0; g < groups.length; g++) {
      var b = st.sets[bp][g];
      var set = b ? this.objs.get(b.id) : null;
      var r = this.bindGroup(p, g, set, b ? b.dynamic : [], pushOff);
      encoder.setBindGroup(g, r.group, r.offsets);
    }
  };

  var DEPTH_FORMAT = /^(depth|stencil)/;

  // A copy of sampler |s| without filtering (for unfilterable textures).
  Dev.prototype.nearest = function (s) {
    if (!s.arcDesc) return s;
    if (!s.arcNearest) {
      var d = Object.assign({}, s.arcDesc, { magFilter: 'nearest', minFilter: 'nearest', mipmapFilter: 'nearest', maxAnisotropy: 1 });
      s.arcNearest = gpu.createSampler(d);
    }
    return s.arcNearest;
  };

  // Whether a set bound for |p| binds a depth view where |p|'s layout wants
  // 'float': then that binding becomes unfilterable and |p| is made again.
  Dev.prototype.unfilterable = function (p, st, bp) {
    if (!p.ready) return;
    var changed = false, self = this;
    for (var g = 0; g < p.groupEntries.length; g++) {
      var b = st.sets[bp][g];
      var set = b ? this.objs.get(b.id) : null;
      if (!set) continue;
      p.groupEntries[g].forEach(function (pl) {
        var t = pl.entry.texture;
        if (!t || t.sampleType != 'float') return;
        var d = set.byBinding[pl.vk], v = d && d.view && self.objs.get(d.view);
        if (v && DEPTH_FORMAT.test(v.desc.format || v.tex.desc.format)) {
          p.unfilterable = p.unfilterable || {};
          p.unfilterable[g + ':' + pl.vk] = true;
          changed = true;
        }
      });
    }
    if (changed) {
      warn('a depth image read as a float texture: binding it unfilterable (pipeline made again)');
      p.ready = false;
      p.variants = null;
      p.gpu = null;
    }
  };

  Dev.prototype.draw = function (st, indexed) {
    var p = st.pipe;
    if (!p || !st.pass) return false;
    st.indexed = indexed;
    this.unfilterable(p, st, 0);
    var rp = this.renderPipeline(p, st);
    if (!rp) return false;
    if (st.sentPipeline !== rp) {
      st.pass.setPipeline(rp);
      st.sentPipeline = rp;
    }
    this.bindAll(st, 0, p, st.pass);
    return true;
  };

  Dev.prototype.dispatch = function (st) {
    var p = st.cpipe;
    if (!p) return false;
    this.unfilterable(p, st, 1);
    var cp = this.computePipeline(p);
    if (!cp) return false;
    if (!st.cpass) st.cpass = st.enc.beginComputePass();
    st.cpass.setPipeline(cp);
    this.bindAll(st, 1, p, st.cpass);
    return true;
  };

  // A triangle fan of |count| vertices as a list: indices 0, i, i + 1.
  Dev.prototype.drawFan = function (st, count, instances, first, firstInstance) {
    if (count < 3) return;
    var n = (count - 2) * 3;
    this.fans = this.fans || {};
    var buf = this.fans[count];
    if (!buf) {
      buf = gpu.createBuffer({ size: n * 4, usage: GPUBufferUsage.INDEX, mappedAtCreation: true });
      var ix = new Uint32Array(buf.getMappedRange());
      for (var i = 0; i < count - 2; i++) {
        ix[3 * i] = 0;
        ix[3 * i + 1] = i + 1;
        ix[3 * i + 2] = i + 2;
      }
      buf.unmap();
      this.fans[count] = buf;
    }
    st.pass.setIndexBuffer(buf, 'uint32');
    st.pass.drawIndexed(n, instances, 0, first, firstInstance);
    if (st.index) st.pass.setIndexBuffer.apply(st.pass, st.index);
  };

  function clamp(v, lo, hi) { return v < lo ? lo : v > hi ? hi : v; }

  Dev.prototype.cmd = function (st, op, a, n, end) {
    var objs = this.objs, self = this, j;
    switch (op) {
      case OPS.BEGIN_PASS: {
        this.endCompute(st);
        j = JSON.parse(str(a));
        var desc = { colorAttachments: [] };
        j.colors.forEach(function (c) {
          if (!c) {
            desc.colorAttachments.push(null);
            return;
          }
          var at = { view: self.view(c.view, true), loadOp: c.load, storeOp: c.store,
                     clearValue: { r: c.clear[0], g: c.clear[1], b: c.clear[2], a: c.clear[3] } };
          if (c.resolve >= 0) at.resolveTarget = self.view(c.resolve, true);
          desc.colorAttachments.push(at);
        });
        if (j.depth) {
          var dsa = { view: this.view(j.depth.view, true) };
          if (j.depth.hasDepth) {
            dsa.depthLoadOp = j.depth.depthLoad;
            dsa.depthStoreOp = j.depth.depthStore;
            dsa.depthClearValue = j.depth.clearDepth;
          }
          if (j.depth.hasStencil) {
            dsa.stencilLoadOp = j.depth.stencilLoad;
            dsa.stencilStoreOp = j.depth.stencilStore;
            dsa.stencilClearValue = j.depth.clearStencil;
          }
          desc.depthStencilAttachment = dsa;
        }
        // The occlusion query set the pass uses (its BEGIN_QUERY ops).
        for (var k = a + strWords(a); k < end;) {
          var h = u32[k], o = h & 0xffff, l = h >>> 16;
          if (!l || o == OPS.END_PASS) break;
          if (o == OPS.BEGIN_QUERY) {
            desc.occlusionQuerySet = objs.get(u32[k + 1]);
            break;
          }
          k += l;
        }
        st.pass = st.enc.beginRenderPass(desc);
        st.passInfo = j;
        st.sentPipeline = null;
        break;
      }
      case OPS.END_PASS:
        if (st.pass) st.pass.end();
        st.pass = null;
        // (Images with swizzled copies the pass drew into.)
        if (st.passInfo) st.passInfo.colors.forEach(function (c) {
          [c && c.view, c && c.resolve].forEach(function (id) {
            var v = id >= 0 && objs.get(id);
            if (v && v.tex && v.tex.swizzled) self.swizzleAfter(st, v.tex, v.desc.baseMipLevel);
          });
        });
        st.sentPipeline = null;
        break;
      case OPS.SET_PIPELINE: {
        var p = objs.get(u32[a]);
        if (!p) break;
        if (p.desc.kind == 'compute') st.cpipe = p; else st.pipe = p;
        break;
      }
      case OPS.SET_VIEWPORT: {
        if (!st.pass) break;
        var W = st.passInfo.width, H = st.passInfo.height;
        var x = fbits(u32[a]), y = fbits(u32[a + 1]), w = fbits(u32[a + 2]), hh = fbits(u32[a + 3]);
        var x0 = clamp(x, 0, W), y0 = clamp(y, 0, H), x1 = clamp(x + w, 0, W), y1 = clamp(y + hh, 0, H);
        st.pass.setViewport(x0, y0, Math.max(0, x1 - x0), Math.max(0, y1 - y0),
                            clamp(fbits(u32[a + 4]), 0, 1), clamp(fbits(u32[a + 5]), 0, 1));
        st.flipped = !!u32[a + 6];
        break;
      }
      case OPS.SET_SCISSOR: {
        if (!st.pass) break;
        var SW = st.passInfo.width, SH = st.passInfo.height;
        var sx = Math.min(u32[a], SW), sy = Math.min(u32[a + 1], SH);
        st.pass.setScissorRect(sx, sy, Math.min(u32[a + 2], SW - sx), Math.min(u32[a + 3], SH - sy));
        break;
      }
      case OPS.SET_BLEND_CONSTANT:
        if (st.pass) st.pass.setBlendConstant({ r: fbits(u32[a]), g: fbits(u32[a + 1]), b: fbits(u32[a + 2]), a: fbits(u32[a + 3]) });
        break;
      case OPS.SET_STENCIL_REF:
        if (st.pass) st.pass.setStencilReference(u32[a]);
        break;
      case OPS.DYNAMIC_STATE:
        st.depthBias = [fbits(u32[a]), fbits(u32[a + 1]), fbits(u32[a + 2])];
        st.stencilMasks = [u32[a + 3], u32[a + 4], u32[a + 5], u32[a + 6]];
        break;
      case OPS.SET_VERTEX_BUFFER: {
        var vb = objs.get(u32[a + 1]);
        if (st.pass && vb && u32[a + 3]) st.pass.setVertexBuffer(u32[a], vb, u32[a + 2], u32[a + 3]);
        break;
      }
      case OPS.SET_INDEX_BUFFER: {
        var ib = objs.get(u32[a]);
        st.indexFormat = u32[a + 1] == 32 ? 'uint32' : 'uint16';
        st.index = ib && u32[a + 3] ? [ib, st.indexFormat, u32[a + 2], u32[a + 3]] : null;
        if (st.pass && st.index) st.pass.setIndexBuffer.apply(st.pass, st.index);
        break;
      }
      case OPS.BIND_SET: {
        var dyn = [];
        for (var d = 0; d < u32[a + 3]; d++) dyn.push(u32[a + 4 + d]);
        st.sets[u32[a]][u32[a + 1]] = { id: u32[a + 2], dynamic: dyn };
        break;
      }
      case OPS.PUSH_CONSTANTS: {
        var off = u32[a], bytes = u32[a + 1];
        st.push.set(u8.subarray(4 * (a + 2), 4 * (a + 2) + Math.min(bytes, 256 - off)), off);
        st.pushDirty = true;
        break;
      }
      case OPS.DRAW:
        if (!this.draw(st, false)) break;
        if (st.pipe.desc.primitive.fan) this.drawFan(st, u32[a], u32[a + 1], u32[a + 2], u32[a + 3]);
        else st.pass.draw(u32[a], u32[a + 1], u32[a + 2], u32[a + 3]);
        break;
      case OPS.DRAW_INDEXED:
        if (this.draw(st, true)) st.pass.drawIndexed(u32[a], u32[a + 1], u32[a + 2], i32[a + 3], u32[a + 4]);
        break;
      case OPS.DRAW_INDIRECT:
      case OPS.DRAW_INDEXED_INDIRECT: {
        var indexedDraw = op == OPS.DRAW_INDEXED_INDIRECT;
        var ibuf = objs.get(u32[a]);
        if (!ibuf || !this.draw(st, indexedDraw)) break;
        for (var c = 0; c < u32[a + 2]; c++) {
          if (indexedDraw) st.pass.drawIndexedIndirect(ibuf, u32[a + 1] + c * u32[a + 3]);
          else st.pass.drawIndirect(ibuf, u32[a + 1] + c * u32[a + 3]);
        }
        break;
      }
      case OPS.DISPATCH:
        if (this.dispatch(st)) st.cpass.dispatchWorkgroups(u32[a], u32[a + 1], u32[a + 2]);
        break;
      case OPS.DISPATCH_INDIRECT: {
        var dbuf = objs.get(u32[a]);
        if (dbuf && this.dispatch(st)) st.cpass.dispatchWorkgroupsIndirect(dbuf, u32[a + 1]);
        break;
      }
      case OPS.COPY_BUFFER: {
        this.endCompute(st);
        var sb = objs.get(u32[a]), db = objs.get(u32[a + 2]), size = u32[a + 4];
        if (sb && db && size) {
          if ((size | u32[a + 1] | u32[a + 3]) & 3) this.copyUnaligned(st, sb, u32[a + 1], db, u32[a + 3], size);
          else st.enc.copyBufferToBuffer(sb, u32[a + 1], db, u32[a + 3], size);
        }
        break;
      }
      case OPS.COPY_BUFFER_TO_TEXTURE:
        this.endCompute(st);
        j = JSON.parse(str(a));
        this.copyToTexture(st, j);
        this.swizzleAfter(st, objs.get(j.texture), j.mipLevel);
        break;
      case OPS.COPY_TEXTURE_TO_BUFFER:
        this.endCompute(st);
        this.copyFromTexture(st, JSON.parse(str(a)));
        break;
      case OPS.COPY_TEXTURE: {
        this.endCompute(st);
        j = JSON.parse(str(a));
        var src = objs.get(j.src.texture), dst = objs.get(j.dst.texture);
        if (src && dst) {
          st.enc.copyTextureToTexture({ texture: src.texture, mipLevel: j.src.mipLevel, origin: j.src.origin, aspect: j.src.aspect },
                                      { texture: dst.texture, mipLevel: j.dst.mipLevel, origin: j.dst.origin, aspect: j.dst.aspect },
                                      j.size);
          this.swizzleAfter(st, dst, j.dst.mipLevel);
        }
        break;
      }
      case OPS.FILL_BUFFER: {
        this.endCompute(st);
        var fb = objs.get(u32[a]), fo = u32[a + 1], fs = u32[a + 2], value = u32[a + 3];
        if (!fb || !fs) break;
        if (value == 0) st.enc.clearBuffer(fb, fo, fs);
        else {
          var tmp = gpu.createBuffer({ size: fs, usage: GPUBufferUsage.COPY_SRC, mappedAtCreation: true });
          new Uint32Array(tmp.getMappedRange()).fill(value);
          tmp.unmap();
          st.enc.copyBufferToBuffer(tmp, 0, fb, fo, fs);
        }
        break;
      }
      case OPS.UPDATE_BUFFER: {
        this.endCompute(st);
        var ub = objs.get(u32[a]), uo = u32[a + 1], ul = u32[a + 2];
        if (!ub || !ul) break;
        var ut = gpu.createBuffer({ size: (ul + 3) & ~3, usage: GPUBufferUsage.COPY_SRC, mappedAtCreation: true });
        new Uint8Array(ut.getMappedRange()).set(u8.subarray(4 * (a + 3), 4 * (a + 3) + ul));
        ut.unmap();
        st.enc.copyBufferToBuffer(ut, 0, ub, uo, (ul + 3) & ~3);
        break;
      }
      case OPS.CLEAR_TEXTURE:
        this.endCompute(st);
        j = JSON.parse(str(a));
        this.clearTexture(st, j);
        this.swizzleAfter(st, objs.get(j.texture), -1);
        break;
      case OPS.CLEAR_ATTACHMENTS:
        this.clearAttachments(st, JSON.parse(str(a)));
        break;
      case OPS.BLIT:
        this.endCompute(st);
        j = JSON.parse(str(a));
        this.blit(st, j);
        this.swizzleAfter(st, objs.get(j.dst), j.dstMip);
        break;
      case OPS.BEGIN_QUERY:
        if (st.pass) st.pass.beginOcclusionQuery(u32[a + 1]);
        break;
      case OPS.END_QUERY:
        if (st.pass) st.pass.endOcclusionQuery();
        break;
      case OPS.RESOLVE_QUERIES: {
        this.endCompute(st);
        var qs = objs.get(u32[a]), rb = objs.get(u32[a + 3]);
        if (!qs || !rb) break;
        var count = u32[a + 2], dstOff = u32[a + 4];
        if (dstOff % 256 == 0) st.enc.resolveQuerySet(qs, u32[a + 1], count, rb, dstOff);
        else {
          var qt = gpu.createBuffer({ size: count * 8, usage: GPUBufferUsage.QUERY_RESOLVE | GPUBufferUsage.COPY_SRC });
          st.enc.resolveQuerySet(qs, u32[a + 1], count, qt, 0);
          st.enc.copyBufferToBuffer(qt, 0, rb, dstOff, count * 8);
        }
        break;
      }
      case OPS.DEBUG_GROUP:
        break;
      default:
        warn('unknown command ' + op);
    }
  };

  Dev.prototype.copyToTexture = function (st, j) {
    var buf = this.objs.get(j.buffer), tex = this.objs.get(j.texture);
    if (!buf || !tex) return;
    // Copy sizes in whole blocks (the last blocks of small mips are partial).
    var w = Math.ceil(j.size[0] / j.blockW) * j.blockW, h = Math.ceil(j.size[1] / j.blockH) * j.blockH;
    var dst = { texture: tex.texture, mipLevel: j.mipLevel, origin: j.origin, aspect: j.aspect };
    var rows = Math.ceil(j.size[1] / j.blockH);
    if (rows <= 1 && j.size[2] <= 1) {
      // One row: no bytesPerRow (WebGPU checks it against 256 whenever it is given).
      st.enc.copyBufferToTexture({ buffer: buf, offset: j.offset }, dst, [w, h, 1]);
    } else if (j.bytesPerRow % 256 == 0) {
      st.enc.copyBufferToTexture({ buffer: buf, offset: j.offset, bytesPerRow: j.bytesPerRow, rowsPerImage: j.rowsPerImage },
                                 dst, [w, h, j.size[2]]);
    } else if (j.host) {
      // Rows of another size: from the host-visible shadow, at the queue
      // (before this command list runs -- staging uploads are made so).
      views();
      gpu.queue.writeTexture(dst, u8, { offset: j.host, bytesPerRow: j.bytesPerRow, rowsPerImage: j.rowsPerImage },
                             [w, h, j.size[2]]);
    } else {
      // Device-local rows of another size: one copy per row of blocks.
      for (var z = 0; z < j.size[2]; z++) {
        for (var r = 0; r < rows; r++) {
          st.enc.copyBufferToTexture({ buffer: buf, offset: j.offset + (z * j.rowsPerImage + r) * j.bytesPerRow },
                                     { texture: tex.texture, mipLevel: j.mipLevel, aspect: j.aspect,
                                       origin: [j.origin[0], j.origin[1] + r * j.blockH, j.origin[2] + z] },
                                     [w, j.blockH, 1]);
        }
      }
    }
  };

  Dev.prototype.copyFromTexture = function (st, j) {
    var buf = this.objs.get(j.buffer), tex = this.objs.get(j.texture);
    if (!buf || !tex) return;
    var w = Math.ceil(j.size[0] / j.blockW) * j.blockW, h = Math.ceil(j.size[1] / j.blockH) * j.blockH;
    var src = { texture: tex.texture, mipLevel: j.mipLevel, origin: j.origin, aspect: j.aspect };
    var rows = Math.ceil(j.size[1] / j.blockH);
    if (rows <= 1 && j.size[2] <= 1) {
      st.enc.copyTextureToBuffer(src, { buffer: buf, offset: j.offset }, [w, h, 1]);
      return;
    }
    if (j.bytesPerRow % 256 == 0) {
      st.enc.copyTextureToBuffer(src, { buffer: buf, offset: j.offset, bytesPerRow: j.bytesPerRow, rowsPerImage: j.rowsPerImage },
                                 [w, h, j.size[2]]);
      return;
    }
    // Through a buffer with 256-byte rows, then row by row.
    var bpr = (j.bytesPerRow + 255) & ~255;
    var tmp = gpu.createBuffer({ size: bpr * rows * j.size[2], usage: GPUBufferUsage.COPY_SRC | GPUBufferUsage.COPY_DST });
    st.enc.copyTextureToBuffer(src, { buffer: tmp, bytesPerRow: bpr, rowsPerImage: rows }, [w, h, j.size[2]]);
    var row = Math.ceil(j.size[0] / j.blockW) * j.blockBytes;
    for (var z = 0; z < j.size[2]; z++) {
      for (var r = 0; r < rows; r++) {
        st.enc.copyBufferToBuffer(tmp, (z * rows + r) * bpr, buf, j.offset + (z * j.rowsPerImage + r) * j.bytesPerRow,
                                  (row + 3) & ~3);
      }
    }
  };

  Dev.prototype.clearTexture = function (st, j) {
    var tex = this.objs.get(j.texture);
    if (!tex) return;
    for (var m = j.baseMip; m < j.baseMip + j.mips; m++) {
      var layers = j.is3d ? Math.max(1, j.depthSize >> m) : j.layers;
      for (var l = 0; l < layers; l++) {
        var view = tex.texture.createView({ dimension: '2d', baseMipLevel: m, mipLevelCount: 1,
                                            baseArrayLayer: j.is3d ? 0 : j.baseLayer + l, arrayLayerCount: 1 });
        var desc;
        if (j.depthStencil) {
          var dsa = { view: view };
          if ((j.aspect & 2) || !/stencil/.test(j.format)) {
            dsa.depthLoadOp = (j.aspect & 2) ? 'clear' : 'load';
            dsa.depthStoreOp = 'store';
            dsa.depthClearValue = j.depth;
          }
          if (/stencil/.test(j.format)) {
            dsa.stencilLoadOp = (j.aspect & 4) ? 'clear' : 'load';
            dsa.stencilStoreOp = 'store';
            dsa.stencilClearValue = j.stencil;
          }
          if (j.format == 'stencil8') {
            delete dsa.depthLoadOp;
            delete dsa.depthStoreOp;
            delete dsa.depthClearValue;
          }
          desc = { colorAttachments: [], depthStencilAttachment: dsa };
        } else {
          desc = { colorAttachments: [{ view: view, loadOp: 'clear', storeOp: 'store',
                                        clearValue: { r: j.color[0], g: j.color[1], b: j.color[2], a: j.color[3] } }] };
        }
        st.enc.beginRenderPass(desc).end();
      }
    }
  };

  // vkCmdClearAttachments: a full-target triangle per rectangle, scissored.
  Dev.prototype.clearAttachments = function (st, j) {
    if (!st.pass) return;
    // The subpass's formats (BEGIN_PASS has them).
    var info = st.passInfo;
    var formats = info.formats, depthFormat = info.depthFormat || '';
    var samples = info.samples || 1;
    if (!formats) {
      if (!st.pipe) return;
      var d = st.pipe.desc;
      formats = d.colors || [];
      depthFormat = d.depthStencil ? d.depthStencil.format : '';
      samples = (d.multisample && d.multisample.count) || 1;
    }
    var mask = 0, depth = false, stencil = false, colors = new Float32Array(36);
    j.attachments.forEach(function (at) {
      if (at.color >= 0) {
        mask |= 1 << at.color;
        colors.set(at.value, 4 * at.color);
      }
      if (at.aspect & 2) {
        depth = true;
        colors[32] = at.depth;
      }
      if (at.aspect & 4) {
        stencil = true;
        st.pass.setStencilReference(at.stencil);
      }
    });
    var pipe = this.clearPipeline(formats, depthFormat, samples, mask, depth, stencil);
    var ub = gpu.createBuffer({ size: colors.byteLength, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
    gpu.queue.writeBuffer(ub, 0, colors);
    var bg = gpu.createBindGroup({ layout: pipe.getBindGroupLayout(0), entries: [{ binding: 0, resource: { buffer: ub } }] });
    st.pass.setPipeline(pipe);
    st.pass.setBindGroup(0, bg);
    st.sentPipeline = null;
    var W = st.passInfo.width, H = st.passInfo.height;
    st.pass.setViewport(0, 0, W, H, 0, 1);
    j.rects.forEach(function (r) {
      var x = clamp(r[0], 0, W), y = clamp(r[1], 0, H);
      st.pass.setScissorRect(x, y, Math.min(r[2], W - x), Math.min(r[3], H - y));
      st.pass.draw(3);
    });
  };

  Dev.prototype.blit = function (st, j) {
    var src = this.objs.get(j.src), dst = this.objs.get(j.dst);
    if (!src || !dst) return;
    if (j.resolve) {
      var rs = src.texture.createView({ dimension: '2d', baseMipLevel: j.srcMip, mipLevelCount: 1, baseArrayLayer: j.srcLayer, arrayLayerCount: 1 });
      var rd = dst.texture.createView({ dimension: '2d', baseMipLevel: j.dstMip, mipLevelCount: 1, baseArrayLayer: j.dstLayer, arrayLayerCount: 1 });
      st.enc.beginRenderPass({ colorAttachments: [{ view: rs, resolveTarget: rd, loadOp: 'load', storeOp: 'store' }] }).end();
      return;
    }
    if (/depth|stencil/.test(j.dstFormat)) {
      warn('blits of depth/stencil images');
      return;
    }
    var pipe = this.blitPipeline(j.dstFormat);
    var sv = src.texture.createView({ dimension: '2d', baseMipLevel: j.srcMip, mipLevelCount: 1, baseArrayLayer: j.srcLayer, arrayLayerCount: 1 });
    var dv = dst.texture.createView({ dimension: '2d', baseMipLevel: j.dstMip, mipLevelCount: 1, baseArrayLayer: j.dstLayer, arrayLayerCount: 1 });
    var sw = j.srcSize[0], sh = j.srcSize[1], sb = j.srcBox, db = j.dstBox;
    var box = new Float32Array([sb[0] / sw, sb[1] / sh, sb[2] / sw, sb[3] / sh]);
    // A flipped destination box: the source box flips instead.
    var x0 = db[0], x1 = db[2], y0 = db[1], y1 = db[3];
    if (x1 < x0) { var t = x0; x0 = x1; x1 = t; var u = box[0]; box[0] = box[2]; box[2] = u; }
    if (y1 < y0) { var t2 = y0; y0 = y1; y1 = t2; var v = box[1]; box[1] = box[3]; box[3] = v; }
    var ub = gpu.createBuffer({ size: 16, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
    gpu.queue.writeBuffer(ub, 0, box);
    this.blitSamplers = this.blitSamplers || {
      linear: gpu.createSampler({ magFilter: 'linear', minFilter: 'linear' }), nearest: gpu.createSampler(),
    };
    var sampler = /float|sint|uint/.test(j.srcFormat) && !/16float/.test(j.srcFormat) ? this.blitSamplers.nearest : this.blitSamplers[j.filter];
    var bg = gpu.createBindGroup({ layout: pipe.getBindGroupLayout(0), entries: [
      { binding: 0, resource: sv }, { binding: 1, resource: sampler }, { binding: 2, resource: { buffer: ub } }] });
    var pass = st.enc.beginRenderPass({ colorAttachments: [{ view: dv, loadOp: 'load', storeOp: 'store' }] });
    pass.setPipeline(pipe);
    pass.setBindGroup(0, bg);
    pass.setViewport(x0, y0, Math.max(1, x1 - x0), Math.max(1, y1 - y0), 0, 1);
    pass.draw(3);
    pass.end();
  };

  // ---- overlays (topic vulkan-present, docs/topics/vulkan-present.md) ---------------------
  //
  // A swapchain's window shown by the page: a WebGPU <canvas> over the
  // display canvas where the window's layer is, as a display controller
  // scans out an overlay plane. The SurfaceFlinger (shim/graphics/
  // layer_overlay.h) says where (place) and whether: only where nothing is
  // drawn above the layer; elsewhere the driver reads frames back into the
  // layer as before. A present copies the image into the canvas' texture on
  // the GPU, and is answered on the next animation frame (the driver keeps
  // one in flight).
  //
  // The layer: a box of the display's size in display pixels, laid over the
  // canvas as browser_input.js maps touches (as the WebView's layer,
  // shim/webview/webview_host.js); in it, per overlay, the logical display
  // (turned by Android's rotation onto the physical one), the visible part
  // and the frames' canvas. No pointer events: touches go to the display.
  var overlays = {};  // id -> {box, clip, canvas, ctx, place, frame, shown, waits}
  var olayer = null, olayerKey = '';

  function overlayLayer() {
    if (olayer) return olayer;
    olayer = document.createElement('div');
    olayer.className = 'arc-vulkan';
    // Above the display, below the WebViews (z 3) and the shell's overlays (5+).
    olayer.style.cssText = 'position:fixed;left:0;top:0;width:0;height:0;z-index:2;' +
        'pointer-events:none;transform-origin:50% 50%;overflow:visible;';
    document.body.appendChild(olayer);
    requestAnimationFrame(followOverlays);
    window.addEventListener('resize', placeOverlayLayer);
    return olayer;
  }

  function placeOverlayLayer() {
    var canvas = Module['canvas'];
    if (!olayer || !canvas) return;
    var r = canvas.getBoundingClientRect();
    var turn = (Module['arcwasmInput'] && Module['arcwasmInput'].turn) || 0;
    var cw = canvas.width || 1, ch = canvas.height || 1;
    var uw = turn ? r.height : r.width, uh = turn ? r.width : r.height;
    var key = [r.left, r.top, r.width, r.height, turn, cw, ch].join();
    if (key === olayerKey) return;
    olayerKey = key;
    var s = olayer.style;
    // (A canvas the page does not show: nothing over it either.)
    s.visibility = r.width > 0 && r.height > 0 ? 'visible' : 'hidden';
    s.width = cw + 'px';
    s.height = ch + 'px';
    s.left = (r.left + r.width / 2 - cw / 2) + 'px';
    s.top = (r.top + r.height / 2 - ch / 2) + 'px';
    s.transform = 'rotate(' + turn + 'deg) scale(' + uw / cw + ',' + uh / ch + ')';
  }

  // The canvas moves with the page's layout: followed every frame while
  // there are overlays.
  function followOverlays() {
    if (!olayer) return;
    placeOverlayLayer();
    for (var id in overlays) { requestAnimationFrame(followOverlays); return; }
    olayer.remove();
    olayer = null;
    olayerKey = '';
    window.removeEventListener('resize', placeOverlayLayer);
  }

  function overlay(id) {
    var o = overlays[id];
    if (o) return o;
    var box = document.createElement('div');
    box.style.cssText = 'position:absolute;left:0;top:0;width:0;height:0;transform-origin:0 0;visibility:hidden;';
    var clip = document.createElement('div');
    clip.style.cssText = 'position:absolute;overflow:hidden;left:0;top:0;width:0;height:0;';
    var canvas = document.createElement('canvas');
    canvas.style.cssText = 'position:absolute;display:block;';
    clip.appendChild(canvas);
    box.appendChild(clip);
    overlayLayer().appendChild(box);
    o = overlays[id] = { box: box, clip: clip, canvas: canvas, ctx: null, place: null, frame: false,
                         shown: false, waits: [] };
    return o;
  }

  function showOverlay(o) {
    var show = !!(o.place && o.place.show && o.frame);
    if (show === o.shown) return;
    o.shown = show;
    o.box.style.visibility = show ? 'visible' : 'hidden';
  }

  // id: the overlay; show: 1 shown, 0 not, -1 the window's surface is gone.
  // x, y, w, h: the frames on the logical display; cl..cb: what of them is
  // visible; rot: Surface.ROTATION_* of the logical display (lw x lh) onto
  // the physical one.
  function place(id, show, x, y, w, h, cl, ct, cr, cb, rot, lw, lh) {
    var o = overlays[id];
    if (show < 0) {
      if (o) {
        o.box.remove();
        o.waits.forEach(function (f) { f(); });
        delete overlays[id];
      }
      return;
    }
    if (!o) {
      if (!show) return;
      o = overlay(id);
    }
    if (!show) {
      o.place = { show: 0 };
      showOverlay(o);
      return;
    }
    // Back after being hidden: the frames went into the layer meanwhile, so
    // the canvas waits for a new one.
    if (!o.place || !o.place.show) o.frame = false;
    o.place = { show: 1, x: x, y: y, w: w, h: h, rot: rot };
    placeOverlayLayer();
    var bs = o.box.style;
    bs.width = lw + 'px';
    bs.height = lh + 'px';
    // Logical (x, y) -> physical: ROTATION_90 (lh - y, x), ROTATION_270 (y, lw - x),
    // as the composer turns its logical display.
    bs.transform = rot == 1 ? 'translate(' + lh + 'px,0) rotate(90deg)' :
                   rot == 3 ? 'translate(0,' + lw + 'px) rotate(-90deg)' : '';
    var cs = o.clip.style;
    cs.left = cl + 'px';
    cs.top = ct + 'px';
    cs.width = (cr - cl) + 'px';
    cs.height = (cb - ct) + 'px';
    var vs = o.canvas.style;
    vs.left = (x - cl) + 'px';
    vs.top = (y - ct) + 'px';
    vs.width = w + 'px';
    vs.height = h + 'px';
    showOverlay(o);
  }

  function presentOverlay(tex, w, h, id, reply) {
    stats.overlay++;
    var o = overlay(id);
    if (!tex || !w || !h) {
      answer(reply, 1);
      return;
    }
    try {
      if (o.canvas.width != w || o.canvas.height != h) {
        o.canvas.width = w;
        o.canvas.height = h;
      }
      if (!o.ctx) {
        o.ctx = o.canvas.getContext('webgpu');
        // (rgba8unorm: what the swapchain's formats copy into, -srgb included)
        o.ctx.configure({ device: gpu, format: 'rgba8unorm', alphaMode: 'opaque',
                          usage: GPUTextureUsage.COPY_DST | GPUTextureUsage.RENDER_ATTACHMENT });
      }
      var enc = gpu.createCommandEncoder();
      enc.copyTextureToTexture({ texture: tex.texture }, { texture: o.ctx.getCurrentTexture() }, [w, h, 1]);
      gpu.queue.submit([enc.finish()]);
      o.frame = true;
      o.last = tex.texture;
      showOverlay(o);
    } catch (e) {
      warn('present (overlay): ' + e);
    }
    // Paced by the display: answered on the next animation frame (or soon,
    // where the page draws none: a hidden tab).
    var done = false;
    function go() {
      if (done) return;
      done = true;
      clearTimeout(timer);
      var i = o.waits.indexOf(go);
      if (i >= 0) o.waits.splice(i, 1);
      answer(reply, 1);
    }
    var timer = setTimeout(go, 100);
    o.waits.push(go);
    requestAnimationFrame(go);
  }

  // The last frame (w x h RGBA rows) of overlay |id| into the wasm memory at
  // ptr, then 1 (or -1) into the word at |word|. (A WebGPU canvas cannot be
  // read once its frame is shown: the swapchain's image is, until the next
  // present.) False if there is none.
  function capture(id, ptr, w, h, word) {
    var o = overlays[id];
    if (!o || !o.frame || !o.last || o.last.width != w || o.last.height != h) return false;
    var bpr = (w * 4 + 255) & ~255;
    var staging = gpu.createBuffer({ size: bpr * h, usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST });
    var enc = gpu.createCommandEncoder();
    enc.copyTextureToBuffer({ texture: o.last }, { buffer: staging, bytesPerRow: bpr }, [w, h, 1]);
    gpu.queue.submit([enc.finish()]);
    staging.mapAsync(GPUMapMode.READ).then(function () {
      views();
      var src = new Uint8Array(staging.getMappedRange());
      for (var y = 0; y < h; y++) u8.set(src.subarray(y * bpr, y * bpr + w * 4), ptr + y * w * 4);
      staging.destroy();
      answer(word, 1);
    }, function (e) {
      warn('capture (overlay): ' + e);
      staging.destroy();
      answer(word, -1);
    });
    return true;
  }

  // ---- the entry ---------------------------------------------------------------------------

  Module['arcVk'] = {
    adapterState: function () { return adapterState; },
    adapterText: function () { return adapterText; },
    place: place,
    capture: capture,
    overlays: overlays,
    // false: every frame read back into the window's layer from now on
    // (the driver's switch), true: shown by the overlay where it can be.
    setOverlay: function (on) {
      views();
      i32[Module['_arcvk_overlay_switch']() >> 2] = on ? 1 : 0;
    },
    stats: function () { return { readback: stats.readback, overlay: stats.overlay, t: performance.now() }; },
    run: function (devId, ptr, words) {
      views();
      // A device request is the first op of its batch.
      var h = u32[ptr >> 2];
      if ((h & 0xffff) == OPS.REQUEST_DEVICE) {
        var a = (ptr >> 2) + 1;
        if (!adapter) {
          answer(u32[a], -1);
          return;
        }
        requestDevice(devId, u32[a], JSON.parse(str(a + 1)));
        return;
      }
      var dev = devices[devId];
      if (!dev) {
        warn('batch for unknown device ' + devId);
        return;
      }
      if (log) console.log('vulkan(webgpu): batch of ' + words + ' words for device ' + devId);
      dev.batch(ptr, words);
    },
  };
})();
