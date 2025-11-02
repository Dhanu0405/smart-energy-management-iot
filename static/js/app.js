// app.js - client side logic
let evtSource = null;
let connected = false;
const maxPoints = 200;

const portSelect = document.getElementById("portSelect");
const refreshBtn = document.getElementById("refreshBtn");
const connectBtn = document.getElementById("connectBtn");
const connStatus = document.getElementById("connStatus");
const logView = document.getElementById("logView");
const relayBtn = document.getElementById("relayBtn");
const cmdButtons = document.querySelectorAll(".cmdBtn");

const voltageEl = document.getElementById("voltage");
const currentEl = document.getElementById("current");
const powerEl = document.getElementById("power");
const predCurrentEl = document.getElementById("pred_current");
const predPowerEl = document.getElementById("pred_power");

const predNext24hWhEl = document.getElementById("pred_next24h_wh");
const predNext24hKwhEl = document.getElementById("pred_next24h_kwh");

// data buffers
const t = []; const volt = []; const curr = []; const pwr = [];
const pcurr = []; const ppwr = [];

function log(msg) {
  const now = new Date().toLocaleTimeString();
  logView.textContent += `[${now}] ${msg}\n`;
  logView.scrollTop = logView.scrollHeight;
}

async function refreshPorts() {
  const res = await fetch("/ports");
  const j = await res.json();
  portSelect.innerHTML = "";
  if (j.ports && j.ports.length) {
    j.ports.forEach(p => {
      const opt = document.createElement("option");
      opt.value = p; opt.textContent = p;
      portSelect.appendChild(opt);
    });
  } else {
    const opt = document.createElement("option");
    opt.textContent = "No ports";
    portSelect.appendChild(opt);
  }
  connStatus.textContent = j.status || "Disconnected";
}

refreshBtn.addEventListener("click", refreshPorts);

connectBtn.addEventListener("click", async () => {
  if (!connected) {
    const port = portSelect.value;
    if (!port || port === "No ports") {
      alert("Select a valid COM port");
      return;
    }
    const res = await fetch("/connect", {method:"POST", headers:{"Content-Type":"application/json"}, body: JSON.stringify({port})});
    const j = await res.json();
    if (j.ok) {
      startSSE();
      connectBtn.textContent = "Disconnect";
      connected = true;
      connStatus.textContent = "Connected: " + port;
      log("Connected to " + port);
    } else {
      alert("Connect failed: " + (j.error||""));
    }
  } else {
    await fetch("/disconnect", {method:"POST"});
    stopSSE();
    connectBtn.textContent = "Connect";
    connected = false;
    connStatus.textContent = "Disconnected";
    log("Disconnected");
  }
});

function startSSE() {
  if (evtSource) evtSource.close();
  evtSource = new EventSource("/stream");
  evtSource.onmessage = (ev) => {
    if (!ev.data) return;
    try {
      const obj = JSON.parse(ev.data);
      if (!obj.type) return;
      if (obj.type === "history") {
        const list = obj.data || [];
        list.forEach(e => handleSample(e));
        log("History loaded: " + list.length);
      } else if (obj.type === "sample") {
        handleSample(obj.data);
      }
    } catch (e) {
      // ignore heartbeat or empty
    }
  };
  evtSource.onerror = (e) => {
    log("SSE error or closed");
    evtSource.close();
    evtSource = null;
  };
}

function stopSSE() {
  if (evtSource) {
    evtSource.close();
    evtSource = null;
  }
}

// send command
async function sendCmd(cmd) {
  const res = await fetch("/cmd", {method:"POST", headers:{"Content-Type":"application/json"}, body: JSON.stringify({cmd})});
  const j = await res.json();
  if (j.ok) log("Sent: " + cmd);
  else log("Cmd failed: " + (j.error||""));
}

cmdButtons.forEach(b => b.addEventListener("click", () => sendCmd(b.dataset.cmd)));

// relay button toggles TOGGLE
relayBtn.addEventListener("click", () => sendCmd("TOGGLE"));

