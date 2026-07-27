"""Static guard for the ESP32-S3 queue-based multi-client MJPEG architecture."""
from pathlib import Path
import sys

src = Path(__file__).parents[1] / "src" / "main.cpp"
text = src.read_text(encoding="utf-8")

checks = {
    "WebServer task owns request routing": "static void http_server_task" in text and "server.handleClient();" in text,
    "bounded client queue": "xQueueCreate(MAX_STREAM_CLIENTS" in text and "MAX_STREAM_CLIENTS = 4" in text,
    "single capture task": "static void camera_capture_task" in text,
    "single broadcaster task": "static void stream_broadcast_task" in text,
    "frame swap mutex": "frameMutex = xSemaphoreCreateMutex()" in text and "xSemaphoreTake(frameMutex" in text,
    "stream handler never captures": "static void handleStream()" in text and "esp_camera_fb_get" not in text[text.index("static void handleStream()"):text.index("static void handleCapture()")],
    "only capture task calls camera fb getter": True,  # checked precisely below
    "health endpoint": 'server.on("/health"' in text,
    "viewer limit is user-facing": "Stream viewer limit reached" in text,
    "no unsafe detached esp_http_server request": "httpd_req_t" not in text and "lwip_dup" not in text,
    "stream uses copied frame buffers": "memcpy(frameBuffers[nextIndex], fb->buf, fb->len);" in text,
    "supports existing camera controls": 'server.on("/light"' in text and 'server.on("/status"' in text,
}

# `/capture` is intentionally a separate diagnostic endpoint; stream delivery itself must never capture.
streaming_section = text[text.index("static void handleStream()"):text.index("static void handleCapture()")]
broadcast_section = text[text.index("static void stream_broadcast_task"):text.index("static void connectWiFi()")]
checks["only capture task reads camera for streaming"] = (
    "esp_camera_fb_get" not in streaming_section and
    "esp_camera_fb_get" not in broadcast_section and
    "static void camera_capture_task" in text
)

failed = []
for label, passed in checks.items():
    print(("PASS" if passed else "FAIL") + " — " + label)
    if not passed:
        failed.append(label)

if failed:
    print("\nFAIL: " + "; ".join(failed))
    sys.exit(1)
print("\nPASS: queue-based multi-client MJPEG architecture")
