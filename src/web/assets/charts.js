const canvas = document.querySelector("#chart");
const statusText = document.querySelector("#chart-status");
const context = canvas.getContext("2d");
const { width, height } = canvas;
async function refresh() {
  try {
    const type = canvas.dataset.chart;
    const response = await fetch("/data/" + type + location.search, {cache: "no-store"});
    if (!response.ok) throw Error("Could not load readings");
    const rows = await response.json();
    context.fillStyle = "#121534";
    context.fillRect(0, 0, width, height);
    if (!rows.length) { statusText.textContent = "Waiting for receptions."; return; }
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
      const [receiver, utc, center, rate] = rows[0];
      const newest = JSON.parse(rows[0][4]);
      context.strokeStyle="#ff8800"; context.beginPath();
      const count = newest.length;
      newest.forEach((db,i) => {let x=i*width/(count-1),y=-db*195/140;i?context.lineTo(x,y):context.moveTo(x,y);});
      context.stroke();
      rows.filter(r=>r[2]===center && r[3]===rate).forEach((row,j)=>{
        JSON.parse(row[4]).forEach((db,i)=>{
          const v=Math.max(0,Math.min(1,(db+115)/90));
          context.fillStyle='hsl('+ (240-v*215) +' 85% '+ (10+v*55) +'%)';
          context.fillRect(i*width/count,205+j*2.6,Math.ceil(width/count),3);
        });
      });
      statusText.textContent=receiver+" · "+(Number(center)-Number(rate)/2)+" to "+(Number(center)+Number(rate)/2)+" Hz · "+new Date(Number(utc)*1000).toISOString();
    }
  } catch (err) {statusText.textContent=err.message;}
  finally {setTimeout(refresh, canvas.dataset.chart === "noise" ? 30000 : 2000);}
}
refresh();

document.querySelector("#saveSpectrum")?.addEventListener("click", () => {
  const link = document.createElement("a");
  link.download = "skimmaster-spectrum.png";
  link.href = canvas.toDataURL("image/png");
  link.click();
});
