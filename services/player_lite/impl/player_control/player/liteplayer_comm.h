/*
 * Copyright (c) 2020-2021 Huawei Device Co., Ltd.
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

#ifndef LITEPLAYER_COMM_H
#define LITEPLAYER_COMM_H

#include <pthread.h>
#include <string>
#include "player_define.h"
#include "hi_demuxer.h"
#include "codec_type.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif
#endif /* End of #ifdef __cplusplus */

int32_t PlayerControlOnEvent(void* priv, EventCbType event, int32_t ext1, int32_t ext2);

uint64_t PlayerControlGetCurRelativeTime();

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif /* End of #ifdef __cplusplus */

#ifdef __cplusplus
namespace OHOS {
namespace Media {
void CondTimeWait(pthread_cond_t &cond, pthread_mutex_t &mutex, uint32_t delayUs);
void GetCurVideoSolution(FormatFileInfo &info, uint32_t &width, uint32_t &height);
AvCodecMime TransformCodecFormatToAvCodecMime(CodecFormat format);
std::string GetAudioNameByAvCodecMime(AvCodecMime mime);
void InitOutputBuffer(OutputInfo &outInfo, CodecType type);
bool IsValidPacket(FormatFrame &packet);
}
}
#endif

#endif  // LITEPLAYER_COMM_H
