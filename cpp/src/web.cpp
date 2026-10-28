#include "mpdm/web.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <iostream>
#include <string>

#include "mpdm/config.hpp"
#include "mpdm/manager.hpp"
#include "mpdm/ratelimit.hpp"

using nlohmann::json;

namespace mpdm {

// The dashboard page — reskinned to the "Industrial" mockup (dark + light).
static const char* PAGE = R"PAGE(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1"/>
<title>MPDM</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Barlow:wght@400;500;600;700&family=Chakra+Petch:wght@400;500;600;700&family=JetBrains+Mono:wght@400;500;600&display=swap" rel="stylesheet">
<style>
:root{
  --bg:#0C1116; --surface:#222B35; --surface2:#18202A; --line:#3B4756;
  --text:#E6EBF0; --muted:#A9B4C1; --faint:#8B97A5;
  --amber:#FFA62B; --accent:#3BE0FF; --green:#6BD66F; --red:#FF6B60; --purple:#A68CFF; --sel:#2a2111;
}
:root[data-theme="light"]{
  --bg:#EEF2F6; --surface:#FFFFFF; --surface2:#F5F7FA; --line:#E4E9EF;
  --text:#18202A; --muted:#3E4A57; --faint:#8A95A3;
  --amber:#FFA62B; --accent:#0B8FAD; --green:#2E9E43; --red:#B3261E; --purple:#7C53D6; --sel:#FFF3DE;
}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);
  font:13px/1.45 "Barlow","Segoe UI",system-ui,sans-serif}
.mono{font-family:"JetBrains Mono",Consolas,monospace}
.h,thead th,button.btn,header .mode,.tile .lbl,.ranges span,.statusbar,.qhead b{
  font-family:"Chakra Petch","Segoe UI",sans-serif;text-transform:uppercase}
header .name{font-family:"Barlow",sans-serif;font-weight:700}
.h{font-size:10px;letter-spacing:1.4px;color:var(--faint)}
.muted{color:var(--muted)} .faint{color:var(--faint)}
.card{background:var(--surface);border:1px solid var(--line);border-radius:8px}
.app{display:flex;flex-direction:column;height:100vh}
header{display:flex;align-items:center;gap:10px;padding:0 14px;height:46px;
  background:var(--surface2);border-bottom:1px solid var(--line)}
header .name{font-weight:700;font-size:14px}
header .mode{font-size:11px;letter-spacing:1.5px;color:var(--faint)}
.spacer{flex:1}
.iconbtn{cursor:pointer;color:var(--muted);background:none;border:none;font-size:13px}
.stats{display:grid;grid-template-columns:repeat(4,1fr);gap:12px;padding:12px 14px}
.stat{display:flex;flex-direction:column;gap:8px;padding:12px 14px}
.statrow{display:flex;align-items:flex-end;justify-content:space-between}
.big{font-size:26px;font-weight:600;line-height:1}
.meter{display:flex;gap:2px;align-items:flex-end;height:20px}
.meter i{width:3px;height:18px;border-radius:1px;background:var(--line)}
.meter i.on{background:var(--accent)}
.dots{display:flex;gap:5px} .dots i{width:8px;height:8px;border-radius:50%;background:var(--line)}
.dots i.on{background:var(--accent)}
.toolbar{display:flex;align-items:center;gap:8px;padding:0 14px 12px}
button.btn{background:var(--surface);border:1px solid var(--line);color:var(--text);
  border-radius:7px;padding:8px 12px;cursor:pointer;font:inherit}
