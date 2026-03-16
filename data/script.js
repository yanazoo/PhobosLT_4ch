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
var timerInterval = null;
var raceStartTime = null;  // Date.now() at race start for accurate timer
var audioEnabled  = false;
var announcerRate = 1.0;
var totalLaps     = 0;
var currentTab    = 'race';
var speechQueue   = [];
var isSpeaking    = false;

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

      for (var i = 0; i < NUM_PILOTS; i++) {
        var p = pilotConfigs[i];

        el('pname'+i).value = p.name || '';
        setBandChan(i, p.freq || 5658);
        var ml = (p.minLap || 100) / 10;
        el('minLap'+i).value  = ml;
        el('minLapN'+i).value = ml.toFixed(1);

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
setInterval(function() {
  var now = Date.now();
  for (var i = 0; i < NUM_PILOTS; i++) {
    var v = rssiValues[i];

    var bar = el('rssiBar'+i), num = el('rssiNum'+i);
    if (bar) bar.style.width = Math.min(100, v / 255 * 100) + '%';
    if (num) num.textContent = v;

    var cv = el('calibRssi'+i);
    if (cv) cv.textContent = v;

    rssiSeries[i].append(now, v);
    updateChartLines(i);
  }
}, 200);

// ── Save functions ─────────────────────────────────────────────────────────
function savePilotConfig(pilot) {
  var b    = el('bandSelect'+pilot).selectedIndex;
  var c    = el('chanSelect'+pilot).selectedIndex;
  var freq = freqLookup[b][c];
  var name = el('pname'+pilot).value;
  var ml   = Math.round(parseFloat(el('minLapN'+pilot).value) * 10);

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
  announcerRate = parseFloat(el('rateN').value);

  fetch('/config/global', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ alarm: alarm, anType: anType, anRate: anRate }),
  })
  .then(function(r) { return r.json(); })
  .then(function() { showToast('グローバル設定を保存 ✓'); })
  .catch(function() { showToast('保存に失敗しました ✗'); });
}

// ── Race management ────────────────────────────────────────────────────────
function startTimer() {
  // Use real clock time for smooth, drift-free timer display
  raceStartTime = Date.now();
  timerInterval = setInterval(function() {
    var elapsed = Date.now() - raceStartTime;
    var ms = Math.floor((elapsed % 1000) / 10); // centiseconds
    var s  = Math.floor(elapsed / 1000) % 60;
    var m  = Math.floor(elapsed / 60000);
    el('timer').textContent = pad(m)+':'+pad(s)+':'+pad(ms);
  }, 50);
}

function pad(n) { return n < 10 ? '0'+n : ''+n; }

async function startRace() {
  el('startRaceButton').disabled = true;
  clearAllLaps();
  totalLaps = parseInt(el('totalLaps').value) || 0;

  fetch('/timer/countdown', { method: 'POST' }).catch(function() {});

  var overlay = el('countdownOverlay');
  var numEl   = el('countdownNum');
  overlay.style.display = 'flex';

  var steps = [
    { label: '3', speak: '3',       freq: 880,  dur: 120 },
    { label: '2', speak: '2',       freq: 880,  dur: 120 },
    { label: '1', speak: '1',       freq: 880,  dur: 120 },
    { label: 'GO!', speak: 'スタート', freq: 1320, dur: 600 },
  ];

  for (var i = 0; i < steps.length; i++) {
    var s = steps[i];
    numEl.textContent = s.label;
    numEl.style.animation = 'none';
    void numEl.offsetWidth;
    numEl.style.animation = '';
    beep(s.dur, s.freq);
    if (window.speechSynthesis) {
      var u = new SpeechSynthesisUtterance(s.speak);
      u.lang = 'ja-JP'; u.rate = 1.3; u.volume = 1;
      window.speechSynthesis.cancel();
      window.speechSynthesis.speak(u);
    }
    await sleep(1000);
  }

  overlay.style.display = 'none';
  startTimer();
  el('stopRaceButton').disabled = false;
}

