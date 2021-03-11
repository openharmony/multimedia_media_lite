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

#ifndef PLAYER_IMPL_H
#define PLAYER_IMPL_H

#include <memory>
#include <vector>
#include <mutex>
#include <pthread.h>
#include "buffer_source.h"
#include "hi_demuxer.h"
#include "liteplayer.h"
#include "player_define.h"
#include "player.h"
#include "source.h"

using namespace std;
using OHOS::Media::PlayerControl;
namespace OHOS {
namespace Media {
typedef enum {
    PAYERADP_EVT_PREPARED = 0,
    PAYERADP_EVT_PLAY_COMPLETE = 1,
    PAYERADP_EVT_REWIND_COMPLETE = 2,
    PAYERADP_EVT_VIDEO_SOLUTION = 3,
    PAYERADP_EVT_VIDEO_PLAYED = 4,
    PAYERADP_EVT_PLAY_TIME = 5,
    PAYERADP_EVT_ERROR           = 6,
    PAYERADP_EVT_INFO            = 7,
}PlayAdapterEvt;

class AdapterStreamCallback : public StreamCallback {
public:
    AdapterStreamCallback(std::shared_ptr<StreamSource> &stream, std::shared_ptr<BufferSource> &buffer);
    virtual ~AdapterStreamCallback();
    uint8_t* GetBuffer(size_t index);
    void QueueBuffer(size_t index, size_t offset, size_t size, int64_t timestampUs, uint32_t flags);
    void SetParameters(const Format &params);
    int Init(void);
    void DeInit(void);
private:
    static void* IdleBufferProcess(void* arg);

    std::weak_ptr<StreamSource> streamSource_;
    std::shared_ptr<BufferSource> bufferSource_;
    pthread_t streamProcess_;
    pthread_mutex_t mutex_;
    bool isRunning_;
};

class PlayerControl;
class Player::PlayerImpl {
friend class BufferSource;
friend struct StreamSource;

public:
    PlayerImpl();
    ~PlayerImpl();
    int32_t SetSource(const Source &source);
    int32_t Prepare();
    int32_t Play();
    bool IsPlaying();
    int32_t Pause();
    int32_t Stop();
    int32_t Rewind(int64_t mSeconds, int32_t mode);
    int32_t SetVolume(float leftVolume, float rightVolume);
    int32_t SetSurface(Surface *surface);
    bool IsLooping();
    int32_t GetPlayerState(int32_t &state);
    int32_t GetCurrentPosition(int64_t &currentPosition);
    int32_t GetDuration(int64_t &duration);
    int32_t GetVideoWidth(int32_t &videoWidth);
    int32_t GetVideoHeight(int32_t &videoHeight);
    int32_t SetAudioStreamType(int32_t type);
    void GetAudioStreamType(int32_t &type);
    int32_t Reset(void);
    int32_t Release();
    int32_t SetLoop(bool loop);
    void SetPlayerCallback(const std::shared_ptr<PlayerCallback> &cb);
    int32_t SetVideoScaleType(int32_t type);
    int32_t Init(void);
    int32_t DeInit(void);
private:
    void NotifySeekComplete(PlayerImpl *curPlayer);
    void NotifyPlaybackComplete(PlayerImpl *curPlayer);
    int32_t SetUriSource(const Source &source);
    int SetStreamSource(const Source &source);
    int32_t RewindInner(int64_t mSeconds, PlayerSeekMode mode);
    bool IsValidRewindMode(PlayerSeekMode mode);
    void UpdateState(PlayerImpl *curPlayer, PlayerStatus state);
    static void PlayerControlEventCb(void* pPlayer, PlayerControlEvent enEvent, const void* pData);
    int GetPlayer();
    static int32_t GetReadableSize(const void *handle);
    static int32_t ReadData(void *handle, uint8_t *data, int32_t size, int32_t timeOutMs, DataFlags *flags);
    int CreatePlayerParamCheck(PlayerControlParam &createParam);
    void GetDurationInner(int64_t &durationMs);
    void ResetInner(void);

    std::shared_ptr<PlayerControl> player_;
    float speed_;
    FormatFileInfo formatFileInfo_;
    PlayerControlStreamAttr mediaAttr_;
    PlayerStatus playerControlState_;

    bool isSingleLoop_;
    int64_t currentPosition_;
    int64_t rewindPosition_;
    Surface* surface_;
    uint32_t currentState_;
    PlayerSeekMode rewindMode_;
    PlayerSeekMode currentRewindMode_;
    int audioStreamType_;
    std::mutex lock_;
    std::shared_ptr<PlayerCallback> callback_;
    bool inited_;
    bool released_;
    bool isStreamSource_;
    QueBuffer buffer_;
    std::shared_ptr<BufferSource> bufferSource_;
    std::shared_ptr<AdapterStreamCallback> streamCallback_;
};
}  // namespace Media
}  // namespace OHOS

#endif  // PLAYER_IMPL_H
