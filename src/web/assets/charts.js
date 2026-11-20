const canvas = document.querySelector("#chart");
const statusText = document.querySelector("#chart-status");
const context = canvas.getContext("2d");
const { width, height } = canvas;
const plot = {left: 58, right: width - 14, top: 14, bottom: 176, waterfall: 214};
const tuneForm = document.querySelector("#tune-form");
const saveSpectrum = document.querySelector("#saveSpectrum");
const mode = document.querySelector("#tune-mode");
const frequency = document.querySelector("#tune-frequency");
const label = document.querySelector("#tune-label");
const hint = document.querySelector("#tune-hint");
const timed = () => ["FT8", "FT4", "WSPR"].includes(mode.value);
let reading = null;
let clicked = null;

function updateTuning() {
  if (!tuneForm) return;
  label.textContent = ["CW", "RTTY"].includes(mode.value) ? "Slice center (Hz)" : "USB dial (Hz)";
  const shift = timed() ? 1500 : 0;
  const live = reading?.live && tuneForm.elements.revision.value;
  const span = (reading?.rate - (mode.value === "CW" ? 48000 : 12000)) / 2;
  const valid = live && span >= 0;
  tuneForm.querySelector("button").disabled = !valid;
  tuneForm.elements.receiver.value = valid ? reading.receiver : "";
  tuneForm.elements.sample_center.value = valid ? reading.center : "";
  tuneForm.elements.sample_rate.value = valid ? reading.rate : "";
  frequency.min = valid ? Math.max(1, Math.ceil(reading.center - span - shift)) : 1;
  frequency.max = valid ? Math.floor(reading.center + span - shift) : "";
  if (clicked !== null && valid) frequency.value = clicked - shift;
  hint.textContent = valid
      ? reading.receiver + " · Valid range " + frequency.min + "–" + frequency.max + " Hz"
      : live ? mode.value + " requires more IQ bandwidth." : "Waiting for a current IQ sample and valid station settings.";
}

function drawSpectrum(rows) {
  const [receiver, utc, center, rate, bins, live, age] = rows[0];
  if (reading && (receiver !== reading.receiver || Number(center) !== reading.center ||
                  Number(rate) !== reading.rate)) {
    frequency.value = "";
    clicked = null;
  }
  reading = {receiver, center: Number(center), rate: Number(rate), live: live === "1"};
  if (!reading.live) clicked = null;
  updateTuning();

  const span = plot.right - plot.left;
  const start = reading.center - reading.rate / 2;
  const x = hz => plot.left + span * (hz - start) / reading.rate;
  const y = db => plot.top - db * (plot.bottom - plot.top) / 160;
  context.font = "12px sans-serif";
  context.strokeStyle = "#484e68";
  context.fillStyle = "#ecedf5";
  for (let db = 0; db >= -160; db -= 40) {
    context.beginPath();
    context.moveTo(plot.left, y(db));
    context.lineTo(plot.right, y(db));
    context.stroke();
    context.textAlign = "right";
    context.fillText(db + " dBFS", plot.left - 5, y(db) + 4);
  }
  for (let tick = 0; tick <= 4; tick++) {
    const hz = start + reading.rate * tick / 4;
    context.textAlign = tick === 4 ? "right" : "left";
    context.fillText(hz + " Hz", x(hz), plot.bottom + 19);
  }

  const newest = JSON.parse(bins);
  context.strokeStyle = "#ffaf58";
  context.beginPath();
  newest.forEach((db, index) => {
    const px = plot.left + span * index / Math.max(1, newest.length - 1);
    if (index) context.lineTo(px, y(db));
    else context.moveTo(px, y(db));
  });
  context.stroke();

  const history = rows.filter(row => row[2] === center && row[3] === rate).slice(0, 80);
  const rowHeight = (height - plot.waterfall - 8) / 80;
  context.textAlign = "right";
  history.forEach((row, index) => {
    const levels = JSON.parse(row[4]);
    const py = plot.waterfall + index * rowHeight;
    for (const [bin, db] of levels.entries()) {
      const level = Math.max(0, Math.min(1, (db + 160) / 160));
      context.fillStyle = "hsl(" + (240 - level * 215) + " 85% " + (10 + level * 55) + "%)";
      context.fillRect(plot.left + bin * span / levels.length, py, Math.ceil(span / levels.length), rowHeight);
    }
    if (index % 20 === 0) {
      context.fillStyle = "#ecedf5";
      context.fillText(new Date(Number(row[1]) * 1000).toISOString().slice(11, 19), plot.left - 5, py + 10);
    }
  });
  const peak = Math.max(...newest);
  const state = reading.live ? "Current capture" : "Historical sample";
  statusText.textContent = state + " · " + receiver + " · " + start + "–" + (start + reading.rate) +
      " Hz · Peak " + peak + " dBFS · " + new Date(Number(utc) * 1000).toISOString() +
      " · " + Math.max(0, Number(age)) + " s ago";
  saveSpectrum.disabled = false;
}

