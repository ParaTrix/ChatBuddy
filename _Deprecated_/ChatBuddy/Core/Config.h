#pragma once

#include <chrono>
#include <functional>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <Windows.h>
#ifdef GetObject
#undef GetObject
#endif
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Data.Json.h>

namespace ChatBuddy
{
    struct Config
    {
    public:
        struct QQConfig
        {
        public:
            std::string appId_;
            std::string appSecret_;
            std::string apiBaseUrl_ = "https://api.bot.qq.com";
            bool replyToAllGroupMessages_ = false;
            int timeoutSeconds_ = 20;
        };

        struct ModelConfig
        {
        public:
            std::string provider_ = "openai";
            std::string apiFormat_ = "responses";
            std::string baseUrl_ = "https://api.openai.com/v1";
            std::string model_;
            std::string apiKey_;
            std::string apiKeyEnvironment_ = "OPENAI_API_KEY";
            std::string systemPrompt_ = "你是 ChatBuddy，一个通过即时通讯提供帮助的助手。请清晰、准确地回答用户。";
            int timeoutSeconds_ = 90;
            int maxOutputTokens_ = 2048;
        };

    public:
        QQConfig qq_;
        ModelConfig model_;
        int workerCount_ = 4;
        int maxPendingMessages_ = 128;
        int maxSessions_ = 256;
        int maxHistoryTurns_ = 12;
        int maxContextBytes_ = 32768;
        int maxInputBytes_ = 8192;
        int sessionTimeoutSeconds_ = 1800;

    public:
        static Config LoadFromEnvironment(bool requireCredentials = true);
        void Validate(bool requireCredentials = true) const;
        static std::string Help();
    };

    // Small shared native-platform helpers; no separate utility project or dependency files.
    namespace Detail
    {
        using JsonObject = winrt::Windows::Data::Json::JsonObject;
        using JsonArray = winrt::Windows::Data::Json::JsonArray;
        using JsonValue = winrt::Windows::Data::Json::JsonValue;
        using HttpResult = std::tuple<unsigned, JsonObject, unsigned>;
        using Transport = std::function<HttpResult(std::string_view, std::string_view, std::string_view, const JsonObject&, int, std::stop_token)>;

        std::string ReadEnvironment(std::string_view name, std::string_view fallback = {});
        std::string String(const JsonObject& object, std::wstring_view key, std::string_view fallback = {});
        std::int64_t Integer(const JsonObject& object, std::wstring_view key, std::int64_t fallback = 0);
        bool Boolean(const JsonObject& object, std::wstring_view key, bool fallback = false);
        JsonObject Object(const JsonObject& object, std::wstring_view key);
        JsonArray Array(const JsonObject& object, std::wstring_view key);
        void Put(JsonObject& object, std::wstring_view key, std::string_view value);
        void Put(JsonObject& object, std::wstring_view key, std::int64_t value);
        void Put(JsonObject& object, std::wstring_view key, bool value);
        std::string Trim(std::string value);
        std::string Utf8Prefix(std::string_view text, std::size_t maxBytes);
        bool IsIdentifier(std::string_view value);
        void ValidateHttpsUrl(std::string_view url);
        bool Wait(std::stop_token stop, std::chrono::milliseconds duration);
        void Log(std::string_view message);
        HttpResult RequestJson(std::string_view method, std::string_view url, std::string_view authorization, const JsonObject& body, int timeoutSeconds, std::stop_token stop);

        template<typename Operation>
        auto Await(const Operation& operation, std::chrono::steady_clock::time_point deadline, std::stop_token stop)
        {
            using winrt::Windows::Foundation::AsyncStatus;
            while (operation.Status() == AsyncStatus::Started)
            {
                if (stop.stop_requested() || std::chrono::steady_clock::now() >= deadline)
                {
                    operation.Cancel();
                    throw std::runtime_error(stop.stop_requested() ? "Request cancelled." : "Network operation timed out.");
                }
                Wait(stop, std::chrono::milliseconds(10));
            }
            if (stop.stop_requested()) throw std::runtime_error("Request cancelled.");
            return operation.GetResults();
        }
    }
}
