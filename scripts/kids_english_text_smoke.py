#!/usr/bin/env python3
import argparse
import hashlib
import hmac
import json
import os
import sys
import time
import uuid
import urllib.error
import urllib.parse
import urllib.request


DEFAULT_BASE_URL = "http://127.0.0.1:3000"
DEFAULT_DEVICE_ID = "esp32-devkit-001"
DEFAULT_DEVICE_SECRET = "dev-secret"
DEFAULT_FIRMWARE_VERSION = "0.1.0"
DEFAULT_HARDWARE_MODEL = "ESP32-S3-Touch-LCD-1.85"


class SmokeTestError(Exception):
    pass


def join_url(base_url, path):
    return urllib.parse.urljoin(base_url.rstrip("/") + "/", path.lstrip("/"))


def stable_json(payload):
    return json.dumps(payload, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def sha256_hex(data):
    if isinstance(data, str):
        data = data.encode("utf-8")
    return hashlib.sha256(data).hexdigest()


def signed_headers(method, path, body_for_signature, device_id, device_secret):
    timestamp = str(int(time.time() * 1000))
    nonce = str(uuid.uuid4())
    body_sha256 = sha256_hex(body_for_signature)
    payload = "\n".join([method.upper(), path, timestamp, nonce, body_sha256])
    signature_key = sha256_hex(device_secret)
    signature = hmac.new(signature_key.encode("utf-8"), payload.encode("utf-8"), hashlib.sha256)
    return {
        "x-device-id": device_id,
        "x-device-timestamp": timestamp,
        "x-device-nonce": nonce,
        "x-device-signature": signature.hexdigest(),
    }


def request_json(method, base_url, path, payload=None, auth=True, args=None, timeout=15):
    body = None
    body_for_signature = ""
    headers = {"Accept": "application/json"}
    if payload is not None:
        body_for_signature = stable_json(payload)
        body = body_for_signature.encode("utf-8")
        headers["Content-Type"] = "application/json"

    if auth:
        headers.update(
            signed_headers(
                method,
                path,
                body_for_signature,
                args.device_id,
                args.device_secret,
            )
        )

    request = urllib.request.Request(join_url(base_url, path), data=body, headers=headers, method=method)
    status, response_body = open_request(request, timeout)
    parsed = parse_json_response(method, path, status, response_body)
    if parsed.get("ok") is not True:
        raise SmokeTestError(f"{method} {path} returned ok=false: {response_body}")
    return status, parsed


def open_request(request, timeout):
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return response.status, response.read()
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read()
    except urllib.error.URLError as exc:
        raise SmokeTestError(f"{request.get_method()} {request.full_url} failed: {exc.reason}") from exc


def parse_json_response(method, path, status, response_body):
    response_text = response_body.decode("utf-8", errors="replace")
    try:
        parsed = json.loads(response_text)
    except json.JSONDecodeError as exc:
        raise SmokeTestError(f"{method} {path} returned invalid JSON: {response_text}") from exc

    if status < 200 or status >= 300:
        raise SmokeTestError(f"{method} {path} failed: HTTP {status}: {response_text}")
    return parsed


def require_string(value, name):
    if not isinstance(value, str) or not value:
        raise SmokeTestError(f"Missing or invalid response field: {name}")
    return value


def create_silent_wav(duration_ms=350, sample_rate=16000):
    samples = max(1, int(sample_rate * duration_ms / 1000))
    data_size = samples * 2
    wav = bytearray(44 + data_size)
    wav[0:4] = b"RIFF"
    wav[4:8] = (36 + data_size).to_bytes(4, "little")
    wav[8:12] = b"WAVE"
    wav[12:16] = b"fmt "
    wav[16:20] = (16).to_bytes(4, "little")
    wav[20:22] = (1).to_bytes(2, "little")
    wav[22:24] = (1).to_bytes(2, "little")
    wav[24:28] = sample_rate.to_bytes(4, "little")
    wav[28:32] = (sample_rate * 2).to_bytes(4, "little")
    wav[32:34] = (2).to_bytes(2, "little")
    wav[34:36] = (16).to_bytes(2, "little")
    wav[36:40] = b"data"
    wav[40:44] = data_size.to_bytes(4, "little")
    return bytes(wav)


def multipart_audio_body(boundary, audio, client_turn_id, duration_ms):
    recorded_at = time.strftime("%Y-%m-%dT%H:%M:%S.000Z", time.gmtime())

    def field(name, value):
        return (
            f"--{boundary}\r\n"
            f"Content-Disposition: form-data; name=\"{name}\"\r\n\r\n"
            f"{value}\r\n"
        ).encode("utf-8")

    file_header = (
        f"--{boundary}\r\n"
        "Content-Disposition: form-data; name=\"audio\"; filename=\"sample.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n"
    ).encode("utf-8")
    footer = f"\r\n--{boundary}--\r\n".encode("utf-8")
    return b"".join(
        [
            field("clientTurnId", client_turn_id),
            field("recordedAt", recorded_at),
            field("durationMs", str(duration_ms)),
            file_header,
            audio,
            footer,
        ]
    )


def request_audio_turn(base_url, conversation_id, audio, client_turn_id, args, timeout=30):
    boundary = f"----xiaozhi-smoke-{uuid.uuid4().hex}"
    path = f"/api/conversations/{conversation_id}/turns/audio"
    body = multipart_audio_body(boundary, audio, client_turn_id, args.audio_duration_ms)
    headers = {
        "Accept": "application/json",
        "Content-Type": f"multipart/form-data; boundary={boundary}",
    }
    headers.update(signed_headers("POST", path, "", args.device_id, args.device_secret))
    request = urllib.request.Request(join_url(base_url, path), data=body, headers=headers, method="POST")
    status, response_body = open_request(request, timeout)
    response_text = response_body.decode("utf-8", errors="replace")
    try:
        parsed = json.loads(response_text)
    except json.JSONDecodeError as exc:
        raise SmokeTestError(f"POST {path} returned invalid JSON: {response_text}") from exc
    return status, parsed, response_text


def download_file(url, output_dir, timeout=30):
    os.makedirs(output_dir, exist_ok=True)
    parsed = urllib.parse.urlparse(url)
    basename = os.path.basename(parsed.path) or "tts-audio"
    timestamp = time.strftime("%Y%m%d-%H%M%S")
    output_path = os.path.join(output_dir, f"{timestamp}-{basename}.wav")

    request = urllib.request.Request(url, headers={"Accept": "audio/wav,*/*"}, method="GET")
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            content_type = response.headers.get("Content-Type", "")
            data = response.read()
    except urllib.error.HTTPError as exc:
        error_body = exc.read().decode("utf-8", errors="replace")
        raise SmokeTestError(f"GET {url} failed: HTTP {exc.code}: {error_body}") from exc
    except urllib.error.URLError as exc:
        raise SmokeTestError(f"GET {url} failed: {exc.reason}") from exc

    with open(output_path, "wb") as f:
        f.write(data)

    if not data.startswith(b"RIFF"):
        raise SmokeTestError(f"GET {url} did not return a WAV RIFF body")
    return output_path, len(data), content_type


def normalize_local_audio_url(base_url, audio_url):
    parsed_base = urllib.parse.urlparse(base_url)
    parsed_audio = urllib.parse.urlparse(audio_url)
    if parsed_audio.hostname in {"localhost", "127.0.0.1"}:
        return audio_url
    if parsed_base.hostname in {"localhost", "127.0.0.1"}:
        return urllib.parse.urlunparse(
            (
                parsed_base.scheme,
                parsed_base.netloc,
                parsed_audio.path,
                parsed_audio.params,
                parsed_audio.query,
                parsed_audio.fragment,
            )
        )
    return audio_url


def run_smoke_test(args):
    base_url = args.base_url.rstrip("/")

    health_status, health = request_json("GET", base_url, "/health", auth=False, args=args, timeout=args.timeout)
    print(f"health: HTTP {health_status} ({health.get('data', {}).get('status', 'ok')})")

    hello_payload = {
        "capabilities": {
            "microphone": True,
            "speaker": True,
            "touchScreen": True,
            "wakeWord": True,
        },
        "firmwareVersion": args.firmware_version,
        "hardwareModel": args.hardware_model,
    }
    hello_status, hello = request_json(
        "POST", base_url, "/api/device/hello", hello_payload, args=args, timeout=args.timeout
    )
    hello_data = hello.get("data", {})
    print(f"device hello: HTTP {hello_status} ({hello_data.get('deviceState')})")

    start_payload = {
        "firmwareVersion": args.firmware_version,
        "trigger": args.trigger,
    }
    start_status, start = request_json(
        "POST", base_url, "/api/conversations/start", start_payload, args=args, timeout=args.timeout
    )
    start_data = start.get("data", {})
    conversation_id = require_string(start_data.get("conversationId"), "data.conversationId")
    tts_audio_url = require_string(start_data.get("ttsAudioUrl"), "data.ttsAudioUrl")
    request_id = start_data.get("diagnostics", {}).get("requestId", "")
    print(f"conversation: HTTP {start_status} {conversation_id}")
    if request_id:
        print(f"start requestId: {request_id}")

    local_audio_url = normalize_local_audio_url(base_url, tts_audio_url)
    output_path, size, content_type = download_file(
        local_audio_url,
        args.output_dir,
        timeout=args.download_timeout,
    )
    print(f"start tts: {output_path} ({size} bytes, {content_type or 'unknown content-type'})")

    audio = create_silent_wav(args.audio_duration_ms)
    turn_status, turn, turn_text = request_audio_turn(
        base_url,
        conversation_id,
        audio,
        "smoke-turn-001",
        args,
        timeout=args.upload_timeout,
    )
    if turn_status < 200 or turn_status >= 300 or turn.get("ok") is not True:
        raise SmokeTestError(f"audio turn failed: HTTP {turn_status}: {turn_text}")

    turn_data = turn.get("data", {})
    turn_id = require_string(turn_data.get("turnId"), "data.turnId")
    turn_request_id = turn_data.get("diagnostics", {}).get("requestId", "")
    print(f"audio turn: HTTP {turn_status} {turn_id}")
    if turn_request_id:
        print(f"turn requestId: {turn_request_id}")

    end_status, end = request_json(
        "POST",
        base_url,
        f"/api/conversations/{conversation_id}/end",
        {"reason": "device_end"},
        args=args,
        timeout=args.timeout,
    )
    print(f"end: HTTP {end_status} ({end.get('data', {}).get('deviceState')})")

    ended_status, ended, ended_text = request_audio_turn(
        base_url,
        conversation_id,
        audio,
        "smoke-turn-after-end",
        args,
        timeout=args.upload_timeout,
    )
    ended_code = ended.get("error", {}).get("code")
    if ended_status != 409 or ended_code != "CONVERSATION_ENDED":
        raise SmokeTestError(f"ended upload expected 409 CONVERSATION_ENDED, got HTTP {ended_status}: {ended_text}")
    print(f"ended upload: HTTP {ended_status} ({ended_code})")


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run Kids English conversation smoke test and download returned TTS audio."
    )
    parser.add_argument(
        "--base-url",
        default=os.environ.get("KIDS_ENGLISH_SERVER_URL", DEFAULT_BASE_URL),
        help=f"Kids English server base URL (default: {DEFAULT_BASE_URL})",
    )
    parser.add_argument(
        "--device-id",
        default=os.environ.get("KIDS_ENGLISH_DEVICE_ID", DEFAULT_DEVICE_ID),
        help=f"deviceId sent to the server (default: {DEFAULT_DEVICE_ID})",
    )
    parser.add_argument(
        "--device-secret",
        default=os.environ.get("KIDS_ENGLISH_DEVICE_SECRET", DEFAULT_DEVICE_SECRET),
        help="device secret used for HMAC signatures (default: dev-secret)",
    )
    parser.add_argument(
        "--firmware-version",
        default=os.environ.get("KIDS_ENGLISH_FIRMWARE_VERSION", DEFAULT_FIRMWARE_VERSION),
        help=f"firmwareVersion sent to the server (default: {DEFAULT_FIRMWARE_VERSION})",
    )
    parser.add_argument(
        "--hardware-model",
        default=os.environ.get("KIDS_ENGLISH_HARDWARE_MODEL", DEFAULT_HARDWARE_MODEL),
        help=f"hardwareModel sent to the server (default: {DEFAULT_HARDWARE_MODEL})",
    )
    parser.add_argument(
        "--trigger",
        choices=("wake_word", "touch", "manual"),
        default=os.environ.get("KIDS_ENGLISH_TRIGGER", "manual"),
        help="conversation trigger sent to /api/conversations/start (default: manual)",
    )
    parser.add_argument(
        "--audio-duration-ms",
        type=int,
        default=int(os.environ.get("KIDS_ENGLISH_AUDIO_DURATION_MS", "350")),
        help="duration for generated silent WAV upload (default: 350)",
    )
    parser.add_argument(
        "--output-dir",
        default=os.environ.get("KIDS_ENGLISH_SMOKE_OUTPUT_DIR", "tmp/kids_english_smoke"),
        help="directory for downloaded TTS audio (default: tmp/kids_english_smoke)",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=15,
        help="JSON request timeout in seconds (default: 15)",
    )
    parser.add_argument(
        "--upload-timeout",
        type=float,
        default=30,
        help="audio upload timeout in seconds (default: 30)",
    )
    parser.add_argument(
        "--download-timeout",
        type=float,
        default=30,
        help="TTS audio download timeout in seconds (default: 30)",
    )
    return parser.parse_args()


def main():
    args = parse_args()
    try:
        run_smoke_test(args)
    except SmokeTestError as exc:
        print(f"smoke test failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
