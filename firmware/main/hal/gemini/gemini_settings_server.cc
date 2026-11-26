/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
// Small LAN web page to set the Gemini API key, model, voice and system prompt.
#include "gemini_live.h"
#include <application.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <functional>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#define TAG "GeminiSettings"

namespace {

constexpr size_t kMaxBodySize   = 8192;
constexpr size_t kMaxPromptSize = 3500;  // NVS strings are limited to 4000 bytes
httpd_handle_t server_          = nullptr;

const char* kVoices[] = {"", "Puck", "Charon", "Kore", "Fenrir", "Aoede", "Leda", "Orus", "Zephyr"};

// The server task's stack is in PSRAM to spare internal RAM, and flash (NVS) access is not
// allowed from such a task, so settings are read and written on the main task.
bool RunOnMainTask(std::function<void()> fn)
{
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (done == nullptr) {
        return false;
    }
    Application::GetInstance().Schedule([&fn, done]() {
        fn();
        xSemaphoreGive(done);
    });
    // The callback references this frame, so wait for it to finish.
    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
    return true;
}

gemini_live::Config LoadConfig()
{
    gemini_live::Config config;
    RunOnMainTask([&config]() { config = gemini_live::load_config(); });
    return config;
}

std::string HtmlEscape(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string UrlDecode(const std::string& in)
{
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '+') {
            out += ' ';
        } else if (in[i] == '%' && i + 2 < in.size()) {
            out += static_cast<char>(strtol(in.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else {
            out += in[i];
        }
    }
    return out;
}

std::map<std::string, std::string> ParseForm(const std::string& body)
{
    std::map<std::string, std::string> fields;
    size_t start = 0;
    while (start <= body.size()) {
        size_t end = body.find('&', start);
        if (end == std::string::npos) {
            end = body.size();
        }
        const std::string pair = body.substr(start, end - start);
        const size_t eq        = pair.find('=');
        if (eq != std::string::npos) {
            fields[UrlDecode(pair.substr(0, eq))] = UrlDecode(pair.substr(eq + 1));
        }
        start = end + 1;
    }
    return fields;
}

std::string RenderPage(const gemini_live::Config& config, const char* notice)
{
    std::string html =
        "<!doctype html><html><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Stack-chan Gemini</title><style>"
        "body{font-family:system-ui,sans-serif;max-width:560px;margin:0 auto;padding:16px;"
        "background:#edf4ff;color:#26206a}"
        "h1{font-size:1.4em}label{display:block;margin-top:14px;font-weight:600}"
        "input,select,textarea{width:100%;box-sizing:border-box;padding:8px;margin-top:4px;"
        "border:1px solid #b8d3fd;border-radius:8px;font:inherit;background:#fff}"
        "textarea{min-height:180px}small{color:#615b9e}"
        "button{margin-top:18px;padding:10px 18px;border:0;border-radius:8px;background:#615b9e;"
        "color:#fff;font:inherit;font-weight:600}"
        ".notice{padding:10px;border-radius:8px;background:#d2f5e3;margin:12px 0}"
        "</style></head><body><h1>Stack-chan &middot; Gemini Live</h1>";
    if (notice != nullptr) {
        html += "<div class=notice>";
        html += notice;
        html += "</div>";
    }
    html += "<form method=post action=/save>";
    html += "<label>Gemini API key</label><input name=api_key type=password autocomplete=off placeholder='";
    html += config.api_key.empty() ? "Paste your key from aistudio.google.com"
                                   : "Saved (leave blank to keep it)";
    html += "'><small>Get a key at aistudio.google.com &rarr; API keys.</small>";

    html += "<label>Model</label><input name=model value='" + HtmlEscape(config.model) + "'>";

    html += "<label>Voice</label><select name=voice>";
    for (const char* voice : kVoices) {
        html += "<option value='";
        html += voice;
        html += "'";
        if (config.voice == voice) {
            html += " selected";
        }
        html += ">";
        html += voice[0] == '\0' ? "Default" : voice;
        html += "</option>";
    }
    html += "</select>";

    html += "<label>System prompt</label><textarea name=prompt maxlength=3500>" + HtmlEscape(config.system_prompt) +
            "</textarea>";
    html += "<button type=submit>Save</button></form>"
            "<p><small>Changes apply from the next conversation.</small></p></body></html>";
    return html;
}

esp_err_t SendPage(httpd_req_t* req, const char* notice)
{
    const std::string html = RenderPage(LoadConfig(), notice);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, html.data(), html.size());
}

esp_err_t HandleIndex(httpd_req_t* req)
{
    char query[32] = {};
    const bool saved = httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK && strstr(query, "saved=1");
    return SendPage(req, saved ? "Saved. Changes apply from the next conversation." : nullptr);
}

// A refresh after saving would otherwise re-request /save with GET.
esp_err_t HandleSaveGet(httpd_req_t* req)
{
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, nullptr, 0);
}

esp_err_t HandleSave(httpd_req_t* req)
{
    if (req->content_len == 0 || req->content_len > kMaxBodySize) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form size");
        return ESP_FAIL;
    }
    std::string body(req->content_len, '\0');
    size_t received = 0;
    while (received < body.size()) {
        const int ret = httpd_req_recv(req, body.data() + received, body.size() - received);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (ret <= 0) {
            return ESP_FAIL;
        }
        received += ret;
    }

