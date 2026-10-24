// --pre-js of dexpreopt.js (shim/dexopt/dexpreopt.cc): the /system image is
// read from $ARC_SYSTEM_IMAGE at startup instead of being packaged with the
// tool, so the tool is linked once and the image can change.
Module['preRun'] = [].concat(Module['preRun'] || [], function() {
  var image = process.env.ARC_SYSTEM_IMAGE;
  if (!image)
    throw new Error('dexpreopt: set ARC_SYSTEM_IMAGE to a /system image');
  FS.mkdirTree('/arc');
  FS.writeFile('/arc/system.img', require('fs').readFileSync(image));
});
