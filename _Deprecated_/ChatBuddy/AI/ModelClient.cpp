#include "ModelClient.h"

#include <stdexcept>

namespace ChatBuddy
{
    ModelClient::ModelClient(Config::ModelConfig config, Transport transport) : config_(std::move(config)), transport_(std::move(transport))
    {
        if (!transport_) throw std::invalid_argument("Model transport is required.");
    }

    Detail::JsonObject ModelClient::BuildRequest(const std::vector<Message>& messages) const
    {
        using namespace Detail;
        JsonObject request;
        Put(request, L"model", config_.model_);
        Put(request, L"stream", false);
        JsonArray input;
        if (config_.apiFormat_ == "chat_completions" && !config_.systemPrompt_.empty())
        {
            JsonObject instructions;
            Put(instructions, L"role", std::string_view("system"));
            Put(instructions, L"content", config_.systemPrompt_);
            input.Append(instructions);
        }
        for (const auto& message : messages)
        {
            if (message.role_ != "user" && message.role_ != "assistant") throw std::invalid_argument("Invalid conversation role.");
            JsonObject item;
            Put(item, L"role", message.role_);
            Put(item, L"content", message.content_);
            input.Append(item);
        }
        if (config_.apiFormat_ == "responses")
        {
            request.Insert(L"input", input);
            Put(request, L"instructions", config_.systemPrompt_);
            Put(request, L"max_output_tokens", static_cast<std::int64_t>(config_.maxOutputTokens_));
            Put(request, L"store", false);
        }
        else
        {
            request.Insert(L"messages", input);
            Put(request, config_.provider_ == "openai" ? L"max_completion_tokens" : L"max_tokens", static_cast<std::int64_t>(config_.maxOutputTokens_));
        }
        return request;
    }

    std::string ModelClient::ParseResponse(const Detail::JsonObject& response) const
    {
        using namespace Detail;
        using winrt::Windows::Data::Json::JsonValueType;
        if (response.HasKey(L"error") && response.Lookup(L"error").ValueType() != JsonValueType::Null)
            throw std::runtime_error("Model API reported an error.");
        std::string result;
        bool truncated = false;
        if (config_.apiFormat_ == "responses")
        {
            const auto status = String(response, L"status");
            if (status != "completed" && status != "incomplete") throw std::runtime_error("Model response is not completed.");
            truncated = status == "incomplete";
            for (const auto& value : Array(response, L"output"))
            {
                if (value.ValueType() != JsonValueType::Object) continue;
                const auto item = value.GetObject();
                if (String(item, L"type") != "message" || String(item, L"role") != "assistant") continue;
                for (const auto& partValue : Array(item, L"content"))
                {
                    if (partValue.ValueType() != JsonValueType::Object) continue;
                    const auto part = partValue.GetObject();
                    const auto type = String(part, L"type");
                    const auto text = type == "output_text" ? String(part, L"text") : type == "refusal" ? String(part, L"refusal") : "";
                    if (!text.empty())
                    {
                        if (!result.empty()) result += '\n';
                        result += text;
                    }
                }
            }
        }
        else
        {
            const auto choices = Array(response, L"choices");
            if (choices.Size() == 0) throw std::runtime_error("Model response contains no choices.");
            const auto choice = choices.GetObjectAt(0);
            const auto reason = String(choice, L"finish_reason");
            if (reason == "tool_calls" || reason == "function_call") throw std::runtime_error("Model requested unsupported tool execution.");
            const auto message = Object(choice, L"message");
            result = String(message, L"content", String(message, L"refusal"));
            truncated = reason == "length";
        }
        result = Trim(std::move(result));
        if (result.empty()) throw std::runtime_error("Model returned no displayable text.");
        if (truncated) result += "\n[模型输出达到限制，内容可能不完整。]";
        return result;
    }

    std::string ModelClient::Generate(const std::vector<Message>& messages, std::stop_token stop) const
    {
        const auto request = BuildRequest(messages);
        const auto url = config_.baseUrl_ + (config_.apiFormat_ == "responses" ? "/responses" : "/chat/completions");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(config_.timeoutSeconds_);
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            if (stop.stop_requested()) throw std::runtime_error("Model request cancelled.");
            const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0) throw std::runtime_error("Model request timed out.");
            const auto [status, response, retryAfter] = transport_("POST", url, "Bearer " + config_.apiKey_, request, static_cast<int>(remaining), stop);
            if (status >= 200 && status < 300) return ParseResponse(response);
            // Only retry explicit rate-limit rejection. Never blindly repeat a possibly billed request.
            const unsigned delay = retryAfter == 0 ? static_cast<unsigned>(1 << attempt) : retryAfter;
            if (status != 429 || attempt == 2 || delay > 10 || std::chrono::steady_clock::now() + std::chrono::seconds(delay) >= deadline)
                throw std::runtime_error("Model API HTTP " + std::to_string(status));
            if (!Detail::Wait(stop, std::chrono::seconds(delay))) throw std::runtime_error("Model request cancelled.");
        }
        throw std::runtime_error("Model request failed.");
    }
}
