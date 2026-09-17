#include "web.h"

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "config.h"
#include "controller.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "history.h"
#include "relay.h"
#include "sdkconfig.h"
#include "factory_reset.h"
#include "sensor.h"
#include "wifi.h"

/*
 * Largest config POST we will read. The settings form sends well under 300
 * bytes; the network form can approach 250 with two 32-byte names and two
 * 63-character passphrases.
 */
#define CONFIG_BODY_MAX 768

static const char *TAG = "web";

static httpd_handle_t s_server;

/*
 * Served as one static string. No framework, no external assets: everything the
 * page needs has to fit in flash and be reachable with no internet access, since
 * clients are attached to our own AP.
 */
static const char index_html[] =
"<!doctype html><html lang=\"en\"><head>"
"<meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>Greenhouse</title><style>"
":root{color-scheme:light dark}"
"*{box-sizing:border-box}"
"body{margin:0;padding:30px 18px;font:21px system-ui,-apple-system,sans-serif;"
"background:#f6f7f5;color:#1a1c19}"
"@media(prefers-color-scheme:dark){body{background:#14171a;color:#e8eae6}"
".card,fieldset{background:#1e2227!important;border-color:#2e343b!important}"
".sub,dt,label{color:#98a0a8!important}"
"input{background:#14171a!important;color:#e8eae6!important;border-color:#39414a!important}}"
"main{max-width:540px;margin:0 auto}"
"h1{font-size:30px;font-weight:600;margin:0 0 6px}"
"h2{font-size:18px;font-weight:600;text-transform:uppercase;letter-spacing:.05em;"
"margin:32px 0 12px;color:#5c6660}"
".sub{color:#5c6660;font-size:17px;margin:0 0 22px}"
".grid{display:flex;gap:12px;flex-wrap:wrap}"
".card{flex:1 1 160px;background:#fff;border:1px solid #e0e4df;border-radius:10px;padding:20px}"
".label{font-size:15px;text-transform:uppercase;letter-spacing:.05em;color:#5c6660}"
".val{font-size:62px;font-weight:600;margin-top:8px;line-height:1.05}"
".unit{font-size:28px;font-weight:400}"
".warn{margin-top:18px;padding:15px;border-radius:8px;background:#fdecea;"
"border:1px solid #f5c6c0;color:#8a1c10;font-size:19px}"
".ok{background:#eaf6ec;border-color:#bfe0c6;color:#1c5b2a}"
"dl{margin:0;font-size:19px}"
"dl div{display:flex;justify-content:space-between;padding:9px 0;border-bottom:1px solid #e0e4df3d}"
"dt{color:#5c6660}dd{margin:0;font-variant-numeric:tabular-nums}"
"fieldset{border:1px solid #e0e4df;border-radius:10px;padding:10px 18px 20px;background:#fff;margin:0}"
"legend{font-size:15px;color:#5c6660;padding:0 6px}"
"form div.row{display:flex;align-items:center;justify-content:space-between;gap:14px;padding:10px 0}"
"label{font-size:19px;color:#5c6660;flex:1}"
"input[type=number]{width:132px;padding:12px 14px;font:inherit;font-size:20px;"
"border:1px solid #d3d9d0;border-radius:7px;background:#fff;color:inherit;"
"font-variant-numeric:tabular-nums;text-align:right}"
"button{margin-top:20px;width:100%;padding:17px;font:inherit;font-size:21px;font-weight:600;"
"border:0;border-radius:8px;background:#2f6b3c;color:#fff;cursor:pointer}"
"button:disabled{opacity:.5;cursor:default}"
".hint{font-size:16px;color:#8a938c;margin:12px 0 0}"
"#msg{margin-top:16px;font-size:19px;padding:15px;border-radius:8px;border:1px solid}"
"#chart{margin:0;position:relative}"
".viz{--surface-1:#fff;--grid:#e6e9e3;--ink:#1a1c19;--muted:#6b736c;"
"--temp:#1baf7a;--hum:#2a78d6;--fan:#eb6834}"
"@media(prefers-color-scheme:dark){.viz{--surface-1:#1e2227;--grid:#2e343b;"
"--ink:#e8eae6;--muted:#98a0a8;--temp:#199e70;--hum:#3987e5;--fan:#d95926}}"
".panel{background:var(--surface-1);border:1px solid #e0e4df;border-radius:10px;"
"padding:14px 14px 6px;margin-bottom:10px}"
"@media(prefers-color-scheme:dark){.panel{border-color:#2e343b}}"
".ptitle{display:flex;justify-content:space-between;align-items:baseline;"
"font-size:17px;color:var(--muted);margin-bottom:6px}"
".pnow{font-size:25px;font-weight:600;font-variant-numeric:tabular-nums}"
"canvas{display:block;width:100%;height:140px;touch-action:pan-y}"
".key{display:flex;gap:16px;font-size:16px;color:var(--muted);margin:2px 0 0;"
"align-items:center;flex-wrap:wrap}"
".key i{display:inline-block;width:13px;height:13px;border-radius:3px;"
"margin-right:6px;vertical-align:-2px}"
".fan{display:flex;align-items:center;gap:10px;margin-top:16px;padding:14px 16px;"
"border-radius:10px;border:1px solid #e0e4df;background:#fff;font-size:19px}"
"@media(prefers-color-scheme:dark){.fan{background:#1e2227;border-color:#2e343b}}"
".dot{width:15px;height:15px;border-radius:50%;background:#b9c0b7;flex:none}"
".fan.on .dot{background:#eb6834}"
".fan b{font-weight:600}"
".fmeta{margin-left:auto;font-size:17px;color:#6b736c;font-variant-numeric:tabular-nums}"
".seg{display:flex;gap:6px;margin-top:16px;padding:6px;border-radius:12px;"
"background:#e8ebe6;border:1px solid #e0e4df}"
"@media(prefers-color-scheme:dark){.seg{background:#181c20;border-color:#2e343b}}"
".seg button{margin:0;flex:1;padding:14px 8px;font:inherit;font-size:19px;"
"font-weight:600;border:0;border-radius:8px;background:transparent;color:#5c6660;"
"cursor:pointer}"
"@media(prefers-color-scheme:dark){.seg button{color:#98a0a8}}"
".seg button.sel{background:#fff;color:#1a1c19;box-shadow:0 1px 3px #0000001f}"
"@media(prefers-color-scheme:dark){.seg button.sel{background:#2b3138;color:#e8eae6}}"
".seg button.sel[data-m=\"2\"]{background:#eb6834;color:#fff}"
".seg button.sel[data-m=\"0\"]{background:#4a5350;color:#fff}"
".tog{display:flex;align-items:center;justify-content:space-between;gap:14px;"
"padding:12px 0;border-bottom:1px solid #e0e4df3d}"
".tog span{font-size:19px}"
".sw{position:relative;width:62px;height:34px;flex:none;cursor:pointer}"
".sw input{position:absolute;opacity:0;width:100%;height:100%;margin:0;cursor:pointer}"
".sw i{position:absolute;inset:0;border-radius:34px;background:#c3c9c1;"
"transition:background .15s;pointer-events:none}"
".sw i:after{content:'';position:absolute;top:3px;left:3px;width:28px;height:28px;"
"border-radius:50%;background:#fff;transition:transform .15s}"
".sw input:checked+i{background:#2f6b3c}"
".sw input:checked+i:after{transform:translateX(28px)}"
"@media(prefers-color-scheme:dark){.sw i{background:#39414a}}"
".off input[type=number]{opacity:.45}"
".off label{opacity:.55}"
"details{margin-top:28px}"
"summary{font-size:16px;color:#6b736c;cursor:pointer;padding:8px 0;"
"list-style:none;-webkit-tap-highlight-color:transparent}"
"summary::-webkit-details-marker{display:none}"
"summary:before{content:'\\25b8 ';display:inline-block;transition:transform .15s}"
"details[open] summary:before{transform:rotate(90deg)}"
"input[type=text],input[type=password]{width:100%;padding:12px 14px;font:inherit;"
"font-size:20px;border:1px solid #d3d9d0;border-radius:8px;background:#fff;color:inherit}"
"@media(prefers-color-scheme:dark){input[type=text],input[type=password]{"
"background:#14171a;color:#e8eae6;border-color:#39414a}}"
".netwarn{margin:0 0 18px;padding:15px;border-radius:8px;background:#fdecea;"
"border:1px solid #f5c6c0;color:#8a1c10;font-size:18px}"
"@media(prefers-color-scheme:dark){.netwarn{background:#2b1a18;border-color:#5c2b25;"
"color:#f0b5ae}}"
".seg.wifi{margin-top:6px}"
".seg.wifi button.sel{background:#2f6b3c;color:#fff}"
"@media(prefers-color-scheme:dark){.seg.wifi button.sel{background:#2f6b3c;color:#fff}}"
".tools{display:flex;gap:10px;margin-top:10px;flex-wrap:wrap}"
".tools button{margin-top:0;flex:1 1 auto;background:#4a5350;font-size:18px;padding:14px}"
".rangerow{display:flex;justify-content:space-between;align-items:center;"
"margin:32px 0 12px;gap:12px;flex-wrap:wrap}"
".rangerow h2{margin:0}"
"select{font:inherit;font-size:18px;padding:10px 12px;border-radius:8px;"
"border:1px solid #d3d9d0;background:#fff;color:inherit}"
"@media(prefers-color-scheme:dark){select{background:#14171a;color:#e8eae6;"
"border-color:#39414a}}"
".xax{display:flex;justify-content:space-between;font-size:15px;"
"color:var(--muted);margin:2px 2px 0;padding:0 6px 0 34px}"
"#read{font-size:17px;color:var(--muted);min-height:22px;margin:6px 0 0;"
"font-variant-numeric:tabular-nums}"

