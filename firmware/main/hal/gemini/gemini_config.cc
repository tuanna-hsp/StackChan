/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "gemini_live.h"
#include <settings.h>
#include <nvs.h>
#include <esp_err.h>
#include <esp_log.h>

#define TAG "GeminiConfig"

namespace gemini_live {

namespace {
constexpr const char* kNamespace     = "gemini";
constexpr const char* kKeyApiKey     = "api_key";
constexpr const char* kKeyModel      = "model";
constexpr const char* kKeyVoice      = "voice";
constexpr const char* kKeyPrompt     = "prompt";
constexpr const char* kDefaultModel  = "models/gemini-3.8-live";
constexpr const char* kDefaultPrompt =
    "You are Stack-chan, a small and friendly desktop robot. Keep your answers short and "
    "conversational, and reply in the language the user speaks.";

// Writes a string only when it changed, to save NVS space and wear.
esp_err_t SetIfChanged(nvs_handle_t handle, const char* key, const std::string& value)
{
    size_t length = 0;
    if (nvs_get_str(handle, key, nullptr, &length) == ESP_OK) {
        std::string current(length, '\0');
        if (nvs_get_str(handle, key, current.data(), &length) == ESP_OK) {
            current.resize(length > 0 ? length - 1 : 0);
            if (current == value) {
                return ESP_OK;
            }
        }
    }
    return nvs_set_str(handle, key, value.c_str());
}
}  // namespace

bool is_enabled()
{
    return true;
}

std::string normalize_model(const std::string& model)
{
    if (model.empty()) {
        return kDefaultModel;
    }
    if (model.rfind("models/", 0) == 0) {
        return model;
    }
    return "models/" + model;
}

Config load_config()
{
    Settings settings(kNamespace, false);
    Config config;
    config.api_key       = settings.GetString(kKeyApiKey);
    config.model         = normalize_model(settings.GetString(kKeyModel, kDefaultModel));
    config.voice         = settings.GetString(kKeyVoice);
    config.system_prompt = settings.GetString(kKeyPrompt, kDefaultPrompt);
    return config;
}

std::string save_config(const Config& config)
{
    // xiaozhi's Settings::SetString aborts on any NVS error (e.g. a full partition),
    // so write directly and report errors to the settings page instead.
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        for (const auto& [key, value] :
             {std::pair<const char*, std::string>{kKeyApiKey, config.api_key},
              {kKeyModel, normalize_model(config.model)},
              {kKeyVoice, config.voice},
              {kKeyPrompt, config.system_prompt}}) {
            err = SetIfChanged(handle, key, value);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save %s: %s", key, esp_err_to_name(err));
                break;
            }
        }
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
        return "Not enough storage space. Try a shorter system prompt.";
    }
    return err == ESP_OK ? std::string() : std::string("Could not save settings: ") + esp_err_to_name(err);
}

}  // namespace gemini_live
