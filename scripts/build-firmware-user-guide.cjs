/* Build the offline user guide from its maintained Markdown and local PNGs.
 * Usage: RYZ_DOCS_NODE_MODULES=/path/to/node_modules node scripts/build-firmware-user-guide.cjs
 * Requires marked; no network, browser or device access. */
const fs = require('node:fs');
const path = require('node:path');
const {pathToFileURL} = require('node:url');

async function main() {
  const modulePath = process.env.RYZ_DOCS_NODE_MODULES
    ? path.join(process.env.RYZ_DOCS_NODE_MODULES, 'marked/lib/marked.esm.js') : null;
  const {marked} = modulePath ? await import(pathToFileURL(modulePath).href) : await import('marked');
  const root = path.resolve(__dirname, '../docs/software/user-guide-v0.10.1');
  const source = fs.readFileSync(path.join(root, 'README.md'), 'utf8');
  let html = await marked.parse(source);
  const toc = [];
  html = html.replace(/<h2>(.*?)<\/h2>/g, (_, title) => {
    const id = `section-${toc.length + 1}`;
    toc.push(`<a href="#${id}">${title}</a>`);
    return `<h2 id="${id}">${title}</h2>`;
  });
  html = html.replace(/<p><img src="(assets\/[^"<>]+\.png)" alt="([^"<>]*)"><\/p>/g, (_, src, alt) => {
    const bytes = fs.readFileSync(path.join(root, src));
    if (bytes.readUInt32BE(16) !== 240 || bytes.readUInt32BE(20) !== 240) throw new Error(`Not native 240px: ${src}`);
    return `<figure><img width="240" height="240" src="data:image/png;base64,${bytes.toString('base64')}" alt="${alt}"><figcaption>${alt}</figcaption></figure>`;
  });
  const page = `<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>RyzoBee V0.10.1 用户使用指南</title><style>
  :root{--orange:#ff6a00;--ink:#181818;--line:#d9d9d9}*{box-sizing:border-box}html{scroll-behavior:smooth;scroll-padding-top:24px}body{margin:0;background:#f4f4f4;color:var(--ink);font:17px/1.85 "Noto Sans SC","PingFang SC",system-ui,sans-serif}a{color:#9d4000;text-underline-offset:3px}aside{position:fixed;inset:0 auto 0 0;width:260px;padding:32px 24px;background:#181818;color:white;overflow:auto}aside strong{font-size:24px;letter-spacing:1px}aside small{display:block;color:#a0a0a0;margin:6px 0 24px}aside a{display:block;color:#d9d9d9;text-decoration:none;font-size:14px;line-height:1.6;padding:8px 0;border-bottom:1px solid #3a3a3a}aside a:hover{color:var(--orange)}main{margin-left:260px;max-width:1120px;padding:48px 64px 100px;background:white;min-height:100vh}h1{font-size:42px;line-height:1.3;letter-spacing:-1px;margin:0 0 16px;border-top:8px solid var(--orange);padding-top:24px}h2{font-size:28px;line-height:1.5;margin:64px 0 24px;padding:12px 0;border-bottom:2px solid var(--orange);break-after:avoid}h3{font-size:21px;margin:28px 0 12px;break-after:avoid}p{margin:14px 0}blockquote{margin:24px 0;padding:12px 22px;border-left:4px solid var(--orange);background:#fff5ed;font-size:16px}ul,ol{padding-left:26px}li{margin:8px 0}table{border-collapse:collapse;width:100%;font-size:15px;margin:24px 0}th,td{text-align:left;vertical-align:top;padding:12px 14px;border:1px solid var(--line);overflow-wrap:anywhere}th{background:#242424;color:#fff}tr:nth-child(even){background:#fafafa}code{font:0.9em/1.5 ui-monospace,SFMono-Regular,monospace;background:#f0f0f0;padding:2px 5px;border-radius:3px;overflow-wrap:anywhere}pre{background:#181818;color:#eeeeee;padding:20px;white-space:pre-wrap;overflow-wrap:anywhere;font-size:14px}pre code{background:none;padding:0}figure{display:inline-flex;vertical-align:top;flex-direction:column;width:264px;max-width:100%;margin:14px 14px 20px 0;padding:12px;background:#181818;break-inside:avoid}figure img{max-width:100%;height:auto;image-rendering:pixelated}figcaption{color:#d9d9d9;font-size:13px;line-height:1.65;padding-top:10px}hr{border:0;border-top:1px solid var(--line);margin:36px 0}.footer{font-size:13px;color:#666;margin-top:48px}button{background:var(--orange);color:#000;font:inherit;border:0;padding:8px 14px;cursor:pointer;margin-bottom:18px}
  @media(max-width:900px){aside{position:relative;width:auto;padding:22px}aside nav{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:0 16px}main{margin:0;padding:28px 22px 64px}h1{font-size:32px}h2{font-size:25px;margin-top:48px}table{font-size:14px}th,td{padding:8px}figure{margin-right:8px}body{font-size:17px}}
  @media print{@page{size:A4;margin:16mm}body{background:white;font-size:10.5pt;line-height:1.6}aside,button{display:none}main{margin:0;padding:0;max-width:none}h1{font-size:30pt}h2{font-size:20pt;margin-top:24pt}h3{font-size:14pt}table{font-size:9pt}figure{width:246px;padding:3px;margin:10px 10px 14px 0}figcaption{color:#181818;background:white;font-size:9pt}pre{font-size:9pt}a{color:inherit}tr,blockquote{break-inside:avoid}*{-webkit-print-color-adjust:exact;print-color-adjust:exact}}
  </style></head><body><aside><strong>RYZOBEE</strong><small>ROOTMAKER / V0.10.1<br>用户使用指南</small><button onclick="window.print()">打印 / 保存 PDF</button><nav>${toc.join('')}</nav></aside><main>${html}<p class="footer">本文件可离线阅读，设备截图已内嵌；功能以配套 V0.10.1 固件与实际设备为准。</p></main></body></html>`;
  fs.writeFileSync(path.join(root, 'RyzoBee-V0.10.1-用户使用指南.html'), page);
  console.log(JSON.stringify({sections:toc.length, embeddedScreenshots:(page.match(/data:image\/png;base64/g)||[]).length, bytes:Buffer.byteLength(page)}));
}
main().catch(error=>{console.error(error);process.exitCode=1;});
