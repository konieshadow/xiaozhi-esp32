# Kids English Latency Plan

## 目标

- 缩短孩子说完话到固件开始上传 WAV 的等待时间。
- 保持当前 HTTP 对话协议不变：服务端仍然只接收完整的 multipart WAV。
- 先优化客户端录音结束时机，再评估是否需要 WebSocket/流式上传。

## 当前事实

- HTTP 流程下，服务端只有在 ESP32 调用上传接口并发送完整 WAV 后才开始 ASR、LLM、TTS。
- 固件已经在 Kids English 模式中使用本地 VAD：`Application::HandleKidsEnglishVadChange()` 记录语音/静音状态，`SubmitKidsEnglishRecording()` 结束 PCM 采集并上传。
- 原本自动停止检查依赖 1 秒时钟 tick；即使尾部静音阈值较短，也可能额外等到下一个 tick。
- `KidsEnglishProtocol::UploadPcmAudio()` 会把 PCM 打包成 `wav / 16kHz / mono / 16-bit PCM`，并限制单段音频不超过服务端接受的 10 秒上限。

## 方案边界

### 1. 缩短固定录音窗口

客户端工作。适合作为没有 VAD 或 VAD 失效时的兜底策略，但不能解决孩子提前说完后的全部等待。

本仓库中对应 `KIDS_ENGLISH_MAX_RECORDING_DURATION_MS`，默认保留 10000ms，避免截断较长回答；需要极低延迟演示时可以通过 `menuconfig` 临时调到 3000-5000ms。

### 2. 本地 VAD 自动断句

当前 HTTP 方案下最有效。ESP32 本地检测到孩子说完后的尾部静音后，立即结束 PCM 采集并上传完整 WAV。

本阶段默认把尾部静音阈值设为 800ms，并使用专用 100ms 录音检查定时器，避免继续受 1 秒状态栏 tick 限制。

### 3. 服务端 VAD

只有切到 WebSocket 或流式上传后才真正能减少“孩子说完到服务端开始处理”的等待。纯 HTTP 完整 WAV 上传下，服务端 VAD 最多裁剪静音或改善 ASR 输入，不能提前启动服务端处理。

## 进度

- [x] 确认当前 HTTP 上传路径只能在客户端录音阶段减少等待。
- [x] 确认固件已有本地 VAD 自动停止路径。
- [x] 将 Kids English 录音阈值配置化。
- [x] 将尾部静音默认值收紧到 800ms。
- [x] 增加 100ms 录音检查定时器，降低尾静音检测后的额外等待。
- [ ] 真机验证：短句说完后 800-900ms 左右进入“提交中”。
- [ ] 如真实环境误切尾音，再把尾部静音阈值调到 1000-1200ms。

## 验证计划

- 编译：`idf.py build`
- 真机：刷入 Waveshare ESP32-S3-Touch-LCD-1.85 后说一个 1-2 秒短句。
- 串口日志应出现：
  - `Kids English recording detected speech`
  - `Kids English recording detected silence`
  - `Kids English recording auto stop: silence=1`
  - `Kids English PCM capture: ... durationMs=...`
  - `Uploading Kids English turn: ... durationMs=...`
