/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "gemini_live_protocol.h"
#include <algorithm>
#include <application.h>
#include <board.h>
#include <hal/board/stackchan_camera.h>
#include <jpg/image_to_jpeg.h>
#include <mcp_server.h>
#include <assets/lang_config.h>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>
#include <cstdlib>
#include <cstring>
#include <vector>

#define TAG "GeminiLive"

namespace {

constexpr const char* kGeminiUrl =
    "wss://generativelanguage.googleapis.com/ws/"
    "google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent";
constexpr int kInputSampleRate       = 16000;
// Mic audio upload rate. 8000 halves the upload (Gemini resamples input itself), at some
// cost in recognition quality; 16 kHz is used now that Wi-Fi has the RAM to keep up.
constexpr int kUploadSampleRate      = 16000;
constexpr int kDefaultOutputRate     = 24000;
constexpr int kSetupTimeoutMs        = 10000;
// Close the session after this long without conversation, back to wake word standby.
constexpr int64_t kIdleTimeoutUs     = 30LL * 1000 * 1000;
constexpr int kSpeakingWaitMs        = 1000;
// Model audio packet size and the audio held back before an answer starts playing.
// The decode queue holds 20 packets, so 200 ms packets give it ~4 s of headroom.
constexpr int kPcmPacketMs           = 200;
constexpr int kPcmPrebufferMs        = 800;  // also refilled after playback runs dry
// The receive task (priority 1) parses, decodes and decrypts every message. During long
// answers that can keep its CPU busy for seconds, starving the idle task and tripping the
// task watchdog, so it sleeps one tick at least this often while busy.
constexpr TickType_t kYieldInterval  = pdMS_TO_TICKS(500);
constexpr const char* kTakePhotoTool = "self.camera.take_photo";
constexpr uint8_t kPhotoJpegQuality  = 60;
// Mic audio queued for upload beyond this (~3 s of 60 ms packets) is dropped, oldest first.
// Dropping breaks up speech and Gemini cannot understand it, so this only guards against
// a stalled connection; a short backlog is caught up during pauses.
constexpr size_t kMaxQueuedAudio     = 50;
constexpr int kStatsIntervalMs       = 5000;
// Reconnects allowed per conversation after the connection drops.
constexpr int kMaxReconnects         = 3;
// Set to true to log upload/download rates and per-task CPU use every 5 s in a session.
constexpr bool kLogStats             = false;
constexpr EventBits_t kSetupDoneBit  = BIT0;
constexpr EventBits_t kClosedBit     = BIT1;

std::string Base64Encode(const uint8_t* data, size_t len)
{
    size_t out_len = 0;
    mbedtls_base64_encode(nullptr, 0, &out_len, data, len);
    std::string out(out_len, '\0');
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(out.data()), out.size(), &out_len, data, len) != 0) {
        return {};
    }
    out.resize(out_len);
    return out;
}

bool Base64Decode(const char* src, size_t len, std::vector<uint8_t>& out)
{
    size_t out_len = 0;
    mbedtls_base64_decode(nullptr, 0, &out_len, reinterpret_cast<const unsigned char*>(src), len);
    out.resize(out_len);
    if (mbedtls_base64_decode(out.data(), out.size(), &out_len, reinterpret_cast<const unsigned char*>(src), len) !=
        0) {
        return false;
    }
    out.resize(out_len);
    return true;
}

std::string PrintJson(const cJSON* json)
{
    char* str = cJSON_PrintUnformatted(json);
    if (str == nullptr) {
        return {};
    }
    std::string out(str);
    cJSON_free(str);
    return out;
}

// Keeps only the JSON Schema keywords Gemini function declarations accept.
cJSON* SanitizeSchema(const cJSON* schema)
{
    static const char* kAllowed[] = {"type", "description", "enum", "minimum", "maximum", "format", "nullable"};
    cJSON* out = cJSON_CreateObject();
    for (const char* key : kAllowed) {
        const cJSON* item = cJSON_GetObjectItem(schema, key);
        if (item != nullptr) {
            cJSON_AddItemToObject(out, key, cJSON_Duplicate(item, true));
        }
    }
    const cJSON* properties = cJSON_GetObjectItem(schema, "properties");
    if (cJSON_IsObject(properties)) {
        cJSON* props_out = cJSON_AddObjectToObject(out, "properties");
        const cJSON* prop = nullptr;
        cJSON_ArrayForEach (prop, properties) {
            cJSON_AddItemToObject(props_out, prop->string, SanitizeSchema(prop));
        }
    }
    const cJSON* items = cJSON_GetObjectItem(schema, "items");
    if (cJSON_IsObject(items)) {
        cJSON_AddItemToObject(out, "items", SanitizeSchema(items));
    }
    const cJSON* required = cJSON_GetObjectItem(schema, "required");
    if (cJSON_IsArray(required) && cJSON_GetArraySize(required) > 0) {
        cJSON_AddItemToObject(out, "required", cJSON_Duplicate(required, true));
    }
    return out;
}

// cJSON nodes are small, so by default they land in scarce internal RAM
// (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL). Building the setup message with all tool
// declarations exhausted it, and the TLS layer could then not allocate its AES
// DMA buffer. Keep cJSON in PSRAM; free() releases either kind of memory.
void* CjsonPsramMalloc(size_t size)
{
    void* ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return ptr != nullptr ? ptr : malloc(size);
}

int ParsePcmRate(const char* mime_type)
{
    if (mime_type == nullptr) {
        return kDefaultOutputRate;
    }
    const char* rate = strstr(mime_type, "rate=");
    return rate != nullptr ? atoi(rate + 5) : kDefaultOutputRate;
}

}  // namespace

