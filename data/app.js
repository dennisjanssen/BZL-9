function minutesToTime(m) {
  const h = Math.floor(m / 60).toString().padStart(2, "0");
  const mm = (m % 60).toString().padStart(2, "0");
  return h + ":" + mm;
}
function timeToMinutes(t) {
  const [h, m] = t.split(":").map(Number);
  return h * 60 + m;
}

async function pollStatus() {
  try {
    const res = await fetch("/api/status");
    if (!res.ok) throw new Error("bad status");
    const s = await res.json();
    document.getElementById("conn-badge").textContent = "online";
    document.getElementById("conn-badge").style.color = "";
    document.getElementById("state-value").textContent = s.state;
    highlightMood(s.state, s.moodOverride);
    document.getElementById("time-unknown").hidden = !s.timeUnknown;

    const uptime = s.uptimeSeconds;
    const h = Math.floor(uptime / 3600);
    const m = Math.floor((uptime % 3600) / 60);
    document.getElementById("f-uptime").textContent = h + "h " + m + "m";

    let wifiText = s.wifi.connected ? "connected (" + s.wifi.rssi + " dBm)" : "disconnected";
    if (s.wifi.apActive) wifiText += " · AP active";
    document.getElementById("f-wifi").textContent = wifiText;

    let wxText = "not yet fetched";
    if (s.weather) {
      wxText = Math.round(s.weather.temperatureC * 10) / 10 + "°C, code " + s.weather.weatherCode;
      if (s.weather.rainExpectedToday) {
        wxText += " · rain later (" + s.weather.rainChancePercent + "%)";
      }
      if (s.weather.stale) wxText += " (stale)";
    }
    document.getElementById("f-weather").textContent = wxText;
  } catch (e) {
    document.getElementById("conn-badge").textContent = "offline";
  }
}

const DAY_MOODS = ["NEUTRAL", "BORED", "EXCITED", "FOCUSED", "SLEEPY"];

// Highlights whichever button matches what the device is actually doing:
// the held mood when overridden, otherwise Auto. Schedule states
// (SLEEPY/SLEEPING) and overlays aren't moods, so nothing is marked then.
function highlightMood(state, override) {
  const active = override && DAY_MOODS.includes(state) ? state.toLowerCase()
               : (!override && DAY_MOODS.includes(state) ? "auto" : null);
  document.querySelectorAll("#mood-grid button").forEach((b) => {
    b.classList.toggle("active", b.dataset.mood === active);
  });
}

document.getElementById("mood-grid").addEventListener("click", (e) => {
  const btn = e.target.closest("button[data-mood]");
  if (!btn) return;
  postJson("/api/mood", { mood: btn.dataset.mood }).then(pollStatus);
});

function rgbToHex(r, g, b) {
  const h = (n) => Math.max(0, Math.min(255, Number(n) || 0)).toString(16).padStart(2, "0");
  return "#" + h(r) + h(g) + h(b);
}
// The device takes three 0-255 channels rather than a hex string, so the
// colour input is split here instead of parsed on the MCU.
function hexToRgb(hex) {
  const m = /^#?([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(String(hex).trim());
  if (!m) return null;
  return [parseInt(m[1], 16), parseInt(m[2], 16), parseInt(m[3], 16)];
}

async function loadConfig() {
  try {
    const res = await fetch("/api/config");
    const c = await res.json();
    document.getElementById("c-workday-start").value = minutesToTime(c.workdayStartMinutes);
    document.getElementById("c-workday-end").value = minutesToTime(c.workdayEndMinutes);
    document.getElementById("c-timezone").value = c.timezone;
    document.getElementById("c-hydration").value = c.hydrationIntervalMinutes;
    document.getElementById("c-movement").value = c.movementIntervalMinutes;
    document.getElementById("c-sleepy-lead").value = c.sleepyLeadMinutes;
    document.getElementById("c-lat").value = c.latitude;
    document.getElementById("c-lon").value = c.longitude;
    document.getElementById("c-wx-interval").value = c.weatherPollIntervalMinutes;
    document.getElementById("c-active-br").value = c.activeBrightnessPercent;
    document.getElementById("c-sleep-br").value = c.sleepBrightnessPercent;
    document.getElementById("c-flip").checked = !!c.displayFlipped;
    document.getElementById("c-led-on").checked = !!c.ledEnabled;
    document.getElementById("c-led-color").value = rgbToHex(c.ledR, c.ledG, c.ledB);
    document.getElementById("c-wifi-ssid").value = c.wifiSsid;
  } catch (e) {
    // Dashboard still works without a loaded config; fields just stay blank.
  }
}

async function postJson(url, body) {
  return fetch(url, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
}

document.getElementById("express-grid").addEventListener("click", (e) => {
  const btn = e.target.closest("button[data-expr]");
  if (!btn) return;
  postJson("/api/express", { expression: btn.dataset.expr });
});

document.getElementById("btn-reboot").addEventListener("click", () => {
  if (confirm("Reboot the device?")) postJson("/api/reboot", {});
});

document.getElementById("config-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  const msg = document.getElementById("config-msg");
  const payload = {
    workdayStartMinutes: timeToMinutes(document.getElementById("c-workday-start").value),
    workdayEndMinutes: timeToMinutes(document.getElementById("c-workday-end").value),
    timezone: document.getElementById("c-timezone").value,
    hydrationIntervalMinutes: Number(document.getElementById("c-hydration").value),
    movementIntervalMinutes: Number(document.getElementById("c-movement").value),
    sleepyLeadMinutes: Number(document.getElementById("c-sleepy-lead").value),
    latitude: Number(document.getElementById("c-lat").value),
    longitude: Number(document.getElementById("c-lon").value),
    weatherPollIntervalMinutes: Number(document.getElementById("c-wx-interval").value),
    activeBrightnessPercent: Number(document.getElementById("c-active-br").value),
    sleepBrightnessPercent: Number(document.getElementById("c-sleep-br").value),
    displayFlipped: document.getElementById("c-flip").checked,
    ledEnabled: document.getElementById("c-led-on").checked,
    wifiSsid: document.getElementById("c-wifi-ssid").value,
  };
  const rgb = hexToRgb(document.getElementById("c-led-color").value);
  if (rgb) {
    payload.ledR = rgb[0];
    payload.ledG = rgb[1];
    payload.ledB = rgb[2];
  }

  const pass = document.getElementById("c-wifi-pass").value;
  if (pass.length > 0) payload.wifiPassword = pass;

  try {
    const res = await postJson("/api/config", payload);
    const data = await res.json();
    if (res.ok) {
      msg.textContent = "Saved.";
      msg.className = "msg ok";
      document.getElementById("c-wifi-pass").value = "";
    } else {
      msg.textContent = data.error || "Save failed.";
      msg.className = "msg error";
    }
  } catch (e) {
    msg.textContent = "Save failed: device unreachable.";
    msg.className = "msg error";
  }
  msg.hidden = false;
});

pollStatus();
setInterval(pollStatus, 1000);
loadConfig();
