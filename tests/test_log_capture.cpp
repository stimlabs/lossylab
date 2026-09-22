#include "lossylab/core/error.hpp"
#include "lossylab/env/log.hpp"
#include "lossylab/io/probe.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace lossylab;

namespace
{
    std::string data_path(std::string_view name)
    {
        return std::string(LOSSYLAB_TEST_DATA_DIR) + "/" + std::string(name);
    }

    std::vector<std::uint8_t> read_file(const std::string& path)
    {
        std::FILE* file = std::fopen(path.c_str(), "rb");
        assert(file != nullptr && "could not open fixture");
        std::vector<std::uint8_t> bytes;
        std::uint8_t buffer[4096];
        std::size_t read = 0;
        while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
        {
            bytes.insert(bytes.end(), buffer, buffer + read);
        }
        std::fclose(file);
        return bytes;
    }

    /// The first half of the H.264 fixture. Its moov atom sits at the end, so
    /// the MP4 demuxer logs "moov atom not found" at error level and fails.
    Source truncated_mp4()
    {
        std::vector<std::uint8_t> bytes = read_file(data_path("testsrc_64x48.mp4"));
        bytes.resize(bytes.size() / 2);
        return Source::from_bytes(std::move(bytes));
    }

    void probe_truncated_mp4()
    {
        try
        {
            static_cast<void>(probe(truncated_mp4()));
            assert(false && "expected a truncated file to fail");
        }
        catch (const Error&)
        {
        }
    }

    bool mentions_missing_moov(const std::vector<LogMessage>& messages)
    {
        return std::any_of(messages.begin(), messages.end(), [](const LogMessage& message)
                           { return message.text.find("moov atom not found") != std::string::npos; });
    }

    /// Collects handler lines for the duration of a test, and restores
    /// FFmpeg's default output afterwards.
    class HandlerLines
    {
    public:
        explicit HandlerLines(const LogLevel level = LogLevel::Warning)
        {
            set_log_handler(
                [this](const LogMessage& message)
                {
                    const std::lock_guard<std::mutex> lock(m_mutex);
                    m_messages.push_back(message);
                },
                level);
        }

        ~HandlerLines() { set_log_handler(nullptr); }

        HandlerLines(const HandlerLines&) = delete;
        HandlerLines& operator=(const HandlerLines&) = delete;

        [[nodiscard]] std::vector<LogMessage> messages()
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            return m_messages;
        }

    private:
        std::mutex m_mutex;
        std::vector<LogMessage> m_messages;
    };

    void test_a_capture_collects_this_threads_lines()
    {
        const LogCapture capture;
        probe_truncated_mp4();

        assert(mentions_missing_moov(capture.messages()));
        const LogMessage& message = capture.messages().front();
        assert(message.level == LogLevel::Error);
        assert(!message.component.empty());
    }

    void test_lines_below_the_capture_level_are_not_collected()
    {
        const HandlerLines handler;
        const LogCapture capture(LogLevel::Fatal);
        probe_truncated_mp4();
        assert(capture.messages().empty());
    }

    void test_collected_lines_bypass_the_handler()
    {
        HandlerLines handler;
        {
            const LogCapture capture;
            probe_truncated_mp4();
            assert(mentions_missing_moov(capture.messages()));
        }
        assert(!mentions_missing_moov(handler.messages()));

        probe_truncated_mp4();
        assert(mentions_missing_moov(handler.messages()));
    }

    void test_a_capture_collects_while_ffmpeg_is_muted()
    {
        mute_log();
        {
            const LogCapture capture;
            probe_truncated_mp4();
            assert(mentions_missing_moov(capture.messages()));
        }
        set_log_handler(nullptr);
    }

    void test_another_threads_lines_are_not_collected()
    {
        HandlerLines handler;
        const LogCapture capture;
        std::thread other(probe_truncated_mp4);
        other.join();

        assert(capture.messages().empty());
        assert(mentions_missing_moov(handler.messages()));
    }

    void test_each_thread_collects_only_its_own_lines()
    {
        std::vector<std::size_t> counts(4);
        std::vector<std::thread> threads;
        for (std::size_t i = 0; i < counts.size(); ++i)
        {
            threads.emplace_back(
                [&counts, i]
                {
                    const LogCapture capture;
                    for (std::size_t repeat = 0; repeat <= i; ++repeat)
                    {
                        probe_truncated_mp4();
                    }
                    counts[i] = static_cast<std::size_t>(
                        std::count_if(capture.messages().begin(), capture.messages().end(),
                                      [](const LogMessage& message)
                                      { return message.text.find("moov atom not found") != std::string::npos; }));
                });
        }
        for (std::thread& thread : threads)
        {
            thread.join();
        }

        // Thread i probed i + 1 times, so a line landing in another thread's
        // capture would show as a count off by one.
        for (std::size_t i = 0; i < counts.size(); ++i)
        {
            assert(counts[i] == counts[0] * (i + 1));
        }
        assert(counts[0] > 0);
    }

    void test_only_the_innermost_capture_collects()
    {
        const LogCapture outer;
        {
            const LogCapture inner;
            probe_truncated_mp4();
            assert(mentions_missing_moov(inner.messages()));
            assert(outer.messages().empty());
        }
        probe_truncated_mp4();
        assert(mentions_missing_moov(outer.messages()));
    }

    void test_take_leaves_the_capture_empty()
    {
        LogCapture capture;
        probe_truncated_mp4();
        const std::vector<LogMessage> taken = capture.take();
        assert(mentions_missing_moov(taken));
        assert(capture.messages().empty());
    }

    void test_log_message_json_round_trip()
    {
        const LogMessage message{LogLevel::Warning, "h264", "error while decoding MB 0 1"};
        assert(LogMessage::from_json(message.to_json()) == message);
        assert(message.to_json().at("level") == "warning");
    }
}

int main()
{
    test_a_capture_collects_this_threads_lines();
    test_lines_below_the_capture_level_are_not_collected();
    test_collected_lines_bypass_the_handler();
    test_a_capture_collects_while_ffmpeg_is_muted();
    test_another_threads_lines_are_not_collected();
    test_each_thread_collects_only_its_own_lines();
    test_only_the_innermost_capture_collects();
    test_take_leaves_the_capture_empty();
    test_log_message_json_round_trip();
    return 0;
}