"</style></head><body><main>"
"<h1>Greenhouse controller</h1>"
"<p class=\"sub\" id=\"ssid\">&nbsp;</p>"
"<div class=\"netwarn\" id=\"netwarn\" hidden></div>"
"<div class=\"grid\">"
"<div class=\"card\"><div class=\"label\">Temperature</div>"
"<div class=\"val\" id=\"t\">--<span class=\"unit\"> &deg;C</span></div></div>"
"<div class=\"card\"><div class=\"label\">Humidity</div>"
"<div class=\"val\" id=\"h\">--<span class=\"unit\"> %</span></div></div>"
"</div>"
"<div class=\"seg\" id=\"seg\">"
"<button type=\"button\" data-m=\"0\">Off</button>"
"<button type=\"button\" data-m=\"1\">Auto</button>"
"<button type=\"button\" data-m=\"2\">On</button>"
"</div>"
"<div class=\"fan\" id=\"fan\"><span class=\"dot\"></span>"
"<span><span id=\"devlabel\">Fan</span> <b id=\"fanstate\">--</b></span>"
"<span class=\"fmeta\" id=\"fanmeta\"></span></div>"
"<div class=\"warn\" id=\"warn\" hidden></div>"
"<h2>Status</h2>"
"<dl>"
"<div><dt>Reading age</dt><dd id=\"age\">--</dd></div>"
"<div><dt>Reads ok / failed</dt><dd id=\"reads\">--</dd></div>"
"<div><dt>Clients</dt><dd id=\"clients\">--</dd></div>"
"<div><dt>Uptime</dt><dd id=\"up\">--</dd></div>"
"<div><dt>Free heap</dt><dd id=\"heap\">--</dd></div>"
"</dl>"
"<div class=\"rangerow\"><h2>History</h2>"
"<select id=\"range\" aria-label=\"Time range\">"
"<option value=\"1\">Last 24 hours</option>"
"<option value=\"2\">Last 2 days</option>"
"<option value=\"3\">Last 3 days</option>"
"<option value=\"4\">Last 4 days</option>"
"<option value=\"5\">Last 5 days</option>"
"<option value=\"6\">Last 6 days</option>"
"<option value=\"7\" selected>Last 7 days</option>"
"</select></div>"
"<div id=\"chart\" class=\"viz\">"
"<div class=\"panel\"><div class=\"ptitle\"><span>Temperature &deg;C</span>"
"<span class=\"pnow\" id=\"nt\">--</span></div>"
"<canvas id=\"ct\"></canvas></div>"
"<div class=\"panel\"><div class=\"ptitle\"><span>Humidity %RH</span>"
"<span class=\"pnow\" id=\"nh\">--</span></div>"
"<canvas id=\"ch\"></canvas></div>"
"<div class=\"xax\"><span id=\"xa\"></span><span>now</span></div>"
"<p class=\"key\"><span><i style=\"background:var(--fan);opacity:.3\"></i>"
"<span id=\"keyfan\">Fan</span> running</span>"
"<span><i style=\"background:var(--muted);opacity:.25\"></i>No reading</span></p>"
"<p id=\"read\">&nbsp;</p>"
"</div>"
"<h2>Settings</h2>"
"<form id=\"f\"><fieldset><legend>Controlled device</legend>"
"<div class=\"row\"><input type=\"text\" id=\"devname\" maxlength=\"23\" "
"placeholder=\"Fan\" aria-label=\"Name of the controlled device\" required></div>"
"<p class=\"hint\">What the relay switches. Shown throughout this page; it "
"changes nothing about how the controller behaves.</p>"
"</fieldset><fieldset style=\"margin-top:14px\"><legend id=\"trigleg\">Fan triggers</legend>"
"<div class=\"tog\"><span>Use temperature</span>"
"<label class=\"sw\"><input type=\"checkbox\" id=\"en_t\" checked><i></i></label></div>"
"<div class=\"tog\"><span>Use humidity</span>"
"<label class=\"sw\"><input type=\"checkbox\" id=\"en_h\" checked><i></i></label></div>"
"<p class=\"hint\" id=\"trighint\"></p>"
"</fieldset><fieldset style=\"margin-top:14px\"><legend>Thresholds</legend>"
"<div class=\"row\" id=\"row_t\"><label for=\"c1\">Temperature threshold (&deg;C)</label>"
"<input type=\"number\" id=\"c1\" name=\"temp_threshold_c\" min=\"0\" max=\"50\" required></div>"
"<div class=\"row\" id=\"row_h\"><label for=\"c2\">Humidity threshold (%RH)</label>"
"<input type=\"number\" id=\"c2\" name=\"humidity_threshold_pct\" min=\"20\" max=\"95\" required></div>"
"<div class=\"row\"><label for=\"c3\">Start delay (min)</label>"
"<input type=\"number\" id=\"c3\" name=\"start_delay_s\" min=\"0\" max=\"60\" step=\"1\" required></div>"
"<div class=\"row\"><label for=\"c4\">Max fan duration (min)</label>"
"<input type=\"number\" id=\"c4\" name=\"max_fan_duration_s\" min=\"1\" max=\"120\" step=\"1\" required></div>"
"<div class=\"row\"><label for=\"c5\">Grace period (min)</label>"
"<input type=\"number\" id=\"c5\" name=\"grace_period_s\" min=\"0\" max=\"120\" step=\"1\" required></div>"
"<div class=\"row\"><label for=\"c6\">Sensor poll interval (s)</label>"
"<input type=\"number\" id=\"c6\" name=\"sensor_poll_interval_s\" min=\"2\" max=\"60\" required></div>"
"<button type=\"submit\" id=\"save\">Save settings</button>"
"<p class=\"hint\">Saved to flash and applied immediately. The poll interval "
"takes effect on the next reading. Times are in minutes here; the JSON API "
"uses seconds.</p>"
"</fieldset></form>"
"<details><summary>Network settings</summary>"
"<fieldset style=\"margin-top:4px\"><legend>Wi-Fi mode</legend>"
"<div class=\"seg wifi\" id=\"wseg\">"
"<button type=\"button\" data-w=\"0\">Own access point</button>"
"<button type=\"button\" data-w=\"1\">Join a network</button>"
"</div>"
"<div id=\"apbox\">"
"<div class=\"row\"><input type=\"text\" id=\"apssid\" maxlength=\"32\" "
"placeholder=\"Greenhouse-XXXX (from MAC)\" aria-label=\"Access point name\"></div>"
"<div class=\"row\" style=\"padding-top:0\"><input type=\"password\" id=\"appw\" "
"maxlength=\"63\" autocomplete=\"new-password\" "
"aria-label=\"Access point password\"></div>"
"<p class=\"hint\">The device serves its own network. Leave the name empty to "
"derive it from this unit's MAC address. Leave the password empty to keep the "
"current one; at least 8 characters to change it.</p>"
"</div>"
"<div id=\"stabox\" hidden>"
"<div class=\"row\"><input type=\"text\" id=\"stassid\" maxlength=\"32\" "
"placeholder=\"Network name\" aria-label=\"Name of the network to join\"></div>"
"<div class=\"row\" style=\"padding-top:0\"><input type=\"password\" id=\"stapw\" "
"maxlength=\"63\" autocomplete=\"new-password\" "
"aria-label=\"Password of the network to join\"></div>"
"<p class=\"hint\">The device joins your existing network and the readings stay "
"reachable from anywhere in the house. Find its address in your router's client "
"list, under the access point name above. If it cannot join at startup it "
"serves its own network for that boot and tries yours again at the next "
"restart.</p>"
"</div>"
"<button type=\"button\" id=\"bnet\">Save network settings</button>"
"<p class=\"hint\"><b>Takes effect after a restart.</b> You will have to "
"reconnect to reach this page again. If you lose access altogether, tap the "
"reset button on the board "
"<span id=\"frn\">5</span> times in a row, about a second apart: that restores "
"the built-in access point and password.</p>"
"</fieldset></details>"
"<details><summary>Bring-up tools</summary>"
"<div class=\"tools\">"
"<button type=\"button\" id=\"btest\">Test relay (5 s)</button>"
"<button type=\"button\" id=\"bpol\">Invert polarity</button>"
"<button type=\"button\" id=\"breboot\">Restart device</button>"
"</div>"
"<p class=\"hint\" id=\"bhint\"></p></details>"
"<div id=\"msg\" hidden></div>"
"</main><script>"
"var F=[['temp_threshold_c',1],['humidity_threshold_pct',1],['start_delay_s',60],"
"['max_fan_duration_s',60],['grace_period_s',60],['sensor_poll_interval_s',1]];"
"var MODE=1,DEV='Fan',WM=0;"
"function paintW(m){WM=m;"
"Array.prototype.forEach.call(document.getElementById('wseg').children,function(b){"
"b.className=(Number(b.dataset.w)===m)?'sel':''});"
"document.getElementById('apbox').hidden=(m!==0);"
"document.getElementById('stabox').hidden=(m!==1);}"
"function setDev(n){if(!n||n===DEV)return;DEV=n;"
"document.getElementById('devlabel').textContent=n;"
"document.getElementById('keyfan').textContent=n;"
"document.getElementById('trigleg').textContent=n+' triggers';}"
"function texts(){"
"document.getElementById('trighint').textContent="
"DEV+' runs when temperature or humidity is at or above its threshold. A "
"disabled input is still measured and charted, it just stops triggering the '"
"+DEV.toLowerCase()+'.';"
"document.getElementById('bhint').textContent="
"'Use the test button to identify the relay by ear. If the relay is energised "
"when the '+DEV.toLowerCase()+' should be off, invert the polarity.';}"
"function paintMode(m){MODE=m;"
"Array.prototype.forEach.call(document.getElementById('seg').children,function(b){"
"b.className=(Number(b.dataset.m)===m)?'sel':''})}"
"async function setMode(m){paintMode(m);try{"
"const r=await fetch('/api/config',{method:'POST',"
"headers:{'Content-Type':'application/json'},body:JSON.stringify({fan_mode:m})});"
"const d=await r.json();"
"if(r.ok&&d.saved){show(m===1?(DEV+' back under automatic control.'):"
"(m===2?(DEV+' forced on. It will stay on until you change this.'):"
"(DEV+' forced off. It will not run until you change this.')),m===1)}"
"else{show(d.error||'Could not change mode.',false)}"
"}catch(e){show('Could not reach the controller.',false)}}"
"Array.prototype.forEach.call(document.getElementById('seg').children,function(b){"
"b.addEventListener('click',function(){setMode(Number(b.dataset.m))})});"
"function dim(){"
"var t=document.getElementById('en_t').checked,h=document.getElementById('en_h').checked;"
"document.getElementById('row_t').className=t?'row':'row off';"
"document.getElementById('row_h').className=h?'row':'row off';}"
"async function setFlag(key,val){try{"
"const r=await fetch('/api/config',{method:'POST',"
"headers:{'Content-Type':'application/json'},body:JSON.stringify(Object.fromEntries([[key,val]]))});"
"const d=await r.json();"
"if(!(r.ok&&d.saved))show(d.error||'Could not save.',false);else show(val?"
"(key==='temp_enabled'?'Temperature now triggers ':'Humidity now triggers ')+DEV.toLowerCase()+'.':"
"(key==='temp_enabled'?'Temperature no longer triggers ':'Humidity no longer triggers ')+DEV.toLowerCase()+'.',true);"
"}catch(e){show('Could not reach the controller.',false)}}"
"['en_t','en_h'].forEach(function(id){document.getElementById(id).addEventListener('change',function(){"
"dim();setFlag(id==='en_t'?'temp_enabled':'humidity_enabled',this.checked)})});"
"function dur(s){if(s<60)return s+'s';if(s<3600)return Math.floor(s/60)+'m '+(s%60)+'s';"
"return Math.floor(s/3600)+'h '+Math.floor((s%3600)/60)+'m'}"
"function show(t,good){var m=document.getElementById('msg');m.hidden=false;"
"m.textContent=t;m.className=good?'ok':'warn'}"
"async function tick(){try{"
"const r=await fetch('/api/status',{cache:'no-store'});const d=await r.json();"
"document.getElementById('ssid').textContent=d.ssid+(d.ip?' \\u00b7 '+d.ip:'')"
"+(d.sensor?' \\u00b7 '+d.sensor:'');"
"var nw=document.getElementById('netwarn');"
"if(d.net==='recovery'){nw.hidden=false;nw.textContent="
"'Could not join the configured network, so the device is serving its own for "
"now. Nothing has been changed: it tries your network again at the next "
"restart, and restarts by itself once nobody is connected here.'}"
"else if(d.factory_reset){nw.hidden=false;nw.textContent="
"'Factory defaults were restored from the reset button. The device is back on "
"its own access point with the built-in password.'}"
"else{nw.hidden=true}"
"document.getElementById('t').innerHTML=(d.valid?d.temperature_c.toFixed(1):'--')+'<span class=\"unit\"> \\u00b0C</span>';"
"document.getElementById('h').innerHTML=(d.valid?d.humidity_pct.toFixed(1):'--')+'<span class=\"unit\"> %</span>';"
"document.getElementById('age').textContent=d.valid?dur(d.age_s):'never';"
"document.getElementById('reads').textContent=d.reads_ok+' / '+d.reads_failed;"
"document.getElementById('clients').textContent=d.clients;"
"document.getElementById('up').textContent=dur(d.uptime_s);"
"document.getElementById('heap').textContent=(d.free_heap/1024).toFixed(1)+' kB';"
"if(d.device){setDev(d.device);texts()}"
"if(typeof d.mode==='number'&&d.mode!==MODE)paintMode(d.mode);"
"var on=(typeof d.relay_on==='boolean')?d.relay_on:d.fan_on;"
"var fe=document.getElementById('fan');fe.className=on?'fan on':'fan';"
"document.getElementById('fanstate').textContent=on?'running':'off';"
"var meta=d.state;"
"if(d.remaining_s>0)meta+=' \u00b7 '+dur(d.remaining_s)+' left';"
"else if(d.state==='idle'&&(d.temp_high||d.humidity_high))meta+=' \u00b7 over threshold';"
"document.getElementById('fanmeta').textContent=meta;"
"const w=document.getElementById('warn');"
"if(d.mode===2){w.hidden=false;w.textContent=DEV+' is forced on and will not switch off by itself. Set Auto to return to automatic control.';}"
"else if(d.mode===0){w.hidden=false;w.textContent=DEV+' is forced off and will not run whatever the readings. Set Auto to return to automatic control.';}"
"else if(d.auto_disabled){w.hidden=false;w.textContent='Both triggers are disabled, so '+DEV.toLowerCase()+' will not start automatically. Readings are still recorded.';}"
"else if(!d.valid){w.hidden=false;w.textContent='No sensor reading yet ('+d.last_error+'). Check the sensor wiring and pull-up.';}"
"else if(d.stale){w.hidden=false;w.textContent='Reading is stale ('+dur(d.age_s)+' old, '+d.last_error+').';}"
"else{w.hidden=true}"
"}catch(e){}}"
"async function loadCfg(){try{"
"const r=await fetch('/api/config',{cache:'no-store'});const d=await r.json();"
"F.forEach(function(f){var el=document.getElementsByName(f[0])[0];"
"if(el&&document.activeElement!==el)el.value=Math.round(d[f[0]]/f[1])});"
"var dn=document.getElementById('devname');"
"if(d.device_name&&document.activeElement!==dn)dn.value=d.device_name;"
"var ap=document.getElementById('apssid');"
"if(document.activeElement!==ap&&typeof d.ap_ssid==='string')ap.value=d.ap_ssid;"
"var ss=document.getElementById('stassid');"
"if(document.activeElement!==ss&&typeof d.sta_ssid==='string')ss.value=d.sta_ssid;"
"if(typeof d.wifi_mode==='number'&&document.activeElement!==ap"
"&&document.activeElement!==ss)paintW(d.wifi_mode);"
"document.getElementById('appw').placeholder="
"d.ap_password_set?'Unchanged':'None, the network is open';"
"document.getElementById('stapw').placeholder="
"d.sta_password_set?'Unchanged':'None, the network is open';"
"if(typeof d.factory_reset_presses==='number')"
"document.getElementById('frn').textContent=d.factory_reset_presses;"
"if(d.device_name){setDev(d.device_name);texts()}"
"if(typeof d.fan_mode==='number')paintMode(d.fan_mode);"
"document.getElementById('en_t').checked=d.temp_enabled!==false;"
"document.getElementById('en_h').checked=d.humidity_enabled!==false;"
"dim();"
"}catch(e){}}"
"document.getElementById('f').addEventListener('submit',async function(ev){"
"ev.preventDefault();var b=document.getElementById('save');b.disabled=true;"
"var body={};F.forEach(function(f){"
"body[f[0]]=Math.round(Number(document.getElementsByName(f[0])[0].value)*f[1])});"
"body.device_name=document.getElementById('devname').value.trim()||'Fan';"
"body.temp_enabled=document.getElementById('en_t').checked;"
"body.humidity_enabled=document.getElementById('en_h').checked;"
"try{const r=await fetch('/api/config',{method:'POST',"
"headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});"
"const d=await r.json();"
"if(r.ok&&d.saved){show('Settings saved.',true)}else{show(d.error||'Save failed.',false)}"
"}catch(e){show('Could not reach the controller.',false)}"
"b.disabled=false;});"
"var HIST=null;"
"function css(n){return getComputedStyle(document.getElementById('chart')).getPropertyValue(n).trim()}"
"function draw(cv,vals,lo,hi,col,hoverIdx){"
"var d=window.devicePixelRatio||1,W=cv.clientWidth,H=cv.clientHeight;"
"cv.width=W*d;cv.height=H*d;var g=cv.getContext('2d');g.setTransform(d,0,0,d,0,0);"
"g.clearRect(0,0,W,H);"
"if(!HIST||!vals.length)return;"
"var n=vals.length,PL=34,PR=6,PT=6,PB=4,w=W-PL-PR,h=H-PT-PB;"
"var x=function(i){return PL+(n<2?w/2:i*w/(n-1))};"
"var y=function(v){return PT+h-(v-lo)/(hi-lo)*h};"
"g.strokeStyle=css('--grid');g.fillStyle=css('--muted');g.lineWidth=1;"
"g.font='12px system-ui,sans-serif';g.textAlign='right';g.textBaseline='middle';"
"for(var k=0;k<=2;k++){var v=lo+(hi-lo)*k/2,yy=Math.round(y(v))+0.5;"
"g.beginPath();g.moveTo(PL,yy);g.lineTo(W-PR,yy);g.stroke();"
"g.fillText(String(Math.round(v)),PL-6,yy)}"
"g.fillStyle=css('--fan');g.globalAlpha=.22;"
"HIST.fan.forEach(function(s){var a=x(s[0]),b=x(Math.min(s[1],n-1));"
"g.fillRect(a,PT,Math.max(b-a,1.5),h)});g.globalAlpha=1;"
"g.fillStyle=css('--muted');g.globalAlpha=.16;"
"var run=-1;for(var i=0;i<=n;i++){var bad=i<n&&vals[i]===null;"
"if(bad&&run<0)run=i;else if(!bad&&run>=0){g.fillRect(x(run),PT,Math.max(x(i-1)-x(run),1.5),h);run=-1}}"
"g.globalAlpha=1;"
"g.strokeStyle=col;g.lineWidth=2;g.lineJoin='round';g.lineCap='round';"
"g.beginPath();var pen=false;"
"for(var i=0;i<n;i++){if(vals[i]===null){pen=false;continue}"
"if(!pen){g.moveTo(x(i),y(vals[i]));pen=true}else{g.lineTo(x(i),y(vals[i]))}}"
"g.stroke();"
"if(hoverIdx!=null&&vals[hoverIdx]!=null){"
"g.strokeStyle=css('--muted');g.lineWidth=1;g.globalAlpha=.6;"
"g.beginPath();g.moveTo(Math.round(x(hoverIdx))+0.5,PT);"
"g.lineTo(Math.round(x(hoverIdx))+0.5,PT+h);g.stroke();g.globalAlpha=1;"
"g.fillStyle=col;g.beginPath();g.arc(x(hoverIdx),y(vals[hoverIdx]),4,0,6.284);g.fill();"
"g.strokeStyle=css('--surface-1');g.lineWidth=2;g.stroke()}"
"}"
"function bounds(v,pad,floor){var a=v.filter(function(z){return z!==null});"
"if(!a.length)return[0,1];var mn=Math.min.apply(null,a),mx=Math.max.apply(null,a);"
"if(mx-mn<floor){var m=(mx+mn)/2;mn=m-floor/2;mx=m+floor/2}"
"return[Math.floor(mn-pad),Math.ceil(mx+pad)]}"
"var TB=[0,1],HB=[0,1],HOVER=null;"
"function redraw(){if(!HIST)return;"
"draw(document.getElementById('ct'),HIST.t,TB[0],TB[1],css('--temp'),HOVER);"
"draw(document.getElementById('ch'),HIST.h,HB[0],HB[1],css('--hum'),HOVER)}"
"function ago(i){if(!HIST)return'';"
"var s=(HIST.count-1-i)*HIST.interval_s+HIST.age_s;"
"if(s<3600)return Math.round(s/60)+' min ago';"
"if(s<86400)return(s/3600).toFixed(1)+' h ago';return(s/86400).toFixed(1)+' days ago'}"
"function onMove(ev){if(!HIST||!HIST.count)return;"
"var cv=document.getElementById('ct'),r=cv.getBoundingClientRect();"
"var cx=(ev.touches?ev.touches[0].clientX:ev.clientX)-r.left;"
"var PL=34,w=r.width-PL-6,n=HIST.count;"
"var i=Math.round((cx-PL)/(w||1)*(n-1));i=Math.max(0,Math.min(n-1,i));"
"HOVER=i;var f=HIST.fan.some(function(s){return i>=s[0]&&i<s[1]});"
"document.getElementById('read').textContent="
"(HIST.t[i]===null?'no reading':HIST.t[i].toFixed(1)+' \\u00b0C, '+HIST.h[i].toFixed(1)+' %RH')"
"+' \\u00b7 '+ago(i)+(f?' \\u00b7 fan on':'');"
"redraw()}"
"function onLeave(){HOVER=null;document.getElementById('read').innerHTML='&nbsp;';redraw()}"
"['ct','ch'].forEach(function(id){var c=document.getElementById(id);"
"c.addEventListener('mousemove',onMove);c.addEventListener('mouseleave',onLeave);"
"c.addEventListener('touchstart',onMove);c.addEventListener('touchmove',onMove);"
"c.addEventListener('touchend',onLeave)});"
"window.addEventListener('resize',redraw);"
"function days(){return document.getElementById('range').value}"
"async function loadHist(){try{"
"const r=await fetch('/api/history?days='+days(),{cache:'no-store'});HIST=await r.json();"
"if(!HIST.count)return;"
"TB=bounds(HIST.t,1,5);HB=bounds(HIST.h,2,10);"
"var lt=null,lh=null;"
"for(var i=HIST.count-1;i>=0;i--){if(HIST.t[i]!==null){lt=HIST.t[i];lh=HIST.h[i];break}}"
"document.getElementById('nt').textContent=lt===null?'--':lt.toFixed(1)+' \\u00b0C';"
"document.getElementById('nh').textContent=lh===null?'--':lh.toFixed(1)+' %';"
"var span=HIST.count*HIST.interval_s;"
"document.getElementById('xa').textContent=HIST.count<2?'':"
"(span<86400?Math.round(span/3600)+' h ago':Math.round(span/86400)+' days ago');"
"redraw()}catch(e){}}"
"document.getElementById('btest').addEventListener('click',async function(){"
"var b=this;b.disabled=true;"
"try{const r=await fetch('/api/relay/test?seconds=5',{method:'POST'});"
"const d=await r.json();"
"show(d.testing?('Relay on for '+d.seconds+' s - listen for the click.'):"
"(d.error||'Test failed.'),!!d.testing)}"
"catch(e){show('Could not reach the controller.',false)}"
"setTimeout(function(){b.disabled=false},5200);});"
"document.getElementById('bpol').addEventListener('click',async function(){"
"try{const g=await fetch('/api/config',{cache:'no-store'});const c=await g.json();"
"const r=await fetch('/api/config',{method:'POST',"
"headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({relay_active_low:!c.relay_active_low})});"
"const d=await r.json();"
"show(d.saved?('Relay polarity is now active '+(!c.relay_active_low?'low':'high')+'.'):"
"(d.error||'Could not change polarity.'),!!d.saved)}"
"catch(e){show('Could not reach the controller.',false)}});"
"Array.prototype.forEach.call(document.getElementById('wseg').children,function(b){"
"b.addEventListener('click',function(){paintW(Number(b.dataset.w))})});"
"document.getElementById('bnet').addEventListener('click',async function(){"
"var b=this;b.disabled=true;var body={wifi_mode:WM};"
"if(WM===0){body.ap_ssid=document.getElementById('apssid').value.trim();"
"body.ap_password=document.getElementById('appw').value}"
"else{body.sta_ssid=document.getElementById('stassid').value.trim();"
"body.sta_password=document.getElementById('stapw').value}"
"try{const r=await fetch('/api/config',{method:'POST',"
"headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});"
"const d=await r.json();"
"if(r.ok&&d.saved){document.getElementById('appw').value='';"
"document.getElementById('stapw').value='';"
"show(WM===0?('Saved. Restart the device, then join '"
"+(body.ap_ssid||'the Greenhouse-XXXX network')+'.'):"
"('Saved. Restart the device and it will join '+body.sta_ssid"
"+'. Look for it in your router client list under the access point name.'),true);"
"loadCfg()}"
"else{show(d.error||'Could not save the network settings.',false)}"
"}catch(e){show('Could not reach the controller.',false)}"
"b.disabled=false;});"
"document.getElementById('breboot').addEventListener('click',async function(){"
"if(!confirm('Restart the controller? The relay switches off while it boots.'))return;"
"try{const r=await fetch('/api/reboot',{method:'POST'});const d=await r.json();"
"show(d.restarting?'Restarting. Rejoin the network and reload this page.':"
"(d.error||'Could not restart.'),!!d.restarting)}"
"catch(e){show('Could not reach the controller.',false)}});"
"document.getElementById('range').addEventListener('change',function(){"
"HOVER=null;document.getElementById('read').innerHTML='&nbsp;';loadHist()});"
"loadHist();setInterval(loadHist,60000);"

