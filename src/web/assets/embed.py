from pathlib import Path
import re
import sys

panel, wizard, reference, reference_wizard, style, charts, chrome, wizard_script, editor, probe, receiver_script, output = (
    map(Path, sys.argv[1:])
)


def replace_inside(source, start, end, content):
    first = source.index(start) + len(start)
    last = source.index(end, first)
    return source[:first] + "\n" + content + "\n" + source[last:]


def raw(text):
    return 'R"SKIM(' + text + ')SKIM"'


reference_html = reference.read_text(encoding="utf-8")
sections = re.findall(
    r'<section class="page(?: active)?" id="page-([^"]+)">(.*?)</section>',
    reference_html,
    re.S,
)
pages = []
help_overrides = {
    "system-summary": (
        "<p><b>Station status:</b> This page shows the configured station identity, "
        "current worker state, recent spectrum timestamps, slice counts and recent stored spots.</p>"
        "<p><b>Clock:</b> The displayed UTC comes from the server clock. Live FT8, FT4 and "
        "WSPR workers check clock synchronization before timed decoding.</p>"
        "<p><b>Processes:</b> The supervisor samples CPU (percent of one core) and resident "
        "memory for itself, the panel and each worker every five seconds. Restarts count "
        "relaunches since the allocation started.</p>"
        "<p><b>Bands:</b> Amateur bands covered by the slices of running receivers.</p>"
        "<p><b>Recent spots:</b> Shows the last five stored results. Use Live Spot Stream or a "
        "mode page to view a larger filtered list.</p>"
    ),
    "spectrum-waterfall": (
        "<p><b>Trace:</b> Each active IQ source stores a 512-bin spectrum averaged over up to "
        "sixteen FFT frames each second. Select a source to view its latest trace and recent "
        "waterfall.</p>"
        "<p><b>Tuning:</b> Click the trace to add a slice at that frequency. CW and RTTY center "
        "their slice on it; FT8, FT4 and WSPR set the USB dial 1500 Hz below it. Shaded bands "
        "mark running slices. The receiver restarts to apply the change.</p>"
        "<p><b>Units:</b> Levels are relative dBFS, not calibrated dBm. The displayed span "
        "follows the receiver center frequency and sample rate.</p>"
        "<p><b>History:</b> The chart polls the latest stored readings.</p>"
    ),
    "live-dx-map": (
        "<p><b>Heard grids:</b> The map plots Maidenhead grids found in stored decoder results "
        "on a simple coordinate grid.</p>"
        "<p><b>Counts:</b> Each marker summarizes stored spots for that grid.</p>"
    ),
    "live-spot-stream": (
        "<p><b>Recent results:</b> Shows up to 200 stored decoder results, newest first. "
        "Recorded IQ playback results are included.</p>"
        "<p><b>Filters and export:</b> Filter by exact receiver and mode, or by callsign prefix. "
        "Download the matching rows as CSV.</p>"
        "<p><b>Refresh:</b> The page reloads every ten seconds. Select Pause refresh to keep "
        "the current result set on screen.</p>"
    ),
    "receiver-status": (
        "<p><b>Receiver pool:</b> Each row is a configured IQ source with its device, tuning, "
        "sample rate, gain, active slices and worker state.</p>"
        "<p><b>Last IQ:</b> This timestamp comes from the latest spectrum sample for the "
        "worker's current tuning.</p>"
        "<p><b>Manage sources:</b> Use Add / Edit Receiver to change hardware settings or "
        "structured slice allocations.</p>"
    ),
    "receiver-edit": (
        "<p><b>Device:</b> Select a discovered source or enter its driver parameter. Scan "
        "enumerates devices; Test Configuration attempts to receive IQ for up to three seconds.</p>"
        "<p><b>Tuning:</b> Receiver center and sample rate define the captured RF passband. "
        "Optional gain and PPM correction are passed to supported live drivers.</p>"
        "<p><b>Decoder slices:</b> Attach slices here, then use the structured slice editor to "
        "change or remove them. CW and RTTY can follow the receiver center or use a slice center.</p>"
        "<p><b>WAV:</b> IQ WAV sources require stereo I/Q data and the recorded sample rate. "
        "Repeat and start-time options are available for WAV playback.</p>"
    ),
    "band-schedules": (
        "<p><b>Schedule windows:</b> Windows use UTC minutes and selected weekdays. Equal start "
        "and end times cover the full selected day; overnight windows continue into the next day.</p>"
        "<p><b>Selection:</b> Manual receiver allocations take precedence. Otherwise, the "
        "lowest priority number wins among enabled active schedules, with list order breaking ties.</p>"
        "<p><b>Overrides:</b> Run Now applies the selected schedule for 30 minutes. Clear Override "
        "returns the receiver to normal schedule selection.</p>"
        "<p><b>Slices:</b> Schedule slices run with any attached receiver slices that fit the "
        "current IQ bandwidth. Retunes are applied by the receiver supervisor.</p>"
    ),
    "slice-allocation": (
        "<p><b>Active allocations:</b> This table lists slices currently present in launched "
        "receiver worker plans. Out-of-band configured slices are omitted.</p>"
        "<p><b>Manage slices:</b> Choose a receiver to open the structured receiver editor, "
        "where manual, attached and schedule-specific slices can be changed or removed.</p>"
    ),
    "cw-skimmer": (
        "<p><b>CW allocations:</b> Lists active CW slices and links to the structured receiver "
        "editor for slice changes.</p>"
        "<p><b>Live channel text:</b> Shows the recent characters of every CW channel the "
        "decoder is tracking, refreshed every two seconds unless refresh is paused.</p>"
        "<p><b>Decode a recording:</b> Upload 48 kHz stereo IQ and its RF center to decode it "
        "with the saved settings. Results are shown only, not stored or reported.</p>"
        "<p><b>Decoder settings:</b> Sets the CW search range, channel count and starting "
        "speed for every CW slice without its own settings table. Saving restarts the "
        "receivers that use CW.</p>"
        "<p><b>Recent results:</b> Shows up to 200 stored CW decoder results, newest first. "
        "Filter by receiver, pause refresh, or download the current rows as CSV.</p>"
    ),
    "rtty-skimmer": (
        "<p><b>RTTY allocations:</b> Lists active RTTY slices and links to the structured "
        "receiver editor for slice changes.</p>"
        "<p><b>Live channel text:</b> Shows the recent characters of every RTTY channel, "
        "refreshed every two seconds unless refresh is paused.</p>"
        "<p><b>Decode a recording:</b> Upload 12 kHz stereo IQ and its RF center to decode it "
        "with the saved settings. Results are shown only, not stored or reported.</p>"
        "<p><b>Decoder settings:</b> Sets shift, baud rate, tone reversal, channel count and "
        "detection threshold. Saving restarts the receivers that use RTTY.</p>"
        "<p><b>Recent results:</b> Shows up to 200 stored RTTY decoder results, newest first. "
        "Filter by receiver, pause refresh, or download the current rows as CSV.</p>"
    ),
    "digital-decoders": (
        "<p><b>Digital allocations:</b> Lists active FT8, FT4 and WSPR slices and links to the "
        "structured receiver editor for slice changes.</p>"
        "<p><b>Decode a recording:</b> Upload one complete slot of 12 kHz mono audio and its "
        "USB dial. Results are shown only, not stored or reported.</p>"
        "<p><b>Decoder settings:</b> Each installed FT8, FT4 and WSPR decoder lists the settings "
        "it declares. Saving restarts the receivers that use the mode.</p>"
        "<p><b>Recent results:</b> Shows up to 200 stored results, newest first. Filter by "
        "receiver and digital mode, pause refresh, or download the matching rows as CSV.</p>"
        "<p><b>Reporting:</b> Public reporting services are configured in Station Settings. "
        "Recorded IQ playback is excluded from reporting.</p>"
    ),
    "heard-stations": (
        "<p><b>Heard stations:</b> Groups stored decoder results by callsign and shows the "
        "number of decodes and most recent UTC time.</p>"
        "<p><b>Filters and export:</b> Filter by receiver, mode and callsign prefix. Download "
        "the current results as CSV. A decode records reception, not a two-way contact.</p>"
    ),
    "band-mode-activity": (
        "<p><b>Heatmap:</b> Cell color represents the number of stored decodes in that band and "
        "mode. It is not a signal-strength measurement. Bands follow the decoded RF frequency.</p>"
        "<p><b>Normalization:</b> Absolute counts compare raw activity; percentage views expose "
        "which mode dominates a band regardless of total volume.</p>"
        "<p><b>Hourly grid:</b> Decodes per UTC hour of day across the selected window, useful "
        "for gray-line and propagation planning.</p>"
        "<p><b>Export:</b> Downloads the band &times; mode matrix as absolute counts.</p>"
    ),
    "noise-floor": (
        "<p><b>Noise Floor:</b> Each running receiver records the median power of its unoccupied "
        "FFT bins once a minute, excluding carriers and the band edges. Values are a density in "
        "dBFS/Hz, so receivers with different sample rates compare directly; add your receiver's "
        "calibration to read dBm/Hz.</p>"
        "<p><b>Statistics:</b> Per-band rows cover the last 24 hours. The trend compares the last "
        "six hours with the six before; a rise of 1 dB or more is flagged. An optional alert "
        "threshold counts the minutes above it. Export downloads the minute values as CSV.</p>"
    ),
    "sstv-gallery": (
        "<p><b>Gallery:</b> Shows up to 100 recent SSTV PNG files stored on the daemon host. "
        "Filter by SSTV mode and UTC date, select an image to view it, or delete an individual image.</p>"
        "<p><b>Index:</b> Download a CSV index of all images matching the selected filters.</p>"
    ),
    "dx-cluster": (
        "<p><b>Local DX Cluster:</b> Enable the loopback Telnet listener and set its port. "
        "The page shows the configured state and supervised worker status.</p>"
        "<p><b>Restart:</b> Restart this local service worker without restarting other services "
        "or receiver workers. The cluster publishes eligible live receptions.</p>"
    ),
    "psk-reporter": (
        "<p><b>PSK Reporter:</b> Configure the opt-in uploader host and port. The page shows "
        "worker state and the count and time recorded after each UDP report send.</p>"
        "<p><b>WSPRnet:</b> Its endpoint, worker state and send receipts share this page. "
        "Calls and grid use the station identity unless an override is entered.</p>"
        "<p><b>Restart:</b> Restart an enabled service worker locally. Only eligible live "
        "receptions are reported; recorded IQ playback is excluded. PSK Reporter uses UDP "
        "and does not receive a remote send acknowledgment.</p>"
    ),
    "rbn": (
        "<p><b>Reverse Beacon Network:</b> Configure the opt-in RBN host, primary and backup "
        "ports, sign-in and band limits. Callsign and grid use the station identity unless "
        "overridden.</p>"
        "<p><b>Status and restart:</b> Shows supervised worker state and recorded report receipts. "
        "Restart affects this service worker. RBN uses its rx/6 API for eligible live CW and "
        "RTTY receptions.</p>"
    ),
    "general-settings": (
        "<p><b>Station Settings:</b> Edit and validate the complete station TOML file. "
        "Receiver, schedule and service changes reload while the daemon is running.</p>"
        "<p><b>Restart required:</b> Listener address, port, database path and gallery path "
        "take effect after a full server restart.</p>"
        "<p><b>Access:</b> The panel has no login. Keep the listener on loopback unless the "
        "network is otherwise protected.</p>"
    ),
    "system-log": (
        "<p><b>System log:</b> Shows up to 500 controller, worker and panel lifecycle records "
        "stored in SQLite.</p>"
        "<p><b>Filter and export:</b> Filter by component and download the visible result set "
        "as CSV.</p>"
    ),
    "backup-restore": (
        "<p><b>Snapshot:</b> Download a ZIP containing a consistent SQLite database snapshot "
        "and SSTV gallery images.</p>"
        "<p><b>Restore:</b> Upload a snapshot to validate it and restart the station into it. "
        "The current data is saved beside the database first. Offline, use "
        "SkimMasterTools --restore with the station stopped. Snapshots do not include the TOML.</p>"
        "<p><b>Diagnostics:</b> A bundle for bug reports. Email, operator name and RBN sign-in "
        "are redacted.</p>"
        "<p><b>Factory reset:</b> Requires typing RESET. A snapshot is saved first.</p>"
    ),
    "restart-daemon": (
        "<p><b>Restart all workers:</b> This action recycles receiver and reporting "
        "workers so they reload the current configuration. The web server and controller stay running.</p>"
        "<p><b>Restart station:</b> Stops every worker and the panel, then starts the station "
        "again from its TOML file. Listener address, port and data-path changes take effect. "
        "The panel is unavailable for a few seconds and may move to the new listener.</p>"
    ),
}
for slug, section in sections:
    title = re.search(r'<div class="page-title">(.*?)</div>', section, re.S)[1]
    help_text = re.search(r'<div class="page-help">(.*?)</div>', section, re.S)[1].strip()
    help_text = help_overrides.get(slug, help_text)
    pages.append((slug, title, help_text))