std::unique_ptr<Protocol> gemini_live::create_protocol()
{
    return std::make_unique<GeminiLiveProtocol>();
}

GeminiLiveProtocol::GeminiLiveProtocol()
{
    cJSON_Hooks hooks = {.malloc_fn = CjsonPsramMalloc, .free_fn = free};
    cJSON_InitHooks(&hooks);

    event_group_ = xEventGroupCreate();
    // Gemini Live sends 24 kHz PCM; xiaozhi reads this for its sample rate check.
    server_sample_rate_ = kDefaultOutputRate;

    esp_timer_create_args_t args = {};
    args.callback                = [](void* arg) { static_cast<GeminiLiveProtocol*>(arg)->CheckIdle(); };
    args.arg                     = this;
    args.name                    = "gemini_idle";
    esp_timer_create(&args, &idle_timer_);

    // PSRAM stack: internal RAM is too scarce. The task never touches flash.
    if (xTaskCreatePinnedToCoreWithCaps(
            [](void* arg) {
                static_cast<GeminiLiveProtocol*>(arg)->SenderTask();
                vTaskDeleteWithCaps(nullptr);
            },
            "gemini_send", 6144, this, 3, &sender_task_, tskNO_AFFINITY,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create sender task");
        sender_task_ = nullptr;
    }
}

GeminiLiveProtocol::~GeminiLiveProtocol()
{
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        sender_stop_ = true;
    }
    send_cv_.notify_all();
    while (sender_task_ != nullptr && eTaskGetState(sender_task_) != eDeleted) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (idle_timer_ != nullptr) {
        esp_timer_stop(idle_timer_);
        esp_timer_delete(idle_timer_);
    }
    websocket_.reset();
    vEventGroupDelete(event_group_);
}

bool GeminiLiveProtocol::Start()
{
    gemini_live::start_settings_server();
    McpServer::GetInstance().AddTool(
        "self.gemini.get_settings_url",
        "Returns the web address of this robot's settings page, where the Gemini API key, model, voice "
        "and system prompt can be changed from a phone or computer on the same Wi-Fi.",
        PropertyList(), [](const PropertyList&) -> ReturnValue {
            const std::string url = gemini_live::settings_url();
            return url.empty() ? std::string("The robot has no network address yet.") : url;
        });
    const std::string url = gemini_live::settings_url();
    ESP_LOGI(TAG, "Settings page: %s", url.c_str());

    if (gemini_live::load_config().api_key.empty()) {
        SetError("Gemini API key is not set", "Open " + url + " to set it");
    }
    esp_timer_start_periodic(idle_timer_, 1000 * 1000);
    return true;
}

bool GeminiLiveProtocol::OpenAudioChannel()
{
    // A new conversation starts a new session.
    {
        std::lock_guard<std::mutex> lock(resume_mutex_);
        resume_handle_.clear();
    }
    reconnects_ = 0;
    const auto config = gemini_live::load_config();
    if (config.api_key.empty()) {
        SetError("Gemini API key is not set", "Open " + gemini_live::settings_url() + " to set it");
        return false;
    }

    auto result = Connect(config, transcription_supported_);
    if (result == ConnectResult::kNetworkError) {
        // e.g. a TLS handshake cut short right after Wi-Fi comes up; try once more.
        ESP_LOGW(TAG, "Connection failed, retrying once");
        vTaskDelay(pdMS_TO_TICKS(500));
        result = Connect(config, transcription_supported_);
    }
    if (result == ConnectResult::kSetupRefused && transcription_supported_) {
        // The model may not support audio transcription; retry once without it.
        ESP_LOGW(TAG, "Setup refused, retrying without transcription");
        transcription_supported_ = false;
        result                   = Connect(config, false);
        if (result != ConnectResult::kOk) {
            transcription_supported_ = true;
        }
    }
    if (result == ConnectResult::kOk) {
        return true;
    }
    if (result == ConnectResult::kNetworkError) {
        SetError("Cannot reach Gemini", "Check the network connection");
    } else {
        SetError("Gemini refused the session",
                 "Check the API key, quota and model at " + gemini_live::settings_url());
    }
    return false;
}

