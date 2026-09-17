#!/usr/bin/env node
// Convert the unchanged monochrome Figma SVG exports into LVGL A8 masks.
// No hand-authored glyphs, scaling, thresholding, or screen-sized bitmaps.
import { readFileSync, writeFileSync, readdirSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const dir = resolve(root, 'components/ryz_system_ui/assets/v5');
const declarations = ['#pragma once', '#include "lvgl.h"'];
const source = ['/* Generated from exact Figma SVG exports by build_v5_assets.mjs. */',
  '#include "ryz_v5_assets.h"'];
for (const name of readdirSync(dir).filter(n => n.endsWith('.svg')).sort()) {
  const svg = readFileSync(resolve(dir, name), 'utf8');
  const width = Math.ceil(Number(svg.match(/<svg[^>]*\bwidth="([\d.]+)"/)[1]));
  const height = Math.ceil(Number(svg.match(/<svg[^>]*\bheight="([\d.]+)"/)[1]));
  const png = spawnSync('rsvg-convert', [], { input: svg });
  if (png.status !== 0) throw Error(png.stderr.toString());
  const alpha = spawnSync('magick', ['png:-', '-alpha', 'extract', '-depth', '8', 'gray:-'],
    { input: png.stdout });
  if (alpha.status !== 0) throw Error(alpha.stderr.toString());
  if (alpha.stdout.length !== width * height) throw Error(`dimensions: ${name}`);
  const key = 'ryz_v5_asset_' + name.slice(0, -4);
  const lines = [];
  for (let i = 0; i < alpha.stdout.length; i += 24)
    lines.push('    ' + [...alpha.stdout.subarray(i, i + 24)].join(',') + ',');
  source.push(`static const uint8_t ${key}_data[] = {\n${lines.join('\n')}\n};`,
    `const lv_image_dsc_t ${key} = {`,
    `    .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8,`,
    `                .w = ${width}, .h = ${height}, .stride = ${width} },`,
    `    .data_size = sizeof(${key}_data), .data = ${key}_data,`, '};');
  declarations.push(`extern const lv_image_dsc_t ${key};`);
  console.log(`${name}: ${width}x${height}, ${alpha.stdout.length} alpha bytes`);
}
writeFileSync(resolve(root, 'components/ryz_system_ui/ryz_v5_assets.c'), source.join('\n') + '\n');
writeFileSync(resolve(root, 'components/ryz_system_ui/include/ryz_v5_assets.h'), declarations.join('\n') + '\n');
