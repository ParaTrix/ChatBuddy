#include "QQBot.h"

#include <algorithm>
#include <charconv>
#include <memory>
#include <random>
#include <stdexcept>
#include <thread>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

namespace ChatBuddy
{
    namespace
    {
        void CheckResponse(unsigned status, const Detail::JsonObject& response)
        {
            if (status < 200 || status >= 300) throw std::runtime_error("QQ API HTTP " + std::to_string(status));
            const auto code = Detail::Integer(response, L"code");
            if (code != 0) throw std::runtime_error("QQ API error code " + std::to_string(code));
        }

        std::optional<std::chrono::system_clock::time_point> ParseTimestamp(std::string_view text)
        {
            using namespace std::chrono;
            if (text.size() < 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' || text[16] != ':') return {};
            auto number = [&](std::size_t offset, std::size_t length)
            {
                int value = -1;
                if (offset + length > text.size()) return -1;
                const auto result = std::from_chars(text.data() + offset, text.data() + offset + length, value);
                return result.ec == std::errc{} && result.ptr == text.data() + offset + length ? value : -1;
            };
            const int yearValue = number(0, 4);
            const int monthValue = number(5, 2);
            const int dayValue = number(8, 2);
            const int hourValue = number(11, 2);
            const int minuteValue = number(14, 2);
            const int secondValue = number(17, 2);
            const year_month_day date{ year{yearValue}, month{static_cast<unsigned>(monthValue)}, day{static_cast<unsigned>(dayValue)} };
            if (!date.ok() || yearValue < 2000 || yearValue > 2200 || hourValue < 0 || hourValue > 23 ||
                minuteValue < 0 || minuteValue > 59 || secondValue < 0 || secondValue > 59) return {};
            std::size_t zone = 19;
            if (text[zone] == '.')
            {
                const auto firstDigit = ++zone;
                while (zone < text.size() && text[zone] >= '0' && text[zone] <= '9') ++zone;
                if (zone == firstDigit) return {};
            }
            int offsetMinutes = 0;
            if (zone < text.size() && text[zone] == 'Z')
            {
                if (zone + 1 != text.size()) return {};
            }
            else
            {
                if (zone + 6 != text.size() || (text[zone] != '+' && text[zone] != '-') || text[zone + 3] != ':') return {};
                const int zoneHour = number(zone + 1, 2);
                const int zoneMinute = number(zone + 4, 2);
                if (zoneHour < 0 || zoneHour > 23 || zoneMinute < 0 || zoneMinute > 59) return {};
                offsetMinutes = (zoneHour * 60 + zoneMinute) * (text[zone] == '+' ? 1 : -1);
            }
            return sys_days(date) + hours(hourValue) + minutes(minuteValue - offsetMinutes) + seconds(secondValue);
        }
    }

    std::string QQBot::Message::ConversationKey() const
    {
        return group_ ? "group:" + targetId_ + ":user:" + senderId_ : "private:" + senderId_;
    }

    std::string QQBot::Message::DeduplicationKey() const
    {
        return ConversationKey() + (sourceIndex_.empty() ? ":id:" + id_ : ":index:" + sourceIndex_);
    }

    QQBot::QQBot(Config::QQConfig config, Transport transport) : config_(std::move(config)), transport_(std::move(transport))
    {
        if (!transport_) throw std::invalid_argument("QQ transport is required.");
    }

    std::string QQBot::GetAccessToken(std::stop_token stop)
    {
        std::lock_guard lock(tokenMutex_);
        if (!accessToken_.empty() && std::chrono::steady_clock::now() + std::chrono::seconds(40) < tokenExpiresAt_) return accessToken_;
        Detail::JsonObject body;
        Detail::Put(body, L"appId", config_.appId_);
        Detail::Put(body, L"clientSecret", config_.appSecret_);
        const auto [status, response, retryAfter] = transport_("POST", config_.apiBaseUrl_ + "/app/getAppAccessToken", "", body, config_.timeoutSeconds_, stop);
        const auto code = Detail::Integer(response, L"code");
        if (status == 401 || status == 403 || code == 100007 || code == 100016 || code == 10004) fatal_ = true;
        CheckResponse(status, response);
        const auto token = Detail::String(response, L"access_token");
        const auto lifetime = Detail::Integer(response, L"expires_in");
        if (token.empty() || token.find_first_of("\r\n") != std::string::npos || lifetime <= 0 || lifetime > 86400)
            throw std::runtime_error("QQ returned an invalid access-token response.");
        accessToken_ = token;
        tokenExpiresAt_ = std::chrono::steady_clock::now() + std::chrono::seconds(lifetime);
        return accessToken_;
    }