GeminiLiveProtocol::ConnectResult GeminiLiveProtocol::Connect(const gemini_live::Config& config,
                                                              bool with_transcription,
                                                              const std::string& resume_handle)
{
    error_occurred_ = false;
    channel_open_   = false;
    in_model_turn_  = false;
    drop_model_turn_ = false;
    user_text_.clear();
    model_text_.clear();
    xEventGroupClearBits(event_group_, kSetupDoneBit | kClosedBit);

    ClearSendQueue();
    auto network = Board::GetInstance().GetNetwork();
    {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        websocket_ = network->CreateWebSocket(1);
    }
    if (websocket_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create websocket");
        return ConnectResult::kNetworkError;
    }
    websocket_->SetReceiveBufferSize(8192);
    // The key goes in a header, not the URL: newer key formats (e.g. "AQ.") are rejected
    // as a ?key= parameter, and it keeps the key out of logged URLs.
    websocket_->SetHeader("x-goog-api-key", config.api_key.c_str());
    websocket_->OnData([this](const char* data, size_t len, bool /*binary*/) {
        // Gemini sends its JSON messages as binary frames.
        last_incoming_time_ = std::chrono::steady_clock::now();
        OnMessage(data, len);
    });
    websocket_->OnDisconnected([this]() {
        ESP_LOGW(TAG, "Gemini closed the connection");
        xEventGroupSetBits(event_group_, kClosedBit);
        if (channel_open_.exchange(false)) {
            // Dropped during a conversation, not closed by us: reconnect on the main task.
            Application::GetInstance().Schedule([this]() { Reconnect(); });
        }
    });

    ESP_LOGI(TAG, "Connecting to Gemini Live, model %s (internal RAM free %u, largest %u)", config.model.c_str(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    if (auto connected = websocket_->Connect(kGeminiUrl); !connected) {
        ESP_LOGE(TAG, "Failed to connect: %s", connected.error().ToString().c_str());
        ResetSocket();
        return ConnectResult::kNetworkError;
    }

    if (!SendNow(BuildSetupMessage(config, with_transcription, resume_handle))) {
        ResetSocket();
        return ConnectResult::kNetworkError;
    }

    const EventBits_t bits = xEventGroupWaitBits(event_group_, kSetupDoneBit | kClosedBit, pdFALSE, pdFALSE,
                                                 pdMS_TO_TICKS(kSetupTimeoutMs));
    if (!(bits & kSetupDoneBit)) {
        ESP_LOGE(TAG, "Setup %s", (bits & kClosedBit) ? "refused (connection closed)" : "timed out");
        ResetSocket();
        return (bits & kClosedBit) ? ConnectResult::kSetupRefused : ConnectResult::kNetworkError;
    }

    ESP_LOGI(TAG, "Session ready");
    last_incoming_time_ = std::chrono::steady_clock::now();
    MarkActivity();
    channel_open_ = true;
    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }
    return ConnectResult::kOk;
}

void GeminiLiveProtocol::Reconnect()
{
    // Skip if the app closed the channel meanwhile (that resets the socket).
    if (channel_open_ || websocket_ == nullptr) {
        return;
    }
    const bool was_answering = in_model_turn_;
    if (was_answering) {
        EndModelTurn();  // the rest of this answer is lost; the queued audio still plays
    }
    drop_model_turn_ = false;
    ClearSendQueue();
    ResetSocket();

    std::string handle;
    {
        std::lock_guard<std::mutex> lock(resume_mutex_);
        handle = resume_handle_;
    }
    ConnectResult result = ConnectResult::kNetworkError;
    if (reconnects_ < kMaxReconnects) {
        ++reconnects_;
        ESP_LOGW(TAG, "Connection lost, reconnecting (%s, attempt %d)",
                 handle.empty() ? "new session" : "resuming session", reconnects_);
        const auto config = gemini_live::load_config();
        result = Connect(config, transcription_supported_, handle);
        if (result != ConnectResult::kOk && !handle.empty()) {
            // The handle may have expired; continue in a new session.
            handle.clear();
            result = Connect(config, transcription_supported_);
        }
    }
    if (result == ConnectResult::kOk) {
        ESP_LOGI(TAG, "Reconnected");
        if (was_answering && !handle.empty()) {
            SendText(
                "{\"realtimeInput\":{\"text\":\"(The connection dropped while you were answering. "
                "Briefly continue from where you stopped.)\"}}");
        }
        return;
    }
    ESP_LOGE(TAG, "Reconnect failed");
    ResetSocket();
    if (on_audio_channel_closed_ != nullptr) {
        on_audio_channel_closed_();
    }
}

void GeminiLiveProtocol::CloseAudioChannel(bool /*send_goodbye*/)
{
    // Unblock the receive task if it is waiting for decode queue space, so the
    // websocket can join it.
    Application::GetInstance().GetAudioService().ResetDecoder();
    const bool was_open = channel_open_.exchange(false);
    ClearSendQueue();
    ResetSocket();
    in_model_turn_ = false;
    if (was_open && on_audio_channel_closed_ != nullptr) {
        on_audio_channel_closed_();
    }
}

bool GeminiLiveProtocol::IsAudioChannelOpened() const
{
    return channel_open_ && websocket_ != nullptr && websocket_->IsConnected() && !error_occurred_ && !IsTimeout();
}

bool GeminiLiveProtocol::SendText(const std::string& text)
{
    return Enqueue(text, false);
}

bool GeminiLiveProtocol::SendNow(const std::string& text)
{
    std::lock_guard<std::mutex> lock(socket_mutex_);
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }
    if (!websocket_->Send(text)) {
        ESP_LOGE(TAG, "Failed to send message");
        return false;
    }
    return true;
}

bool GeminiLiveProtocol::Enqueue(std::string text, bool is_audio)
{
    if (sender_task_ == nullptr) {
        return SendNow(text);
    }
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        if (is_audio && queued_audio_ >= kMaxQueuedAudio) {
            auto oldest = std::find_if(send_queue_.begin(), send_queue_.end(),
                                       [](const Outgoing& item) { return item.is_audio; });
            if (oldest != send_queue_.end()) {
                send_queue_.erase(oldest);
                --queued_audio_;
                stat_up_dropped_++;
            }
        }
        send_queue_.push_back({std::move(text), is_audio});
        if (is_audio) {
            ++queued_audio_;
        }
    }
    send_cv_.notify_one();
    return true;
}

void GeminiLiveProtocol::DropQueuedAudio()
{
    std::lock_guard<std::mutex> lock(send_mutex_);
    send_queue_.erase(std::remove_if(send_queue_.begin(), send_queue_.end(),
                                     [](const Outgoing& item) { return item.is_audio; }),
                      send_queue_.end());
    queued_audio_ = 0;
}

void GeminiLiveProtocol::ResetSocket()
{
    std::lock_guard<std::mutex> lock(socket_mutex_);
    websocket_.reset();
}

void GeminiLiveProtocol::ClearSendQueue()
{
    std::lock_guard<std::mutex> lock(send_mutex_);
    send_queue_.clear();
    queued_audio_ = 0;
}

