// --------- util & UI ----------
const $ = (id) => document.getElementById(id);
const logEl = $('logs');
function log(line, type = 'info') {
  const ts = new Date().toLocaleTimeString();
  logEl.value += `[${ts}] ${line}\n`;
  logEl.scrollTop = logEl.scrollHeight;
  (type === 'error' ? console.error : console.log)(line);
}
function setBadge(el, text, state) {
  el.textContent = text;
  el.classList.remove('muted','ok','err');
  if (state === 'ok') el.classList.add('ok');
  else if (state === 'err') el.classList.add('err');
  else el.classList.add('muted');
}
function uiStatus(msg, ok = false) {
  const el = $('status');
  el.textContent = msg;
  el.className = ok ? 'ok' : 'err';
  log(msg, ok ? 'info' : 'error');
}
$('clearLogs').addEventListener('click', () => (logEl.value = ''));

// --------- elements ----------
const select = $('project');
const installBtn = $('installBtn');
const autoImprov = $('autoImprov');
const autoSerial = $('autoSerial');

const ssidSelect = $('ssidSelect');
const ssidInput = $('ssidInput');
const passwordInput = $('password');
const improvBtn = $('improvBtn');
const scanBtn = $('scanBtn');

const deviceUrlInput = $('deviceUrl');
const probeBtn = $('probeBtn');
const saveBtn = $('saveBtn');

const portBadge = $('portBadge');
const improvBadge = $('improvBadge');
const wifiBadge = $('wifiBadge');
const httpBadge = $('httpBadge');

const serialConnectBtn = $('serialConnect');
const serialDisconnectBtn = $('serialDisconnect');

// --------- load projects ----------
async function loadProjects() {
  const res = await fetch('./projects.json');
  const projects = await res.json();
  for (const p of projects) {
    const opt = document.createElement('option');
    opt.value = p.manifest;
    opt.textContent = p.name;
    select.appendChild(opt);
  }
  if (projects[0]) installBtn.setAttribute('manifest', projects[0].manifest);
  log('Projekti učitani.');
}
select.addEventListener('change', () => {
  installBtn.setAttribute('manifest', select.value);
  log(`Odabran manifest: ${select.value}`);
});

// --------- SSID history ----------
const SSID_KEY = 'inkplate_ssids';
function getSSIDHistory() {
  try { return JSON.parse(localStorage.getItem(SSID_KEY) || '[]'); } catch { return []; }
}
function setSSIDHistory(ssids) {
  localStorage.setItem(SSID_KEY, JSON.stringify(Array.from(new Set(ssids)).slice(0,10)));
}
function renderSSIDOptions(list = []) {
  const hist = getSSIDHistory();
  const all = [...list, ...hist.filter(h => !list.includes(h))];
  ssidSelect.innerHTML = '';
  if (all.length === 0) {
    const o = document.createElement('option');
    o.value = ''; o.textContent = '— nema mreža, upiši ručno —';
    ssidSelect.appendChild(o);
  } else {
    for (const s of all) {
      const o = document.createElement('option'); o.value = s; o.textContent = s;
      ssidSelect.appendChild(o);
    }
  }
  // preselektiraj zadnji korišten
  if (hist.length) ssidSelect.value = hist[0];
}
renderSSIDOptions();

// kad user promijeni dropdown, sinkroniziraj u input (da može editirati)
ssidSelect.addEventListener('change', () => {
  ssidInput.value = ssidSelect.value || '';
});

// --------- Serial logging (permadev) ----------
let currentPort = null;
let readerCancel = null;

