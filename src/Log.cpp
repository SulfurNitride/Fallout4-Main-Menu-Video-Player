#include "PCH.h"

#include "Log.h"

namespace
{
    class Win32FileSink final :
        public spdlog::sinks::base_sink<std::mutex>
    {
    public:
        explicit Win32FileSink(const std::filesystem::path& path)
        {
            file_ = CreateFileW(
                path.c_str(),
                GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (file_ == INVALID_HANDLE_VALUE) {
                throw spdlog::spdlog_ex(
                    "Could not create MainMenuVideoPlayer.log");
            }
        }

        ~Win32FileSink() override
        {
            if (file_ != INVALID_HANDLE_VALUE) {
                CloseHandle(file_);
            }
        }

    protected:
        void sink_it_(const spdlog::details::log_msg& message) override
        {
            spdlog::memory_buf_t formatted;
            formatter_->format(message, formatted);

            const char* cursor = formatted.data();
            std::size_t remaining = formatted.size();
            while (remaining > 0) {
                DWORD written = 0;
                const DWORD chunk = static_cast<DWORD>(
                    std::min<std::size_t>(
                        remaining,
                        std::numeric_limits<DWORD>::max()));
                if (!WriteFile(file_, cursor, chunk, &written, nullptr) ||
                    written == 0) {
                    break;
                }
                cursor += written;
                remaining -= written;
            }
        }

        void flush_() override
        {
            FlushFileBuffers(file_);
        }

    private:
        HANDLE file_{ INVALID_HANDLE_VALUE };
    };

    std::filesystem::path ResolveLogPath(const HMODULE module)
    {
        std::array<wchar_t, 32768> modulePath{};
        const DWORD length = GetModuleFileNameW(
            module,
            modulePath.data(),
            static_cast<DWORD>(modulePath.size()));
        if (length > 0 && length < modulePath.size()) {
            return std::filesystem::path{
                std::wstring_view(modulePath.data(), length)
            }.parent_path() / L"MainMenuVideoPlayer.log";
        }
        return "Data/F4SE/Plugins/MainMenuVideoPlayer.log";
    }
}

void Log::Initialize(const HMODULE module)
{
    const std::filesystem::path path = ResolveLogPath(module);
    try {
        auto sink = std::make_shared<Win32FileSink>(path);
        auto logger = std::make_shared<spdlog::logger>(
            "MainMenuVideoPlayer",
            std::move(sink));
        logger->set_level(spdlog::level::trace);
        logger->flush_on(spdlog::level::info);
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
        spdlog::set_default_logger(std::move(logger));
        spdlog::info("Logging beside MainMenuVideoPlayer.dll");
    } catch (const spdlog::spdlog_ex& error) {
        const std::string message = std::format(
            "Main Menu Video Player logging failed: {}\n",
            error.what());
        OutputDebugStringA(message.c_str());
    }
}
