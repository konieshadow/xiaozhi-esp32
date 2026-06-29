#!/usr/bin/env python3
"""Local state-machine simulation for Kids English goodbye conversation handling."""

from __future__ import annotations


class SimulatedHttpConversation:
    def __init__(self) -> None:
        self.conversation_id = "conversation-1"
        self.channel_open = True
        self.state = "listening"
        self.played_audio_urls: list[str] = []
        self.messages: list[tuple[str, str | None, bool]] = []

    def handle_turn_response(self, data: dict) -> None:
        if data.get("conversationId"):
            self.conversation_id = data["conversationId"]
        if data.get("ttsText"):
            self.messages.append(("assistant", data["ttsText"], True))
        if data.get("ttsAudioUrl"):
            self.state = "speaking"
            self.played_audio_urls.append(data["ttsAudioUrl"])

        should_continue = bool(data.get("shouldContinueListening", True))
        if data.get("deviceState") == "finished":
            should_continue = False

        if should_continue:
            self.state = "listening"
            self.messages.append(("tts.stop", None, True))
            return

        self.conversation_id = ""
        self.channel_open = False
        self.state = "idle"
        self.messages.append(("tts.stop", data.get("ttsText"), False))

    def handle_ended_upload_409(self, error_code: str) -> None:
        if error_code != "CONVERSATION_ENDED":
            raise AssertionError(f"unexpected error code {error_code}")
        self.conversation_id = ""
        self.channel_open = False
        self.state = "idle"
        self.messages.append(("tts.stop", None, False))


def test_normal_turn_continues_listening() -> None:
    client = SimulatedHttpConversation()
    client.handle_turn_response(
        {
            "conversationId": "conversation-1",
            "deviceState": "speaking",
            "ttsText": "Tell me one more thing.",
            "ttsAudioUrl": "/api/audio/normal.wav",
            "shouldContinueListening": True,
        }
    )
    assert client.played_audio_urls == ["/api/audio/normal.wav"]
    assert client.conversation_id == "conversation-1"
    assert client.channel_open is True
    assert client.state == "listening"
    assert client.messages[-1] == ("tts.stop", None, True)


def test_goodbye_turn_plays_tts_then_clears_conversation() -> None:
    client = SimulatedHttpConversation()
    client.handle_turn_response(
        {
            "conversationId": "conversation-1",
            "turnId": "turn-1",
            "deviceState": "finished",
            "screenCue": "finished",
            "ttsText": "Bye bye. Great speaking today. See you next time.",
            "ttsAudioUrl": "/api/audio/goodbye.wav",
            "shouldContinueListening": False,
            "endedAt": "2026-06-29T00:00:00.000Z",
            "endReason": "user_goodbye",
            "diagnostics": {"userGoodbyeDetected": True},
        }
    )
    assert client.played_audio_urls == ["/api/audio/goodbye.wav"]
    assert client.conversation_id == ""
    assert client.channel_open is False
    assert client.state == "idle"
    assert client.messages[-1] == (
        "tts.stop",
        "Bye bye. Great speaking today. See you next time.",
        False,
    )


def test_ended_upload_409_clears_conversation() -> None:
    client = SimulatedHttpConversation()
    client.handle_ended_upload_409("CONVERSATION_ENDED")
    assert client.conversation_id == ""
    assert client.channel_open is False
    assert client.state == "idle"
    assert client.messages[-1] == ("tts.stop", None, False)


def main() -> None:
    test_normal_turn_continues_listening()
    test_goodbye_turn_plays_tts_then_clears_conversation()
    test_ended_upload_409_clears_conversation()
    print("kids_english_goodbye_flow_sim: ok")


if __name__ == "__main__":
    main()