async function refresh() {
  try {
    const type = canvas.dataset.chart;
    const query = type === "spectrum" ? "?" + new URLSearchParams({receiver: canvas.dataset.source}) : location.search;
    const response = await fetch("/data/" + type + query, {cache: "no-store"});
    if (!response.ok) throw Error("Could not load readings");
    const rows = await response.json();
    context.fillStyle = "#121534";
    context.fillRect(0, 0, width, height);
    if (!rows.length) {
      statusText.textContent = type === "spectrum" ? "No IQ spectrum samples for this source." : "Waiting for receptions.";
      reading = null;
      clicked = null;
      updateTuning();
      if (saveSpectrum) saveSpectrum.disabled = true;
      return;
    }
    if (type === "noise") {
      const now = Date.now() / 1000, start = now - 86400;
      const values = rows.map(r => Number(r[2]));
      const low = Math.floor(Math.min(...values) - 3), high = Math.ceil(Math.max(...values) + 3);
      const x = utc => width * (utc - start) / 86400;
      const y = db => height - 20 - (height - 30) * (db - low) / (high - low);
      context.strokeStyle = "#484e68"; context.fillStyle = "#bdc1d0";
      for (let hour = 0; hour <= 24; hour += 3) {
        const px = width * hour / 24;
        context.beginPath(); context.moveTo(px, 0); context.lineTo(px, height - 20); context.stroke();
        context.fillText("-" + (24 - hour) + "h", Math.min(px + 2, width - 24), height - 6);
      }
      for (let db = Math.ceil(low / 5) * 5; db <= high; db += 5) {
        context.beginPath(); context.moveTo(0, y(db)); context.lineTo(width, y(db)); context.stroke();
        context.fillText(db, 2, y(db) - 2);
      }
      const threshold = Number(new URLSearchParams(location.search).get("threshold"));
      if (threshold) {
        context.strokeStyle = "#e57373"; context.setLineDash([6, 4]);
        context.beginPath(); context.moveTo(0, y(threshold)); context.lineTo(width, y(threshold)); context.stroke();
        context.setLineDash([]);
      }
      const colors = ["#ff8800", "#7fb3ff", "#7bd389", "#e57373", "#f0e68c", "#d39bff"];
      const series = [...new Set(rows.map(r => r[0]))];
      const legend = [];
      series.forEach((name, n) => {
        const points = rows.filter(r => r[0] === name);
        context.strokeStyle = colors[n % colors.length]; context.lineWidth = 2; context.beginPath();
        points.forEach((r, i) => i ? context.lineTo(x(Number(r[1])), y(Number(r[2]))) : context.moveTo(x(Number(r[1])), y(Number(r[2]))));
        context.stroke(); context.lineWidth = 1;
        legend.push(name + " (" + colors[n % colors.length] + "): " + points[points.length - 1][2] + " dBFS/Hz");
      });
      statusText.textContent = legend.join(" · ");
    } else if (type === "map") {
      context.strokeStyle = "#484e68";
      context.fillStyle = "#bdc1d0";
      for (let lon = -180; lon <= 180; lon += 30) {
        let x = width * (lon + 180) / 360;
        context.beginPath(); context.moveTo(x, 0); context.lineTo(x, height); context.stroke();
        context.fillText(lon + "°", x + 2, height - 5);
      }
      for (let lat = -60; lat <= 60; lat += 30) {
        let y = height * (90 - lat) / 180;
        context.beginPath(); context.moveTo(0, y); context.lineTo(width, y); context.stroke();
        context.fillText(lat + "°", 2, y - 2);
      }
      for (const [raw, count] of rows) {
        const g = raw.toUpperCase();
        if (!/^[A-R]{2}[0-9]{2}([A-X]{2})?$/.test(g)) continue;
        let lon = (g.charCodeAt(0)-65)*20 + 2*Number(g[2]);
        let lat = (g.charCodeAt(1)-65)*10 + Number(g[3]);
        lon += g.length===6 ? (g.charCodeAt(4)-64.5)/12 : 1;
        lat += g.length===6 ? (g.charCodeAt(5)-64.5)/24 : .5;
        context.fillStyle = "#ff8800"; context.beginPath();
        context.arc(width*lon/360, height*(1-lat/180), Math.min(8,2+Math.log1p(count)), 0, 2*Math.PI); context.fill();
      }
      statusText.textContent = rows.length + " heard grids";
    } else {
      drawSpectrum(rows);
      try {
        if (reading.live) {
          const response = await fetch("/data/slices?receiver=" + encodeURIComponent(reading.receiver), {cache: "no-store"});
          if (!response.ok) throw new Error("Slice overlays unavailable");
          const marks = await response.json();
          const span = plot.right - plot.left;
          const start = reading.center - reading.rate / 2;
          context.save();
          context.beginPath();
          context.rect(plot.left, plot.top, span, plot.bottom - plot.top);
          context.clip();
          for (const [name, hz, bandwidth] of marks) {
            const left = plot.left + span * (Number(hz) - Number(bandwidth) / 2 - start) / reading.rate;
            const size = span * Number(bandwidth) / reading.rate;
            context.fillStyle = "rgba(127,179,255,0.18)";
            context.fillRect(left, plot.top, size, plot.bottom - plot.top);
            context.fillStyle = "#bdc1d0";
            context.textAlign = "left";
            context.fillText(name, Math.max(plot.left, left) + 2, plot.top + 14);
          }
          context.restore();
        }
      } catch {}
    }
  } catch (err) {
    statusText.textContent = "Chart unavailable. " + err.message;
    reading = null;
    clicked = null;
    updateTuning();
    if (saveSpectrum) saveSpectrum.disabled = true;
  }
  finally {setTimeout(refresh, canvas.dataset.chart === "noise" ? 30000 : 2000);}
}
refresh();