# Keep the design's actual shell. only the operational content and status
# values are supplied by the service.
probe_controls = probe.read_text(encoding="utf-8")
editor_content = editor.read_text(encoding="utf-8").replace(
    "<!-- receiver probe -->", probe_controls)
shell = replace_inside(
    reference_html, '<main class="center-panel">', "</main>",
    '<section class="page active" id="page-{{ page }}">\n'
    '<div class="page-title">{{ title }}</div>\n'
    + panel.read_text(encoding="utf-8").replace(
        "<!-- receiver editor -->", editor_content) + "</section>",
)
shell = replace_inside(shell, '<div id="helpBody">', "</div>", "{{ reference_help }}")
shell = replace_inside(shell, "<title>", "</title>", "{{ title }} - Skim Master")
shell = replace_inside(
    shell, '<div class="sub-bar">', "</div>",
    '    <span>Receivers: {% if length(receivers) == 0 %}<b>None</b>{% endif %}'
    '{% for receiver in receivers %}{% if not loop.is_first %} &nbsp;|&nbsp; {% endif %}'
    '<b>{{ receiver.0 }}</b> (IF {{ receiver.1 }} MHz, BW {{ receiver.2 }} kHz){% endfor %}</span>\n'
    '    <span>Bands: <b>{{ bands }}</b> | Decoders: <b>{{ slice_count }} Channels</b>'
    ' | CPU Load: <b>{{ cpu_load }}</b></span>',
)
source_link = '<a href="{{ source_url }}">Source code</a>'
shell = shell.replace("v0.0.0</b><br>", "v{{ version }}</b> · " + source_link + "<br>")
shell = shell.replace("<b>14d 06h 22m</b>", '<b id="uptime" data-seconds="{{ uptime }}">{{ uptime_label }}</b>')
shell = shell.replace('class="mono js-utc">----</span>', 'class="mono js-utc">{{ utc }}</span>')
shell = shell.replace('href="style.css"', 'href="/style.css"')
shell = shell.replace('src="app.js"', 'src="/chrome.js"')
shell = shell.replace('href="wizard.html"', 'href="/wizard.html"')
shell = re.sub(
    r'<li(?: class="active")?><a href="#([a-z0-9-]+)" data-page="\1">',
    lambda m: ('<li{% if page == "' + m[1] + '" %} class="active"{% endif %}>'
               '<a href="/' + m[1] + '" data-page="' + m[1] + '"'
               '{% if page == "' + m[1] + '" %} aria-current="page"{% endif %}>'),
    shell,
)

