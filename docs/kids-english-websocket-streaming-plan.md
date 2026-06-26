# Kids English WebSocket Streaming Adaptation Plan

## Scope

Adapt the Kids English client to the xiaozhi-server `xiaozhi.conversation.ws.v1`
streaming conversation protocol while keeping the existing HTTP v1 conversation
flow as fallback.

## Protocol Checklist

- [x] Read `../xiaozhi-server/docs/protocol.md` sections for WebSocket streaming,
  binary audio frames, server events, flow, and fallback rules.
- [x] Honor `POST /api/device/hello` `conversationTransport.selected` for normal
  transport selection.
- [x] Keep HTTP v1 start / turn upload / end flow available.
- [x] Send client audio as `XZWS` direction `1` binary frames with
  `pcm_s16le / 16 kHz / mono / 20 ms` payloads.
- [x] Play server direction `2` audio according to `assistant.audio.start`
  `audioFormat` and `transport`.
- [x] Start speaking on `assistant.audio.start`; do not wait for text final,
  ASR final, turn complete, server turn ID, or byte length.
- [x] Stream `pcm_s16le + binary_chunk` output into the playback queue as chunks
  arrive.
- [x] Keep adapter fallback buffers when `server.fallback` arrives, then continue
  waiting for final text/audio events.
- [x] Download and play `assistant.audio.start.transport=url` with the existing
  HTTP audio logic.
- [x] Re-hello and fall back to HTTP v1 for WebSocket failures, expired tokens,
  heartbeat timeout, and `error.payload.fallback=http`.
- [x] Avoid logging device secrets, connect tokens, signatures, or provider
  secrets.

## Verification

- [x] Run local protocol simulation for binary frame and event-state behavior.
- [x] Build ESP32-S3 firmware for the Waveshare target.
