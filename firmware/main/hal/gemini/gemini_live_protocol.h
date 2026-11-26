/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "gemini_live.h"
#include <protocol.h>
#include <web_socket.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_timer.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>
#include <string>

struct cJSON;

// Talks to the Gemini Live API (BidiGenerateContent over a TLS WebSocket) directly from the
// device and presents it to xiaozhi's Application like a xiaozhi server:
//  - mic PCM16 16 kHz -> realtimeInput, Gemini PCM16 24 kHz -> playback (no Opus)
//  - model turns become "tts" start/stop JSON, transcriptions become "stt"/"tts" text
//  - the device's MCP tools are offered to Gemini as functions and run locally
class GeminiLiveProtocol : public Protocol {
public:
    GeminiLiveProtocol();
    ~GeminiLiveProtocol() override;

    bool Start() override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    void SendWakeWordDetected(const std::string& wake_word) override;
    void SendStartListening(ListeningMode mode) override;
    void SendStopListening() override;
    void SendAbortSpeaking(AbortReason reason) override;
    void SendMcpMessage(const std::string& message) override;

protected:
    bool SendText(const std::string& text) override;

private:
    std::unique_ptr<WebSocket> websocket_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t idle_timer_  = nullptr;

    std::atomic<bool> channel_open_{false};
    std::atomic<bool> in_model_turn_{false};
    std::atomic<bool> drop_model_turn_{false};
    std::atomic<int64_t> last_activity_us_{0};
    // Some models reject transcription settings; remember after the first refusal.
    bool transcription_supported_ = true;
    int next_mcp_id_                = 1;
    // Model audio is merged into larger packets, and the start of each answer is held
    // back briefly, so the decode queue covers network jitter (see HandleModelAudio).
    std::mutex pcm_mutex_;
    std::vector<uint8_t> pending_pcm_;
    int pending_pcm_rate_   = 0;
    bool playback_started_  = false;
    std::string user_text_;
    std::string model_text_;

    enum class ConnectResult { kOk, kNetworkError, kSetupRefused };
    ConnectResult Connect(const gemini_live::Config& config, bool with_transcription);
    std::string BuildSetupMessage(const gemini_live::Config& config, bool with_transcription);
    cJSON* BuildFunctionDeclarations();
    void OnMessage(const char* data, size_t len);
    void HandleServerContent(const cJSON* content);
    void HandleModelAudio(const cJSON* inline_data);
    void FlushPcm();
    void ClearPcm();
    void HandleToolCall(const cJSON* tool_call);
    void SendToolResponse(const std::string& call_id, const std::string& name, const std::string& mcp_reply);
    void BeginModelTurn();
    void EndModelTurn();
    void EmitJson(cJSON* root);
    void EmitTts(const char* state, const std::string& text = "");
    void EmitStt(const std::string& text);
    void MarkActivity();
    void CheckIdle();
};
