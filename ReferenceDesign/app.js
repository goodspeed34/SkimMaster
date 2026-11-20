/*
 * Skim Master control panel - navigation and static instrumentation.
 * Classic script: no modules, no network access, no persistent storage.
 */
(function () {
    'use strict';

    var DEFAULT_PAGE = 'system-summary';
    var sections = [];
    var navLinks = [];
    var helpBody = null;
    var resizeTimer = null;

    function byId(id) {
        return document.getElementById(id);
    }

    function closestClass(node, className) {
        while (node && node.nodeType === 1) {
            if (node.className && (' ' + node.className + ' ').indexOf(' ' + className + ' ') !== -1) {
                return node;
            }
            node = node.parentNode;
        }
        return null;
    }

    function pageSlugFromHash() {
        var raw = String(window.location.hash || '').replace(/^#/, '');
        var slug = raw;
        try {
            slug = decodeURIComponent(raw);
        } catch (err) {
            slug = raw;
        }
        if (!/^[a-z0-9-]+$/.test(slug)) {
            return DEFAULT_PAGE;
        }
        return byId('page-' + slug) ? slug : DEFAULT_PAGE;
    }

    function pageTitle(section) {
        var title = section ? section.querySelector('.page-title') : null;
        return title ? title.textContent.replace(/\s+/g, ' ').replace(/^\s+|\s+$/g, '') : 'Control Panel';
    }

    function showPage(slug) {
        var activeSection = null;

        sections.forEach(function (section) {
            var on = section.id === 'page-' + slug;
            if (on) {
                section.classList.add('active');
                activeSection = section;
            } else {
                section.classList.remove('active');
            }
        });

        updateNav(slug);
        updateHelp(activeSection);
        document.title = pageTitle(activeSection) + ' - Skim Master';
        window.scrollTo(0, 0);
        drawForPage(slug);
    }

    function updateNav(slug) {
        navLinks.forEach(function (link) {
            var item = link.parentNode;
            var active = link.getAttribute('data-page') === slug;
            if (active) {
                item.classList.add('active');
                link.setAttribute('aria-current', 'page');
                var group = item.parentNode;
                while (group && group.nodeType === 1 && !group.classList.contains('nav-group')) {
                    group = group.parentNode;
                }
                if (group && group.classList.contains('collapsed')) {
                    setCollapsed(group, false);
                }
            } else {
                item.classList.remove('active');
                link.removeAttribute('aria-current');
            }
        });
    }

    function updateHelp(section) {
        if (!helpBody) {
            return;
        }
        var help = section ? section.querySelector('.page-help') : null;
        helpBody.innerHTML = help ? help.innerHTML : '<p>No help text is available for this page.</p>';
    }

    function setCollapsed(group, collapsed) {
        if (!group) {
            return;
        }
        if (collapsed) {
            group.classList.add('collapsed');
        } else {
            group.classList.remove('collapsed');
        }
        var toggle = group.querySelector('.nav-toggle');
        if (toggle) {
            toggle.textContent = collapsed ? '+' : '-';
        }
        var title = group.querySelector('.nav-group-title');
        if (title) {
            title.setAttribute('aria-expanded', collapsed ? 'false' : 'true');
        }
    }

    function initRouting() {
        if ('scrollRestoration' in window.history) {
            try {
                window.history.scrollRestoration = 'manual';
            } catch (err) {
                /* ignore */
            }
        }

        navLinks.forEach(function (link) {
            link.addEventListener('click', function (ev) {
                var href = link.getAttribute('href') || '';
                if (href.charAt(0) !== '#') {
                    return;
                }
                ev.preventDefault();
                var slug = link.getAttribute('data-page');
                if (!slug) {
                    return;
                }
                if (window.location.hash === '#' + slug) {
                    showPage(slug);
                } else {
                    window.location.hash = slug;
                }
            });
        });

        window.addEventListener('hashchange', function () {
            showPage(pageSlugFromHash());
        });

        showPage(pageSlugFromHash());
    }

    function initAccordion() {
        var groups = document.querySelectorAll('.nav-group');
        Array.prototype.forEach.call(groups, function (group) {
            var title = group.querySelector('.nav-group-title');
            if (!title) {
                return;
            }
            title.setAttribute('role', 'button');
            title.setAttribute('tabindex', '0');
            title.setAttribute('aria-expanded', group.classList.contains('collapsed') ? 'false' : 'true');

            function toggle() {
                setCollapsed(group, !group.classList.contains('collapsed'));
            }

            title.addEventListener('click', toggle);
            title.addEventListener('keydown', function (ev) {
                if (ev.key === 'Enter' || ev.key === ' ' || ev.keyCode === 13 || ev.keyCode === 32) {
                    ev.preventDefault();
                    toggle();
                }
            });
        });
    }

    function initTabs() {
        var bars = document.querySelectorAll('.subtabs');
        Array.prototype.forEach.call(bars, function (bar) {
            bar.addEventListener('click', function (ev) {
                var tab = ev.target;
                while (tab && tab !== bar && !(tab.classList && tab.classList.contains('subtab'))) {
                    tab = tab.parentNode;
                }
                if (!tab || tab === bar) {
                    return;
                }
                ev.preventDefault();
                var key = tab.getAttribute('data-subtab');
                var scope = bar.parentNode;

                Array.prototype.forEach.call(bar.querySelectorAll('.subtab'), function (item) {
                    if (item === tab) {
                        item.classList.add('active');
                        item.setAttribute('aria-selected', 'true');
                    } else {
                        item.classList.remove('active');
                        item.setAttribute('aria-selected', 'false');
                    }
                });

                Array.prototype.forEach.call(scope.querySelectorAll('.subpane'), function (pane) {
                    if (pane.getAttribute('data-subtab-pane') === key) {
                        pane.classList.add('active');
                    } else {
                        pane.classList.remove('active');
                    }
                });

                var page = document.querySelector('.page.active');
                if (page) {
                    drawForPage(page.id.replace(/^page-/, ''));
                }
            });
        });
    }

    function pad2(value) {
        return value < 10 ? '0' + value : String(value);
    }

    function updateClock() {
        var clocks = document.querySelectorAll('.js-utc');
        if (!clocks.length) {
            return;
        }
        var now = new Date();
        var stamp =
            now.getUTCFullYear() + '-' +
            pad2(now.getUTCMonth() + 1) + '-' +
            pad2(now.getUTCDate()) + ' ' +
            pad2(now.getUTCHours()) + ':' +
            pad2(now.getUTCMinutes()) + ':' +
            pad2(now.getUTCSeconds());
        Array.prototype.forEach.call(clocks, function (clock) {
            clock.textContent = stamp;
        });
    }

    function initClock() {
        updateClock();
        if (byId('utcClock')) {
            window.setInterval(updateClock, 1000);
        }
    }

    function initForms() {
        document.addEventListener('submit', function (ev) {
            ev.preventDefault();
        });

        document.addEventListener('click', function (ev) {
            var button = closestClass(ev.target, 'btn-native') || ev.target;
            var message = button && button.getAttribute ? button.getAttribute('data-confirm') : null;
            if (message && !window.confirm(message)) {
                ev.preventDefault();
                ev.stopPropagation();
            }
        });

        var restartButton = byId('restartConfirmBtn');
        var restartPhrase = byId('restartPhrase');
        if (restartButton && restartPhrase) {
            restartButton.disabled = true;
            restartPhrase.addEventListener('input', function () {
                restartButton.disabled = restartPhrase.value.replace(/^\s+|\s+$/g, '').toUpperCase() !== 'RESTART';
            });
        }

        initLogFilter();
    }

    function initLogFilter() {
        var level = byId('logLevel');
        var search = byId('logSearch');
        var consoleBox = byId('systemLogConsole');
        if (!level || !consoleBox) {
            return;
        }

        function apply() {
            var wanted = level.value;
            var query = search ? search.value.toLowerCase() : '';
            var lines = consoleBox.querySelectorAll('.log-line');
            var shown = 0;
            Array.prototype.forEach.call(lines, function (line) {
                var levelOk = wanted === 'ALL' || line.getAttribute('data-level') === wanted;
                var textOk = !query || (line.textContent || '').toLowerCase().indexOf(query) !== -1;
                var visible = levelOk && textOk;
                line.style.display = visible ? 'block' : 'none';
                if (visible) {
                    shown += 1;
                }
            });
            var count = byId('logShownCount');
            if (count) {
                count.textContent = String(shown);
            }
        }

        level.addEventListener('change', apply);
        if (search) {
            search.addEventListener('input', apply);
        }
        apply();
    }

    function fitCanvas(canvas) {
        if (!canvas || canvas.offsetParent === null) {
            return null;
        }
        var width = canvas.clientWidth;
        var height = canvas.clientHeight;
        if (!width || !height) {
            return null;
        }
        var dpr = window.devicePixelRatio || 1;
        var pixelWidth = Math.round(width * dpr);
        var pixelHeight = Math.round(height * dpr);
        if (canvas.width !== pixelWidth) {
            canvas.width = pixelWidth;
        }
        if (canvas.height !== pixelHeight) {
            canvas.height = pixelHeight;
        }
        var ctx = canvas.getContext('2d');
        if (!ctx) {
            return null;
        }
        ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
        ctx.clearRect(0, 0, width, height);
        return { ctx: ctx, w: width, h: height };
    }

    function seededRandom(seed) {
        var state = seed >>> 0;
        return function () {
            state = (state * 1664525 + 1013904223) >>> 0;
            return state / 4294967296;
        };
    }

    function drawSpectrum() {
        var canvas = byId('spectrumCanvas');
        var frame = fitCanvas(canvas);
        if (!frame) {
            return;
        }
        var ctx = frame.ctx;
        var w = frame.w;
        var h = frame.h;
        var plotTop = 6;
        var plotBottom = h - 16;
        var plotHeight = plotBottom - plotTop;
        var rnd = seededRandom(0x1407485);

        ctx.fillStyle = '#08121E';
        ctx.fillRect(0, 0, w, h);

        ctx.strokeStyle = '#12304A';
        ctx.lineWidth = 1;
        var gx;
        for (gx = 0; gx <= 10; gx += 1) {
            var x = Math.round(gx * (w - 1) / 10) + 0.5;
            ctx.beginPath();
            ctx.moveTo(x, plotTop);
            ctx.lineTo(x, plotBottom);
            ctx.stroke();
        }
        for (gx = 0; gx <= 4; gx += 1) {
            var y = Math.round(plotTop + gx * plotHeight / 4) + 0.5;
            ctx.beginPath();
            ctx.moveTo(0, y);
            ctx.lineTo(w, y);
            ctx.stroke();
        }

        var peaks = [
            { at: 0.1170, height: 0.34 },
            { at: 0.3745, height: 0.52 },
            { at: 0.4305, height: 0.42 },
            { at: 0.4855, height: 0.30 },
            { at: 0.5600, height: 0.22 },
            { at: 0.6250, height: 0.38 },
            { at: 0.7100, height: 0.18 },
            { at: 0.8600, height: 0.44 }
        ];
        var baseline = plotTop + plotHeight * 0.78;

        ctx.strokeStyle = '#3BE06A';
        ctx.lineWidth = 1;
        ctx.beginPath();
        var px;
        for (px = 0; px < w; px += 2) {
            var t = px / w;
            var value = baseline + (rnd() - 0.5) * 7;
            var i;
            for (i = 0; i < peaks.length; i += 1) {
                var distance = Math.abs(t - peaks[i].at);
                if (distance < 0.015) {
                    value -= peaks[i].height * plotHeight * (1 - distance / 0.015);
                }
            }
            if (value < plotTop + 2) {
                value = plotTop + 2;
            }
            if (px === 0) {
                ctx.moveTo(px, value);
            } else {
                ctx.lineTo(px, value);
            }
        }
        ctx.stroke();

        ctx.font = '10px "Courier New", monospace';
        ctx.fillStyle = '#7E93A8';
        for (gx = 0; gx <= 10; gx += 1) {
            var label = (14000 + gx * 20) / 1000;
            var lx = gx * (w - 1) / 10;
            ctx.fillText(label.toFixed(3), Math.max(2, Math.min(w - 32, lx + 2)), h - 3);
        }

        var annotations = [
            { at: 0.1170, y: 14, text: 'CW 14023.4' },
            { at: 0.3745, y: 28, text: 'FT8 14074.9' },
            { at: 0.4305, y: 42, text: 'RTTY 14086.1' },
            { at: 0.4855, y: 56, text: 'WSPR 14097.1' }
        ];
        ctx.fillStyle = '#FF9900';
        annotations.forEach(function (annotation) {
            var ax = Math.round(annotation.at * w);
            ctx.fillRect(ax, plotTop, 1, plotBottom - plotTop);
            ctx.fillText(annotation.text, Math.max(2, Math.min(w - 70, ax + 4)), annotation.y);
        });

        ctx.fillStyle = '#556E88';
        ctx.fillText('dBm', 4, plotTop + 9);
        ctx.fillText('-40', 4, plotTop + 22);
        ctx.fillText('-120', 4, plotBottom - 3);
    }

    function heatColor(value) {
        if (value > 0.84) {
            return '#FFD27A';
        }
        if (value > 0.70) {
            return '#FF9900';
        }
        if (value > 0.56) {
            return '#C8C020';
        }
        if (value > 0.42) {
            return '#2C9B55';
        }
        if (value > 0.28) {
            return '#146040';
        }
        if (value > 0.15) {
            return '#0E3040';
        }
        return '#08121E';
    }

    function drawWaterfall() {
        var canvas = byId('waterfallCanvas');
        var frame = fitCanvas(canvas);
        if (!frame) {
            return;
        }
        var ctx = frame.ctx;
        var w = frame.w;
        var h = frame.h;
        var rnd = seededRandom(0xC0FFEE);
        var peaks = [0.1170, 0.3745, 0.4305, 0.4855, 0.5600, 0.6250, 0.7100, 0.8600];
        var rowHeight = 2;
        var x;
        var y;

        ctx.fillStyle = '#08121E';
        ctx.fillRect(0, 0, w, h);

        for (y = 0; y < h; y += rowHeight) {
            for (x = 0; x < w; x += 2) {
                var t = x / w;
                var value = rnd() * 0.17;
                var i;
                for (i = 0; i < peaks.length; i += 1) {
                    var distance = Math.abs(t - peaks[i]);
                    if (distance < 0.020) {
                        value += (1 - distance / 0.020) * (0.55 + rnd() * 0.40);
                    }
                }
                ctx.fillStyle = heatColor(value);
                ctx.fillRect(x, y, 2, rowHeight);
            }
        }

        ctx.strokeStyle = '#12304A';
        ctx.lineWidth = 1;
        var gx;
        for (gx = 0; gx <= 10; gx += 1) {
            var gxp = Math.round(gx * (w - 1) / 10) + 0.5;
            ctx.beginPath();
            ctx.moveTo(gxp, 0);
            ctx.lineTo(gxp, h);
            ctx.stroke();
        }

        ctx.fillStyle = 'rgba(8, 18, 30, 0.72)';
        ctx.fillRect(0, h - 14, w, 14);
        ctx.font = '10px "Courier New", monospace';
        ctx.fillStyle = '#8FA3B8';
        ctx.fillText('14.000', 4, h - 4);
        ctx.fillText('14.100', w / 2 - 18, h - 4);
        ctx.fillText('14.200', w - 42, h - 4);
    }

    function drawNoise() {
        var canvas = byId('noiseCanvas');
        var frame = fitCanvas(canvas);
        if (!frame) {
            return;
        }
        var ctx = frame.ctx;
        var w = frame.w;
        var h = frame.h;
        var left = 40;
        var right = 12;
        var top = 12;
        var bottom = h - 22;
        var plotW = w - left - right;
        var plotH = bottom - top;
        var yMin = -140;
        var yMax = -108;
        var airspy = [-134.2, -134.0, -133.8, -134.5, -134.1, -133.6, -133.2, -132.8, -133.0, -133.4, -133.9, -134.3, -134.6, -134.4, -134.0, -133.5, -133.1, -132.9, -133.3, -133.8, -134.2, -134.5, -134.1, -133.7];
        var audio = [-131.4, -131.1, -131.5, -131.8, -131.2, -130.8, -130.4, -130.1, -130.6, -131.0, -131.5, -131.9, -132.2, -132.0, -131.6, -131.2, -130.9, -130.7, -131.1, -131.4, -131.8, -132.0, -131.7, -131.3];
        var i;

        ctx.fillStyle = '#FFFFFF';
        ctx.fillRect(0, 0, w, h);

        function xAt(index) {
            return left + index * plotW / 23;
        }
        function yAt(value) {
            return top + (yMax - value) * plotH / (yMax - yMin);
        }

        ctx.font = '10px "Courier New", monospace';
        ctx.strokeStyle = '#D2DAE6';
        ctx.lineWidth = 1;
        var gridValue;
        for (gridValue = yMin; gridValue <= yMax; gridValue += 5) {
            var gy = Math.round(yAt(gridValue)) + 0.5;
            ctx.beginPath();
            ctx.moveTo(left, gy);
            ctx.lineTo(w - right, gy);
            ctx.stroke();
            ctx.fillStyle = '#555E68';
            ctx.fillText(String(gridValue), 3, gy + 3);
        }
        var hour;
        for (hour = 0; hour < 24; hour += 3) {
            var gx = Math.round(xAt(hour)) + 0.5;
            ctx.beginPath();
            ctx.moveTo(gx, top);
            ctx.lineTo(gx, bottom);
            ctx.stroke();
            ctx.fillStyle = '#555E68';
            ctx.fillText(pad2(hour), gx - 5, h - 7);
        }

        ctx.strokeStyle = '#8A95A5';
        ctx.beginPath();
        ctx.moveTo(left, top);
        ctx.lineTo(left, bottom);
        ctx.lineTo(w - right, bottom);
        ctx.stroke();

        function plotSeries(values, color) {
            ctx.strokeStyle = color;
            ctx.fillStyle = color;
            ctx.lineWidth = 1;
            ctx.beginPath();
            for (i = 0; i < values.length; i += 1) {
                var sx = xAt(i);
                var sy = yAt(values[i]);
                if (i === 0) {
                    ctx.moveTo(sx, sy);
                } else {
                    ctx.lineTo(sx, sy);
                }
            }
            ctx.stroke();
            for (i = 0; i < values.length; i += 3) {
                ctx.fillRect(Math.round(xAt(i)) - 2, Math.round(yAt(values[i])) - 2, 4, 4);
            }
        }

        plotSeries(airspy, '#262362');
        plotSeries(audio, '#FF8800');

        ctx.fillStyle = '#262362';
        ctx.fillRect(w - 190, 4, 7, 7);
        ctx.fillStyle = '#333333';
        ctx.font = '10px Arial, sans-serif';
        ctx.fillText('Airspy HF+ (14.050 MHz)', w - 179, 11);
        ctx.fillStyle = '#FF8800';
        ctx.fillRect(w - 190, 16, 7, 7);
        ctx.fillStyle = '#333333';
        ctx.fillText('RX-40m-ZeroIF (7.050 MHz)', w - 179, 23);

        ctx.fillStyle = '#555E68';
        ctx.font = '10px "Courier New", monospace';
        ctx.fillText('dBm', 3, 10);
        ctx.fillText('UTC hour', w / 2 - 20, h - 7);
    }

    function drawForPage(slug) {
        if (slug === 'spectrum-waterfall') {
            drawSpectrum();
            drawWaterfall();
        } else if (slug === 'noise-floor') {
            drawNoise();
        }
    }

    function initResize() {
        window.addEventListener('resize', function () {
            if (resizeTimer) {
                window.clearTimeout(resizeTimer);
            }
            resizeTimer = window.setTimeout(function () {
                var page = document.querySelector('.page.active');
                if (page) {
                    drawForPage(page.id.replace(/^page-/, ''));
                }
            }, 100);
        });
    }

    function initChartButtons() {
        var spectrumButton = byId('btnSpectrumRefresh');
        if (spectrumButton) {
            spectrumButton.addEventListener('click', function () {
                drawSpectrum();
                drawWaterfall();
            });
        }
        var noiseButton = byId('btnNoiseRefresh');
        if (noiseButton) {
            noiseButton.addEventListener('click', drawNoise);
        }
    }

    function init() {
        sections = Array.prototype.slice.call(document.querySelectorAll('.page'));
        navLinks = Array.prototype.slice.call(document.querySelectorAll('.nav-list a[data-page]'));
        helpBody = byId('helpBody');

        initAccordion();
        initTabs();
        initClock();
        initForms();
        initResize();
        initChartButtons();
        initRouting();
    }

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', init);
    } else {
        init();
    }
}());
