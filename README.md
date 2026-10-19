# Drone Live Tracker (server + map + photos)

A Node.js server and web map that receives data from two kinds of device and
shows it live on an OpenStreetMap map. It is hosted on **Render**
(`https://tracker-drone.onrender.com`).

| Device | Sketch | Sends |
|---|---|---|
| EC200U 4G GPS tracker | `new/new.ino` | GPS position as JSON → `POST /` |
| **Waveshare ESP32-S3-SIM7670G-4G** (camera + GNSS) | `esp32s3_sim7670g/esp32s3_sim7670g.ino` | JPEG photo + GPS → `POST /api/photo` |

```
drone-tracker-updated/
├── server.js                 <- receives positions + photos, serves the map and APIs
├── package.json
├── render.yaml               <- Render settings (build/start command, health check)
├── config.json               <- saved transmission delay
├── public/index.html         <- live map: trail, drone marker, photos, delay selector
├── new/new.ino               <- EC200U tracker sketch
├── esp32s3_sim7670g/
│   └── esp32s3_sim7670g.ino  <- Waveshare camera board sketch
└── mobile/App.js             <- Expo (React Native) phone app
```

## How the transmission delay works

1. Choose a delay on the website (for example **10 seconds**). It is saved on
   the server.
2. After each upload the server replies with `{"intervalMs": 10000}`. The
   device reads this and uses it from its **next** upload onwards. A change
   therefore reaches the device within one old interval. For example, going
   from 60 s to 10 s can take up to 60 s.
3. Both sketches measure the delay **start-to-start**: a new upload begins
   every 10 s, and the upload time is included in the 10 s. Before this fix
   the sketch waited 10 s *after* each upload finished, so updates really
   came every 10 s + upload time.
4. The website and the phone app check the server every second and redraw as
   soon as new data arrives. The panel shows:
   - **Last data**: how long ago the newest data arrived
   - **Next expected**: a countdown based on the chosen delay
   - **Actual interval**: the measured gap between the last two uploads, so
     you can check that the device keeps to the delay
   - **Status**: Live / Late / Offline

> A photo upload over LTE Cat-1 takes roughly 2–6 s, depending on signal and
> photo size. If the chosen delay is shorter than the upload time (for
> example 1 s or 2 s), the board sends back-to-back as fast as it can and
> prints a NOTE in the Serial Monitor. For the camera board, use **5 s or
> more**.

## Server API

| Method & path | Purpose |
|---|---|
| `POST /` | EC200U JSON position (unchanged format) |
| `POST /api/photo?trackerId=&lat=&lon=&alt=&ts=` | Raw JPEG body from the camera board. GPS fields are optional. |
| `GET /api/status` | Small summary: latest point, latest photo, delay. Polled every second. |
| `GET /api/history[?since=seq]` | Trail points (only newer ones when `since` is given) |
| `GET /api/latest` | Most recent point |
| `GET /api/photos` | List of stored photos (newest first) |
| `GET /api/photos/<id>.jpg`, `GET /api/photos/latest.jpg` | Photo image |
| `GET/POST /api/config` | Read / change the delay `{ "intervalMs": 10000 }` |
| `POST /api/reset` | Clear trail and photos |
| `GET /healthz` | Health check for Render |

### Limits on Render's free plan

- **Photos and the trail are kept in memory.** They are lost whenever the
  service restarts, which happens on every deploy and when the service
  sleeps. The server keeps the newest **60 photos (max 40 MB)** and the last
  **1,000 points**. To keep photos permanently, add a database or cloud
  storage, or move to a VPS.
- **The service sleeps after 15 minutes with no requests.** The first request
  after that takes ~30–60 s. The sketches wait up to 60–90 s for a reply, so
  the first upload still goes through. Once a device is sending, the service
  stays awake.
- `config.json` (the saved delay) also resets to its committed value after a
  redeploy.

---

## NEXT STEPS (what you need to do)

### Step 1: Deploy the new server to Render

Render deploys from the GitHub repository connected to your
`tracker-drone` service.

1. Copy these files into that repository, replacing the old ones:
   `server.js`, `package.json`, `render.yaml`, `public/index.html`
   (and optionally `README.md`, `new/`, `esp32s3_sim7670g/`, `mobile/`).
2. Commit and push:
   ```bash
   git add .
   git commit -m "Add photo upload for ESP32-S3 SIM7670G and interval-accurate updates"
   git push
   ```
