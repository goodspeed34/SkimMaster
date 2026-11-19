(() => {
    const panes = Array.from(document.querySelectorAll(".step-pane"));
    const steps = Array.from(document.querySelectorAll(".step-item"));
    const form = document.getElementById("receiverForm");
    const back = document.getElementById("btnBack");
    const next = document.getElementById("btnNext");
    const finish = document.getElementById("btnFinish");
    let current = 0;

    function validateTuning() {
        const mode = form.elements.mode.value;
        const rate = Number(form.elements.rate.value);
        const skimmer = mode === "CW" || mode === "RTTY";
        const dialField = form.elements.dial_hz;
        document.getElementById("dialRow").hidden = skimmer;
        dialField.disabled = skimmer;
        dialField.required = !skimmer;
        const dial = Number(dialField.value);
        const output = mode === "CW" ? 48000 : 12000;
        const shift = ["FT8", "FT4", "WSPR"].includes(mode) ? 1500 : 0;
        const center = form.elements.center_hz.valueAsNumber || dial + shift;
        const margin = Math.floor((rate - output) / 2);
        const minDial = Math.max(1, center - margin - shift);
        const maxDial = center + margin - shift;
        const hint = document.getElementById("tuningHint");
        const error = document.getElementById("tuningError");
        hint.textContent = skimmer ? "The " + mode + " skimmer searches the " +
            output + " Hz slice around the IQ source’s RF center." : rate >= output
            ? "At this RF center and sample rate, " + mode + " dial frequencies can range from " +
              minDial + " to " + maxDial + " Hz."
            : "";
        let message = "";
        if (skimmer && !form.elements.center_hz.valueAsNumber)
            message = "Go back to IQ Hardware Source and enter the receiver RF center.";
        else if (rate < output || (!skimmer && (dial < minDial || dial > maxDial)))
            hint.textContent = "This slice will wait until the IQ bandwidth covers it.";
        error.textContent = message;
        error.hidden = !message;
        return !message;
    }

    function show(step) {
        current = step;
        if (step === 2) validateTuning();
        panes.forEach((pane, index) => pane.classList.toggle("active", index === step));
        steps.forEach((item, index) => item.classList.toggle("active", index === step));
        back.disabled = step === 0;
        next.style.display = step === panes.length - 1 ? "none" : "";
        finish.style.display = step === panes.length - 1 ? "" : "none";
        if (step === panes.length - 1) {
            document.getElementById("reviewStation").textContent =
                form.elements.callsign.value.toUpperCase() + " · " + form.elements.grid.value;
            document.getElementById("reviewName").textContent = form.elements.name.value;
            document.getElementById("reviewEmail").textContent = form.elements.email.value;
            document.getElementById("reviewReceiver").textContent = form.elements.receiver.value;
            document.getElementById("reviewDevice").textContent = form.elements.device.value;
            document.getElementById("reviewRate").textContent = form.elements.rate.value;
            const mode = form.elements.mode.value;
            const skimmer = mode === "CW" || mode === "RTTY";
            document.getElementById("reviewDecoderLabel").textContent =
                skimmer ? "Decoder and Slice Center (Hz)" : "Decoder and Dial (Hz)";
            document.getElementById("reviewDecoder").textContent = mode + " · " +
                (skimmer ? form.elements.center_hz.value : form.elements.dial_hz.value);
        }
    }

    function advance() {
        for (const entry of panes[current].querySelectorAll("input, select"))
            if (!entry.reportValidity()) return;
        if (current === 2 && !validateTuning()) return;
        show(current + 1);
    }

    back.addEventListener("click", () => show(current - 1));
    next.addEventListener("click", advance);
    form.addEventListener("keydown", event => {
        if (event.key === "Enter" && current < panes.length - 1 &&
            event.target.tagName === "INPUT") {
            event.preventDefault();
            advance();
        }
    });
    form.elements.mode.addEventListener("change", validateTuning);
    form.elements.dial_hz.addEventListener("input", validateTuning);
    show(Number(form.dataset.initialStep));
})();