    Detail::JsonObject QQBot::Request(std::string_view method, std::string_view path, const Detail::JsonObject& body, std::stop_token stop)
    {
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            const auto token = GetAccessToken(stop);
            const auto [status, response, retryAfter] = transport_(method, config_.apiBaseUrl_ + std::string(path), "QQBot " + token, body, config_.timeoutSeconds_, stop);
            if (status == 401 && attempt == 0)
            {
                std::lock_guard lock(tokenMutex_);
                if (accessToken_ == token) accessToken_.clear();
                continue;
            }
            CheckResponse(status, response);
            return response;
        }
        throw std::runtime_error("QQ authorization failed.");
    }

    void QQBot::SendPayload(const Detail::JsonObject& payload)
    {
        const auto text = winrt::to_string(payload.Stringify());
        if (!socket_ || socketClosing_) throw std::runtime_error("QQ gateway is disconnected.");
        winrt::Windows::Storage::Streams::DataWriter writer(socket_.OutputStream());
        try
        {
            writer.UnicodeEncoding(winrt::Windows::Storage::Streams::UnicodeEncoding::Utf8);
            writer.WriteString(winrt::to_hstring(text));
            Detail::Await(writer.StoreAsync(), std::chrono::steady_clock::now() + std::chrono::seconds(10), connectionStop_);
            writer.DetachStream();
        }
        catch (...)
        {
            writer.DetachStream();
            throw;
        }
    }

    void QQBot::SendHeartbeat()
    {
        Detail::JsonObject heartbeat;
        Detail::Put(heartbeat, L"op", std::int64_t(1));
        const auto sequence = sequence_.load();
        if (sequence < 0) heartbeat.Insert(L"d", Detail::JsonValue::CreateNullValue());
        else Detail::Put(heartbeat, L"d", sequence);
        awaitingHeartbeat_ = true;
        SendPayload(heartbeat);
    }

    void QQBot::Disconnect()
    {
        if (socket_ && !socketClosing_)
        {
            socketClosing_ = true;
            try { socket_.Close(1000, L""); } catch (...) {}
        }
    }

    void QQBot::HandleClose(unsigned short code)
    {
        if (code == 4004)
        {
            std::lock_guard lock(tokenMutex_);
            accessToken_.clear();
        }
        if (code == 4006 || code == 4007 || (code >= 4900 && code <= 4913))
        {
            sessionId_.clear();
            sequence_ = -1;
        }
        if (code == 4001 || code == 4002 || (code >= 4010 && code <= 4014) || code == 4914 || code == 4915) fatal_ = true;
        Detail::Log("QQ gateway closed, code " + std::to_string(code));
    }