"texts();tick();loadCfg();setInterval(tick,2000);"
"</script></body></html>";

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    /* This SDK predates HTTPD_RESP_USE_STRLEN; pass the length explicitly. */
    return httpd_resp_send(req, index_html, sizeof(index_html) - 1);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    sensor_snapshot_t s;
    sensor_get(&s);

    controller_status_t c;
    controller_get(&c);

    greenhouse_config_t cfg;
    config_get(&cfg);

    char buf[704];
    int n = snprintf(buf, sizeof(buf),
                     "{\"ssid\":\"%s\""
                     ",\"valid\":%s"
                     ",\"stale\":%s"
                     ",\"temperature_c\":%d.%d"
                     ",\"humidity_pct\":%d.%d"
                     ",\"age_s\":%u"
                     ",\"reads_ok\":%u"
                     ",\"reads_failed\":%u"
                     ",\"last_error\":\"%s\""
                     ",\"clients\":%d"
                     ",\"uptime_s\":%u"
                     ",\"sensor\":\"%s\""
                     ",\"free_heap\":%u"
                     ",\"fan_on\":%s"
                     ",\"state\":\"%s\""
                     ",\"in_state_s\":%u"
                     ",\"remaining_s\":%u"
                     ",\"temp_high\":%s"
                     ",\"humidity_high\":%s"
                     ",\"temp_enabled\":%s"
                     ",\"humidity_enabled\":%s"
                     ",\"auto_disabled\":%s"
                     ",\"mode\":%u"
                     ",\"relay_on\":%s"
                     ",\"ip\":\"%s\""
                     ",\"net\":\"%s\""
                     ",\"factory_reset\":%s"
                     ",\"device\":\"%s\"}",
                     wifi_ssid(),
                     s.valid ? "true" : "false",
                     s.stale ? "true" : "false",
                     s.temperature_dc / 10, abs(s.temperature_dc % 10),
                     s.humidity_dpct / 10, abs(s.humidity_dpct % 10),
                     s.age_s,
                     s.reads_ok,
                     s.reads_failed,
                     sensor_error_name(s.last_error),
                     wifi_client_count(),
                     (uint32_t)(esp_timer_get_time() / 1000000),
                     sensor_model(),
                     esp_get_free_heap_size(),
                     c.fan_on ? "true" : "false",
                     controller_state_name(c.state),
                     c.in_state_s,
                     c.remaining_s,
                     c.temp_high ? "true" : "false",
                     c.humidity_high ? "true" : "false",
                     c.temp_enabled ? "true" : "false",
                     c.humidity_enabled ? "true" : "false",
                     c.auto_disabled ? "true" : "false",
                     c.mode,
                     relay_is_on() ? "true" : "false",
                     wifi_ip(),
                     wifi_state_name(),
                     factory_reset_triggered() ? "true" : "false",
                     cfg.device_name);

    if (n < 0 || n >= (int)sizeof(buf)) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, n);
}

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *body)
{
    if (status != NULL) {
        httpd_resp_set_status(req, status);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, strlen(body));
}

