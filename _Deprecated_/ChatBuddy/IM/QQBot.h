#pragma once

#include "../Core/Config.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>
#include <Windows.h>
#include <winrt/Windows.Networking.Sockets.h>

namespace ChatBuddy
{
    class QQBot
    {
    private:
        // Callbacks own this state independently of QQBot so a late callback cannot use a destroyed bot.
        struct GatewayState
        {
        public:
            std::mutex mutex_;
            std::condition_variable_any changed_;
            std::deque<std::string> incoming_;
            std::size_t pendingBytes_ = 0;
            bool closed_ = false;
            unsigned short closeCode_ = 1006;
            std::string failure_;
        };

    public:
        struct Message
        {
        public:
            std::string id_;
            std::string sourceIndex_;
            std::string senderId_;
            std::string targetId_;
            std::string content_;
            bool group_ = false;
            std::chrono::system_clock::time_point expiresAt_;

        public:
            std::string ConversationKey() const;
            std::string DeduplicationKey() const;
        };

        using MessageHandler = std::function<bool(const Message&)>;
        using Transport = Detail::Transport;

    private:
        Config::QQConfig config_;
        Transport transport_;
        std::mutex tokenMutex_;
        std::string accessToken_;
        std::chrono::steady_clock::time_point tokenExpiresAt_;
        winrt::Windows::Networking::Sockets::MessageWebSocket socket_{ nullptr };
        std::stop_token connectionStop_;
        bool socketClosing_ = false;
        std::atomic<bool> fatal_{ false };
        std::atomic<std::int64_t> sequence_{ -1 };
        std::atomic<int> heartbeatInterval_{ 0 };
        std::atomic<bool> awaitingHeartbeat_{ false };
        std::atomic<bool> ready_{ false };
        std::string sessionId_;

    public:
        explicit QQBot(Config::QQConfig config, Transport transport = Detail::RequestJson);
        QQBot(const QQBot&) = delete;
        QQBot& operator=(const QQBot&) = delete;

    private:
        std::string GetAccessToken(std::stop_token stop);
        Detail::JsonObject Request(std::string_view method, std::string_view path, const Detail::JsonObject& body, std::stop_token stop);
        void SendPayload(const Detail::JsonObject& payload);
        void SendHeartbeat();
        void Disconnect();
        void RunConnection(const MessageHandler& handler, std::stop_token stop);
        bool HandlePayload(const Detail::JsonObject& payload, const MessageHandler& handler, std::stop_token stop);
        void HandleClose(unsigned short code);

    public:
        void Run(const MessageHandler& handler, std::stop_token stop);
        std::string Reply(const Message& message, std::string_view text, std::stop_token stop);
        static std::optional<Message> ParseMessage(const Detail::JsonObject& payload, bool replyToAllGroups);
        static std::vector<std::string> SplitReply(std::string_view text, bool group);
#ifdef _DEBUG
        static int SelfTest();
#endif
    };
}
