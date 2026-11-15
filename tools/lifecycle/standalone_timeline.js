'use strict';
const colors = ['#309bff','#fb8535','#58c978','#ed75b9','#9c7af0','#31bbb6','#939594'];
const labels = ['图形','计算','间接命令','光追命令','复制','标签等','未归因'];
const categoryByKind = {graphics:0,compute:1,indirect:2,raytrace:3,copy:4,target:5,capture:5};
const categoryByOperation = {1:0,2:0,3:1,4:4,5:4,6:4,7:5,8:2,9:6,10:1,11:1,12:3,13:3,14:5,15:5};
const operationNames = {1:'Draw',2:'DrawIndexed',3:'Dispatch',4:'CopyBuffer',5:'CopyTexture',6:'CopyResource',7:'Barrier',8:'ExecuteIndirect',9:'Bundle',10:'MetaInitialize',11:'MetaExecute',12:'ASBuild',13:'DispatchRays',14:'UAVClearUint',15:'UAVClearFloat'};
const queueNames = {'3':'ExecuteCommandLists','4':'CommandList','5':'Queue.Wait','6':'Queue.Signal','7':'GetCompletedValue','8':'SetEventOnCompletion','9':'CPU.Signal','10':'JobIdentity'};
const displaySlots=Array.from({length:6},(_,index)=>'display_slot_'+(index+1));
const fgGpuTracks=Array.from({length:5},(_,index)=>'fg_gpu_'+(index+1));
const sdkDetailTracks=['nr_running','helper_import_running','game_other_running','other_process_running','sdk_wait_input','sdk_input_lifetime','sdk_native_present','sdk_fg_batch_window','sdk_fg_batch_residency','sdk_packets','display_long_gap',...displaySlots];
const primaryTracks = ['direct','source_compute','ngx','out_copy','in_copy','nr','nr1','nr2','nr_running','output',...fgGpuTracks,'helper_wait_nr','helper_import_running','sdk_wait_input','sdk_input_lifetime','helper_wait_sdk','sdk_native_present','sdk_fg_batch_window','sdk_fg_batch_residency','sdk_running','mouse','controller','controller_hid','game_cpu','game_camera','helper','fg_sdk',...displaySlots,'display','display_long_gap'];
const extraTracks = ['native_other','fine','capture_copy','nr_other','bridge_cpu','gpu_clock','source_camera','game_display','sdk_packets','game_other_running','other_process_running'];
const trackTitles = {direct:'4080S · 主 DIRECT 区间',ngx:'NGX 调用相关区间（主队列子集）',out_copy:'4080S · 出站 COPY',in_copy:'5080 · 接收 COPY',nr:'5080 · NR 合计',nr1:'5080 · NR 第 1 遍（查询区间）',nr2:'5080 · NR 第 2 遍（查询区间）',output:'5080 · NR 输出快照',mouse:'鼠标 · 观察器收到位移',game_cpu:'游戏 · CPU 阶段标记',game_camera:'源相机 · 提交常量',helper:'helper · CPU 提交 / Present',fg_sdk:'5080 · 官方插帧 FG（SDK 状态）',display:'系统 · helper 显示事件'};
trackTitles.controller='手柄 · XInput原始状态';trackTitles.controller_hid='手柄 · HID报告 / 原始轴值';
trackTitles.helper_wait_nr='helper GPU · 等 NR-ready（内核依赖区间）';
trackTitles.helper_wait_sdk='helper GPU · 等 SDK（内核依赖区间）';
trackTitles.sdk_running='5080 · 官方 FG 所在 SDK 上下文 · GPU 驻留（含内部其他工作）';
const canvas = document.getElementById('timeline');
const context = canvas.getContext('2d');
const overview = document.getElementById('overview');
const position = document.getElementById('position');
const seconds = document.getElementById('seconds');
const mode = document.getElementById('window');
const statusElement = document.getElementById('status');
const detailElement = document.getElementById('detail');
const loaded = new Map();
const loading = new Map();
let manifest;
let selected;
let targetSeconds = 5;
let drawScheduled = false;
let hovered;
let revision = 0;
let currentFrameIndex = 0;
let visibleRows = [];
let selectedBatchSource;
const sourceColors=['#087f8c','#a13e74','#6256af','#9b6815','#277d36','#b84a27'];
function sourceColor(source){return sourceColors[source%sourceColors.length];}
const metrics = {renders:0,lastDrawMs:0};
const tooltip = document.createElement('div');
tooltip.style.cssText='position:fixed;display:none;z-index:20;pointer-events:none;max-width:440px;white-space:pre-wrap;background:#fff;border:1px solid #cdd5dc;border-radius:5px;padding:9px;font:12px/1.5 Consolas,"Microsoft YaHei",sans-serif;box-shadow:0 2px 12px #0002';
document.body.appendChild(tooltip);
tooltip.style.maxWidth='min(440px,calc(100vw - 20px))';
trackTitles.native_other='进程内 · 其他 DIRECT / COMPUTE 查询';
trackTitles.source_compute='4080S · COMPUTE 查询（含NGX标记，功能未核实）';
const tableContainer=document.createElement('div');
tableContainer.style.overflowX='auto';
const frameTable=document.getElementById('frame-table');
frameTable.before(tableContainer);tableContainer.appendChild(frameTable);frameTable.style.minWidth='1050px';

async function inflate(id) {
  const element = document.getElementById(id);
  if (!element) throw new Error('离线数据缺少 '+id);
  if (typeof DecompressionStream === 'undefined') throw new Error('此浏览器不支持离线解压；请用新版 Chrome 或 Edge 打开。');
  const binary = atob(element.textContent.trim());
  const bytes = new Uint8Array(binary.length);
  for (let index=0;index<binary.length;index++) bytes[index]=binary.charCodeAt(index);
  return new Response(new Blob([bytes]).stream().pipeThrough(new DecompressionStream('gzip'))).arrayBuffer();
}

async function loadTrack(track) {
  if (loaded.has(track)) return loaded.get(track);
  if (!manifest.tracks[track]) return null;
  if (!loading.has(track)) {
    loading.set(track,inflate('track-'+track).then(buffer=>{
      const values = new Uint32Array(buffer);
      if (values.length !== manifest.tracks[track].count*6) throw new Error(track+' 事件长度校验失败');
      const result = {values,count:values.length/6};
      loaded.set(track,result);
      loading.delete(track);
      return result;
    }));
  }
  return loading.get(track);
}

function lowerBound(track, timestamp) {
  let lower=0;
  let upper=track.count;
  while(lower<upper){const middle=Math.floor((lower+upper)/2);if(track.values[middle*6]<timestamp)lower=middle+1;else upper=middle;}
  return lower;
}

function frameBoundary(timestamp) {
  let lower=0;
  let upper=manifest.frames.length;
  while(lower<upper){const middle=Math.floor((lower+upper)/2);if(manifest.frames[middle][0]<=timestamp)lower=middle+1;else upper=middle;}
  return Math.max(0,lower-1);
}

