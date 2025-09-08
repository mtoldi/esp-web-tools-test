const select = document.getElementById('project');
const installBtn = document.getElementById('installBtn');

async function loadProjects() {
  const res = await fetch('./projects.json');
  const projects = await res.json();

  // Napuni <select>
  for (const p of projects) {
    const opt = document.createElement('option');
    opt.value = p.manifest;
    opt.textContent = p.name;
    select.appendChild(opt);
  }

  // Postavi početni manifest
  if (projects[0]) {
    installBtn.setAttribute('manifest', projects[0].manifest);
  }
}

select.addEventListener('change', () => {
  installBtn.setAttribute('manifest', select.value);
});

loadProjects();
