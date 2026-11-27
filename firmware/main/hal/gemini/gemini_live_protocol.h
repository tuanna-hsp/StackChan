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
#include <condition_variable>
#include <deque>
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
    // Guards websocket_ between the main task (connect, close) and the sender task.
    std::mutex socket_mutex_;
    std::unique_ptr<WebSocket> websocket_;

    // Outgoing messages are sent by a separate task, so a slow upload never blocks the
    // main task (which runs tool calls and state changes). Old mic audio is dropped when
    // the upload falls behind, which keeps Gemini's view of the conversation current.
    struct Outgoing {
        std::string text;
        bool is_audio;
    };
    std::mutex send_mutex_;
    std::condition_variable send_cv_;
    std::deque<Outgoing> send_queue_;
    size_t queued_audio_ = 0;
    TaskHandle_t sender_task_ = nullptr;
    std::atomic<bool> sender_stop_{false};
    void SenderTask();
    bool Enqueue(std::string text, bool is_audio);
    void ClearSendQueue();
    void DropQueuedAudio();
    void ResetSocket();
    bool SendNow(const std::string& text);

    // Diagnostics, logged every 5 s during a session when kLogStats is true.
    std::atomic<uint32_t> stat_up_msgs_{0}, stat_up_bytes_{0}, stat_up_dropped_{0};
    std::atomic<uint32_t> stat_send_total_ms_{0}, stat_send_max_ms_{0};
    std::atomic<uint32_t> stat_down_msgs_{0}, stat_down_bytes_{0}, stat_down_audio_bytes_{0};
    std::vector<TaskStatus_t> last_task_stats_;
    uint32_t last_task_runtime_ = 0;
    void LogStats();
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t idle_timer_  = nullptr;

    std::atomic<bool> channel_open_{false};
    std::atomic<bool> in_model_turn_{false};
    std::atomic<bool> drop_model_turn_{false};
    std::atomic<int64_t> last_activity_us_{0};
    TickType_t last_yield_tick_ = 0;
    // Some models reject transcription settings; remember after the first refusal.
    bool transcription_supported_ = true;
    int next_mcp_id_                = 1;
    // Model audio is merged into larger packets, and the start of each answer (and the
    // audio after playback runs dry) is held back briefly, so the decode queue covers
    // network jitter (see HandleModelAudio).
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
    void SendFunctionResult(const std::string& call_id, const std::string& name, const std::string& text,
                            bool is_error);
    void TakePhoto(const std::string& call_id, const std::string& name, const std::string& question);
    void BeginModelTurn();
    void EndModelTurn();
    void EmitJson(cJSON* root);
    void EmitTts(const char* state, const std::string& text = "");
    void EmitStt(const std::string& text);
    void MarkActivity();
    void CheckIdle();
};