function chooseWindow() {
  const frequency=manifest.frequency;
  const timestamp=targetSeconds*frequency;
  let begin;
  let end;
  let frames=[];
  if(mode.value==='full'){
    begin=0;end=manifest.duration*frequency;
  }else if(mode.value.endsWith('s')){
    const duration=Number(mode.value.slice(0,-1))*frequency;
    begin=Math.min(timestamp,Math.max(0,manifest.duration*frequency-duration));
    end=Math.min(manifest.duration*frequency,begin+duration);
    const startIndex=frameBoundary(begin);
    frames=manifest.frames.slice(startIndex+1).filter(frame=>frame[0]<=end).slice(0,80);
    currentFrameIndex=startIndex;
  }else{
    const count=Number(mode.value);
    let startIndex=frameBoundary(timestamp);
    while(startIndex>0 && manifest.frames[startIndex][0]>manifest.duration*frequency)startIndex--;
    const lastComplete=frameBoundary(manifest.duration*frequency);
    startIndex=Math.min(startIndex,Math.max(0,lastComplete-count));
    currentFrameIndex=startIndex;
    frames=manifest.frames.slice(startIndex+1,startIndex+count+1);
    begin=Math.max(0,manifest.frames[startIndex][0]);
    end=Math.min(manifest.duration*frequency,frames[frames.length-1][0]);
  }
  const tail=mode.value==='full'?0:Math.min(.05*frequency,(end-begin)*.15);
  return {begin,end,tail,frames,sourceStart:currentFrameIndex,mode:mode.value,target:targetSeconds,frequency};
}

function subset(trackName, begin, end) {
  const track=loaded.get(trackName);
  if(!track)return [];
  const first=lowerBound(track,Math.max(0,begin-manifest.tracks[trackName].max_duration_ticks));
  const last=lowerBound(track,end+1);
  const rows=[];
  for(let index=first;index<last;index++){
    const offset=index*6;
    if(track.values[offset+1]>=begin)rows.push(index);
  }
  return rows;
}

function record(trackName,index){const values=loaded.get(trackName).values;return Array.from(values.subarray(index*6,index*6+6));}
function eventKind(row){return manifest.kinds[row[3]];}
function eventCategory(trackName,row){
  const kind=eventKind(row);
  if(trackName==='direct'||trackName==='native_other'||trackName==='source_compute')return kind==='ngx_begin'?1:categoryByKind[kind]??6;
  if(trackName==='fine')return categoryByOperation[Number(kind.slice(10))]??6;
  if(['out_copy','in_copy','output','capture_copy','nr_other'].includes(trackName))return 4;
  if(['nr','nr1','nr2','nr_running','sdk_running','sdk_packets','sdk_input_lifetime','sdk_fg_batch_window','sdk_fg_batch_residency'].includes(trackName))return 1;
  if(trackName==='helper_import_running')return 4;
  if(trackName==='sdk_native_present')return 5;
  if(fgGpuTracks.includes(trackName))return 0;
  if(trackName==='helper'||trackName==='game_cpu'||trackName==='game_camera'||trackName==='source_camera'||trackName==='gpu_clock'||trackName.startsWith('controller'))return 5;
  return 6;
}

function clippedUnion(rows,trackName,begin,end){
  const intervals=rows.map(index=>record(trackName,index)).filter(row=>row[1]>row[0]).map(row=>[Math.max(begin,row[0]),Math.min(end,row[1])]).filter(row=>row[1]>row[0]).sort((first,second)=>first[0]-second[0]);
  let total=0,lower=0,upper=-1;
  for(const interval of intervals){if(interval[0]>upper){total+=Math.max(0,upper-lower);[lower,upper]=interval;}else upper=Math.max(upper,interval[1]);}
  return total+Math.max(0,upper-lower);
}

function setupCanvas(element,height){
  const ratio=window.devicePixelRatio||1;
  const width=element.clientWidth;
  element.style.height=height+'px';
  element.width=Math.round(width*ratio);element.height=Math.round(height*ratio);
  const painter=element.getContext('2d');painter.setTransform(ratio,0,0,ratio,0,0);
  return {painter,width,height};
}

function hatch(painter,left,top,width,height){
  if(width<=0)return;
  painter.save();painter.beginPath();painter.rect(left,top,width,height);painter.clip();
  painter.fillStyle='#efe7d6';painter.fillRect(left,top,width,height);
  painter.strokeStyle='#ac9770';painter.lineWidth=.7;
  for(let offset=-height;offset<width+height;offset+=8){painter.beginPath();painter.moveTo(left+offset,top+height);painter.lineTo(left+offset+height,top);painter.stroke();}
  painter.restore();
}

function drawOverview(){
  if(!manifest)return;
  const {painter,width,height}=setupCanvas(overview,78);
  painter.fillStyle='#fff';painter.fillRect(0,0,width,height);
  const left=52,right=14,plot=width-left-right;
  const periods=manifest.frames.slice(1).map((frame,index)=>[(frame[0]/manifest.frequency),(frame[0]-manifest.frames[index][0])*1000/manifest.frequency]).filter(row=>row[0]>=0&&row[0]<=manifest.duration);
  const maximum=Math.max(...periods.map(row=>row[1]));
  painter.fillStyle='#ece4d3';painter.fillRect(left+plot*manifest.native_until/manifest.duration,3,plot*(manifest.duration-manifest.native_until)/manifest.duration,6);
  painter.fillStyle='#d5e9f7';painter.fillRect(left,3,plot*manifest.native_until/manifest.duration,6);
  painter.beginPath();painter.strokeStyle='#5183a7';painter.lineWidth=.8;
  periods.forEach((row,index)=>{const horizontal=left+plot*row[0]/manifest.duration;const vertical=height-15-row[1]/maximum*(height-29);if(index===0)painter.moveTo(horizontal,vertical);else painter.lineTo(horizontal,vertical);});painter.stroke();
  painter.font='11px "Microsoft YaHei",sans-serif';painter.fillStyle='#67727b';painter.fillText('源帧ms',3,22);painter.fillText(maximum.toFixed(0),20,37);
  painter.fillText('0s',left,height-2);painter.textAlign='right';painter.fillText(manifest.duration.toFixed(2)+'s',width-right,height-2);painter.textAlign='left';
  for(const phase of manifest.phases.slice(1)){const horizontal=left+plot*phase.start/manifest.duration;painter.strokeStyle='#c8d0d8';painter.beginPath();painter.moveTo(horizontal,10);painter.lineTo(horizontal,height-13);painter.stroke();}
  if(selected){const begin=left+plot*selected.begin/manifest.frequency/manifest.duration;const end=left+plot*selected.end/manifest.frequency/manifest.duration;painter.fillStyle='#367ab322';painter.fillRect(begin,9,Math.max(2,end-begin),height-22);}
  const marker=left+plot*targetSeconds/manifest.duration;painter.strokeStyle='#b54b48';painter.lineWidth=1.5;painter.beginPath();painter.moveTo(marker,0);painter.lineTo(marker,height-12);painter.stroke();
}

