// PhobosLT 4ch - Web UI Script
const NUM_PILOTS = 4;

const bcf = document.getElementById("bandChannelFreq");
const bandSelect = document.getElementById("bandSelect");
const channelSelect = document.getElementById("channelSelect");
const freqOutput = document.getElementById("freqOutput");
const announcerSelect = document.getElementById("announcerSelect");
const announcerRateInput = document.getElementById("rate");
const pilotNameInput = document.getElementById("pname");
const minLapInput = document.getElementById("minLap");
const alarmThreshold = document.getElementById("alarmThreshold");
const enterRssiCfgInput = document.getElementById("enterRssiCfg");
const exitRssiCfgInput = document.getElementById("exitRssiCfg");
const enterSpanCfg = document.getElementById("enterSpanCfg");
const exitSpanCfg = document.getElementById("exitSpanCfg");

const freqLookup = [
  [5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725],
  [5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866],
  [5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945],
  [5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880],
  [5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917],
  [5362, 5399, 5436, 5473, 5510, 5547, 5584, 5621],
];

const configTab = document.getElementById("config");
const raceTab = document.getElementById("race");
const calibTab = document.getElementById("calib");
const otaTab = document.getElementById("ota");

// Per-pilot state
var pilotConfigs = [];
var currentPilot = 0;
var announcerRate = 1.0;

// Per-pilot lap tracking
var lapNos = [-1, -1, -1, -1];
var lapTimesArr = [[], [], [], []];

var timerInterval;
const timer = document.getElementById("timer");
const startRaceButton = document.getElementById("startRaceButton");
const stopRaceButton = document.getElementById("stopRaceButton");
const batteryVoltageDisplay = document.getElementById("bvolt");

// RSSI charting
var rssiBuffers = [[], [], [], []];
var rssiValues = [0, 0, 0, 0];
var rssiSending = false;
var rssiCharts = [];
var rssiSeries = [];
var rssiCrossingSeries = [];

var audioEnabled = false;
var speakObjsQueue = [];

const pilotColors = [
  "hsl(214, 53%, 60%)",  // blue
  "hsl(0, 70%, 60%)",    // red
  "hsl(120, 50%, 50%)",  // green
  "hsl(45, 90%, 55%)",   // yellow
];

const pilotFillColors = [
  "hsla(214, 53%, 60%, 0.4)",
  "hsla(0, 70%, 60%, 0.4)",
  "hsla(120, 50%, 50%, 0.4)",
  "hsla(45, 90%, 55%, 0.4)",
];

// Initialize on page load
onload = function (e) {
  configTab.style.display = "block";
  raceTab.style.display = "none";
  calibTab.style.display = "none";
  otaTab.style.display = "none";

  fetch("/config")
    .then((response) => response.json())
    .then((cfg) => {
      console.log("Config loaded:", cfg);

      // Store pilot configs
      pilotConfigs = cfg.pilots || [];
      while (pilotConfigs.length < NUM_PILOTS) {
        pilotConfigs.push({ freq: 0, minLap: 100, enterRssi: 120, exitRssi: 100, name: "" });
      }

      // Global settings
      alarmThreshold.value = (parseFloat(cfg.alarm) / 10).toFixed(1);
      updateAlarmThreshold(alarmThreshold, alarmThreshold.value);
      announcerSelect.selectedIndex = cfg.anType || 0;
      announcerRateInput.value = (parseFloat(cfg.anRate || 10) / 10).toFixed(1);
      updateAnnouncerRate(announcerRateInput, announcerRateInput.value);

      // Load first pilot
      selectPilot(0);

      // Update race card headers
      updateRaceHeaders();

      stopRaceButton.disabled = true;
      startRaceButton.disabled = false;
      clearInterval(timerInterval);
      timer.innerHTML = "00:00:00s";

      // Create RSSI charts
      for (var i = 0; i < NUM_PILOTS; i++) {
        createRssiChart(i);
      }
    });
};

function updateRaceHeaders() {
  for (var i = 0; i < NUM_PILOTS; i++) {
    var name = pilotConfigs[i].name || ("Pilot " + (i + 1));
    var freq = pilotConfigs[i].freq || 0;
    var card = document.getElementById("raceCard" + i);
    var header = card.querySelector(".pilot-header");
    var freqSpan = header.querySelector(".pilot-freq");
    header.childNodes[0].textContent = name + " ";
    freqSpan.textContent = freq > 0 ? "(" + freq + "MHz)" : "(OFF)";

    // Update calibration headers too
    var rssiBlock = document.getElementById("rssiBlock" + i);
    var h4 = rssiBlock.querySelector("h4");
    h4.childNodes[0].textContent = name + ": ";
  }
}

