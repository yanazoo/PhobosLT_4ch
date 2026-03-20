// PhobosLT 4ch - Web UI Script
'use strict';

const NUM_PILOTS = 4;

const freqLookup = [
  [5865,5845,5825,5805,5785,5765,5745,5725], // A
  [5733,5752,5771,5790,5809,5828,5847,5866], // B
  [5705,5685,5665,5645,5885,5905,5925,5945], // E
  [5740,5760,5780,5800,5820,5840,5860,5880], // F
  [5658,5695,5732,5769,5806,5843,5880,5917], // R
  [5362,5399,5436,5473,5510,5547,5584,5621], // L
];
const bandNames = ['A','B','E','F','R','L'];

const PILOT_COLORS      = ['#58a6ff','#f85149','#3fb950','#d29922'];
const PILOT_FILL_COLORS = ['rgba(88,166,255,0.25)','rgba(248,81,73,0.25)',
                            'rgba(63,185,80,0.25)','rgba(210,153,34,0.25)'];

// ── State ──────────────────────────────────────────────────────────────────
var pilotConfigs  = [];
var rssiValues    = [0, 0, 0, 0];
var lapNos        = [-1,-1,-1,-1];
var lapTimesArr   = [[],[],[],[]];
var timerRAF      = null;   // requestAnimationFrame handle for race timer
var timerInterval = null;  // setInterval backup for timer (resilience against rAF throttling)
var raceStartTime = null;  // Date.now() at race start (kept for compatibility)
var raceStartPerf = null;  // performance.now() at race start (monotonic, sub-ms precision)
var audioEnabled  = true;   // default ON
var announcerRate = 1.0;
var totalLaps     = 0;
var currentTab    = 'race';
var speechQueue   = [];
var isSpeaking    = false;
var minLapSec     = 10;   // global minimum lap time (seconds), updated from UI
var lastBatUpdateMs = 0; // throttle battery display to 1/sec (suppress SSE DOM thrash)

// RSSI charts — series created immediately, charts created lazily when calib tab opens
var rssiSeries  = [new TimeSeries(), new TimeSeries(), new TimeSeries(), new TimeSeries()];
var rssiCharts  = [null, null, null, null];
var chartsReady = false;

// ── Tab management ─────────────────────────────────────────────────────────
function switchTab(name) {
  currentTab = name;

  document.querySelectorAll('.tab-pane').forEach(function(s) {
    s.style.display = 'none';
  });
  var pane = el(name);
  if (pane) pane.style.display = 'block';

  document.querySelectorAll('.tab-btn').forEach(function(b) {
    b.classList.toggle('active', b.dataset.tab === name);
  });

  if (name === 'calib') {
    if (!chartsReady) {
      setTimeout(function() {
        chartsReady = true;
        for (var i = 0; i < NUM_PILOTS; i++) {
          createRssiChart(i);
          updateChartLines(i);
        }
      }, 150);
    } else {
      rssiCharts.forEach(function(c) { if (c) c.start(); });
    }
  } else {
    rssiCharts.forEach(function(c) { if (c) c.stop(); });
  }
}

// ── Helpers ────────────────────────────────────────────────────────────────
function el(id) { return document.getElementById(id); }

function showToast(msg, ms) {
  var t = el('toast');
  t.textContent = msg;
  t.classList.add('show');
  setTimeout(function() { t.classList.remove('show'); }, ms || 2200);
}

function sleep(ms) { return new Promise(function(r) { setTimeout(r, ms); }); }

function syncSN(sId, nId, dec) {
  var s = el(sId), n = el(nId);
  if (s && n) n.value = parseFloat(s.value).toFixed(dec);
}

function syncNS(nId, sId, dec, maxV) {
  var n = el(nId), s = el(sId);
  if (!n || !s) return;
  var v = parseFloat(n.value);
  if (isNaN(v)) return;
  v = Math.max(parseFloat(s.min), Math.min(parseFloat(maxV || s.max), v));
  s.value = v;
  n.value = v.toFixed(dec);
}

