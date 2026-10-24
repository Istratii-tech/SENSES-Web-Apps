// Emscripten's buffer uploads for a thread whose WebGL2 runs on the page
// (shim/gles/remote_webgl.js), a --js-library of the phone builds.
//
// With MAXIMUM_MEMORY above 2 GB Emscripten leaves out WebGL 2's
// (data, offset, length) forms (a Firefox bug with large offsets) and
// passes HEAPU8.subarray(...) instead: a view per glBufferSubData, ~30 a
// frame in Angry Birds' menu, garbage for the thread's GC (docs/topics/
// ios-mem.md). The stand-in context copies such a range itself (RANGES
// there) and the page gets no offset, so for it the heap goes with an
// offset and a length; a real context keeps Emscripten's way.

addToLibrary({
  $webglBufferSubData__internal: true,
  $webglBufferSubData: (target, offset, size, data, src = HEAPU8) => {
    if (GLctx.arcRemote) {
      // A length of 0 would mean the rest of the heap.
      size && GLctx.bufferSubData(target, offset, src, data, size);
      return;
    }
    GLctx.bufferSubData(target, offset, src.subarray(data, data + size));
  },
  glBufferData: (target, size, data, usage) => {
    if (GLctx.arcRemote && data && size) {
      GLctx.bufferData(target, HEAPU8, usage, data, size);
      return;
    }
    GLctx.bufferData(target, data ? HEAPU8.subarray(data, data + size) : size, usage);
  },
  // Its emscripten_ name (what graphics_translation calls) is a copy made
  // before this library, not an alias.
  emscripten_glBufferData: 'glBufferData',
});
