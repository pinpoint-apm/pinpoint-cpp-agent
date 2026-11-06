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

#include <cstdint>

namespace pinpoint {

    /**
     * @brief Number of completed stores into the global agent holder.
     *
     * StartAgent(), Shutdown() and the test helpers bump it once the new
     * global agent is published (see AtomicSharedPtr::generation()). Read it
     * before GlobalAgent(): as long as a later read returns the same count,
     * the agent that GlobalAgent() returned is still the global one, apart
     * from a store still in flight. A caller can therefore skip the owning
     * load until the count moves. That load costs a shared lock and a
     * refcount RMW on cache lines every request thread shares, and the C
     * API's pt_global_agent() would otherwise pay it on every request.
     */
    uint64_t global_agent_generation();

}  // namespace pinpoint
