#pragma once

#include "Config.h"
#include "../AI/ModelClient.h"
#include "../IM/QQBot.h"
#include <condition_variable>
#include <deque>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace ChatBuddy
{
    class Application
    {
    private:
        using History = std::vector<ModelClient::Message>;
        using Session = std::pair<std::chrono::steady_clock::time_point, History>;

    private:
        Config config_;
        ModelClient model_;
        QQBot qq_;
        std::stop_source stopSource_;
        std::mutex mutex_;
        std::condition_variable_any changed_;
        std::deque<QQBot::Message> pending_;
        std::unordered_set<std::string> busy_;
        std::unordered_map<std::string, Session> sessions_;
        std::unordered_set<std::string> seen_;
        std::deque<std::pair<std::chrono::steady_clock::time_point, std::string>> seenOrder_;
        std::exception_ptr workerFailure_;
        std::vector<std::jthread> workers_;

    public:
        explicit Application(Config config, Detail::Transport modelTransport = Detail::RequestJson, Detail::Transport qqTransport = Detail::RequestJson);
        ~Application();
        Application(const Application&) = delete;
        Application& operator=(const Application&) = delete;

    private:
        bool Enqueue(const QQBot::Message& message);
        void StartWorkers();
        void Worker();
        void Process(const QQBot::Message& message);
        void TrimHistory(History& history, std::size_t reservedBytes) const;
        void SaveHistory(const std::string& key, History history);

    public:
        void Run();
        void Stop();
#ifdef _DEBUG
        static void SelfTest();
#endif
    };
}