button.btn:hover{border-color:var(--amber)}
button.btn.primary{background:var(--amber);color:#0b0e13;border:none;font-weight:700}
button.btn:disabled{opacity:.45;cursor:default}
input,select{font:inherit;background:var(--surface2);color:var(--text);
  border:1px solid var(--line);border-radius:7px;padding:8px 10px}
.main{flex:1;display:grid;grid-template-columns:210px 1fr 300px;gap:12px;padding:0 14px 12px;min-height:0}
.col{display:flex;flex-direction:column;gap:12px;min-height:0}
.tiles{display:grid;grid-template-columns:1fr 1fr;gap:8px}
.tile{border:1px solid var(--line);border-radius:7px;padding:10px;background:var(--surface2)}
.tile.on{border-color:var(--amber);background:color-mix(in srgb,var(--amber) 12%,transparent)}
.tile .top{display:flex;justify-content:space-between;align-items:center}
.tile .lbl{font-size:10px;letter-spacing:.5px;font-weight:600;margin-top:8px}
.tile.on .lbl,.tile.on .cnt{color:var(--amber)}
.statuslist{display:flex;flex-direction:column;gap:11px}
.strow{display:flex;align-items:center;justify-content:space-between}
.strow .l{display:flex;align-items:center;gap:9px}
.dot{width:8px;height:8px;border-radius:50%}
.chartcard{padding:14px;display:flex;flex-direction:column;gap:10px}
.chhead{display:flex;align-items:center;justify-content:space-between}
.ranges{display:flex;gap:4px}
.ranges span{padding:3px 8px;border-radius:5px;color:var(--faint);cursor:pointer;font-size:12px}
.ranges span.on{background:color-mix(in srgb,var(--accent) 16%,transparent);color:var(--accent);font-weight:700}
#chart{width:100%;height:130px;display:block;background:var(--surface2);border-radius:6px}
.queue{flex:1;display:flex;flex-direction:column;overflow:hidden}
.qhead{display:flex;justify-content:space-between;padding:12px 14px;border-bottom:1px solid var(--line)}
table{width:100%;border-collapse:collapse}
thead th{position:sticky;top:0;background:var(--surface);text-align:left;padding:8px 10px;
  font-size:10px;letter-spacing:1px;color:var(--faint);border-bottom:1px solid var(--line)}
th.r,td.r{text-align:right}
.tbwrap{overflow:auto}
tbody td{padding:9px 10px;border-bottom:1px solid var(--line);vertical-align:middle}
tbody tr{cursor:pointer}
tbody tr.sel{background:var(--sel)}
tbody tr.sel .nm{color:var(--amber)}
.nm{font-weight:500}
.err{color:var(--red);font-size:11px;margin-top:2px}
.bar{height:8px;border-radius:3px;background:var(--line);overflow:hidden;min-width:90px}
.bar>.fill{height:100%;background:repeating-linear-gradient(90deg,currentColor 0 6px,transparent 6px 9px)}
.pct{color:var(--muted);margin-left:8px}
.state{display:flex;align-items:center;gap:7px;color:var(--muted)}
.selpanel{padding:14px;display:flex;flex-direction:column;gap:12px;overflow:auto}
.selhead{display:flex;justify-content:space-between;align-items:center}
.selname{font-weight:600}
.pctbig{font-size:30px;font-weight:700}
.conns{display:flex;flex-direction:column;gap:9px}
.crow{display:flex;align-items:center;gap:8px}
.crow .cl{width:24px;color:var(--faint)}
.crow .cbar{flex:1;height:6px;border-radius:3px;background:var(--line);overflow:hidden}
.crow .cbar>i{display:block;height:100%;background:repeating-linear-gradient(90deg,currentColor 0 6px,transparent 6px 9px)}
.crow .cs{width:36px;text-align:right;color:var(--muted)}
.note{border-left:2px solid var(--accent);background:color-mix(in srgb,var(--accent) 8%,transparent);
  padding:9px 10px;border-radius:0 6px 6px 0;color:var(--muted);font-size:11.5px}
.kv{background:var(--surface2);border-radius:6px;padding:12px;display:flex;flex-direction:column;gap:8px}
.kv .row{display:flex;justify-content:space-between}
.selbtns{display:flex;gap:10px;margin-top:auto}
.selbtns button{flex:1}
.statusbar{display:flex;align-items:center;gap:18px;height:30px;padding:0 14px;
  background:var(--surface2);border-top:1px solid var(--line);color:var(--muted);font-size:11px}
.sbit{display:flex;align-items:center;gap:7px}
</style>
)PAGE"
R"PAGE(
</head>
<body>
<div class="app">
  <header>
    <svg width="24" height="24" viewBox="0 0 100 100">
      <path d="M26 13 L86 13 L86 55 L50 88 L14 55 L14 25 Z" fill="#2b3542" stroke="var(--amber)" stroke-width="6" stroke-linejoin="round"/>
      <circle cx="26" cy="24" r="3.5" fill="var(--amber)"/>
      <path d="M45 23 L55 23 L55 41 L45 41 Z" fill="var(--amber)"/>
      <path d="M35 40 L65 40 L50 63 Z" fill="var(--accent)"/>
      <path d="M32 63 L50 78 L68 63" fill="none" stroke="#fff" stroke-width="7" stroke-linejoin="round" stroke-linecap="round"/>
    </svg>
    <span class="name">MyPersonalDownloadManager</span>
    <span class="mode">DOWNLOAD CONTROL</span>
    <span class="spacer"></span>
    <button class="iconbtn" id="theme" title="Toggle theme">◐ THEME</button>
  </header>

  <div class="stats">
    <div class="stat card"><span class="h">TOTAL SPEED</span>
      <div class="statrow"><span class="big mono" id="s_speed">0</span><div class="meter" id="s_meter"></div></div></div>
    <div class="stat card"><span class="h">ACTIVE SLOTS</span>
      <div class="statrow"><span class="big mono" id="s_slots">0/0</span><div class="dots" id="s_dots"></div></div></div>
    <div class="stat card"><span class="h">DOWNLOADED TODAY</span>
      <div class="statrow"><span class="big mono" id="s_today">0</span><span class="muted" id="s_files">0 files</span></div></div>
    <div class="stat card"><span class="h">SCHEDULER</span>
      <div class="statrow"><span class="big mono" id="s_sched">OFF</span><span class="muted">status</span></div></div>
  </div>

  <div class="toolbar">
    <button class="btn primary" id="add">+ ADD</button>
    <button class="btn" id="resume">RESUME</button>
    <button class="btn" id="pause">PAUSE</button>
    <button class="btn" id="remove">REMOVE</button>
    <button class="btn" id="clear">CLEAR</button>
    <span class="spacer"></span>
    <input id="search" placeholder="Search downloads" style="width:220px"/>
    <span class="faint">LIMIT</span>
    <select id="glimit">
      <option value="">UNLIMITED</option><option value="500k">500 KB/s</option>
      <option value="1m">1 MB/s</option><option value="2m">2 MB/s</option><option value="5m">5 MB/s</option>
    </select>
  </div>

  <div class="main">
    <div class="col">
      <div class="card" style="padding:12px"><span class="h">CATEGORIES</span>
        <div class="tiles" id="cats" style="margin-top:10px"></div></div>
      <div class="card" style="padding:12px"><span class="h">STATUS</span>
        <div class="statuslist" id="statuslist" style="margin-top:10px"></div></div>
      <div style="flex:1"></div>
      <div class="card" style="padding:12px"><span class="h">OUTPUT</span>
        <div class="mono" id="outdir" style="margin-top:6px">D:\Downloads</div>
        <div class="faint" style="margin-top:4px">Sorted by category</div></div>
    </div>

    <div class="col">
      <div class="card chartcard">
        <div class="chhead"><div style="display:flex;gap:14px;align-items:center">
          <span class="h">THROUGHPUT</span>
          <span class="faint">NOW <b class="mono" id="t_now" style="color:var(--accent)">0</b></span>
          <span class="faint">PEAK <b class="mono" id="t_peak">0</b></span></div>
          <div class="ranges"><span>1M</span><span class="on">10M</span><span>1H</span><span>10H</span></div>
        </div>
        <canvas id="chart"></canvas>
      </div>
      <div class="card queue">
        <div class="qhead"><span><b class="h">QUEUE</b> <span class="h" id="qcount">0 ITEMS</span></span>
          <span class="faint">Newest first</span></div>
        <div class="tbwrap"><table>
          <thead><tr><th>NAME</th><th class="r">SIZE</th><th>PROGRESS</th><th class="r">SPEED</th><th>STATE</th></tr></thead>
          <tbody id="rows"></tbody>
        </table></div>
      </div>
    </div>

    <div class="col">
      <div class="card selpanel" id="selpanel">
        <div class="selhead"><span class="h">SELECTED</span><span class="state" id="sel_state"></span></div>
        <div><div class="selname" id="sel_name">No selection</div><div class="faint mono" id="sel_host"></div></div>
        <div class="statrow"><span class="pctbig mono" id="sel_pct">—</span><span class="mono muted" id="sel_size"></span></div>
        <div class="bar" style="height:8px"><div class="fill" id="sel_bar" style="width:0%;color:var(--accent)"></div></div>
        <span class="h">CONNECTIONS</span>
        <div class="conns" id="sel_conns"></div>
        <div class="note" id="sel_note" style="display:none"></div>
        <div class="kv" id="sel_kv"></div>
        <div class="selbtns">
          <button class="btn" id="sel_pause">PAUSE</button>
          <button class="btn" id="sel_open">OPEN FOLDER</button>
        </div>
      </div>
    </div>
  </div>

  <div class="statusbar">
    <span class="sbit"><span class="dot" style="background:var(--green)"></span><span id="sb_net">NET 0</span></span>
    <span class="sbit"><span class="dot" style="background:var(--accent)"></span><span id="sb_slots">SLOTS 0/0</span></span>
    <span class="sbit"><span class="dot" style="background:var(--amber)"></span>SCHED OFF</span>
    <span class="spacer"></span>
    <span class="faint" id="sb_out"></span>
  </div>