function activeTracks(){
  const focus=document.getElementById('focus-sdk').checked&&manifest.sdk_source_batches;
  const tracks=primaryTracks.filter(track=>(!fgGpuTracks.includes(track)||manifest.tracks[track])&&(!focus||['in_copy','nr1','nr2','nr_running','output',...fgGpuTracks,'helper_wait_nr','helper_import_running','sdk_wait_input','sdk_input_lifetime','helper_wait_sdk','sdk_native_present','sdk_fg_batch_window','sdk_fg_batch_residency','sdk_running','helper','fg_sdk',...displaySlots,'display','display_long_gap'].includes(track)));
  if(document.getElementById('extra').checked)tracks.push(...extraTracks.filter(track=>manifest.tracks[track]));
  if(document.getElementById('queue').checked)tracks.push('queue_api');
  return tracks;
}

function draw(){
  if(!manifest)return;
  const started=performance.now();
  selected=chooseWindow();
  const tracks=activeTracks();
  const header=100,rowHeight=72,footer=36;
  const {width,height}=setupCanvas(canvas,header+tracks.length*rowHeight+footer);
  const left=14,right=14,plot=width-left-right;
  const viewEnd=selected.end+selected.tail;
  const duration=viewEnd-selected.begin;
  const xFor=timestamp=>left+plot*(timestamp-selected.begin)/duration;
  context.fillStyle='#fff';context.fillRect(0,0,width,height);
  context.font='13px "Microsoft YaHei",sans-serif';
  visibleRows=[];
  if(selected.frames.length&&selected.frames.length<=20){
    let previous=selected.begin;
    selected.frames.forEach((frame,index)=>{
      const end=Math.min(frame[0],selected.end);
      const horizontal=xFor(previous);
      context.fillStyle=index%2?'#f3f4f5':'#fafafa';context.fillRect(horizontal,header,Math.max(0,xFor(end)-horizontal),height-header-footer);
      context.strokeStyle='#dedede';context.beginPath();context.moveTo(horizontal,header);context.lineTo(horizontal,height-footer);context.stroke();
      const center=(horizontal+xFor(end))/2;
      const period=(frame[0]-(index?selected.frames[index-1][0]:manifest.frames[selected.sourceStart][0]))*1000/manifest.frequency;
      context.fillStyle='#20252a';context.font='15px "Microsoft YaHei",sans-serif';context.textAlign='center';context.fillText('F'+(index+1),center,27);
      context.font='13px "Microsoft YaHei",sans-serif';context.fillText(period.toFixed(2)+' ms',center,50);
      context.fillStyle='#68737c';context.font='11px "Microsoft YaHei",sans-serif';context.fillText('源 '+frame[1]+' · '+(1000/period).toFixed(2)+' fps',center,73);
      previous=end;
    });
  }else{
    context.fillStyle='#344451';context.fillText('完整记录 · 缩小到连续5帧可查看逐命令色条',left,38);
  }
  context.textAlign='left';
  const aggregate=duration/manifest.frequency>2;
  tracks.forEach((trackName,trackIndex)=>{
    const top=header+trackIndex*rowHeight;
    const info=manifest.tracks[trackName];
    context.fillStyle='#ffffff';context.fillRect(left,top,plot,22);
    context.fillStyle='#252b30';context.font='13px "Microsoft YaHei",sans-serif';context.fillText(trackName==='sdk_running'?trackTitles.sdk_running:info?.name||trackTitles[trackName],left+3,top+16);
    context.strokeStyle='#e6e9ec';context.beginPath();context.moveTo(left,top);context.lineTo(width-right,top);context.stroke();
    const chartTop=top+26;
    let rows=[];
    if(info&&aggregate){
      const binTicks=manifest.duration*manifest.frequency/600;
      for(let bucket=0;bucket<600;bucket++){
        const start=bucket*binTicks,end=(bucket+1)*binTicks;
        if(end<selected.begin||start>viewEnd||!info.bins[bucket][0])continue;
        const alpha=Math.min(.95,.2+Math.log2(1+info.bins[bucket][0])/20);
        context.globalAlpha=alpha;context.fillStyle=colors[['out_copy','in_copy','output'].includes(trackName)?4:['nr','nr1','nr2'].includes(trackName)?1:6];
        context.fillRect(Math.max(left,xFor(start)),chartTop,Math.max(1,Math.min(width-right,xFor(end))-Math.max(left,xFor(start))),31);
      }
      context.globalAlpha=1;
    }else if(info&&loaded.has(trackName)){
      rows=subset(trackName,selected.begin,viewEnd);
      const sourceFrames=new Map(selected.frames.map((frame,index)=>[frame[1],'F'+(index+1)]));
      for(const index of rows){
        const row=record(trackName,index);
        const start=Math.max(selected.begin,row[0]),end=Math.min(viewEnd,row[1]);
        const horizontal=xFor(start),endX=xFor(end);
        const length=end-start;
        context.fillStyle=colors[eventCategory(trackName,row)];
        if(trackName==='fg_sdk'){
          context.fillStyle=colors[1];context.fillRect(horizontal,chartTop,1,18);
          context.fillStyle='#444';context.font='11px sans-serif';context.textAlign='center';context.fillText(String(row[4]),horizontal,chartTop+34);context.textAlign='left';
        }else if(trackName==='game_camera'||trackName==='source_camera'){
          context.beginPath();context.moveTo(horizontal-4,chartTop+4);context.lineTo(horizontal+4,chartTop+4);context.lineTo(horizontal,chartTop+12);context.closePath();context.fill();
          const source=sourceFrames.get(row[2]);
          if(source){context.fillStyle='#555';context.font='10px sans-serif';context.textAlign='center';context.fillText(source+' / '+row[2],horizontal,chartTop+29);context.textAlign='left';}
        }else if(trackName.startsWith('display_slot_')){
          const detail=manifest.sdk_detail_events[row[4]];
          context.fillStyle=sourceColor(row[2]);context.fillRect(horizontal-1.5,chartTop,3,23);
          context.font='10px sans-serif';context.textAlign='center';context.fillText(String(row[2])+(detail.same_display_timestamp_count>1?' ×'+detail.same_display_timestamp_count:''),horizontal,chartTop+36);context.textAlign='left';
        }else if(length===0){
          if(trackName==='mouse'||trackName==='display'||trackName==='game_display'||trackName==='fine'||trackName.startsWith('controller'))context.fillRect(horizontal,chartTop+1,1,29);
        }else{
          context.fillRect(horizontal,chartTop,Math.max(.35,endX-horizontal),31);
          if(fgGpuTracks.includes(trackName)&&endX-horizontal>32){context.fillStyle='#152b43';context.font='10px sans-serif';context.fillText(((row[1]-row[0])*1000/manifest.frequency).toFixed(2)+'ms',horizontal+2,chartTop+20);}
          if(['sdk_running','sdk_fg_batch_window','sdk_fg_batch_residency'].includes(trackName)&&endX-horizontal>38){context.fillStyle='#222';context.font='10px sans-serif';context.fillText(((row[1]-row[0])*1000/manifest.frequency).toFixed(2)+'ms',horizontal+2,chartTop+20);}
          if(trackName==='display_long_gap'&&endX-horizontal>35){context.fillStyle='#222';context.font='11px sans-serif';context.fillText(((row[1]-row[0])*1000/manifest.frequency).toFixed(2)+' ms',horizontal+3,chartTop+20);}
          if(['nr','nr1','nr2','out_copy','in_copy'].includes(trackName)&&endX-horizontal>22&&sourceFrames.has(row[2])){
            context.fillStyle='#30343a';context.font='11px sans-serif';context.textAlign='center';context.fillText(sourceFrames.get(row[2]),(horizontal+endX)/2,chartTop+20);context.textAlign='left';
          }
        }
      }
      if(trackName==='direct'&&document.getElementById('fine').checked&&loaded.has('fine')){
        for(const index of subset('fine',selected.begin,viewEnd)){
          const row=record('fine',index);
          if(row[1]<=row[0]||manifest.jobs[row[4]][1]!==manifest.main_queue)continue;
          context.fillStyle=colors[eventCategory('fine',row)];
          context.fillRect(xFor(Math.max(selected.begin,row[0])),chartTop,Math.max(.2,xFor(Math.min(viewEnd,row[1]))-xFor(Math.max(selected.begin,row[0]))),31);
        }
      }
    }
    if(['direct','fine','native_other','source_compute'].includes(trackName)){
      if(!aggregate){
        for(const tail of manifest.truncated_tails||[]){
          const queue=manifest.jobs[tail[2]][1];
          if(trackName==='source_compute'&&!(manifest.queues[queue].type===2&&manifest.queues[queue].luid===manifest.queues[manifest.main_queue].luid))continue;
          if((trackName==='direct'&&queue!==manifest.main_queue)||(trackName==='native_other'&&queue===manifest.main_queue))continue;
          const lower=Math.max(selected.begin,tail[0]),upper=Math.min(viewEnd,tail[1]);
          if(upper>lower)hatch(context,xFor(lower),chartTop,xFor(upper)-xFor(lower),31);
        }
      }
      const cutoff=manifest.native_until*manifest.frequency;
      if(cutoff<viewEnd){const startX=Math.max(left,xFor(Math.max(selected.begin,cutoff)));hatch(context,startX,chartTop,width-right-startX,31);context.fillStyle='#78613c';context.font='12px "Microsoft YaHei",sans-serif';context.textAlign='center';context.fillText('超出已观测查询覆盖范围',Math.max(startX+100,(startX+width-right)/2),chartTop+21);context.textAlign='left';}
    }
    if(trackName==='ngx'&&!info){context.fillStyle='#7c858d';context.font='12px "Microsoft YaHei",sans-serif';context.textAlign='center';context.fillText('本轮 NGX / dlssd 归属未核实',width/2,chartTop+21);context.textAlign='left';}
    if(trackName.startsWith('controller')&&!info){context.fillStyle='#7c858d';context.textAlign='center';context.fillText('本轮无此输入记录，不能推断为没有操作',width/2,chartTop+21);context.textAlign='left';}
    let metric='';
    if(trackName==='direct')metric=selected.end/manifest.frequency>manifest.native_until?'含缺测：不能给出完整耗时':aggregate?'概览按事件数分箱':(clippedUnion(rows,trackName,selected.begin,selected.end)*1000/manifest.frequency/Math.max(1,selected.frames.length)).toFixed(2)+' ms / 源区间（查询并集）';
    else if(['out_copy','in_copy','nr','nr1','nr2'].includes(trackName)&&!aggregate){const durations=rows.map(index=>record(trackName,index)).filter(row=>selected.frames.some(frame=>frame[1]===row[2]));metric=durations.length?(durations.reduce((sum,row)=>sum+row[1]-row[0],0)*1000/manifest.frequency/durations.length).toFixed(2)+' ms / 源帧':'';}
    else if(trackName==='fg_sdk')metric='数量 ≠ 每个生成帧的显示时间';
    else if(trackName==='source_compute')metric='与其他队列是子集关系；查询重叠≠执行重叠';
    else if(trackName==='display')metric='原始记录总览（同时间戳会重叠）';
    else if(trackName.startsWith('display_slot_'))metric='颜色＝源帧批次；×N＝同批N条同时间戳';
    else if(trackName==='sdk_running'&&!aggregate)metric='本窗口驻留 '+(clippedUnion(rows,trackName,selected.begin,selected.end)*1000/manifest.frequency).toFixed(2)+' ms；纯FG未分离';
    else if(trackName==='sdk_fg_batch_window'&&!aggregate)metric='命令包窗口；含排队和内部其他命令';
    else if(trackName==='sdk_fg_batch_residency'&&!aggregate)metric='按输入生命周期归属；可能跨批次';
    else if(fgGpuTracks.includes(trackName)&&!aggregate){const matching=rows.map(index=>record(trackName,index)).filter(row=>selected.frames.some(frame=>frame[1]===row[2]));metric=matching.length?(matching.reduce((sum,row)=>sum+row[1]-row[0],0)*1000/manifest.frequency/matching.length).toFixed(3)+' ms / 次；真实GPU查询':'真实GPU查询；留白不代表SDK完全空闲';}
    else if(trackName==='sdk_running'||trackName==='nr_running')metric='调度驻留；不是着色器占用率';
    else if(trackName==='sdk_input_lifetime')metric='含调度等待；不是纯FG耗时';
    else if(trackName==='sdk_packets')metric='提交→完成；含排队，功能未分类';
    else if(trackName==='game_camera')metric='提交常量 ≠ 内部相机更新';
    else if(trackName==='helper')metric='CPU Present返回 ≠ GPU完成';
    else if(info)metric=aggregate?info.count.toLocaleString()+' 全程事件':rows.length.toLocaleString()+' 窗口事件';
    context.textAlign='right';context.fillStyle='#6b737b';context.font='11px "Microsoft YaHei",sans-serif';context.fillText(metric,width-right-3,top+16);context.textAlign='left';
    visibleRows.push({track:trackName,top:chartTop,bottom:chartTop+39,indices:rows});
  });
  context.font='11px "Microsoft YaHei",sans-serif';context.fillStyle='#63717d';
  const ticks=6;
  for(let tick=0;tick<=ticks;tick++){
    const timestamp=selected.begin+duration*tick/ticks;const horizontal=xFor(timestamp);
    context.textAlign=tick===0?'left':tick===ticks?'right':'center';
    context.fillText((timestamp/manifest.frequency).toFixed(aggregate?1:3)+'s',horizontal,height-13);
  }
  context.textAlign='left';
  updateTable();drawOverview();
  document.getElementById('position-label').textContent=targetSeconds.toFixed(3)+' / '+manifest.duration.toFixed(3)+' 秒';
  const countText=aggregate?'全程/宽窗口按事件密度分箱；缩回5帧显示真实命令类别。':'连续 '+selected.frames.length+' 个真实源帧；色条按命令类型，横轴为共同QPC时间。';
  statusElement.textContent=countText+' 当前窗口 '+(selected.begin/manifest.frequency).toFixed(3)+'–'+(selected.end/manifest.frequency).toFixed(3)+'s。源帧分栏≠该批显示范围；六次呈现按源帧着色并分行。'+(selected.end/manifest.frequency>manifest.native_until?' 4080S细分缺测已标斜线。':' 条外留白不等于GPU空闲。');
  statusElement.className=selected.end/manifest.frequency>manifest.native_until?'warning':'';
  metrics.renders++;metrics.lastDrawMs=performance.now()-started;
}

