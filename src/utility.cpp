/*
 * Copyright 2020-present NAVER Corp.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <algorithm>
#include <array>
#include <limits>
#include <pthread.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <new>
#include <random>
#include <sys/socket.h>

#include "absl/strings/numbers.h"
#include "logging.h"
#include "utility.h"
#include "MurmurHash3.h"

namespace pinpoint {

    // Constants
    namespace {
        constexpr size_t kHostNameMaxLength = 256;  // RFC 1035
        constexpr size_t kMurmurHashOutputSize = 16;  // 128 bits
        constexpr uint32_t kMurmurHashSeed = 0;
    }

    int64_t generate_span_id() {
        static thread_local std::mt19937_64 rand_source{std::random_device()()};
        return static_cast<int64_t>(rand_source());
    }

    int64_t to_milli_seconds(const std::chrono::system_clock::time_point &tm) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(tm.time_since_epoch()).count();
    }

    void superviseWorker(std::string_view name, std::chrono::milliseconds interval,
                         std::mutex& mutex, std::condition_variable& cond_var,
                         const std::function<bool()>& is_exiting,
                         const std::function<void()>& body) {
        while (true) {
            try {
                body();
                break;
            } catch (const std::exception& e) {
                LOG_ERROR("{} exception = {}", name, e.what());
            } catch (...) {
                LOG_ERROR("{} unknown exception", name);
            }

            std::unique_lock<std::mutex> lock(mutex);
            if (cond_var.wait_for(lock, interval, is_exiting)) {
                break;
            }
        }
        LOG_INFO("{} end", name);
    }

    std::string get_host_name() {
        std::array<char, kHostNameMaxLength> host_name{};

        if (gethostname(host_name.data(), host_name.size()) != 0) {
            return "unknown";
        }

        // Ensure null-termination
        host_name[kHostNameMaxLength - 1] = '\0';
        return {host_name.data()};
    }

    static std::string resolve_host_ip_addr() {
        const auto host = get_host_name();
        if (host == "unknown") {  // get_host_name()'s failure sentinel
            return "0.0.0.0";
        }

        // Use getaddrinfo instead of deprecated gethostbyname
        struct addrinfo hints{};
        struct addrinfo* result = nullptr;

        hints.ai_family = AF_INET;  // IPv4
        hints.ai_socktype = SOCK_STREAM;

        if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || !result) {
            return "0.0.0.0";
        }

        // Thread-safe conversion using inet_ntop instead of inet_ntoa
        std::array<char, INET_ADDRSTRLEN> ip_str{};
        auto* sockaddr_ipv4 = reinterpret_cast<struct sockaddr_in*>(result->ai_addr);

        if (inet_ntop(AF_INET, &(sockaddr_ipv4->sin_addr), ip_str.data(), ip_str.size()) == nullptr) {
            freeaddrinfo(result);
            return "0.0.0.0";
        }

        std::string ip_address{ip_str.data()};
        freeaddrinfo(result);
        return ip_address;
    }

    std::string get_host_ip_addr() {
        // getaddrinfo can block for seconds when the hostname is not
        // resolvable (e.g. no DNS); the address is stable for the process
        // lifetime, so resolve once and reuse.
        // Intentionally heap-allocated and never destroyed, matching the
        // Logger, noop and global-agent singletons: the gRPC agent-info
        // worker calls this on every re-registration and may still be
        // running during process-exit static destruction when the host exits
        // without Shutdown() — copying from a destroyed value static there
        // would be use-after-destruction.
        static const auto* cached_ip_addr = new std::string(resolve_host_ip_addr());
        return *cached_ip_addr;
    }

    namespace {
        // Constant-initialized (std::atomic's value constructor is constexpr),
        // so reading it is defined even before this file's dynamic
        // initialization below has run.
        std::atomic<pid_t> cached_pid{0};

        void refresh_cached_pid() noexcept {
            cached_pid.store(getpid(), std::memory_order_relaxed);
        }

        // Registered during this file's dynamic initialization — at library
        // load, before the host can have forked — rather than lazily on the
        // first current_pid() call. A function-local static would put a
        // magic-static guard on that first call, and a fork() from another
        // thread while that guard is held leaves it locked forever in the
        // child. This library explicitly supports pre-fork servers, so that
        // window must not exist. pthread_atfork itself is thread-safe, and
        // every interleaving of a fork() with the initialization below leaves
        // the child either fully hooked or with the flag still false, which
        // the fallback in current_pid() handles.
        const bool pid_hook_installed = [] {
            if (pthread_atfork(nullptr, nullptr, &refresh_cached_pid) != 0) {
                return false;
            }
            refresh_cached_pid();
            return true;
        }();
    }

    pid_t current_pid() noexcept {
        // A false flag means either that the handler could not be registered
        // or that this file's dynamic initialization has not run yet (another
        // translation unit's initializer calling in). Both must bypass the
        // cache: without the fork handler a cached value can go stale in a
        // child, and a stale pid would let an inherited agent record spans
        // into queues whose worker threads do not exist in that process.
        if (!pid_hook_installed) {
            return getpid();
        }
        return cached_pid.load(std::memory_order_relaxed);
    }

    void abandon_thread(std::thread& t) noexcept {
        if (!t.joinable()) {
            return;
        }
        // Never detach(): for a handle inherited across fork(), glibc's
        // pthread_detach unconditionally dereferences the thread descriptor,
        // which the child's fork() has already reclaimed (__reclaim_stacks) —
        // it segfaults instead of returning ESRCH as macOS does. Reusing the
        // object's storage ends the joinable thread object's lifetime without
        // invoking its destructor, then constructs an empty handle in place.
        // Placement new performs no allocation, std::thread's default
        // constructor is noexcept, and no pthread call is made.
        ::new (static_cast<void*>(&t)) std::thread();
    }

    size_t utf8SafeCutLength(std::string_view s, size_t max_len) {
        if (s.length() <= max_len) {
            return s.length();
        }
        size_t back = 0;
        while (back < 3 && back < max_len &&
               (static_cast<unsigned char>(s[max_len - 1 - back]) & 0xC0) == 0x80) {
            ++back;
        }
        if (back >= max_len) {
            return max_len;
        }
        const auto lead = static_cast<unsigned char>(s[max_len - 1 - back]);
        size_t expected = 1;
        if ((lead & 0xF8) == 0xF0) {
            expected = 4;
        } else if ((lead & 0xF0) == 0xE0) {
            expected = 3;
        } else if ((lead & 0xE0) == 0xC0) {
            expected = 2;
        }
        if (expected > back + 1) {
            return max_len - (back + 1);
        }
        return max_len;
    }

    std::optional<int> stoi_(std::string_view str) {
        int result{};
        return absl::SimpleAtoi(str, &result) ? std::optional<int>(result) : std::nullopt;
    }

    std::optional<int64_t> stoll_(std::string_view str) {
        int64_t result{};
        return absl::SimpleAtoi(str, &result) ? std::optional<int64_t>(result) : std::nullopt;
    }

    std::optional<double> stod_(std::string_view str) {
        double result{};
        return absl::SimpleAtod(str, &result) ? std::optional<double>(result) : std::nullopt;
    }

    std::optional<bool> stob_(std::string_view str) {
        bool result{};
        return absl::SimpleAtob(str, &result) ? std::optional<bool>(result) : std::nullopt;
    }

    SqlUid generate_sql_uid(std::string_view sql) {
        // MurmurHash3_x64_128 produces 16 bytes (128 bits) of output
        static_assert(SqlUid{}.size() == kMurmurHashOutputSize,
                      "SqlUid size must match MurmurHash3_x64_128 output");
        SqlUid result{};
        // MurmurHash3 takes an int length; clamp so a >2 GiB input (never
        // produced by the normalizer, which caps SQL length, but this is a
        // public utility) cannot go negative in the cast.
        const auto length = static_cast<int>(std::min<size_t>(
            sql.length(), static_cast<size_t>(std::numeric_limits<int>::max())));
        MurmurHash3_x64_128(sql.data(), length, kMurmurHashSeed, result.data());
        return result;
    }

 }
