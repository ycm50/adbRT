// tmpscripts/gen-icon.mjs —— 用同一份几何定义同时生成 SVG（矢量）与 PNG/ICO（位图）
// 不依赖任何第三方库：自写抗锯齿光栅化 + node:zlib 压 PNG
import { deflateSync } from 'node:zlib';
import { writeFileSync, mkdirSync } from 'node:fs';
import { dirname } from 'node:path';

const S = 1024;                       // 母版画布
const OUT = 'A:/Downloads/adbRT/packaging/assets';

// ---------------- 几何定义（SVG 与位图共用） ----------------
const BG_TOP = [0x3b, 0x82, 0xf6];
const BG_BOT = [0x1d, 0x4e, 0xd8];
const WHITE = [255, 255, 255];

const shapes = [
  // Android 机器人头：白色圆角矩形身体
  { type: 'rrect', x: 256, y: 424, w: 512, h: 344, r: 172, color: WHITE },
  // 两只触角
  { type: 'capsule', x1: 372, y1: 430, x2: 306, y2: 322, width: 46, color: WHITE },
  { type: 'capsule', x1: 652, y1: 430, x2: 718, y2: 322, width: 46, color: WHITE },
  // 眼睛（挖成背景色 → 用叠加背景渐变近似）
  { type: 'circle', cx: 400, cy: 566, r: 52, color: BG_BOT },
  { type: 'circle', cx: 624, cy: 566, r: 52, color: BG_BOT },
];

// ---------------- SVG ----------------
function svg() {
  const grad = '<linearGradient id="g" x1="0" y1="0" x2="0" y2="1">' +
    '<stop offset="0" stop-color="#' + hex(BG_TOP) + '"/>' +
    '<stop offset="1" stop-color="#' + hex(BG_BOT) + '"/></linearGradient>';
  let body = '';
  for (const s of shapes) {
    const c = '#' + hex(s.color);
    if (s.type === 'rrect') {
      body += '<rect x="' + s.x + '" y="' + s.y + '" width="' + s.w + '" height="' + s.h +
              '" rx="' + s.r + '" ry="' + s.r + '" fill="' + c + '"/>\n  ';
    } else if (s.type === 'circle') {
      body += '<circle cx="' + s.cx + '" cy="' + s.cy + '" r="' + s.r + '" fill="' + c + '"/>\n  ';
    } else if (s.type === 'capsule') {
      body += '<line x1="' + s.x1 + '" y1="' + s.y1 + '" x2="' + s.x2 + '" y2="' + s.y2 +
              '" stroke="' + c + '" stroke-width="' + s.width + '" stroke-linecap="round"/>\n  ';
    }
  }
  return '<?xml version="1.0" encoding="UTF-8"?>\n' +
    '<!-- adbRT 应用图标（矢量母版）。位图由 tools/gen-icon 用同一份几何定义生成，' +
    '保证 SVG 与 PNG/ICO 完全一致。 -->\n' +
    '<svg xmlns="http://www.w3.org/2000/svg" width="' + S + '" height="' + S +
    '" viewBox="0 0 ' + S + ' ' + S + '">\n' +
    '  <defs>' + grad + '</defs>\n' +
    '  <rect x="0" y="0" width="' + S + '" height="' + S + '" rx="224" ry="224" fill="url(#g)"/>\n  ' +
    body + '</svg>\n';
}
function hex(c) { return c.map(v => v.toString(16).padStart(2, '0')).join(''); }

// ---------------- 光栅化（4x4 超采样） ----------------
const SS = 4;
function render() {
  const buf = new Float32Array(S * S * 4);
  for (let y = 0; y < S; y++) {
    const t = y / (S - 1);
    const bg = [0, 1, 2].map(i => BG_TOP[i] + (BG_BOT[i] - BG_TOP[i]) * t);
    for (let x = 0; x < S; x++) {
      const i = (y * S + x) * 4;
      const inRound = insideRoundRect(x, y, 0, 0, S, S, 224);
      if (inRound > 0) {
        buf[i] = bg[0]; buf[i + 1] = bg[1]; buf[i + 2] = bg[2]; buf[i + 3] = inRound;
      }
    }
  }
  for (const s of shapes) {
    for (let y = 0; y < S; y++) {
      for (let x = 0; x < S; x++) {
        let cov = 0;
        for (let sy = 0; sy < SS; sy++) {
          for (let sx = 0; sx < SS; sx++) {
            const px = x + (sx + 0.5) / SS, py = y + (sy + 0.5) / SS;
            if (hit(s, px, py)) cov += 1;
          }
        }
        if (cov === 0) continue;
        const a = cov / (SS * SS);
        const i = (y * S + x) * 4;
        // source-over（同色覆盖只需要 alpha 混合）
        const da = buf[i + 3];
        const outA = a + da * (1 - a);
        if (outA <= 0) continue;
        for (let k = 0; k < 3; k++) {
          buf[i + k] = (s.color[k] * a + buf[i + k] * da * (1 - a)) / outA;
        }
        buf[i + 3] = outA;
      }
    }
  }
  return buf;
}

