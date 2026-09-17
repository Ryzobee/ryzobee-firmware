/* Visual/readability smoke test for the generated offline HTML.
 * RYZ_DOCS_NODE_MODULES=/path/to/node_modules node scripts/check-firmware-user-guide.cjs */
const fs=require('node:fs');
const path=require('node:path');
const os=require('node:os');
const assert=require('node:assert/strict');
const {pathToFileURL}=require('node:url');
const {chromium}=require(process.env.RYZ_DOCS_NODE_MODULES?path.join(process.env.RYZ_DOCS_NODE_MODULES,'playwright'):'playwright');
async function main(){
  const file=path.resolve(__dirname,'../docs/software/user-guide-v0.10.1/RyzoBee-V0.10.1-用户使用指南.html');
  const directory=fs.mkdtempSync(path.join(os.tmpdir(),'ryz-guide-review-'));
  const chrome='/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
  const browser=await chromium.launch({headless:true,...(fs.existsSync(chrome)?{executablePath:chrome}:{})});
  try {
    const page=await browser.newPage();
    const errors=[];page.on('pageerror',e=>errors.push(e.message));
    await page.route(/^https?:\/\//,route=>route.abort());
    await page.setViewportSize({width:1280,height:960});
    await page.goto(pathToFileURL(file).href);
    await page.locator('img').last().waitFor();
    const imageState=await page.locator('img').evaluateAll(nodes=>nodes.map(n=>({complete:n.complete,w:n.naturalWidth,h:n.naturalHeight})));
    assert.equal(imageState.length,21);assert(imageState.every(n=>n.complete&&n.w===240&&n.h===240));
    assert.equal(await page.locator('main h2').count(),16);
    assert.equal(await page.locator('aside nav a').count(),16);
    for(const size of [{width:1280,height:960},{width:390,height:844}]){
      await page.setViewportSize(size);await page.evaluate(()=>scrollTo(0,0));
      assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),`Horizontal overflow at ${size.width}`);
      await page.screenshot({path:path.join(directory,`guide-${size.width}.png`)});
    }
    await page.setViewportSize({width:1280,height:960});
    await page.locator('#section-7').scrollIntoViewIfNeeded();
    await page.screenshot({path:path.join(directory,'guide-apps.png')});
    assert.deepEqual(errors,[]);
    console.log(JSON.stringify({result:'PASS',images:21,sections:16,viewports:[1280,390],screenshots:directory}));
  } finally {await browser.close();}
}
main().catch(error=>{console.error(error);process.exitCode=1;});