void GeminiLiveProtocol::SenderTask()
{
    int64_t next_stats_us = esp_timer_get_time() + kStatsIntervalMs * 1000LL;
    while (true) {
        Outgoing item;
        {
            std::unique_lock<std::mutex> lock(send_mutex_);
            send_cv_.wait_for(lock, std::chrono::milliseconds(500),
                              [this]() { return sender_stop_ || !send_queue_.empty(); });
            if (sender_stop_) {
                break;
            }
            if (!send_queue_.empty()) {
                item = std::move(send_queue_.front());
                send_queue_.pop_front();
                if (item.is_audio) {
                    --queued_audio_;
                }
            }
        }
        if (!item.text.empty()) {
            const int64_t start = esp_timer_get_time();
            if (SendNow(item.text)) {
                const uint32_t ms = (esp_timer_get_time() - start) / 1000;
                if (ms > 500) {
                    ESP_LOGW(TAG, "Slow send: %u bytes took %u ms", (unsigned)item.text.size(), (unsigned)ms);
                }
                stat_up_msgs_++;
                stat_up_bytes_ += item.text.size();
                stat_send_total_ms_ += ms;
                if (ms > stat_send_max_ms_) {
                    stat_send_max_ms_ = ms;
                }
            }
        }
        if (esp_timer_get_time() >= next_stats_us) {
            next_stats_us = esp_timer_get_time() + kStatsIntervalMs * 1000LL;
            if (kLogStats && channel_open_) {
                LogStats();
            }
        }
    }
    sender_task_ = nullptr;
}

void GeminiLiveProtocol::LogStats()
{
    const uint32_t up_msgs = stat_up_msgs_.exchange(0);
    const uint32_t up_ms   = stat_send_total_ms_.exchange(0);
    size_t queued          = 0;
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        queued = send_queue_.size();
    }
    ESP_LOGI(TAG,
             "Stats %ds: up %u msgs %u KB (send avg %u ms, max %u ms, dropped %u, queued %u) | "
             "down %u msgs %u KB, audio %u ms | internal RAM %u",
             kStatsIntervalMs / 1000, (unsigned)up_msgs, (unsigned)(stat_up_bytes_.exchange(0) / 1024),
             (unsigned)(up_msgs ? up_ms / up_msgs : 0), (unsigned)stat_send_max_ms_.exchange(0),
             (unsigned)stat_up_dropped_.exchange(0), (unsigned)queued, (unsigned)stat_down_msgs_.exchange(0),
             (unsigned)(stat_down_bytes_.exchange(0) / 1024),
             (unsigned)(stat_down_audio_bytes_.exchange(0) / (kDefaultOutputRate * 2 / 1000)),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    // CPU use per task since the last report (top 8).
    std::vector<TaskStatus_t> tasks(uxTaskGetNumberOfTasks() + 4);
    uint32_t runtime = 0;
    tasks.resize(uxTaskGetSystemState(tasks.data(), tasks.size(), &runtime));
    const uint32_t elapsed = runtime - last_task_runtime_;
    if (!last_task_stats_.empty() && elapsed > 0) {
        std::vector<std::pair<uint32_t, const char*>> usage;
        for (const auto& task : tasks) {
            for (const auto& prev : last_task_stats_) {
                if (prev.xHandle == task.xHandle) {
                    usage.emplace_back(task.ulRunTimeCounter - prev.ulRunTimeCounter, task.pcTaskName);
                    break;
                }
            }
        }
        std::sort(usage.begin(), usage.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::string line;
        for (size_t i = 0; i < usage.size() && i < 8; ++i) {
            char entry[40];
            // Two cores: 200% is both fully busy.
            snprintf(entry, sizeof(entry), " %s %u%%", usage[i].second,
                     (unsigned)((uint64_t)usage[i].first * 100 / elapsed));
            line += entry;
        }
        ESP_LOGI(TAG, "CPU:%s", line.c_str());
    }
    last_task_stats_  = std::move(tasks);
    last_task_runtime_ = runtime;
}

bool GeminiLiveProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet)
{
    if (!channel_open_ || packet->payload.empty()) {
        return false;
    }
    if (in_model_turn_ || !Application::GetInstance().GetAudioService().IsPlaybackIdle()) {
        // Half duplex: skip the mic while Gemini's answer is arriving or still playing, so
        // the answer gets the bandwidth and the speaker's echo cannot interrupt it.
        return true;
    }
    const int input_rate = packet->sample_rate > 0 ? packet->sample_rate : kInputSampleRate;
    const auto* samples  = reinterpret_cast<const int16_t*>(packet->payload.data());
    size_t count         = packet->payload.size() / sizeof(int16_t);
    int rate             = input_rate;
    std::vector<int16_t> reduced;
    if (kUploadSampleRate * 2 == input_rate) {
        // Average sample pairs: a simple low-pass and 2:1 decimation, enough for speech.
        reduced.resize(count / 2);
        for (size_t i = 0; i < reduced.size(); ++i) {
            reduced[i] = static_cast<int16_t>((samples[2 * i] + samples[2 * i + 1]) / 2);
        }
        samples = reduced.data();
        count   = reduced.size();
        rate    = kUploadSampleRate;
    }
    std::string message = "{\"realtimeInput\":{\"audio\":{\"mimeType\":\"audio/pcm;rate=";
    message += std::to_string(rate);
    message += "\",\"data\":\"";
    message += Base64Encode(reinterpret_cast<const uint8_t*>(samples), count * sizeof(int16_t));
    message += "\"}}}";
    return Enqueue(std::move(message), true);
}

void GeminiLiveProtocol::SendWakeWordDetected(const std::string& /*wake_word*/)
{
    // Gemini detects speech itself; nothing to send.
}