async function openSerialLogs() {
  if (!('serial' in navigator)) throw new Error('Web Serial nije dostupan u ovom pregledniku.');
  // ako već imamo port (npr. auto nakon flasha), koristi njega; inače pitaj
  if (!currentPort) currentPort = await navigator.serial.requestPort();
  await currentPort.open({ baudRate: 115200 });
  setBadge(portBadge, 'PORT: connected', 'ok');
  log('Serial: povezan.');

  const decoder = new TextDecoderStream();
  const ws = currentPort.readable.pipeTo(decoder.writable);
  const reader = decoder.readable.getReader();
  readerCancel = async () => { try { reader.releaseLock(); await currentPort.close(); } catch {} };

  (async () => {
    try {
      while (true) {
        const { value, done } = await reader.read();
        if (done) break;
        if (value) {
          // heuristika za status bedževe
          if (value.includes('WiFi STA OK') || value.includes('Connected. URL:'))
            setBadge(wifiBadge, 'WIFI: connected', 'ok');
          if (value.includes('Server started'))
            setBadge(httpBadge, 'HTTP: started', 'ok');
          log(value.replace(/\r/g,'').trimEnd());
        }
      }
    } catch (e) {
      log('Serial read error: ' + e.message, 'error');
    }
  })();
}
async function closeSerialLogs() {
  if (readerCancel) await readerCancel();
  readerCancel = null;
  currentPort = null;
  setBadge(portBadge, 'PORT: disconnected', 'muted');
  log('Serial: odspojen.');
}
serialConnectBtn.addEventListener('click', () => openSerialLogs().catch(e => uiStatus(e.message,false)));
serialDisconnectBtn.addEventListener('click', () => closeSerialLogs());

// --------- Improv SDK (browser) ----------
let ImprovSerial;
async function ensureImprovSDK() {
  if (!ImprovSerial) {
    const mod = await import('https://unpkg.com/improv-wifi-serial-sdk/dist/esm/improv.js');
    ImprovSerial = mod.ImprovSerial;
  }
  if (!('serial' in navigator)) throw new Error('Web Serial nije podržan.');
}

async function withPort(fn) {
  // koristi postojeći port ako smo već na logu; inače pitaj
  if (!currentPort) currentPort = await navigator.serial.requestPort();
  if (!currentPort.readable) await currentPort.open({ baudRate: 115200 });
  try { return await fn(currentPort); }
  finally { /* ostavi otvoren za logove */ }
}

async function sendWiFiOverSerial() {
  await ensureImprovSDK();
  const ssid = (ssidInput.value || ssidSelect.value || '').trim();
  const password = passwordInput.value.trim();
  if (!ssid) throw new Error('Odaberi/upiši SSID.');

  setSSIDHistory([ssid, ...getSSIDHistory()]);
  renderSSIDOptions();

  return withPort(async (port) => {
    const improv = new ImprovSerial(port);
    setBadge(improvBadge, 'IMPROV: connecting…', 'muted');
    log('Improv: connect handshake…');
    await improv.connect();

    // opcionalni scan je odvojen; ovdje šaljemo credse
    log('Improv: provisioning…');
    await improv.provision(ssid, password);

    // pokušaj dohvatiti URL
    let url = '';
    try { await improv.identify(); if (improv.info && improv.info.url) url = improv.info.url; } catch {}
    if (!url && improv.getInfo) { const info = improv.getInfo(); if (info && info.url) url = info.url; }

    if (url) {
      deviceUrlInput.value = url;
      setBadge(improvBadge, 'IMPROV: done', 'ok');
      setBadge(wifiBadge, 'WIFI: connected', 'ok');
      setBadge(httpBadge, 'HTTP: starting…', 'muted');
      log(`Uređaj online: ${url}`);
    } else {
      setBadge(improvBadge, 'IMPROV: done (no URL)', 'ok');
      log('Wi-Fi poslan. Ako URL fali, vidi “Visit device” ili DHCP listu.');
    }
  });
}

async function scanWifiViaImprov() {
  await ensureImprovSDK();
  return withPort(async (port) => {
    const improv = new ImprovSerial(port);
    setBadge(improvBadge, 'IMPROV: connecting…', 'muted');
    await improv.connect();
    log('Improv: tražim skeniranje mreža…');

    // SDK nema univerzalni "scan" za sve firmware-e; neke implementacije ga pružaju kao custom command.
    // Pokušaj "identify" i iskoristi eventualne "capabilities". Ako nema, javimo fallback.
    if (!improv.scan || typeof improv.scan !== 'function') {
      log('Ovaj firmware/SDK ne izlaže scan preko Improv-a. Koristi ručni SSID ili povuci iz povijesti.', 'error');
      throw new Error('Scan nije dostupan u ovoj implementaciji.');
    }

    const nets = await improv.scan(); // očekuje [{ssid, rssi, secure}, ...]
    const ssids = (nets || []).map(n => n.ssid).filter(Boolean);
    if (!ssids.length) throw new Error('Nema pronađenih mreža.');
    renderSSIDOptions(ssids);
    ssidSelect.value = ssids[0];
    ssidInput.value = ssids[0];
    log(`Pronađeno mreža: ${ssids.length}`);
  });
}

