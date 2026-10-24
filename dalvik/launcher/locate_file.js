// --extern-pre-js of dalvikvm.js (emitted before the file packager code,
// which reads dalvikvm.data as soon as the script loads): under Node, files
// the runtime loads by name are found next to dalvikvm.js whatever the
// current directory is. Browsers already resolve them against the page.
if (typeof process == 'object' && typeof __dirname == 'string') {
  var Module = typeof Module != 'undefined' ? Module : {};
  Module['locateFile'] ??= (path, prefix) => (prefix || __dirname + '/') + path;
  // The /system image (the packager's .data: nothing else is in it) is not
  // read into memory: the VM fills its pages from the file when first used
  // (topic image-on-demand, posix_translation/readonly_file.cc), and the
  // packager gets an empty package. ARC_IMAGE_LAZY=0: read whole, as before.
  if (process.env.ARC_IMAGE_LAZY !== '0' && !Module['getPreloadedPackage']) {
    Module['getPreloadedPackage'] = (name, size) => {
      Module['arcImageSource'] = {url: name, size: size};
      return new ArrayBuffer(0);
    };
  }
}
// Where the VM reads the image from, when the page (run.html: a Blob's URL)
// or the lines above gave it instead of the package: ARC_IMAGE_SOURCE and
// ARC_IMAGE_SIZE of the program's environment (the VM's threads have no
// Module). (The page's Module is globalThis.Module: the browser builds' code
// is wrapped in a function, tools/slim_thread_js.py, where Module is not set yet.)
var arcImageModule = typeof Module != 'undefined' ? Module : globalThis.Module;
if (arcImageModule && !(typeof ENVIRONMENT_IS_PTHREAD != 'undefined' && ENVIRONMENT_IS_PTHREAD)) {
  arcImageModule['preRun'] = [].concat(arcImageModule['preRun'] || [], [function() {
    var s = arcImageModule['arcImageSource'];
    if (s && s.url && !ENV['ARC_IMAGE_SOURCE']) {
      ENV['ARC_IMAGE_SOURCE'] = String(s.url);
      ENV['ARC_IMAGE_SIZE'] = String(s.size);
    }
  }]);
}
