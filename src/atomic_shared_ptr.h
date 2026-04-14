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

#include <memory>
#include <shared_mutex>

namespace pinpoint {

    /**
     * @brief Thread-safe wrapper around std::shared_ptr using std::shared_mutex.
     *
     * Provides the same interface as std::atomic<std::shared_ptr<T>> (C++20),
     * working around the missing libc++ specialization on Apple platforms.
     * Can be replaced with std::atomic<std::shared_ptr<T>> once libc++ adds support.
     */
    template <typename T>
    class AtomicSharedPtr {
    public:
        AtomicSharedPtr() = default;
        explicit AtomicSharedPtr(std::shared_ptr<T> ptr) : ptr_(std::move(ptr)) {}

        AtomicSharedPtr(const AtomicSharedPtr&) = delete;
        AtomicSharedPtr& operator=(const AtomicSharedPtr&) = delete;

        std::shared_ptr<T> load() const {
            std::shared_lock lock(mutex_);
            return ptr_;
        }

        void store(std::shared_ptr<T> desired) {
            std::unique_lock lock(mutex_);
            ptr_ = std::move(desired);
        }

    private:
        mutable std::shared_mutex mutex_;
        std::shared_ptr<T> ptr_;
    };

}  // namespace pinpoint