saveSpectrum?.addEventListener("click", () => {
  const output = document.createElement("canvas");
  output.width = width;
  output.height = height + 46;
  const image = output.getContext("2d");
  image.fillStyle = "#121534";
  image.fillRect(0, 0, width, output.height);
  image.fillStyle = "#ecedf5";
  image.font = "13px sans-serif";
  image.fillText(statusText.textContent, 12, 20, width - 24);
  image.fillText("Waterfall starts with newest sample · timestamps in UTC · color scales from −160 to 0 dBFS", 12, 38, width - 24);
  image.drawImage(canvas, 0, 46);
  const link = document.createElement("a");
  link.download = "skimmaster-spectrum.png";
  link.href = output.toDataURL("image/png");
  link.click();
});

if (tuneForm) {
  canvas.addEventListener("click", event => {
    if (!reading?.live) return;
    const box = canvas.getBoundingClientRect();
    const x = width * (event.clientX - box.left) / box.width;
    const y = height * (event.clientY - box.top) / box.height;
    if (x < plot.left || x > plot.right || y < plot.top || y > plot.bottom) return;
    const fraction = (x - plot.left) / (plot.right - plot.left);
    clicked = Math.round(reading.center - reading.rate / 2 + fraction * reading.rate);
    updateTuning();
    frequency.focus();
  });
  mode.addEventListener("change", updateTuning);
  frequency.addEventListener("input", () => { clicked = null; });
}
