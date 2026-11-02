#pragma once

#include "../Core/Config.h"
#include <vector>

namespace ChatBuddy
{
    class ModelClient
    {
    public:
        struct Message
        {
        public:
            std::string role_;
            std::string content_;
        };

        using Transport = Detail::Transport;

    private:
        Config::ModelConfig config_;
        Transport transport_;

    public:
        explicit ModelClient(Config::ModelConfig config, Transport transport = Detail::RequestJson);

    public:
        Detail::JsonObject BuildRequest(const std::vector<Message>& messages) const;
        std::string ParseResponse(const Detail::JsonObject& response) const;
        std::string Generate(const std::vector<Message>& messages, std::stop_token stop = {}) const;
    };
}