function updateTable(){
  const body=document.querySelector('#frame-table tbody');body.replaceChildren();
  if(!selected.frames.length||selected.frames.length>20)return;
  let begin=selected.begin;
  selected.frames.forEach((frame,index)=>{
    const prior=manifest.frames[selected.sourceStart+index];
    const period=(frame[0]-prior[0])*1000/manifest.frequency;
    const values=['F'+(index+1)+' · '+frame[1],period.toFixed(3),(1000/period).toFixed(2)];
    const nativeValid=frame[0]/manifest.frequency<=manifest.native_until;
    values.push(nativeValid?(clippedUnion(subset('direct',begin,frame[0]),'direct',begin,frame[0])*1000/manifest.frequency).toFixed(3):'缺测');
    for(const track of ['out_copy','in_copy','nr']){
      const matching=subset(track,selected.begin,selected.end+selected.tail).map(position=>record(track,position)).filter(row=>row[2]===frame[1]);
      values.push(matching.length?(matching.reduce((sum,row)=>sum+row[1]-row[0],0)*1000/manifest.frequency).toFixed(3):'未关联');
    }
    const states=subset('fg_sdk',selected.begin,selected.end+selected.tail).map(position=>record('fg_sdk',position)).filter(row=>row[2]===frame[1]);
    values.push(states.length?states.map(row=>row[4]).join(', '):'未关联');
    const line=document.createElement('tr');for(const value of values){const cell=document.createElement('td');cell.textContent=value;line.appendChild(cell);}body.appendChild(line);
    begin=frame[0];
  });
}