    bool QQBot::HandlePayload(const Detail::JsonObject& payload, const MessageHandler& handler, std::stop_token stop)
    {
        using namespace Detail;
        const auto operation = Integer(payload, L"op", -1);
        if (operation == 10)
        {
            if (heartbeatInterval_.load() != 0) throw std::runtime_error("Duplicate QQ gateway Hello.");
            const auto interval = Integer(Object(payload, L"d"), L"heartbeat_interval");
            if (interval < 1000 || interval > 300000) throw std::runtime_error("Invalid QQ heartbeat interval.");
            JsonObject authentication;
            JsonObject data;
            Put(data, L"token", "QQBot " + GetAccessToken(stop));
            if (!sessionId_.empty() && sequence_.load() >= 0)
            {
                Put(authentication, L"op", std::int64_t(6));
                Put(data, L"session_id", sessionId_);
                Put(data, L"seq", sequence_.load());
            }
            else
            {
                Put(authentication, L"op", std::int64_t(2));
                Put(data, L"intents", std::int64_t(1) << 25);
                JsonArray shard;
                shard.Append(JsonValue::CreateNumberValue(0));
                shard.Append(JsonValue::CreateNumberValue(1));
                data.Insert(L"shard", shard);
            }
            authentication.Insert(L"d", data);
            SendPayload(authentication);
            heartbeatInterval_ = static_cast<int>(interval);
            return true;
        }
        if (operation == 11) { awaitingHeartbeat_ = false; return true; }
        if (operation == 1) { SendHeartbeat(); return true; }
        if (operation == 7) return false;
        if (operation == 9)
        {
            // Re-identify after Invalid Session, even if a server claims it might be resumable.
            sessionId_.clear();
            sequence_ = -1;
            return false;
        }
        if (operation != 0) return true;
        const auto type = String(payload, L"t");
        if (type == "READY")
        {
            sessionId_ = String(Object(payload, L"d"), L"session_id");
            if (sessionId_.empty()) throw std::runtime_error("QQ Ready has no session ID.");
            ready_ = true;
            Log("QQ gateway ready.");
        }
        else if (type == "RESUMED")
        {
            ready_ = true;
            Log("QQ gateway session resumed.");
        }
        else if (const auto message = ParseMessage(payload, config_.replyToAllGroupMessages_))
        {
            if (!handler(*message))
            {
                Log("Message queue is full; reconnecting to replay the unaccepted event.");
                return false;
            }
        }
        // Only acknowledge a dispatch sequence after its event has been accepted (or ignored).
        const auto sequence = Integer(payload, L"s", -1);
        if (sequence >= 0) sequence_ = sequence;
        return true;
    }