void GeminiLiveProtocol::SendStartListening(ListeningMode /*mode*/)
{
    // Gemini's automatic voice activity detection handles turn taking.
}

void GeminiLiveProtocol::SendStopListening()
{
    // Lets Gemini's voice activity detection finish the current user turn.
    SendText("{\"realtimeInput\":{\"audioStreamEnd\":true}}");
}

void GeminiLiveProtocol::SendAbortSpeaking(AbortReason /*reason*/)
{
    // Gemini has no "stop talking" message: drop the rest of this answer locally.
    if (in_model_turn_) {
        drop_model_turn_ = true;
        Application::GetInstance().GetAudioService().ResetDecoder();
        EndModelTurn();
    }
}

void GeminiLiveProtocol::SendMcpMessage(const std::string& /*message*/)
{
    // MCP notifications target the xiaozhi server; tool calls are handled locally.
}

std::string GeminiLiveProtocol::BuildSetupMessage(const gemini_live::Config& config, bool with_transcription,
                                                  const std::string& resume_handle)
{
    cJSON* root  = cJSON_CreateObject();
    cJSON* setup = cJSON_AddObjectToObject(root, "setup");
    cJSON_AddStringToObject(setup, "model", config.model.c_str());

    cJSON* generation = cJSON_AddObjectToObject(setup, "generationConfig");
    cJSON* modalities = cJSON_AddArrayToObject(generation, "responseModalities");
    cJSON_AddItemToArray(modalities, cJSON_CreateString("AUDIO"));
    if (!config.voice.empty()) {
        cJSON* speech = cJSON_AddObjectToObject(generation, "speechConfig");
        cJSON* voice  = cJSON_AddObjectToObject(speech, "voiceConfig");
        cJSON* prebuilt = cJSON_AddObjectToObject(voice, "prebuiltVoiceConfig");
        cJSON_AddStringToObject(prebuilt, "voiceName", config.voice.c_str());
    }

    if (!config.system_prompt.empty()) {
        cJSON* instruction = cJSON_AddObjectToObject(setup, "systemInstruction");
        cJSON* parts       = cJSON_AddArrayToObject(instruction, "parts");
        cJSON* part        = cJSON_CreateObject();
        cJSON_AddStringToObject(part, "text", config.system_prompt.c_str());
        cJSON_AddItemToArray(parts, part);
    }

    cJSON* tools        = cJSON_AddArrayToObject(setup, "tools");
    cJSON* declarations = BuildFunctionDeclarations();
    if (cJSON_GetArraySize(declarations) > 0) {
        cJSON* function_tool = cJSON_CreateObject();
        cJSON_AddItemToObject(function_tool, "functionDeclarations", declarations);
        cJSON_AddItemToArray(tools, function_tool);
    } else {
        cJSON_Delete(declarations);
    }
    cJSON* search_tool = cJSON_CreateObject();
    cJSON_AddObjectToObject(search_tool, "googleSearch");
    cJSON_AddItemToArray(tools, search_tool);

    // Gemini then sends resumption handles; with one, the session continues after a drop.
    cJSON* resumption = cJSON_AddObjectToObject(setup, "sessionResumption");
    if (!resume_handle.empty()) {
        cJSON_AddStringToObject(resumption, "handle", resume_handle.c_str());
    }

    if (with_transcription) {
        cJSON_AddObjectToObject(setup, "inputAudioTranscription");
        cJSON_AddObjectToObject(setup, "outputAudioTranscription");
    }

    std::string message = PrintJson(root);
    cJSON_Delete(root);
    return message;
}

// Lists the device's MCP tools (servos, LEDs, avatar, volume, ...) as Gemini functions.
cJSON* GeminiLiveProtocol::BuildFunctionDeclarations()
{
    cJSON* declarations = cJSON_CreateArray();
    std::string cursor;
    for (int page = 0; page < 10; ++page) {
        cJSON* request = cJSON_CreateObject();
        cJSON_AddStringToObject(request, "jsonrpc", "2.0");
        cJSON_AddNumberToObject(request, "id", next_mcp_id_++);
        cJSON_AddStringToObject(request, "method", "tools/list");
        cJSON* params = cJSON_AddObjectToObject(request, "params");
        if (!cursor.empty()) {
            cJSON_AddStringToObject(params, "cursor", cursor.c_str());
        }
        std::string reply;
        McpServer::GetInstance().ParseMessage(request, [&reply](const std::string& payload) { reply = payload; });
        cJSON_Delete(request);

        cJSON* reply_json = cJSON_Parse(reply.c_str());
        const cJSON* result = cJSON_GetObjectItem(reply_json, "result");
        const cJSON* tools  = cJSON_GetObjectItem(result, "tools");
        const cJSON* tool   = nullptr;
        cJSON_ArrayForEach (tool, tools) {
            const cJSON* name        = cJSON_GetObjectItem(tool, "name");
            const cJSON* description = cJSON_GetObjectItem(tool, "description");
            const cJSON* schema      = cJSON_GetObjectItem(tool, "inputSchema");
            if (!cJSON_IsString(name)) {
                continue;
            }
            cJSON* declaration = cJSON_CreateObject();
            cJSON_AddStringToObject(declaration, "name", name->valuestring);
            cJSON_AddStringToObject(declaration, "description",
                                    cJSON_IsString(description) ? description->valuestring : "");
            const cJSON* properties = cJSON_GetObjectItem(schema, "properties");
            if (cJSON_IsObject(properties) && cJSON_GetArraySize(properties) > 0) {
                cJSON_AddItemToObject(declaration, "parameters", SanitizeSchema(schema));
            }
            cJSON_AddItemToArray(declarations, declaration);
        }
        const cJSON* next = cJSON_GetObjectItem(result, "nextCursor");
        cursor            = cJSON_IsString(next) ? next->valuestring : "";
        cJSON_Delete(reply_json);
        if (cursor.empty()) {
            break;
        }
    }
    ESP_LOGI(TAG, "Offering %d device tools to Gemini", cJSON_GetArraySize(declarations));
    return declarations;
}

