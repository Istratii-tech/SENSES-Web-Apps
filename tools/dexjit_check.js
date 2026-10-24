// Validates the modules tools/dexjit_check.cc wrote (topic dex-jit):
//   node tools/dexjit_check.js OUT.bin   (names in OUT.bin.names)
// Prints the first failures with WebAssembly.Module's message.
const fs = require('fs');
const file = process.argv[2];
const buf = fs.readFileSync(file);
const names = fs.readFileSync(file + '.names', 'utf8').split('\n').filter(l => l && !l.startsWith('#'));
let off = 0, n = 0, bad = 0;
while (off < buf.length) {
  const len = buf.readUInt32LE(off);
  const bytes = buf.subarray(off + 4, off + 4 + len);
  off += 4 + len;
  try {
    new WebAssembly.Module(bytes);
  } catch (e) {
    if (bad < 20) {
      console.log('INVALID #' + n + ' ' + names[n] + ': ' + e.message);
      fs.writeFileSync(file + '.bad' + bad + '.wasm', bytes);
    }
    bad++;
  }
  n++;
}
console.log(n + ' modules, ' + bad + ' invalid');
process.exit(bad ? 1 : 0);
