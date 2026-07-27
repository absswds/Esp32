# ESP32-S3 Multi-Client MJPEG Implementation Plan

> **For Hermes:** Implement this plan directly after the plan is saved. Do not upload firmware automatically; Sam will upload to the camera board after local verification.

**Goal:** Let the FireBeetle 2 ESP32-S3 camera serve the same live MJPEG feed to the dashboard and one or more direct viewers at the same time, while `/light` and `/status` remain responsive.

**Architecture:** Replace the current one-client `esp_http_server` `while(true)` handler with the architecture from the researched, working `arkhipenko/esp32-cam-mjpeg-multiclient` project: a dedicated HTTP task registers streaming clients, one camera-capture task owns `esp_camera_fb_get()`, and one broadcaster task sends the same copied JPEG frame round-robin to every connected `WiFiClient`. This avoids both failure modes already observed: (1) per-client frame-buffer contention and (2) transferring an `esp_http_server` request/socket to a task after its handler returned.

**Tech Stack:** Arduino on ESP32-S3, `WebServer`, `WiFiClient`, FreeRTOS tasks/queues/semaphores, `esp_camera`, PlatformIO.

**Research basis:**
- `arkhipenko/esp32-cam-mjpeg-multiclient` explicitly documents FreeRTOS streaming to up to ten clients and provides a complete queue + capture + stream-task implementation.
- Its key design details are: one camera capture task, two copied frame buffers, a queue of `WiFiClient*`, a semaphore protecting frame swaps, and a broadcaster task that serves one client at a time from the shared current frame.
- The current raw-socket attempt was reverted because it compiled but broke both viewers. We will not reuse its detached-request / `dup()` design.

---

## Preconditions and Safety Gates

1. Keep the currently restored single-client firmware as the Git rollback point (`4fe9241`).
2. Do **not** modify the main ESP32 dashboard in this feature; the browser still connects directly to `http://<camIP>/stream`.
3. Do **not** upload automatically. PlatformIO detected no serial `upload_port`.
4. Treat success as a hardware integration test, not merely a compiler result:
   - Viewer A: dashboard live-image card
   - Viewer B: `http://<camIP>/stream` in a second browser/device
   - Both show frames for 2 minutes.
   - `/light?ir=1`, `/light?led=1`, and `/status` answer while both streams run.

---

### Task 1: Preserve rollback point and add a reproducible source-level validation script

**Objective:** Make it impossible to accidentally reintroduce the unsafe detached-`esp_http_server` request/socket approach.

**Files:**
- Modify: `camera/src/main.cpp`
- Create: `camera/scripts/verify_multiclient_arch.py`

**Steps:**
1. Confirm the source is at the known working one-client revision before replacing the server architecture.
2. Add a verification script that asserts:
   - no `httpd_req_t` is copied to heap or used in a spawned task;
   - exactly one task owns `esp_camera_fb_get()`;
   - a capture task, a stream task, queue, and frame semaphore exist;
   - stream clients are retained as `WiFiClient` objects in a bounded queue;
   - `/light`, `/status`, and `/capture` endpoints remain registered;
   - client capacity is bounded at 4 (not 10) to fit the S3 and phone-hotspot bandwidth.
3. Run it before and after implementation.

**Verification:**
```bash
cd D:\binbi\Desktop\test\camera
python scripts/verify_multiclient_arch.py
```
Expected after implementation: `PASS`.

---

### Task 2: Move HTTP handling to a dedicated FreeRTOS task

**Objective:** Make route registration and `handleClient()` run independently of camera capture and frame delivery.

**Files:**
- Modify: `camera/src/main.cpp`

**Steps:**
1. Replace `esp_http_server` with Arduino `WebServer server(80)` only in the camera firmware.
2. Create `mjpeg_server_task` pinned to the application core.
3. In that task:
   - construct a bounded `QueueHandle_t` for up to four `WiFiClient*` stream clients;
   - create the binary frame semaphore;
   - register `/stream`, `/capture`, `/light`, `/status`, `/`; 
   - call `server.handleClient()` every 50–100 ms.
4. Keep `/stream` handler minimal: send the multipart response header immediately, enqueue a heap-owned `WiFiClient`, and return. It must not capture frames or loop.
5. Keep root `/` returning a short health message, so `http://<camIP>/` is no longer a confusing 404.

**Verification:**
- `pio run`
- source verifier confirms no blocking loop exists in the stream request handler.

---

### Task 3: Add a single-owner capture task with double PSRAM buffers

**Objective:** Ensure exactly one task calls `esp_camera_fb_get()` and produces one JPEG frame for all viewers.

**Files:**
- Modify: `camera/src/main.cpp`

