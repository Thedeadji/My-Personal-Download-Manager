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

// Dashboard page — built to the "Industrial" design spec (DESIGN.md + reference/).
// Split into adjacent raw-string literals to stay under MSVC's per-literal limit.
static const char* PAGE =
R"PAGE(<!doctype html>
<html lang="en" data-theme="dark">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1"/>
<title>MPDM</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Barlow:wght@400;500&family=Chakra+Petch:wght@500;600&family=JetBrains+Mono:wght@400;500&display=swap" rel="stylesheet">
<style>
:root[data-theme="dark"]{
  --frame:#1A2129;--panel:#222B35;--panelHi:#2B3643;--well:#0C1116;
  --row:#131A21;--rowAlt:#10161C;--rowSel:#2B2111;--cellOff:#1B242E;--lampOff:#2A3542;--grid:#18212B;
  --bevelL:#3B4756;--bevelD:rgba(0,0,0,.8);--insetD:rgba(0,0,0,.9);--insetL:#2E3946;
  --text:#E6EBF0;--text2:#A9B4C1;--text3:#8B97A5;
  --amber:#FFA62B;--amberInk:#1C1203;--amberTrack:#3A2A10;--amberText:#FFA62B;
  --cyan:#3BE0FF;--cyanText:#3BE0FF;--cyanTrack:#0E2A33;
  --green:#6BD66F;--red:#FF6B60;--violet:#A68CFF;--queued:#8B97A5;
  --glow:0 0 6px;
}
:root[data-theme="light"]{
  --frame:#C9D1DA;--panel:#E4E9EF;--panelHi:#D5DCE4;--well:#F5F7FA;
  --row:#EEF2F6;--rowAlt:#E8EDF2;--rowSel:#F8E9CF;--cellOff:#CCD4DD;--lampOff:#CCD4DD;--grid:#DCE2E9;
  --bevelL:#FFFFFF;--bevelD:rgba(40,55,70,.35);--insetD:rgba(30,40,50,.28);--insetL:#FFFFFF;
  --text:#18202A;--text2:#3E4A57;--text3:#56626F;
  --amber:#FFA62B;--amberInk:#1C1203;--amberTrack:#F3DFC0;--amberText:#8F5200;
  --cyan:#0B8FAD;--cyanText:#00708A;--cyanTrack:#CDE9F0;
  --green:#2E9E43;--red:#C9372C;--violet:#6A4FD6;--queued:#8A95A3;
  --glow:0 0 0;
}
*{box-sizing:border-box}
html,body{height:100%}
body{margin:0;background:var(--frame);color:var(--text);font:400 13px/18px 'Barlow',sans-serif}
.lbl{font:600 11px/14px 'Chakra Petch',sans-serif;letter-spacing:.1em;text-transform:uppercase;color:var(--text3)}
.mono{font-family:'JetBrains Mono',monospace}
.cham{clip-path:polygon(8px 0,100% 0,100% calc(100% - 8px),calc(100% - 8px) 100%,0 100%,0 8px)}
.chamTL{clip-path:polygon(8px 0,100% 0,100% 100%,0 100%,0 8px)}
.panel{background:var(--panel);box-shadow:inset 1px 1px 0 var(--bevelL),inset -1px -1px 0 var(--bevelD)}
.well{background:var(--well);box-shadow:inset 1px 1px 0 var(--insetD),inset -1px -1px 0 var(--insetL)}
.app{min-height:100vh;display:flex;flex-direction:column;gap:6px;padding:6px;box-shadow:inset 1px 1px 0 var(--bevelL)}
header{display:flex;align-items:center;gap:10px;min-height:44px;padding:0 6px 0 12px;background:var(--panelHi);
  box-shadow:inset 1px 1px 0 var(--bevelL),inset -1px -1px 0 var(--bevelD)}