// Update global minLapSec from current UI value — called on every slider/input change
function updateMinLapVar() {
  var n = el('globalMinLapN');
  var v = n ? parseFloat(n.value) : NaN;
  if (!isNaN(v) && v >= 0) minLapSec = v;
}

// ── Frequency helpers ──────────────────────────────────────────────────────
function updateFreq(pilot) {
  var b = el('bandSelect'+pilot).selectedIndex;
  var c = el('chanSelect'+pilot).selectedIndex;
  var freq = freqLookup[b][c];
  el('freqDisplay'+pilot).textContent = bandNames[b] + (c+1) + ' ' + freq + 'MHz';
  return freq;
}

function setBandChan(pilot, freq) {
  for (var b = 0; b < freqLookup.length; b++) {
    for (var c = 0; c < freqLookup[b].length; c++) {
      if (freqLookup[b][c] === freq) {
        el('bandSelect'+pilot).selectedIndex = b;
        el('chanSelect'+pilot).selectedIndex = c;
        updateFreq(pilot);
        return;
      }
    }
  }
  updateFreq(pilot);
}

function updateRaceCard(i) {
  var cfg  = pilotConfigs[i] || {};
  var name = cfg.name || ('Pilot '+(i+1));
  var freq = cfg.freq || 0;
  var card = el('raceCard'+i);
  if (!card) return;
  card.querySelector('.pilot-name').textContent        = name;
  card.querySelector('.pilot-freq-badge').textContent  = freq > 0 ? freq+'MHz' : 'OFF';
  var cn = el('calibName'+i);
  if (cn) cn.textContent = name;
}

// ── Init on page load ──────────────────────────────────────────────────────
window.addEventListener('load', function() {
  fetch('/config')
    .then(function(r) { return r.json(); })
    .then(function(cfg) {
      pilotConfigs = cfg.pilots || [];
      while (pilotConfigs.length < NUM_PILOTS) {
        pilotConfigs.push({freq:5658, minLap:100, enterRssi:120, exitRssi:100, name:''});
      }

      // Global minLap: read from pilot 0 (minLap stored as ×10 tenths)
      var rawMl = pilotConfigs[0] ? pilotConfigs[0].minLap : 0;
      var gml   = (rawMl > 0) ? rawMl / 10 : 10;
      el('globalMinLap').value  = gml;
      el('globalMinLapN').value = gml.toFixed(1);
      minLapSec = gml;  // sync global variable

      for (var i = 0; i < NUM_PILOTS; i++) {
        var p = pilotConfigs[i];

        el('pname'+i).value = p.name || '';
        setBandChan(i, p.freq || 5658);

        el('enterRssi'+i).value  = p.enterRssi || 120;
        el('enterRssiN'+i).value = p.enterRssi || 120;
        el('exitRssi'+i).value   = p.exitRssi  || 100;
        el('exitRssiN'+i).value  = p.exitRssi  || 100;

        updateRaceCard(i);
        var cn = el('calibName'+i);
        if (cn) cn.textContent = p.name || ('Pilot '+(i+1));
      }

      var alarm = (cfg.alarm || 36) / 10;
      el('alarmThreshold').value = alarm;
      el('alarmN').value         = alarm.toFixed(1);
      el('announcerSelect').selectedIndex = cfg.anType || 0;
      var rate = (cfg.anRate || 10) / 10;
      el('rate').value  = rate;
      el('rateN').value = rate.toFixed(1);
      announcerRate     = rate;
    })
    .catch(function(e) { console.error('Config load error:', e); });
});

// ── Calibration slider sync ────────────────────────────────────────────────
function syncCalibSlider(pilot, type) {
  var isEnter = (type === 'enter');
  var sId = (isEnter ? 'enterRssi' : 'exitRssi') + pilot;
  var nId = (isEnter ? 'enterRssiN': 'exitRssiN')  + pilot;
  var val = parseInt(el(sId).value);
  el(nId).value = val;

  if (isEnter) {
    var exitS = el('exitRssi'+pilot), exitN = el('exitRssiN'+pilot);
    if (parseInt(exitS.value) >= val) {
      exitS.value = Math.max(50, val - 5);
      exitN.value = exitS.value;
    }
  } else {
    var entS = el('enterRssi'+pilot), entN = el('enterRssiN'+pilot);
    if (parseInt(entS.value) <= val) {
      entS.value = Math.min(255, val + 5);
      entN.value = entS.value;
    }
  }
  updateChartLines(pilot);
}

