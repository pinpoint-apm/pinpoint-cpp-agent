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

#pragma once

#include <atomic>
#include <memory>
#include <utility>

#if !defined(__cpp_lib_atomic_shared_ptr)
#include <mutex>
#include <shared_mutex>
#endif

namespace pinpoint {

    /**
     * @brief Thread-safe wrapper around std::shared_ptr.
     *
     * Uses std::atomic<std::shared_ptr<T>> when the C++20 specialization is
     * available, and falls back to std::shared_mutex on platforms such as Apple
     * libc++ where the specialization is still missing.
     */
    template <typename T>
    class AtomicSharedPtr {
    public:
        AtomicSharedPtr() = default;
        explicit AtomicSharedPtr(std::shared_ptr<T> ptr) : ptr_(std::move(ptr)) {}

        AtomicSharedPtr(const AtomicSharedPtr&) = delete;
        AtomicSharedPtr& operator=(const AtomicSharedPtr&) = delete;
        AtomicSharedPtr(AtomicSharedPtr&&) = delete;
        AtomicSharedPtr& operator=(AtomicSharedPtr&&) = delete;

        std::shared_ptr<T> load() const {
#if defined(__cpp_lib_atomic_shared_ptr)
            return ptr_.load();
#else
            std::shared_lock lock(mutex_);
            return ptr_;
#endif
        }

        void store(std::shared_ptr<T> desired) {
#if defined(__cpp_lib_atomic_shared_ptr)
            ptr_.store(std::move(desired));
#else
            std::unique_lock lock(mutex_);
            ptr_ = std::move(desired);
#endif
        }

    private:
#if defined(__cpp_lib_atomic_shared_ptr)
        std::atomic<std::shared_ptr<T>> ptr_{std::shared_ptr<T>{}};
#else
        mutable std::shared_mutex mutex_;
        std::shared_ptr<T> ptr_;
#endif
    };

}  // namespace pinpoint