wizard_shell = reference_wizard.read_text(encoding="utf-8")
wizard_shell = replace_inside(
    wizard_shell, '<div class="wiz-body">', '</div>\n\n  <!-- 底部向导',
    wizard.read_text(encoding="utf-8").replace(
        "<!-- receiver probe -->", probe_controls),
)
wizard_shell = re.sub(r' onclick="(?:changeStep\([^)]+\)|finishWizard\(\))"', "", wizard_shell)
wizard_shell = wizard_shell.replace('type="button" class="btn-native" id="btnFinish"',
                                    'type="submit" form="receiverForm" class="btn-native" id="btnFinish"')
wizard_shell = re.sub(r'<script>.*?</script>',
                      '<script src="/receivers.js" defer></script>\n'
                      '<script src="/wizard.js" defer></script>',
                      wizard_shell, flags=re.S)
wizard_shell = wizard_shell.replace(
    "Return to Control Panel</a>", "Return to Control Panel</a> · " + source_link)
wizard_shell = wizard_shell.replace("index.html#system-summary", "/system-summary")
wizard_shell = wizard_shell.replace('href="style.css"', 'href="/style.css"')

with output.open("w", encoding="utf-8") as stream:
    for var, content in (
        ("kTemplate", shell),
        ("kWizard", wizard_shell),
        ("kStyle", style.read_text(encoding="utf-8")),
        ("kCharts", charts.read_text(encoding="utf-8")),
        ("kChrome", chrome.read_text(encoding="utf-8")),
        ("kWizardScript", wizard_script.read_text(encoding="utf-8")),
        ("kReceiversScript", receiver_script.read_text(encoding="utf-8")),
    ):
        stream.write(f'constexpr char {var}[] = {raw(content)};\n')
    stream.write("struct SmReferencePage { const char* slug; const char* title; const char* help; };\n")
    stream.write("constexpr SmReferencePage kReferencePages[] = {\n")
    for page in pages:
        stream.write("  {" + ", ".join(map(raw, page)) + "},\n")
    stream.write("};\n")
