#include "Config.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <syncstream>
#include <Windows.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Filters.h>
#include <winrt/Windows.Web.Http.Headers.h>

namespace ChatBuddy
{
    namespace
    {
        int ReadInteger(std::string_view name, int fallback)
        {
            const auto text = Detail::ReadEnvironment(name);
            if (text.empty()) return fallback;
            int result = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
                throw std::runtime_error("Invalid integer environment variable: " + std::string(name));
            return result;
        }

        bool ReadBoolean(std::string_view name, bool fallback)
        {
            const auto value = Detail::ReadEnvironment(name);
            if (value.empty()) return fallback;
            if (value == "1" || value == "true") return true;
            if (value == "0" || value == "false") return false;
            throw std::runtime_error("Expected true/false or 1/0: " + std::string(name));
        }

        void CheckRange(int value, int minimum, int maximum, std::string_view name)
        {
            if (value < minimum || value > maximum)
                throw std::runtime_error("Out-of-range environment variable: " + std::string(name));
        }

    }

    namespace Detail
    {
        std::string ReadEnvironment(std::string_view name, std::string_view fallback)
        {
            const auto wideName = winrt::to_hstring(name);
            const DWORD required = GetEnvironmentVariableW(wideName.c_str(), nullptr, 0);
            if (required == 0) return std::string(fallback);
            std::wstring value(required, L'\0');
            const DWORD length = GetEnvironmentVariableW(wideName.c_str(), value.data(), required);
            if (length == 0 || length >= required) throw std::runtime_error("Environment changed while loading configuration.");
            value.resize(length);
            return winrt::to_string(value);
        }

        std::string String(const JsonObject& object, std::wstring_view key, std::string_view fallback)
        {
            const auto value = object.TryLookup(key);
            if (!value || value.ValueType() != winrt::Windows::Data::Json::JsonValueType::String) return std::string(fallback);
            return winrt::to_string(value.GetString());
        }

        std::int64_t Integer(const JsonObject& object, std::wstring_view key, std::int64_t fallback)
        {
            const auto value = object.TryLookup(key);
            if (!value) return fallback;
            if (value.ValueType() == winrt::Windows::Data::Json::JsonValueType::Number)
            {
                const double number = value.GetNumber();
                if (std::isfinite(number) && std::trunc(number) == number && number > -9e15 && number < 9e15)
                    return static_cast<std::int64_t>(number);
            }
            if (value.ValueType() == winrt::Windows::Data::Json::JsonValueType::String)
            {
                const auto text = winrt::to_string(value.GetString());
                std::int64_t result = 0;
                const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
                if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()) return result;
            }
            return fallback;
        }

        bool Boolean(const JsonObject& object, std::wstring_view key, bool fallback)
        {
            const auto value = object.TryLookup(key);
            return value && value.ValueType() == winrt::Windows::Data::Json::JsonValueType::Boolean ? value.GetBoolean() : fallback;
        }

        JsonObject Object(const JsonObject& object, std::wstring_view key)
        {
            const auto value = object.TryLookup(key);
            return value && value.ValueType() == winrt::Windows::Data::Json::JsonValueType::Object ? value.GetObject() : JsonObject{};
        }

        JsonArray Array(const JsonObject& object, std::wstring_view key)
        {
            const auto value = object.TryLookup(key);
            return value && value.ValueType() == winrt::Windows::Data::Json::JsonValueType::Array ? value.GetArray() : JsonArray{};
        }

        void Put(JsonObject& object, std::wstring_view key, std::string_view value)
        {
            object.Insert(key, JsonValue::CreateStringValue(winrt::to_hstring(value)));
        }

        void Put(JsonObject& object, std::wstring_view key, std::int64_t value)
        {
            object.Insert(key, JsonValue::CreateNumberValue(static_cast<double>(value)));
        }

        void Put(JsonObject& object, std::wstring_view key, bool value)
        {
            object.Insert(key, JsonValue::CreateBooleanValue(value));
        }

