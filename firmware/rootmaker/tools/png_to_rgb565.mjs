#!/usr/bin/env node

import { readFileSync, writeFileSync } from 'node:fs';
import { spawnSync } from 'node:child_process';

function usage() {
  console.error('usage: png_to_rgb565.mjs <input.png> <output.rgb565>');
  process.exit(2);
}

const [, , inputPath, outputPath] = process.argv;
if (!inputPath || !outputPath) usage();

const dimensions = spawnSync(
  'magick',
  ['identify', '-format', '%w %h', inputPath],
  { encoding: 'utf8' },
);
if (dimensions.status !== 0) {
  process.stderr.write(dimensions.stderr || 'failed to inspect PNG\n');
  process.exit(dimensions.status ?? 1);
}
const [width, height] = dimensions.stdout.trim().split(/\s+/).map(Number);
if (width !== 240 || height !== 240) {
  console.error(`expected 240x240 input, got ${width}x${height}`);
  process.exit(1);
}

const converted = spawnSync(
  'magick',
  [inputPath, '-alpha', 'off', '-depth', '8', 'rgb:-'],
  { encoding: null, maxBuffer: 240 * 240 * 4 },
);
if (converted.status !== 0) {
  process.stderr.write(converted.stderr || Buffer.from('failed to decode PNG\n'));
  process.exit(converted.status ?? 1);
}

const rgb = converted.stdout;
const pixelCount = width * height;
if (rgb.length !== pixelCount * 3) {
  console.error(`expected ${pixelCount * 3} RGB bytes, got ${rgb.length}`);
  process.exit(1);
}

const rgb565 = Buffer.allocUnsafe(pixelCount * 2);
for (let pixel = 0; pixel < pixelCount; pixel += 1) {
  const source = pixel * 3;
  const value =
    ((rgb[source] >> 3) << 11) |
    ((rgb[source + 1] >> 2) << 5) |
    (rgb[source + 2] >> 3);
  const destination = pixel * 2;
  rgb565[destination] = value >> 8;
  rgb565[destination + 1] = value & 0xff;
}
writeFileSync(outputPath, rgb565);

const written = readFileSync(outputPath);
if (!written.equals(rgb565)) {
  console.error('RGB565 readback verification failed');
  process.exit(1);
}
console.log(`${inputPath} -> ${outputPath} (${width}x${height}, ${written.length} bytes)`);