    void QQBot::RunConnection(const MessageHandler& handler, std::stop_token stop)
    {
        using namespace Detail;
        const auto gateway = Request("GET", "/gateway/bot", JsonObject{}, stop);
        if (Integer(gateway, L"shards", 1) > 1)
        {
            fatal_ = true;
            throw std::runtime_error("This bot requires multiple QQ gateway shards; this executable uses one shard.");
        }
        const auto limit = Object(gateway, L"session_start_limit");
        if (sessionId_.empty() && limit.HasKey(L"remaining") && Integer(limit, L"remaining") <= 0)
        {
            fatal_ = true;
            throw std::runtime_error("QQ session-start quota is exhausted; retry after the platform reset.");
        }
        const auto url = String(gateway, L"url");
        if (!url.starts_with("wss://")) throw std::runtime_error("QQ gateway must use WSS.");
        const winrt::Windows::Foundation::Uri uri(winrt::to_hstring(url));
        const auto host = winrt::to_string(uri.Host());
        if (!(host == "qq.com" || host.ends_with(".qq.com")) || !uri.UserName().empty() || !uri.Password().empty())
            throw std::runtime_error("QQ returned an unexpected gateway host.");
        using namespace winrt::Windows::Networking::Sockets;
        MessageWebSocket socket;
        socket.Control().MessageType(SocketMessageType::Utf8);
        socket.Control().ReceiveMode(MessageWebSocketReceiveMode::FullMessage);
        socket.Control().MaxMessageSize(1024 * 1024);
        socket_ = socket;
        socketClosing_ = false;
        connectionStop_ = stop;
        auto cleanup = [this](QQBot*) { Disconnect(); socket_ = nullptr; };
        // Non-owning registration guard, including exceptions while attaching handlers/connecting.
        std::unique_ptr<QQBot, decltype(cleanup)> registration(this, cleanup);
        const auto state = std::make_shared<GatewayState>();
        auto received = socket.MessageReceived(winrt::auto_revoke, [state](const auto&, const auto& event)
        {
            try
            {
                if (event.MessageType() != SocketMessageType::Utf8) throw std::runtime_error("Unexpected binary QQ gateway message.");
                const auto reader = event.GetDataReader();
                reader.UnicodeEncoding(winrt::Windows::Storage::Streams::UnicodeEncoding::Utf8);
                const auto length = reader.UnconsumedBufferLength();
                if (length > 1024 * 1024) throw std::runtime_error("QQ gateway message is too large.");
                auto text = winrt::to_string(reader.ReadString(length));
                {
                    std::lock_guard lock(state->mutex_);
                    if (state->closed_) return;
                    if (state->incoming_.size() >= 256 || state->pendingBytes_ + text.size() > 4 * 1024 * 1024)
                    {
                        state->closed_ = true;
                        state->failure_ = "QQ gateway receive queue overflow; reconnecting for replay.";
                    }
                    else
                    {
                        state->pendingBytes_ += text.size();
                        state->incoming_.push_back(std::move(text));
                    }
                }
                state->changed_.notify_all();
            }
            catch (...)
            {
                std::lock_guard lock(state->mutex_);
                state->closed_ = true;
                state->failure_ = "QQ gateway message decoding failed.";
                state->changed_.notify_all();
            }
        });
        auto closed = socket.Closed(winrt::auto_revoke, [state](const auto&, const auto& event)
        {
            std::lock_guard lock(state->mutex_);
            state->closed_ = true;
            state->closeCode_ = event.Code();
            state->changed_.notify_all();
        });
        heartbeatInterval_ = 0;
        awaitingHeartbeat_ = false;
        ready_ = false;
        Await(socket.ConnectAsync(uri), std::chrono::steady_clock::now() + std::chrono::seconds(config_.timeoutSeconds_), stop);
        const auto connectedAt = std::chrono::steady_clock::now();
        auto nextHeartbeat = connectedAt;
        while (!stop.stop_requested())
        {
            if (fatal_.load()) throw std::runtime_error("QQ credentials became invalid while connected.");
            std::string text;
            {
                std::unique_lock lock(state->mutex_);
                state->changed_.wait_for(lock, stop, std::chrono::milliseconds(100), [&] { return state->closed_ || !state->incoming_.empty(); });
                if (stop.stop_requested()) break;
                if (state->closed_)
                {
                    if (!state->failure_.empty()) throw std::runtime_error(state->failure_);
                    HandleClose(state->closeCode_);
                    break;
                }
                if (!state->incoming_.empty())
                {
                    text = std::move(state->incoming_.front());
                    state->incoming_.pop_front();
                    state->pendingBytes_ -= text.size();
                }
            }
            if (!text.empty())
            {
                JsonObject payload;
                if (!JsonObject::TryParse(winrt::to_hstring(text), payload)) throw std::runtime_error("QQ gateway returned invalid JSON.");
                if (!HandlePayload(payload, handler, stop)) break;
            }
            const auto now = std::chrono::steady_clock::now();
            if (!ready_.load() && now - connectedAt > std::chrono::seconds(30)) throw std::runtime_error("QQ gateway handshake timed out.");
            const auto interval = heartbeatInterval_.load();
            if (interval > 0 && now >= nextHeartbeat)
            {
                if (awaitingHeartbeat_.load()) throw std::runtime_error("QQ heartbeat acknowledgement timed out.");
                SendHeartbeat();
                nextHeartbeat = now + std::chrono::milliseconds(interval);
            }
        }
    }

    void QQBot::Run(const MessageHandler& handler, std::stop_token stop)
    {
        if (!handler) throw std::invalid_argument("QQ message handler is required.");
        std::minstd_rand random(std::random_device{}());
        int retrySeconds = 1;
        while (!stop.stop_requested())
        {
            const auto started = std::chrono::steady_clock::now();
            try { RunConnection(handler, stop); }
            catch (const std::exception& error) { if (!stop.stop_requested()) Detail::Log(error.what()); }
            catch (const winrt::hresult_error&) { if (!stop.stop_requested()) Detail::Log("QQ gateway native operation failed."); }
            if (fatal_.load()) throw std::runtime_error("QQ cannot reconnect with this account/configuration; check the reported code.");
            if (stop.stop_requested()) break;
            if (ready_.load() && std::chrono::steady_clock::now() - started > std::chrono::seconds(60)) retrySeconds = 1;
            const auto delay = std::chrono::milliseconds(retrySeconds * 1000 + random() % 500);
            Detail::Log("QQ gateway reconnecting in " + std::to_string(delay.count()) + " ms.");
            if (!Detail::Wait(stop, delay)) break;
            retrySeconds = std::min(retrySeconds * 2, 60);
        }
    }