.title{font:600 15px/20px 'Chakra Petch',sans-serif}
.sub{font:600 11px/14px 'Chakra Petch',sans-serif;letter-spacing:.1em;text-transform:uppercase;color:var(--text3)}
.wc{width:44px;height:44px;border:0;background:transparent;color:var(--text2);cursor:pointer;display:grid;place-items:center}
.readouts{display:flex;flex-wrap:wrap;gap:6px}
.tile{flex:1 1 220px;padding:9px 12px 11px;display:flex;flex-direction:column;gap:6px}
.tile .v{display:flex;align-items:flex-end;gap:6px}
.big{font:500 28px/32px 'JetBrains Mono',monospace}
.unit{font:400 13px/22px 'JetBrains Mono',monospace;color:var(--text2)}
.grow{flex:1 1 auto}
.toolbar{display:flex;flex-wrap:wrap;align-items:center;gap:6px;padding:6px}
.btn{display:inline-flex;align-items:center;gap:6px;min-height:44px;padding:0 12px 0 10px;border:0;cursor:pointer;
  background:var(--panelHi);color:var(--text);font:600 12px/16px 'Chakra Petch',sans-serif;letter-spacing:.06em;
  text-transform:uppercase;box-shadow:inset 1px 1px 0 var(--bevelL),inset -1px -1px 0 var(--bevelD);
  clip-path:polygon(5px 0,100% 0,100% calc(100% - 5px),calc(100% - 5px) 100%,0 100%,0 5px)}
