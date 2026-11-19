for (const probe of document.querySelectorAll("[data-receiver-probe]")) {
    const select = probe.querySelector("[data-probe-driver]");
    const target = probe.querySelector("[data-probe-target]");
    const scan = probe.querySelector("[data-probe-scan]");
    const results = probe.querySelector("[data-probe-results]");
    const list = probe.querySelector("[data-probe-list]");
    const status = probe.querySelector("[data-probe-status]");
    const form = probe.closest("form");
    const {device, rate, csrf, mode} = form.elements;
    const placeholders = {
        soapy: "Optional driver=… filter",
        rtltcp: "127.0.0.1:1234",
        alsa: "Optional PCM name filter",
        wav: "File or directory on server"
    };
    let devices = [];

    function resetResults() {
        target.placeholder = placeholders[select.value];
        results.hidden = true;
        status.textContent = "";
    }
    function toggleRow(row, visible) {
        if (!row) return;
        row.hidden = !visible;
        for (const input of row.querySelectorAll("input"))
            input.disabled = !visible;
    }
    function showOptions() {
        const wav = device.value.startsWith("wav:");
        const alsa = device.value.startsWith("alsa:");
        for (const row of form.querySelectorAll("[data-live-option]"))
            toggleRow(row, !wav && !alsa);
        toggleRow(form.querySelector("[data-wav-option]"), wav);
        toggleRow(form.querySelector("[data-slice-center]"), mode && ["CW", "RTTY"].includes(mode.value));
        toggleRow(form.querySelector("[data-dial-option]"),
                  mode && ["FT8", "FT4", "WSPR", "SSTV"].includes(mode.value));
    }
    select.addEventListener("change", () => {
        target.value = "";
        resetResults();
    });
    device.addEventListener("input", showOptions);
    if (mode) mode.addEventListener("change", showOptions);
    const test = form.querySelector("[data-capture-test]");
    if (test) {
        const feedback = form.querySelector("[data-capture-status]");
        test.addEventListener("click", async () => {
            test.disabled = true;
            feedback.textContent = "Waiting for IQ samples…";
            try {
                const response = await fetch("/receiver-test", {
                    method: "POST",
                    body: new URLSearchParams(new FormData(form))
                });
                feedback.textContent = await response.text();
            } catch (error) {
                feedback.textContent = error.message;
            } finally {
                test.disabled = false;
            }
        });
    }
    scan.addEventListener("click", async () => {
        resetResults();
        scan.disabled = select.disabled = target.disabled = true;
        status.textContent = "Scanning…";
        try {
            const response = await fetch("/receiver-probe", {
                method: "POST",
                body: new URLSearchParams({driver: select.value, target: target.value, csrf: csrf.value})
            });
            if (!response.ok) throw new Error(await response.text());
            devices = await response.json();
            list.replaceChildren(new Option("Select a device…", ""));
            devices.forEach(({label, device}, i) => {
                list.add(new Option(label === device ? label : label + " · " + device, i));
            });
            results.hidden = devices.length === 0;
            status.textContent = devices.length ? "" : "No devices found. You can enter a device parameter manually.";
        } catch (error) {
            status.textContent = error.message;
        } finally {
            scan.disabled = select.disabled = target.disabled = false;
        }
    });
    list.addEventListener("change", () => {
        if (list.value === "") return;
        const selected = devices[Number(list.value)];
        device.value = selected.device;
        if (selected.rate) rate.value = selected.rate;
        showOptions();
    });
    if (select.value === "rtltcp" || select.value === "wav")
        target.value = device.value;
    resetResults();
    showOptions();
}