</div>
)PAGE"
R"PAGE(
<script>
const $=s=>document.querySelector(s);
let selId=null, state={downloads:[]}, samples=[], peak=1;

// theme
const savedTheme=localStorage.getItem("mpdm-theme"); if(savedTheme)document.documentElement.dataset.theme=savedTheme;
$("#theme").onclick=()=>{const l=document.documentElement.dataset.theme==="light";
  document.documentElement.dataset.theme=l?"":"light";localStorage.setItem("mpdm-theme",l?"":"light");draw();};

function human(n){if(n==null)return "—";n=+n;const u=["B","KB","MB","GB","TB"];let i=0;
  while(Math.abs(n)>=1024&&i<u.length-1){n/=1024;i++;}return (i?n.toFixed(1):n.toFixed(0))+u[i];}
function htime(s){if(s==null)return "—";s=Math.round(s);const h=s/3600|0,m=(s%3600)/60|0;
  return h?h+"h "+m+"m":m?m+" min":s+"s";}
function cvar(n){return getComputedStyle(document.documentElement).getPropertyValue(n).trim();}
function stColor(st){return {downloading:"var(--accent)",paused:"var(--purple)",completed:"var(--green)",
  error:"var(--red)",queued:"var(--faint)"}[st]||"var(--faint)";}