static esp_err_t config_get_handler(httpd_req_t *req)
{
    greenhouse_config_t c;
    config_get(&c);

    char buf[448];
    int n = snprintf(buf, sizeof(buf),
                     "{\"temp_threshold_c\":%d"
                     ",\"humidity_threshold_pct\":%u"
                     ",\"start_delay_s\":%u"
                     ",\"max_fan_duration_s\":%u"
                     ",\"grace_period_s\":%u"
                     ",\"sensor_poll_interval_s\":%u"
                     ",\"relay_active_low\":%s"
                     ",\"temp_enabled\":%s"
                     ",\"humidity_enabled\":%s"
                     ",\"fan_mode\":%u"
                     ",\"device_name\":\"%s\""
                     ",\"ap_ssid\":\"%s\""
                     ",\"wifi_mode\":%u"
                     ",\"sta_ssid\":\"%s\""
                     ",\"ap_password_set\":%s"
                     ",\"sta_password_set\":%s"
                     ",\"factory_reset_presses\":%d}",
                     c.temp_threshold_c, c.humidity_threshold_pct,
                     c.start_delay_s, c.max_fan_duration_s, c.grace_period_s,
                     c.sensor_poll_interval_s,
                     c.relay_active_low ? "true" : "false",
                     c.temp_enabled ? "true" : "false",
                     c.humidity_enabled ? "true" : "false",
                     c.fan_mode,
                     c.device_name,
                     c.ap_ssid,
                     c.wifi_mode,
                     c.sta_ssid,
                     /* Whether one is set, never what it is. The page has no
                      * business holding the passphrase, and rendering it into
                      * the response would put it in every proxy and cache
                      * between here and the browser. */
                     c.ap_password[0] != '\0' ? "true" : "false",
                     c.sta_password[0] != '\0' ? "true" : "false",
                     CONFIG_GREENHOUSE_FACTORY_RESET_PRESSES);

    if (n < 0 || n >= (int)sizeof(buf)) {
        return httpd_resp_send_500(req);
    }

    return send_json(req, NULL, buf);
}

