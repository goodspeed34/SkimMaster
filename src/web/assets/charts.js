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
    if (type === "map") {
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
      newest.forEach((db,i) => {let x=i*width/255,y=-db*195/140;i?context.lineTo(x,y):context.moveTo(x,y);});
      context.stroke();
      rows.filter(r=>r[2]===center && r[3]===rate).forEach((row,j)=>{
        JSON.parse(row[4]).forEach((db,i)=>{
          const v=Math.max(0,Math.min(1,(db+115)/90));
          context.fillStyle='hsl('+ (240-v*215) +' 85% '+ (10+v*55) +'%)';
          context.fillRect(i*width/256,205+j*2.6,Math.ceil(width/256),3);
        });
      });
      statusText.textContent=receiver+" · "+(Number(center)-Number(rate)/2)+" to "+(Number(center)+Number(rate)/2)+" Hz · "+new Date(Number(utc)*1000).toISOString();
    }
  } catch (err) {statusText.textContent=err.message;}
  finally {setTimeout(refresh, 2000);}
}
refresh();

document.querySelector("#saveSpectrum")?.addEventListener("click", () => {
  const link = document.createElement("a");
  link.download = "skimmaster-spectrum.png";
  link.href = canvas.toDataURL("image/png");
  link.click();
});
