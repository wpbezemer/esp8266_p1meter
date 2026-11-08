#pragma once
// **********************************
// * Web interface (single page)    *
// **********************************
// Eén HTML pagina in PROGMEM (flash, geen RAM) voor /, /info, /settings, /files en /update.
// De pagina haalt zelf data op via /api/*; de ESP hoeft dus niets te renderen.

const char WEBUI_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="nl"><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>P1 Meter</title>
<style>
:root{--bg:#f0f2f5;--card:#fff;--fg:#1d2330;--mut:#6b7280;--line:#e5e7eb;--acc:#2563eb;--ok:#16a34a;--bad:#dc2626;--warn:#d97706;--use:#ea580c;--ret:#16a34a}
@media(prefers-color-scheme:dark){:root{--bg:#0f1115;--card:#1a1d24;--fg:#e5e7eb;--mut:#9ca3af;--line:#2a2f3a;--acc:#60a5fa;--use:#fb923c;--ret:#4ade80}}
*{box-sizing:border-box}
body{margin:0;font:14px/1.45 -apple-system,Segoe UI,Roboto,sans-serif;background:var(--bg);color:var(--fg)}
header{background:var(--card);border-bottom:1px solid var(--line);position:sticky;top:0;z-index:2}
.hw{max-width:900px;margin:0 auto;padding:10px 16px;display:flex;align-items:center;gap:12px;flex-wrap:wrap}
.brand{font-weight:700;font-size:16px;margin-right:auto}.brand small{font-weight:400;color:var(--mut);font-size:12px}
nav{display:flex;gap:4px;flex-wrap:wrap}
nav a{padding:6px 10px;border-radius:8px;color:var(--mut);text-decoration:none;font-weight:500}
nav a.on{background:var(--acc);color:#fff}
main{max-width:900px;margin:0 auto;padding:16px}
.grid{display:grid;gap:12px;grid-template-columns:repeat(auto-fit,minmax(190px,1fr))}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px 16px;margin-bottom:12px}
.card h2{font-size:13px;font-weight:600;margin:0 0 10px;color:var(--mut);text-transform:uppercase;letter-spacing:.04em}
.big{font-size:30px;font-weight:700;font-variant-numeric:tabular-nums}.big small{font-size:14px;color:var(--mut);font-weight:500}
.use{color:var(--use)}.ret{color:var(--ret)}
table{width:100%;border-collapse:collapse;font-variant-numeric:tabular-nums}
td,th{padding:6px 4px;border-bottom:1px solid var(--line);text-align:left}
th{color:var(--mut);font-weight:500;font-size:12px}
td.r,th.r{text-align:right}
tr:last-child td{border-bottom:0}
.st{display:flex;gap:14px;flex-wrap:wrap;font-size:12px;color:var(--mut)}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:5px;background:var(--mut)}
.dot.ok{background:var(--ok)}.dot.bad{background:var(--bad)}.dot.warn{background:var(--warn)}
label{display:block;font-size:12px;font-weight:500;margin:10px 0 4px}
input[type=text],input[type=number],input[type=password]{width:100%;padding:8px 10px;border:1px solid var(--line);border-radius:8px;background:var(--bg);color:var(--fg);font:inherit}
.row{display:flex;align-items:center;justify-content:space-between;gap:10px;padding:8px 0}
.row .t{font-weight:500}.row .s{font-size:12px;color:var(--mut)}
button,.btn{display:inline-block;padding:8px 14px;border:0;border-radius:8px;background:var(--acc);color:#fff;font:inherit;font-weight:500;cursor:pointer;text-decoration:none}
button.sec,.btn.sec{background:transparent;color:var(--fg);border:1px solid var(--line)}
button.danger{background:var(--bad)}
button:disabled{opacity:.5;cursor:default}
.actions{display:flex;gap:8px;flex-wrap:wrap;margin-top:12px}
.bar{height:8px;background:var(--line);border-radius:4px;overflow:hidden}.bar>div{height:100%;background:var(--acc);width:0;transition:width .3s}
.msg{padding:9px 12px;border-radius:8px;margin:10px 0;font-size:13px;border-left:4px solid var(--acc);background:var(--bg)}
.msg.ok{border-color:var(--ok)}.msg.bad{border-color:var(--bad)}
.mut{color:var(--mut)}.mono{font-family:ui-monospace,Menlo,monospace;font-size:12px;word-break:break-all}
svg.spark{width:100%;height:60px;display:block}
.hide{display:none}
pre{background:var(--bg);border:1px solid var(--line);border-radius:8px;padding:10px;overflow:auto;max-height:420px;font:12px/1.4 ui-monospace,Menlo,monospace;margin:10px 0 0;white-space:pre}
</style></head><body>
<header><div class="hw"><div class="brand">P1 Meter <small id="ver"></small></div>
<nav><a href="/" data-v="dash">Dashboard</a><a href="/info" data-v="info">Info</a><a href="/settings" data-v="settings">Instellingen</a><a href="/files" data-v="files">Bestanden</a><a href="/update" data-v="update">Update</a></nav></div></header>
<main id="app"></main>
<script>
const $=s=>document.querySelector(s);
const app=$('#app');
let timer=null,busy=false;
function api(u,o){return fetch(u,o).then(r=>{const j=(r.headers.get('content-type')||'').indexOf('json')>=0;return(j?r.json():r.text()).then(d=>{if(!r.ok)throw new Error(d&&d.error?d.error:(typeof d=='string'&&d?d:'HTTP '+r.status));return d})})}
function post(u,data){return api(u,{method:'POST',body:new URLSearchParams(data)})}
const esc=s=>String(s==null?'':s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const n=(v,d)=>Number(v).toLocaleString('nl-NL',{minimumFractionDigits:d,maximumFractionDigits:d});
function bytes(b){if(b<1024)return b+' B';if(b<1048576)return n(b/1024,1)+' KB';return n(b/1048576,2)+' MB'}
function dur(s){const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return(d?d+'d ':'')+h+'u '+m+'m '+(s%60)+'s'}
function age(ms){return ms<0?'nooit':ms<1000?'nu':ms<60000?Math.round(ms/1000)+' s geleden':Math.round(ms/60000)+' min geleden'}
function every(fn,ms){clearInterval(timer);const run=()=>{if(busy||document.hidden)return;busy=true;Promise.resolve(fn()).catch(()=>{}).finally(()=>busy=false)};run();timer=setInterval(run,ms)}
function msg(el,text,cls){el.innerHTML='<div class="msg '+(cls||'')+'">'+text+'</div>'}
function xhrUpload(url,file,field,onProg){return new Promise((res,rej)=>{const x=new XMLHttpRequest(),fd=new FormData();fd.append(field,file,file.name);x.open('POST',url);x.upload.onprogress=e=>{if(e.lengthComputable)onProg(e.loaded/e.total)};x.onload=()=>x.status<300?res(x.responseText):rej(new Error(x.responseText||('HTTP '+x.status)));x.onerror=()=>rej(new Error('verbinding verbroken'));x.send(fd)})}
function waitReboot(el){let tries=0,down=false;msg(el,'Wachten tot de ESP opnieuw opstart...');const t=setInterval(()=>{tries++;fetch('/version',{cache:'no-store'}).then(r=>r.text()).then(v=>{if(down||tries>10){clearInterval(t);msg(el,'Terug online met v'+esc(v)+'. Pagina wordt herladen...','ok');setTimeout(()=>location.reload(),1500)}}).catch(()=>{down=true});if(tries>60){clearInterval(t);msg(el,'ESP reageert niet. Herlaad de pagina handmatig.','bad')}},2000)}

/* ---------- Dashboard ---------- */
const hist=[];
function vDash(){
app.innerHTML=`<div class="grid">
<div class="card"><h2>Verbruik nu</h2><div class="big use" id="p">–</div></div>
<div class="card"><h2>Teruglevering nu</h2><div class="big ret" id="r">–</div></div>
<div class="card"><h2>Tarief</h2><div class="big" id="tar">–</div></div></div>
<div class="card"><h2>Netto vermogen (laatste minuten)</h2><svg class="spark" id="sp" viewBox="0 0 300 60" preserveAspectRatio="none"></svg></div>
<div class="card"><h2>Per fase</h2><table><thead><tr><th></th><th class="r">Verbruik</th><th class="r">Terug</th><th class="r">Stroom</th><th class="r">Spanning</th></tr></thead><tbody id="ph"></tbody></table></div>
<div class="grid"><div class="card"><h2>Meterstanden</h2><table id="tot"></table></div>
<div class="card"><h2>Netkwaliteit</h2><table id="ev"></table></div></div>
<div class="card st" id="st"></div>`;
every(()=>api('/api/live').then(d=>{
$('#p').innerHTML=n(d.p,0)+' <small>W</small>';$('#r').innerHTML=n(d.r,0)+' <small>W</small>';
$('#tar').innerHTML=d.tarif==1?'Laag <small>(T1)</small>':d.tarif==2?'Normaal <small>(T2)</small>':'–';
hist.push(d.p-d.r);if(hist.length>150)hist.shift();spark();
$('#ph').innerHTML=d.ph.map((x,i)=>(i==0||x[3]>0)?`<tr><td><b>L${i+1}</b></td><td class="r use">${n(x[0],0)} W</td><td class="r ret">${n(x[1],0)} W</td><td class="r">${n(x[2]/1000,0)} A</td><td class="r">${n(x[3]/1000,1)} V</td></tr>`:'').join('');
$('#tot').innerHTML=`<tr><td>Verbruik T1 (laag)</td><td class="r">${n(d.t1/1000,3)} kWh</td></tr><tr><td>Verbruik T2 (normaal)</td><td class="r">${n(d.t2/1000,3)} kWh</td></tr><tr><td>Teruglevering T1</td><td class="r">${n(d.r1/1000,3)} kWh</td></tr><tr><td>Teruglevering T2</td><td class="r">${n(d.r2/1000,3)} kWh</td></tr><tr><td>Gas</td><td class="r">${n(d.gas/1000,3)} m³</td></tr>`;
$('#ev').innerHTML=`<tr><td>Korte onderbrekingen</td><td class="r">${d.ev[0]}</td></tr><tr><td>Lange onderbrekingen</td><td class="r">${d.ev[1]}</td></tr><tr><td>Spanningsdips L1</td><td class="r">${d.ev[2]}</td></tr><tr><td>Spanningspieken L1</td><td class="r">${d.ev[3]}</td></tr>`;
const fresh=d.age>=0&&d.age<5000;
$('#st').innerHTML=`<span><span class="dot ${fresh?'ok':'bad'}"></span>Telegram: ${age(d.age)}</span><span><span class="dot ${d.mqtt?'ok':'bad'}"></span>MQTT ${d.mqtt?'verbonden':'niet verbonden'}</span><span>Interval: ${d.interval/1000} s</span><span>Telegrammen: ${d.ok} &middot; CRC fouten: ${d.crc}</span><span>Meter tijd: ${esc(d.ts)}</span>`;
}),1000)}
function spark(){const el=$('#sp');if(!el||hist.length<2)return;const mx=Math.max(...hist,10),mn=Math.min(...hist,0),rg=mx-mn||1,w=300/(150-1);
const pts=hist.map((v,i)=>(i*w).toFixed(1)+','+(58-(v-mn)/rg*56).toFixed(1)).join(' ');const z=(58-(0-mn)/rg*56).toFixed(1);
el.innerHTML=`<line x1="0" x2="300" y1="${z}" y2="${z}" stroke="currentColor" stroke-opacity=".2" stroke-dasharray="3 3"/><polyline points="${pts}" fill="none" stroke="var(--acc)" stroke-width="1.5" vector-effect="non-scaling-stroke"/>`}

/* ---------- Info ---------- */
function vInfo(){
app.innerHTML=`<div id="inf"></div><div class="card"><h2>Acties</h2><div class="actions"><a class="btn sec" href="/telegram" target="_blank">Telegram JSON</a><a class="btn sec" href="/api/info" target="_blank">Info JSON</a><button class="danger" id="rb">Herstarten</button></div><div id="rbm"></div></div>
<div class="card"><h2>Debug: ruw telegram</h2><p class="mut">Vangt het eerstvolgende geldige telegram op zoals de meter het stuurt. Gebruikt alleen tijdens debuggen 2 KB geheugen.</p>
<div class="actions"><button class="sec" id="rw">Ruw telegram ophalen</button><button class="sec hide" id="rc">Kopiëren</button><button class="sec hide" id="rf">Geheugen vrijgeven</button></div><div id="rm"></div><pre id="rp" class="hide"></pre></div>`;
$('#rw').onclick=()=>{$('#rw').disabled=true;msg($('#rm'),'Wachten op volgend telegram...');let n=0;
const t=()=>fetch('/api/raw',{cache:'no-store'}).then(r=>r.text().then(x=>{if(r.status==200){$('#rm').innerHTML='';$('#rp').textContent=x;$('#rp').classList.remove('hide');$('#rc').classList.remove('hide');$('#rf').classList.remove('hide');$('#rw').disabled=false}else if(++n<30)setTimeout(t,1000);else{msg($('#rm'),'Geen geldig telegram ontvangen binnen 30 s.','bad');$('#rw').disabled=false}})).catch(e=>{msg($('#rm'),esc(e.message),'bad');$('#rw').disabled=false});t()};
$('#rc').onclick=()=>{const x=$('#rp').textContent;(navigator.clipboard?navigator.clipboard.writeText(x):Promise.reject()).then(()=>msg($('#rm'),'Gekopieerd.','ok')).catch(()=>{const r=document.createRange();r.selectNodeContents($('#rp'));getSelection().removeAllRanges();getSelection().addRange(r)})};
$('#rf').onclick=()=>api('/api/raw?free=1').then(()=>{$('#rp').classList.add('hide');$('#rc').classList.add('hide');$('#rf').classList.add('hide');msg($('#rm'),'Vrijgegeven.','ok')});
$('#rb').onclick=()=>{if(!confirm('ESP herstarten?'))return;api('/restart').then(()=>waitReboot($('#rbm'))).catch(e=>msg($('#rbm'),esc(e.message),'bad'))};
const T=(t,rows)=>`<div class="card"><h2>${t}</h2><table>${rows.map(r=>`<tr><td class="mut">${r[0]}</td><td class="r ${r[2]||''}">${r[1]}</td></tr>`).join('')}</table></div>`;
every(()=>api('/api/info').then(d=>{
const q=d.rssi>-60?'goed':d.rssi>-75?'redelijk':'zwak';
$('#inf').innerHTML='<div class="grid">'+
T('Systeem',[['Firmware','v'+esc(d.version)],['Hostname',esc(d.hostname)],['Uptime',dur(d.uptime)],['Laatste reset',esc(d.reset_reason)],['Chip ID',d.chip_id],['CPU',d.cpu_mhz+' MHz'],['Core / SDK',esc(d.core)+' / '+esc(d.sdk).split('(')[0]],['Langste loop (10 s)',n(d.max_loop_us/1000,1)+' ms']])+
T('Geheugen',[['Vrij heap',bytes(d.heap_free)],['Grootste blok',bytes(d.heap_max_block)],['Fragmentatie',d.heap_frag+' %'],['Flash',bytes(d.flash_size)],['Sketch',bytes(d.sketch_size)],['Vrij voor OTA',bytes(d.sketch_free)],['Bestandssysteem',bytes(d.fs_used)+' / '+bytes(d.fs_total)]])+
T('WiFi',[['SSID',esc(d.ssid)],['Signaal',d.rssi+' dBm ('+q+')'],['Kanaal',d.channel],['IP',d.ip],['Gateway',d.gateway],['DNS',d.dns],['MAC',d.mac],['BSSID',d.bssid],['Reconnects',d.wifi_reconnects]])+
T('MQTT',[['Status','<span class="dot '+(d.mqtt_connected?'ok':'bad')+'"></span>'+(d.mqtt_connected?'verbonden':'niet verbonden (rc='+d.mqtt_state+')')],['Broker',esc(d.mqtt_host)+':'+esc(d.mqtt_port)],['Root topic','<span class="mono">'+esc(d.mqtt_topic)+'</span>'],['Interval',d.interval/1000+' s'],['Laatste publicatie',age(d.last_publish_age)+' ('+d.last_publish_count+' topics)'],['Berichten verstuurd',d.mqtt_publishes],['Mislukt',d.mqtt_failures],['Verbindingen',d.mqtt_reconnects],['HA discovery',d.ha_discovery?'aan':'uit']])+
T('P1 meter',[['Laatste telegram',age(d.last_telegram_age)],['Geldige telegrammen',d.telegrams_ok],['CRC fouten',d.crc_errors+(d.telegrams_ok?' ('+n(100*d.crc_errors/(d.telegrams_ok+d.crc_errors),2)+' %)':'')],['DSMR versie',esc(d.dsmr_version)],['Meter ID','<span class="mono">'+esc(d.meter_id)+'</span>'],['Gasmeter ID','<span class="mono">'+esc(d.gas_id)+'</span>']])+
'</div>';
}),5000)}

/* ---------- Instellingen ---------- */
function vSettings(){
app.innerHTML=`<div class="card"><h2>Algemeen</h2>
<label>MQTT update interval (seconden, 1–3600)</label><input type="number" id="iv" min="1" max="3600">
<div class="row"><div><div class="t">Home Assistant discovery</div><div class="s">Sensors automatisch aanmaken in Home Assistant</div></div><input type="checkbox" id="ha"></div>
<div class="actions"><button id="s1">Opslaan</button></div><div id="m1"></div><p class="mut">Wordt direct actief, geen herstart nodig.</p></div>
<div class="card"><h2>MQTT broker</h2>
<label>Host</label><input type="text" id="mh" maxlength="63">
<label>Poort</label><input type="text" id="mp" maxlength="5">
<label>Gebruiker</label><input type="text" id="mu" maxlength="31" autocomplete="off">
<label>Wachtwoord</label><input type="password" id="mw" maxlength="31" autocomplete="new-password" placeholder="leeg = ongewijzigd">
<label>Root topic</label><input type="text" id="mt" maxlength="31">
<div class="actions"><button id="s2">Opslaan en herstarten</button></div><div id="m2"></div></div>`;
api('/api/settings').then(d=>{$('#iv').value=d.interval;$('#ha').checked=d.ha_discovery;$('#mh').value=d.mqtt_host;$('#mp').value=d.mqtt_port;$('#mu').value=d.mqtt_user;$('#mt').value=d.mqtt_topic;if(d.mqtt_pass_set)$('#mw').placeholder='•••••• (leeg = ongewijzigd)'}).catch(e=>msg($('#m1'),esc(e.message),'bad'));
$('#s1').onclick=()=>post('/api/settings',{interval:$('#iv').value,ha_discovery:$('#ha').checked?1:0}).then(()=>msg($('#m1'),'Opgeslagen.','ok')).catch(e=>msg($('#m1'),esc(e.message),'bad'));
$('#s2').onclick=()=>{if(!confirm('MQTT instellingen opslaan? De ESP herstart bij wijzigingen.'))return;post('/api/settings',{mqtt_host:$('#mh').value,mqtt_port:$('#mp').value,mqtt_user:$('#mu').value,mqtt_pass:$('#mw').value,mqtt_topic:$('#mt').value}).then(d=>d.restart?waitReboot($('#m2')):msg($('#m2'),'Niets gewijzigd.','ok')).catch(e=>msg($('#m2'),esc(e.message),'bad'))};
}

/* ---------- Bestanden ---------- */
function vFiles(){
app.innerHTML=`<div class="card"><h2>Bestandssysteem (LittleFS)</h2><div class="bar"><div id="fu"></div></div><p class="mut" id="ft"></p>
<table><thead><tr><th>Naam</th><th class="r">Grootte</th><th></th></tr></thead><tbody id="fl"></tbody></table></div>
<div class="card"><h2>Uploaden</h2><input type="file" id="ff"><div class="actions"><button id="fb">Upload</button></div><div class="bar" style="margin-top:10px"><div id="fp"></div></div><div id="fm"></div><p class="mut">Max. 31 tekens per bestandsnaam (inclusief /).</p></div>`;
const load=()=>api('/api/fs/list').then(d=>{
$('#fu').style.width=(d.total?100*d.used/d.total:0)+'%';$('#ft').textContent=bytes(d.used)+' van '+bytes(d.total)+' gebruikt';
$('#fl').innerHTML=d.files.length?d.files.map(f=>{const u=encodeURIComponent(f.name);return`<tr><td class="mono"><a href="/api/fs/file?path=${u}" target="_blank">${esc(f.name)}</a></td><td class="r">${bytes(f.size)}</td><td class="r"><a class="btn sec" href="/api/fs/file?path=${u}&download=1">&#8595;</a> <button class="sec" data-del="${esc(f.name)}">&#10005;</button></td></tr>`}).join(''):'<tr><td colspan="3" class="mut">Geen bestanden</td></tr>';
}).catch(e=>msg($('#fm'),esc(e.message),'bad'));
$('#fl').onclick=e=>{const p=e.target.dataset.del;if(!p||!confirm('Verwijder '+p+'?'))return;post('/api/fs/delete',{path:p}).then(load).catch(e=>msg($('#fm'),esc(e.message),'bad'))};
$('#fb').onclick=()=>{const f=$('#ff').files[0];if(!f)return;if(f.name.length>30){msg($('#fm'),'Bestandsnaam te lang.','bad');return}
$('#fb').disabled=true;xhrUpload('/api/fs/upload',f,'file',p=>$('#fp').style.width=p*100+'%').then(()=>{msg($('#fm'),'Geüpload: '+esc(f.name),'ok');load()}).catch(e=>msg($('#fm'),esc(e.message),'bad')).finally(()=>{$('#fb').disabled=false})};
load()}

/* ---------- Update ---------- */
function vcmp(a,b){a=a.split('.').map(Number);b=b.split('.').map(Number);for(let i=0;i<3;i++){const d=(a[i]||0)-(b[i]||0);if(d)return d}return 0}
function vUpdate(){
app.innerHTML=`<div class="card"><h2>Firmware</h2><div id="us">Laden...</div><div id="gh"></div>
<p class="mut">De versiecheck doet je browser rechtstreeks bij GitHub; de ESP zelf maakt geen verbinding met internet.</p></div>
<div class="card"><h2>Update via .bin bestand</h2><input type="file" id="bf" accept=".bin"><div class="actions"><button id="bb">Upload en installeer</button></div><div class="bar" style="margin-top:10px"><div id="bp"></div></div><div id="bm"></div>
<p class="mut">Arduino IDE: Sketch &rarr; Export Compiled Binary. PlatformIO: .pio/build/d1_mini/firmware.bin</p></div>`;
api('/update/check').then(d=>{
$('#us').innerHTML=`<div class="msg">Huidige versie: <b>v${esc(d.current)}</b></div>`;
fetch('https://api.github.com/repos/'+d.repo+'/releases/latest').then(r=>r.ok?r.json():Promise.reject()).then(g=>{
const v=String(g.tag_name||'').replace(/^v/i,''),nw=vcmp(v,d.current)>0,bin=(g.assets||[]).find(a=>/\.bin$/i.test(a.name));
$('#gh').innerHTML=`<div class="msg ${nw?'bad':'ok'}">${nw?'Nieuwe versie beschikbaar: <b>v'+esc(v)+'</b>':'Up-to-date (laatste release: v'+esc(v)+')'}</div>`+
`<div class="actions">${bin?`<a class="btn${nw?'':' sec'}" href="${esc(bin.browser_download_url)}">Download ${esc(bin.name)}</a>`:''}<a class="btn sec" href="${esc(g.html_url)}" target="_blank">Release notes</a></div>`+
(nw&&bin?'<p class="mut">Download het bestand en upload het hieronder.</p>':'')}).catch(()=>{$('#gh').innerHTML='<div class="msg">Kon GitHub niet bereiken (of er is nog geen release).</div>'})});
$('#bb').onclick=()=>{const f=$('#bf').files[0];if(!f)return;if(!confirm('Firmware '+f.name+' installeren?'))return;$('#bb').disabled=true;
xhrUpload('/update/upload',f,'firmware',p=>$('#bp').style.width=p*100+'%').then(()=>waitReboot($('#bm'))).catch(e=>{msg($('#bm'),esc(e.message),'bad');$('#bb').disabled=false})};
}

/* ---------- Router ---------- */
const views={'/':vDash,'/info':vInfo,'/settings':vSettings,'/files':vFiles,'/update':vUpdate};
function route(){clearInterval(timer);let p=location.pathname.replace(/\/$/,'')||'/';(views[p]||vDash)();
document.querySelectorAll('nav a').forEach(a=>a.classList.toggle('on',a.getAttribute('href')==p))}
document.querySelector('nav').onclick=e=>{const a=e.target.closest('a');if(!a)return;e.preventDefault();history.pushState(0,'',a.getAttribute('href'));route()};
window.onpopstate=route;
api('/version').then(v=>$('#ver').textContent='v'+v).catch(()=>{});
route();
</script></body></html>)rawliteral";