/*
 * Read one integer field, leaving *dst untouched when the key is absent so a
 * partial POST updates only the fields it names. Returns false only when the key
 * is present with the wrong type.
 */
static bool json_int(const cJSON *root, const char *key, long *dst)
{
    const cJSON *item = cJSON_GetObjectItem(root, key);
    if (item == NULL) {
        return true;
    }
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    *dst = (long)item->valuedouble;
    return true;
}

/*
 * Read one string field. An absent key leaves the stored value alone, so a
 * partial POST updates only what it names.
 *
 * `keep_empty` is for the passphrases: the page is never told what they are, so
 * it sends an empty field to mean "unchanged" rather than "clear it". Clearing
 * one -- which opens the network -- is then only reachable through a deliberate
 * API call, not through a form the user left blank.
 *
 * Returns false when the key is present with the wrong type; an over-long value
 * is reported through *too_long so the caller can name the field.
 */
static bool json_str(const cJSON *root, const char *key, char *dst, size_t len,
                     bool keep_empty, const char **too_long)
{
    const cJSON *item = cJSON_GetObjectItem(root, key);
    if (item == NULL) {
        return true;
    }
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }
    if (keep_empty && item->valuestring[0] == '\0') {
        return true;
    }
    if (strlen(item->valuestring) >= len) {
        /* Refuse rather than truncate: a silently shortened name is worse than
         * being told it is too long, and a truncated passphrase would lock the
         * owner out of a network they believe they just configured. */
        *too_long = key;
        return true;
    }

    strlcpy(dst, item->valuestring, len);
    return true;
}