function syncCalibNum(pilot, type) {
  var isEnter = (type === 'enter');
  var sId = (isEnter ? 'enterRssi' : 'exitRssi') + pilot;
  var nId = (isEnter ? 'enterRssiN': 'exitRssiN')  + pilot;
  var val = parseInt(el(nId).value);
  if (isNaN(val)) return;
  val = Math.max(50, Math.min(255, val));
  el(sId).value = val;
  el(nId).value = val;
  syncCalibSlider(pilot, type);
}

function updateChartLines(pilot) {
  if (!rssiCharts[pilot]) return;
  var enter = parseInt(el('enterRssi'+pilot).value);
  var exit_ = parseInt(el('exitRssi'+pilot).value);
  rssiCharts[pilot].options.horizontalLines = [
    { color: PILOT_COLORS[pilot], lineWidth: 2,   value: enter },
    { color: '#d29922',            lineWidth: 1.5, value: exit_ },
  ];
  rssiCharts[pilot].options.maxValue = Math.max(enter + 30, rssiValues[pilot] + 20, 150);
}

// ── RSSI Charts (lazy init when calib tab opened) ──────────────────────────
function createRssiChart(pilot) {
  var canvas = el('rssiChart' + pilot);
  if (!canvas) { console.error('[Chart] canvas not found: pilot', pilot); return; }

  var w = canvas.offsetWidth;
  var h = canvas.offsetHeight;
  if (w === 0 || h === 0) {
    var parent = canvas.parentElement;
    w = parent ? parent.clientWidth  : 300;
    h = parent ? parent.clientHeight : 130;
    if (h < 50) h = 130;
  }

  canvas.width  = w;
  canvas.height = h;

  var chart = new SmoothieChart({
    responsive:      true,
    millisPerPixel:  50,
    grid: {
      strokeStyle:      'rgba(255,255,255,0.12)',
      sharpLines:       true,
      verticalSections: 4,
      borderVisible:    false,
      fillStyle:        '#0d1117',
    },
    labels:   { fillStyle: '#8b949e', precision: 0, fontSize: 10 },
    maxValue: 220,
    minValue: 0,
    horizontalLines: [],
  });

  chart.addTimeSeries(rssiSeries[pilot], {
    lineWidth:   2.5,
    strokeStyle: PILOT_COLORS[pilot],
    fillStyle:   PILOT_FILL_COLORS[pilot],
  });

  var now = Date.now();
  rssiSeries[pilot].append(now - 12000, rssiValues[pilot]);
  rssiSeries[pilot].append(now,          rssiValues[pilot]);

  chart.streamTo(canvas, 250);
  rssiCharts[pilot] = chart;
}

// ── RSSI display update (200ms) ────────────────────────────────────────────
var rssiDisplayed = [-1,-1,-1,-1];  // last written value per pilot — skip DOM write if unchanged
setInterval(function() {
  var now = Date.now();
  for (var i = 0; i < NUM_PILOTS; i++) {
    var v = rssiValues[i];

    // Only write RSSI DOM elements when value has changed (avoids unnecessary layout)
    if (v !== rssiDisplayed[i]) {
      rssiDisplayed[i] = v;
      var bar = el('rssiBar'+i), num = el('rssiNum'+i);
      if (bar) bar.style.width = Math.min(100, v / 255 * 100) + '%';
      if (num) num.textContent = v;

      var cv = el('calibRssi'+i);
      if (cv) cv.textContent = v;
    }

    rssiSeries[i].append(now, v);
    // updateChartLines only needed when calib tab is open and chart exists
    if (currentTab === 'calib') updateChartLines(i);
  }
}, 200);