function requestDraw(){if(drawScheduled)return;drawScheduled=true;requestAnimationFrame(()=>{drawScheduled=false;draw();});}
async function seek(value){targetSeconds=Math.max(0,Math.min(manifest.duration,Number(value)||0));position.value=String(targetSeconds);seconds.value=targetSeconds.toFixed(3);revision++;requestDraw();await new Promise(resolve=>requestAnimationFrame(resolve));return selected;}

function describe(track,index){
  const row=record(track,index),kind=eventKind(row);
  const info={轨道:manifest.tracks[track]?.name||track,事件:kind,开始秒:row[0]/manifest.frequency,结束秒:row[1]/manifest.frequency,耗时毫秒:(row[1]-row[0])*1000/manifest.frequency,源帧:row[2]||'未关联'};
  if(['direct','native_other','fine','ngx','source_compute'].includes(track)){const job=manifest.jobs[row[4]],queue=manifest.queues[job[1]];info.任务=String(manifest.session)+':'+job[0];info.队列=queue.pointer;info.队列角色=queue.roles;info.适配器LUID=queue.luid??'未关联';info.说明='命令查询区间包含依赖和屏障，不是纯场景渲染时间。';}
  if(track==='fine'){info.命令=operationNames[Number(kind.slice(10))]||kind;info.调用来源编号=row[5]||'未知';}
  if(track==='mouse'){info.dx=row[4]|0;info.dy=row[5]|0;info.设备=manifest.mouse_sources?.[String(row[0])]??'本轮未保存设备来源';}
  if(track.startsWith('controller'))info.原始输入=manifest.controller_payloads?.[row[4]]??'未记录';
  if(track==='fg_sdk'){info.SDK报告数量=row[4];info.SDK状态=row[5];info.说明='未验证每个生成帧的GPU或物理显示时间';}
  if(track==='queue_api'){info.API=queueNames[kind]||kind;info.序号=row[4];info.线程=row[5];info.说明='CPU API事件，不是GPU等待持续时间';}
  if(['out_copy','in_copy','nr','nr1','nr2','output','capture_copy','nr_other'].includes(track))info.seal=row[4];
  if(track==='game_camera'||track==='source_camera')info.姿态=manifest.cameras[String(row[2])];
  if(track==='sdk_running'){info.轨道=trackTitles.sdk_running;info.说明='FG所在SDK上下文的GPU调度驻留起止；含SDK内部其他工作，未分离纯FG计算，也未关联到单张生成帧。';}
  if(fgGpuTracks.includes(track)){
    info.FG测量=manifest.fg_gpu_evaluations[row[4]];
    info.说明='在真实NGX FrameGeneration EvaluateFeature前后记录的GPU时间戳；公开MultiFrameIndex为生成序号，源帧由同一fence/value匹配。含GPU抢占与内部屏障；独立队列工作未自动包含。';
  }
  if(sdkDetailTracks.includes(track)){
    info.SDK明细=manifest.sdk_detail_events[row[4]];
    info.说明=track.startsWith('display_slot_')?'同批输入fence绑定的原生Present序号；原生/生成身份和屏幕像素尚未验证。':track.endsWith('_running')?'校准后的GPU上下文调度驻留，不能据此细分shader功能。':'SDK输入批次关联；区间可能包含排队和等待。';
  }
  if(['helper_wait_nr','helper_wait_sdk'].includes(track)){
    info.依赖序号=row[4];info.说明='内核Wait/Signal匹配，按队列前序依赖裁剪；区间含调度等待。';
  }
  return info;
}

function selectBatch(source){
  if(!manifest.sdk_source_batches?.[String(source)])return;
  selectedBatchSource=String(source);
  document.getElementById('batch-source').value=selectedBatchSource;
  drawBatch();
}