function insideRoundRect(px, py, x, y, w, h, r) {
  const cx = x + w / 2, cy = y + h / 2, hw = w / 2, hh = h / 2;
  const qx = Math.abs(px - cx) - (hw - r), qy = Math.abs(py - cy) - (hh - r);
  const d = Math.min(Math.max(qx, qy), 0) + Math.hypot(Math.max(qx, 0), Math.max(qy, 0)) - r;
  return d <= 0 ? 1 : 0;
}

function hit(s, px, py) {
  if (s.type === 'rrect') return insideRoundRect(px, py, s.x, s.y, s.w, s.h, s.r) > 0;
  if (s.type === 'circle') return Math.hypot(px - s.cx, py - s.cy) <= s.r;
  if (s.type === 'capsule') {
    const dx = s.x2 - s.x1, dy = s.y2 - s.y1;
    const len2 = dx * dx + dy * dy || 1;
    let t = ((px - s.x1) * dx + (py - s.y1) * dy) / len2;
    t = Math.max(0, Math.min(1, t));
    const qx = s.x1 + t * dx, qy = s.y1 + t * dy;
    return Math.hypot(px - qx, py - qy) <= s.width / 2;
  }
  return false;
}

// ---------------- 降采样 + PNG ----------------
function downsample(src, size) {
  const out = new Uint8Array(size * size * 4);
  const step = S / size;
  for (let y = 0; y < size; y++) {
    for (let x = 0; x < size; x++) {
      let r = 0, g = 0, b = 0, a = 0, n = 0;
      const y0 = Math.floor(y * step), y1 = Math.floor((y + 1) * step);
      const x0 = Math.floor(x * step), x1 = Math.floor((x + 1) * step);
      for (let sy = y0; sy < y1; sy++) {
        for (let sx = x0; sx < x1; sx++) {
          const i = (sy * S + sx) * 4;
          const al = src[i + 3];
          r += src[i] * al; g += src[i + 1] * al; b += src[i + 2] * al; a += al; n++;
        }
      }
      const o = (y * size + x) * 4;
      if (a > 0) { out[o] = Math.round(r / a); out[o + 1] = Math.round(g / a); out[o + 2] = Math.round(b / a); }
      out[o + 3] = n ? Math.round((a / n) * 255) : 0;
    }
  }
  return out;
}

const CRC_TABLE = (() => {
  const t = new Int32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c;
  }
  return t;
})();
function crc32(buf) {
  let c = 0xffffffff;
  for (let i = 0; i < buf.length; i++) c = CRC_TABLE[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}
function chunk(type, data) {
  const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
  const td = Buffer.concat([Buffer.from(type, 'ascii'), data]);
  const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(td));
  return Buffer.concat([len, td, crc]);
}
function encodePng(rgba, size) {
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(size, 0); ihdr.writeUInt32BE(size, 4);
  ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
  const raw = Buffer.alloc((size * 4 + 1) * size);
  for (let y = 0; y < size; y++) {
    raw[y * (size * 4 + 1)] = 0;
    Buffer.from(rgba.buffer, rgba.byteOffset + y * size * 4, size * 4)
      .copy(raw, y * (size * 4 + 1) + 1);
  }
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', ihdr), chunk('IDAT', deflateSync(raw, { level: 9 })), chunk('IEND', Buffer.alloc(0)),
  ]);
}

// ---------------- ICO ----------------
function buildIco(entries) {
  const header = Buffer.alloc(6);
  header.writeUInt16LE(0, 0); header.writeUInt16LE(1, 2); header.writeUInt16LE(entries.length, 4);
  const dir = Buffer.alloc(16 * entries.length);
  let offset = 6 + 16 * entries.length;
  const blobs = [];
  entries.forEach((e, i) => {
    const b = 16 * i;
    dir[b] = e.size >= 256 ? 0 : e.size;
    dir[b + 1] = e.size >= 256 ? 0 : e.size;
    dir[b + 2] = 0; dir[b + 3] = 0;
    dir.writeUInt16LE(1, b + 4);   // planes
    dir.writeUInt16LE(32, b + 6);  // bpp
    dir.writeUInt32LE(e.png.length, b + 8);
    dir.writeUInt32LE(offset, b + 12);
    offset += e.png.length;
    blobs.push(e.png);
  });
  return Buffer.concat([header, dir, ...blobs]);
}

// ---------------- 主流程 ----------------
mkdirSync(OUT, { recursive: true });
writeFileSync(OUT + '/adbrt.svg', svg());

const master = render();
const sizes = [16, 24, 32, 48, 64, 128, 256];
const entries = [];
for (const s of sizes) {
  const rgba = downsample(master, s);
  const png = encodePng(rgba, s);
  entries.push({ size: s, png });
  writeFileSync(OUT + '/adbrt-' + s + '.png', png);
}
// 应用主图（512 供 README/安装器使用）
{
  const rgba = downsample(master, 512);
  writeFileSync(OUT + '/adbrt.png', encodePng(rgba, 512));
}
writeFileSync(OUT + '/adbrt.ico', buildIco(entries));
console.log('generated: adbrt.svg, adbrt.ico, adbrt.png, adbrt-{16..256}.png in ' + OUT);