function selectPilot(idx) {
  currentPilot = idx;
  var cfg = pilotConfigs[idx];

  // Update UI
  setBandChannelIndex(cfg.freq);
  populateFreqOutput();
  pilotNameInput.value = cfg.name || "";
  minLapInput.value = (parseFloat(cfg.minLap) / 10).toFixed(1);
  updateMinLap(minLapInput, minLapInput.value);
  enterRssiCfgInput.value = cfg.enterRssi || 120;
  enterSpanCfg.textContent = enterRssiCfgInput.value;
  exitRssiCfgInput.value = cfg.exitRssi || 100;
  exitSpanCfg.textContent = exitRssiCfgInput.value;

  // Update pilot tab active state
  var tabs = document.querySelectorAll(".pilot-tab");
  tabs.forEach(function (t, i) {
    t.classList.toggle("active", i === idx);
  });
}

function savePilotConfig() {
  var freq = freqLookup[bandSelect.selectedIndex][channelSelect.selectedIndex];
  var data = {
    freq: freq,
    minLap: parseInt(parseFloat(minLapInput.value) * 10),
    enterRssi: parseInt(enterRssiCfgInput.value),
    exitRssi: parseInt(exitRssiCfgInput.value),
    name: pilotNameInput.value,
  };

  // Update local cache
  pilotConfigs[currentPilot] = data;
  updateRaceHeaders();

  fetch("/config/pilot?p=" + currentPilot, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(data),
  })
    .then((r) => r.json())
    .then((r) => console.log("Pilot " + currentPilot + " config saved:", r));
}

function saveGlobalConfig() {
  var data = {
    alarm: parseInt(parseFloat(alarmThreshold.value) * 10),
    anType: announcerSelect.selectedIndex,
    anRate: parseInt(parseFloat(announcerRateInput.value) * 10),
  };

  fetch("/config/global", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(data),
  })
    .then((r) => r.json())
    .then((r) => console.log("Global config saved:", r));
}

function getBatteryVoltage() {
  fetch("/status")
    .then((response) => response.text())
    .then((response) => {
      var match = response.match(/Battery Voltage:\s*([\d.]+v)/);
      batteryVoltageDisplay.innerText = match ? match[1] : "N/A";
    });
}
setInterval(getBatteryVoltage, 2000);

// RSSI Chart management
function createRssiChart(pilotIdx) {
  var series = new TimeSeries();
  var crossingSeries = new TimeSeries();
  rssiSeries.push(series);
  rssiCrossingSeries.push(crossingSeries);

  var chart = new SmoothieChart({
    responsive: true,
    millisPerPixel: 50,
    grid: {
      strokeStyle: "rgba(255,255,255,0.25)",
      sharpLines: true,
      verticalSections: 0,
      borderVisible: false,
    },
    labels: { precision: 0 },
    maxValue: 150,
    minValue: 0,
  });

  chart.addTimeSeries(series, {
    lineWidth: 1.7,
    strokeStyle: pilotColors[pilotIdx],
    fillStyle: pilotFillColors[pilotIdx],
  });
  chart.addTimeSeries(crossingSeries, {
    lineWidth: 1.7,
    strokeStyle: "none",
    fillStyle: "hsla(136, 71%, 70%, 0.3)",
  });

  var canvas = document.getElementById("rssiChart" + pilotIdx);
  chart.streamTo(canvas, 200);

  rssiCharts.push(chart);
}

function addRssiPoints() {
  if (calibTab.style.display == "none") {
    for (var i = 0; i < NUM_PILOTS; i++) {
      rssiCharts[i].stop();
    }
    return;
  }

  var now = Date.now();
  for (var i = 0; i < NUM_PILOTS; i++) {
    rssiCharts[i].start();
    var val = rssiValues[i];
    var cfg = pilotConfigs[i];
    var enterRssi = cfg ? cfg.enterRssi : 120;
    var exitRssi = cfg ? cfg.exitRssi : 100;

    rssiSeries[i].append(now, val);

    // Update RSSI display
    var block = document.getElementById("rssiBlock" + i);
    var valSpan = block.querySelector(".rssi-val");
    valSpan.textContent = val;

    // Update horizontal lines for enter/exit thresholds
    rssiCharts[i].options.horizontalLines = [
      { color: "hsl(8.2, 86.5%, 53.7%)", lineWidth: 1.7, value: enterRssi },
      { color: "hsl(25, 85%, 55%)", lineWidth: 1.7, value: exitRssi },
    ];
    rssiCharts[i].options.maxValue = Math.max(enterRssi + 30, val + 10);
    rssiCharts[i].options.minValue = 0;
  }
}
setInterval(addRssiPoints, 200);

