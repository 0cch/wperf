#pragma once

namespace wperf::detail {

inline constexpr wchar_t kFlameGraphStyle[] = LR"CSS(
:root{color-scheme:light;--bg:#eef2f5;--surface:#fff;--surface-2:#f7f9fb;--ink:#172033;--muted:#64748b;--line:#d9e1e8;--line-strong:#bcc8d4;--accent:#2563eb;--accent-soft:#eff6ff;--shadow:0 8px 28px rgba(30,41,59,.08)}
*{box-sizing:border-box}body{margin:0;background:linear-gradient(180deg,#f7f9fb 0,#eef2f5 220px);color:var(--ink);font:14px/1.45 "Segoe UI",system-ui,sans-serif}
button,input{font:inherit}button{border:1px solid var(--line);border-radius:8px;background:var(--surface);color:var(--ink);padding:8px 11px;cursor:pointer;transition:border-color .15s,background .15s,box-shadow .15s}button:hover:not(:disabled){border-color:var(--line-strong);background:#fbfdff;box-shadow:0 1px 2px rgba(15,23,42,.05)}button:disabled{opacity:.45;cursor:default}
.app{max-width:1840px;margin:0 auto;padding:24px 28px 18px}.hero{display:flex;justify-content:space-between;gap:28px;align-items:flex-end;margin-bottom:16px}.eyebrow{font-size:10px;font-weight:700;letter-spacing:.11em;text-transform:uppercase;color:var(--muted)}h1{font-size:28px;line-height:1.15;margin:5px 0 8px;letter-spacing:-.02em}.subtitle{margin:0;color:var(--muted);word-break:break-all;font-size:12px}.hero-metric{min-width:190px;border:1px solid var(--line);background:var(--surface);border-radius:12px;padding:12px 15px;box-shadow:var(--shadow)}.hero-metric span{display:block;color:var(--muted);font-size:11px}.hero-metric strong{display:block;margin-top:2px;font-size:18px}
.stats-strip{display:flex;gap:8px;flex-wrap:wrap;margin:0 0 14px}.chip{border:1px solid var(--line);background:rgba(255,255,255,.82);border-radius:999px;padding:5px 10px;color:var(--muted);font-size:12px;white-space:nowrap}.chip b{color:var(--ink);font-weight:600}.chip.warn{border-color:#f2c79d;background:#fff7ed;color:#9a3412}.chip.good{border-color:#b7ddd6;background:var(--accent-soft);color:#0f5f58}
.card{border:1px solid var(--line);border-radius:12px;background:var(--surface);box-shadow:var(--shadow)}.toolbar{display:flex;gap:10px;align-items:center;flex-wrap:wrap;padding:11px;margin-bottom:14px}.search-field{position:relative;display:flex;align-items:center;flex:1;min-width:260px}.search-field input{width:100%;padding:9px 76px 9px 12px;border:1px solid var(--line);border-radius:9px;background:var(--surface-2);color:var(--ink);outline:none}.search-field input:focus{border-color:var(--accent);box-shadow:0 0 0 3px rgba(15,118,110,.12)}#search-counter{position:absolute;right:10px;color:var(--muted);font-size:11px;pointer-events:none}.tool-group{display:flex;gap:6px;align-items:center}.icon-button{width:36px;height:36px;padding:0}.filter-control{display:flex;align-items:center;gap:8px;border:1px solid var(--line);border-radius:9px;padding:6px 10px;background:var(--surface-2);font-size:12px;color:var(--muted)}.filter-control input{width:110px;accent-color:var(--accent)}.filter-control output{min-width:42px;text-align:right;color:var(--ink)}
.workspace{display:grid;grid-template-columns:minmax(0,1fr) 380px;gap:14px;align-items:start}.chart-card{min-width:0;overflow:hidden}.chart-head{display:flex;justify-content:space-between;gap:16px;align-items:flex-start;padding:12px 14px 10px;border-bottom:1px solid var(--line);background:linear-gradient(180deg,#fff,#fbfcfd)}.chart-title{margin:0;font-size:15px;font-weight:650}.chart-status{display:block;margin-top:3px;color:var(--muted);font-size:11px}.breadcrumb{display:flex;gap:4px;align-items:center;flex-wrap:wrap;justify-content:flex-end;max-width:62%}.crumb{max-width:210px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;border:0;background:transparent;padding:3px 5px;color:var(--ink);font-size:11px;font-weight:500}.crumb:hover{background:var(--accent-soft);color:#1d4ed8;box-shadow:none}.crumb-sep{color:#94a3b8;font-size:10px}.chart-stage{overflow:auto;max-height:calc(100vh - 260px);padding:10px;background:#fff}#chart{display:block;width:100%;min-height:90px;user-select:none;touch-action:none;cursor:crosshair}.chart-hints{padding:8px 14px 10px;border-top:1px solid var(--line);background:var(--surface-2);color:var(--muted);font-size:11px}
.frame{cursor:pointer;stroke:#fff;stroke-width:1}.frame:hover{stroke:#111827;stroke-width:2}.frame.match{stroke:#2563eb;stroke-width:2}.frame.current-match{stroke:#2563eb;stroke-width:3}.frame.selected{stroke:#111827;stroke-width:3}.frame-label{pointer-events:none;fill:#1f2937;font:12px "Segoe UI",system-ui,sans-serif}.frame-label.match{fill:#1d4ed8;font-weight:700}.frame-label.current-match{fill:#1d4ed8;font-weight:800}
.details-card{position:sticky;top:14px;padding:16px;max-height:calc(100vh - 28px);overflow:auto}.details-head{display:flex;justify-content:space-between;gap:12px;align-items:flex-start}.details-title{margin:5px 0 0;font-size:15px;line-height:1.35;word-break:break-word;max-height:82px;overflow:auto}.details-actions{display:flex;gap:6px;flex-shrink:0}.details-actions button{padding:6px 9px;font-size:11px}.details-module{margin-top:6px;color:var(--muted);font-size:11px;word-break:break-all}.details-meta{margin-top:8px;color:var(--muted);font-size:10px}.details-meta summary{cursor:pointer;color:var(--muted)}.details-meta code{display:block;margin-top:5px;color:#475569;word-break:break-all;font:10px/1.45 Consolas,"Cascadia Mono",monospace}.metric-grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:7px;margin:14px 0}.metric{min-width:0;border:1px solid var(--line);border-radius:9px;background:var(--surface-2);padding:8px}.metric span{display:block;color:var(--muted);font-size:10px}.metric strong{display:block;margin-top:3px;font-size:13px;line-height:1.2;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.metric small{display:block;margin-top:1px;color:var(--muted);font-size:9px;white-space:nowrap}.stack-head{display:flex;justify-content:space-between;gap:10px;align-items:center;margin-top:12px}.stack-head h3{margin:0;font-size:12px;color:var(--muted);font-weight:600;text-transform:uppercase;letter-spacing:.06em}.stack-actions{display:flex;gap:6px}.stack-actions button{padding:5px 8px;font-size:11px}.details-stack{height:220px;max-height:220px;overflow:auto;margin:8px 0 0;padding:10px 12px;border:1px solid var(--line);border-radius:9px;background:var(--surface-2);color:#273449;font:11.5px/1.65 Consolas,"Cascadia Mono",monospace;white-space:pre;word-break:normal;overflow-wrap:normal;tab-size:2;scrollbar-gutter:stable}.details-stack::-webkit-scrollbar{width:10px;height:10px}.details-stack::-webkit-scrollbar-thumb{background:#cbd5e1;border-radius:8px;border:2px solid var(--surface-2)}.details-stack::-webkit-scrollbar-track{background:transparent}.empty-details{color:var(--muted);font-size:12px;margin-top:12px}.hint{margin:10px 0 0;color:var(--muted);font-size:11px}
#tooltip{position:fixed;display:none;max-width:620px;background:#0f172a;color:#f8fafc;padding:8px 10px;border-radius:7px;font-size:12px;pointer-events:none;z-index:20;box-shadow:0 10px 30px rgba(0,0,0,.24);white-space:pre-wrap}footer{display:flex;justify-content:space-between;gap:16px;flex-wrap:wrap;color:var(--muted);font-size:11px;padding:14px 2px 0}
@media(max-width:1120px){.workspace{grid-template-columns:1fr}.details-card{position:static;max-height:none}.chart-stage{max-height:none}.breadcrumb{max-width:100%}}
@media(max-width:720px){.app{padding:14px}.hero{align-items:flex-start;flex-direction:column}h1{font-size:22px}.hero-metric{width:100%}.search-field{min-width:100%;flex-basis:100%}.filter-control{flex:1}.details-head{flex-direction:column}.details-actions{width:100%}.metric-grid{grid-template-columns:1fr}.tool-group{flex-wrap:wrap}}
)CSS";

inline constexpr wchar_t kFlameGraphScript[] =
LR"JS(
(() => {
  "use strict";
  const data = JSON.parse(document.getElementById("flame-data").textContent);
  const svg = document.getElementById("chart");
  const tooltip = document.getElementById("tooltip");
  const search = document.getElementById("search");
  const searchCounter = document.getElementById("search-counter");
  const prevMatch = document.getElementById("prev-match");
  const nextMatch = document.getElementById("next-match");
  const up = document.getElementById("up");
  const reset = document.getElementById("reset");
  const minPercent = document.getElementById("min-percent");
  const minPercentValue = document.getElementById("min-percent-value");
  const breadcrumb = document.getElementById("breadcrumb");
  const visibleStatus = document.getElementById("visible-status");
  const totalValue = document.getElementById("total-value");
  const detailsName = document.getElementById("details-name");
  const detailsModule = document.getElementById("details-module");
  const detailsIdentity = document.getElementById("details-identity");
  const detailsMeta = document.getElementById("details-meta");
  const detailsInclusive = document.getElementById("details-inclusive");
  const detailsInclusiveUnit = document.getElementById("details-inclusive-unit");
  const detailsSelf = document.getElementById("details-self");
  const detailsSelfUnit = document.getElementById("details-self-unit");
  const detailsShare = document.getElementById("details-share");
  const detailsStack = document.getElementById("details-stack");
  const detailsEmpty = document.getElementById("details-empty");
  const zoomSelected = document.getElementById("zoom-selected");
  const copyStack = document.getElementById("copy-stack");
  const copyFolded = document.getElementById("copy-folded");
  const downloadSvg = document.getElementById("download-svg");
  const downloadPng = document.getElementById("download-png");
  const ROW = 20;
  const ns = "http://www.w3.org/2000/svg";
  const number = new Intl.NumberFormat(undefined, {maximumFractionDigits:2});
  const state = {
    focus:data, navigation:[data], query:"", matchIndex:0, matches:[],
    minPercent:0, selectedNode:null, selectedPath:[],
    frameAreas:[], drag:null, lastActivation:null,
    lastVisible:0, lastHidden:0
  };

  function clamp(value, low, high) { return Math.min(high, Math.max(low, value)); }
  function weightText(value) { return number.format(value) + " " + data.weight_name; }
  function percentText(value, total) { return total > 0 ? (100 * value / total).toFixed(2) + "%" : "0.00%"; }
  function shortLabel(value, max=80) { return value.length > max ? value.slice(0, max-1) + "…" : value; }
  function parseFrameName(value) {
    const match=(value||"").match(/^(.+?)!(.+?)(?: \[(.+)\])?$/);
    if(!match)return {title:value||"(unnamed frame)",module:"",identity:""};
    return {title:match[2],module:match[1],identity:match[3]||""};
  }
  function frameDisplayName(value) {
    const parsed=parseFrameName(value||"");
    return parsed.module?parsed.module+"!"+parsed.title:parsed.title;
  }
  function color(name) {
    let hash = 2166136261 >>> 0;
    for (let i = 0; i < name.length; ++i) { hash ^= name.charCodeAt(i); hash = Math.imul(hash, 16777619) >>> 0; }
    return "hsl(" + (17 + (hash % 39)) + " 72% " + (57 + ((hash >>> 9) % 13)) + "%)";
  }
  async function copyText(text, button) {
    try { await navigator.clipboard.writeText(text); }
    catch {
      const area = document.createElement("textarea");
      area.value = text; document.body.appendChild(area); area.select(); document.execCommand("copy"); area.remove();
    }
    const old = button.textContent; button.textContent = "Copied"; setTimeout(() => { button.textContent = old; }, 1100);
  }
  function clearDetails() {
    state.selectedNode = null; state.selectedPath = [];
    for (const element of document.querySelectorAll(".frame.selected")) element.classList.remove("selected");
    detailsName.textContent = "No frame selected";
    detailsModule.textContent = ""; detailsIdentity.textContent = ""; detailsMeta.hidden = true;
    detailsInclusive.textContent = "—"; detailsInclusiveUnit.textContent = "";
    detailsSelf.textContent = "—"; detailsSelfUnit.textContent = "";
    detailsShare.textContent = "—";
    detailsStack.textContent = ""; detailsEmpty.hidden = false;
    zoomSelected.disabled = true; copyStack.disabled = true; copyFolded.disabled = true;
  }
  function selectNode(node, path, total, rect=null) {
    state.selectedNode = node; state.selectedPath = path;
    for (const element of document.querySelectorAll(".frame.selected")) element.classList.remove("selected");
    if (rect) rect.classList.add("selected");
    const parsed = parseFrameName(node.name || "");
    detailsName.textContent = parsed.title;
    detailsModule.textContent = parsed.module ? "Module: " + parsed.module : "";
    detailsIdentity.textContent = parsed.identity;
    detailsMeta.hidden = !parsed.identity;
    detailsInclusive.textContent = number.format(node.value);
    detailsInclusiveUnit.textContent = data.weight_name;
    detailsSelf.textContent = number.format(node.self);
    detailsSelfUnit.textContent = data.weight_name;
    detailsShare.textContent = percentText(node.value, total);
    detailsStack.textContent = path.map((item, index) => "  ".repeat(Math.min(index, 16)) + frameDisplayName(item.name)).join("\n");
    detailsEmpty.hidden = true;
    zoomSelected.disabled = !(node.children && node.children.length);
    copyStack.disabled = false; copyFolded.disabled = false;
  }
  function updateBreadcrumb() {
    breadcrumb.replaceChildren();
    state.navigation.forEach((item, index) => {
      if (index) { const sep=document.createElement("span"); sep.className="crumb-sep"; sep.textContent="›"; breadcrumb.appendChild(sep); }
      const button=document.createElement("button"); button.type="button"; button.className="crumb"; button.textContent=item.name || "root"; button.title=item.name || "root";
      button.addEventListener("click", () => { state.navigation=state.navigation.slice(0,index+1); state.focus=item; state.lastActivation=null; clearDetails(); render(); });
      breadcrumb.appendChild(button);
    });
    up.disabled = state.navigation.length <= 1;
  }
  function resetView() {
    state.focus=data; state.navigation=[data]; state.query=""; state.matchIndex=0; search.value="";
    state.minPercent=0; minPercent.value="0"; minPercentValue.value="0%"; state.lastActivation=null; clearDetails(); render();
  }
  function zoomTo(node) {
    if (!(node.children && node.children.length)) return;
    state.focus=node; state.navigation.push(node); state.matchIndex=0; state.lastActivation=null; clearDetails(); render();
  }
  function goUp() {
    if (state.navigation.length <= 1) return;
    state.navigation.pop(); state.focus=state.navigation[state.navigation.length-1];
    state.matchIndex=0; state.lastActivation=null; clearDetails(); render();
  }
  function cycleMatch(delta) {
    if (!state.matches.length) return;
    state.matchIndex=(state.matchIndex+delta+state.matches.length)%state.matches.length; render();
  }
)JS"
LR"JS(
  function selectionRoot(focus, startFraction, endFraction) {
    const selectionValue=focus.value*(endFraction-startFraction);
    function clip(node,nodeStart,nodeEnd) {
      const a=Math.max(startFraction,nodeStart), b=Math.min(endFraction,nodeEnd);
      if (b<=a) return null;
      const value=focus.value*(b-a); const result={name:node.name,value,self:0,children:[]};
      let cursor=nodeStart, childValue=0;
      for (const child of (node.children||[])) {
        const span=focus.value>0?child.value/focus.value:0; const clipped=clip(child,cursor,cursor+span);
        if(clipped){result.children.push(clipped); childValue+=clipped.value;} cursor+=span;
      }
      result.self=Math.max(0,value-childValue); return result;
    }
    const clipped=clip(focus,0,1);
    const selected={name:"selection "+(100*startFraction).toFixed(1)+"%–"+(100*endFraction).toFixed(1)+"%",value:selectionValue,self:0,children:clipped&&clipped.children?clipped.children:[]};
    selected.self=Math.max(0,selectionValue-selected.children.reduce((sum,child)=>sum+child.value,0)); return selected;
  }
  function eventPoint(event) {
    const bounds=svg.getBoundingClientRect(); const view=svg.viewBox.baseVal;
    return {x:(event.clientX-bounds.left)*view.width/Math.max(1,bounds.width), y:(event.clientY-bounds.top)*view.height/Math.max(1,bounds.height)};
  }
  function hitTest(point) {
    for(let i=state.frameAreas.length-1;i>=0;--i){const area=state.frameAreas[i]; if(point.x>=area.x&&point.x<=area.x+area.width&&point.y>=area.y&&point.y<area.y+area.height)return area;}
    return null;
  }
  function updateSelectionOverlay(overlay,startX,currentX,height) {
    overlay.setAttribute("x",Math.min(startX,currentX).toFixed(2)); overlay.setAttribute("y","0");
    overlay.setAttribute("width",Math.abs(currentX-startX).toFixed(2)); overlay.setAttribute("height",String(height));
    overlay.setAttribute("fill","rgba(15,118,110,.16)"); overlay.setAttribute("stroke","#0f766e"); overlay.setAttribute("stroke-width","1.5"); overlay.setAttribute("pointer-events","none");
  }
  function safeFileName() { return (document.title||"flamegraph").replace(/[\\/:*?"<>|]+/g,"_"); }
  function downloadBlob(blob,suffix) {
    const link=document.createElement("a"); const url=URL.createObjectURL(blob); link.href=url; link.download=safeFileName()+suffix; document.body.appendChild(link); link.click(); link.remove(); setTimeout(()=>URL.revokeObjectURL(url),1000);
  }
  function exportSubtitle() {
    const focus=state.navigation.map(item=>item.name||"root").join(" > ");
    const parts=[focus,weightText(state.focus.value||0),state.lastVisible+" visible frames"];
    if(state.lastHidden)parts.push(state.lastHidden+" hidden frames");
    if(state.minPercent)parts.push("minimum "+state.minPercent.toFixed(2).replace(/0+$/,"").replace(/\.$/,"")+"%");
    if(state.query)parts.push('search "'+state.query+'"');
    return parts.join(" · ");
  }
  function svgBlob() {
    const width=Math.max(1,svg.viewBox.baseVal.width), chartHeight=Math.max(1,svg.viewBox.baseVal.height), headerHeight=60;
    const wrapper=document.createElementNS(ns,"svg"); wrapper.setAttribute("xmlns",ns); wrapper.setAttribute("width",String(width)); wrapper.setAttribute("height",String(headerHeight+chartHeight)); wrapper.setAttribute("viewBox","0 0 "+width+" "+(headerHeight+chartHeight));
    const bg=document.createElementNS(ns,"rect"); bg.setAttribute("width","100%"); bg.setAttribute("height","100%"); bg.setAttribute("fill","#fff"); wrapper.appendChild(bg);
    const title=document.createElementNS(ns,"text"); title.setAttribute("x","12"); title.setAttribute("y","23"); title.setAttribute("fill","#172033"); title.setAttribute("font-family",'"Segoe UI",sans-serif'); title.setAttribute("font-size","16"); title.setAttribute("font-weight","600"); title.textContent=document.title||"wperf flame graph"; wrapper.appendChild(title);
    const subtitle=document.createElementNS(ns,"text"); subtitle.setAttribute("x","12"); subtitle.setAttribute("y","43"); subtitle.setAttribute("fill","#64748b"); subtitle.setAttribute("font-family",'"Segoe UI",sans-serif'); subtitle.setAttribute("font-size","11"); subtitle.textContent=exportSubtitle(); wrapper.appendChild(subtitle);
    const style=document.createElementNS(ns,"style"); style.textContent='text{font:12px "Segoe UI",sans-serif;fill:#1f2937}.frame{stroke:#fff;stroke-width:1}.match{stroke:#0f766e}.current-match{stroke:#0f766e;stroke-width:3}.selected{stroke:#111827;stroke-width:3}'; wrapper.appendChild(style);
    const source=svg.cloneNode(true); source.removeAttribute("id"); source.setAttribute("x","0"); source.setAttribute("y",String(headerHeight)); source.setAttribute("width",String(width)); source.setAttribute("height",String(chartHeight)); wrapper.appendChild(source);
    return new Blob([new XMLSerializer().serializeToString(wrapper)],{type:"image/svg+xml;charset=utf-8"});
  }
  async function downloadPngFile() {
    const url=URL.createObjectURL(svgBlob()); const image=new Image();
    await new Promise((resolve,reject)=>{image.onload=resolve;image.onerror=reject;image.src=url;}).catch(()=>null);
    if(!image.naturalWidth){URL.revokeObjectURL(url);return;}
    const canvas=document.createElement("canvas"); canvas.width=Math.max(1,image.naturalWidth*2); canvas.height=Math.max(1,image.naturalHeight*2);
    const context=canvas.getContext("2d"); context.fillStyle="#fff"; context.fillRect(0,0,canvas.width,canvas.height); context.drawImage(image,0,0,canvas.width,canvas.height); URL.revokeObjectURL(url);
    canvas.toBlob(blob=>{if(blob)downloadBlob(blob,".png");},"image/png");
  }
)JS"
LR"JS(
  function render() {
    const focus=state.focus, total=focus.value||0; totalValue.textContent=weightText(total); updateBreadcrumb();
    const width=Math.max(320,svg.parentElement.clientWidth||0); let maxDepth=0,visible=0,hidden=0; const matches=[]; state.frameAreas=[];
    const fragment=document.createDocumentFragment();
    function visit(node,x,depth,path) {
      const nodeWidth=total>0?width*node.value/total:0, nodePercent=total>0?100*node.value/total:0;
      if(nodeWidth<0.7||nodePercent<state.minPercent){hidden+=1;return;}
      maxDepth=Math.max(maxDepth,depth); ++visible;
      const currentPath=path.concat(node), lower=(node.name||"").toLowerCase(), matched=state.query&&lower.includes(state.query); let currentMatched=false;
      if(matched){currentMatched=matches.length===state.matchIndex;matches.push({node,path:currentPath});}
      const selected=state.selectedNode===node; const className="frame"+(matched?" match":"")+(currentMatched?" current-match":"")+(selected?" selected":"");
      const y=depth*ROW, rect=document.createElementNS(ns,"rect"); rect.setAttribute("x",x.toFixed(2)); rect.setAttribute("y",String(y)); rect.setAttribute("width",Math.max(0.4,nodeWidth).toFixed(2)); rect.setAttribute("height",String(ROW-1)); rect.setAttribute("rx","2"); rect.setAttribute("fill",color(node.name||"")); rect.setAttribute("stroke","#fff"); rect.setAttribute("stroke-width","1"); rect.setAttribute("class",className); rect.setAttribute("data-name",node.name||"");
      state.frameAreas.push({node,path:currentPath,total,rect,x,y,width:Math.max(0.4,nodeWidth),height:ROW-1});
      rect.addEventListener("mousemove",event=>{tooltip.textContent=(node.name||"")+"\n"+weightText(node.value)+"  ("+percentText(node.value,total)+")\nself: "+weightText(node.self);tooltip.style.display="block";tooltip.style.left=Math.max(8,Math.min(event.clientX+14,window.innerWidth-320))+"px";tooltip.style.top=Math.max(8,event.clientY+14)+"px";});
      rect.addEventListener("mouseleave",()=>{tooltip.style.display="none";}); fragment.appendChild(rect);
      if(nodeWidth>=36){const text=document.createElementNS(ns,"text");text.setAttribute("x",(x+4).toFixed(2));text.setAttribute("y",String(y+13));text.setAttribute("class","frame-label"+(matched?" match":"")+(currentMatched?" current-match":""));text.setAttribute("fill","#1f2937");text.setAttribute("font-size","12");text.setAttribute("font-family",'"Segoe UI",sans-serif');const maxChars=Math.max(2,Math.floor(nodeWidth/7)),label=node.name||"";text.textContent=label.length>maxChars?label.slice(0,maxChars-1)+"…":label;fragment.appendChild(text);}
      let childX=x; for(const child of(node.children||[])){const childWidth=total>0?width*child.value/total:0;visit(child,childX,depth+1,currentPath);childX+=childWidth;}
    }
    visit(focus,0,0,[]); state.matches=matches; state.lastVisible=visible; state.lastHidden=hidden;
    if(matches.length)state.matchIndex%=matches.length;else state.matchIndex=0;
    const height=Math.max(ROW,(maxDepth+1)*ROW+8); svg.setAttribute("viewBox","0 0 "+width+" "+height); svg.setAttribute("height",String(height)); svg.replaceChildren(fragment);
    const parts=[visible+" visible frames"]; if(hidden)parts.push(hidden+" hidden by width/minimum percent"); if(state.query)parts.push(matches.length?(state.matchIndex+1)+" / "+matches.length+" matches":"no matches");
    visibleStatus.textContent=parts.join(" · "); searchCounter.textContent=state.query?(matches.length?(state.matchIndex+1)+"/"+matches.length:"0/0"):"";
    if(state.selectedNode&&!state.frameAreas.some(area=>area.node===state.selectedNode))clearDetails();
  }
)JS"
LR"JS(
  search.addEventListener("input",()=>{state.query=search.value.trim().toLowerCase();state.matchIndex=0;render();});
  search.addEventListener("keydown",event=>{if(event.key==="Enter"){event.preventDefault();cycleMatch(event.shiftKey?-1:1);}});
  prevMatch.addEventListener("click",()=>cycleMatch(-1)); nextMatch.addEventListener("click",()=>cycleMatch(1));
  minPercent.addEventListener("input",()=>{state.minPercent=Number(minPercent.value)||0;minPercentValue.value=state.minPercent?state.minPercent.toFixed(2).replace(/0+$/,"").replace(/\.$/,"")+"%":"0%";state.matchIndex=0;render();});
  reset.addEventListener("click",resetView); up.addEventListener("click",goUp);
  zoomSelected.addEventListener("click",()=>{if(state.selectedNode)zoomTo(state.selectedNode);});
  copyStack.addEventListener("click",()=>{if(state.selectedPath.length)copyText(state.selectedPath.map(item=>frameDisplayName(item.name)).join("\n"),copyStack);});
  copyFolded.addEventListener("click",()=>{if(state.selectedNode)copyText(state.selectedPath.map(item=>frameDisplayName(item.name)).join(";")+" "+Math.round(state.selectedNode.value),copyFolded);});
  downloadSvg.addEventListener("click",()=>downloadBlob(svgBlob(),".svg")); downloadPng.addEventListener("click",downloadPngFile);

  svg.addEventListener("pointerdown",event=>{
    if(event.button!==0)return;
    const point=eventPoint(event); const targetRect=event.target&&event.target.closest?event.target.closest(".frame"):null;
    const hit=targetRect?state.frameAreas.find(area=>area.rect===targetRect)||null:hitTest(point);
    state.drag={pointerId:event.pointerId,startX:point.x,startY:point.y,currentX:point.x,currentY:point.y,hit,moved:false,captured:false};
  });
  svg.addEventListener("pointermove",event=>{
    if(!state.drag||state.drag.pointerId!==event.pointerId)return;
    const point=eventPoint(event); state.drag.currentX=point.x;state.drag.currentY=point.y;
    if(Math.abs(state.drag.currentX-state.drag.startX)>4)state.drag.moved=true;
    if(state.drag.moved){
      if(!state.drag.captured){try{svg.setPointerCapture(event.pointerId);state.drag.captured=true;}catch{}}
      let overlay=svg.querySelector("#range-overlay");if(!overlay){overlay=document.createElementNS(ns,"rect");overlay.id="range-overlay";svg.appendChild(overlay);}updateSelectionOverlay(overlay,state.drag.startX,state.drag.currentX,svg.viewBox.baseVal.height);tooltip.style.display="none";
    }
  });
  function finishDrag(event,cancelled=false){
    if(!state.drag||(event&&state.drag.pointerId!==event.pointerId))return; const drag=state.drag;
    if(event&&event.type!=="pointercancel"){const point=eventPoint(event);drag.currentX=point.x;drag.currentY=point.y;if(Math.abs(drag.currentX-drag.startX)>4)drag.moved=true;}
    state.drag=null;try{if(event&&svg.hasPointerCapture(event.pointerId))svg.releasePointerCapture(event.pointerId);}catch{}
    const overlay=svg.querySelector("#range-overlay");if(overlay)overlay.remove();tooltip.style.display="none";
    if(cancelled)return;
    if(!drag.moved){
      if(!drag.hit){state.lastActivation=null;clearDetails();return;}
      const now=performance.now(), previous=state.lastActivation;
      const doubleClick=previous&&previous.node===drag.hit.node&&now-previous.time<450;
      selectNode(drag.hit.node,drag.hit.path,drag.hit.total,drag.hit.rect);
      if(doubleClick){state.lastActivation=null;zoomTo(drag.hit.node);}else state.lastActivation={node:drag.hit.node,time:now};
      return;
    }
    state.lastActivation=null;const width=Math.max(1,svg.viewBox.baseVal.width),a=clamp(Math.min(drag.startX,drag.currentX)/width,0,1),b=clamp(Math.max(drag.startX,drag.currentX)/width,0,1);
    if(b-a<0.002)return; state.focus=selectionRoot(state.focus,a,b);state.navigation.push(state.focus);state.matchIndex=0;clearDetails();render();
  }
  svg.addEventListener("pointerup",event=>finishDrag(event,false)); svg.addEventListener("pointercancel",event=>finishDrag(event,true));
  document.addEventListener("keydown",event=>{if(event.key==="Escape")resetView();else if(event.key==="Backspace"&&document.activeElement!==search){event.preventDefault();goUp();}});
  window.addEventListener("resize",render); clearDetails(); render();
})();
)JS";

}  // namespace wperf::detail
