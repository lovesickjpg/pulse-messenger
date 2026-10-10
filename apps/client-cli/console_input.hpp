#pragma once

#include <boost/asio.hpp>

#include <array>
#include <cerrno>
#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <poll.h>
#include <unistd.h>
#endif

// stdin shouldn't hold up a disconnect
class ConsoleInput {
  public:
    ConsoleInput(boost::asio::io_context &context, std::function<void(std::string)> on_line,
                 std::function<void()> on_eof)
        : timer_(context), on_line_(std::move(on_line)), on_eof_(std::move(on_eof)) {}

    void start() { poll(); }
    void stop() {
        stopped_ = true;
        timer_.cancel();
    }
    void finish() {
        stop();
        on_eof_();
    }

  private:
#ifdef _WIN32
    void echo(std::wstring_view text) {
        DWORD written = 0;
        WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), text.data(), static_cast<DWORD>(text.size()),
                      &written, nullptr);
    }

    void read_console(HANDLE input) {
        DWORD pending = 0;
        if (!GetNumberOfConsoleInputEvents(input, &pending)) {
            throw std::runtime_error("Cannot poll console input");
        }
        if (pending == 0) {
            return;
        }
        std::array<INPUT_RECORD, 128> events{};
        DWORD count = 0;
        if (!ReadConsoleInputW(input, events.data(), static_cast<DWORD>(events.size()), &count)) {
            throw std::runtime_error("Cannot read console input");
        }
        for (DWORD index = 0; index < count && !stopped_; ++index) {
            const auto &event = events[index];
            if (event.EventType != KEY_EVENT || !event.Event.KeyEvent.bKeyDown) {
                continue;
            }
            const auto &key = event.Event.KeyEvent;
            for (WORD repeat = 0; repeat < key.wRepeatCount && !stopped_; ++repeat) {
                const wchar_t character = key.uChar.UnicodeChar;
                if (character == 26) {
                    finish();
                } else if (character == L'\r') {
                    echo(L"\r\n");
                    if (!console_line_.empty() && !discarding_) {
                        const auto size = WideCharToMultiByte(
                            CP_UTF8, WC_ERR_INVALID_CHARS, console_line_.data(),
                            static_cast<int>(console_line_.size()), nullptr, 0, nullptr, nullptr);
                        if (size == 0) {
                            std::cerr << "Invalid console text; skipped\n";
                        } else {
                            std::string line(size, '\0');
                            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, console_line_.data(),
                                                static_cast<int>(console_line_.size()), line.data(),
                                                size, nullptr, nullptr);
                            on_line_(std::move(line));
                        }
                    }
                    console_line_.clear();
                    discarding_ = false;
                } else if (character == L'\b' && !console_line_.empty()) {
                    const auto last = console_line_.back();
                    console_line_.pop_back();
                    if (last >= 0xDC00 && last <= 0xDFFF && !console_line_.empty() &&
                        console_line_.back() >= 0xD800 && console_line_.back() <= 0xDBFF) {
                        console_line_.pop_back();
                    }
                    echo(L"\b \b");
                } else if (character >= L' ' && !discarding_) {
                    if (console_line_.size() == 65536) {
                        std::cerr << "Input line too long; skipped\n";
                        console_line_.clear();
                        discarding_ = true;
                    } else {
                        console_line_ += character;
                        echo(std::wstring_view(&character, 1));
                    }
                }
            }
        }
    }
#endif

    void eof() {
        if (!line_.empty() && !discarding_) {
            on_line_(std::move(line_));
        }
        if (!stopped_) {
            finish();
        }
    }

    void consume(std::string_view bytes) {
        for (char byte : bytes) {
            if (stopped_) {
                return;
            }
            if (byte == '\n') {
                if (!discarding_) {
                    if (!line_.empty() && line_.back() == '\r') {
                        line_.pop_back();
                    }
                    on_line_(std::move(line_));
                }
                line_.clear();
                discarding_ = false;
            } else if (!discarding_) {
                if (line_.size() == 65536) {
                    std::cerr << "Input line exceeds 64 KiB; skipped\n";
                    line_.clear();
                    discarding_ = true;
                } else {
                    line_ += byte;
                }
            }
        }
    }

    void poll() {
        std::array<char, 4096> bytes{};
#ifdef _WIN32
        const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode = 0;
        if (GetConsoleMode(input, &mode)) {
            read_console(input);
        } else {
            DWORD available = static_cast<DWORD>(bytes.size());
            if (GetFileType(input) == FILE_TYPE_PIPE &&
                !PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr)) {
                if (GetLastError() == ERROR_BROKEN_PIPE) {
                    eof();
                } else {
                    throw std::runtime_error("Cannot poll stdin pipe");
                }
            } else if (available != 0) {
                DWORD count = 0;
                if (!ReadFile(input, bytes.data(), static_cast<DWORD>(bytes.size()), &count,
                              nullptr)) {
                    if (GetLastError() != ERROR_BROKEN_PIPE) {
                        throw std::runtime_error("Cannot read stdin");
                    }
                    eof();
                } else if (count == 0) {
                    eof();
                } else {
                    consume(std::string_view(bytes.data(), count));
                }
            }
        }
#else
        pollfd input{STDIN_FILENO, POLLIN, 0};
        const int ready = ::poll(&input, 1, 0);
        if (ready < 0 && errno != EINTR) {
            throw std::runtime_error("Cannot poll stdin");
        }
        if (ready > 0) {
            const auto count = ::read(STDIN_FILENO, bytes.data(), bytes.size());
            if (count == 0) {
                eof();
            } else if (count > 0) {
                consume(std::string_view(bytes.data(), static_cast<std::size_t>(count)));
            } else if (errno != EINTR) {
                throw std::runtime_error("Cannot read stdin");
            }
        }
#endif
        if (!stopped_) {
            timer_.expires_after(std::chrono::milliseconds(25));
            timer_.async_wait([this](boost::system::error_code error) {
                if (!error) {
                    poll();
                }
            });
        }
    }

    boost::asio::steady_timer timer_;
    std::function<void(std::string)> on_line_;
    std::function<void()> on_eof_;
    std::string line_;
#ifdef _WIN32
    std::wstring console_line_;
#endif
    bool stopped_ = false;
    bool discarding_ = false;
};
