 /*
 * Copyright (C) 2026 RKH Corp.
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

#ifndef PLAYER_SAMPLE_COMMON_H
#define PLAYER_SAMPLE_COMMON_H

#include "player_sample_common.h"
#include <pthread.h>
#include <queue>

#include "player_define.h"
#include "player_source.h"
#include "player_sink_manager.h"
#include "hi_demuxer.h"
#include "decoder.h"
#include "codec_interface.h"
#include "surface.h"

using namespace OHOS;
using namespace OHOS::Media;
using OHOS::Media::PlayerSource;
using OHOS::Media::Decoder;

struct CommonData {
    std::shared_ptr<PlayerSource> playerSource;
    std::shared_ptr<Decoder> videoDecoder;
    LayerFuncs *layerFuncs;
    uint32_t layerId;
    pthread_t threadSource;
    pthread_t threadDecoder;
    pthread_t threadSink;
    std::string fileName;
    std::queue<OutputInfo> frameBufferQueue;
    pthread_rwlock_t queueLock;
    int x;
    int y;
    int width;
    int height;
    bool isEos;
    bool isSourceStarted;
    bool isSourceEnd;
};

int InitPlayer(CommonData &data);
int DeinitPlayer(CommonData &data);

#endif // PLAYER_SAMPLE_COMMON_H