/*
 * config_save() names the field it rejected. Most are numeric and "out of
 * range" describes them, but the Wi-Fi fields need saying properly.
 */
static const char *explain(const char *field)
{
    if (field == NULL) {
        return "a setting is not valid";
    }
    if (strcmp(field, "ap_password") == 0 || strcmp(field, "sta_password") == 0) {
        return "a Wi-Fi password must be 8 to 63 characters";
    }
    if (strcmp(field, "sta_ssid") == 0) {
        return "joining a network needs its name";
    }
    if (strcmp(field, "ap_ssid") == 0) {
        return "that network name cannot be used";
    }
    if (strcmp(field, "wifi_mode") == 0) {
        return "unknown Wi-Fi mode";
    }
    return NULL;
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len >= CONFIG_BODY_MAX) {
        return send_json(req, HTTPD_400, "{\"error\":\"body too large\"}");
    }

    char body[CONFIG_BODY_MAX];
    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, body + received, req->content_len - received);
        if (r <= 0) {
            /* Let the client retry rather than half-applying a truncated body. */
            return ESP_FAIL;
        }
        received += r;
    }
    body[received] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return send_json(req, HTTPD_400, "{\"error\":\"malformed JSON\"}");
    }

    /* Start from the running config so an omitted field keeps its value. */
    greenhouse_config_t c;
    config_get(&c);

    long temp = c.temp_threshold_c;
    long hum = c.humidity_threshold_pct;
    long start = c.start_delay_s;
    long maxfan = c.max_fan_duration_s;
    long grace = c.grace_period_s;
    long poll = c.sensor_poll_interval_s;
    long mode = c.fan_mode;
    long wmode = c.wifi_mode;

    bool ok = json_int(root, "temp_threshold_c", &temp) &&
              json_int(root, "humidity_threshold_pct", &hum) &&
              json_int(root, "start_delay_s", &start) &&
              json_int(root, "max_fan_duration_s", &maxfan) &&
              json_int(root, "grace_period_s", &grace) &&
              json_int(root, "sensor_poll_interval_s", &poll) &&
              json_int(root, "fan_mode", &mode) &&
              json_int(root, "wifi_mode", &wmode);

    static const struct {
        const char *key;
        size_t offset;
    } flags[] = {
        {"relay_active_low", offsetof(greenhouse_config_t, relay_active_low)},
        {"temp_enabled", offsetof(greenhouse_config_t, temp_enabled)},
        {"humidity_enabled", offsetof(greenhouse_config_t, humidity_enabled)},
    };

    const char *too_long = NULL;
    ok = json_str(root, "device_name", c.device_name, sizeof(c.device_name),
                  false, &too_long) && ok;
    ok = json_str(root, "ap_ssid", c.ap_ssid, sizeof(c.ap_ssid),
                  false, &too_long) && ok;
    ok = json_str(root, "sta_ssid", c.sta_ssid, sizeof(c.sta_ssid),
                  false, &too_long) && ok;
    ok = json_str(root, "ap_password", c.ap_password, sizeof(c.ap_password),
                  true, &too_long) && ok;
    ok = json_str(root, "sta_password", c.sta_password, sizeof(c.sta_password),
                  true, &too_long) && ok;

    if (too_long != NULL) {
        char msg[96];
        snprintf(msg, sizeof(msg), "{\"error\":\"%s is too long\"}", too_long);
        cJSON_Delete(root);
        return send_json(req, HTTPD_400, msg);
    }

    for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
        const cJSON *item = cJSON_GetObjectItem(root, flags[i].key);
        if (item == NULL) {
            continue;
        }
        if (!cJSON_IsBool(item)) {
            ok = false;
            continue;
        }
        *((bool *)((char *)&c + flags[i].offset)) = cJSON_IsTrue(item);
    }

    cJSON_Delete(root);

    if (!ok) {
        return send_json(req, HTTPD_400, "{\"error\":\"a field has the wrong type\"}");
    }

    c.temp_threshold_c = (int16_t)temp;
    c.humidity_threshold_pct = (uint8_t)hum;
    c.start_delay_s = (uint16_t)start;
    c.max_fan_duration_s = (uint16_t)maxfan;
    c.grace_period_s = (uint16_t)grace;
    c.sensor_poll_interval_s = (uint8_t)poll;
    c.fan_mode = (uint8_t)mode;
    c.wifi_mode = (uint8_t)wmode;

    const char *bad_field = NULL;
    esp_err_t err = config_save(&c, &bad_field);

    char out[192];

    if (err == ESP_ERR_INVALID_ARG) {
        const char *why = explain(bad_field);
        if (why != NULL) {
            snprintf(out, sizeof(out), "{\"error\":\"%s\"}", why);
        } else {
            snprintf(out, sizeof(out), "{\"error\":\"%s is out of range\"}",
                     bad_field != NULL ? bad_field : "a field");
        }
        return send_json(req, HTTPD_400, out);
    }

    if (err != ESP_OK) {
        snprintf(out, sizeof(out), "{\"error\":\"could not save: %s\"}",
                 esp_err_to_name(err));
        return send_json(req, HTTPD_500, out);
    }

    /* Polarity may have changed; re-assert the pin now rather than at the next
     * controller tick, so bring-up gives immediate feedback. */
    relay_refresh();

    return send_json(req, NULL, "{\"saved\":true}");
}


