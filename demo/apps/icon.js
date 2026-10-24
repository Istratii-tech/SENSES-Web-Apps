// SENSES Web Apps: an app's icon as the library shows it, drawn once at
// install time into a square PNG (icon.png in the library; topic
// senses-shell). Input: SensesApk's icon ({kind: 'image' | 'adaptive' |
// 'svg' | 'color'}). An adaptive icon is drawn as a launcher does: the
// background and the foreground on the 108 dp canvas, of which the middle
// 72 dp shows; the page rounds the corners.
(function (root) {
  'use strict';
  var SIZE = 192;
  function loadImage(src) {
    return new Promise(function (resolve, reject) {
      var img = new Image();
      img.onload = function () { resolve(img); };
      img.onerror = function () { reject(new Error('image')); };
      img.src = src;
    });
  }
  function urlOf(layer) {
    if (layer.kind === 'image') return URL.createObjectURL(new Blob([layer.bytes], {type: layer.mime}));
    if (layer.kind === 'svg') return URL.createObjectURL(new Blob([layer.svg], {type: 'image/svg+xml'}));
    return null;
  }
  // A layer over the whole 108 dp canvas (scaled so its middle 72 dp fill
  // the icon), less its inset.
  function drawLayer(g, layer, full) {
    if (!layer) return Promise.resolve();
    if (layer.kind === 'color') {
      g.fillStyle = layer.color;
      g.fillRect(0, 0, SIZE, SIZE);
      return Promise.resolve();
    }
    var url = urlOf(layer);
    if (!url) return Promise.resolve();
    return loadImage(url).then(function (img) {
      var s = full ? SIZE * 108 / 72 : SIZE, o = (SIZE - s) / 2;
      var inset = (layer.inset || 0) * s;
      g.drawImage(img, o + inset, o + inset, s - 2 * inset, s - 2 * inset);
    }).catch(function () {}).then(function () { URL.revokeObjectURL(url); });
  }
  function toPng(canvas) {
    return new Promise(function (resolve) {
      if (canvas.toBlob) {
        canvas.toBlob(function (b) {
          if (!b) { resolve(null); return; }
          b.arrayBuffer().then(function (a) { resolve(new Uint8Array(a)); }, function () { resolve(null); });
        }, 'image/png');
      } else {
        var bin = atob(canvas.toDataURL('image/png').split(',')[1]), out = new Uint8Array(bin.length);
        for (var i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
        resolve(out);
      }
    });
  }
  // A tile with the first letter, for apps without a usable icon.
  function letterTile(g, label) {
    var hue = 0;
    for (var i = 0; i < label.length; i++) hue = (hue * 31 + label.charCodeAt(i)) % 360;
    g.fillStyle = 'hsl(' + hue + ',28%,26%)';
    g.fillRect(0, 0, SIZE, SIZE);
    g.fillStyle = '#e6e6e6';
    g.font = '600 ' + Math.round(SIZE * 0.46) + 'px -apple-system, system-ui, sans-serif';
    g.textAlign = 'center';
    g.textBaseline = 'middle';
    g.fillText((label.trim().charAt(0) || '?').toUpperCase(), SIZE / 2, SIZE / 2 + SIZE * 0.03);
  }
  function render(icon, label) {
    var canvas = document.createElement('canvas');
    canvas.width = canvas.height = SIZE;
    var g = canvas.getContext('2d');
    var drawn;
    if (!icon) {
      letterTile(g, label || '?');
      drawn = Promise.resolve();
    } else if (icon.kind === 'adaptive') {
      g.fillStyle = '#ffffff';
      if (!icon.background) g.fillRect(0, 0, SIZE, SIZE);
      drawn = drawLayer(g, icon.background, true).then(function () { return drawLayer(g, icon.foreground, true); });
    } else if (icon.kind === 'color') {
      drawn = drawLayer(g, icon, false);
    } else {
      drawn = drawLayer(g, icon, false);
    }
    return drawn.then(function () {
      // Nothing visible (an undecodable image): the letter tile.
      var px = g.getImageData(0, 0, SIZE, SIZE).data, seen = false;
      for (var i = 3; i < px.length; i += 64) if (px[i]) { seen = true; break; }
      if (!seen) letterTile(g, label || '?');
      return toPng(canvas);
    });
  }
  root.SensesIcon = {render: render, SIZE: SIZE};
})(self);