**Steps:**
1. Create `camera_capture_task` pinned to the application core.
2. Configure it at 8 FPS initially (125 ms cadence), not 14–15 FPS, because the phone hotspot and two browser clients are the actual bottleneck.
3. Copy each `camera_fb_t` into one of two reusable PSRAM buffers; grow a buffer only when a larger JPEG arrives.
4. Use the frame semaphore to swap the active pointer/length only between broadcasts.
5. Suspend capture when no clients are queued; resume it when `/stream` accepts a client.
6. Log frame size, FPS, free heap/PSRAM only when a client joins/leaves or an allocation fails—never per frame.

**Verification:**
- Source verifier asserts `esp_camera_fb_get()` appears only in the capture task.
- `pio run` passes.

---

### Task 4: Add the round-robin broadcaster task

**Objective:** Deliver the same completed JPEG frame to all connected viewers without competing camera reads.

**Files:**
- Modify: `camera/src/main.cpp`

**Steps:**
1. Create `stream_broadcast_task` pinned to the application core.
2. Remove one `WiFiClient*` from the queue, test `client->connected()`, and discard disconnected clients.
3. Hold the frame semaphore while writing multipart header + JPEG + boundary to that one client.
4. Requeue a still-connected client at the end of the queue.
5. Divide the 8 FPS cadence by active client count to time-slice viewers fairly.
6. Add a short write timeout / disconnect policy for a slow client so it cannot stop all other clients.
7. Cap the queue at four simultaneous clients. A full queue returns HTTP 503 with a readable message instead of hanging.

**Verification:**
- Source verifier checks the queue capacity and stream task.
- `pio run` passes.

---

### Task 5: Preserve control endpoints and add hardware-readable diagnostics

**Objective:** Make LED/IR controls operate while streams run and make failures diagnosable from Serial Monitor.

**Files:**
- Modify: `camera/src/main.cpp`

**Steps:**
1. Maintain `/light?ir=0|1&led=0|1` and `/status` in the HTTP task.
2. Return JSON state from both endpoints.
3. Add `/health` returning `clients`, `frameBytes`, `freeHeap`, `freePsram`, and Wi-Fi RSSI.
4. Print concise lifecycle logs:
   - `[STREAM] client added; active=N`
   - `[STREAM] client dropped; active=N`
   - `[CAM] allocation failed` / Wi-Fi reconnection events.
5. No logging inside the high-frequency send loop except errors.

**Verification:**
- Compile.
- Later hardware checks: open `/health` before streaming, with one viewer, and with two viewers; the reported client count must rise accordingly.

---

### Task 6: Run local regression checks, commit, then hardware validation

**Objective:** Ship only a buildable implementation with a clear test procedure.

**Files:**
- Modify: `camera/src/main.cpp`
- Create: `camera/scripts/verify_multiclient_arch.py`

**Steps:**
1. Run source verifier.
2. Build both targets:
   ```bash
   cd D:\binbi\Desktop\test
   pio run
   cd camera
   pio run
   ```
3. Review `git diff` and confirm no main-board code changed unexpectedly.
4. Commit and push only after both builds pass.
5. Sam uploads only the **camera** firmware.
6. Hardware test sequence:
   - Confirm `/health` responds.
   - Open dashboard camera stream on device A.
   - Open `/stream` directly on device B.
   - Keep both open 2 minutes.
   - Toggle IR and LED from the dashboard while both view.
   - Refresh one viewer; remaining viewer must stay active.
   - Capture Serial Monitor logs and `/health` response if any check fails.

---

## Risks and Design Decisions

| Risk | Mitigation |
|---|---|
| Phone hotspot cannot sustain duplicated MJPEG for several devices | Start at QVGA, JPEG quality 20, 8 FPS, max 4 viewers. If two viewers still saturate it, lower to 6 FPS before lowering resolution further. |
| `WebServer` normally blocks per request | Only the HTTP task calls `handleClient`; all frame capture/broadcast work is outside it. This is the established multiclient reference pattern. |
| Slow client can stall writes | Serve clients round-robin and drop disconnected/slow clients; never recapture per client. |
| PSRAM pressure | Reusable double buffers in PSRAM, bounded clients, explicit allocation failure logs. |
| Existing raw-socket design may tempt re-use | It is explicitly forbidden in the source verifier and not part of this plan. |

## Acceptance Criteria

1. Camera endpoint and dashboard recover to normal one-view operation after upload.
2. Dashboard and direct `/stream` page show live video concurrently for two minutes.
3. `/light` and `/status` respond during concurrent viewing.
4. No WDT reset, heap allocation failure, or camera reboot appears in Serial Monitor.
5. Both local PlatformIO builds and `camera/scripts/verify_multiclient_arch.py` pass.