/*
 * History is streamed in chunks rather than built in one buffer: 2016 samples is
 * roughly 12 kB of JSON, far more than we can hold at once, and copying a window
 * at a time also keeps the history lock held only briefly so the sensor task is
 * never blocked waiting on an HTTP client.
 */
#define HISTORY_WINDOW 64

static esp_err_t history_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    size_t held = history_count();
    uint32_t interval = history_interval_s();

    /*
     * ?days=N trims the response to the most recent N days. Filtering here
     * rather than in the browser matters on this device: the full seven days is
     * roughly 12 kB and the page refetches every minute, while a one-day view is
     * closer to 1.7 kB.
     */
    int days = 7;
    char query[48];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[8];
        if (httpd_query_key_value(query, "days", val, sizeof(val)) == ESP_OK) {
            int d = atoi(val);
            if (d >= 1 && d <= 31) {
                days = d;
            }
        }
    }

    size_t wanted = ((size_t)days * 86400u) / (interval ? interval : 1);
    size_t offset = (held > wanted) ? held - wanted : 0;
    size_t total = held - offset;

    char buf[256];

    int n = snprintf(buf, sizeof(buf),
                     "{\"interval_s\":%u,\"count\":%u,\"age_s\":%u,\"days\":%d,\"t\":[",
                     interval, (unsigned)total, history_age_s(), days);
    if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
        return ESP_FAIL;
    }

    history_sample_t win[HISTORY_WINDOW];

    /* Two value passes over the ring, then a third for fan spans below.
     * Re-reading the ring is cheaper than buffering all three arrays at once. */
    for (int pass = 0; pass < 2; pass++) {
        size_t done = 0;
        while (done < total) {
            size_t got = history_copy(offset + done, HISTORY_WINDOW, win);
            if (got > total - done) {
                got = total - done;
            }
            if (got == 0) {
                break;
            }

            int len = 0;
            for (size_t i = 0; i < got; i++) {
                const char *sep = (done + i == 0) ? "" : ",";
                if (win[i].flags & HISTORY_FLAG_VALID) {
                    int v = (pass == 0) ? win[i].temperature_dc : win[i].humidity_dpct;
                    len += snprintf(buf + len, sizeof(buf) - len, "%s%d.%d",
                                    sep, v / 10, abs(v % 10));
                } else {
                    /* null, not 0: the chart must show a gap, never a fake
                     * reading. With the sensor issues still open this matters. */
                    len += snprintf(buf + len, sizeof(buf) - len, "%snull", sep);
                }

                /* Flush well before the buffer could overflow on the next item. */
                if (len > (int)sizeof(buf) - 24) {
                    if (httpd_resp_send_chunk(req, buf, len) != ESP_OK) {
                        return ESP_FAIL;
                    }
                    len = 0;
                }
            }

            if (len > 0 && httpd_resp_send_chunk(req, buf, len) != ESP_OK) {
                return ESP_FAIL;
            }

            done += got;
        }

        const char *mid = (pass == 0) ? "],\"h\":[" : "],\"fan\":[";
        if (httpd_resp_send_chunk(req, mid, strlen(mid)) != ESP_OK) {
            return ESP_FAIL;
        }
    }

    /*
     * Fan periods go out as spans, not a flag per sample. The controller
     * switches the fan rarely and holds it for minutes, so a handful of ranges
     * replaces 2016 booleans.
     */
    bool in_span = false;
    size_t span_start = 0;
    bool first_span = true;
    size_t done = 0;

    while (done < total) {
        size_t got = history_copy(offset + done, HISTORY_WINDOW, win);
        if (got > total - done) {
            got = total - done;
        }
        if (got == 0) {
            break;
        }

        for (size_t i = 0; i < got; i++) {
            bool on = (win[i].flags & HISTORY_FLAG_FAN) != 0;
            size_t idx = done + i;

            if (on && !in_span) {
                in_span = true;
                span_start = idx;
            } else if (!on && in_span) {
                n = snprintf(buf, sizeof(buf), "%s[%u,%u]", first_span ? "" : ",",
                             (unsigned)span_start, (unsigned)idx);
                if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
                    return ESP_FAIL;
                }
                first_span = false;
                in_span = false;
            }
        }

        done += got;
    }

    if (in_span) {
        n = snprintf(buf, sizeof(buf), "%s[%u,%u]", first_span ? "" : ",",
                     (unsigned)span_start, (unsigned)total);
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            return ESP_FAIL;
        }
    }

    if (httpd_resp_send_chunk(req, "]}", 2) != ESP_OK) {
        return ESP_FAIL;
    }

    /* Zero-length chunk terminates the response. */
    return httpd_resp_send_chunk(req, NULL, 0);
}