.btn svg{width:16px;height:16px;stroke:var(--text2);fill:none;stroke-width:2}
.btn:disabled{opacity:.38;cursor:default}
.btn.primary{background:var(--amber);color:var(--amberInk);box-shadow:inset 1px 1px 0 #FFD58F,inset -1px -1px 0 #8A4F06,0 0 12px rgba(255,166,43,.35)}
.btn.primary svg{stroke:var(--amberInk)}
.divider{width:2px;height:26px;background:var(--well);box-shadow:-1px 0 0 var(--bevelL)}
.search{display:flex;align-items:center;gap:8px;min-height:44px;padding:0 10px;width:220px;background:var(--well);
  box-shadow:inset 1px 1px 0 var(--insetD),inset -1px -1px 0 var(--insetL)}
.search svg{width:16px;height:16px;stroke:var(--text3);fill:none;stroke-width:2}
.search input{flex:1;min-width:0;border:0;outline:none;background:transparent;color:var(--text);font:400 13px/18px 'Barlow',sans-serif}
.sel{display:inline-flex;align-items:center;gap:8px;min-height:44px;padding:0 10px;border:0;cursor:pointer;background:var(--well);
  color:var(--text);font:400 11px/14px 'JetBrains Mono',monospace;box-shadow:inset 1px 1px 0 var(--insetD),inset -1px -1px 0 var(--insetL)}
main{display:flex;flex-wrap:wrap;gap:6px;flex:1 1 auto}
aside{flex:1 1 216px;max-width:216px;display:flex;flex-direction:column;gap:6px}
.p{padding:10px 12px 12px;display:flex;flex-direction:column;gap:8px}
.cats{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:6px}
.slot{position:relative;height:58px;border:1px solid transparent;cursor:pointer;background:var(--well);
  display:flex;flex-direction:column;align-items:center;justify-content:center;gap:5px;
  box-shadow:inset 1px 1px 0 var(--insetD),inset -1px -1px 0 var(--insetL)}
.slot svg{width:20px;height:20px;stroke:var(--text2);fill:none;stroke-width:2}
.slot .sl{font:600 10px/12px 'Chakra Petch',sans-serif;letter-spacing:.08em;text-transform:uppercase;color:var(--text2)}
.slot .cn{position:absolute;top:3px;right:5px;font:400 11px/14px 'JetBrains Mono',monospace;color:var(--text2)}
.slot.on{border-color:var(--amber);box-shadow:inset 1px 1px 0 var(--insetD),inset -1px -1px 0 var(--insetL),0 0 8px rgba(255,166,43,.3)}
.slot.on svg{stroke:var(--amber)} .slot.on .sl,.slot.on .cn{color:var(--amber)}
.strow{display:flex;align-items:center;gap:10px;padding:6px 4px}
.lamp{width:7px;height:7px;border-radius:50%;flex:0 0 auto}
.out{padding:8px 10px;display:flex;flex-direction:column;gap:3px}
.center{flex:999 1 560px;min-width:0;display:flex;flex-direction:column;gap:6px}
.ranges{display:flex;gap:2px}
.ranges button{border:0;background:transparent;cursor:pointer;padding:4px 6px;font:400 11px/14px 'JetBrains Mono',monospace;color:var(--text3)}
.ranges button.on{color:var(--text);border-bottom:2px solid var(--cyan);padding-bottom:2px}
#chart{display:block;width:100%;height:118px}
.queue{flex:1 1 auto;padding-top:10px;display:flex;flex-direction:column;gap:6px;overflow:hidden}
.qhrow{display:flex;align-items:center;gap:10px;padding:0 12px}
.tbwrap{overflow:auto}
.colh{display:flex;align-items:center;gap:12px;padding:5px 10px;background:var(--well);
  font:600 11px/14px 'Chakra Petch',sans-serif;letter-spacing:.1em;text-transform:uppercase;color:var(--text3)}
.rw{position:relative;display:flex;align-items:center;gap:12px;padding:7px 10px;cursor:pointer}
.rw .ic{width:28px;height:28px;flex:0 0 auto;display:grid;place-items:center;background:var(--well);
  box-shadow:inset 1px 1px 0 var(--insetD),inset -1px -1px 0 var(--insetL)}
.rw .ic svg{width:16px;height:16px;stroke:var(--text2);fill:none;stroke-width:2}
.rw .nm{font:400 13px/18px 'Barlow',sans-serif;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.rw .err{font:400 12px/16px 'Barlow',sans-serif;color:var(--red)}
.rw.sel{background:var(--rowSel);box-shadow:inset 2px 0 0 var(--amber)}
.rw.sel .nm{color:var(--amberText)} .rw.sel .ic svg{stroke:var(--amber)}
.c-size{width:64px;flex:0 0 auto;font:400 11px/14px 'JetBrains Mono',monospace;color:var(--text2)}
.c-prog{width:140px;flex:0 0 auto;display:flex;align-items:center;gap:6px}
.c-speed{width:70px;flex:0 0 auto;font:400 11px/14px 'JetBrains Mono',monospace}
.c-state{width:96px;flex:0 0 auto;display:flex;align-items:center;gap:6px;font:400 12px/16px 'Barlow',sans-serif;color:var(--text2)}
.led{display:flex;gap:1px}
.led span{width:4px;height:8px;background:var(--cellOff)}
.right{flex:1 1 300px;max-width:320px;display:flex;flex-direction:column}
.right .p{flex:1 1 auto;gap:10px}
.selbig{font:500 28px/32px 'JetBrains Mono',monospace}
.ledbig{display:flex;gap:1px}.ledbig span{height:10px;flex:1 1 auto;background:var(--cellOff)}
.lanes{display:flex;flex-direction:column;gap:6px}
.lane{display:flex;align-items:center;gap:8px}
.lane .cl{width:26px;font:400 11px/14px 'JetBrains Mono',monospace;color:var(--text3)}
.lane .lb{flex:1;height:8px;background:var(--cyanTrack);overflow:hidden}
.lane .lb>i{display:block;height:100%;background:var(--cyan)}
.lane .ls{width:36px;text-align:right;font:400 11px/14px 'JetBrains Mono',monospace;color:var(--text2)}
.note{border-left:2px solid var(--amber);padding:6px 10px;color:var(--text2);font:400 12px/16px 'Barlow',sans-serif}
.kv{padding:10px;display:flex;flex-direction:column;gap:7px}
.kv .r{display:flex;justify-content:space-between;gap:10px}
.kv .k{font:600 11px/14px 'Chakra Petch',sans-serif;letter-spacing:.08em;text-transform:uppercase;color:var(--text3)}
.kv .vv{font:400 11px/14px 'JetBrains Mono',monospace}
.selbtns{display:flex;gap:6px;margin-top:auto}
.selbtns .btn{flex:1;justify-content:center;padding:0 8px}
.statusbar{display:flex;align-items:center;gap:18px;min-height:24px;padding:0 8px}
.sbit{display:flex;align-items:center;gap:7px;font:600 11px/14px 'Chakra Petch',sans-serif;letter-spacing:.08em;text-transform:uppercase;color:var(--text3)}
</style>
</head>
)PAGE"
R"PAGE(<body>
<div class="app">
  <header class="chamTL">
    <svg width="22" height="22" viewBox="0 0 64 64"><path d="M6 10L14 2H58V44L32 62L6 44Z" fill="#222B35" stroke="#FFA62B" stroke-width="3"/><path d="M28.5 13H35.5V22.6L28.5 25.4Z" fill="#FFA62B"/><path d="M28.5 28.9L35.5 26.1V39L32 41.7L28.5 39Z" fill="#3BE0FF"/><path d="M12 11.5a2.5 2.5 0 1 0 5 0a2.5 2.5 0 1 0-5 0Z" fill="#FFA62B"/><path d="M19 41L32 51L45 41" fill="none" stroke="#E6EBF0" stroke-width="4.5"/></svg>
    <span class="title">MyPersonalDownloadManager</span>
    <span class="sub">Download control</span>
    <div class="grow"></div>
    <button class="wc" id="theme" title="Theme">◐</button>
    <button class="wc"><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M5 12h14"/></svg></button>
    <button class="wc"><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M6 5h12a1 1 0 0 1 1 1v12a1 1 0 0 1-1 1H6a1 1 0 0 1-1-1V6a1 1 0 0 1 1-1z"/></svg></button>
    <button class="wc" style="color:var(--red)"><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M18 6L6 18M6 6l12 12"/></svg></button>
  </header>

  <section class="readouts">
    <div class="tile panel cham"><span class="lbl">Total speed</span>
      <div class="v"><span class="big" id="s_speed" style="color:var(--cyan)">0</span><span class="unit">MB/s</span>
        <div class="grow"></div><div class="led" id="s_meter" style="gap:2px"></div></div></div>
    <div class="tile panel cham"><span class="lbl">Active slots</span>
      <div class="v"><span class="big" id="s_slots">0/0</span><span class="unit">running</span>
        <div class="grow"></div><div style="display:flex;gap:5px" id="s_lamps"></div></div></div>
    <div class="tile panel cham"><span class="lbl">Downloaded today</span>
      <div class="v"><span class="big" id="s_today">0</span><span class="unit" id="s_unit">GB</span>
        <div class="grow"></div><span class="mono" style="color:var(--text3);font-size:11px" id="s_files">0 files</span></div></div>
    <div class="tile panel cham"><span class="lbl">Scheduler</span>
      <div class="v"><span class="big" id="s_sched" style="color:var(--amber)">OFF</span><span class="unit" id="s_schedsub"></span></div></div>
  </section>

  <nav class="toolbar panel chamTL">
    <button class="btn primary" id="add"><svg viewBox="0 0 24 24" stroke-linecap="round"><path d="M5 12h14M12 5v14"/></svg>Add</button>
    <span class="divider"></span>
    <button class="btn" id="resume"><svg viewBox="0 0 24 24" stroke-linecap="round" stroke-linejoin="round"><path d="M6 3l14 9-14 9V3z"/></svg>Resume</button>
    <button class="btn" id="pause"><svg viewBox="0 0 24 24" stroke-linecap="round" stroke-linejoin="round"><path d="M7 4h2v16H7zM15 4h2v16h-2z"/></svg>Pause</button>
    <button class="btn" id="stop"><svg viewBox="0 0 24 24" stroke-linecap="round" stroke-linejoin="round"><path d="M7 5h10a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V7a2 2 0 0 1 2-2z"/></svg>Stop</button>
    <button class="btn" id="remove"><svg viewBox="0 0 24 24" stroke-linecap="round" stroke-linejoin="round"><path d="M3 6h18M19 6v14c0 1-1 2-2 2H7c-1 0-2-1-2-2V6M8 6V4c0-1 1-2 2-2h4c1 0 2 1 2 2v2"/></svg>Remove</button>
    <span class="divider"></span>
    <button class="btn" id="clear"><svg viewBox="0 0 24 24" stroke-linecap="round" stroke-linejoin="round"><path d="M3 6h18M8 6V4h8v2M6 6l1 14h10l1-14"/></svg>Clear</button>
    <div class="grow"></div>
    <label class="search"><svg viewBox="0 0 24 24" stroke-linecap="round"><path d="M3 11a8 8 0 1 0 16 0a8 8 0 1 0-16 0M21 21l-4.3-4.3"/></svg><input id="search" placeholder="Search downloads"></label>
    <span class="lbl">Limit</span>
    <select class="sel" id="glimit"><option value="">UNLIMITED</option><option value="500k">500 KB/s</option><option value="1m">1 MB/s</option><option value="2m">2 MB/s</option><option value="5m">5 MB/s</option></select>
  </nav>

  <main>
    <aside>
      <div class="p panel cham"><span class="lbl">Categories</span><div class="cats" id="cats"></div></div>
      <div class="p panel cham" style="flex:1 1 auto;gap:2px">
        <span class="lbl" style="margin-bottom:6px">Status</span>
        <div id="statuslist"></div>
        <div class="grow" style="min-height:12px"></div>
        <div class="out well"><span class="lbl">Output</span>
          <span class="mono" style="font-size:11px" id="outdir">D:\Downloads</span>
          <span style="color:var(--text3);font-size:12px">Sorted by category</span></div>
      </div>
    </aside>

    <div class="center">
      <section class="p panel cham">
        <div style="display:flex;flex-wrap:wrap;align-items:center;gap:10px">
          <span class="lbl">Throughput</span>
          <span class="mono" style="font-size:11px;color:var(--cyan)" id="t_now">NOW 0</span>
          <span class="mono" style="font-size:11px;color:var(--text3)" id="t_peak">PEAK 0</span>
          <div class="grow"></div>
          <div class="ranges"><button>1M</button><button class="on">10M</button><button>1H</button><button>10H</button></div>
        </div>
        <div class="well"><canvas id="chart"></canvas></div>
      </section>
      <section class="queue panel cham">
        <div class="qhrow"><span class="lbl">Queue</span><span class="mono" style="font-size:11px;color:var(--text3)" id="qcount">0 ITEMS</span>
          <div class="grow"></div><span style="color:var(--text2);font-size:12px">Newest first</span></div>
        <div class="tbwrap"><div style="min-width:660px">
          <div class="colh"><span style="width:28px"></span><span style="flex:1 1 auto">Name</span><span style="width:64px">Size</span><span style="width:140px">Progress</span><span style="width:70px">Speed</span><span style="width:96px">State</span></div>
          <div id="rows"></div>
        </div></div>
      </section>
    </div>

    <div class="right panel cham">
      <div class="p">
        <div style="display:flex;justify-content:space-between;align-items:center"><span class="lbl">Selected</span><span class="c-state" id="sel_state"></span></div>
        <div><div style="font:500 14px/19px 'Barlow',sans-serif" id="sel_name">No selection</div><div class="mono" style="font-size:11px;color:var(--text3)" id="sel_host"></div></div>
        <div style="display:flex;align-items:flex-end;gap:6px"><span class="selbig" id="sel_pct">—</span><span class="mono" style="color:var(--text2);font-size:11px" id="sel_amt"></span></div>
        <div class="ledbig" id="sel_meter"></div>
        <span class="lbl">Connections</span>
        <div class="lanes" id="sel_lanes"></div>
        <div class="note" id="sel_note" style="display:none"></div>
        <div class="kv well" id="sel_kv"></div>
        <div class="selbtns">
          <button class="btn" id="sel_pause"><svg viewBox="0 0 24 24" stroke-linecap="round" stroke-linejoin="round"><path d="M7 4h2v16H7zM15 4h2v16h-2z"/></svg>Pause</button>
          <button class="btn" id="sel_open"><svg viewBox="0 0 24 24" stroke-linecap="round" stroke-linejoin="round"><path d="M4 7h5l2 2h9v9a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1V8a1 1 0 0 1 1-1z"/></svg>Open folder</button>
        </div>
      </div>
    </div>
  </main>

  <div class="statusbar">
    <span class="sbit"><span class="lamp" style="background:var(--green)"></span><span id="sb_net">NET 0</span></span>
    <span class="sbit"><span class="lamp" style="background:var(--cyan)"></span><span id="sb_slots">SLOTS 0/0</span></span>
    <span class="sbit"><span class="lamp" style="background:var(--amber)"></span><span id="sb_sched">SCHED OFF</span></span>
    <div class="grow"></div>
    <span class="sbit" id="sb_proxy">PROXY OFF</span>
  </div>
</div>
)PAGE"
R"PAGE(<script>
const $=s=>document.querySelector(s);
let selId=null,state={downloads:[]},samples=[],peak=1;
const savedTheme=localStorage.getItem("mpdm-theme"); if(savedTheme)document.documentElement.dataset.theme=savedTheme;
$("#theme").onclick=()=>{const l=document.documentElement.dataset.theme==="light";
  document.documentElement.dataset.theme=l?"dark":"light";localStorage.setItem("mpdm-theme",l?"dark":"light");draw();};
