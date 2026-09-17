const fs = require('fs');
const path = require('path');
const { Resvg } = require('@resvg/resvg-js');

const SRC = 'D:/esp32project/BMS/BMS System/docs/assets';
const OUT = path.join(SRC, '_png');
fs.mkdirSync(OUT, { recursive: true });

const files = fs.readdirSync(SRC).filter(f => f.toLowerCase().endsWith('.svg')).sort();
let ok = 0, fail = 0;
for (const f of files) {
  const inPath = path.join(SRC, f);
  const outPath = path.join(OUT, f.replace(/\.svg$/i, '.png'));
  try {
    const svg = fs.readFileSync(inPath, 'utf8');
    const resvg = new Resvg(svg, {
      fitTo: { mode: 'zoom', value: 2.5 },
      background: 'white',
      font: { loadSystemFonts: true, defaultFontFamily: 'Microsoft YaHei' }
    });
    const png = resvg.render();
    fs.writeFileSync(outPath, png.asPng());
    const dim = png.width + 'x' + png.height;
    console.log('OK  ', f.padEnd(34), dim);
    ok++;
  } catch (e) {
    console.log('FAIL', f, '->', e.message);
    fail++;
  }
}
console.log(`\nDone: ${ok} ok, ${fail} failed -> ${OUT}`);