async function api(p,b){const r=await fetch(p,{method:b?"POST":"GET",
  headers:b?{"Content-Type":"application/json"}:{},body:b?JSON.stringify(b):undefined});
  state=await r.json();render();}

const CATS=[["ALL","12"],["VIDEO"],["AUDIO"],["DOCS"],["ARCHIVE"],["APPS"],["OTHER"],["PRIVATE"]];
function render(){
  const list=state.downloads||[];
  const filter=($("#search").value||"").toLowerCase();
  const shown=list.filter(d=>!filter||(d.filename||d.url||"").toLowerCase().includes(filter));
  // stats
  let gspeed=0,active=0,dl=0,fin=0;
  for(const d of list){if(d.status==="downloading"){gspeed+=d.speed||0;active++;}dl+=d.downloaded||0;if(d.status==="completed")fin++;}
  $("#s_speed").textContent=human(gspeed)+"/s";
  $("#s_slots").textContent=active+"/"+(state.max_concurrent||0);
  $("#s_today").textContent=human(dl); $("#s_files").textContent=fin+" files";
  peak=Math.max(peak,gspeed);
  $("#t_now").textContent=human(gspeed)+"/s"; $("#t_peak").textContent=human(peak)+"/s";
  $("#sb_net").textContent="NET "+human(gspeed)+"/s"; $("#sb_slots").textContent="SLOTS "+active+"/"+(state.max_concurrent||0);
  $("#sb_out").textContent="SAVE TO "+(state.output_dir||""); $("#outdir").textContent=state.output_dir||"D:\\Downloads";
  // meter + dots
  const mf=Math.round(Math.min(1,gspeed/(peak||1))*12);
  $("#s_meter").innerHTML=Array.from({length:12},(_,i)=>`<i class="${i<mf?'on':''}"></i>`).join("");
  const mc=state.max_concurrent||5;
  $("#s_dots").innerHTML=Array.from({length:mc},(_,i)=>`<i class="${i<active?'on':''}"></i>`).join("");
  // categories
  const counts={}; for(const d of list)counts[(d.category||"Other")]=(counts[(d.category||"Other")]||0)+1;
  const catmap={ALL:list.length,VIDEO:counts.Video||0,AUDIO:counts.Audio||0,DOCS:counts.Documents||0,
    ARCHIVE:counts.Archives||0,APPS:counts.Programs||0,OTHER:counts.Other||0,PRIVATE:0};
  $("#cats").innerHTML=Object.keys(catmap).map((k,i)=>`<div class="tile ${i===0?'on':''}">
    <div class="top"><span class="h">${k[0]}</span><span class="cnt mono faint">${catmap[k]||''}</span></div>
    <div class="lbl">${k}</div></div>`).join("");
  // status list
  const sc={Downloading:0,Queued:0,Paused:0,Finished:0,Failed:0};
  for(const d of list){({downloading:"Downloading",queued:"Queued",paused:"Paused",completed:"Finished",error:"Failed"})[d.status]&&sc[({downloading:"Downloading",queued:"Queued",paused:"Paused",completed:"Finished",error:"Failed"})[d.status]]++;}
  const scol={Downloading:"var(--accent)",Queued:"var(--faint)",Paused:"var(--purple)",Finished:"var(--green)",Failed:"var(--red)"};
  $("#statuslist").innerHTML=Object.keys(sc).map(k=>`<div class="strow"><span class="l">
    <span class="dot" style="background:${scol[k]}"></span>${k}</span><span class="mono muted">${sc[k]}</span></div>`).join("");
  // table
  $("#qcount").textContent=list.length+" ITEMS";
  $("#rows").innerHTML=shown.map(d=>{
    const pct=d.percent==null?0:Math.min(100,d.percent);
    const err=d.error?`<div class="err">${d.error}</div>`:"";
    return `<tr data-id="${d.id}" class="${d.id===selId?'sel':''}">
      <td><div class="nm">${d.filename||d.url}</div>${err}</td>
      <td class="r mono">${human(d.size)}</td>
      <td><div style="display:flex;align-items:center"><div class="bar"><div class="fill" style="width:${pct}%;color:${stColor(d.status)}"></div></div>
        <span class="pct mono">${d.percent==null?'0':pct.toFixed(0)}%</span></div></td>
      <td class="r mono">${d.status==="downloading"&&d.speed?human(d.speed)+"/s":"—"}</td>
      <td><span class="state"><span class="dot" style="background:${stColor(d.status)}"></span>${d.status}</span></td></tr>`;
  }).join("");
  $("#rows").querySelectorAll("tr").forEach(tr=>tr.onclick=()=>{selId=tr.dataset.id;render();});
  renderSelected(list);
  draw();
}

