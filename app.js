const select = document.getElementById('project');
const installBtn = document.getElementById('installBtn');
const saveBtn = document.getElementById('saveBtn');

async function loadProjects() {
  const res = await fetch('./projects.json');
  const projects = await res.json();

  for (const p of projects) {
    const opt = document.createElement('option');
    opt.value = p.manifest;
    opt.textContent = p.name;
    select.appendChild(opt);
  }

  if (projects[0]) {
    installBtn.setAttribute('manifest', projects[0].manifest);
  }
}

select.addEventListener('change', () => {
  installBtn.setAttribute('manifest', select.value);
});

function uiStatus(msg, ok = false) {
  const el = document.getElementById('status');
  el.textContent = msg;
  el.className = ok ? 'ok' : 'err';
}

async function resolveDeviceUrlManual() {
  let url = document.getElementById('deviceUrl').value.trim();
  if (!/^https?:\/\//.test(url)) url = 'http://' + url;
  return url;
}

saveBtn.addEventListener('click', async () => {
  try {
    const url = await resolveDeviceUrlManual();
    const body = {
      ssid: document.getElementById('ssid').value || '',
      password: document.getElementById('password').value || '',
      username: document.getElementById('username').value || '',
      city: document.getElementById('city').value || '',
      timeZone: Number(document.getElementById('timeZone').value || 0),
      latitude: Number(document.getElementById('latitude').value || 0),
      longitude: Number(document.getElementById('longitude').value || 0),
      metricUnits: !!document.getElementById('metricUnits').checked,
    };

    const res = await fetch(new URL('/provision', url), {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    });

    if (!res.ok) throw new Error('HTTP ' + res.status);
    const json = await res.json();
    uiStatus(`Postavke spremljene ✅ (${json.username || body.username} @ ${json.city || body.city}).`, true);
  } catch (e) {
    console.error(e);
    uiStatus('Upis postavki nije uspio. Jesi li spojen na Inkplate-Setup-XXXX i je li URL http://192.168.4.1?', false);
  }
});

loadProjects();
