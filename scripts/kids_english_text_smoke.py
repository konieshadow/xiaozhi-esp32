#!/usr/bin/env python3
import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request


DEFAULT_BASE_URL = "http://127.0.0.1:3000"
DEFAULT_DEVICE_ID = "esp32-devkit-001"
DEFAULT_FIRMWARE_VERSION = "0.1.0"
DEFAULT_TEXT = "I like apples."


class SmokeTestError(Exception):
    pass


def join_url(base_url, path):
    return urllib.parse.urljoin(base_url.rstrip("/") + "/", path.lstrip("/"))


def request_json(method, url, payload=None, timeout=15):
    body = None
    headers = {"Accept": "application/json"}
    if payload is not None:
        body = json.dumps(payload).encode("utf-8")
        headers["Content-Type"] = "application/json"

    request = urllib.request.Request(url, data=body, headers=headers, method=method)
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            response_body = response.read().decode("utf-8")
            status = response.status
    except urllib.error.HTTPError as exc:
        error_body = exc.read().decode("utf-8", errors="replace")
        raise SmokeTestError(f"{method} {url} failed: HTTP {exc.code}: {error_body}") from exc
    except urllib.error.URLError as exc:
        raise SmokeTestError(f"{method} {url} failed: {exc.reason}") from exc

    if status < 200 or status >= 300:
        raise SmokeTestError(f"{method} {url} failed: HTTP {status}: {response_body}")

    try:
        parsed = json.loads(response_body)
    except json.JSONDecodeError as exc:
        raise SmokeTestError(f"{method} {url} returned invalid JSON: {response_body}") from exc

    if parsed.get("ok") is not True:
        raise SmokeTestError(f"{method} {url} returned ok=false: {response_body}")

    return parsed


def require_string(value, name):
    if not isinstance(value, str) or not value:
        raise SmokeTestError(f"Missing or invalid response field: {name}")
    return value


def download_file(url, output_dir, timeout=30):
    os.makedirs(output_dir, exist_ok=True)
    parsed = urllib.parse.urlparse(url)
    basename = os.path.basename(parsed.path) or "tts-audio"
    timestamp = time.strftime("%Y%m%d-%H%M%S")
    output_path = os.path.join(output_dir, f"{timestamp}-{basename}")

    request = urllib.request.Request(url, headers={"Accept": "*/*"}, method="GET")
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

    return output_path, len(data), content_type


def run_smoke_test(args):
    base_url = args.base_url.rstrip("/")

    health = request_json("GET", join_url(base_url, "/health"), timeout=args.timeout)
    health_status = health.get("data", {}).get("status", "ok")
    print(f"health: ok ({health_status})")

    start = request_json(
        "POST",
        join_url(base_url, "/api/sessions/start"),
        {
            "deviceId": args.device_id,
            "firmwareVersion": args.firmware_version,
        },
        timeout=args.timeout,
    )
    start_data = start.get("data", {})
    session_id = require_string(start_data.get("sessionId"), "data.sessionId")
    prompt = start_data.get("prompt", {})
    prompt_id = require_string(prompt.get("id"), "data.prompt.id")
    prompt_text = require_string(prompt.get("text"), "data.prompt.text")
    print(f"session: {session_id}")
    print(f"prompt: {prompt_id} {prompt_text}")

    practice = request_json(
        "POST",
        join_url(base_url, "/api/practice/text"),
        {
            "sessionId": session_id,
            "promptId": prompt_id,
            "deviceId": args.device_id,
            "text": args.text,
        },
        timeout=args.timeout,
    )
    practice_data = practice.get("data", {})
    tts_audio_url = require_string(practice_data.get("ttsAudioUrl"), "data.ttsAudioUrl")
    request_id = practice_data.get("diagnostics", {}).get("requestId", "")
    next_prompt = practice_data.get("nextPrompt", {})
    next_prompt_id = next_prompt.get("id", "")
    next_prompt_text = next_prompt.get("text", "")

    print(f"score: {practice_data.get('score')}")
    if request_id:
        print(f"requestId: {request_id}")
    if next_prompt_id or next_prompt_text:
        print(f"nextPrompt: {next_prompt_id} {next_prompt_text}".rstrip())
    print(f"ttsAudioUrl: {tts_audio_url}")

    output_path, size, content_type = download_file(
        tts_audio_url,
        args.output_dir,
        timeout=args.download_timeout,
    )
    print(f"downloaded: {output_path} ({size} bytes, {content_type or 'unknown content-type'})")


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run Kids English text smoke test and download returned TTS audio."
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
        "--firmware-version",
        default=os.environ.get("KIDS_ENGLISH_FIRMWARE_VERSION", DEFAULT_FIRMWARE_VERSION),
        help=f"firmwareVersion sent to the server (default: {DEFAULT_FIRMWARE_VERSION})",
    )
    parser.add_argument(
        "--text",
        default=os.environ.get("KIDS_ENGLISH_TEXT", DEFAULT_TEXT),
        help=f"text submitted to /api/practice/text (default: {DEFAULT_TEXT!r})",
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
