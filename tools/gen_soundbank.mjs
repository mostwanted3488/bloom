// Génère Source/SoundBank.h à partir du tableau SOUNDS de Resources/index.html
import fs from 'node:fs';
const html = fs.readFileSync(new URL('../Resources/index.html', import.meta.url), 'utf8');
const i = html.indexOf('const SOUNDS = ['), j = html.indexOf('\n];', i);
const SOUNDS = eval(html.slice(i + 'const SOUNDS = '.length, j + 2));
const W = { sine: 0, triangle: 1, sawtooth: 2, square: 3 };
const f = v => { const s = String(v === undefined ? 0 : v); return (/[.e]/.test(s) ? s : s + '.0') + 'f'; };
let out = `// Généré par tools/gen_soundbank.mjs — ne pas éditer à la main.\n#pragma once\n#include <vector>\n\nnamespace bloom {\n\n`;
out += `struct Partial { int wave; float r, g, dec, det; bool fm, bl; };\n`;
out += `struct SoundDef {\n  const char* name; const char* cat; float gain; int oct;\n  std::vector<Partial> p;\n  bool hasFm; float fmR, fmI, fmDec;\n  bool hasVib; float vibRate, vibDepth;\n  std::vector<std::pair<float, float>> formants;\n  float click;\n  bool hasNoise; float noiseR, noiseQ, noiseG;\n  float a, len, hd, hs, rel, f0a, f0b, f1a, f1b, ft, q, verb, dly;\n};\n\n`;
out += `inline std::vector<SoundDef> makeSoundBank()\n{\n    return {\n`;
for (const s of SOUNDS) {
  const p = s.p.map(x => `{ ${W[x.w]}, ${f(x.r)}, ${f(x.g)}, ${f(x.dec)}, ${f(x.det)}, ${x.fm ? 'true' : 'false'}, ${x.bl ? 'true' : 'false'} }`).join(', ');
  const fo = (s.formants || []).map(([a, b]) => `{ ${f(a)}, ${f(b)} }`).join(', ');
  out += `        { "${s.name}", "${s.cat}", ${f(s.gain ?? 1)}, ${s.oct || 0},\n          { ${p} },\n`;
  out += `          ${s.fm ? 'true' : 'false'}, ${f(s.fm?.r)}, ${f(s.fm?.i)}, ${f(s.fm?.dec)},\n`;
  out += `          ${s.vib ? 'true' : 'false'}, ${f(s.vib?.rate)}, ${f(s.vib?.depth)},\n          { ${fo} },\n          ${f(s.click)},\n`;
  out += `          ${s.noise ? 'true' : 'false'}, ${f(s.noise?.r ?? 1)}, ${f(s.noise?.q)}, ${f(s.noise?.g)},\n`;
  out += `          ${f(s.a)}, ${f(s.len)}, ${f(s.hd)}, ${f(s.hs)}, ${f(s.rel)}, ${f(s.f0[0])}, ${f(s.f0[1])}, ${f(s.f1[0])}, ${f(s.f1[1])}, ${f(s.ft)}, ${f(s.q ?? .6)}, ${f(s.verb)}, ${f(s.dly)} },\n`;
}
out += `    };\n}\n\n`;
// listes de noms pour les paramètres (même ordre que l'interface)
const block = (start, end) => { const a = html.indexOf(start); return html.slice(a, html.indexOf(end, a)); };
const flowers = [...block('const SPECIES = [', '\n];').matchAll(/\{ name:'([A-Z]+)'/g)].map(m => m[1]);
const colors = [...block('const VARIANTS = {', '\n};').matchAll(/^\s+([A-Z]+):\s+\{ h:/gm)].map(m => m[1]);
const presets = [...block('const PRESETS = [', '\n];').matchAll(/\{ name:'([A-Z ]+)'/g)].map(m => m[1]);
const list = (n, a) => `inline const char* const ${n}[] = { ${a.map(x => `"${x}"`).join(', ')} };\ninline constexpr int ${n}Count = ${a.length};\n`;
out += list('kFlowerNames', flowers) + list('kColorNames', colors) + list('kPresetNames', presets);
out += `\n} // namespace bloom\n`;
console.log('fleurs', flowers.length, 'couleurs', colors.length, 'presets', presets.length);
fs.writeFileSync(new URL('../Source/SoundBank.h', import.meta.url), out);
console.log('SoundBank.h :', SOUNDS.length, 'sons');
