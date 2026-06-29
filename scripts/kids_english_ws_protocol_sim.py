#!/usr/bin/env python3
"""Local simulation checks for the Kids English WebSocket conversation protocol."""

from __future__ import annotations

import struct


MAGIC = b"XZWS"
VERSION = 1
CLIENT_INPUT = 1
SERVER_OUTPUT = 2
FLAG_END_OF_SEGMENT = 1
SAMPLE_RATE = 16_000
FRAME_DURATION_MS = 20
FRAME_BYTES = SAMPLE_RATE * FRAME_DURATION_MS // 1000 * 2


def encode_audio_frame(direction: int, seq: int, payload: bytes, end_of_segment: bool = False) -> bytes:
    flags = FLAG_END_OF_SEGMENT if end_of_segment else 0
    return MAGIC + bytes([VERSION, direction]) + struct.pack(">HII", flags, seq, len(payload)) + payload


def decode_audio_frame(frame: bytes) -> tuple[int, int, int, bytes]:
    if len(frame) < 16:
        raise AssertionError("frame too short")
    if frame[:4] != MAGIC:
        raise AssertionError("bad magic")
    version = frame[4]
    if version != VERSION:
        raise AssertionError(f"bad version {version}")
    direction = frame[5]
    flags, seq, payload_len = struct.unpack(">HII", frame[6:16])
    payload = frame[16:]
    if payload_len != len(payload):
        raise AssertionError("payload length mismatch")
    return direction, flags, seq, payload


def chunk_input_pcm(pcm: bytes) -> list[bytes]:
    frames: list[bytes] = []
    seq = 1
    for offset in range(0, len(pcm), FRAME_BYTES):
        payload = pcm[offset : offset + FRAME_BYTES]
        if len(payload) < FRAME_BYTES:
            payload = payload + b"\x00" * (FRAME_BYTES - len(payload))
        frames.append(
            encode_audio_frame(
                CLIENT_INPUT,
                seq,
                payload,
                end_of_segment=offset + FRAME_BYTES >= len(pcm),
            )
        )
        seq += 1
    return frames


class SimulatedWsClient:
    def __init__(self) -> None:
        self.audio_active = False
        self.audio_format = ""
        self.transport = ""
        self.playback_chunks: list[bytes] = []
        self.downloaded_urls: list[str] = []
        self.buffer = bytearray()
        self.state_events: list[str] = []
        self.turn_complete = False
        self.conversation_id = "conversation-1"
        self.reconnect_required = False

    def on_event(self, event: dict) -> None:
        event_type = event["type"]
        payload = event.get("payload", {})
        if event_type == "assistant.text.delta":
            return
        if event_type == "server.fallback":
            self.state_events.append("server.fallback")
            return
        if event_type == "assistant.audio.start":
            self.audio_active = True
            self.audio_format = payload.get("audioFormat", "wav")
            self.transport = payload.get("transport", "")
            self.state_events.append("tts.start")
            if self.transport == "url":
                self.downloaded_urls.append(payload["url"])
            return
        if event_type == "assistant.audio.end":
            if self.transport == "binary_chunk" and self.audio_format != "pcm_s16le":
                self.playback_chunks.append(bytes(self.buffer))
                self.buffer.clear()
            self.audio_active = False
            return
        if event_type == "turn.complete":
            self.turn_complete = True
            if payload.get("shouldContinueListening"):
                self.state_events.append("listening")
            else:
                self.state_events.append("idle")
                self.conversation_id = ""
            return
        if event_type == "conversation.ended":
            self.conversation_id = ""
            self.state_events.append("idle")
            return
        if event_type == "error" and payload.get("fallback") == "http":
            self.reconnect_required = True

    def on_binary(self, frame: bytes) -> None:
        direction, flags, _seq, payload = decode_audio_frame(frame)
        if direction != SERVER_OUTPUT:
            raise AssertionError("server output frame expected")
        if not self.audio_active or self.transport != "binary_chunk":
            raise AssertionError("audio chunk outside active segment")
        if self.audio_format == "pcm_s16le":
            if payload:
                self.playback_chunks.append(payload)
        else:
            self.buffer.extend(payload)
        if flags & FLAG_END_OF_SEGMENT:
            self.audio_active = False