static esp_err_t relay_test_handler(httpd_req_t *req)
{
    int seconds = relay_test_max_s();

    char query[48];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[8];
        if (httpd_query_key_value(query, "seconds", val, sizeof(val)) == ESP_OK) {
            seconds = atoi(val);
        }
    }

    esp_err_t err = relay_test_pulse(seconds);
    if (err != ESP_OK) {
        return send_json(req, HTTPD_500, "{\"error\":\"relay unavailable\"}");
    }

    char out[96];
    snprintf(out, sizeof(out), "{\"testing\":true,\"seconds\":%d}",
             seconds > relay_test_max_s() ? relay_test_max_s() : seconds);
    return send_json(req, NULL, out);
}


static void reboot_timer_cb(void *arg)
{
    esp_restart();
}

/*
 * Reboot is deferred by a moment so the HTTP response reaches the browser
 * first; restarting inside the handler drops the connection mid-reply and the
 * page cannot tell success from failure.
 */
static esp_err_t reboot_handler(httpd_req_t *req)
{
    static esp_timer_handle_t timer;

    if (timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = reboot_timer_cb,
            .name = "reboot",
        };
        if (esp_timer_create(&args, &timer) != ESP_OK) {
            return send_json(req, HTTPD_500, "{\"error\":\"could not schedule restart\"}");
        }
    }

    esp_err_t err = send_json(req, NULL, "{\"restarting\":true}");
    esp_timer_start_once(timer, 700000);
    return err;
}

static const httpd_uri_t uri_index = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = index_handler,
};

static const httpd_uri_t uri_status = {
    .uri = "/api/status",
    .method = HTTP_GET,
    .handler = status_handler,
};

static const httpd_uri_t uri_reboot = {
    .uri = "/api/reboot",
    .method = HTTP_POST,
    .handler = reboot_handler,
};

static const httpd_uri_t uri_relay_test = {
    .uri = "/api/relay/test",
    .method = HTTP_POST,
    .handler = relay_test_handler,
};

static const httpd_uri_t uri_history = {
    .uri = "/api/history",
    .method = HTTP_GET,
    .handler = history_handler,
};

static const httpd_uri_t uri_config_get = {
    .uri = "/api/config",
    .method = HTTP_GET,
    .handler = config_get_handler,
};

static const httpd_uri_t uri_config_post = {
    .uri = "/api/config",
    .method = HTTP_POST,
    .handler = config_post_handler,
};

esp_err_t web_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    httpd_register_uri_handler(s_server, &uri_index);
    httpd_register_uri_handler(s_server, &uri_status);
    httpd_register_uri_handler(s_server, &uri_reboot);
    httpd_register_uri_handler(s_server, &uri_relay_test);
    httpd_register_uri_handler(s_server, &uri_history);
    httpd_register_uri_handler(s_server, &uri_config_get);
    httpd_register_uri_handler(s_server, &uri_config_post);

    ESP_LOGI(TAG, "http server listening on port %d", config.server_port);

    return ESP_OK;
}