function drawBatch(){
  const batch=manifest.sdk_source_batches?.[selectedBatchSource];
  if(!batch)return;
  const images=batch.display_events;
  const generation=manifest.fg_gpu_by_source?.[selectedBatchSource]||[];
  const shown=images.filter(event=>event.displayed_seconds!==null);
  const callGap=Math.max(...images.slice(1).map((event,index)=>(event.native_present_begin_seconds-images[index].native_present_begin_seconds)*1000));
  const summary='源帧 '+batch.source+' → SDK 输入fence '+batch.sdk_sequence+' → '+batch.native_calls+' 次原生 Present → '+batch.unique_display_times+' 个不同显示时间戳。批内原生调用最大间隔 '+callGap.toFixed(3)+' ms。输入就绪→释放 '+(batch.sdk_input_release_seconds===null?'未配对':((batch.sdk_input_release_seconds-batch.sdk_input_ready_seconds)*1000).toFixed(3)+' ms')+'；此区间 SDK 上下文驻留 '+(batch.sdk_residency_within_input_lifetime_ms===null?'未知':batch.sdk_residency_within_input_lifetime_ms.toFixed(3)+' ms')+'（含内部其他工作；蓝色泳道另列实际NGX查询）。';
  document.getElementById('batch-summary').textContent=summary;
  const generationBody=document.querySelector('#fg-query-table tbody');generationBody.replaceChildren();
  document.getElementById('fg-query-table').hidden=!generation.length;
  document.getElementById('fg-query-summary').textContent=generation.length?'该源帧 '+generation.length+' 次实际 FG 生成调用，GPU 区间合计 '+generation.reduce((total,event)=>total+event.gpu_elapsed_ms,0).toFixed(3)+' ms。下方蓝条来自 GPU 时间戳；生成序号尚未逐张绑定到原生 PresentId。':'本批没有额外的 NGX GPU 查询记录。';
  generation.forEach(event=>{
    const line=document.createElement('tr');
    const values=[event.multi_frame_index+'/'+event.multi_frame_count,event.gpu_begin_seconds.toFixed(6),event.gpu_end_seconds.toFixed(6),event.gpu_elapsed_ms.toFixed(6),event.source_association.source,'同批可关联；逐张显示身份未验证'];
    values.forEach(value=>{const cell=document.createElement('td');cell.textContent=value;cell.style.cssText='padding:7px;border-bottom:1px solid #e7ebee;text-align:right';line.appendChild(cell);});generationBody.appendChild(line);
  });
  const body=document.querySelector('#batch-table tbody');body.replaceChildren();
  images.forEach((event,index)=>{
    const prior=images[index-1];
    const values=[event.batch_position+'/'+batch.native_calls,event.present_id,event.native_present_begin_seconds.toFixed(6),((event.native_present_end_seconds-event.native_present_begin_seconds)*1000).toFixed(3),event.displayed_seconds===null?'未显示':event.displayed_seconds.toFixed(6),prior?.displayed_seconds!==null&&prior&&event.displayed_seconds!==null?((event.displayed_seconds-prior.displayed_seconds)*1000).toFixed(3):'—',event.same_display_timestamp_count,event.driver_frame_type+'（原生/生成未知）'];
    const line=document.createElement('tr');
    values.forEach(value=>{const cell=document.createElement('td');cell.textContent=value;cell.style.cssText='padding:7px;border-bottom:1px solid #e7ebee;text-align:right';line.appendChild(cell);});body.appendChild(line);
  });
  const nrRows=['nr1','nr2'].flatMap(track=>subset(track,0,manifest.duration*manifest.frequency).map(index=>({track,row:record(track,index)})).filter(event=>event.row[2]===batch.source));
  const rawBegin=Math.min(batch.sdk_input_wait_start_seconds,...images.map(event=>event.native_present_begin_seconds),...nrRows.map(event=>event.row[0]/manifest.frequency),...generation.map(event=>event.gpu_begin_seconds));
  const rawEnd=Math.max(batch.sdk_input_release_seconds??batch.sdk_input_ready_seconds,batch.sdk_input_ready_seconds,...shown.map(event=>event.displayed_seconds),...nrRows.map(event=>event.row[1]/manifest.frequency),...generation.map(event=>event.gpu_end_seconds));
  const begin=rawBegin-.002,end=rawEnd+.003;
  const element=document.getElementById('batch-timeline');
  const generationLanes=generation.length?Math.max(...generation.map(event=>event.multi_frame_index)):0;
  const {painter,width,height}=setupCanvas(element,455+generationLanes*36);
  const left=180,right=22,top=30,rowHeight=36;
  const horizontal=seconds=>left+(width-left-right)*(seconds-begin)/(end-begin);
  painter.fillStyle='#fff';painter.fillRect(0,0,width,height);painter.font='12px "Microsoft YaHei",sans-serif';
  for(let tick=0;tick<=8;tick++){
    const seconds=begin+(end-begin)*tick/8;const position=horizontal(seconds);
    painter.strokeStyle='#e2e6eb';painter.beginPath();painter.moveTo(position,top);painter.lineTo(position,height-27);painter.stroke();
    painter.fillStyle='#626c76';painter.textAlign='center';painter.fillText(seconds.toFixed(3)+'s',position,17);
  }
  const lane=(index,label)=>{const vertical=top+index*rowHeight;painter.fillStyle='#353e47';painter.textAlign='left';painter.fillText(label,10,vertical+19);return vertical;};
  for(let index=0;index<2;index++){
    const vertical=lane(index,'NR '+(index+1)+' · 源 '+batch.source);
    for(const event of nrRows.filter(event=>event.track==='nr'+(index+1))){painter.fillStyle='#fb8535';painter.fillRect(horizontal(event.row[0]/manifest.frequency),vertical+5,Math.max(1,horizontal(event.row[1]/manifest.frequency)-horizontal(event.row[0]/manifest.frequency)),20);}
  }
  const dependency=lane(2,'SDK 输入依赖（含等待）');
  painter.fillStyle='#d6b7dc';painter.fillRect(horizontal(batch.sdk_input_wait_start_seconds),dependency+5,Math.max(1,horizontal(batch.sdk_input_ready_seconds)-horizontal(batch.sdk_input_wait_start_seconds)),20);
  const lifecycle=lane(3,'SDK 输入就绪→释放');
  if(batch.sdk_input_release_seconds!==null){painter.fillStyle='#fde0bd';painter.fillRect(horizontal(batch.sdk_input_ready_seconds),lifecycle+5,Math.max(1,horizontal(batch.sdk_input_release_seconds)-horizontal(batch.sdk_input_ready_seconds)),20);}
  const running=lane(4,'FG所在SDK驻留 · 全批次');
  for(const index of subset('sdk_running',Math.max(0,begin*manifest.frequency),end*manifest.frequency)){
    const row=record('sdk_running',index);painter.fillStyle='#e77d22';painter.fillRect(horizontal(Math.max(begin,row[0]/manifest.frequency)),running+5,Math.max(1,horizontal(Math.min(end,row[1]/manifest.frequency))-horizontal(Math.max(begin,row[0]/manifest.frequency))),20);
  }
  for(let ordinal=1;ordinal<=generationLanes;ordinal++){
    const vertical=lane(4+ordinal,'FG 生成 '+ordinal+'/'+generationLanes+' · GPU查询');
    for(const event of generation.filter(event=>event.multi_frame_index===ordinal)){
      const lower=horizontal(event.gpu_begin_seconds),upper=horizontal(event.gpu_end_seconds);
      painter.fillStyle='#309bff';painter.fillRect(lower,vertical+4,Math.max(2,upper-lower),23);
      painter.fillStyle='#173c62';painter.textAlign='left';painter.font='11px sans-serif';
      painter.fillText(event.gpu_elapsed_ms.toFixed(3)+' ms',Math.min(width-right-62,upper+5),vertical+22);painter.font='12px "Microsoft YaHei",sans-serif';
    }
  }
  images.forEach((event,index)=>{
    const vertical=lane(5+generationLanes+index,'Present '+event.batch_position+'/'+batch.native_calls+' · '+event.present_id);
    const call=horizontal(event.native_present_begin_seconds);
    painter.fillStyle='#31bbb6';painter.fillRect(call,vertical+6,Math.max(2,horizontal(event.native_present_end_seconds)-call),17);
    if(event.displayed_seconds!==null){
      const display=horizontal(event.displayed_seconds);painter.strokeStyle=sourceColor(batch.source);painter.setLineDash([3,3]);painter.beginPath();painter.moveTo(call,vertical+15);painter.lineTo(display,vertical+15);painter.stroke();painter.setLineDash([]);
      painter.fillStyle=sourceColor(batch.source);painter.fillRect(display-2,vertical+2,4,26);
      painter.font='11px sans-serif';painter.textAlign='right';painter.fillText((event.same_display_timestamp_count>1?'同刻 ×'+event.same_display_timestamp_count+' · ':'')+event.displayed_seconds.toFixed(6)+'s',width-right,vertical+30);painter.font='12px "Microsoft YaHei",sans-serif';
    }
  });
  painter.fillStyle='#626c76';painter.textAlign='left';painter.fillText(generation.length?'蓝色：NGX生成GPU查询　青色：CPU原生Present调用　虚线/竖线：PresentId→系统显示　FG序号≠Present序号':'青色：CPU原生Present调用　虚线：同一PresentId关联　竖线：系统显示时间戳　RUNNING不能细分为每张生成帧',10,height-10);
}