// ── Battery voltage display ────────────────────────────────────────────────
function updateBatteryDisplay(voltStr) {
  var bv1 = el('bvolt');     if (bv1) bv1.textContent = voltStr;
  var bv2 = el('bvoltRace'); if (bv2) bv2.textContent = voltStr;

  // Low-voltage visual alert: turn red + blink when at or below alarm threshold
  var alarmEl = el('alarmN');
  var alarmV  = alarmEl ? parseFloat(alarmEl.value) : 0;
  var dispV   = parseFloat(voltStr);
  var isLow   = (alarmV > 0 && !isNaN(dispV) && dispV <= alarmV);
  ['bvolt','bvoltRace','bvoltIcon','bvoltRaceIcon'].forEach(function(id) {
    var el2 = el(id);
    if (!el2) return;
    if (isLow) el2.classList.add('bat-low');
    else       el2.classList.remove('bat-low');
  });
}

// Poll /status every 5 seconds for battery voltage (works with all firmware versions)
setInterval(function() {
  fetch('/status')
    .then(function(r) { return r.text(); })
    .then(function(t) {
      var m = t.match(/Battery Voltage:\s*([\d.]+)v/i);
      if (m) updateBatteryDisplay(m[1] + 'v');
    })
    .catch(function() {});
}, 5000);

// ── Save functions ─────────────────────────────────────────────────────────
function savePilotConfig(pilot) {
  var b    = el('bandSelect'+pilot).selectedIndex;
  var c    = el('chanSelect'+pilot).selectedIndex;
  var freq = freqLookup[b][c];
  var name = el('pname'+pilot).value;
  // minLap is global — read from global setting
  var ml   = Math.round(parseFloat(el('globalMinLapN').value) * 10);

  var cfg   = pilotConfigs[pilot] || {};
  var enter = cfg.enterRssi || 120;
  var exit_ = cfg.exitRssi  || 100;

  var data  = { freq: freq, minLap: ml, enterRssi: enter, exitRssi: exit_, name: name };
  pilotConfigs[pilot] = Object.assign(cfg, data);
  updateRaceCard(pilot);

  fetch('/config/pilot?p='+pilot, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(data),
  })
  .then(function(r) { return r.json(); })
  .then(function() { showToast('Pilot '+(pilot+1)+' を保存しました ✓'); })
  .catch(function() { showToast('保存に失敗しました ✗'); });
}

function saveCalibConfig(pilot) {
  var enter = parseInt(el('enterRssi'+pilot).value);
  var exit_ = parseInt(el('exitRssi'+pilot).value);
  var cfg   = pilotConfigs[pilot] || {};

  var data = {
    freq:      cfg.freq   || 5658,
    minLap:    cfg.minLap || 100,
    name:      cfg.name   || ('Pilot '+(pilot+1)),
    enterRssi: enter,
    exitRssi:  exit_,
  };

  pilotConfigs[pilot] = Object.assign(cfg, { enterRssi: enter, exitRssi: exit_ });
  updateChartLines(pilot);

  fetch('/config/pilot?p='+pilot, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(data),
  })
  .then(function(r) { return r.json(); })
  .then(function() { showToast('Pilot '+(pilot+1)+' キャリブを保存 ✓'); })
  .catch(function() { showToast('保存に失敗しました ✗'); });
}

function saveGlobalConfig() {
  var alarm  = Math.round(parseFloat(el('alarmN').value) * 10);
  var anType = el('announcerSelect').selectedIndex;
  var anRate = Math.round(parseFloat(el('rateN').value) * 10);
  var ml     = Math.round(parseFloat(el('globalMinLapN').value) * 10);
  announcerRate = parseFloat(el('rateN').value);

  // Save global settings (alarm, announcer)
  fetch('/config/global', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ alarm: alarm, anType: anType, anRate: anRate }),
  }).catch(function() {});

  // Apply minLap to all pilots
  var saved = 0;
  for (var i = 0; i < NUM_PILOTS; i++) {
    var cfg   = pilotConfigs[i] || {};
    var data  = {
      freq:      cfg.freq      || 5658,
      minLap:    ml,
      enterRssi: cfg.enterRssi || 120,
      exitRssi:  cfg.exitRssi  || 100,
      name:      cfg.name      || ('Pilot '+(i+1)),
    };
    pilotConfigs[i] = Object.assign(cfg, { minLap: ml });
    (function(idx) {
      fetch('/config/pilot?p='+idx, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(data),
      })
      .then(function(r) { return r.json(); })
      .then(function() {
        saved++;
        if (saved === NUM_PILOTS) showToast('グローバル設定を保存 ✓');
      })
      .catch(function() { showToast('保存に失敗しました ✗'); });
    })(i);
  }
}

