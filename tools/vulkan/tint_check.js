// Runs Tint (build/vulkan/tint/arc_tint.js) on SPIR-V files under Node and
// prints the WGSL, as the page's shader path does (topic vulkan): for
// checking the driver's SPIR-V pass (tests/vulkan/spirv_prepass_main.cc)
// and Tint's reading of real games' shaders without a browser.
//
//   node tools/vulkan/tint_check.js build/vulkan/tint/arc_tint.js A.spv [B.spv...]
//
// Combined image samplers get their sampler at binding + 500, as the driver
// asks (the bindings come from a scan of the module's decorations).
'use strict';
const fs = require('fs');
const path = require('path');

function combinedBindings(words) {
  // OpDecorate DescriptorSet/Binding, OpVariable UniformConstant of a sampled image type.
  const set = {}, binding = {}, ptr = {}, sampled = new Set(), arrays = {}, out = [];
  for (let i = 5; i < words.length;) {
    const n = words[i] >>> 16, op = words[i] & 0xffff;
    if (op == 71 && words[i + 2] == 34) set[words[i + 1]] = words[i + 3];
    if (op == 71 && words[i + 2] == 33) binding[words[i + 1]] = words[i + 3];
    if (op == 32) ptr[words[i + 1]] = words[i + 3];
    if (op == 27) sampled.add(words[i + 1]);
    if (op == 28 || op == 29) arrays[words[i + 1]] = words[i + 2];
    if (op == 59 && words[i + 3] == 0) {
      let t = ptr[words[i + 1]];
      while (arrays[t] !== undefined) t = arrays[t];
      if (sampled.has(t)) {
        const v = words[i + 2];
        out.push([set[v] || 0, binding[v] || 0, set[v] || 0, (binding[v] || 0) + 500]);
      }
    }
    i += n || 1;
  }
  return out;
}

async function main() {
  const factory = require(path.resolve(process.argv[2]));
  const m = await factory();
  let bad = 0;
  for (const file of process.argv.slice(3)) {
    const buf = fs.readFileSync(file);
    const words = new Uint32Array(buf.buffer, buf.byteOffset, buf.byteLength >> 2);
    const map = combinedBindings(words);
    const p = m._malloc(words.byteLength), q = m._malloc(16 * map.length + 4);
    m.HEAPU8.set(new Uint8Array(words.buffer, words.byteOffset, words.byteLength), p);
    map.forEach((e, i) => e.forEach((v, k) => { m.HEAPU32[(q >> 2) + 4 * i + k] = v; }));
    const r = m._arc_tint_spirv_to_wgsl(p, words.length, q, map.length);
    const text = m.UTF8ToString(r);
    m._arc_tint_free(r);
    m._free(p);
    m._free(q);
    console.log('// ' + file + (text[0] == 'W' ? '' : ': ERROR'));
    console.log(text.substring(1));
    if (text[0] != 'W') bad++;
  }
  process.exit(bad);
}
main();