function handleSample(entry) {
  // entry: {"raw":..., "ts":..., "parsed": {...}}
  const parsed = entry.parsed || {};
  const ts = entry.ts || Date.now()/1000;
  const v = parseFloat(parsed.voltage ?? parsed.get?.("voltage") ?? NaN);
  const c = parseFloat(parsed.current ?? NaN);
  const pw = parseFloat(parsed.power ?? NaN);
  const pc = parseFloat(parsed.pred_current ?? parsed.predcurrent ?? NaN);
  const pp = parseFloat(parsed.pred_power ?? parsed.predpower ?? NaN);

  // push to arrays (use last known if NaN)
  const lastV = (volt.length ? volt[volt.length-1] : 0);
  const lastC = (curr.length ? curr[curr.length-1] : 0);
  const lastP = (pwr.length ? pwr[pwr.length-1] : 0);
  const lastPc = (pcurr.length ? pcurr[pcurr.length-1] : 0);
  const lastPp = (ppwr.length ? ppwr[ppwr.length-1] : 0);

  volt.push(isFinite(v)? v : lastV);
  curr.push(isFinite(c)? c : lastC);
  pwr.push(isFinite(pw)? pw : lastP);
  pcurr.push(isFinite(pc)? pc : lastPc);
  ppwr.push(isFinite(pp)? pp : lastPp);
  t.push(new Date().toLocaleTimeString());

  while (t.length > maxPoints) { t.shift(); volt.shift(); curr.shift(); pwr.shift(); pcurr.shift(); ppwr.shift(); }

  // update numeric displays
  voltageEl.textContent = (volt.length? volt[volt.length-1].toFixed(4) : "--");
  currentEl.textContent = (curr.length? curr[curr.length-1].toFixed(6) : "--");
  powerEl.textContent = (pwr.length? pwr[pwr.length-1].toFixed(6) : "--");
  predCurrentEl.textContent = (pcurr.length? pcurr[pcurr.length-1].toFixed(6) : "--");
  predPowerEl.textContent = (ppwr.length? ppwr[ppwr.length-1].toFixed(6) : "--");

  // update relay state if present
  const relay = parsed.relay ?? parsed["relay"];
  if (relay) {
    updateRelayButton(String(relay).toUpperCase());
  }

  // update prediction display if present (from PREDICT command response)
  if (parsed.pred_next24h_wh !== undefined || parsed.pred_next24hwh !== undefined) {
    const next24hWh = parseFloat(parsed.pred_next24h_wh ?? parsed.pred_next24hwh);
    const next24hKwh = parseFloat(parsed.pred_next24h_kwh ?? parsed.pred_next24hkwh);
    
    if (isFinite(next24hWh)) predNext24hWhEl.textContent = next24hWh.toFixed(6);
    if (isFinite(next24hKwh)) predNext24hKwhEl.textContent = next24hKwh.toFixed(9);
  }

  // update charts
  updateCharts();
  // update log
  log(entry.raw || JSON.stringify(parsed));
}

function updateRelayButton(state) {
  relayBtn.textContent = "Relay: " + state;
  if (state === "ON") {
    relayBtn.classList.remove("off"); relayBtn.classList.add("on");
  } else if (state === "OFF") {
    relayBtn.classList.remove("on"); relayBtn.classList.add("off");
  } else {
    relayBtn.classList.remove("on"); relayBtn.classList.remove("off");
  }
}

// Setup charts
const ctxA = document.getElementById("chartActual").getContext("2d");
const chartActual = new Chart(ctxA, {
  type: 'line',
  data: {
    labels: t,
    datasets: [
      { label: 'Actual Current (A)', data: curr, borderWidth: 2, borderColor: '#2563eb', backgroundColor: 'rgba(37,99,235,0.08)', fill: false, tension: 0.2, pointRadius:0 },
      { label: 'Predicted Current (A)', data: pcurr, borderWidth: 2, borderColor: '#f97316', backgroundColor: 'rgba(249,115,22,0.08)', fill: false, tension: 0.2, pointRadius:0 }
    ]
  },
  options: {
    animation:false,
    responsive:true,
    maintainAspectRatio:false,
    plugins: { legend: { position:'top' } },
    interaction: { intersect:false, mode:'index' },
    scales: {
      x: { display:true, title:{display:false} },
      y: { type:'linear', position:'left', title:{display:true, text:'Current (A)'} }
    }
  }
});

const ctxB = document.getElementById("chartPred").getContext("2d");
const chartPred = new Chart(ctxB, {
  type:'line',
  data: {
    labels: t,
    datasets: [
      { label: 'Actual Power (W)', data: pwr, borderWidth:2, borderColor:'#16a34a', backgroundColor:'rgba(22,163,74,0.08)', fill:false, tension:0.2, pointRadius:0 },
      { label: 'Predicted Power (W)', data: ppwr, borderWidth:2, borderColor:'#a855f7', backgroundColor:'rgba(168,85,247,0.08)', fill:false, tension:0.2, pointRadius:0 }
    ]
  },
  options: {
    animation:false,
    responsive:true,
    maintainAspectRatio:false,
    plugins: { legend: { position:'top' } },
    interaction:{intersect:false, mode:'index'},
    scales: {
      x:{display:true},
      y:{type:'linear', position:'left', title:{display:true, text:'Power (W)'}}
    }
  }
});

const ctxC = document.getElementById("chartVolt").getContext("2d");
const chartVolt = new Chart(ctxC, {
  type:'line',
  data: { labels: t, datasets: [{ label: 'Voltage (V)', data: volt, borderWidth: 2, borderColor:'#0ea5e9', backgroundColor:'rgba(14,165,233,0.08)', fill:false, tension:0.2, pointRadius:0 }] },
  options: { animation:false, responsive:true, maintainAspectRatio:false, plugins:{ legend:{ position:'top' } }, scales:{ x:{}, y:{ title:{display:true, text:'Voltage (V)'} } } }
});

function updateCharts(){
  chartActual.update('none');
  chartPred.update('none');
  chartVolt.update('none');
}

// initial refresh
refreshPorts();