// ── Race management ────────────────────────────────────────────────────────

function startTimer() {
  // Use performance.now() — monotonic clock, sub-ms precision, not affected by Date clamping.
  // raceStartTime (Date.now()) is kept only for external compatibility.
  if (!raceStartTime) raceStartTime = Date.now();
  raceStartPerf = performance.now();

  var timerEl  = el('timer');
  var lastText = '';

  function updateTimerDisplay() {
    if (raceStartPerf === null) return;
    var elapsed = performance.now() - raceStartPerf;
    var ms = elapsed | 0;                 // truncate to whole ms
    var cs = Math.floor((ms % 1000) / 10);
    var s  = Math.floor(ms / 1000) % 60;
    var m  = Math.floor(ms / 60000);
    var text = pad(m)+':'+pad(s)+':'+pad(cs);
    if (text !== lastText) {
      timerEl.textContent = text;
      lastText = text;
    }
  }

  function tick() {
    if (raceStartPerf === null) return;
    updateTimerDisplay();
    timerRAF = requestAnimationFrame(tick);
  }
  timerRAF = requestAnimationFrame(tick);

  // Backup: setInterval at 50 ms keeps display updating even if rAF is throttled by the OS.
  if (timerInterval) clearInterval(timerInterval);
  timerInterval = setInterval(updateTimerDisplay, 50);
}

function pad(n) { return n < 10 ? '0'+n : ''+n; }

async function startRace() {
  el('startRaceButton').disabled = true;
  clearAllLaps();
  totalLaps = parseInt(el('totalLaps').value) || 0;

  // Cancel any queued TTS immediately (fire-and-forget — IPC response arrives during countdown).
  if (window.speechSynthesis) window.speechSynthesis.cancel();
  speechQueue = []; isSpeaking = false;

  fetch('/timer/countdown', { method: 'POST' }).catch(function() {});

  var overlay = el('countdownOverlay');
  var numEl   = el('countdownNum');
  overlay.style.display = 'flex';

  // 3, 2, 1 — visual only, fixed 900 ms per step.
  // TTS is NOT awaited: awaiting speechSynthesis.onend causes its IPC completion
  // callback to arrive on the main thread ~1–2 s after the race starts, blocking
  // requestAnimationFrame and freezing the timer display at ~1.3 s.
  var countLabels = ['3', '2', '1'];
  for (var i = 0; i < countLabels.length; i++) {
    numEl.textContent = countLabels[i];
    numEl.style.animation = 'none';
    void numEl.offsetWidth;
    numEl.style.animation = '';
    await sleep(900);
  }

  numEl.textContent = 'GO!';
  numEl.style.animation = 'none';
  void numEl.offsetWidth;
  numEl.style.animation = '';
  beep(600, 1320);

  // Record start time BEFORE any DOM changes to avoid layout-delay skewing the timer.
  raceStartTime = Date.now();
  overlay.style.display = 'none';
  startTimer();
  el('stopRaceButton').disabled = false;
}

function stopRace() {
  speakImmediate('レース終了');
  cancelAnimationFrame(timerRAF);
  timerRAF = null;
  clearInterval(timerInterval);
  timerInterval = null;
  raceStartTime = null;
  raceStartPerf = null;
  el('timer').textContent = '00:00:00';
  fetch('/timer/stop', { method: 'POST' }).catch(function() {});
  el('stopRaceButton').disabled  = true;
  el('startRaceButton').disabled = false;
}