def test_input_frames_are_20ms_binary_frames() -> None:
    pcm = bytes(range(251)) * 5
    frames = chunk_input_pcm(pcm)
    assert len(frames) == 2
    for index, frame in enumerate(frames, start=1):
        direction, flags, seq, payload = decode_audio_frame(frame)
        assert direction == CLIENT_INPUT
        assert seq == index
        assert len(payload) == FRAME_BYTES
        assert bool(flags & FLAG_END_OF_SEGMENT) == (index == len(frames))


def test_native_pcm_streaming_starts_on_audio_start_and_plays_chunks() -> None:
    client = SimulatedWsClient()
    client.on_event({"type": "assistant.text.delta", "payload": {"text": "he"}})
    client.on_event(
        {
            "type": "assistant.audio.start",
            "payload": {
                "audioFormat": "pcm_s16le",
                "transport": "binary_chunk",
                "sampleRateHz": SAMPLE_RATE,
                "channels": 1,
            },
        }
    )
    assert client.state_events == ["tts.start"]
    client.on_binary(encode_audio_frame(SERVER_OUTPUT, 1, b"\x01\x00" * 320))
    assert len(client.playback_chunks) == 1
    client.on_binary(encode_audio_frame(SERVER_OUTPUT, 2, b"", end_of_segment=True))
    client.on_event({"type": "assistant.audio.end", "payload": {}})
    client.on_event({"type": "turn.complete", "payload": {"shouldContinueListening": True}})
    assert client.state_events[-1] == "listening"


def test_adapter_fallback_keeps_turn_and_plays_wav_after_end() -> None:
    client = SimulatedWsClient()
    client.on_event({"type": "server.fallback", "payload": {"reason": "native_init_failed"}})
    client.on_event(
        {
            "type": "assistant.audio.start",
            "payload": {"audioFormat": "wav", "transport": "binary_chunk"},
        }
    )
    client.on_binary(encode_audio_frame(SERVER_OUTPUT, 1, b"RIFF"))
    assert client.playback_chunks == []
    client.on_binary(encode_audio_frame(SERVER_OUTPUT, 2, b"WAVE", end_of_segment=True))
    client.on_event({"type": "assistant.audio.end", "payload": {}})
    assert client.playback_chunks == [b"RIFFWAVE"]


def test_url_transport_downloads_on_audio_start() -> None:
    client = SimulatedWsClient()
    client.on_event(
        {
            "type": "assistant.audio.start",
            "payload": {
                "audioFormat": "wav",
                "transport": "url",
                "url": "/api/audio/example",
            },
        }
    )
    assert client.downloaded_urls == ["/api/audio/example"]


def test_goodbye_turn_waits_for_audio_then_returns_idle() -> None:
    client = SimulatedWsClient()
    client.on_event(
        {
            "type": "assistant.audio.start",
            "payload": {"audioFormat": "wav", "transport": "binary_chunk"},
        }
    )
    client.on_binary(encode_audio_frame(SERVER_OUTPUT, 1, b"RIFF"))
    client.on_binary(encode_audio_frame(SERVER_OUTPUT, 2, b"WAVE", end_of_segment=True))
    client.on_event({"type": "assistant.audio.end", "payload": {}})
    assert client.playback_chunks == [b"RIFFWAVE"]
    assert client.conversation_id == "conversation-1"
    client.on_event(
        {
            "type": "turn.complete",
            "payload": {
                "shouldContinueListening": False,
                "diagnostics": {"userGoodbyeDetected": True},
            },
        }
    )
    assert client.state_events[-1] == "idle"
    assert client.conversation_id == ""


def test_error_fallback_http_requires_reconnect() -> None:
    client = SimulatedWsClient()
    client.on_event(
        {
            "type": "error",
            "payload": {"code": "TOKEN_EXPIRED", "recoverable": False, "fallback": "http"},
        }
    )
    assert client.reconnect_required


def main() -> None:
    test_input_frames_are_20ms_binary_frames()
    test_native_pcm_streaming_starts_on_audio_start_and_plays_chunks()
    test_adapter_fallback_keeps_turn_and_plays_wav_after_end()
    test_url_transport_downloads_on_audio_start()
    test_goodbye_turn_waits_for_audio_then_returns_idle()
    test_error_fallback_http_requires_reconnect()
    print("kids_english_ws_protocol_sim: ok")


if __name__ == "__main__":
    main()
