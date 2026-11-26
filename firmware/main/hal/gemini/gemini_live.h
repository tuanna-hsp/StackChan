/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <memory>
#include <string>

class Protocol;

// Google Gemini Live as the AI Agent backend, in place of the xiaozhi server.
namespace gemini_live {

struct Config {
    std::string api_key;
    std::string model;          // e.g. "models/gemini-3.8-live"
    std::string voice;          // prebuilt voice name, empty = Gemini default
    std::string system_prompt;  // system instruction
};

bool is_enabled();
std::unique_ptr<Protocol> create_protocol();

Config load_config();
// Returns an empty string on success, otherwise a message for the user.
std::string save_config(const Config& config);
// Normalizes a user-entered model name to the "models/..." form Gemini expects.
std::string normalize_model(const std::string& model);

// LAN web page for the API key, model, voice and prompt. Safe to call repeatedly.
bool start_settings_server();
// "http://<ip>/" of the settings page, or "" while the device has no IP.
std::string settings_url();

}  // namespace gemini_live
