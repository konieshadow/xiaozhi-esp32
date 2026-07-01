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
        self.features = {
            "asrPartial": False,
            "asrSpeechEndpoint": False,
            "serverVadAutoEnd": False,
        }
        self.output_audio_config = {}
        self.input_active = False
        self.turn_audio_end_sent = False
        self.server_auto_end_received = False
        self.client_turn_id = "client-turn-1"
        self.sent_events: list[dict] = []
        self.sent_input_frames: list[bytes] = []
        self.suppressed_input_frames = 0
        self.playback_chunks: list[bytes] = []
        self.downloaded_urls: list[str] = []
        self.buffer = bytearray()
        self.state_events: list[str] = []
        self.turn_complete = False
        self.should_continue_listening = True
        self.playback_finished = False
        self.conversation_id = "conversation-1"
        self.reconnect_required = False

    def on_device_hello(self, response: dict) -> None:
        transport = response.get("data", {}).get("conversationTransport", {})
        if transport.get("selected") == "websocket":
            self.output_audio_config.update(transport.get("outputAudio", {}))

    def on_event(self, event: dict) -> None:
        event_type = event["type"]
        payload = event.get("payload", {})
        if event_type == "session.ready":
            self.features.update(payload.get("features", {}))
            self.output_audio_config.update(payload.get("outputAudio", {}))
            return
        if event_type == "assistant.text.delta":
            return
        if event_type == "server.fallback":
            self.state_events.append("server.fallback")
            return
        if event_type == "asr.speech_started":
            self.state_events.append("asr.speech_started")
            return
        if event_type == "asr.speech_stopped":
            self.state_events.append("asr.speech_stopped")
            if (
                self.features["asrSpeechEndpoint"]
                and not self.features["serverVadAutoEnd"]
                and self.input_active
                and not self.turn_audio_end_sent
                and payload.get("conversationId", self.conversation_id) == self.conversation_id
                and payload.get("clientTurnId", self.client_turn_id) == self.client_turn_id
            ):
                self.send_turn_audio_end("vad_silence")
            return
        if event_type == "turn.audio.auto_end":
            if (
                payload.get("conversationId", self.conversation_id) == self.conversation_id
                and payload.get("clientTurnId", self.client_turn_id) == self.client_turn_id
            ):
                self.server_auto_end_received = True
                self.input_active = False
                self.state_events.append("turn.audio.auto_end")
            return
        if event_type == "assistant.audio.start":
            self.audio_active = True
            self.audio_format = payload.get("audioFormat", "wav")
            self.transport = payload.get("transport", "")
            self.playback_finished = False
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
            self.should_continue_listening = payload.get("shouldContinueListening", True)
            self.maybe_emit_tts_stop()
            return
        if event_type == "conversation.ended":
            self.turn_complete = True
            self.should_continue_listening = False
            self.maybe_emit_tts_stop()
            return
        if event_type == "error" and payload.get("fallback") == "http":
            self.reconnect_required = True

    def start_turn(self) -> None:
        self.input_active = True
        self.turn_audio_end_sent = False
        self.server_auto_end_received = False
        self.sent_events.append(
            {
                "type": "turn.audio.start",
                "conversationId": self.conversation_id,
                "turnId": self.client_turn_id,
                "payload": {"clientTurnId": self.client_turn_id},
            }
        )

    def send_turn_audio_end(self, reason: str = "manual_stop") -> None:
        if self.server_auto_end_received or self.turn_audio_end_sent:
            return
        self.input_active = False
        self.turn_audio_end_sent = True
        self.sent_events.append(
            {
                "type": "turn.audio.end",
                "conversationId": self.conversation_id,
                "turnId": self.client_turn_id,
                "payload": {"reason": reason},
            }
        )

    def send_input_pcm_frame(self, payload: bytes = b"\x00" * FRAME_BYTES) -> bool:
        if not self.input_active:
            if self.server_auto_end_received:
                self.suppressed_input_frames += 1
            return False
        frame = encode_audio_frame(CLIENT_INPUT, len(self.sent_input_frames) + 1, payload)
        self.sent_input_frames.append(frame)
        return True

    def mark_upload_submitted(self) -> None:
        self.state_events.append("upload.submitted.log_only")

    def finish_playback(self) -> None:
        self.playback_finished = True
        self.maybe_emit_tts_stop()

    def maybe_emit_tts_stop(self) -> None:
        if not self.turn_complete or not self.playback_finished:
            return
        if self.should_continue_listening:
            self.state_events.append("listening")
        else:
            self.state_events.append("idle")
            self.conversation_id = ""

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
    assert client.state_events[-1] == "tts.start"
    client.finish_playback()
    assert client.state_events[-1] == "listening"


def test_output_audio_config_is_optional_and_session_ready_can_override_hello() -> None:
    client = SimulatedWsClient()
    client.on_event({"type": "session.ready", "payload": {}})
    assert client.output_audio_config == {}
    client.on_device_hello(
        {
            "data": {
                "conversationTransport": {
                    "selected": "websocket",
                    "outputAudio": {
                        "maxChunkBytes": 4096,
                        "pacingMs": 0,
                        "backpressureHighWaterBytes": 65536,
                        "backpressureLowWaterBytes": 32768,
                    },
                }
            }
        }
    )
    assert client.output_audio_config["maxChunkBytes"] == 4096
    client.on_event(
        {
            "type": "session.ready",
            "payload": {
                "outputAudio": {
                    "maxChunkBytes": 1024,
                    "pacingMs": 15,
                    "backpressureHighWaterBytes": 32768,
                    "backpressureLowWaterBytes": 16384,
                }
            },
        }
    )
    assert client.output_audio_config == {
        "maxChunkBytes": 1024,
        "pacingMs": 15,
        "backpressureHighWaterBytes": 32768,
        "backpressureLowWaterBytes": 16384,
    }