function bootBatches(){
  if(!manifest.sdk_source_batches)return;
  document.getElementById('sdk-batches').hidden=false;
  const audit=manifest.sdk_batch_audit;
  document.getElementById('batch-audit').textContent='正式窗口 '+audit.formal_input_batches+' 批，原生Present数量：'+Object.entries(audit.native_calls_per_batch||{'6':audit.six_native_calls}).map(([count,batches])=>count+'次 / '+batches+'批').join('；')+'。每批不同显示时点数：'+Object.entries(audit.unique_display_times_per_formal_batch).sort((first,second)=>Number(second[0])-Number(first[0])).map(([count,batches])=>count+' 个时点 / '+batches+' 批').join('；')+'。绑定依据：输入fence序号、同一线程、顺序和交换链；原生调用时间与CSV估计时间最大差 '+audit.native_present_timestamp_max_error_ms.toFixed(4)+' ms。';
  const control=document.getElementById('batch-source');
  for(const batch of Object.values(manifest.sdk_source_batches).sort((first,second)=>first.source-second.source)){
    const option=document.createElement('option');option.value=String(batch.source);option.textContent='源 '+batch.source+' / SDK '+batch.sdk_sequence+' / '+batch.native_calls+'记录·'+batch.unique_display_times+'时点';control.appendChild(option);
  }
  control.onchange=()=>selectBatch(control.value);
  const fourExample=Object.values(manifest.sdk_source_batches).find(batch=>batch.native_calls===6&&batch.unique_display_times===4);
  document.getElementById('example-four').disabled=!fourExample;
  document.getElementById('example-four').onclick=()=>selectBatch(fourExample.source);
  document.getElementById('locate-batch').onclick=()=>{mode.value='5';seek(Math.max(0,manifest.sdk_source_batches[selectedBatchSource].sdk_input_wait_start_seconds));};
  document.getElementById('long-display-gap').onclick=()=>{
    const track=loaded.get('display_long_gap');
    let longest;
    for(let index=0;index<track.count;index++){const row=record('display_long_gap',index);if(!longest||row[1]-row[0]>longest[1]-longest[0])longest=row;}
    if(longest){mode.value='5';seek(Math.max(0,longest[0]/manifest.frequency-.05));const detail=manifest.sdk_detail_events[longest[4]];const source=detail.next_display_records[0].source;selectBatch(source);}
  };
  document.getElementById('long-display-gap').disabled=!manifest.tracks.display_long_gap?.count;
  selectBatch(fourExample?fourExample.source:control.value);
  new ResizeObserver(drawBatch).observe(document.getElementById('batch-timeline').parentElement);
}

function findEvent(event){
  if(!selected)return null;
  const bounds=canvas.getBoundingClientRect();const horizontal=event.clientX-bounds.left,vertical=event.clientY-bounds.top;
  const lane=visibleRows.find(row=>vertical>=row.top&&vertical<=row.bottom);
  if(!lane||!lane.indices.length)return null;
  const timestamp=selected.begin+(horizontal-14)/(canvas.clientWidth-28)*(selected.end+selected.tail-selected.begin);
  let found=null,best=Infinity;
  for(const index of lane.indices){const row=record(lane.track,index);const distance=timestamp>=row[0]&&timestamp<=row[1]?0:Math.min(Math.abs(timestamp-row[0]),Math.abs(timestamp-row[1]));if(distance<best){best=distance;found={track:lane.track,index};}}
  return best<(selected.end+selected.tail-selected.begin)*10/(canvas.clientWidth-28)?found:null;
}

canvas.addEventListener('pointermove',event=>{
  hovered=findEvent(event);
  if(!hovered){tooltip.style.display='none';return;}
  const content=JSON.stringify(describe(hovered.track,hovered.index),null,2);detailElement.textContent=content;tooltip.textContent=content;
  tooltip.style.display='block';tooltip.style.left=Math.max(5,Math.min(window.innerWidth-tooltip.offsetWidth-8,event.clientX+15))+'px';tooltip.style.top=Math.max(5,Math.min(window.innerHeight-tooltip.offsetHeight-10,event.clientY+12))+'px';
});
canvas.addEventListener('pointerleave',()=>{tooltip.style.display='none';});
canvas.addEventListener('click',event=>{const selectedEvent=findEvent(event);if(selectedEvent){detailElement.textContent=JSON.stringify(describe(selectedEvent.track,selectedEvent.index),null,2);const source=record(selectedEvent.track,selectedEvent.index)[2];if(manifest.sdk_source_batches?.[String(source)])selectBatch(source);}});
overview.addEventListener('click',event=>{const bounds=overview.getBoundingClientRect();seek((event.clientX-bounds.left-52)/(overview.clientWidth-66)*manifest.duration);});
position.addEventListener('input',()=>seek(position.value));seconds.addEventListener('change',()=>seek(seconds.value));mode.addEventListener('change',requestDraw);
document.getElementById('whole').onclick=()=>{mode.value='full';requestDraw();};
document.getElementById('previous').onclick=()=>{mode.value='5';const frame=manifest.frames[Math.max(0,currentFrameIndex-5)];seek(Math.max(0,frame[0]/manifest.frequency+.000001));};
document.getElementById('next').onclick=()=>{mode.value='5';const frame=manifest.frames[Math.min(manifest.frames.length-6,currentFrameIndex+5)];seek(Math.min(manifest.duration,frame[0]/manifest.frequency+.000001));};
for(const id of ['fine','extra','queue'])document.getElementById(id).addEventListener('change',async()=>{
  try{const wanted=id==='extra'?extraTracks:id==='queue'?['queue_api']:['fine'];statusElement.textContent='正在解码所选轨道…';await Promise.all(wanted.map(loadTrack));requestDraw();}catch(error){document.getElementById('boot-error').textContent=error.stack||error.message;}
});
document.getElementById('focus-sdk').addEventListener('change',requestDraw);
document.getElementById('export-json').onclick=()=>{
  const result={capture_seconds:manifest.duration,window:{begin_seconds:selected.begin/manifest.frequency,end_seconds:selected.end/manifest.frequency},frames:selected.frames,events:{},native_coverage_end_seconds:manifest.native_until,capture_quality:{main_dropped_records:manifest.main_dropped_records,fine_truncated_jobs:manifest.fine_truncated_jobs,complete_pipeline_verified:false},known_truncated_tails:(manifest.truncated_tails||[]).filter(row=>row[1]>selected.begin&&row[0]<selected.end)};
  for(const track of activeTracks()){const rows=subset(track,selected.begin,selected.end+selected.tail);if(rows.length<=200000)result.events[track]=rows.map(index=>describe(track,index));else result.events[track]={count:rows.length,omitted:'请缩小到五帧后导出原始窗口事件'};}
  result.selected_sdk_batch=manifest.sdk_source_batches?.[selectedBatchSource];result.sdk_batch_audit=manifest.sdk_batch_audit;
  result.selected_fg_gpu_evaluations=manifest.fg_gpu_by_source?.[selectedBatchSource];result.fg_gpu_summary=manifest.fg_gpu_summary;
  const link=document.createElement('a');const object=URL.createObjectURL(new Blob([JSON.stringify(result,null,2)],{type:'application/json'}));link.href=object;link.download='pipeline-'+(selected.begin/manifest.frequency).toFixed(3)+'s.json';link.click();setTimeout(()=>URL.revokeObjectURL(object),1000);
};