        std::string Trim(std::string value)
        {
            const auto first = value.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) return {};
            const auto last = value.find_last_not_of(" \t\r\n");
            return value.substr(first, last - first + 1);
        }

        std::string Utf8Prefix(std::string_view text, std::size_t maxBytes)
        {
            std::size_t length = std::min(text.size(), maxBytes);
            if (length < text.size())
                while (length > 0 && (static_cast<unsigned char>(text[length]) & 0xc0) == 0x80) --length;
            return std::string(text.substr(0, length));
        }

        bool IsIdentifier(std::string_view value)
        {
            return !value.empty() && value.size() <= 256 && std::all_of(value.begin(), value.end(), [](unsigned char character)
            {
                return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'z') ||
                    (character >= 'A' && character <= 'Z') || character == '_' || character == '-';
            });
        }

        void ValidateHttpsUrl(std::string_view url)
        {
            if (url.find_first_of("\r\n\t ") != std::string_view::npos) throw std::runtime_error("Invalid HTTPS endpoint.");
            try
            {
                const winrt::Windows::Foundation::Uri uri(winrt::to_hstring(url));
                if (uri.SchemeName() != L"https" || uri.Host().empty() || !uri.UserName().empty() ||
                    !uri.Password().empty() || !uri.Fragment().empty() || !uri.Query().empty())
                    throw std::runtime_error("Endpoints must be HTTPS without user information, query or fragment.");
            }
            catch (const winrt::hresult_error&) { throw std::runtime_error("Invalid HTTPS endpoint."); }
        }

        bool Wait(std::stop_token stop, std::chrono::milliseconds duration)
        {
            std::mutex mutex;
            std::condition_variable_any changed;
            std::unique_lock lock(mutex);
            changed.wait_for(lock, stop, duration, [] { return false; });
            return !stop.stop_requested();
        }

        void Log(std::string_view message)
        {
            std::osyncstream(std::cerr) << "[ChatBuddy] " << message << '\n';
        }

        HttpResult RequestJson(std::string_view method, std::string_view url, std::string_view authorization, const JsonObject& body, int timeoutSeconds, std::stop_token stop)
        {
            using namespace winrt::Windows;
            ValidateHttpsUrl(url);
            if (authorization.find_first_of("\r\n") != std::string_view::npos) throw std::runtime_error("Invalid authorization value.");
            if (stop.stop_requested()) throw std::runtime_error("Request cancelled.");
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
            Web::Http::Filters::HttpBaseProtocolFilter filter;
            filter.AllowAutoRedirect(false);
            filter.AllowUI(false);
            filter.AutomaticDecompression(true);
            Web::Http::HttpClient client(filter);
            Web::Http::HttpRequestMessage request(Web::Http::HttpMethod(winrt::to_hstring(method)), Foundation::Uri(winrt::to_hstring(url)));
            request.Headers().TryAppendWithoutValidation(L"User-Agent", L"ChatBuddy/1.0");
            request.Headers().TryAppendWithoutValidation(L"Accept", L"application/json");
            if (!authorization.empty()) request.Headers().TryAppendWithoutValidation(L"Authorization", winrt::to_hstring(authorization));
            if (method != "GET") request.Content(Web::Http::HttpStringContent(body.Stringify(), Storage::Streams::UnicodeEncoding::Utf8, L"application/json"));
            try
            {
                auto response = Await(client.SendRequestAsync(request, Web::Http::HttpCompletionOption::ResponseHeadersRead), deadline, stop);
                const unsigned status = static_cast<unsigned>(response.StatusCode());
                unsigned retryAfter = 0;
                if (response.Headers().HasKey(L"Retry-After"))
                {
                    const auto text = winrt::to_string(response.Headers().Lookup(L"Retry-After"));
                    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), retryAfter);
                    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) retryAfter = 60;
                }
                auto stream = Await(response.Content().ReadAsInputStreamAsync(), deadline, stop);
                std::string text;
                Storage::Streams::Buffer buffer(8192);
                for (;;)
                {
                    const auto part = Await(stream.ReadAsync(buffer, buffer.Capacity(), Storage::Streams::InputStreamOptions::Partial), deadline, stop);
                    if (part.Length() == 0) break;
                    if (text.size() + part.Length() > 4 * 1024 * 1024) throw std::runtime_error("HTTP response exceeds 4 MiB.");
                    text.append(reinterpret_cast<const char*>(part.data()), part.Length());
                }
                stream.Close();
                JsonObject result;
                if (!text.empty() && !JsonObject::TryParse(winrt::to_hstring(text), result) && status >= 200 && status < 300)
                    throw std::runtime_error("API returned invalid JSON.");
                return { status, result, retryAfter };
            }
            catch (const winrt::hresult_error& error)
            {
                // Never log native exception text: servers can echo credentials or user content.
                throw std::runtime_error("HTTP transport failure, HRESULT " + std::to_string(error.code().value));
            }
        }
    }

    Config Config::LoadFromEnvironment(bool requireCredentials)
    {
        Config config;
        config.qq_.appId_ = Detail::ReadEnvironment("QQ_APP_ID");
        config.qq_.appSecret_ = Detail::ReadEnvironment("QQ_APP_SECRET");
        config.qq_.replyToAllGroupMessages_ = ReadBoolean("CHATBUDDY_QQ_REPLY_ALL", false);
        config.model_.provider_ = Detail::ReadEnvironment("CHATBUDDY_PROVIDER", "openai");
        const bool deepSeek = config.model_.provider_ == "deepseek";
        const bool compatible = config.model_.provider_ == "compatible";
        config.model_.apiFormat_ = Detail::ReadEnvironment("CHATBUDDY_API_FORMAT", deepSeek || compatible ? "chat_completions" : "responses");
        config.model_.baseUrl_ = Detail::ReadEnvironment("CHATBUDDY_BASE_URL", deepSeek ? "https://api.deepseek.com" : compatible ? "" : "https://api.openai.com/v1");
        while (config.model_.baseUrl_.ends_with('/')) config.model_.baseUrl_.pop_back();
        config.model_.apiKeyEnvironment_ = Detail::ReadEnvironment("CHATBUDDY_API_KEY_ENV", deepSeek ? "DEEPSEEK_API_KEY" : compatible ? "MODEL_API_KEY" : "OPENAI_API_KEY");
        config.model_.apiKey_ = Detail::ReadEnvironment(config.model_.apiKeyEnvironment_);
        config.model_.model_ = Detail::ReadEnvironment("CHATBUDDY_MODEL");
        config.model_.systemPrompt_ = Detail::ReadEnvironment("CHATBUDDY_SYSTEM_PROMPT", config.model_.systemPrompt_);
        config.model_.timeoutSeconds_ = ReadInteger("CHATBUDDY_MODEL_TIMEOUT", 90);
        config.model_.maxOutputTokens_ = ReadInteger("CHATBUDDY_MAX_OUTPUT_TOKENS", 2048);
        config.workerCount_ = ReadInteger("CHATBUDDY_WORKERS", 4);
        config.maxPendingMessages_ = ReadInteger("CHATBUDDY_MAX_PENDING", 128);
        config.maxSessions_ = ReadInteger("CHATBUDDY_MAX_SESSIONS", 256);
        config.maxHistoryTurns_ = ReadInteger("CHATBUDDY_HISTORY_TURNS", 12);
        config.maxContextBytes_ = ReadInteger("CHATBUDDY_CONTEXT_BYTES", 32768);
        config.maxInputBytes_ = ReadInteger("CHATBUDDY_INPUT_BYTES", 8192);
        config.sessionTimeoutSeconds_ = ReadInteger("CHATBUDDY_SESSION_TIMEOUT", 1800);
        config.Validate(requireCredentials);
        return config;
    }

    void Config::Validate(bool requireCredentials) const
    {
        if (model_.provider_ != "openai" && model_.provider_ != "deepseek" && model_.provider_ != "compatible")
            throw std::runtime_error("CHATBUDDY_PROVIDER must be openai, deepseek or compatible.");
        if (model_.apiFormat_ != "responses" && model_.apiFormat_ != "chat_completions")
            throw std::runtime_error("CHATBUDDY_API_FORMAT must be responses or chat_completions.");
        Detail::ValidateHttpsUrl(model_.baseUrl_);
        Detail::ValidateHttpsUrl(qq_.apiBaseUrl_);
        if (!Detail::IsIdentifier(model_.apiKeyEnvironment_)) throw std::runtime_error("Invalid CHATBUDDY_API_KEY_ENV.");
        if (model_.model_.empty() && requireCredentials) throw std::runtime_error("Missing CHATBUDDY_MODEL; choose an API model available to your account.");
        if (model_.model_.size() > 256) throw std::runtime_error("CHATBUDDY_MODEL is too long.");
        if (requireCredentials && !Detail::IsIdentifier(qq_.appId_)) throw std::runtime_error("Missing or invalid QQ_APP_ID.");
        if (requireCredentials && qq_.appSecret_.empty()) throw std::runtime_error("Missing QQ_APP_SECRET.");
        if (requireCredentials && model_.apiKey_.empty()) throw std::runtime_error("Missing model API key environment variable: " + model_.apiKeyEnvironment_);
        if (model_.apiKey_.find_first_of("\r\n") != std::string::npos) throw std::runtime_error("Model API key contains a line break.");
        CheckRange(model_.timeoutSeconds_, 5, 120, "CHATBUDDY_MODEL_TIMEOUT");
        CheckRange(model_.maxOutputTokens_, 128, 32768, "CHATBUDDY_MAX_OUTPUT_TOKENS");
        CheckRange(qq_.timeoutSeconds_, 1, 30, "QQ timeout");
        CheckRange(workerCount_, 1, 16, "CHATBUDDY_WORKERS");
        CheckRange(maxPendingMessages_, 1, 4096, "CHATBUDDY_MAX_PENDING");
        CheckRange(maxSessions_, workerCount_, 4096, "CHATBUDDY_MAX_SESSIONS");
        CheckRange(maxHistoryTurns_, 0, 100, "CHATBUDDY_HISTORY_TURNS");
        CheckRange(maxInputBytes_, 256, 65536, "CHATBUDDY_INPUT_BYTES");
        CheckRange(maxContextBytes_, maxInputBytes_ + 1024, 1048576, "CHATBUDDY_CONTEXT_BYTES");
        CheckRange(sessionTimeoutSeconds_, 60, 86400, "CHATBUDDY_SESSION_TIMEOUT");
        if (model_.systemPrompt_.size() > static_cast<std::size_t>(maxContextBytes_ / 2))
            throw std::runtime_error("System prompt exceeds half the context byte budget.");
    }

    std::string Config::Help()
    {
        return
            "ChatBuddy - QQ official bot, C++20 / Visual Studio\n"
            "Usage: ChatBuddy.exe [--help | --check-config"
#ifdef _DEBUG
            " | --self-test"
#endif
            "]\n\n"
            "Required environment variables (restart VS after changing inherited variables):\n"
            "  QQ_APP_ID, QQ_APP_SECRET       QQ official bot credentials\n"
            "  CHATBUDDY_MODEL               API model name (no assumed default)\n"
            "  OPENAI_API_KEY                For provider=openai\n"
            "  DEEPSEEK_API_KEY              For provider=deepseek\n\n"
            "Optional:\n"
            "  CHATBUDDY_PROVIDER            openai (default), deepseek, compatible\n"
            "  CHATBUDDY_API_FORMAT          responses or chat_completions\n"
            "  CHATBUDDY_BASE_URL            HTTPS API base, required for compatible\n"
            "  CHATBUDDY_API_KEY_ENV         Name of the key environment variable\n"
            "  CHATBUDDY_SYSTEM_PROMPT       Assistant instructions\n"
            "  CHATBUDDY_QQ_REPLY_ALL        false (default) / true; QQ must enable full events\n"
            "  CHATBUDDY_MODEL_TIMEOUT       Total request timeout seconds, default 90, max 120\n"
            "  CHATBUDDY_MAX_OUTPUT_TOKENS   Default 2048\n"
            "  CHATBUDDY_WORKERS             Default 4; each conversation is serialized\n"
            "  CHATBUDDY_MAX_PENDING         Default 128\n"
            "  CHATBUDDY_MAX_SESSIONS        Default 256\n"
            "  CHATBUDDY_HISTORY_TURNS       Default 12, 0 disables retained history\n"
            "  CHATBUDDY_CONTEXT_BYTES       Default 32768 (UTF-8 bytes, not tokens)\n"
            "  CHATBUDDY_INPUT_BYTES         Default 8192\n"
            "  CHATBUDDY_SESSION_TIMEOUT     Inactivity seconds, default 1800\n\n"
            "QQ setup: enable WebSocket and GROUP_AND_C2C_EVENT events; grant group/private scope.\n"
            "Private chats reply directly. Groups default to @ mentions only.\n"
            "Group histories are isolated by group AND sender; private histories are separate.\n"
            "Text messages are supported; media-only messages receive a text-input notice.\n"
            "Chat commands: /help, /reset (only resets the current conversation).\n"
            "History/deduplication are bounded and in memory; restart clears them.\n"
            "Replies use QQ passive-message windows; expired messages are not sent proactively.\n"
            "Long replies are split into at most 4 private / 5 group messages, then truncated.\n"
            "--check-config validates locally without contacting any service. Ctrl+C stops.\n";
    }
}
