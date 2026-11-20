(() => {
    const links = [...document.querySelectorAll(".nav-list a[data-page]")];
    function followHash() {
        const link = links.find(item => "#" + item.dataset.page === location.hash);
        if (link && link.pathname !== location.pathname) location.replace(link.href);
    }
    window.addEventListener("hashchange", followHash);
    followHash();

    for (const group of document.querySelectorAll(".nav-group")) {
        const title = group.querySelector(".nav-group-title");
        const indicator = title.querySelector(".nav-toggle");
        const key = "collapsed:" + title.firstElementChild.textContent;
        title.setAttribute("role", "button");
        title.setAttribute("tabindex", "0");
        function setCollapsed(collapsed) {
            group.classList.toggle("collapsed", collapsed);
            indicator.textContent = collapsed ? "+" : "-";
            title.setAttribute("aria-expanded", String(!collapsed));
        }
        try {
            setCollapsed(sessionStorage.getItem(key) === "true" &&
                         !group.querySelector('[aria-current="page"]'));
        } catch {
            setCollapsed(false);
        }
        function toggle() {
            const collapsed = !group.classList.contains("collapsed");
            setCollapsed(collapsed);
            try { sessionStorage.setItem(key, String(collapsed)); } catch {}
        }
        title.addEventListener("click", toggle);
        title.addEventListener("keydown", event => {
            if (event.key === "Enter" || event.key === " ") {
                event.preventDefault();
                toggle();
            }
        });
    }

    for (const [selector, width] of [["[data-panel-nav]", 680], ["[data-panel-help]", 980]]) {
        const details = document.querySelector(selector);
        const breakpoint = matchMedia("(max-width: " + width + "px)");
        const sync = () => { details.open = !breakpoint.matches; };
        breakpoint.addEventListener("change", sync);
        sync();
    }

    const clock = document.getElementById("utcClock");
    const uptime = document.getElementById("uptime");
    const start = performance.now();
    const serverTime = Date.parse(clock.textContent.trim().replace(" ", "T") + "Z");
    function tick() {
        const running = performance.now() - start;
        clock.textContent = new Date(serverTime + running).toISOString().slice(0, 19).replace("T", " ");
        const elapsed = Math.floor((Number(uptime.dataset.seconds) * 1000 + running) / 60000);
        uptime.textContent = Math.floor(elapsed / 1440) + "d " +
            String(Math.floor(elapsed / 60) % 24).padStart(2, "0") + "h " +
            String(elapsed % 60).padStart(2, "0") + "m";
    }
    tick();
    window.setInterval(tick, 1000);

    const pause = document.querySelector("[data-pause-refresh]");
    const state = document.querySelector("[data-refresh-state]");
    const regions = [...document.querySelectorAll("[data-live-region]")];
    const pageUrl = new URL(pause?.form.action || location.href, location.href);
    if (pause) {
        // Use the applied filters, including on pages rendered after a POST.
        const call = new URLSearchParams(location.search).get("call");
        pageUrl.search = new URLSearchParams({receiver: pause.dataset.receiver, mode: pause.dataset.mode});
        if (call) pageUrl.searchParams.set("call", call);
    }
    let refreshGeneration = 0;
    let refreshing = false;
    function setRefreshState(message) {
        if (state && state.textContent !== message) state.textContent = message;
    }
    async function refreshResults() {
        if (pause?.checked || refreshing || document.hidden) return;
        refreshing = true;
        const generation = refreshGeneration;
        try {
            const response = await fetch(pageUrl, {cache: "no-store"});
            if (!response.ok) throw new Error("Refresh failed");
            const snapshot = new DOMParser().parseFromString(await response.text(), "text/html");
            if (generation !== refreshGeneration) return;
            for (const region of regions) {
                const fresh = snapshot.getElementById(region.id);
                if (fresh && !region.contains(document.activeElement))
                    region.replaceChildren(...fresh.children);
            }
            setRefreshState("Refresh active");
        } catch {
            if (generation === refreshGeneration) setRefreshState("Refresh unavailable. Retrying…");
        } finally {
            refreshing = false;
        }
    }
    if (regions.length) {
        if (pause) {
            setRefreshState(pause.checked ? "Refresh paused" : "Refresh active");
            pause.addEventListener("change", () => {
                refreshGeneration++;
                setRefreshState(pause.checked ? "Refresh paused" : "Refresh active");
                if (!pause.checked) refreshResults();
            });
        }
        setInterval(refreshResults, 10000);
    }

    // CW and RTTY channel text has a faster cadence than the result tables.
    const liveText = document.getElementById("live-text");
    if (liveText) {
        const body = liveText.tBodies[0];
        const columns = liveText.tHead.rows[1].cells.length;
        const query = new URLSearchParams({mode: liveText.dataset.mode,
                                           receiver: liveText.dataset.receiver});
        let updating = false;
        async function refreshLiveText() {
            if (pause.checked || updating || document.hidden) return;
            updating = true;
            const generation = refreshGeneration;
            try {
                const response = await fetch("/data/live-text?" + query, {cache: "no-store"});
                if (!response.ok) return;
                const rows = await response.json();
                if (generation !== refreshGeneration) return;
                const lines = rows.map(row => {
                    const line = document.createElement("tr");
                    for (const value of row) {
                        const cell = document.createElement("td");
                        cell.className = "mono";
                        cell.textContent = value;
                        line.append(cell);
                    }
                    return line;
                });
                if (!lines.length) {
                    const line = document.createElement("tr");
                    const cell = document.createElement("td");
                    cell.colSpan = columns;
                    cell.textContent = "No channels are decoding text.";
                    line.append(cell);
                    lines.push(line);
                }
                body.replaceChildren(...lines);
            } catch {
                // Keep the last readable text during intermittent failures.
            } finally {
                updating = false;
            }
        }
        pause.addEventListener("change", refreshLiveText);
        setInterval(refreshLiveText, 2000);
    }
})();