function clearAllLaps() {
  for (var i = 0; i < NUM_PILOTS; i++) {
    var tb = document.querySelector('#lapTable'+i+' tbody');
    if (tb) tb.innerHTML = '';
    lapNos[i]      = -1;
    lapTimesArr[i] = [];
    var badge = document.querySelector('#raceCard'+i+' .pilot-lapcount');
    if (badge) badge.textContent = '';
    var ptEl = el('pilotTotal'+i);
    if (ptEl) ptEl.textContent = '--';
  }
}

function addLap(pilotIdx, lapMs) {
  // Minimum lap time filter — skip for holeshot (first crossing), apply from lap 1 onward
  if (lapNos[pilotIdx] >= 0 && minLapSec > 0 && lapMs / 1000 < minLapSec) return;

  var lapNo  = ++lapNos[pilotIdx];
  var lapSec = lapMs / 1000;
  var lapStr = lapSec.toFixed(2);
  var laps   = lapTimesArr[pilotIdx];
  var name   = (pilotConfigs[pilotIdx] && pilotConfigs[pilotIdx].name) || ('Pilot '+(pilotIdx+1));

  laps.push(lapSec);

  // Cumulative total (kept for lap table column and announcer)
  var totalSec = laps.reduce(function(a, b) { return a + b; }, 0);
  var totalStr = totalSec.toFixed(2);

  // Best lap time: minimum of real laps (lap 1+); fall back to holeshot if none yet
  var realLaps = laps.slice(1);
  var bestSec  = realLaps.length > 0 ? Math.min.apply(null, realLaps) : null;
  var bestStr  = bestSec !== null ? bestSec.toFixed(2) : '--';

  var tb = document.querySelector('#lapTable'+pilotIdx+' tbody');
  if (!tb) return;

  var row = tb.insertRow(0);
  if (lapNo === 0) row.className = 'hs-row';

  row.insertCell(0).textContent = lapNo;

  var tc = row.insertCell(1);
  tc.className = 'lap-time';
  tc.textContent = lapNo === 0 ? 'HS: '+lapStr+'s' : lapStr+'s';

  row.insertCell(2).textContent = totalStr+'s';

  // Update pilot best-lap display card
  var ptEl = el('pilotTotal'+pilotIdx);
  if (ptEl && bestStr !== '--') ptEl.textContent = bestStr+'s';

  // Highlight the best-lap row in the table (clear previous, mark current)
  if (bestSec !== null && lapNo > 0) {
    var allRows = document.querySelectorAll('#lapTable'+pilotIdx+' tbody tr');
    allRows.forEach(function(r) { r.classList.remove('best-row'); });
    // The best row's lap time cell matches bestStr+'s'
    allRows.forEach(function(r) {
      var tc = r.querySelector('.lap-time');
      if (tc && tc.textContent === bestStr+'s') r.classList.add('best-row');
    });
  }

  // Lap count badge
  var badge = document.querySelector('#raceCard'+pilotIdx+' .pilot-lapcount');
  if (badge) badge.textContent = lapNo+'周';

  // Goal / announce
  if (totalLaps > 0 && lapNo >= totalLaps) {
    speakImmediate(name+' ゴール！ '+lapStr+'秒');
    showToast(name+' ゴール！ '+lapStr+'s', 3000);
  } else {
    announceType(lapNo, lapStr, totalStr, name, pilotIdx);
  }
}

function announceType(lapNo, lapStr, totalStr, name, pilotIdx) {
  var anVal = el('announcerSelect').value;
  var hs = (lapNo === 0);
  var lapLabel = hs ? 'ホールショット' : lapNo + '周';

  switch (anVal) {
    case 'laptime':
      // ラップタイム
      speakJa(name + ' ' + (hs ? 'ホールショット ' : '') + lapStr + '秒');
      break;

    case 'lap_laptime':
      // 周回数 & ラップタイム
      speakJa(name + ' ' + lapLabel + ' ' + lapStr + '秒');
      break;

    case 'laptime_total':
      // ラップタイム & 累計ラップ
      if (hs) speakJa(name + ' ホールショット ' + lapStr + '秒');
      else    speakJa(name + ' ' + lapStr + '秒 累計 ' + totalStr + '秒');
      break;

    case 'lap_total':
      // 周回数 & 累計ラップ
      if (hs) speakJa(name + ' ホールショット');
      else    speakJa(name + ' ' + lapLabel + ' 累計 ' + totalStr + '秒');
      break;

    case 'lap_laptime_total':
      // 周回数 & ラップタイム & 累計ラップ
      if (hs) speakJa(name + ' ホールショット ' + lapStr + '秒');
      else    speakJa(name + ' ' + lapLabel + ' ' + lapStr + '秒 累計 ' + totalStr + '秒');
      break;

    case 'beep':
      beep(100, 330 + pilotIdx * 110);
      break;

    case 'none':
    default:
      break;
  }
}