def test_small_pcm_binary_frames_remain_one_active_audio_segment_until_audio_end() -> None:
    client = SimulatedWsClient()
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
    for seq in range(1, 5):
        client.on_binary(encode_audio_frame(SERVER_OUTPUT, seq, bytes([seq]) * 128))
        assert client.audio_active
    client.on_event({"type": "assistant.audio.end", "payload": {}})
    assert not client.audio_active
    assert client.playback_chunks == [bytes([seq]) * 128 for seq in range(1, 5)]


def test_adapter_fallback_keeps_turn_and_plays_wav_after_end() -> None:
    client = SimulatedWsClient()
    client.start_turn()
    assert client.send_input_pcm_frame()
    client.on_event({"type": "server.fallback", "payload": {"reason": "native_init_failed"}})
    client.send_turn_audio_end("manual_stop")
    assert client.sent_events[-1]["type"] == "turn.audio.end"
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


def test_speech_stopped_without_server_auto_end_sends_turn_audio_end() -> None:
    client = SimulatedWsClient()
    client.on_event(
        {
            "type": "session.ready",
            "payload": {
                "features": {
                    "asrPartial": True,
                    "asrSpeechEndpoint": True,
                    "serverVadAutoEnd": False,
                }
            },
        }
    )
    client.start_turn()
    assert client.send_input_pcm_frame()
    client.on_event(
        {
            "type": "asr.speech_stopped",
            "payload": {
                "conversationId": client.conversation_id,
                "clientTurnId": client.client_turn_id,
                "serverVadAutoEndEnabled": False,
                "serverVadAutoEndSkippedReason": "disabled",
            },
        }
    )
    assert client.sent_events[-1]["type"] == "turn.audio.end"
    assert client.sent_events[-1]["payload"]["reason"] == "vad_silence"
    assert not client.input_active


def test_server_vad_auto_end_stops_upload_without_duplicate_end() -> None:
    client = SimulatedWsClient()
    client.on_event(
        {
            "type": "session.ready",
            "payload": {
                "features": {
                    "asrPartial": True,
                    "asrSpeechEndpoint": True,
                    "serverVadAutoEnd": True,
                }
            },
        }
    )
    client.start_turn()
    assert client.send_input_pcm_frame()
    client.on_event(
        {
            "type": "asr.speech_stopped",
            "payload": {
                "conversationId": client.conversation_id,
                "clientTurnId": client.client_turn_id,
                "serverVadAutoEndEnabled": True,
                "serverVadAutoEndScheduled": True,
            },
        }
    )
    assert [event["type"] for event in client.sent_events].count("turn.audio.end") == 0
    client.on_event(
        {
            "type": "turn.audio.auto_end",
            "payload": {
                "conversationId": client.conversation_id,
                "clientTurnId": client.client_turn_id,
                "reason": "vad_silence",
            },
        }
    )
    assert not client.input_active
    assert not client.send_input_pcm_frame()
    client.send_turn_audio_end("manual_stop")
    assert [event["type"] for event in client.sent_events].count("turn.audio.end") == 0
    assert client.suppressed_input_frames == 1


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
    assert client.state_events[-1] == "tts.start"
    assert client.conversation_id == "conversation-1"
    client.finish_playback()
    assert client.state_events[-1] == "idle"
    assert client.conversation_id == ""


def test_turn_complete_before_playback_drain_defers_tts_stop() -> None:
    client = SimulatedWsClient()
    client.on_event(
        {
            "type": "assistant.audio.start",
            "payload": {"audioFormat": "pcm_s16le", "transport": "binary_chunk"},
        }
    )
    client.on_event({"type": "turn.complete", "payload": {"shouldContinueListening": True}})
    assert client.state_events == ["tts.start"]
    client.on_binary(encode_audio_frame(SERVER_OUTPUT, 1, b"\x01\x00" * 320))
    client.on_event({"type": "assistant.audio.end", "payload": {}})
    assert client.state_events == ["tts.start"]
    client.finish_playback()
    assert client.state_events[-1] == "listening"


def test_native_upload_submitted_does_not_emit_user_subtitle() -> None:
    client = SimulatedWsClient()
    client.on_event(
        {
            "type": "assistant.audio.start",
            "payload": {"audioFormat": "pcm_s16le", "transport": "binary_chunk"},
        }
    )
    client.mark_upload_submitted()
    assert "upload.submitted.log_only" in client.state_events
    assert "stt.Audio submitted." not in client.state_events
    assert client.state_events[0] == "tts.start"


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
    test_output_audio_config_is_optional_and_session_ready_can_override_hello()
    test_small_pcm_binary_frames_remain_one_active_audio_segment_until_audio_end()
    test_adapter_fallback_keeps_turn_and_plays_wav_after_end()
    test_speech_stopped_without_server_auto_end_sends_turn_audio_end()
    test_server_vad_auto_end_stops_upload_without_duplicate_end()
    test_url_transport_downloads_on_audio_start()
    test_goodbye_turn_waits_for_audio_then_returns_idle()
    test_turn_complete_before_playback_drain_defers_tts_stop()
    test_native_upload_submitted_does_not_emit_user_subtitle()
    test_error_fallback_http_requires_reconnect()
    print("kids_english_ws_protocol_sim: ok")


if __name__ == "__main__":
    main()
