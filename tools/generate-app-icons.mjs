#!/usr/bin/env node

import fs from "node:fs";
import path from "node:path";
import zlib from "node:zlib";
import { fileURLToPath } from "node:url";

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const iconDir = path.join(repoRoot, "packaging", "icons");

const colours = {
  shell: [17, 20, 20, 255],
  shellInner: [21, 24, 24, 255],
  border: [42, 48, 48, 255],
  mark: [215, 104, 44, 255],
  markSoft: [224, 132, 76, 255],
  light: [225, 220, 210, 255],
};

function clamp(value, min = 0, max = 1) {
  return Math.max(min, Math.min(max, value));
}

function mix(a, b, amount) {
  return Math.round(a + (b - a) * amount);
}

function compositePixel(buffer, index, colour, alpha) {
  const sourceAlpha = clamp(alpha) * (colour[3] / 255);
  const destAlpha = buffer[index + 3] / 255;
  const outAlpha = sourceAlpha + destAlpha * (1 - sourceAlpha);
  if (outAlpha <= 0) return;

  for (let channel = 0; channel < 3; channel += 1) {
    const source = colour[channel] / 255;
    const dest = buffer[index + channel] / 255;
    buffer[index + channel] = Math.round(
      ((source * sourceAlpha) + (dest * destAlpha * (1 - sourceAlpha))) / outAlpha * 255,
    );
  }
  buffer[index + 3] = Math.round(outAlpha * 255);
}

function roundedRectCoverage(px, py, x, y, width, height, radius, aa) {
  const cx = x + width / 2;
  const cy = y + height / 2;
  const qx = Math.abs(px - cx) - (width / 2 - radius);
  const qy = Math.abs(py - cy) - (height / 2 - radius);
  const outside = Math.hypot(Math.max(qx, 0), Math.max(qy, 0));
  const inside = Math.min(Math.max(qx, qy), 0);
  const distance = outside + inside - radius;
  return clamp(0.5 - distance / aa);
}

function drawRoundedRect(buffer, size, rect, radius, colour) {
  const aa = 1.15 / size;
  const minX = Math.max(0, Math.floor(rect.x * size) - 2);
  const maxX = Math.min(size, Math.ceil((rect.x + rect.width) * size) + 2);
  const minY = Math.max(0, Math.floor(rect.y * size) - 2);
  const maxY = Math.min(size, Math.ceil((rect.y + rect.height) * size) + 2);

  for (let y = minY; y < maxY; y += 1) {
    for (let x = minX; x < maxX; x += 1) {
      const px = (x + 0.5) / size;
      const py = (y + 0.5) / size;
      const coverage = roundedRectCoverage(
        px,
        py,
        rect.x,
        rect.y,
        rect.width,
        rect.height,
        radius,
        aa,
      );
      if (coverage > 0) {
        compositePixel(buffer, (y * size + x) * 4, colour, coverage);
      }
    }
  }
}

function drawCircle(buffer, size, cx, cy, radius, colour) {
  const aa = 1.15 / size;
  const minX = Math.max(0, Math.floor((cx - radius) * size) - 2);
  const maxX = Math.min(size, Math.ceil((cx + radius) * size) + 2);
  const minY = Math.max(0, Math.floor((cy - radius) * size) - 2);
  const maxY = Math.min(size, Math.ceil((cy + radius) * size) + 2);

  for (let y = minY; y < maxY; y += 1) {
    for (let x = minX; x < maxX; x += 1) {
      const px = (x + 0.5) / size;
      const py = (y + 0.5) / size;
      const distance = Math.hypot(px - cx, py - cy) - radius;
      const coverage = clamp(0.5 - distance / aa);
      if (coverage > 0) {
        compositePixel(buffer, (y * size + x) * 4, colour, coverage);
      }
    }
  }
}

function renderIcon(size) {
  const buffer = Buffer.alloc(size * size * 4);

  drawRoundedRect(buffer, size, { x: 0.045, y: 0.045, width: 0.91, height: 0.91 }, 0.19, colours.border);
  drawRoundedRect(buffer, size, { x: 0.065, y: 0.065, width: 0.87, height: 0.87 }, 0.165, colours.shell);
  drawRoundedRect(buffer, size, { x: 0.115, y: 0.12, width: 0.77, height: 0.76 }, 0.12, colours.shellInner);

  const bars = [
    { x: 0.29, h: 0.22, c: colours.light },
    { x: 0.37, h: 0.4, c: colours.markSoft },
    { x: 0.45, h: 0.58, c: colours.mark },
    { x: 0.53, h: 0.5, c: colours.mark },
    { x: 0.61, h: 0.34, c: colours.markSoft },
    { x: 0.69, h: 0.24, c: colours.light },
  ];

  for (const bar of bars) {
    const width = 0.045;
    drawRoundedRect(
      buffer,
      size,
      { x: bar.x - width / 2, y: 0.5 - bar.h / 2, width, height: bar.h },
      width / 2,
      bar.c,
    );
  }

  drawCircle(buffer, size, 0.43, 0.72, 0.025, colours.mark);
  drawCircle(buffer, size, 0.57, 0.28, 0.022, colours.light);

  return buffer;
}

