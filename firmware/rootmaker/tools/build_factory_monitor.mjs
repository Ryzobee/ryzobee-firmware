#!/usr/bin/env node
// Keep the editable factory copy byte-identical to its maintained source.
// The approved 16 KiB quota accommodates the complete V5 Monitor as text.
import fs from 'node:fs';
import {fileURLToPath} from 'node:url';
const root=fileURLToPath(new URL('../',import.meta.url));
const source=fs.readFileSync(root+'scripts/tool_monitor.lua','utf8');
const quota=Number(fs.readFileSync(root+'components/ryz_runtime/include/lua_runtime.h','utf8')
  .match(/^#define RYZ_LUA_SOURCE_MAX (\d+)$/m)?.[1]);
if(!source.startsWith('-- ryz-app/1\n') || source.includes('\0') ||
   !Number.isInteger(quota) || Buffer.byteLength(source)>quota)
  throw new Error('Factory Monitor source/byte quota validation failed');
const output=root+'fs/tool_monitor.lua';
if(process.argv.includes('--check')) {
  if(fs.readFileSync(output,'utf8')!==source)
    throw new Error('Run node tools/build_factory_monitor.mjs to refresh the factory copy');
} else fs.writeFileSync(output,source);
console.log('Factory Monitor: '+Buffer.byteLength(source)+'/'+quota+' bytes (readable, byte-identical)');