void GeminiLiveProtocol::OnMessage(const char* data, size_t len)
{
    stat_down_msgs_++;
    stat_down_bytes_ += len;
    cJSON* root = cJSON_ParseWithLength(data, len);
    if (root == nullptr) {
        ESP_LOGW(TAG, "Unparsable message (%u bytes)", (unsigned)len);
        return;
    }

    if (cJSON_GetObjectItem(root, "setupComplete") != nullptr) {
        xEventGroupSetBits(event_group_, kSetupDoneBit);
    } else if (const cJSON* content = cJSON_GetObjectItem(root, "serverContent"); cJSON_IsObject(content)) {
        HandleServerContent(content);
    } else if (const cJSON* tool_call = cJSON_GetObjectItem(root, "toolCall"); cJSON_IsObject(tool_call)) {
        HandleToolCall(tool_call);
    } else if (const cJSON* update = cJSON_GetObjectItem(root, "sessionResumptionUpdate"); cJSON_IsObject(update)) {
        const cJSON* handle = cJSON_GetObjectItem(update, "newHandle");
        if (cJSON_IsTrue(cJSON_GetObjectItem(update, "resumable")) && cJSON_IsString(handle) &&
            handle->valuestring[0] != '\0') {
            std::lock_guard<std::mutex> lock(resume_mutex_);
            resume_handle_ = handle->valuestring;
        }
    } else if (cJSON_GetObjectItem(root, "goAway") != nullptr) {
        // The session is about to hit its time limit; Reconnect() resumes it when it closes.
        ESP_LOGW(TAG, "Session ending soon (goAway)");
    } else if (cJSON_GetArraySize(root) > 0 && cJSON_GetObjectItem(root, "toolCallCancellation") == nullptr &&
               cJSON_GetObjectItem(root, "usageMetadata") == nullptr &&
               cJSON_GetObjectItem(root, "voiceActivity") == nullptr &&
               cJSON_GetObjectItem(root, "sessionResumptionUpdate") == nullptr) {
        // data is not NUL-terminated.
        ESP_LOGI(TAG, "Unhandled message: %.*s", (int)std::min<size_t>(len, 200), data);
    }
    cJSON_Delete(root);

    const TickType_t now = xTaskGetTickCount();
    if (now - last_yield_tick_ >= kYieldInterval) {
        vTaskDelay(1);
        last_yield_tick_ = xTaskGetTickCount();
    }
}

void GeminiLiveProtocol::HandleServerContent(const cJSON* content)
{
    if (const cJSON* input = cJSON_GetObjectItem(content, "inputTranscription"); cJSON_IsObject(input)) {
        const cJSON* text = cJSON_GetObjectItem(input, "text");
        if (cJSON_IsString(text) && text->valuestring[0] != '\0') {
            user_text_ += text->valuestring;
            EmitStt(user_text_);
            MarkActivity();
        }
    }

    if (cJSON_IsTrue(cJSON_GetObjectItem(content, "interrupted"))) {
        // The user talked over the answer (needs device AEC to happen).
        ESP_LOGI(TAG, "Interrupted by user");
        if (in_model_turn_) {
            Application::GetInstance().GetAudioService().ResetDecoder();
            EndModelTurn();
        }
        drop_model_turn_ = false;
    }

    if (const cJSON* turn = cJSON_GetObjectItem(content, "modelTurn"); cJSON_IsObject(turn)) {
        const cJSON* part = nullptr;
        cJSON_ArrayForEach (part, cJSON_GetObjectItem(turn, "parts")) {
            if (const cJSON* inline_data = cJSON_GetObjectItem(part, "inlineData"); cJSON_IsObject(inline_data)) {
                HandleModelAudio(inline_data);
            }
        }
    }

    if (const cJSON* output = cJSON_GetObjectItem(content, "outputTranscription"); cJSON_IsObject(output)) {
        const cJSON* text = cJSON_GetObjectItem(output, "text");
        if (cJSON_IsString(text) && text->valuestring[0] != '\0') {
            // Text can arrive before the first audio chunk; keep it for the turn.
            model_text_ += text->valuestring;
            if (in_model_turn_) {
                EmitTts("sentence_start", model_text_);
            }
        }
    }

    if (cJSON_IsTrue(cJSON_GetObjectItem(content, "turnComplete"))) {
        if (!drop_model_turn_) {
            FlushPcm();
        }
        EndModelTurn();
        drop_model_turn_ = false;
        user_text_.clear();
    }
}