    auto fields   = ParseForm(body);
    auto config   = LoadConfig();
    const auto key = fields["api_key"];
    if (!key.empty()) {
        config.api_key = key;  // blank keeps the saved key
    }
    config.model         = gemini_live::normalize_model(fields["model"]);
    config.voice         = fields["voice"];
    config.system_prompt = fields["prompt"].substr(0, kMaxPromptSize);
    std::string error = "Could not save settings";
    RunOnMainTask([&config, &error]() { error = gemini_live::save_config(config); });
    if (!error.empty()) {
        return SendPage(req, error.c_str());
    }
    ESP_LOGI(TAG, "Settings saved (model %s, voice %s, prompt %u bytes)", config.model.c_str(),
             config.voice.empty() ? "default" : config.voice.c_str(), (unsigned)config.system_prompt.size());

    // Clear a "key not set" alert on the screen.
    if (!config.api_key.empty()) {
        Application::GetInstance().Schedule([]() { Application::GetInstance().DismissAlert(); });
    }
    // Redirect so refreshing the page does not resubmit the form.
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/?saved=1");
    return httpd_resp_send(req, nullptr, 0);
}

}  // namespace

namespace gemini_live {

bool start_settings_server()
{
    if (server_ != nullptr) {
        return true;
    }
    httpd_config_t config   = HTTPD_DEFAULT_CONFIG();
    config.stack_size       = 6144;
    config.task_caps        = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    config.lru_purge_enable = true;
    if (httpd_start(&server_, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start settings server");
        server_ = nullptr;
        return false;
    }
    httpd_uri_t index = {.uri = "/", .method = HTTP_GET, .handler = HandleIndex, .user_ctx = nullptr};
    httpd_uri_t save  = {.uri = "/save", .method = HTTP_POST, .handler = HandleSave, .user_ctx = nullptr};
    httpd_uri_t save_get = {.uri = "/save", .method = HTTP_GET, .handler = HandleSaveGet, .user_ctx = nullptr};
    httpd_register_uri_handler(server_, &index);
    httpd_register_uri_handler(server_, &save);
    httpd_register_uri_handler(server_, &save_get);
    return true;
}

std::string settings_url()
{
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    if (netif == nullptr || esp_netif_get_ip_info(netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        return "";
    }
    char url[32];
    snprintf(url, sizeof(url), "http://" IPSTR "/", IP2STR(&ip_info.ip));
    return url;
}

}  // namespace gemini_live
