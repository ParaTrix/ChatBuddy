#include "Application.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace ChatBuddy
{
    Application::Application(Config config, Detail::Transport modelTransport, Detail::Transport qqTransport)
        : config_(std::move(config)), model_(config_.model_, std::move(modelTransport)), qq_(config_.qq_, std::move(qqTransport))
    {
        config_.Validate(false);
    }

    Application::~Application()
    {
        Stop();
        workers_.clear();
    }

    void Application::Stop()
    {
        stopSource_.request_stop();
        changed_.notify_all();
    }

    bool Application::Enqueue(const QQBot::Message& message)
    {
        if (stopSource_.stop_requested()) return false;
        const auto now = std::chrono::steady_clock::now();
        const auto wallNow = std::chrono::system_clock::now();
        if (message.expiresAt_ <= wallNow + std::chrono::seconds(config_.qq_.timeoutSeconds_ + 2)) return true;
        if (message.expiresAt_ > wallNow + std::chrono::minutes(message.group_ ? 10 : 65)) return true;
        const auto key = message.DeduplicationKey();
        std::lock_guard lock(mutex_);
        while (!seenOrder_.empty() && now - seenOrder_.front().first >= std::chrono::hours(1))
        {
            seen_.erase(seenOrder_.front().second);
            seenOrder_.pop_front();
        }
        if (seen_.contains(key)) return true;
        if (pending_.size() >= static_cast<std::size_t>(config_.maxPendingMessages_)) return false;
        while (seenOrder_.size() >= 8192)
        {
            seen_.erase(seenOrder_.front().second);
            seenOrder_.pop_front();
        }
        auto queued = message;
        // Preserve the over-limit condition without storing arbitrarily large queued content.
        if (queued.content_.size() > static_cast<std::size_t>(config_.maxInputBytes_))
            queued.content_ = Detail::Utf8Prefix(queued.content_, static_cast<std::size_t>(config_.maxInputBytes_) + 8);
        pending_.push_back(std::move(queued));
        seen_.insert(key);
        seenOrder_.emplace_back(now, key);
        changed_.notify_all();
        return true;
    }

    void Application::StartWorkers()
    {
        if (!workers_.empty()) throw std::logic_error("Application is already running.");
        for (int index = 0; index < config_.workerCount_; ++index) workers_.emplace_back([this] { Worker(); });
    }

    void Application::Worker()
    {
        try
        {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            const auto stop = stopSource_.get_token();
            while (!stop.stop_requested())
            {
                QQBot::Message message;
                std::string key;
                {
                    std::unique_lock lock(mutex_);
                    auto available = [&]
                    {
                        return std::find_if(pending_.begin(), pending_.end(), [&](const auto& candidate)
                        {
                            return !busy_.contains(candidate.ConversationKey());
                        });
                    };
                    if (!changed_.wait(lock, stop, [&] { return available() != pending_.end(); })) break;
                    if (stop.stop_requested()) break;
                    const auto next = available();
                    message = std::move(*next);
                    pending_.erase(next);
                    key = message.ConversationKey();
                    busy_.insert(key);
                }
                try { Process(message); }
                catch (const std::exception& error) { if (!stop.stop_requested()) Detail::Log(error.what()); }
                catch (const winrt::hresult_error&) { if (!stop.stop_requested()) Detail::Log("Native message-processing failure."); }
                {
                    std::lock_guard lock(mutex_);
                    busy_.erase(key);
                }
                changed_.notify_all();
            }
            winrt::uninit_apartment();
        }
        catch (...)
        {
            {
                std::lock_guard lock(mutex_);
                if (!workerFailure_) workerFailure_ = std::current_exception();
            }
            Detail::Log("Worker initialization or scheduling failed; stopping the application.");
            Stop();
        }
    }

    void Application::TrimHistory(History& history, std::size_t reservedBytes) const
    {
        auto bytes = config_.model_.systemPrompt_.size() + reservedBytes;
        for (const auto& item : history) bytes += item.content_.size();
        const auto maxMessages = static_cast<std::size_t>(config_.maxHistoryTurns_) * 2;
        while (history.size() >= 2 && (history.size() > maxMessages || bytes > static_cast<std::size_t>(config_.maxContextBytes_)))
        {
            bytes -= history[0].content_.size() + history[1].content_.size();
            history.erase(history.begin(), history.begin() + 2);
        }
    }

    void Application::SaveHistory(const std::string& key, History history)
    {
        TrimHistory(history, 0);
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard lock(mutex_);
        std::erase_if(sessions_, [&](const auto& item)
        {
            return now - item.second.first >= std::chrono::seconds(config_.sessionTimeoutSeconds_);
        });
        if (history.empty()) { sessions_.erase(key); return; }
        if (!sessions_.contains(key) && sessions_.size() >= static_cast<std::size_t>(config_.maxSessions_))
        {
            const auto oldest = std::min_element(sessions_.begin(), sessions_.end(), [](const auto& first, const auto& second)
            {
                return first.second.first < second.second.first;
            });
            sessions_.erase(oldest);
        }
        sessions_.insert_or_assign(key, Session{ now, std::move(history) });
    }

    void Application::Process(const QQBot::Message& message)
    {
        const auto stop = stopSource_.get_token();
        if (stop.stop_requested() || std::chrono::system_clock::now() + std::chrono::seconds(config_.qq_.timeoutSeconds_ + 2) >= message.expiresAt_) return;
        const auto key = message.ConversationKey();
        const auto content = Detail::Trim(message.content_);
        if (content == "/reset")
        {
            {
                std::lock_guard lock(mutex_);
                sessions_.erase(key);
            }
            qq_.Reply(message, "当前会话记录已清除。", stop);
            return;
        }
        if (content == "/help")
        {
            qq_.Reply(message, "我是 ChatBuddy。私聊可直接提问；群聊默认请 @ 我。\n/reset 清除你在当前会话中的上下文。\n目前支持文字问答；会话记录保存在内存中。", stop);
            return;
        }
        if (content.empty())
        {
            qq_.Reply(message, "目前请使用文字提问；图片、语音和文件尚不能直接处理。", stop);
            return;
        }
        if (content.size() > static_cast<std::size_t>(config_.maxInputBytes_) ||
            content.size() + config_.model_.systemPrompt_.size() > static_cast<std::size_t>(config_.maxContextBytes_))
        {
            qq_.Reply(message, "这条消息过长，请缩短内容或分成几个问题。", stop);
            return;
        }
        History history;
        {
            std::lock_guard lock(mutex_);
            const auto previous = sessions_.find(key);
            if (previous != sessions_.end())
            {
                if (std::chrono::steady_clock::now() - previous->second.first < std::chrono::seconds(config_.sessionTimeoutSeconds_))
                    history = previous->second.second;
                else sessions_.erase(previous);
            }
        }
        TrimHistory(history, content.size());
        history.push_back({ "user", content });
        // Do not bill a model request when it cannot finish inside the QQ reply window.
        if (std::chrono::system_clock::now() + std::chrono::seconds(config_.model_.timeoutSeconds_ + config_.qq_.timeoutSeconds_ + 5) >= message.expiresAt_)
        {
            qq_.Reply(message, "这条消息的回复时限即将到期，请重新发送问题。", stop);
            return;
        }
        std::string response;
        try { response = model_.Generate(history, stop); }
        catch (const std::exception& error)
        {
            if (stop.stop_requested()) return;
            Detail::Log(error.what());
            qq_.Reply(message, "模型服务暂时无法完成请求，请稍后重试。", stop);
            return;
        }
        catch (const winrt::hresult_error&)
        {
            if (stop.stop_requested()) return;
            Detail::Log("Model response parsing failed.");
            qq_.Reply(message, "模型返回的数据暂时无法处理，请稍后重试。", stop);
            return;
        }
        // Sending failures must not trigger another reply: earlier chunks may already be visible.
        const auto delivered = qq_.Reply(message, response, stop);
        history.push_back({ "assistant", delivered });
        SaveHistory(key, std::move(history));
    }

    void Application::Run()
    {
        try
        {
            StartWorkers();
            qq_.Run([this](const auto& message) { return Enqueue(message); }, stopSource_.get_token());
        }
        catch (...)
        {
            Stop();
            workers_.clear();
            throw;
        }
        Stop();
        workers_.clear();
        if (workerFailure_) std::rethrow_exception(workerFailure_);
    }

#ifdef _DEBUG
    void Application::SelfTest()
    {
        using namespace Detail;
        int checks = 0;
        auto expect = [&](bool condition, std::string_view name)
        {
            if (!condition) throw std::runtime_error("Self-test failed: " + std::string(name));
            ++checks;
        };
        auto rejects = [&](auto action, std::string_view name)
        {
            bool rejected = false;
            try { action(); } catch (...) { rejected = true; }
            expect(rejected, name);
        };
        auto json = [](std::string_view text) { return JsonObject::Parse(winrt::to_hstring(text)); };

        Config config;
        config.model_.model_ = "offline-test-model";
        config.Validate(false);
        auto invalid = config;
        invalid.workerCount_ = 0;
        rejects([&] { invalid.Validate(false); }, "invalid worker count");
        rejects([&] { config.Validate(true); }, "missing credentials");
        rejects([&] { ValidateHttpsUrl("http://example.invalid"); }, "HTTPS required");
        rejects([&] { ValidateHttpsUrl("https://user:password@example.invalid"); }, "endpoint credentials rejected");
        expect(Utf8Prefix("中文abc", 4) == "中", "UTF-8 truncation");

        const auto groupPayload = json(R"({"op":0,"s":7,"t":"GROUP_AT_MESSAGE_CREATE","d":{"id":"message-a","group_openid":"group-a","author":{"member_openid":"alice"},"content":"  你好  ","timestamp":"2026-11-02T12:30:00+08:00","message_scene":{"ext":["msg_idx=index-a","auth_token=unused"]}}})");
        const auto parsed = QQBot::ParseMessage(groupPayload, false);
        expect(parsed && parsed->content_ == "你好" && parsed->sourceIndex_ == "index-a", "group event and message index");
        auto fullPayload = json(winrt::to_string(groupPayload.Stringify()));
        Put(fullPayload, L"t", std::string_view("GROUP_MESSAGE_CREATE"));
        expect(!QQBot::ParseMessage(fullPayload, false), "mention-only mode ignores full stream");
        expect(QQBot::ParseMessage(fullPayload, true)->DeduplicationKey() == parsed->DeduplicationKey(), "cross-event deduplication");
        auto privatePayload = json(R"({"op":0,"t":"C2C_MESSAGE_CREATE","d":{"id":"p-a","author":{"user_openid":"alice"},"content":"hello","timestamp":"2026-11-02T04:30:00.123Z"}})");
        const auto privateMessage = QQBot::ParseMessage(privatePayload, false);
        expect(privateMessage && privateMessage->ConversationKey() != parsed->ConversationKey(), "private/group isolation");
        expect(privateMessage->expiresAt_ - parsed->expiresAt_ == std::chrono::minutes(55), "timestamp timezone conversion");
        auto privateData = Object(privatePayload, L"d");
        Put(privateData, L"timestamp", std::string_view("2026-99-02T04:30:00Z"));
        expect(!QQBot::ParseMessage(privatePayload, false), "invalid timestamp");
        Put(privateData, L"timestamp", std::string_view("2026-11-02T04:30:00Z"));
        auto privateAuthor = Object(privateData, L"author");
        Put(privateAuthor, L"bot", true);
        expect(!QQBot::ParseMessage(privatePayload, false), "ignore bot messages");
        auto foreignSender = *parsed;
        foreignSender.senderId_ = "bob";
        expect(foreignSender.ConversationKey() != parsed->ConversationKey(), "per-group sender isolation");
        expect(QQBot::SplitReply(std::string(10000, 'a'), false).size() == 4, "private reply quota");
        expect(QQBot::SplitReply(std::string(10000, 'a'), true).size() == 5, "group reply quota");
        std::string unicode;
        for (int index = 0; index < 4000; ++index) unicode += "你";
        const auto chunks = QQBot::SplitReply(unicode, true);
        for (const auto& chunk : chunks) expect(winrt::to_string(winrt::to_hstring(chunk)) == chunk && chunk.size() <= 1800, "valid UTF-8 reply chunk");
        expect(chunks.back().find("已截断") != std::string::npos, "visible truncation notice");

        ModelClient responses(config.model_);
        const auto request = responses.BuildRequest({ {"user", "hello"}, {"assistant", "world"}, {"user", "next"} });
        expect(Array(request, L"input").Size() == 3 && !Boolean(request, L"store", true), "Responses request and no server storage");
        expect(responses.ParseResponse(json(R"({"status":"completed","output":[{"type":"reasoning","summary":[]},{"type":"message","role":"assistant","content":[{"type":"output_text","text":"first"},{"type":"output_text","text":"second"}]}]})")) == "first\nsecond", "Responses multi-item text extraction");
        expect(responses.ParseResponse(json(R"({"status":"completed","output":[{"type":"message","role":"assistant","content":[{"type":"refusal","refusal":"declined"}]}]})")) == "declined", "Responses refusal");
        rejects([&] { responses.ParseResponse(json(R"({"status":"failed","output":[]})")); }, "failed model response");
        rejects([&] { responses.BuildRequest({ {"system", "untrusted"} }); }, "conversation role validation");
        auto deepSeekConfig = config.model_;
        deepSeekConfig.provider_ = "deepseek";
        deepSeekConfig.apiFormat_ = "chat_completions";
        ModelClient chat(deepSeekConfig);
        expect(Array(chat.BuildRequest({ {"user", "hi"} }), L"messages").Size() == 2, "Chat Completions system prompt");
        expect(chat.ParseResponse(json(R"({"choices":[{"finish_reason":"stop","message":{"role":"assistant","content":"answer","reasoning_content":"private"}}]})")) == "answer", "reasoning not exposed");
        expect(chat.ParseResponse(json(R"({"choices":[{"finish_reason":"length","message":{"content":"partial"}}]})")).find("不完整") != std::string::npos, "model truncation notice");
        rejects([&] { chat.ParseResponse(json(R"({"choices":[{"finish_reason":"tool_calls","message":{"content":""}}]})")); }, "unsupported tool call");

        int rejectedCalls = 0;
        ModelClient failing(config.model_, [&](auto, auto, auto, const auto&, auto, auto) -> HttpResult
        {
            ++rejectedCalls;
            return { 500, JsonObject{}, 0 };
        });
        rejects([&] { failing.Generate({ {"user", "hi"} }); }, "model server error");
        expect(rejectedCalls == 1, "no duplicate potentially billed requests");
        std::stop_source cancelled;
        cancelled.request_stop();
        rejects([&] { failing.Generate({ {"user", "hi"} }, cancelled.get_token()); }, "model cancellation");
        expect(rejectedCalls == 1, "cancelled request never reaches transport");

        std::mutex observedMutex;
        std::vector<std::string> modelRequests;
        std::vector<std::string> sentRequests;
        std::atomic<int> tokenCalls{ 0 };
        std::atomic<int> activeCalls{ 0 };
        std::atomic<int> concurrentCalls{ 0 };
        auto modelTransport = [&](auto, auto, auto, const JsonObject& body, auto, std::stop_token stop) -> HttpResult
        {
            const auto input = Array(body, L"input");
            const auto prompt = String(input.GetObjectAt(input.Size() - 1), L"content");
            {
                std::lock_guard lock(observedMutex);
                modelRequests.push_back(winrt::to_string(body.Stringify()));
            }
            if (prompt == "fail") throw std::runtime_error("Simulated model failure.");
            if (++activeCalls > 1) ++concurrentCalls;
            Wait(stop, std::chrono::milliseconds(30));
            --activeCalls;
            JsonObject response;
            Put(response, L"status", std::string_view("completed"));
            JsonObject item;
            Put(item, L"type", std::string_view("message"));
            Put(item, L"role", std::string_view("assistant"));
            JsonObject part;
            Put(part, L"type", std::string_view("output_text"));
            Put(part, L"text", "reply:" + prompt);
            JsonArray content;
            content.Append(part);
            item.Insert(L"content", content);
            JsonArray output;
            output.Append(item);
            response.Insert(L"output", output);
            return { 200, response, 0 };
        };
        auto qqTransport = [&](auto, std::string_view url, auto, const JsonObject& body, auto, auto) -> HttpResult
        {
            if (url.ends_with("/getAppAccessToken"))
            {
                ++tokenCalls;
                return { 200, json(R"({"access_token":"offline-test-token","expires_in":"7200"})"), 0 };
            }
            {
                std::lock_guard lock(observedMutex);
                sentRequests.push_back(winrt::to_string(body.Stringify()));
            }
            return { 200, json(R"({"id":"offline-reply"})"), 0 };
        };
        Application app(config, modelTransport, qqTransport);
        auto message = *privateMessage;
        message.expiresAt_ = std::chrono::system_clock::now() + std::chrono::minutes(60);
        message.content_ = "one";
        app.Process(message);
        message.content_ = "two";
        app.Process(message);
        expect(Array(json(modelRequests.back()), L"input").Size() == 3, "history carried into next turn");
        auto groupMessage = *parsed;
        groupMessage.expiresAt_ = std::chrono::system_clock::now() + std::chrono::minutes(5);
        groupMessage.content_ = "group";
        app.Process(groupMessage);
        expect(Array(json(modelRequests.back()), L"input").Size() == 1, "group request excludes private history");
        expect(tokenCalls == 1, "QQ token cache reused");
        message.content_ = "/reset";
        const auto beforeReset = modelRequests.size();
        app.Process(message);
        expect(modelRequests.size() == beforeReset && !app.sessions_.contains(message.ConversationKey()), "reset is local and skips model");
        expect(app.sessions_.contains(groupMessage.ConversationKey()), "reset preserves unrelated session");
        message.content_ = "fail";
        app.Process(message);
        expect(!app.sessions_.contains(message.ConversationKey()), "failed turn not committed");
        expect(String(json(sentRequests.back()), L"content").find("暂时") != std::string::npos, "safe failure reply");
        message.content_ = std::string(10000, 'x');
        const auto beforeOversize = modelRequests.size();
        app.Process(message);
        expect(modelRequests.size() == beforeOversize, "oversize input skips model");
        auto expired = message;
        expired.expiresAt_ = std::chrono::system_clock::now() - std::chrono::seconds(1);
        const auto beforeExpired = sentRequests.size();
        app.Process(expired);
        expect(sentRequests.size() == beforeExpired, "expired event sends nothing");

        History history{ {"user", "a"}, {"assistant", "b"}, {"user", "c"}, {"assistant", "d"} };
        auto smallConfig = config;
        smallConfig.maxHistoryTurns_ = 1;
        smallConfig.maxPendingMessages_ = 1;
        Application small(smallConfig, modelTransport, qqTransport);
        small.TrimHistory(history, 0);
        expect(history.size() == 2 && history.front().content_ == "c", "trim complete turns");
        message.content_ = "queued";
        message.id_ = "queue-a";
        expect(small.Enqueue(message), "enqueue first message");
        expect(small.Enqueue(message) && small.pending_.size() == 1, "duplicate event not queued twice");
        message.id_ = "queue-b";
        expect(!small.Enqueue(message), "bounded queue backpressure");
        expect(!small.seen_.contains(message.DeduplicationKey()), "unaccepted message can be replayed");

        {
            std::lock_guard lock(observedMutex);
            modelRequests.clear();
            sentRequests.clear();
        }
        Application concurrent(config, modelTransport, qqTransport);
        auto first = message;
        first.id_ = "fifo-a";
        first.content_ = "first";
        auto second = first;
        second.id_ = "fifo-b";
        second.content_ = "second";
        auto other = first;
        other.id_ = "other-a";
        other.senderId_ = "bob";
        other.targetId_ = "bob";
        other.content_ = "other";
        concurrent.Enqueue(first);
        concurrent.Enqueue(second);
        concurrent.Enqueue(other);
        concurrent.StartWorkers();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        bool finished = false;
        while (std::chrono::steady_clock::now() < deadline)
        {
            {
                std::lock_guard lock(concurrent.mutex_);
                finished = concurrent.pending_.empty() && concurrent.busy_.empty();
            }
            if (finished) break;
            Wait({}, std::chrono::milliseconds(10));
        }
        concurrent.Stop();
        concurrent.workers_.clear();
        expect(finished && modelRequests.size() == 3 && sentRequests.size() == 3, "workers drain accepted messages");
        expect(concurrentCalls > 0, "independent conversations execute concurrently");
        bool ordered = false;
        for (const auto& recorded : modelRequests)
        {
            const auto input = Array(json(recorded), L"input");
            if (String(input.GetObjectAt(input.Size() - 1), L"content") == "second")
                ordered = input.Size() == 3 && String(input.GetObjectAt(0), L"content") == "first";
        }
        expect(ordered, "same conversation executes in FIFO order");

        checks += QQBot::SelfTest();
        std::cout << "Self-test passed: " << checks << " checks; no network requests or real credentials used.\n";
    }
#endif
}
