#include "Core/Application.h"

#include <atomic>
#include <iostream>
#include <string_view>

namespace
{
    std::atomic<bool> interrupted{ false };

    BOOL WINAPI ConsoleHandler(DWORD event)
    {
        if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT)
        {
            interrupted.store(true, std::memory_order_relaxed);
            return TRUE;
        }
        return FALSE;
    }
}

int main(int argc, char* argv[])
{
    SetConsoleOutputCP(CP_UTF8);
    try
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        const std::string_view argument = argc == 2 ? argv[1] : "";
        if (argc > 2) throw std::runtime_error("Use --help for command-line options.");
        if (argument == "--help" || argument == "-h")
        {
            std::cout << ChatBuddy::Config::Help();
            return 0;
        }
#ifdef _DEBUG
        if (argument == "--self-test")
        {
            ChatBuddy::Application::SelfTest();
            return 0;
        }
#endif
        if (!argument.empty() && argument != "--check-config") throw std::runtime_error("Unknown option. Use --help.");
        auto config = ChatBuddy::Config::LoadFromEnvironment();
        if (argument == "--check-config")
        {
            std::cout << "Configuration is valid. Credentials are present but have not been authenticated. No network request was made.\n";
            return 0;
        }
        if (!SetConsoleCtrlHandler(ConsoleHandler, TRUE)) throw std::runtime_error("Cannot install console stop handler.");
        ChatBuddy::Application application(std::move(config));
        std::jthread shutdownMonitor([&](std::stop_token stop)
        {
            while (!stop.stop_requested())
            {
                if (interrupted.load(std::memory_order_relaxed)) { application.Stop(); break; }
                ChatBuddy::Detail::Wait(stop, std::chrono::milliseconds(100));
            }
        });
        ChatBuddy::Detail::Log("Starting QQ official bot. Press Ctrl+C to stop.");
        application.Run();
        ChatBuddy::Detail::Log("Stopped.");
        return 0;
    }
    catch (const std::exception& error)
    {
        ChatBuddy::Detail::Log(error.what());
        return 1;
    }
    catch (const winrt::hresult_error& error)
    {
        ChatBuddy::Detail::Log("Windows API failure, HRESULT " + std::to_string(error.code().value));
        return 1;
    }
}