void GeminiLiveProtocol::HandleModelAudio(const cJSON* inline_data)
{
    if (drop_model_turn_) {
        return;
    }
    const cJSON* data      = cJSON_GetObjectItem(inline_data, "data");
    const cJSON* mime_type = cJSON_GetObjectItem(inline_data, "mimeType");
    if (!cJSON_IsString(data)) {
        return;
    }

    std::vector<uint8_t> pcm;
    if (!Base64Decode(data->valuestring, strlen(data->valuestring), pcm) || pcm.empty()) {
        ESP_LOGW(TAG, "Bad audio data");
        return;
    }
    const int rate = ParsePcmRate(cJSON_IsString(mime_type) ? mime_type->valuestring : nullptr);
    stat_down_audio_bytes_ += pcm.size();

    if (!in_model_turn_) {
        BeginModelTurn();
    }
    MarkActivity();

    bool flush = false;
    {
        std::lock_guard<std::mutex> lock(pcm_mutex_);
        if (pending_pcm_rate_ != 0 && pending_pcm_rate_ != rate && !pending_pcm_.empty()) {
            flush = true;  // handled below; rate changes are rare
        }
    }
    if (flush) {
        FlushPcm();
    }

    // On a slow Wi-Fi link the answer can arrive slower than it plays. When playback runs
    // dry, buffer again before resuming: a few short pauses sound better than constant
    // stutter.
    const bool playback_dry = Application::GetInstance().GetAudioService().IsPlaybackIdle();

    std::unique_lock<std::mutex> lock(pcm_mutex_);
    if (playback_started_ && playback_dry) {
        ESP_LOGW(TAG, "Playback ran dry, buffering");
        playback_started_ = false;
    }
    pending_pcm_rate_ = rate;
    pending_pcm_.insert(pending_pcm_.end(), pcm.begin(), pcm.end());
    const size_t bytes_per_ms = static_cast<size_t>(rate) * sizeof(int16_t) / 1000;
    const size_t threshold    = bytes_per_ms * (playback_started_ ? kPcmPacketMs : kPcmPrebufferMs);
    if (pending_pcm_.size() < threshold) {
        return;
    }
    playback_started_ = true;
    // Push outside the lock: the push waits while the decode queue is full.
    auto packet         = std::make_unique<AudioStreamPacket>();
    packet->is_pcm      = true;
    packet->sample_rate = rate;
    packet->payload     = std::move(pending_pcm_);
    pending_pcm_        = {};
    lock.unlock();
    if (on_incoming_audio_ != nullptr) {
        on_incoming_audio_(std::move(packet));
    }
}

void GeminiLiveProtocol::FlushPcm()
{
    std::unique_ptr<AudioStreamPacket> packet;
    {
        std::lock_guard<std::mutex> lock(pcm_mutex_);
        if (pending_pcm_.empty()) {
            return;
        }
        packet              = std::make_unique<AudioStreamPacket>();
        packet->is_pcm      = true;
        packet->sample_rate = pending_pcm_rate_;
        packet->payload     = std::move(pending_pcm_);
        pending_pcm_        = {};
        playback_started_   = true;
    }
    if (on_incoming_audio_ != nullptr) {
        on_incoming_audio_(std::move(packet));
    }
}

void GeminiLiveProtocol::ClearPcm()
{
    std::lock_guard<std::mutex> lock(pcm_mutex_);
    pending_pcm_.clear();
    pending_pcm_.shrink_to_fit();
    playback_started_ = false;
}

void GeminiLiveProtocol::BeginModelTurn()
{
    ClearPcm();
    in_model_turn_ = true;
    // Gemini is answering, so mic audio still waiting for upload is stale; dropping it
    // leaves the link to the answer.
    DropQueuedAudio();

    // Entering the speaking state resets the decoder on the main task. Wait for that
    // before streaming, otherwise the reset discards the start of the answer.
    auto& app            = Application::GetInstance();
    const uint32_t epoch = app.GetSpeakingEpoch();
    EmitTts("start");
    for (int waited = 0; waited < kSpeakingWaitMs && app.GetSpeakingEpoch() == epoch; waited += 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!model_text_.empty()) {
        EmitTts("sentence_start", model_text_);
    }
}

void GeminiLiveProtocol::EndModelTurn()
{
    // On a normal end, HandleServerContent flushes the held audio first.
    ClearPcm();
    if (in_model_turn_.exchange(false)) {
        EmitTts("stop");
    }
    model_text_.clear();
    MarkActivity();
}

void GeminiLiveProtocol::HandleToolCall(const cJSON* tool_call)
{
    MarkActivity();
    const cJSON* call = nullptr;
    cJSON_ArrayForEach (call, cJSON_GetObjectItem(tool_call, "functionCalls")) {
        const cJSON* id   = cJSON_GetObjectItem(call, "id");
        const cJSON* name = cJSON_GetObjectItem(call, "name");
        const cJSON* args = cJSON_GetObjectItem(call, "args");
        if (!cJSON_IsString(name)) {
            continue;
        }
        const std::string call_id   = cJSON_IsString(id) ? id->valuestring : "";
        const std::string tool_name = name->valuestring;
        ESP_LOGI(TAG, "Tool call: %s", tool_name.c_str());

        if (tool_name == kTakePhotoTool) {
            // xiaozhi's version uploads the photo to the xiaozhi vision server; show it to
            // Gemini instead.
            const cJSON* question = cJSON_GetObjectItem(args, "question");
            TakePhoto(call_id, tool_name, cJSON_IsString(question) ? question->valuestring : "");
            continue;
        }

        cJSON* request = cJSON_CreateObject();
        cJSON_AddStringToObject(request, "jsonrpc", "2.0");
        cJSON_AddNumberToObject(request, "id", next_mcp_id_++);
        cJSON_AddStringToObject(request, "method", "tools/call");
        cJSON* params = cJSON_AddObjectToObject(request, "params");
        cJSON_AddStringToObject(params, "name", tool_name.c_str());
        cJSON_AddItemToObject(params, "arguments",
                              cJSON_IsObject(args) ? cJSON_Duplicate(args, true) : cJSON_CreateObject());
        // The reply may arrive later from the main task, after the tool has run.
        McpServer::GetInstance().ParseMessage(request, [this, call_id, tool_name](const std::string& reply) {
            SendToolResponse(call_id, tool_name, reply);
        });
        cJSON_Delete(request);
    }
}