const crcTable = Array.from({ length: 256 }, (_, index) => {
  let c = index;
  for (let k = 0; k < 8; k += 1) {
    c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1);
  }
  return c >>> 0;
});

function crc32(buffer) {
  let c = 0xffffffff;
  for (const byte of buffer) {
    c = crcTable[(c ^ byte) & 0xff] ^ (c >>> 8);
  }
  return (c ^ 0xffffffff) >>> 0;
}

function pngChunk(type, data) {
  const typeBuffer = Buffer.from(type, "ascii");
  const length = Buffer.alloc(4);
  length.writeUInt32BE(data.length, 0);
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(Buffer.concat([typeBuffer, data])), 0);
  return Buffer.concat([length, typeBuffer, data, crc]);
}

function pngFromRgba(width, height, rgba) {
  const raw = Buffer.alloc((width * 4 + 1) * height);
  for (let y = 0; y < height; y += 1) {
    raw[y * (width * 4 + 1)] = 0;
    rgba.copy(raw, y * (width * 4 + 1) + 1, y * width * 4, (y + 1) * width * 4);
  }

  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8;
  ihdr[9] = 6;
  ihdr[10] = 0;
  ihdr[11] = 0;
  ihdr[12] = 0;

  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    pngChunk("IHDR", ihdr),
    pngChunk("IDAT", zlib.deflateSync(raw, { level: 9 })),
    pngChunk("IEND", Buffer.alloc(0)),
  ]);
}

function writeIco(entries, outputPath) {
  const header = Buffer.alloc(6);
  header.writeUInt16LE(0, 0);
  header.writeUInt16LE(1, 2);
  header.writeUInt16LE(entries.length, 4);

  const directory = Buffer.alloc(entries.length * 16);
  let offset = header.length + directory.length;
  entries.forEach((entry, index) => {
    const base = index * 16;
    directory[base] = entry.size >= 256 ? 0 : entry.size;
    directory[base + 1] = entry.size >= 256 ? 0 : entry.size;
    directory[base + 2] = 0;
    directory[base + 3] = 0;
    directory.writeUInt16LE(1, base + 4);
    directory.writeUInt16LE(32, base + 6);
    directory.writeUInt32LE(entry.png.length, base + 8);
    directory.writeUInt32LE(offset, base + 12);
    offset += entry.png.length;
  });

  fs.writeFileSync(outputPath, Buffer.concat([header, directory, ...entries.map((entry) => entry.png)]));
}

function writeIcns(entries, outputPath) {
  const chunks = entries.map((entry) => {
    const header = Buffer.alloc(8);
    header.write(entry.type, 0, "ascii");
    header.writeUInt32BE(entry.png.length + 8, 4);
    return Buffer.concat([header, entry.png]);
  });

  const header = Buffer.alloc(8);
  header.write("icns", 0, "ascii");
  header.writeUInt32BE(8 + chunks.reduce((total, chunk) => total + chunk.length, 0), 4);
  fs.writeFileSync(outputPath, Buffer.concat([header, ...chunks]));
}

function writeSvg(outputPath) {
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024">
  <rect x="46" y="46" width="932" height="932" rx="195" fill="#2a3030"/>
  <rect x="67" y="67" width="890" height="890" rx="169" fill="#111414"/>
  <rect x="118" y="123" width="788" height="778" rx="123" fill="#151818"/>
  <rect x="274" y="399" width="46" height="225" rx="23" fill="#e1dcd2"/>
  <rect x="356" y="307" width="46" height="410" rx="23" fill="#e0844c"/>
  <rect x="438" y="215" width="46" height="594" rx="23" fill="#d7682c"/>
  <rect x="520" y="256" width="46" height="512" rx="23" fill="#d7682c"/>
  <rect x="602" y="328" width="46" height="348" rx="23" fill="#e0844c"/>
  <rect x="684" y="389" width="46" height="246" rx="23" fill="#e1dcd2"/>
  <circle cx="440" cy="737" r="26" fill="#d7682c"/>
  <circle cx="584" cy="287" r="23" fill="#e1dcd2"/>
</svg>
`;
  fs.writeFileSync(outputPath, svg);
}

fs.mkdirSync(iconDir, { recursive: true });

const pngs = new Map();
for (const size of [16, 24, 32, 48, 64, 128, 256, 512, 1024]) {
  pngs.set(size, pngFromRgba(size, size, renderIcon(size)));
}

writeSvg(path.join(iconDir, "sesivo.svg"));
fs.writeFileSync(path.join(iconDir, "sesivo-1024.png"), pngs.get(1024));
writeIco(
  [16, 24, 32, 48, 64, 128, 256].map((size) => ({ size, png: pngs.get(size) })),
  path.join(iconDir, "sesivo.ico"),
);
writeIcns(
  [
    { type: "icp4", size: 16 },
    { type: "icp5", size: 32 },
    { type: "icp6", size: 64 },
    { type: "ic07", size: 128 },
    { type: "ic08", size: 256 },
    { type: "ic09", size: 512 },
    { type: "ic10", size: 1024 },
  ].map((entry) => ({ type: entry.type, png: pngs.get(entry.size) })),
  path.join(iconDir, "sesivo.icns"),
);

console.log(`Generated app icons in ${path.relative(repoRoot, iconDir)}`);