// ── Audio (Web Speech API) ─────────────────────────────────────────────────
function toggleVoice() {
  audioEnabled = !audioEnabled;
  var btn = el('voiceToggleBtn');
  if (audioEnabled) {
    btn.textContent = '🔊 音声 ON';
    btn.classList.add('on');
    if (window.speechSynthesis) {
      var u = new SpeechSynthesisUtterance('音声を有効にしました');
      u.lang = 'ja-JP'; u.volume = 0.5; u.rate = announcerRate;
      window.speechSynthesis.speak(u);
    }
  } else {
    btn.textContent = '🔇 音声 OFF';
    btn.classList.remove('on');
    if (window.speechSynthesis) window.speechSynthesis.cancel();
    speechQueue = []; isSpeaking = false;
  }
}

function testVoice() {
  if (!window.speechSynthesis) { showToast('音声API非対応ブラウザです'); return; }
  window.speechSynthesis.cancel();
  speechQueue = []; isSpeaking = false;
  for (var i = 0; i < NUM_PILOTS; i++) {
    var n = (pilotConfigs[i] && pilotConfigs[i].name) || ('パイロット'+(i+1));
    var u = new SpeechSynthesisUtterance(n+' テスト');
    u.lang = 'ja-JP'; u.rate = announcerRate;
    window.speechSynthesis.speak(u);
  }
}

function speakJa(text) {
  if (!audioEnabled || !window.speechSynthesis) return;
  speechQueue.push(text);
  processSpeech();
}

function speakImmediate(text) {
  if (!window.speechSynthesis) return;
  speechQueue = []; isSpeaking = false;
  window.speechSynthesis.cancel();
  var u = new SpeechSynthesisUtterance(text);
  u.lang = 'ja-JP'; u.rate = announcerRate;
  isSpeaking = true;
  u.onend = u.onerror = function() { isSpeaking = false; };
  window.speechSynthesis.speak(u);
}

function processSpeech() {
  if (isSpeaking || speechQueue.length === 0) return;
  var text = speechQueue.shift();
  var u = new SpeechSynthesisUtterance(text);
  u.lang = 'ja-JP'; u.rate = announcerRate; u.volume = 1;
  isSpeaking = true;
  u.onend  = function() { isSpeaking = false; processSpeech(); };
  u.onerror = function() { isSpeaking = false; processSpeech(); };
  window.speechSynthesis.speak(u);
}

// ── Beep ───────────────────────────────────────────────────────────────────
// Singleton AudioContext — avoids repeated create/close GC stalls.
//
// Keep-alive oscillator: a 1 Hz oscillator at gain=0.0001 (-80 dB, inaudible)
// keeps the audio session active on iOS/Android.  An all-zero silent buffer
// may be detected as "no output" by iOS and still trigger audio-session
// deactivation; a non-zero (though inaudible) signal prevents that.
//
// osc.onended disconnect: removes the node from the graph immediately when the
// beep finishes, allowing GC without a stop-the-world pause.
var _audioCtx  = null;
var _keepOsc   = null;   // 1 Hz near-silent oscillator — prevents audio-session deactivation
var _keepGain  = null;