    std::optional<QQBot::Message> QQBot::ParseMessage(const Detail::JsonObject& payload, bool replyToAllGroups)
    {
        using namespace Detail;
        if (Integer(payload, L"op", -1) != 0) return {};
        const auto event = String(payload, L"t");
        const bool group = event == "GROUP_AT_MESSAGE_CREATE" || event == "GROUP_MESSAGE_CREATE";
        if (event != "C2C_MESSAGE_CREATE" && !group) return {};
        if (event == "GROUP_MESSAGE_CREATE" && !replyToAllGroups) return {};
        const auto data = Object(payload, L"d");
        const auto author = Object(data, L"author");
        if (Boolean(author, L"bot")) return {};
        Message message;
        message.group_ = group;
        message.id_ = String(data, L"id");
        message.senderId_ = String(author, group ? L"member_openid" : L"user_openid", String(author, L"id"));
        message.targetId_ = group ? String(data, L"group_openid") : message.senderId_;
        message.content_ = Trim(String(data, L"content"));
        const auto timestamp = ParseTimestamp(String(data, L"timestamp"));
        if (!timestamp || message.id_.empty() || message.id_.size() > 1024 || !IsIdentifier(message.senderId_) || !IsIdentifier(message.targetId_)) return {};
        message.expiresAt_ = *timestamp + std::chrono::minutes(group ? 5 : 60);
        for (const auto& value : Array(Object(data, L"message_scene"), L"ext"))
        {
            if (value.ValueType() != winrt::Windows::Data::Json::JsonValueType::String) continue;
            const auto text = winrt::to_string(value.GetString());
            if (text.starts_with("msg_idx=") && text.size() > 8 && text.size() <= 1032) message.sourceIndex_ = text.substr(8);
        }
        return message;
    }

    std::vector<std::string> QQBot::SplitReply(std::string_view text, bool group)
    {
        constexpr std::size_t chunkBytes = 1800;
        const std::size_t maxChunks = group ? 5 : 4;
        const std::string suffix = "\n[回复过长，已截断。可要求我继续或缩小问题范围。]";
        std::vector<std::string> result;
        while (!text.empty() && result.size() < maxChunks)
        {
            const bool last = result.size() + 1 == maxChunks;
            const bool truncated = last && text.size() > chunkBytes;
            auto chunk = Detail::Utf8Prefix(text, truncated ? chunkBytes - suffix.size() : chunkBytes);
            if (chunk.empty()) throw std::runtime_error("Invalid UTF-8 reply.");
            text.remove_prefix(chunk.size());
            if (truncated) { chunk += suffix; text = {}; }
            result.push_back(std::move(chunk));
        }
        return result;
    }

