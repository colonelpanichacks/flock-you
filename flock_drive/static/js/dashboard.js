document.addEventListener('DOMContentLoaded', () => {
    const socket = io();

    // DOM Elements
    const feedList = document.getElementById('feed-list');
    const radarBlips = document.getElementById('radar-blips');
    const threatBar = document.getElementById('threat-bar');
    const threatText = document.getElementById('threat-text');
    const elDetectionCount = document.getElementById('detection-count');
    const elGpsStatus = document.getElementById('gps-status');
    const elUptime = document.getElementById('uptime');
    const alertFlash = document.getElementById('alert-flash');

    // View Toggles
    const btnRadar = document.getElementById('btn-radar');
    const btnMap = document.getElementById('btn-map');
    const viewRadar = document.getElementById('radar-view');
    const viewMap = document.getElementById('map-view');

    let startTime = Date.now();
    let highestThreat = 0;
    let threatDecayTimer = null;

    // Audio Context (must be initialized after user interaction)
    let audioCtx = null;
    let audioEnabled = false;

    // Map State
    let map = null;
    let userMarker = null;
    let markers = [];
    let currentLocation = null; // {lat, lon}

    // --- INITIALIZATION ---
    function initAudio() {
        if (!audioCtx) {
            audioCtx = new (window.AudioContext || window.webkitAudioContext)();
            audioEnabled = true;
            console.log("[Audio] Browser Audio Enabled");
            playTone(800, 0.1, 'sine'); // Test beep
        }
    }

    // Auto-enable audio on first click
    document.body.addEventListener('click', initAudio, { once: true });

    function initMap() {
        // Initialize Leaflet
        // Default to 0,0 until GPS fix
        map = L.map('map', {
            zoomControl: false,
            attributionControl: false
        }).setView([0, 0], 2);

        // Offline Grid (No tiles by default to support offline)
        // If we want tiles, we can add them here, but tactical grid is handled by CSS
        // We can add a simple tile layer if available, or just use the grid

        // Custom Marker Icons
        const userIcon = L.divIcon({
            className: 'custom-div-icon',
            html: "<div style='background-color:#00ff41;width:10px;height:10px;border-radius:50%;box-shadow:0 0 5px #00ff41;'></div>",
            iconSize: [10, 10],
            iconAnchor: [5, 5]
        });

        userMarker = L.marker([0, 0], {icon: userIcon}).addTo(map);
    }

    // Toggle Handlers
    btnRadar.addEventListener('click', () => {
        btnRadar.classList.add('active');
        btnMap.classList.remove('active');
        viewRadar.style.display = 'block';
        viewMap.style.display = 'none';
    });

    btnMap.addEventListener('click', () => {
        btnMap.classList.add('active');
        btnRadar.classList.remove('active');
        viewMap.style.display = 'block';
        viewRadar.style.display = 'none';

        // Leaflet needs to know it's visible to resize correctly
        if (map) {
            setTimeout(() => { map.invalidateSize(); }, 100);
        } else {
            initMap();
        }
    });

    // --- SOCKET HANDLERS ---

    socket.on('connect', () => {
        console.log('[System] Connected to mainframe.');
    });

    socket.on('status_update', (stats) => {
        elDetectionCount.innerText = stats.detection_count;
        elGpsStatus.innerText = stats.gps_status;
        if(stats.start_time) startTime = stats.start_time * 1000;
    });

    socket.on('gps_update', (data) => {
        elGpsStatus.innerText = `FIX [${data.lat.toFixed(4)}, ${data.lon.toFixed(4)}]`;
        elGpsStatus.style.color = 'var(--primary)';

        currentLocation = { lat: data.lat, lon: data.lon };

        if (map) {
            const newLatLng = new L.LatLng(data.lat, data.lon);
            userMarker.setLatLng(newLatLng);
            // Center map if it's the first fix or user requests it (auto-follow could be added)
            // For now, center if we were at 0,0
            if (map.getCenter().lat === 0 && map.getCenter().lng === 0) {
                map.setView(newLatLng, 15);
            }
        }
    });

    socket.on('new_detection', (data) => {
        addFeedItem(data);
        addRadarBlip(data);
        updateThreatLevel(data.threat_score);

        // Alerts
        if (data.threat_score >= 70) {
            triggerVisualAlert();
            playAlertSound('high');
        } else if (data.threat_score >= 50) {
            playAlertSound('low');
        }

        // Map Plotting
        if (map && currentLocation && data.latitude && data.longitude) {
            addMapMarker(data);
        }
    });

    // --- UI FUNCTIONS ---

    function triggerVisualAlert() {
        alertFlash.classList.add('active');
        setTimeout(() => {
            alertFlash.classList.remove('active');
        }, 500);
    }

    function playTone(freq, duration, type='square') {
        if (!audioCtx) return;
        const osc = audioCtx.createOscillator();
        const gain = audioCtx.createGain();

        osc.type = type;
        osc.frequency.value = freq;
        osc.connect(gain);
        gain.connect(audioCtx.destination);

        osc.start();
        gain.gain.exponentialRampToValueAtTime(0.00001, audioCtx.currentTime + duration);
        osc.stop(audioCtx.currentTime + duration);
    }

    function playAlertSound(level) {
        if (level === 'high') {
            playTone(1200, 0.1, 'sawtooth');
            setTimeout(() => playTone(1200, 0.1, 'sawtooth'), 150);
            setTimeout(() => playTone(1200, 0.1, 'sawtooth'), 300);
        } else {
            playTone(800, 0.15, 'square');
        }
    }

    function addFeedItem(data) {
        const item = document.createElement('div');
        item.className = 'feed-item';
        if (data.threat_score >= 90) item.classList.add('critical');

        const timeStr = new Date().toLocaleTimeString('en-US', {hour12: false});

        item.innerHTML = `
            <span>${timeStr}</span>
            <span>${data.protocol || 'WIFI'}</span>
            <span>${data.mac}<br><small>${data.name || 'UNKNOWN'}</small></span>
            <span>${data.rssi}dB</span>
        `;

        feedList.insertBefore(item, feedList.firstChild);

        // Limit feed size
        if (feedList.children.length > 50) {
            feedList.removeChild(feedList.lastChild);
        }
    }

    function addRadarBlip(data) {
        const blip = document.createElement('div');
        blip.className = 'blip';

        // Random position for visual effect (since RSSI isn't directional)
        // RSSI (-100 to -30) -> Distance (Center is close)
        // Map RSSI to radius (0 to 45%)
        let rssi = Math.max(-100, Math.min(-30, data.rssi));
        let distance = ((rssi + 30) / -70) * 45;

        let angle = Math.random() * 360;

        blip.style.left = `calc(50% + ${distance * Math.cos(angle * Math.PI / 180)}%)`;
        blip.style.top = `calc(50% + ${distance * Math.sin(angle * Math.PI / 180)}%)`;

        if (data.threat_score >= 90) blip.classList.add('critical');
        else if (data.threat_score < 50) blip.classList.add('safe');

        radarBlips.appendChild(blip);

        // Remove after animation
        setTimeout(() => {
            blip.remove();
        }, 3000);
    }

    function addMapMarker(data) {
        // Red dot for threat
        const color = data.threat_score >= 70 ? '#ff0055' : '#ffcc00';

        const icon = L.divIcon({
            className: 'custom-div-icon',
            html: `<div style='background-color:${color};width:8px;height:8px;border-radius:50%;box-shadow:0 0 5px ${color};'></div>`,
            iconSize: [8, 8],
            iconAnchor: [4, 4]
        });

        // Use detected lat/lon (which comes from GPS manager at time of detection)
        // Note: If detection has no lat/lon, we use current location as approx or skip
        const lat = data.latitude || currentLocation.lat;
        const lon = data.longitude || currentLocation.lon;

        const marker = L.marker([lat, lon], {icon: icon})
            .bindPopup(`${data.name || 'Unknown'} (${data.rssi}dB)`)
            .addTo(map);

        markers.push(marker);

        // Limit markers on map
        if (markers.length > 100) {
            const old = markers.shift();
            map.removeLayer(old);
        }
    }

    function updateThreatLevel(score) {
        if (score > highestThreat) {
            highestThreat = score;
        }

        threatBar.style.width = `${highestThreat}%`;

        if (highestThreat >= 90) {
            threatText.innerText = "CRITICAL";
            threatText.style.color = "var(--accent)";
        } else if (highestThreat >= 50) {
            threatText.innerText = "ELEVATED";
            threatText.style.color = "var(--warning)";
        } else {
            threatText.innerText = "LOW";
            threatText.style.color = "var(--primary)";
        }

        // Reset decay timer
        if (threatDecayTimer) clearTimeout(threatDecayTimer);
        threatDecayTimer = setTimeout(() => {
            highestThreat = 0;
            threatBar.style.width = '0%';
            threatText.innerText = "SCANNING";
            threatText.style.color = "var(--secondary)";
        }, 5000);
    }

    // Uptime Clock
    setInterval(() => {
        const diff = Date.now() - startTime;
        const seconds = Math.floor((diff / 1000) % 60).toString().padStart(2, '0');
        const minutes = Math.floor((diff / (1000 * 60)) % 60).toString().padStart(2, '0');
        const hours = Math.floor((diff / (1000 * 60 * 60))).toString().padStart(2, '0');
        elUptime.innerText = `${hours}:${minutes}:${seconds}`;
    }, 1000);
});