function renderSelected(list){
  let d=list.find(x=>x.id===selId); if(!d&&list.length){d=list[0];selId=d.id;}
  if(!d){$("#sel_name").textContent="No selection";$("#sel_conns").innerHTML="";return;}
  const pct=d.percent==null?0:Math.min(100,d.percent);
  $("#sel_state").innerHTML=`<span class="dot" style="background:${stColor(d.status)}"></span><span style="color:${stColor(d.status)}">${d.status}</span>`;
  $("#sel_name").textContent=d.filename||d.url; $("#sel_host").textContent=d.url;
  $("#sel_pct").textContent=(d.percent==null?0:pct.toFixed(0))+"%";
  $("#sel_size").textContent=human(d.downloaded)+" / "+human(d.size);
  const sb=$("#sel_bar");sb.style.width=pct+"%";sb.style.color=stColor(d.status);
  const cs=d.conn_stats||[];
  $("#sel_conns").innerHTML=cs.slice(0,8).map((c,i)=>{
    const col=i===0?"var(--amber)":"var(--accent)";const f=Math.round((c.fraction||0)*100);
    return `<div class="crow"><span class="cl mono">C${c.index+1}</span>
      <div class="cbar"><i style="width:${f}%;color:${col}"></i></div>
      <span class="cs mono">${((c.speed||0)/1048576).toFixed(1)}</span></div>`;}).join("");
  const stolen=cs.find(c=>c.stole_from>=0);
  const note=$("#sel_note");
  if(stolen){note.style.display="block";note.textContent=`C${stolen.index+1} finished its own range early and took over part of C${stolen.stole_from+1}'s remainder.`;}
  else note.style.display="none";
  $("#sel_kv").innerHTML=[["SPEED",d.status==="downloading"?human(d.speed)+"/s":"—"],
    ["TIME LEFT",d.status==="downloading"?htime(d.eta):"—"],
    ["LIMIT",d.max_rate_bps?human(d.max_rate_bps)+"/s":"Unlimited"],
    ["STATUS",d.status],["SAVE TO",d.dest_path||"—"]]
    .map(([k,v])=>`<div class="row"><span class="faint">${k}</span><span class="mono">${v}</span></div>`).join("");
  $("#sel_pause").onclick=()=>api(d.status==="paused"?"/api/resume":"/api/pause",{id:d.id});
}