3. On the Render dashboard, open the **tracker-drone** service. It deploys
   automatically, or you can click **Manual Deploy → Deploy latest commit**.
   Check the settings:
   - **Build Command:** `npm install`
   - **Start Command:** `npm start`
   - **Health Check Path:** `/healthz`
4. Open `https://tracker-drone.onrender.com`. You should see the new panel
   with **Latest photo**, **Next expected** and **Actual interval**.
5. Test without hardware. Any JPEG on your PC works:
   ```bash
   curl -X POST "https://tracker-drone.onrender.com/api/photo?trackerId=CAM-01&lat=23.0580&lon=72.5210" --data-binary @photo.jpg
   ```

### Step 2: Prepare the Waveshare ESP32-S3-SIM7670G board

1. Insert a **4G data SIM** (nano SIM) and connect the **LTE antenna** and the
   **GNSS antenna**. GNSS needs open sky to get a fix.
2. Connect the camera module (**OV2640** or OV5640) to the camera connector.
3. On the back of the board, set the DIP switches **CAM = ON** and **4G = ON**.
   Set the USB switch so the Type-C port connects to the ESP32-S3, so you can
   flash it and use the Serial Monitor.
4. Find out whether your board is **V1 or V2** (printed on the board, or ask
   the seller). The camera pins differ between the two.

### Step 3: Flash the camera sketch

1. In the Arduino IDE, install **esp32 by Espressif Systems** (version 3.x)
   from Boards Manager. The camera library is included.
2. Open `esp32s3_sim7670g/esp32s3_sim7670g.ino` and check the settings at
   the top:
   - `BOARD_VERSION`: 1 or 2
   - `APN`: `airtelgprs.com` for Airtel, `jionet` for Jio, `www` for Vi,
     `bsnlnet` for BSNL
   - `TRACKER_ID`: name shown on the map (default `CAM-01`)
   - `SERVER_BASE`: already set to the Render URL
   - `FRAME_SIZE` / `JPEG_QUALITY`: photo size vs upload speed
3. Under **Tools**, set:
   - Board: **ESP32S3 Dev Module**
   - USB CDC On Boot: **Enabled**
   - PSRAM: **QSPI PSRAM** (if the camera fails to start, try **OPI PSRAM**)
   - Flash Size: **16MB**
   - Partition Scheme: any with a 3 MB+ app
4. Upload, then open the Serial Monitor at **115200**. A healthy start looks
   like:
   ```
   Camera ready
   Modem ready at 921600 baud
   Network registered
   Modem initialised
   GNSS : 23.057984, 72.521018      (or "no fix yet" indoors)
   Photo: 38211 bytes
   HTTP status 200, 52 byte reply
   Upload successful
   ```
5. To test indoors (no GNSS fix), set `SIMULATE_GPS_IF_NO_FIX = true` so the
   board still appears on the map. Photos upload either way.

### Step 4: Re-flash the EC200U tracker

`new/new.ino` now uses the same start-to-start timing. Upload it again so the
EC200U also keeps exactly to the chosen delay.

### Step 5: Update and rebuild the phone app

`mobile/App.js` now points to `https://tracker-drone.onrender.com`. It shows
the latest photo and refreshes in step with the chosen delay. Run it with:
```bash
cd mobile
npx expo start
```
For a new APK, rebuild it the way you built the previous one.

### Troubleshooting the camera board

| Serial Monitor shows | Fix |
|---|---|
| `Camera init failed` | CAM switch ON? Correct `BOARD_VERSION`? Try the other PSRAM option. Reseat the camera ribbon. |
| `modem not responding` | 4G switch ON? Try `MODEM_FAST_BAUD 0`. Check that the modem UART is on GPIO17/18 for your board revision. |
| `SIM not ready` | SIM inserted the right way, PIN lock disabled, SIM has data |
| `HTTP status 7xx` | Network/SSL error inside the modem: check APN, signal and data balance |
| `HTTP status 413` | Photo too large: use a smaller `FRAME_SIZE` or a higher `JPEG_QUALITY` number |
| `GNSS : no fix yet` for a long time | GNSS antenna connected and outdoors? A cold start can take a few minutes. |
| `NOTE: this cycle took ... longer than the interval` | Choose a longer delay on the website, or make photos smaller |

## Run locally (optional)

```bash
npm install
npm start
```
Then open `http://localhost:3000`. Simulate the EC200U:
```bash
curl -X POST http://localhost:3000/ -d '{"TrackerId":"TRK-05","Latitude":"23.057984","Longitude":"72.521018","Altitude":"15m","Timestamp":"2026-10-19 10:00:00"}'
```