async function boot(){
  try{
    manifest=JSON.parse(new TextDecoder().decode(await inflate('pipeline-manifest')));
    document.getElementById('capture-info').textContent=(manifest.controlled_fixture?'受控样例（非游戏采样） ':manifest.input_time_aligned===false?'已封存GPU公共窗口 ':'正式采样 ')+manifest.duration.toFixed(6)+' 秒 · 原始流水线约 '+manifest.binary_stream_seconds.toFixed(2)+' 秒 · 内含 '+manifest.exported_events.toLocaleString()+' 条事件 · 单文件离线查看';
    position.max=String(manifest.duration);seconds.max=String(manifest.duration);document.getElementById('end-label').textContent=manifest.duration.toFixed(3)+'s';
    const legend=document.getElementById('legend');labels.forEach((label,index)=>{const item=document.createElement('span');item.className='key';const swatch=document.createElement('i');swatch.style.background=colors[index];item.appendChild(swatch);item.appendChild(document.createTextNode(label));legend.appendChild(item);});
    const bookmarks=document.getElementById('bookmarks');for(const seconds of manifest.bookmarks){const button=document.createElement('button');button.type='button';button.textContent=seconds.toFixed(3)+'s';button.onclick=()=>{mode.value='5';seek(seconds);};bookmarks.appendChild(button);}
    const baseline=manifest.baseline;
    const baselineText=baseline?'采集前只读 '+baseline.Seconds.toFixed(2)+' 秒：源 '+baseline.SourceRate.toFixed(2)+'/s，SDK '+baseline.SDKRate.toFixed(2)+'/s，前台 '+baseline.ForegroundSamples+'/'+baseline.TotalSamples+'。':'';
    const inputWarning=manifest.input_time_aligned===false?' 输入观察器晚启动：本图未对齐鼠标、手柄和前台采样；时间窗取三内部流的实际公共区间。':'';
    const conditionWarning=manifest.capture_summary?' 本窗口源 '+manifest.capture_summary.source_fps.toFixed(2)+'/s，SDK '+manifest.capture_summary.sdk_fps.toFixed(2)+'/s；'+manifest.capture_summary.condition_text+'。'+manifest.capture_summary.input_note:'';
    document.getElementById('measurement-impact').textContent=baselineText+' 图中阶段来自详细测量，包含测量开销。主流丢失 '+manifest.main_dropped_records+' 条；细分截断 '+manifest.fine_truncated_jobs+' 个任务。时轴使用GPU查询校准，斜线为已知未测尾段。'+inputWarning+conditionWarning+(manifest.capture_summary?.fg_note||'');
    if(manifest.source_admission_runtime_audit?.disabled_in_all_available_capture_status_samples)document.getElementById('measurement-impact').textContent+=' 本轮采样前输入额度保护已超时关闭；帧率改善不能证明新准入策略有效。用户反馈更流畅但输入延迟明显增大。';
    document.getElementById('notes').textContent='覆盖公共 '+manifest.duration.toFixed(6)+' 秒，保存 '+manifest.exported_events.toLocaleString()+' 条已记录时间轴事件，相对QPC整数时标；主DIRECT依据实际tagged_submission角色选择。主流丢失 '+manifest.main_dropped_records+' 条；细分截断 '+manifest.fine_truncated_jobs+' 个任务，保留缺测标记。预算截断后直到命令列表结束的未测尾段以斜线表示，其他未绘制部分也不等于空闲。DIRECT为GPU命令查询区间，并非ETW调度RUNNING或纯渲染时间；NGX具体功能归属未逐段核实。SDK数量是计数，未提供每个生成帧的GPU/物理显示时间。显示轨道保留PresentMon实际分类，未验证屏幕像素。队列轨道选择Execute、Wait、Signal和事件注册CPU调用，不导出GetCompletedValue轮询和全部命令表元数据；完整原始队列记录仍在queue.bin。此次保持同场景，没有预定手柄操作阶段。原始数据：'+manifest.raw_directory+'。';
    if(manifest.fg_gpu_summary)document.getElementById('notes').textContent+=' 本轮新增蓝色FG生成泳道：GPU时间戳包围真实NGX FrameGeneration调用，按公开生成序号1–5分行；源帧通过fence匹配。其区间包括GPU抢占及内部屏障，不自动包括独立队列工作。生成序号与六次PresentId之间的逐图身份尚未验证。';
    statusElement.textContent='正在解码主要轨道及4080S细分…';
    await Promise.all([...primaryTracks,'fine'].map(loadTrack));
    await seek(targetSeconds);
    bootBatches();
    window.pipelineViewer={seek,draw,selectBatch,getBatch:source=>manifest.sdk_source_batches?.[String(source)],getState:()=>({duration:manifest.duration,position:targetSeconds,window:selected,selectedBatchSource,loaded:Array.from(loaded.keys()),trackCounts:Object.fromEntries(Object.entries(manifest.tracks).map(([key,value])=>[key,value.count])),metrics}),describe,loadTrack,record};
    new ResizeObserver(requestDraw).observe(canvas.parentElement);
  }catch(error){document.getElementById('boot-error').textContent=error.stack||error.message;statusElement.textContent='打开失败，请保留错误信息。';console.error(error);}
}
boot();