// throughput chart
function draw(){
  const c=$("#chart");const r=c.getBoundingClientRect();c.width=r.width;c.height=r.height;
  const g=c.getContext("2d");g.clearRect(0,0,c.width,c.height);
  const line=cvar("--line"),acc=cvar("--accent");
  g.strokeStyle=line;g.lineWidth=1;
  for(let i=1;i<4;i++){const y=c.height*i/4;g.beginPath();g.moveTo(0,y);g.lineTo(c.width,y);g.stroke();}
  if(samples.length<2)return;
  const n=80,sc=Math.max(peak*1.15,1);
  const X=i=>c.width*i/(n-1),Y=v=>c.height-(v/sc)*c.height*0.9;
  const off=n-samples.length;
  g.beginPath();g.moveTo(X(off),Y(samples[0]));
  for(let i=1;i<samples.length;i++)g.lineTo(X(off+i),Y(samples[i]));
  g.strokeStyle=acc;g.lineWidth=2;g.stroke();
  g.lineTo(X(n-1),c.height);g.lineTo(X(off),c.height);g.closePath();
  g.fillStyle=acc+"22";g.fill();
}

// actions
$("#add").onclick=()=>{const u=prompt("Paste a URL (ranges like [1-100] expand into many):");
  if(u&&u.trim())api("/api/add",{url:u.trim(),connections:8});};
$("#resume").onclick=()=>selId&&api("/api/resume",{id:selId});
$("#pause").onclick=()=>selId&&api("/api/pause",{id:selId});
$("#remove").onclick=()=>selId&&confirm("Remove this download?")&&api("/api/remove",{id:selId,delete:false});
$("#clear").onclick=()=>api("/api/clear",{});
$("#glimit").onchange=e=>api("/api/global_rate",{rate:e.target.value});
$("#search").oninput=render;
$("#sel_open").onclick=()=>{};
window.addEventListener("resize",draw);

async function poll(){try{const r=await fetch("/api/state");state=await r.json();
  let g=0;for(const d of (state.downloads||[]))if(d.status==="downloading")g+=d.speed||0;
  samples.push(g);while(samples.length>80)samples.shift();render();}catch(e){}
  setTimeout(poll,1000);}
poll();
</script>
</body>
</html>)PAGE";

static json item_to_json(const std::shared_ptr<DownloadItem>& it) {
    json j;
    j["id"] = it->id;
    j["url"] = it->url;
    j["filename"] = it->filename;
    j["size"] = it->size ? json(*it->size) : json(nullptr);
    j["downloaded"] = it->downloaded;
    auto pct = it->percent();
    j["percent"] = pct ? json(*pct) : json(nullptr);
    j["speed"] = it->speed;
    j["eta"] = it->eta ? json(*it->eta) : json(nullptr);
    j["status"] = it->status;
    j["category"] = it->category();
    j["max_rate_bps"] = it->max_rate_bps;
    j["dest_path"] = it->dest_path;
    j["error"] = it->error;
    json cs = json::array();
    for (const auto& c : it->conn_stats)
        cs.push_back({{"index", c.index},
                      {"downloaded", c.downloaded},
                      {"size", c.size()},
                      {"fraction", c.fraction()},
                      {"speed", c.speed_bps},
                      {"active", c.active},
                      {"stole_from", c.stole_from}});
    j["conn_stats"] = cs;
    return j;
}