// gumbi
improvBtn.addEventListener('click', async () => {
  try { await sendWiFiOverSerial(); }
  catch (e) { uiStatus('Improv greška: ' + e.message, false); setBadge(improvBadge, 'IMPROV: error', 'err'); }
});
scanBtn.addEventListener('click', async () => {
  try { await scanWifiViaImprov(); }
  catch (e) { uiStatus(e.message, false); }
});

// --------- /provision ----------
async function resolveDeviceUrl() {
  let url = deviceUrlInput.value.trim();
  if (!url) throw new Error('Device URL nije postavljen.');
  if (!/^https?:\/\//.test(url)) url = 'http://' + url;
  return url;
}

saveBtn.addEventListener('click', async () => {
  try {
    const url = await resolveDeviceUrl();
    const body = {
      ssid: (ssidInput.value || ssidSelect.value || '').trim(),
      password: passwordInput.value.trim(),
      username: $('username').value || '',
      city: $('city').value || '',
      timeZone: Number($('timeZone').value || 0),
      latitude: Number($('latitude').value || 0),
      longitude: Number($('longitude').value || 0),
      metricUnits: $('metricUnits').value === 'true',
    };

    log(`POST ${url}/provision`);
    const res = await fetch(new URL('/provision', url), {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    });

    if (!res.ok) throw new Error('HTTP ' + res.status);
    await res.json().catch(() => ({}));
    uiStatus(`Postavke spremljene ✅`, true);
    setBadge(httpBadge, 'HTTP: reachable', 'ok');
  } catch (e) {
    uiStatus('Upis postavki nije uspio: ' + e.message, false);
    setBadge(httpBadge, 'HTTP: error', 'err');
  }
});

// --------- Probe /info ----------
probeBtn.addEventListener('click', async () => {
  try {
    const url = await resolveDeviceUrl();
    const res = await fetch(new URL('/info', url), { cache: 'no-store' });
    if (!res.ok) throw new Error('HTTP ' + res.status);
    const j = await res.json();
    uiStatus(`Uređaj na mreži (IP ${j.ip || 'n/a'})`, true);
    setBadge(httpBadge, 'HTTP: reachable', 'ok');
  } catch (e) {
    uiStatus('Ne mogu dohvatiti /info: ' + e.message, false);
    setBadge(httpBadge, 'HTTP: error', 'err');
  }
});

// --------- Auto nakon flasha ----------
function attachInstallEvents() {
  const evs = ['install-started', 'installing', 'install-failed', 'install-success', 'state-changed', 'log'];
  evs.forEach(ev => {
    installBtn.addEventListener(ev, (e) => {
      log(`[installer] ${ev}` + (e?.detail ? ` — ${JSON.stringify(e.detail)}` : ''));
      if (ev === 'install-success') {
        // 1) auto prikvači serial logove (ako je uključeno)
        if (autoSerial.checked) {
          openSerialLogs().catch(err => uiStatus('Serial log: ' + err.message, false));
        }
        // 2) auto Improv (pošalji SSID/lozinku koje je user upisao)
        if (autoImprov.checked) {
          (async () => {
            try {
              await sendWiFiOverSerial();
            } catch (err) {
              uiStatus('Auto-Improv nije uspio: ' + err.message, false);
              setBadge(improvBadge, 'IMPROV: error', 'err');
            }
          })();
        }
      }
    });
  });
}

// init
window.addEventListener('DOMContentLoaded', () => {
  loadProjects();
  attachInstallEvents();
  setBadge(portBadge, 'PORT: disconnected', 'muted');
  setBadge(improvBadge, 'IMPROV: idle', 'muted');
  setBadge(wifiBadge, 'WIFI: idle', 'muted');
  setBadge(httpBadge, 'HTTP: idle', 'muted');
  log('Spremno. 1) Flash → 2) (auto) Wi-Fi preko USB-a → 3) Spremi postavke. Logovi su stalno uključivi.');
});