void GeminiLiveProtocol::SendToolResponse(const std::string& call_id, const std::string& name,
                                          const std::string& mcp_reply)
{
    cJSON* reply_json = cJSON_Parse(mcp_reply.c_str());
    std::string text;
    bool is_error = false;
    if (const cJSON* error = cJSON_GetObjectItem(reply_json, "error"); cJSON_IsObject(error)) {
        const cJSON* message = cJSON_GetObjectItem(error, "message");
        text                 = cJSON_IsString(message) ? message->valuestring : "error";
        is_error             = true;
    } else {
        const cJSON* result = cJSON_GetObjectItem(reply_json, "result");
        is_error            = cJSON_IsTrue(cJSON_GetObjectItem(result, "isError"));
        const cJSON* item   = nullptr;
        cJSON_ArrayForEach (item, cJSON_GetObjectItem(result, "content")) {
            const cJSON* item_text = cJSON_GetObjectItem(item, "text");
            if (cJSON_IsString(item_text)) {
                text += item_text->valuestring;
            }
        }
    }
    cJSON_Delete(reply_json);
    SendFunctionResult(call_id, name, text, is_error);
}

void GeminiLiveProtocol::SendFunctionResult(const std::string& call_id, const std::string& name,
                                            const std::string& text, bool is_error)
{
    cJSON* root      = cJSON_CreateObject();
    cJSON* response  = cJSON_AddObjectToObject(root, "toolResponse");
    cJSON* responses = cJSON_AddArrayToObject(response, "functionResponses");
    cJSON* entry     = cJSON_CreateObject();
    if (!call_id.empty()) {
        cJSON_AddStringToObject(entry, "id", call_id.c_str());
    }
    cJSON_AddStringToObject(entry, "name", name.c_str());
    cJSON* body = cJSON_AddObjectToObject(entry, "response");
    cJSON_AddStringToObject(body, is_error ? "error" : "result", text.empty() ? "ok" : text.c_str());
    cJSON_AddItemToArray(responses, entry);
    SendText(PrintJson(root));
    cJSON_Delete(root);
}

void GeminiLiveProtocol::TakePhoto(const std::string& call_id, const std::string& name,
                                   const std::string& question)
{
    // Like other tools, run on the main task: capture blocks, and the camera plays its
    // shutter sound and preview.
    Application::GetInstance().Schedule([this, call_id, name, question]() {
        auto* camera = static_cast<StackChanCamera*>(Board::GetInstance().GetCamera());
        if (camera == nullptr || !camera->Capture()) {
            SendFunctionResult(call_id, name, "Failed to take a photo", true);
            return;
        }

        uint8_t* jpeg   = nullptr;
        size_t jpeg_len = 0;
        {
            auto lock = camera->TryLockFrame();
            if (!lock.owns_lock() || camera->GetFrameData() == nullptr) {
                SendFunctionResult(call_id, name, "The camera is busy", true);
                return;
            }
            image_to_jpeg(const_cast<uint8_t*>(camera->GetFrameData()), camera->GetFrameSize(),
                          camera->GetFrameWidth(), camera->GetFrameHeight(),
                          static_cast<v4l2_pix_fmt_t>(camera->GetFrameFormat()), kPhotoJpegQuality, &jpeg,
                          &jpeg_len);
        }
        if (jpeg == nullptr || jpeg_len == 0) {
            free(jpeg);
            SendFunctionResult(call_id, name, "Failed to encode the photo", true);
            return;
        }

        std::string message = "{\"realtimeInput\":{\"video\":{\"mimeType\":\"image/jpeg\",\"data\":\"";
        message += Base64Encode(jpeg, jpeg_len);
        message += "\"}}}";
        free(jpeg);
        ESP_LOGI(TAG, "Sending photo to Gemini (%u bytes JPEG)", (unsigned)jpeg_len);
        if (!SendText(message)) {
            SendFunctionResult(call_id, name, "Failed to send the photo", true);
            return;
        }
        std::string result = "The photo was taken and sent to you as the latest video frame. Look at it";
        result += question.empty() ? " and describe what you see." : " and answer: " + question;
        SendFunctionResult(call_id, name, result, false);
        MarkActivity();
    });
}

void GeminiLiveProtocol::EmitJson(cJSON* root)
{
    if (on_incoming_json_ != nullptr) {
        on_incoming_json_(root);
    }
    cJSON_Delete(root);
}

void GeminiLiveProtocol::EmitTts(const char* state, const std::string& text)
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "tts");
    cJSON_AddStringToObject(root, "state", state);
    if (!text.empty()) {
        cJSON_AddStringToObject(root, "text", text.c_str());
    }
    EmitJson(root);
}

void GeminiLiveProtocol::EmitStt(const std::string& text)
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "stt");
    cJSON_AddStringToObject(root, "text", text.c_str());
    EmitJson(root);
}

void GeminiLiveProtocol::MarkActivity()
{
    last_activity_us_ = esp_timer_get_time();
}

void GeminiLiveProtocol::CheckIdle()
{
    if (!channel_open_ || in_model_turn_) {
        return;
    }
    auto& app = Application::GetInstance();
    if (app.GetDeviceState() != kDeviceStateListening) {
        MarkActivity();
        return;
    }
    if (esp_timer_get_time() - last_activity_us_ > kIdleTimeoutUs) {
        ESP_LOGI(TAG, "No conversation for %lld s, closing session", kIdleTimeoutUs / 1000000);
        MarkActivity();
        app.Schedule([this]() {
            if (channel_open_) {
                CloseAudioChannel();
            }
        });
    }
}