    std::string QQBot::Reply(const Message& message, std::string_view text, std::stop_token stop)
    {
        const auto chunks = SplitReply(text, message.group_);
        const auto path = std::string(message.group_ ? "/v2/groups/" : "/v2/users/") + message.targetId_ + "/messages";
        std::string delivered;
        std::int64_t sequence = 1;
        for (const auto& chunk : chunks)
        {
            if (stop.stop_requested()) throw std::runtime_error("QQ reply cancelled.");
            if (std::chrono::system_clock::now() + std::chrono::seconds(config_.timeoutSeconds_ + 2) >= message.expiresAt_)
                throw std::runtime_error("QQ passive-reply window expired.");
            Detail::JsonObject body;
            Detail::Put(body, L"content", chunk);
            Detail::Put(body, L"msg_type", std::int64_t(0));
            Detail::Put(body, L"msg_id", message.id_);
            Detail::Put(body, L"msg_seq", sequence++);
            const auto response = Request("POST", path, body, stop);
            if (Detail::String(response, L"id").empty()) throw std::runtime_error("QQ send acknowledgement has no message ID.");
            delivered += chunk;
        }
        return delivered;
    }

#ifdef _DEBUG
    int QQBot::SelfTest()
    {
        using namespace Detail;
        int checks = 0;
        auto expect = [&](bool condition, std::string_view name)
        {
            if (!condition) throw std::runtime_error("QQ self-test failed: " + std::string(name));
            ++checks;
        };
        auto json = [](std::string_view text) { return JsonObject::Parse(winrt::to_hstring(text)); };
        int tokenRequests = 0;
        std::vector<std::int64_t> sequences;
        QQBot bot({}, [&](auto, std::string_view url, auto, const JsonObject& body, auto, auto) -> HttpResult
        {
            if (url.ends_with("/getAppAccessToken"))
            {
                ++tokenRequests;
                return { 200, json(R"({"access_token":"offline-token","expires_in":"7200"})"), 0 };
            }
            sequences.push_back(Integer(body, L"msg_seq"));
            return { 200, json(R"({"id":"offline-sent"})"), 0 };
        });
        expect(bot.GetAccessToken({}) == "offline-token", "token response parsing");
        bot.GetAccessToken({});
        expect(tokenRequests == 1, "unexpired token reused");
        bot.tokenExpiresAt_ = std::chrono::steady_clock::now();
        bot.GetAccessToken({});
        expect(tokenRequests == 2, "expiring token refreshed");
        bool accepted = false;
        auto handler = [&](const auto&) { return accepted; };
        expect(bot.HandlePayload(json(R"({"op":0,"t":"READY","s":10,"d":{"session_id":"offline-session"}})"), handler, {}), "Ready accepted");
        expect(bot.sessionId_ == "offline-session" && bot.sequence_ == 10 && bot.ready_, "Ready establishes resume state");
        const auto event = json(R"({"op":0,"s":11,"t":"C2C_MESSAGE_CREATE","d":{"id":"incoming","author":{"user_openid":"alice"},"content":"hi","timestamp":"2026-11-02T04:30:00Z"}})");
        expect(!bot.HandlePayload(event, handler, {}) && bot.sequence_ == 10, "backpressure preserves replay sequence");
        accepted = true;
        expect(bot.HandlePayload(event, handler, {}) && bot.sequence_ == 11, "accepted dispatch advances sequence");
        bot.awaitingHeartbeat_ = true;
        bot.HandlePayload(json(R"({"op":11})"), handler, {});
        expect(!bot.awaitingHeartbeat_, "heartbeat acknowledgement clears pending state");
        expect(!bot.HandlePayload(json(R"({"op":7})"), handler, {}), "server reconnect requested");
        expect(!bot.HandlePayload(json(R"({"op":9,"d":false})"), handler, {}) && bot.sessionId_.empty() && bot.sequence_ == -1, "invalid session resets resume state");
        bot.sessionId_ = "bad-session";
        bot.sequence_ = 25;
        bot.HandleClose(4006);
        expect(bot.sessionId_.empty() && bot.sequence_ == -1 && !bot.fatal_, "invalid session close permits identify");
        bot.HandleClose(4004);
        expect(bot.accessToken_.empty(), "expired credentials discarded");
        bot.HandleClose(4014);
        expect(bot.fatal_, "permission close stops reconnect loop");
        bot.fatal_ = false;
        Message message;
        message.id_ = "reply-to";
        message.senderId_ = "alice";
        message.targetId_ = "alice";
        message.expiresAt_ = std::chrono::system_clock::now() + std::chrono::minutes(60);
        const auto delivered = bot.Reply(message, std::string(10000, 'a'), {});
        expect(sequences == std::vector<std::int64_t>{1, 2, 3, 4}, "reply sequence and quota");
        expect(delivered.find("已截断") != std::string::npos, "only delivered reply retained");
        int authFailures = 0;
        QQBot unauthorized({}, [&](auto, auto, auto, const auto&, auto, auto) -> HttpResult
        {
            ++authFailures;
            return { 200, json(R"({"code":100016,"message":"ignored"})"), 0 };
        });
        bool rejected = false;
        try { unauthorized.GetAccessToken({}); } catch (const std::exception&) { rejected = true; }
        expect(rejected && unauthorized.fatal_ && authFailures == 1, "QQ business error despite HTTP 200");
        return checks;
    }
#endif
}
