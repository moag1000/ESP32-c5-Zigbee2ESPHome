/**
 * @file captive_portal_html.h
 * @brief Embedded HTML for WiFi Captive Portal
 *
 * Contains the HTML/CSS/JS for the captive portal configuration page.
 * Stored in flash (~2KB). Placeholders %SCAN_RESULTS% and %VERSION%
 * are replaced at runtime.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef CAPTIVE_PORTAL_HTML_H
#define CAPTIVE_PORTAL_HTML_H

/**
 * @brief Main captive portal HTML page
 *
 * Placeholders:
 *   %SCAN_RESULTS% - Replaced with WiFi network list HTML
 *   %VERSION%      - Replaced with firmware version string
 */
static const char CAPTIVE_PORTAL_HTML[] =
    "<!DOCTYPE html>"
    "<html><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32-C5 WiFi Setup</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:-apple-system,sans-serif;background:#1a1a2e;color:#e0e0e0;"
    "min-height:100vh;display:flex;justify-content:center;align-items:center;padding:16px}"
    ".card{background:#16213e;border-radius:12px;padding:24px;width:100%%;max-width:400px;"
    "box-shadow:0 4px 24px rgba(0,0,0,.4)}"
    "h1{font-size:1.3em;text-align:center;margin-bottom:4px;color:#e94560}"
    ".sub{text-align:center;font-size:.8em;color:#888;margin-bottom:16px}"
    ".net{padding:10px 12px;margin:4px 0;background:#0f3460;border-radius:8px;cursor:pointer;"
    "display:flex;justify-content:space-between;align-items:center;transition:background .2s}"
    ".net:hover{background:#1a4a7a}"
    ".net .ssid{font-weight:600;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;max-width:70%%}"
    ".net .info{font-size:.8em;color:#aaa;display:flex;align-items:center;gap:6px}"
    ".lock::before{content:'\\1F512';font-size:.7em}"
    ".bars{display:inline-flex;align-items:flex-end;gap:1px;height:12px}"
    ".bars span{width:3px;background:#4a9;border-radius:1px}"
    ".b1{height:25%%}.b2{height:50%%}.b3{height:75%%}.b4{height:100%%}"
    ".weak .bars span{background:#e94560}"
    ".mid .bars span{background:#f0a500}"
    "label{display:block;font-size:.85em;margin:12px 0 4px;color:#aaa}"
    "input[type=text],input[type=password]{width:100%%;padding:10px;background:#0f3460;"
    "border:1px solid #1a4a7a;border-radius:6px;color:#fff;font-size:1em;outline:none}"
    "input:focus{border-color:#e94560}"
    ".btn{width:100%%;padding:12px;background:#e94560;color:#fff;border:none;border-radius:6px;"
    "font-size:1em;font-weight:600;cursor:pointer;margin-top:16px;transition:background .2s}"
    ".btn:hover{background:#c73e54}"
    ".btn:disabled{background:#555;cursor:not-allowed}"
    ".btn-scan{background:#0f3460;margin-top:8px}"
    ".btn-scan:hover{background:#1a4a7a}"
    ".status{text-align:center;padding:12px;margin-top:12px;border-radius:6px;display:none}"
    ".status.ok{display:block;background:#0a3d2a;color:#4a9}"
    ".status.err{display:block;background:#3d0a0a;color:#e94560}"
    ".status.wait{display:block;background:#2a2a0a;color:#f0a500}"
    ".scan-hint{text-align:center;color:#666;font-size:.8em;padding:8px}"
    ".pw-wrap{position:relative}"
    ".pw-toggle{position:absolute;right:8px;top:50%%;transform:translateY(-50%%);"
    "background:none;border:none;color:#aaa;cursor:pointer;font-size:1.1em;padding:4px}"
    "</style></head><body>"
    "<div class='card'>"
    "<h1>ESP32-C5 WiFi Setup</h1>"
    "<p class='sub'>v%VERSION%</p>"
    "<div id='nets'>%SCAN_RESULTS%</div>"
    "<button class='btn btn-scan' onclick='rescan()'>Scan Networks</button>"
    "<form id='f' onsubmit='return doConnect()'>"
    "<label for='s'>SSID</label>"
    "<input type='text' id='s' name='ssid' required maxlength='31' placeholder='Network name'>"
    "<label for='p'>Password</label>"
    "<div class='pw-wrap'>"
    "<input type='password' id='p' name='password' maxlength='63' placeholder='WiFi password'>"
    "<button type='button' class='pw-toggle' onclick='togglePw()'>&#128065;</button>"
    "</div>"
    "<button type='submit' class='btn' id='cb'>Connect</button>"
    "</form>"
    "<div class='status' id='st'></div>"
    "</div>"
    "<script>"
    "function selectNet(s){document.getElementById('s').value=s;"
    "document.getElementById('p').focus()}"
    "function togglePw(){var p=document.getElementById('p');"
    "p.type=p.type==='password'?'text':'password'}"
    "function rescan(){"
    "var btn=event.target;btn.disabled=true;btn.textContent='Scanning...';"
    "fetch('/scan').then(r=>r.text()).then(h=>{"
    "document.getElementById('nets').innerHTML=h;"
    "btn.disabled=false;btn.textContent='Scan Networks';"
    "}).catch(()=>{btn.disabled=false;btn.textContent='Scan Networks'})}"
    "var _poll;"
    "function pollStatus(){"
    "fetch('/status').then(r=>r.json()).then(d=>{"
    "var st=document.getElementById('st');"
    "var cb=document.getElementById('cb');"
    "if(d.state==='testing'){setTimeout(pollStatus,2000);return}"
    "clearTimeout(_poll);"
    "if(d.state==='connected'){"
    "st.className='status ok';st.textContent='Connected! IP: '+(d.ip||'')+'. Restarting...';"
    "cb.textContent='Success!';"
    "}else{"
    "st.className='status err';st.textContent='Failed: '+(d.error||'Connection failed');"
    "cb.disabled=false;cb.textContent='Connect'}"
    "}).catch(()=>{setTimeout(pollStatus,2000)})}"
    "function doConnect(){"
    "var s=document.getElementById('s').value;"
    "var p=document.getElementById('p').value;"
    "var st=document.getElementById('st');"
    "var cb=document.getElementById('cb');"
    "if(!s){st.className='status err';st.textContent='Enter SSID';return false}"
    "cb.disabled=true;cb.textContent='Connecting...';"
    "st.className='status wait';st.textContent='Testing connection...';"
    "fetch('/connect',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
    "body:'ssid='+encodeURIComponent(s)+'&password='+encodeURIComponent(p)"
    "}).then(r=>r.json()).then(d=>{"
    "if(d.testing){_poll=setTimeout(pollStatus,2000)}"
    "else if(d.error){st.className='status err';st.textContent=d.error;"
    "cb.disabled=false;cb.textContent='Connect'}"
    "}).catch(()=>{"
    "st.className='status err';st.textContent='Request failed';"
    "cb.disabled=false;cb.textContent='Connect'});"
    "return false}"
    "</script></body></html>";

#endif /* CAPTIVE_PORTAL_HTML_H */