// Tab management
function openTab(evt, tabName) {
  var tabcontent = document.getElementsByClassName("tabcontent");
  for (var i = 0; i < tabcontent.length; i++) {
    tabcontent[i].style.display = "none";
  }
  var tablinks = document.getElementsByClassName("tablinks");
  for (var i = 0; i < tablinks.length; i++) {
    tablinks[i].className = tablinks[i].className.replace(" active", "");
  }
  document.getElementById(tabName).style.display = "block";
  evt.currentTarget.className += " active";

  if (tabName === "calib" && !rssiSending) {
    fetch("/timer/rssiStart", { method: "POST" })
      .then((r) => { if (r.ok) rssiSending = true; return r.json(); })
      .then((r) => console.log("RSSI start:", r));
  } else if (rssiSending && tabName !== "calib") {
    fetch("/timer/rssiStop", { method: "POST" })
      .then((r) => { if (r.ok) rssiSending = false; return r.json(); })
      .then((r) => console.log("RSSI stop:", r));
  }
}

// Config helpers
function updateEnterRssiCfg(obj, value) {
  enterSpanCfg.textContent = value;
  if (parseInt(value) <= parseInt(exitRssiCfgInput.value)) {
    exitRssiCfgInput.value = Math.max(0, parseInt(value) - 1);
    exitSpanCfg.textContent = exitRssiCfgInput.value;
  }
}

function updateExitRssiCfg(obj, value) {
  exitSpanCfg.textContent = value;
  if (parseInt(value) >= parseInt(enterRssiCfgInput.value)) {
    enterRssiCfgInput.value = Math.min(255, parseInt(value) + 1);
    enterSpanCfg.textContent = enterRssiCfgInput.value;
  }
}

function updateMinLap(obj, value) {
  $(obj).parent().find("span").text(parseFloat(value).toFixed(1) + "s");
}

function updateAlarmThreshold(obj, value) {
  $(obj).parent().find("span").text(parseFloat(value).toFixed(1) + "v");
}

function updateAnnouncerRate(obj, value) {
  announcerRate = parseFloat(value);
  $(obj).parent().find("span").text(announcerRate.toFixed(1));
}

function populateFreqOutput() {
  var band = bandSelect.options[bandSelect.selectedIndex].value;
  var chan = channelSelect.options[channelSelect.selectedIndex].value;
  var freq = freqLookup[bandSelect.selectedIndex][channelSelect.selectedIndex];
  freqOutput.textContent = band + chan + " " + freq + "MHz";
}

bcf.addEventListener("change", function () {
  populateFreqOutput();
});

function setBandChannelIndex(freq) {
  for (var i = 0; i < freqLookup.length; i++) {
    for (var j = 0; j < freqLookup[i].length; j++) {
      if (freqLookup[i][j] == freq) {
        bandSelect.selectedIndex = i;
        channelSelect.selectedIndex = j;
        return;
      }
    }
  }
}

// Beep
function beep(duration, frequency, type) {
  var context = new AudioContext();
  var oscillator = context.createOscillator();
  oscillator.type = type;
  oscillator.frequency.value = frequency;
  oscillator.connect(context.destination);
  oscillator.start();
  setTimeout(function () { oscillator.stop(); }, duration);
}

// Lap management
function addLap(pilotIdx, lapStr) {
  var lapNo = ++lapNos[pilotIdx];
  var newLap = parseFloat(lapStr);
  var pilotLaps = lapTimesArr[pilotIdx];
  var name = pilotConfigs[pilotIdx] ? pilotConfigs[pilotIdx].name : "Pilot " + (pilotIdx + 1);

  var table = document.getElementById("lapTable" + pilotIdx);
  var row = table.insertRow();
  var cell1 = row.insertCell(0);
  var cell2 = row.insertCell(1);
  var cell3 = row.insertCell(2);
  var cell4 = row.insertCell(3);

  cell1.innerHTML = lapNo;
  var last2lapStr = "";
  var last3lapStr = "";

  if (lapNo == 0) {
    cell2.innerHTML = "HS: " + lapStr + "s";
  } else {
    cell2.innerHTML = lapStr + "s";
  }

  if (pilotLaps.length >= 2 && lapNo != 0) {
    last2lapStr = (newLap + pilotLaps[pilotLaps.length - 1]).toFixed(2);
    cell3.innerHTML = last2lapStr + "s";
  }
  if (pilotLaps.length >= 3 && lapNo != 0) {
    last3lapStr = (newLap + pilotLaps[pilotLaps.length - 2] + pilotLaps[pilotLaps.length - 1]).toFixed(2);
    cell4.innerHTML = last3lapStr + "s";
  }

  // Announcer
  var anType = announcerSelect.options[announcerSelect.selectedIndex].value;
  switch (anType) {
    case "beep":
      beep(100, 330 + pilotIdx * 110, "square");
      break;
    case "1lap":
      if (lapNo == 0) {
        queueSpeak("<p>" + name + " Hole Shot " + lapStr + "</p>");
      } else {
        queueSpeak("<p>" + name + " Lap " + lapNo + ", " + lapStr + "</p>");
      }
      break;
    case "2lap":
      if (lapNo == 0) {
        queueSpeak("<p>" + name + " Hole Shot " + lapStr + "</p>");
      } else if (last2lapStr) {
        queueSpeak("<p>" + name + " 2 laps " + last2lapStr + "</p>");
      }
      break;
    case "3lap":
      if (lapNo == 0) {
        queueSpeak("<p>" + name + " Hole Shot " + lapStr + "</p>");
      } else if (last3lapStr) {
        queueSpeak("<p>" + name + " 3 laps " + last3lapStr + "</p>");
      }
      break;
  }
  pilotLaps.push(newLap);
}