function human(n){if(n==null)return "—";n=+n;const u=["B","KB","MB","GB","TB"];let i=0;
  while(Math.abs(n)>=1024&&i<u.length-1){n/=1024;i++;}return (i?n.toFixed(1):n.toFixed(0))+u[i];}
function htime(s){if(s==null)return "—";s=Math.round(s);const h=s/3600|0,m=(s%3600)/60|0;return h?h+"h "+m+"m":m?m+" min":s+"s";}
function cvar(n){return getComputedStyle(document.documentElement).getPropertyValue(n).trim();}
function stColor(st){return {downloading:"var(--cyan)",paused:"var(--violet)",completed:"var(--green)",error:"var(--red)",queued:"var(--queued)"}[st]||"var(--queued)";}
async function api(p,b){const r=await fetch(p,{method:b?"POST":"GET",headers:b?{"Content-Type":"application/json"}:{},body:b?JSON.stringify(b):undefined});state=await r.json();render();}
function led(frac,cells,color){let h="";const on=Math.round(frac*cells);for(let i=0;i<cells;i++)h+=`<span style="background:${i<on?color:'var(--cellOff)'}"></span>`;return h;}
const CICON={All:'<path d="M22 12h-6l-2 3h-4l-2-3H2M5.45 5.11 2 12v6a2 2 0 0 0 2 2h16a2 2 0 0 0 2-2v-6l-3.45-6.89A2 2 0 0 0 16.76 4H7.24a2 2 0 0 0-1.79 1.11z"/>',
Video:'<path d="M5 3h14a2 2 0 0 1 2 2v14a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2zM7 3v18M3 12h18M17 3v18"/>',
Audio:'<path d="M9 18V5l12-2v13M3 18a3 3 0 1 0 6 0a3 3 0 1 0-6 0M15 16a3 3 0 1 0 6 0a3 3 0 1 0-6 0"/>',
Docs:'<path d="M15 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V7ZM14 2v4a2 2 0 0 0 2 2h4"/>',
Archive:'<path d="M3 3h18a1 1 0 0 1 1 1v3a1 1 0 0 1-1 1H3a1 1 0 0 1-1-1V4a1 1 0 0 1 1-1zM4 8v11a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8M10 12h4"/>',
Apps:'<path d="M4 4h16a2 2 0 0 1 2 2v12a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V6a2 2 0 0 1 2-2zM2 8h20"/>',
Other:'<path d="M15 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V7ZM14 2v4a2 2 0 0 0 2 2h4"/>',
Private:'<path d="M10.73 5.08A10.43 10.43 0 0 1 12 5c7 0 10 7 10 7a13.16 13.16 0 0 1-1.67 2.68M6.61 6.61A13.53 13.53 0 0 0 2 12s3 7 10 7a9.74 9.74 0 0 0 5.39-1.61M2 2l20 20"/>'};
function render(){
  const list=state.downloads||[];
  const filter=($("#search").value||"").toLowerCase();
  const shown=list.filter(d=>!filter||(d.filename||d.url||"").toLowerCase().includes(filter));
  let g=0,active=0,dl=0,fin=0;
  for(const d of list){if(d.status==="downloading"){g+=d.speed||0;active++;}dl+=d.downloaded||0;if(d.status==="completed")fin++;}
  peak=Math.max(peak,g);const mc=state.max_concurrent||5;
  $("#s_speed").textContent=(g/1048576).toFixed(1);
  $("#s_meter").innerHTML=led(Math.min(1,g/(peak||1)),16,"var(--cyan)");
  $("#s_slots").textContent=active+"/"+mc;
  $("#s_lamps").innerHTML=Array.from({length:mc},(_,i)=>`<span class="lamp" style="background:${i<active?'var(--cyan)':'var(--lampOff)'};box-shadow:${i<active?'var(--glow) var(--cyan)':'none'}"></span>`).join("");
  $("#s_today").textContent=human(dl).replace(/[A-Z/]+$/,"");$("#s_unit").textContent=human(dl).replace(/[0-9.]+/,"");$("#s_files").textContent=fin+" files";
  $("#t_now").textContent="NOW "+human(g)+"/s";$("#t_peak").textContent="PEAK "+human(peak)+"/s";
  $("#sb_net").textContent="NET "+human(g)+"/s";$("#sb_slots").textContent="SLOTS "+active+"/"+mc;
  $("#outdir").textContent=state.output_dir||"D:\\Downloads";
  // categories
  const cc={};for(const d of list)cc[d.category||"Other"]=(cc[d.category||"Other"]||0)+1;
  const cm=[["All",list.length],["Video",cc.Video||0],["Audio",cc.Audio||0],["Docs",cc.Documents||0],["Archive",cc.Archives||0],["Apps",cc.Programs||0],["Other",cc.Other||0],["Private",0]];
  $("#cats").innerHTML=cm.map(([k,n],i)=>`<button class="slot ${i===0?'on':''}"><svg viewBox="0 0 24 24" stroke-linecap="round" stroke-linejoin="round">${CICON[k]}</svg><span class="sl">${k}</span>${n?`<span class="cn">${n}</span>`:''}</button>`).join("");
  // status
  const sc={Downloading:['downloading','var(--cyan)',0],Queued:['queued','var(--queued)',0],Paused:['paused','var(--violet)',0],Finished:['completed','var(--green)',0],Failed:['error','var(--red)',0]};
  for(const d of list)for(const k in sc)if(sc[k][0]===d.status)sc[k][2]++;
  $("#statuslist").innerHTML=Object.entries(sc).map(([k,v])=>`<div class="strow"><span class="lamp" style="background:${v[1]};box-shadow:${v[0]==='queued'?'none':'var(--glow) '+v[1]}"></span><span style="flex:1 1 auto">${k}</span><span class="mono" style="font-size:11px;color:var(--text2)">${v[2]}</span></div>`).join("");
  // table
  $("#qcount").textContent=list.length+" ITEMS";
  $("#rows").innerHTML=shown.map((d,idx)=>{
    const frac=d.percent==null?0:Math.min(1,d.percent/100);
    const alt=idx%2?'var(--rowAlt)':'var(--row)';
    const err=d.error?`<div class="err">${d.error}</div>`:"";
    return `<div class="rw ${d.id===selId?'sel':''}" data-id="${d.id}" style="background:${d.id===selId?'var(--rowSel)':alt}">
      <span class="ic"><svg viewBox="0 0 24 24" stroke-linecap="round" stroke-linejoin="round"><path d="M15 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V7ZM14 2v4a2 2 0 0 0 2 2h4"/></svg></span>
      <div style="flex:1 1 auto;min-width:0"><div class="nm">${d.filename||d.url}</div>${err}</div>
      <span class="c-size">${human(d.size)}</span>
      <div class="c-prog"><div class="led">${led(frac,20,stColor(d.status))}</div><span class="mono" style="font-size:11px;color:var(--text2)">${d.percent==null?'0':Math.round(d.percent)}%</span></div>
      <span class="c-speed" style="color:${d.status==='downloading'?'var(--cyan)':'var(--text2)'}">${d.status==='downloading'&&d.speed?human(d.speed)+'/s':'—'}</span>
      <span class="c-state"><span class="lamp" style="background:${stColor(d.status)};box-shadow:${d.status==='queued'?'none':'var(--glow) '+stColor(d.status)}"></span>${d.status}</span></div>`;
  }).join("");
  $("#rows").querySelectorAll(".rw").forEach(r=>r.onclick=()=>{selId=r.dataset.id;render();});
  renderSel(list);draw();
}
function renderSel(list){
  let d=list.find(x=>x.id===selId);if(!d&&list.length){d=list[0];selId=d.id;}
  if(!d){$("#sel_name").textContent="No selection";$("#sel_lanes").innerHTML="";$("#sel_meter").innerHTML="";$("#sel_kv").innerHTML="";return;}
  const frac=d.percent==null?0:Math.min(1,d.percent/100);
  $("#sel_state").innerHTML=`<span class="lamp" style="background:${stColor(d.status)};box-shadow:var(--glow) ${stColor(d.status)}"></span>${d.status}`;
  $("#sel_name").textContent=d.filename||d.url;$("#sel_host").textContent=d.url;
  $("#sel_pct").textContent=(d.percent==null?0:Math.round(d.percent))+"%";
  $("#sel_amt").textContent=human(d.downloaded)+" / "+human(d.size);
  $("#sel_meter").innerHTML=led(frac,46,stColor(d.status));
  const cs=d.conn_stats||[];
  $("#sel_lanes").innerHTML=cs.slice(0,8).map(c=>{const stolen=c.stole_from>=0;const col=stolen?'var(--amber)':'var(--cyan)';const tr=stolen?'var(--amberTrack)':'var(--cyanTrack)';
    return `<div class="lane"><span class="cl" style="color:${stolen?'var(--amber)':'var(--text3)'}">C${c.index+1}</span><div class="lb" style="background:${tr}"><i style="width:${Math.round((c.fraction||0)*100)}%;background:${col}"></i></div><span class="ls">${((c.speed||0)/1048576).toFixed(1)}</span></div>`;}).join("");
  const stn=cs.find(c=>c.stole_from>=0);const note=$("#sel_note");
  if(stn){note.style.display="block";note.textContent=`C${stn.index+1} finished its own range early and took over part of C${stn.stole_from+1}'s remainder.`;}else note.style.display="none";
  $("#sel_kv").innerHTML=[["Speed",d.status==="downloading"?human(d.speed)+"/s":"—"],["Time left",d.status==="downloading"?htime(d.eta):"—"],["Limit",d.max_rate_bps?human(d.max_rate_bps)+"/s":"Unlimited"],["Resume",d.status==="completed"?"Done":"Saved"],["Save to",d.dest_path||"—"]].map(([k,v])=>`<div class="r"><span class="k">${k}</span><span class="vv">${v}</span></div>`).join("");
  $("#sel_pause").onclick=()=>api(d.status==="paused"?"/api/resume":"/api/pause",{id:d.id});
}
function draw(){const c=$("#chart");const r=c.getBoundingClientRect();c.width=r.width;c.height=118;const g=c.getContext("2d");
  g.clearRect(0,0,c.width,c.height);g.strokeStyle=cvar("--grid");g.lineWidth=1;
  for(let i=1;i<4;i++){const y=c.height*i/4;g.beginPath();g.moveTo(0,y);g.lineTo(c.width,y);g.stroke();}
  for(let x=60;x<c.width;x+=60){g.beginPath();g.moveTo(x,0);g.lineTo(x,c.height);g.stroke();}
  if(samples.length<2)return;const n=80,sc=Math.max(peak*1.15,1),acc=cvar("--cyan");
  const X=i=>c.width*i/(n-1),Y=v=>c.height-(v/sc)*c.height*0.9,off=n-samples.length;
  g.beginPath();g.moveTo(X(off),Y(samples[0]));for(let i=1;i<samples.length;i++)g.lineTo(X(off+i),Y(samples[i]));
  g.strokeStyle=acc;g.lineWidth=1.6;g.stroke();
  g.lineTo(X(n-1),c.height);g.lineTo(X(off),c.height);g.closePath();g.globalAlpha=.12;g.fillStyle=acc;g.fill();g.globalAlpha=1;}
$("#add").onclick=()=>{const u=prompt("Paste a URL (ranges like [1-100] expand):");if(u&&u.trim())api("/api/add",{url:u.trim(),connections:8});};
$("#resume").onclick=()=>selId&&api("/api/resume",{id:selId});
$("#pause").onclick=()=>selId&&api("/api/pause",{id:selId});
$("#stop").onclick=()=>selId&&api("/api/pause",{id:selId});
$("#remove").onclick=()=>selId&&confirm("Remove this download?")&&api("/api/remove",{id:selId,delete:false});
$("#clear").onclick=()=>api("/api/clear",{});
$("#glimit").onchange=e=>api("/api/global_rate",{rate:e.target.value});
$("#search").oninput=render;$("#sel_open").onclick=()=>{};window.addEventListener("resize",draw);
async function poll(){try{const r=await fetch("/api/state");state=await r.json();let g=0;for(const d of(state.downloads||[]))if(d.status==="downloading")g+=d.speed||0;samples.push(g);while(samples.length>80)samples.shift();render();}catch(e){}setTimeout(poll,1000);}
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
    manager.resume_interrupted();  // no prompts here: a changed file restarts (engine default)
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
        manager.retry_failed_due();  // the page polls every second
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
