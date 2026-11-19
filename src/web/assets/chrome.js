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

    const clock = document.getElementById("utcClock");
    const uptime = document.getElementById("uptime");
    const start = Date.now() - Number(uptime.dataset.seconds) * 1000;
    function tick() {
        clock.textContent = new Date().toISOString().slice(0, 19).replace("T", " ");
        const elapsed = Math.floor((Date.now() - start) / 60000);
        uptime.textContent = Math.floor(elapsed / 1440) + "d " +
            String(Math.floor(elapsed / 60) % 24).padStart(2, "0") + "h " +
            String(elapsed % 60).padStart(2, "0") + "m";
    }
    tick();
    window.setInterval(tick, 1000);
})();
