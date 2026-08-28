/*
 * Copyright (c) 2020-2026 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef PARSE_SURFACE_REGION_INT_H
#define PARSE_SURFACE_REGION_INT_H

#include <charconv>
#include <cstdint>
#include <string>

namespace OHOS {
namespace Media {
inline bool ParseSurfaceRegionInt(const std::string &text, int32_t &out)
{
    if (text.empty()) {
        return false;
    }
    const char *first = text.data();
    const char *last = first + text.size();
    auto [ptr, ec] = std::from_chars(first, last, out);
    return ec == std::errc{} && ptr == last;
}
} // namespace Media
} // namespace OHOS
#endif // PARSE_SURFACE_REGION_INT_H
