const {chromium}=require(process.env.PLAYWRIGHT_MODULE||'C:/Users/Administrator/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const {pathToFileURL}=require('node:url');

(async()=>{
  const root=path.resolve(__dirname,'..');
  const output=path.join(root,'_build/preview4');
  const browser=await chromium.launch({headless:true,args:['--disable-gpu'],executablePath:process.env.CHROME_PATH||'C:/Program Files/Google/Chrome/Application/chrome.exe'});
  const errors=[],requests=[];
  try{
    const page=await browser.newPage({viewport:{width:1500,height:1000}});
    page.setDefaultTimeout(90000);
    page.on('pageerror',error=>errors.push(error.message));
    page.on('request',request=>{if(!/^(file:|blob:)/.test(request.url()))requests.push(request.url());});
    await page.setContent(fs.readFileSync(path.join(root,'docs/nte/figures/lifecycle-five-frames.svg'),'utf8'));
    await page.locator('svg').screenshot({path:path.join(output,'five-frame-svg.png')});
    await page.goto(pathToFileURL(path.join(root,'docs/nte/lifecycle-five-frame-timeline.html')).href);
    await page.waitForFunction(()=>window.pipelineViewer||document.querySelector('#boot-error').textContent);
    assert.equal(await page.locator('#boot-error').textContent(),'');
    const state=await page.evaluate(()=>window.pipelineViewer.getState());
    assert.equal(state.trackCounts.fg_gpu_1,205);
    assert.equal(state.trackCounts.fg_gpu_5,205);
    assert.equal(state.trackCounts.display,1229);
    for(const count of [5,10,20]){
      await page.selectOption('#window',String(count));
      await page.waitForFunction(count=>window.pipelineViewer.getState().window.frames.length===count,count);
    }
    await page.selectOption('#window','5');
    await page.evaluate(()=>window.pipelineViewer.seek(5));
    assert.equal(await page.locator('#frame-table tbody tr').count(),5);
    assert.equal(await page.locator('#fg-query-table tbody tr').count(),5);
    await page.screenshot({path:path.join(output,'timeline-desktop.png'),fullPage:true});
    await page.locator('#batch-timeline').screenshot({path:path.join(output,'batch-gpu-and-display.png')});
    await page.setViewportSize({width:430,height:900});
    await page.screenshot({path:path.join(output,'timeline-mobile.png'),fullPage:true});
    assert.deepEqual(errors,[]);
    assert.deepEqual(requests,[]);
    const report={passed:true,tracks:state.trackCounts,windows:[5,10,20],browserErrors:errors,externalRequests:requests};
    fs.writeFileSync(path.join(output,'publication-browser-qa.json'),JSON.stringify(report,null,2));
    process.stdout.write(JSON.stringify(report)+'\n');
  }finally{await browser.close();}
})().catch(error=>{process.stderr.write(error.stack+'\n');process.exitCode=1;});