function clearAllLaps() {
  for (var i = 0; i < NUM_PILOTS; i++) {
    var table = document.getElementById("lapTable" + i);
    var rowCount = table.rows.length;
    for (var j = 1; j < rowCount; j++) {
      table.deleteRow(1);
    }
    lapNos[i] = -1;
    lapTimesArr[i] = [];
  }
}

// Race management
function startTimer() {
  var millis = 0, seconds = 0, minutes = 0;
  timerInterval = setInterval(function () {
    millis += 1;
    if (millis == 100) { millis = 0; seconds++; }
    if (seconds == 60) { seconds = 0; minutes++; }
    if (minutes == 60) { minutes = 0; }
    var m = minutes < 10 ? "0" + minutes : minutes;
    var s = seconds < 10 ? "0" + seconds : seconds;
    var ms = millis < 10 ? "0" + millis : millis;
    timer.innerHTML = m + ":" + s + ":" + ms + "s";
  }, 10);

  fetch("/timer/start", { method: "POST" })
    .then((r) => r.json())
    .then((r) => console.log("Race started:", r));
}

async function startRace() {
  startRaceButton.disabled = true;
  clearAllLaps();

  var baseWPS = (150 / 60) * announcerRate;
  var t1 = (3 / baseWPS) * 1000;
  queueSpeak("<p>Arm your quad</p>");
  await new Promise((r) => setTimeout(r, t1));

  var t2 = (8 / baseWPS) * 1000;
  queueSpeak("<p>Starting on the tone in less than five</p>");
  var delay = Math.random() * (5000 - 1000) + t2;
  await new Promise((r) => setTimeout(r, delay));

  beep(1, 1, "square");
  beep(500, 880, "square");
  startTimer();
  stopRaceButton.disabled = false;
}

function stopRace() {
  queueSpeak("<p>Race stopped</p>");
  clearInterval(timerInterval);
  timer.innerHTML = "00:00:00s";

  fetch("/timer/stop", { method: "POST" })
    .then((r) => r.json())
    .then((r) => console.log("Race stopped:", r));

  stopRaceButton.disabled = true;
  startRaceButton.disabled = false;
}

// Audio
function queueSpeak(obj) {
  if (!audioEnabled) return;
  speakObjsQueue.push(obj);
}

async function enableAudioLoop() {
  audioEnabled = true;
  while (audioEnabled) {
    if (speakObjsQueue.length > 0) {
      if (!$().articulate("isSpeaking")) {
        doSpeak(speakObjsQueue.shift());
      }
    }
    await new Promise((r) => setTimeout(r, 100));
  }
}

function disableAudioLoop() { audioEnabled = false; }

function generateAudio() {
  if (!audioEnabled) return;
  for (var i = 0; i < NUM_PILOTS; i++) {
    var name = pilotConfigs[i] ? pilotConfigs[i].name : "Pilot " + (i + 1);
    queueSpeak("<div>Testing " + name + "</div>");
  }
}

function doSpeak(obj) {
  $(obj).articulate("rate", announcerRate).articulate("speak");
}

// SSE Events
if (!!window.EventSource) {
  var source = new EventSource("/events");

  source.addEventListener("open", function (e) { console.log("Events Connected"); }, false);

  source.addEventListener("error", function (e) {
    if (e.target.readyState != EventSource.OPEN) console.log("Events Disconnected");
  }, false);

  source.addEventListener("rssi", function (e) {
    try {
      var data = JSON.parse(e.data);
      if (data.r) {
        for (var i = 0; i < NUM_PILOTS && i < data.r.length; i++) {
          rssiValues[i] = data.r[i];
        }
      }
    } catch (err) {
      console.log("RSSI parse error:", err);
    }
  }, false);

  source.addEventListener("lap", function (e) {
    try {
      var data = JSON.parse(e.data);
      var pilot = data.p;
      var lapMs = data.t;
      var lap = (parseFloat(lapMs) / 1000).toFixed(2);
      addLap(pilot, lap);
      console.log("Pilot " + pilot + " lap:", lap);
    } catch (err) {
      console.log("Lap parse error:", err);
    }
  }, false);
}