function getAudioCtx() {
  try {
    if (!_audioCtx || _audioCtx.state === 'closed') {
      _audioCtx = new (window.AudioContext || window.webkitAudioContext)();

      // Keep-alive: 1 Hz oscillator at -80 dB.  Non-zero samples prevent iOS from
      // classifying the stream as silent and deactivating the audio hardware.
      _keepOsc  = _audioCtx.createOscillator();
      _keepGain = _audioCtx.createGain();
      _keepGain.gain.value    = 0.0001;  // -80 dB — inaudible
      _keepOsc.frequency.value = 1;      // 1 Hz — below hearing range
      _keepOsc.connect(_keepGain);
      _keepGain.connect(_audioCtx.destination);
      _keepOsc.start();
    }
    // Resume if suspended (browser autoplay policy).
    if (_audioCtx.state === 'suspended') _audioCtx.resume().catch(function() {});
    return _audioCtx;
  } catch(e) { return null; }
}

function beep(duration, freq) {
  try {
    var ctx = getAudioCtx();
    if (!ctx) return;
    var osc = ctx.createOscillator();
    osc.type = 'square'; osc.frequency.value = freq || 880;
    osc.connect(ctx.destination);
    // Fix B: disconnect immediately when done — prevents V8 audio-GC stall.
    osc.onended = function() { try { osc.disconnect(); } catch(e) {} };
    osc.start();
    osc.stop(ctx.currentTime + (duration || 200) / 1000);
  } catch(e) {}
}

// ── SSE ────────────────────────────────────────────────────────────────────
(function() {
  if (!window.EventSource) return;
  var src = new EventSource('/events');

  src.addEventListener('open', function() {
    var dot = el('sseStatus');
    if (dot) { dot.className = 'sse-dot connected'; dot.title = 'SSE接続中'; }
  });

  src.addEventListener('error', function() {
    var dot = el('sseStatus');
    if (dot) { dot.className = 'sse-dot disconnected'; dot.title = 'SSE切断'; }
  });

  src.addEventListener('rssi', function(e) {
    try {
      var d = JSON.parse(e.data);
      if (Array.isArray(d.r)) {
        for (var i = 0; i < NUM_PILOTS && i < d.r.length; i++) {
          rssiValues[i] = d.r[i];
        }
      }
      // Battery voltage from SSE — throttle DOM update to once per second
      // (SSE fires at 20/sec; calling updateBatteryDisplay every time = 120 DOM ops/sec → layout thrash)
      if (typeof d.v === 'number') {
        var nowMs = Date.now();
        if (nowMs - lastBatUpdateMs >= 1000) {
          lastBatUpdateMs = nowMs;
          updateBatteryDisplay((d.v / 10).toFixed(1) + 'v');
        }
      }
    } catch(err) {}
  });

  src.addEventListener('lap', function(e) {
    try {
      var d = JSON.parse(e.data);
      if (typeof d.p === 'number' && typeof d.t === 'number') {
        addLap(d.p, d.t);
      }
    } catch(err) {}
  });
})();

// ── Captive-portal focus guard ──────────────────────────────────────────────
// iOS / Android captive-portal WebViews periodically auto-focus the first
// <input> on the page without any user interaction.
//
// Strategy:
//   1. Track genuine user gestures via pointerdown / touchstart / keydown
//      directly on each input (not document-wide, so Tab-key navigation works).
//   2. On focus, if no gesture preceded it, defer blur() via setTimeout(0).
//      The defer is required because iOS sometimes re-focuses after a
//      synchronous blur() — letting the event loop flush first prevents this.
//   3. autocomplete="off" is also set in the HTML to further reduce
//      browser-initiated auto-focus attempts.
(function() {
  window.addEventListener('load', function() {
    document.querySelectorAll('#config input[type="text"]').forEach(function(inp) {
      var gestured = false;

      // Mark genuine user intent on this specific element
      ['pointerdown', 'touchstart', 'mousedown', 'keydown'].forEach(function(type) {
        inp.addEventListener(type, function() { gestured = true; }, true);
      });

      inp.addEventListener('focus', function() {
        if (!gestured) {
          // No user gesture on this element → captive-portal auto-focus.
          // Defer so iOS cannot immediately re-focus after a sync blur().
          var self = inp;
          setTimeout(function() { try { self.blur(); } catch(e) {} }, 0);
        }
        gestured = false;   // consume — next focus needs a fresh gesture
      });
    });
  });
})();
