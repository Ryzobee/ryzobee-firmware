#!/usr/bin/env node
// Compare the actual native LVGL frame against the whole Figma SVG export,
// independently of the crop/indexed asset generator. Never accesses a board.
import { readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';
import assert from 'node:assert/strict';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
assert(process.argv[2], 'Usage: check_v5_boot_pixels.mjs boot-0000.rgb565');
const svg = readFileSync(resolve(root, 'components/ryz_system_ui/assets/boot/screen.svg'));
const png = spawnSync('rsvg-convert', [], { input: svg });
assert.equal(png.status, 0, png.stderr.toString());
const result = spawnSync('magick', ['png:-', '-alpha', 'off', '-depth', '8', 'rgb:-'], { input: png.stdout });
assert.equal(result.status, 0, result.stderr.toString());
const rgb = result.stdout, actual = readFileSync(process.argv[2]);
assert.equal(rgb.length, 240 * 240 * 3);
assert.equal(actual.length, 240 * 240 * 2);
let differentPixels = 0;
for (let i = 0; i < 240 * 240; ++i) {
  const expected = ((rgb[3 * i] >> 3) << 11) | ((rgb[3 * i + 1] >> 2) << 5) | (rgb[3 * i + 2] >> 3);
  if (actual.readUInt16LE(i * 2) !== expected) ++differentPixels;
}
console.log(JSON.stringify({ differentPixels, totalPixels: 240 * 240, reference: 'Whole Figma SVG export converted to RGB565' }));
assert.equal(differentPixels, 0, 'Boot first frame differs from the current Figma export');