static json state_json(DownloadManager& m) {
    json dl = json::array();
    for (auto& it : m.ordered()) dl.push_back(item_to_json(it));
    return {{"downloads", dl},
            {"global_rate_bps", m.global_rate_bps()},
            {"max_concurrent", m.max_concurrent},
            {"output_dir", m.settings.output_dir}};
}

// parse_rate on a possibly-missing/empty JSON value ("" -> 0, like the JS UI).
static long long rate_of(const json& body, const char* key = "rate") {
    if (!body.contains(key) || body[key].is_null()) return 0;
    std::string s = body[key].is_string() ? body[key].get<std::string>() : body[key].dump();
    return parse_rate(s);
}

static std::string id_of(const json& body) {
    if (body.contains("id") && body["id"].is_string()) return body["id"].get<std::string>();
    return "";
}

static void open_in_browser(const std::string& url) {
#ifdef _WIN32
    std::system(("start \"\" \"" + url + "\"").c_str());
#elif defined(__APPLE__)
    std::system(("open \"" + url + "\" >/dev/null 2>&1 &").c_str());
#else
    std::system(("xdg-open \"" + url + "\" >/dev/null 2>&1 &").c_str());
#endif
}

int run_web(const std::string& host, int port, bool open_browser) {
    DownloadManager manager(Settings{}, 3, 0);
    manager.start_scheduler();
    httplib::Server svr;

    auto send_json = [](httplib::Response& res, const json& j, int code = 200) {
        res.status = code;
        res.set_content(j.dump(), "application/json");
    };

    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(PAGE, "text/html; charset=utf-8");
    });
    svr.Get("/index.html", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(PAGE, "text/html; charset=utf-8");
    });
    svr.Get("/api/state", [&](const httplib::Request&, httplib::Response& res) {
        send_json(res, state_json(manager));
    });

    auto handle_post = [&](const httplib::Request& req, httplib::Response& res,
                           const std::string& path) {
        json body;
        if (!req.body.empty()) {
            try {
                body = json::parse(req.body);
            } catch (...) {
                body = json::object();
            }
        } else {
            body = json::object();
        }
        try {
            if (path == "/api/add") {
                std::string url = body.value("url", "");
                size_t a = url.find_first_not_of(" \t\r\n");
                size_t b = url.find_last_not_of(" \t\r\n");
                url = (a == std::string::npos) ? "" : url.substr(a, b - a + 1);
                if (url.empty()) return send_json(res, {{"error", "no url"}}, 400);
                int conns = 0;
                if (body.contains("connections") && body["connections"].is_number())
                    conns = body["connections"].get<int>();
                bool start = body.value("start", true);
                manager.add_batch(url, conns, start, rate_of(body));
            } else if (path == "/api/pause") {
                manager.pause(id_of(body));
            } else if (path == "/api/resume") {
                manager.resume(id_of(body));
            } else if (path == "/api/remove") {
                manager.remove(id_of(body), body.value("delete", false));
            } else if (path == "/api/item_rate") {
                manager.set_item_rate(id_of(body), rate_of(body));
            } else if (path == "/api/global_rate") {
                manager.set_global_rate(static_cast<double>(rate_of(body)));
            } else if (path == "/api/pause_all") {
                manager.pause_all();
            } else if (path == "/api/resume_all") {
                manager.resume_all();
            } else if (path == "/api/clear") {
                manager.clear_finished();
            } else {
                return send_json(res, {{"error", "not found"}}, 404);
            }
        } catch (const std::exception& e) {
            return send_json(res, {{"error", e.what()}}, 500);
        }
        send_json(res, state_json(manager));
    };

    for (const char* p : {"/api/add", "/api/pause", "/api/resume", "/api/remove", "/api/item_rate",
                          "/api/global_rate", "/api/pause_all", "/api/resume_all", "/api/clear"}) {
        std::string path = p;
        svr.Post(path, [&handle_post, path](const httplib::Request& req, httplib::Response& res) {
            handle_post(req, res, path);
        });
    }

    std::string url = "http://" + host + ":" + std::to_string(port) + "/";
    std::cout << "MPDM web UI on " << url << "  (Ctrl+C to stop)\n";
    if (open_browser) open_in_browser(url);

    if (!svr.listen(host, port)) {
        std::cerr << "Error: could not bind " << host << ":" << port << "\n";
        return 1;
    }
    return 0;
}

} // namespace mpdm