function stopRace() {
  speakImmediate('レース終了');
  clearInterval(timerInterval);
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
  var lapNo  = ++lapNos[pilotIdx];
  var lapSec = lapMs / 1000;
  var lapStr = lapSec.toFixed(2);
  var laps   = lapTimesArr[pilotIdx];
  var name   = (pilotConfigs[pilotIdx] && pilotConfigs[pilotIdx].name) || ('Pilot '+(pilotIdx+1));

  // Keep s2/s3 for announcer (internal only, not displayed in table)
  var s2 = '', s3 = '';
  if (laps.length >= 1 && lapNo > 0) {
    s2 = (lapSec + laps[laps.length-1]).toFixed(2);
  }
  if (laps.length >= 2 && lapNo > 0) {
    s3 = (lapSec + laps[laps.length-1] + laps[laps.length-2]).toFixed(2);
  }

  laps.push(lapSec);

  // Cumulative total
  var totalSec = laps.reduce(function(a, b) { return a + b; }, 0);
  var totalStr = totalSec.toFixed(2);

  var tb = document.querySelector('#lapTable'+pilotIdx+' tbody');
  if (!tb) return;

  var row = tb.insertRow(0);
  if (lapNo === 0) row.className = 'hs-row';

  row.insertCell(0).textContent = lapNo;

  var tc = row.insertCell(1);
  tc.className = 'lap-time';
  tc.textContent = lapNo === 0 ? 'HS: '+lapStr+'s' : lapStr+'s';

  row.insertCell(2).textContent = totalStr+'s';

  // Update pilot total display card
  var ptEl = el('pilotTotal'+pilotIdx);
  if (ptEl) ptEl.textContent = totalStr+'s';

  // Lap count badge
  var badge = document.querySelector('#raceCard'+pilotIdx+' .pilot-lapcount');
  if (badge) badge.textContent = lapNo+'周';

  // Goal / announce
  if (totalLaps > 0 && lapNo >= totalLaps) {
    speakImmediate(name+' ゴール！ '+lapStr+'秒');
    showToast(name+' ゴール！ '+lapStr+'s', 3000);
  } else {
    announceType(lapNo, lapStr, s2, s3, name, pilotIdx);
  }
}

function announceType(lapNo, lapStr, s2, s3, name, pilotIdx) {
  var anVal = el('announcerSelect').value;
  switch (anVal) {
    case 'beep':
      beep(100, 330 + pilotIdx * 110);
      break;
    case '1lap':
      if (lapNo === 0) speakJa(name+' ホールショット '+lapStr+'秒');
      else             speakJa(name+' ラップ'+lapNo+' '+lapStr+'秒');
      break;
    case '2lap':
      if (lapNo === 0) speakJa(name+' ホールショット '+lapStr+'秒');
      else if (s2)     speakJa(name+' 2ラップ '+s2+'秒');
      break;
    case '3lap':
      if (lapNo === 0) speakJa(name+' ホールショット '+lapStr+'秒');
      else if (s3)     speakJa(name+' 3ラップ '+s3+'秒');
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
function beep(duration, freq) {
  try {
    var ctx = new (window.AudioContext || window.webkitAudioContext)();
    var osc = ctx.createOscillator();
    osc.type = 'square'; osc.frequency.value = freq || 880;
    osc.connect(ctx.destination); osc.start();
    setTimeout(function() { osc.stop(); ctx.close(); }, duration || 200);
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
      // Battery voltage from SSE (v = raw * 10, so divide by 10 for volts)
      if (typeof d.v === 'number' && d.v > 0) {
        var vStr = (d.v / 10).toFixed(1) + 'v';
        var bv1 = el('bvolt');     if (bv1) bv1.textContent = vStr;
        var bv2 = el('bvoltRace'); if (bv2) bv2.textContent = vStr;
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
