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

#include "player_impl.h"
#include <cinttypes>
#include <climits>
#include <string>
#include <sys/prctl.h>
#include "unistd.h"
#include "securec.h"
#include "format_type.h"
#include "hi_liteplayer_err.h"
#include "hi_liteplayer.h"
#include "player_define.h"
#include "media_log.h"
extern "C"
{
#include "codec_interface.h"
#include "hal_display.h"
}

using namespace std;
using OHOS::Media::AdapterStreamCallback;

namespace OHOS {
namespace Media {
const int DEFAULT_SEND_LEVEL = 3;
const int INVALID_MEDIA_POSITION = -1;
const int DEFAULT_REWIND_TIME = 0;
const float DEFAULT_MEDIA_VOLUME = 100.0f;
const int INIT_VIDEO_SIZE = 0;
const int DEFAULT_THREAD_ID = -1;
const int32_t IDLE_QUEQUE_SLEEP_TIME_US = 5000;
const float MAX_MEDIA_VOLUME = 300.0f;

#define CHECK_FAILED_PRINT(value, target, printfString) \
do { \
    if ((value) != (target)) { \
        MEDIA_ERR_LOG("%s", printfString ? printfString : " "); \
    } \
} while (0)

#define CHECK_FAILED_RETURN(value, target, ret, printfString) \
do { \
    if ((value) != (target)) { \
        MEDIA_ERR_LOG("%s, ret:%d", printfString ? printfString : " ", ret); \
        return ret; \
    } \
} while (0)

#define CHK_NULL_RETURN(ptr) \
do { \
    if (ptr == nullptr) { \
        MEDIA_ERR_LOG("ptr null"); \
        return -1; \
    } \
} while (0)

Player::PlayerImpl::PlayerImpl()
    : player_(nullptr), speed_(1.0), playerControlState_(PLAY_STATUS_IDLE),
      isSingleLoop_(false),
      currentPosition_(INVALID_MEDIA_POSITION),
      rewindPosition_(INVALID_MEDIA_POSITION),
      surface_(nullptr),
      currentState_(PLAYER_IDLE),
      rewindMode_(PLAYER_SEEK_PREVIOUS_SYNC),
      currentRewindMode_(PLAYER_SEEK_PREVIOUS_SYNC),
      audioStreamType_(0),
      callback_(nullptr),
      inited_ (false),
      released_(false),
      isStreamSource_(false),
      bufferSource_(nullptr),
      streamCallback_(nullptr)
{
    (void)memset_s(&formatFileInfo_, sizeof(formatFileInfo_), 0, sizeof(FormatFileInfo));
    buffer_.idx = -1;
    buffer_.flag = 0;
    buffer_.offset = 0;
    buffer_.size = 0;
    buffer_.timestamp = 0;
    (void)memset_s(&mediaAttr_, sizeof(mediaAttr_), 0, sizeof(PlayerControlStreamAttr));
}

int32_t Player::PlayerImpl::Init(void)
{
    int ret;
    if (inited_ == true) {
        return 0;
    }
    ret = HalPlayerSysInit();
    if (ret != 0) {
        MEDIA_WARNING_LOG("SystemInit has been inited before Ret: %x", ret);
    }
    ret = CodecInit();
    if (ret != 0) {
        return ret;
    }
    if (memset_s(&buffer_, sizeof(QueBuffer), 0, sizeof(QueBuffer)) != EOK) {
        return -1;
    }
    buffer_.idx = -1;
    inited_ = true;
    MEDIA_INFO_LOG("process success");
    return 0;
}

int32_t Player::PlayerImpl::DeInit(void)
{
    if (inited_ != true) {
        return 0;
    }
    if (released_ == false) {
        Release();
    }
    inited_ = false;
    return 0;
}

Player::PlayerImpl::~PlayerImpl()
{
    DeInit();
    player_ = nullptr;
    MEDIA_INFO_LOG("~PlayerImpl process");
}

int32_t Player::PlayerImpl::SetSource(const Source &source)
{
    std::lock_guard<std::mutex> valueLock(lock_);
    MEDIA_INFO_LOG("process in");
    CHECK_FAILED_RETURN(released_, false, -1, "have released or not create");
    if (currentState_ != PLAYER_IDLE ) {
        MEDIA_ERR_LOG("current state is:%d, not support SetSource\n", currentState_);
        return -1;
    }
    GetPlayer();
    CHK_NULL_RETURN(player_);
    SourceType sType = source.GetSourceType();
    int32_t ret = -1;
    if (sType == SourceType::SOURCE_TYPE_FD) {
        MEDIA_ERR_LOG("not support fdSource now");
    } else if (sType == SourceType::SOURCE_TYPE_URI) {
        ret = SetUriSource(source);
    } else if (sType == SourceType::SOURCE_TYPE_STREAM) {
        ret = SetStreamSource(source);
    } else {
        MEDIA_ERR_LOG("SetSource failed, source type is %d", static_cast<int32_t>(sType));
    }
    return ret;
}

static void ShowFileInfo(const FormatFileInfo *fileInfo)
{
    for (int i = 0; i < HI_DEMUXER_RESOLUTION_CNT; i++) {
        const StreamResolution *resolution = &fileInfo->stSteamResolution[i];
        MEDIA_INFO_LOG("video[%d],w=%u,h=%u,index=%d ", i, resolution->u32Width,
            resolution->u32Height, resolution->s32VideoStreamIndex);
    }
    MEDIA_INFO_LOG("audio channel_cnt=%u,sampleRate=%u,AudioStreamIndex=%d videoIndex:%d",
        fileInfo->u32AudioChannelCnt, fileInfo->u32SampleRate, fileInfo->s32UsedAudioStreamIndex,
        fileInfo->s32UsedVideoStreamIndex);
}

void Player::PlayerImpl::UpdateState(PlayerImpl *curPlayer, PlayerStatus state)
{
    if (curPlayer == nullptr) {
        return;
    }
    switch (state) {
        case PLAY_STATUS_IDLE:
            curPlayer->currentState_ |= PLAYER_IDLE;
            break;
        case PLAY_STATUS_INIT:
            curPlayer->currentState_ |= PLAYER_INITIALIZED;
            break;
        case PLAY_STATUS_PREPARED:
            curPlayer->currentState_ |= PLAYER_PREPARED;
            break;
        case PLAY_STATUS_PLAY:
            curPlayer->currentState_ |= PLAYER_STARTED;
            break;
        case PLAY_STATUS_TPLAY:
            curPlayer->currentState_ |= PLAYER_STARTED;
            break;
        case PLAY_STATUS_PAUSE:
            curPlayer->currentState_ |= PLAYER_PAUSED;
            break;
        default:
            break;
    }
    curPlayer->playerControlState_ = state;
    MEDIA_INFO_LOG("@@player UpdateState, state:%d", state);
}

void Player::PlayerImpl::PlayerControlEventCb(void* pPlayer, PlayerControlEvent enEvent, const void* pData)
{
    PlayerControlError subErr = PLAYERCONTROL_ERROR_BUTT;
    PlayerImpl *curPlayer = (PlayerImpl *)pPlayer;

    if (curPlayer == nullptr) {
        MEDIA_ERR_LOG("the handle is error");
        return;
    }
    switch (enEvent) {
        case PLAYERCONTROL_EVENT_STATE_CHANGED:
            if (pData == nullptr) {
                return;
            }
            curPlayer->UpdateState(curPlayer, *reinterpret_cast<const PlayerStatus *>(pData));
            break;
        case PLAYERCONTROL_EVENT_EOF:
            MEDIA_INFO_LOG("end of file");
            curPlayer->NotifyPlaybackComplete(curPlayer);
            break;
        case PLAYERCONTROL_EVENT_SOF:
            MEDIA_INFO_LOG("start of file");
            break;
        case PLAYERCONTROL_EVENT_ERROR:
            if (pData == nullptr) {
                return;
            }
            subErr = *reinterpret_cast<const PlayerControlError *>(pData);
            MEDIA_ERR_LOG("error: %d", subErr);
            if (curPlayer->callback_ != nullptr) {
                curPlayer->callback_->OnError(0, subErr);
            }
            break;
        case PLAYERCONTROL_EVENT_PROGRESS:
            if (pData == nullptr) {
                return;
            }
            curPlayer->currentPosition_ = *reinterpret_cast<const int64_t *>(pData);
            break;
        case PLAYERCONTROL_EVENT_SEEK_END:
            if (pData == nullptr) {
                return;
            }
            MEDIA_INFO_LOG("seek action end, time is %lld",  *reinterpret_cast<const int64_t *>(pData));
            curPlayer->NotifySeekComplete(curPlayer);
            break;
        default:
            break;
    }
}

int32_t Player::PlayerImpl::Prepare()
{
    std::lock_guard<std::mutex> valueLock(lock_);
    int ret;
    MEDIA_INFO_LOG("process in");
    CHECK_FAILED_RETURN(released_, false, -1, "have released or not create");
    CHK_NULL_RETURN(player_);
    if (currentState_ != PLAYER_INITIALIZED) {
        MEDIA_ERR_LOG("Can not Prepare, currentState_ is %d\n", currentState_);
        return -1;
    }
    PlayerCtrlCallbackParam param;
    param.player = this;
    param.callbackFun = PlayerControlEventCb;
    ret = player_->RegCallback(param);
    if (ret != 0) {
        MEDIA_ERR_LOG("RegCallback exec failed ");
        return -1;
    }

    currentState_ = PLAYER_PREPARING;
    player_->Prepare();
    currentState_ = PLAYER_PREPARED;

    ret = player_->GetFileInfo(formatFileInfo_);
    if (ret != 0) {
        MEDIA_ERR_LOG("GetFileInfo failed");
        return ret;
    }
    ShowFileInfo(&formatFileInfo_);
    /* report video solution */
    for (int i = 0; i < HI_DEMUXER_RESOLUTION_CNT; i++) {
        if (formatFileInfo_.stSteamResolution[i].s32VideoStreamIndex == formatFileInfo_.s32UsedVideoStreamIndex) {
            if (callback_ != nullptr) {
                callback_->OnVideoSizeChanged(formatFileInfo_.stSteamResolution[i].u32Width,
                    formatFileInfo_.stSteamResolution[i].u32Height);
            }
        }
    }
    MEDIA_INFO_LOG("process out");
    return 0;
}

int32_t Player::PlayerImpl::Play()
{
    std::lock_guard<std::mutex> valueLock(lock_);
    int ret;
    MEDIA_INFO_LOG("PlayerImpl::%s process in\n", __func__);
    CHECK_FAILED_RETURN(released_, false, -1, "have released or not create");
    if (player_ == nullptr) {
        MEDIA_ERR_LOG("Play failed, player_ is null");
        return -1;
    }
    if (currentState_ == PLAYER_STARTED) {
        MEDIA_ERR_LOG("Can not Play, currentState_ is MEDIA_PLAYER_STARTED");
        return 0;
    }

    if (currentState_ != PLAYER_PREPARED && currentState_ != PLAYER_PLAYBACK_COMPLETE &&
        currentState_ != PLAYER_PAUSED) {
        MEDIA_ERR_LOG("Can not Play, currentState is %d", currentState_);
        return -1;
    }
    if (currentState_ == PLAYER_PAUSED) {
        goto play;
    }
    if (currentState_ == PLAYER_PLAYBACK_COMPLETE) {
        MEDIA_ERR_LOG("Can not Play, currentState_ is PLAYER_PLAYBACK_COMPLETE");
        return 0;
    }
    mediaAttr_.s32VidStreamId = formatFileInfo_.s32UsedVideoStreamIndex;
    mediaAttr_.s32AudStreamId = formatFileInfo_.s32UsedAudioStreamIndex;
    ret = player_->SetMedia(mediaAttr_);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetMedia  exec failed");
        return  ret;
    }

    for (int i = 0; i < HI_DEMUXER_RESOLUTION_CNT; i++) {
        StreamResolution *resolution = &formatFileInfo_.stSteamResolution[i];
        if (resolution->s32VideoStreamIndex == mediaAttr_.s32VidStreamId) {
            MEDIA_INFO_LOG("used video w=%u,h=%u,index=%d",
                resolution->u32Width, resolution->u32Height, mediaAttr_.s32VidStreamId);
            break;
        }
    }

play:
    ret = player_->Play();
    if (ret != 0) {
        MEDIA_ERR_LOG("Play exec failed %x", ret);
        return -1;
    }
    currentState_ = PLAYER_STARTED;
    MEDIA_INFO_LOG("process out");
    return 0;
}

bool Player::PlayerImpl::IsPlaying()
{
    std::lock_guard<std::mutex> valueLock(lock_);
    MEDIA_INFO_LOG("process in\n");
    CHECK_FAILED_RETURN(released_, false, -1, "have released or not create");
    bool isPlaying = false;
    if (player_ != nullptr) {
        isPlaying = (currentState_ != PLAYER_STARTED) ? false : true;
    }
    return isPlaying;
}

int32_t Player::PlayerImpl::Pause()
{
    std::lock_guard<std::mutex> valueLock(lock_);
    MEDIA_INFO_LOG("process in");
    CHECK_FAILED_RETURN(released_, false, -1, "have released or not create");
    CHK_NULL_RETURN(player_);
    if (currentState_ == PLAYER_PAUSED || currentState_ == PLAYER_PLAYBACK_COMPLETE) {
        MEDIA_ERR_LOG("currentState_ is %d", currentState_);
        return 0;
    }

    if (currentState_ != PLAYER_STARTED) {
        MEDIA_ERR_LOG("Can not Pause, currentState_ is %d", currentState_);
        return -1;
    }

    player_->Pause();
    currentState_ = PLAYER_PAUSED;
    return 0;
}

int32_t Player::PlayerImpl::Stop()
{
    std::lock_guard<std::mutex> valueLock(lock_);
    MEDIA_INFO_LOG("process in");
    CHECK_FAILED_RETURN(released_, false, -1, "have released or not create");
    if (currentState_ == PLAYER_STOPPED) {
        return 0;
    }
    if ((currentState_ != PLAYER_STARTED) && (currentState_ != PLAYER_PAUSED) &&
        (currentState_ != PLAYER_PLAYBACK_COMPLETE) && (currentState_ != PLAYER_STATE_ERROR)) {
        MEDIA_INFO_LOG("current state: %d, no need to do stop", currentState_);
        return 0;
    }

    if (player_ != nullptr) {
        int32_t ret = player_->Stop();
        if (ret != 0) {
            MEDIA_ERR_LOG("Stop failed, ret is %d", ret);
        }
    }
    currentState_ = PLAYER_STOPPED;
    MEDIA_INFO_LOG("process out");
    return 0;
}

int32_t Player::PlayerImpl::RewindInner(int64_t mSeconds, PlayerSeekMode mode)
{
    MEDIA_INFO_LOG("process in");
    CHK_NULL_RETURN(player_);
    if (mSeconds < DEFAULT_REWIND_TIME) {
        MEDIA_WARNING_LOG("Attempt to rewind to invalid position %lld", mSeconds);
        mSeconds = DEFAULT_REWIND_TIME;
    }
    int32_t ret;
    int64_t durationMs = -1;
    GetDurationInner(durationMs);
    if ((durationMs > DEFAULT_REWIND_TIME) && (mSeconds > durationMs)) {
        MEDIA_WARNING_LOG("Attempt to rewind to past end of file, request is %lld, durationMs is %lld", mSeconds,
            durationMs);
        return -1;
    }
    currentRewindMode_ = mode;
    if (rewindPosition_ >= DEFAULT_REWIND_TIME) {
        return 0;
    }

    rewindPosition_ = mSeconds;
    rewindMode_ = mode;
    ret = player_->Seek(mSeconds);
    if (ret != 0) {
        MEDIA_ERR_LOG("RewindInner failed, ret is %d", ret);
    }
    MEDIA_INFO_LOG("process out");
    return ret;
}

bool Player::PlayerImpl::IsValidRewindMode(PlayerSeekMode mode)
{
    switch (mode) {
        case PLAYER_SEEK_PREVIOUS_SYNC:
        case PLAYER_SEEK_NEXT_SYNC:
        case PLAYER_SEEK_CLOSEST_SYNC:
        case PLAYER_SEEK_CLOSEST:
        case PLAYER_SEEK_FRAME_INDEX:
            break;
        default:
            MEDIA_ERR_LOG("Unknown rewind mode %d", mode);
            return false;
    }
    return true;
}

int32_t Player::PlayerImpl::Rewind(int64_t mSeconds, int32_t mode)
{
    std::lock_guard<std::mutex> valueLock(lock_);
    MEDIA_INFO_LOG("process in");
    CHECK_FAILED_RETURN(released_, false, -1, "have released or not create");
    if (currentState_ != PLAYER_STARTED && currentState_ != PLAYER_PAUSED) {
        MEDIA_ERR_LOG("Can not Rewind, currentState_ is %d", currentState_);
        return -1;
    }

    if (IsValidRewindMode((PlayerSeekMode)mode) != true) {
        MEDIA_ERR_LOG("Rewind failed, msec is %lld, mode is %d", mSeconds, mode);
        return -1;
    }

    if (isStreamSource_ == true) {
        MEDIA_ERR_LOG("Failed, streamsource not support Rewind");
        return 0;
    }
    int32_t ret = RewindInner(mSeconds, (PlayerSeekMode)mode);
    if (ret != 0) {
        MEDIA_ERR_LOG("ReWind failed, ret is %d", ret);
    } else {
        currentPosition_ = mSeconds;
    }
    MEDIA_INFO_LOG("process out");
    return ret;
}

int32_t Player::PlayerImpl::SetVolume(float leftVolume, float rightVolume)
{
    std::lock_guard<std::mutex> valueLock(lock_);
    MEDIA_INFO_LOG("process in");
    VolumeAttr attr;
    CHECK_FAILED_RETURN(released_, false, -1, "have released or not create");
    CHK_NULL_RETURN(player_);
    if ((currentState_ != PLAYER_STARTED) && (currentState_ != PLAYER_PAUSED) &&
        (currentState_ != PLAYER_PREPARED)) {
        MEDIA_ERR_LOG("SetVolume failed, currentState_ is %d", currentState_);
        return -1;
    }
    if (leftVolume < 0 || leftVolume > MAX_MEDIA_VOLUME || rightVolume < 0 || rightVolume > MAX_MEDIA_VOLUME) {
        MEDIA_ERR_LOG("SetVolume failed, the volume should be set to a value ranging from 0 to 300");
        return -1;
    }
    attr.leftVolume = leftVolume;
    attr.rightVolume = rightVolume;
    int ret = player_->SetVolume(attr);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetVolume failed %x", ret);
    }
    MEDIA_INFO_LOG("process out\n");
    return ret;
}

int32_t Player::PlayerImpl::SetSurface(Surface *surface)
{
    std::lock_guard<std::mutex> valueLock(lock_);
    MEDIA_INFO_LOG("process in");
    CHECK_FAILED_RETURN(released_, false, -1, "have released or not create");
    if (currentState_ != PLAYER_PREPARED) {
        MEDIA_ERR_LOG("SetSurface failed, currentState_ is %d", currentState_);
        return -1;
    }
    surface_ = surface;
    player_->SetSurface(surface);
    return 0;
}

bool Player::PlayerImpl::IsLooping()
{
    std::lock_guard<std::mutex> valueLock(lock_);
    CHECK_FAILED_RETURN(released_, false, -1, "have released or not create");
    bool isLoop = (player_ == nullptr) ? false : isSingleLoop_;
    return isLoop;
}

int32_t Player::PlayerImpl::GetPlayerState(int32_t &state)
{
    std::lock_guard<std::mutex> valueLock(lock_);
    CHECK_FAILED_RETURN(released_, false, -1, "have released or not create");
    state = currentState_;
    return 0;
}

int32_t Player::PlayerImpl::GetCurrentPosition(int64_t &position)
{
    std::lock_guard<std::mutex> valueLock(lock_);
    MEDIA_INFO_LOG("process in");
    CHECK_FAILED_RETURN(released_, false, -1, "have released or not create");
    position = (currentPosition_ >= 0) ? currentPosition_ : -1;
    return 0;
}

void Player::PlayerImpl::GetDurationInner(int64_t &durationMs)
{
    FormatFileInfo formatInfo;
    int32_t ret = player_->GetFileInfo(formatInfo);
    if (ret != 0) {
        MEDIA_ERR_LOG("GetDuration failed, ret is %d", ret);
    }
    durationMs = (ret == 0) ? formatInfo.s64Duration : -1;
}

int32_t Player::PlayerImpl::GetDuration(int64_t &durationMs)
{
    std::lock_guard<std::mutex> valueLock(lock_);
    MEDIA_INFO_LOG("process in");
    CHECK_FAILED_RETURN(released_, false, -1, "have released or not create");
    CHK_NULL_RETURN(player_);
    if (currentState_ != PLAYER_PREPARED && currentState_ != PLAYER_STARTED && currentState_ != PLAYER_PAUSED &&
        currentState_ != PLAYER_STOPPED && currentState_ != PLAYER_PLAYBACK_COMPLETE) {
        durationMs = -1;
        MEDIA_ERR_LOG("Can not GetDuration, currentState_ is %d", currentState_);
        return -1;
    }
    GetDurationInner(durationMs);
    return 0;
}

int32_t Player::PlayerImpl::GetVideoWidth(int32_t &videoWidth)
{
    std::lock_guard<std::mutex> valueLock(lock_);
    MEDIA_INFO_LOG("process in");
    CHECK_FAILED_RETURN(released_, false, -1, "have released or not create");
    CHK_NULL_RETURN(player_);
    FormatFileInfo formatInfo;
    int32_t ret;
    videoWidth = 0;
    if (currentState_ != PLAYER_PREPARED && currentState_ != PLAYER_STARTED && currentState_ != PLAYER_PAUSED &&
        currentState_ != PLAYER_STOPPED && currentState_ != PLAYER_PLAYBACK_COMPLETE) {
        MEDIA_ERR_LOG("Can not GetVideoWidth, currentState_ is %d", currentState_);
        return -1;
    }
    CHECK_FAILED_PRINT(memset_s(&formatInfo, sizeof(formatInfo), 0, sizeof(FormatFileInfo)), 0, "memset failed");
    ret = player_->GetFileInfo(formatInfo);
    if (ret != 0) {
        MEDIA_ERR_LOG("GetFileInfo failed, ret is %d", ret);
        return ret;
    }
    if (formatFileInfo_.s32UsedVideoStreamIndex == -1) {
        return -1;
    }
    for (int i = 0; i < HI_DEMUXER_RESOLUTION_CNT; i++) {
        if (formatFileInfo_.stSteamResolution[i].s32VideoStreamIndex == formatFileInfo_.s32UsedVideoStreamIndex) {
            videoWidth = formatFileInfo_.stSteamResolution[i].u32Width;
            break;
        }
    }
    return 0;
}

int32_t Player::PlayerImpl::GetVideoHeight(int32_t &videoHeight)
{
    std::lock_guard<std::mutex> valueLock(lock_);
    MEDIA_INFO_LOG("process in");
    CHECK_FAILED_RETURN(released_, false, -1, "have released or not create");
    CHK_NULL_RETURN(player_);
    FormatFileInfo formatInfo;
    int32_t ret;
    videoHeight = 0;
    if (currentState_ != PLAYER_PREPARED && currentState_ != PLAYER_STARTED && currentState_ != PLAYER_PAUSED &&
        currentState_ != PLAYER_STOPPED && currentState_ != PLAYER_PLAYBACK_COMPLETE) {
        MEDIA_ERR_LOG("Can not GetVideoHeight, currentState_ is %d", currentState_);
        return -1;
    }
    CHECK_FAILED_PRINT(memset_s(&formatInfo, sizeof(formatInfo), 0, sizeof(FormatFileInfo)), 0, "memset failed");
    ret = player_->GetFileInfo(formatInfo);
    if (ret != 0) {
        MEDIA_ERR_LOG("GetFileInfo failed, ret is %d", ret);
        return ret;
    }
    if (formatFileInfo_.s32UsedVideoStreamIndex == -1) {
        return -1;
    }
    for (int i = 0; i < HI_DEMUXER_RESOLUTION_CNT; i++) {
        if (formatFileInfo_.stSteamResolution[i].s32VideoStreamIndex == formatFileInfo_.s32UsedVideoStreamIndex) {
            videoHeight = formatFileInfo_.stSteamResolution[i].u32Height;
            break;
        }
    }
    return ret;
}

int32_t Player::PlayerImpl::SetAudioStreamType(int32_t type)
{
    std::lock_guard<std::mutex> valueLock(lock_);
    MEDIA_INFO_LOG("process in");
    CHECK_FAILED_RETURN(released_, false, -1, "have released or not create");
    if (currentState_ == PLAYER_PREPARED || currentState_ == PLAYER_STARTED ||
        currentState_ == PLAYER_PAUSED || currentState_ == PLAYER_PLAYBACK_COMPLETE) {
        MEDIA_ERR_LOG("SetAudioStreamType called in state %d,type:%d",
            currentState_, type);
        return -1;
    }
    audioStreamType_ = type;
    return 0;
}

void Player::PlayerImpl::GetAudioStreamType(int32_t &type)
{
    std::lock_guard<std::mutex> valueLock(lock_);
    if (released_) {
        MEDIA_ERR_LOG("have released or not create");
        return;
    }
    type = static_cast<int32_t>(audioStreamType_);
}

void Player::PlayerImpl::ResetInner(void)
{
    isSingleLoop_ = false;
    if (player_ != nullptr) {
        if (currentState_ != PLAYER_IDLE && currentState_ != PLAYER_STOPPED) {
            CHECK_FAILED_PRINT(player_->Stop(), HI_SUCCESS, "stop failed");
        }
        (void)player_->Deinit();
        player_.reset();
        player_ = nullptr;
    }
    if (bufferSource_ != nullptr) {
        bufferSource_.reset();
        bufferSource_ = nullptr;
    }
    if (streamCallback_ != nullptr) {
        streamCallback_.reset();
        streamCallback_ = nullptr;
    }
    if (callback_ != nullptr) {
        callback_.reset();
        callback_ = nullptr;
    }
    currentState_ = PLAYER_IDLE;
    currentPosition_ = INVALID_MEDIA_POSITION;
    currentRewindMode_ = PLAYER_SEEK_PREVIOUS_SYNC;
    rewindPosition_ = INVALID_MEDIA_POSITION;
    rewindMode_ = PLAYER_SEEK_PREVIOUS_SYNC;
}

int32_t Player::PlayerImpl::Reset(void)
{
    std::lock_guard<std::mutex> valueLock(lock_);
    MEDIA_INFO_LOG("process in");
    CHECK_FAILED_RETURN(released_, false, -1, "have released or not create");
    if (currentState_ == PLAYER_IDLE) {
        return 0;
    }
    ResetInner();
    return 0;
}

int32_t Player::PlayerImpl::Release()
{
    std::lock_guard<std::mutex> valueLock(lock_);
    MEDIA_INFO_LOG("process in");
    CHECK_FAILED_RETURN(released_, false, -1, "have released or not create");
    ResetInner();
    released_ = true;
    return 0;
}

int Player::PlayerImpl::CreatePlayerParamCheck(PlayerControlParam &createParam)
{
    if (createParam.u32PlayPosNotifyIntervalMs < MIN_NOTIFY_INTERVAL_MS
        && createParam.u32PlayPosNotifyIntervalMs > 0) {
        MEDIA_ERR_LOG("notify interval small than min value %d",
            MIN_NOTIFY_INTERVAL_MS);
        return HI_ERR_PLAYERCONTROL_ILLEGAL_PARAM;
    }
    if ((createParam.u32VideoEsBufSize < AV_ESBUF_SIZE_MIN && createParam.u32VideoEsBufSize > 0)
        || createParam.u32VideoEsBufSize > VIDEO_ESBUF_SIZE_LIMIT) {
        MEDIA_ERR_LOG("video esbuffer illegal %u",
            createParam.u32VideoEsBufSize);
        return HI_ERR_PLAYERCONTROL_ILLEGAL_PARAM;
    }
    if ((createParam.u32AudioEsBufSize < AV_ESBUF_SIZE_MIN && createParam.u32AudioEsBufSize > 0)
        || createParam.u32AudioEsBufSize > AUDIO_ESBUF_SIZE_LIMIT) {
        MEDIA_ERR_LOG("audio esbuffer illegal %u",
            createParam.u32VideoEsBufSize);
        return HI_ERR_PLAYERCONTROL_ILLEGAL_PARAM;
    }
    if ((createParam.u32VdecFrameBufCnt < VDEC_VBBUF_CONUT_MIN) &&
        (createParam.u32VdecFrameBufCnt != 0)) {
        MEDIA_ERR_LOG("VDEC vb buffer count %u small than %d",
            createParam.u32VdecFrameBufCnt, VDEC_VBBUF_CONUT_MIN);
        return HI_ERR_PLAYERCONTROL_ILLEGAL_PARAM;
    }
    return 0;
}

int Player::PlayerImpl::GetPlayer()
{
    MEDIA_INFO_LOG("process in");
    PlayerControlParam playerParam;
    if (player_ != nullptr) {
        return 0;
    }
    if (memset_s(&playerParam, sizeof(PlayerControlParam), 0x00, sizeof(playerParam)) != EOK) {
        return -1;
    }

    playerParam.u32PlayPosNotifyIntervalMs = 300;
    if (CreatePlayerParamCheck(playerParam) != 0) {
        MEDIA_ERR_LOG("CreatePlayerParamCheck failed");
        return -1;
    }
    player_ = std::make_shared<PlayerControl>();
    if (player_ == nullptr || player_.get() == nullptr) {
        MEDIA_ERR_LOG("playerControl new failed");
        return HI_ERR_PLAYERCONTROL_MEM_MALLOC;
    }
    if (player_->Init(playerParam) != HI_SUCCESS) {
        MEDIA_ERR_LOG("playerControl init failed");
        return HI_ERR_PLAYERCONTROL_MEM_MALLOC;
    }
    currentState_ = PLAYER_INITIALIZED;
    MEDIA_INFO_LOG("GetPlayer success");
    return 0;
}

void Player::PlayerImpl::SetPlayerCallback(const std::shared_ptr<PlayerCallback> &cb)
{
    std::lock_guard<std::mutex> valueLock(lock_);
    MEDIA_INFO_LOG("process in");
    if (released_) {
        MEDIA_ERR_LOG("have released or not create");
        return;
    }
    callback_ = cb;
}

void Player::PlayerImpl::NotifyPlaybackComplete(PlayerImpl *curPlayer)
{
    if (curPlayer == nullptr) {
        return;
    }
    if (!isSingleLoop_) {
        curPlayer->currentState_ = PLAYER_PLAYBACK_COMPLETE;
        MEDIA_INFO_LOG("OnPlayBackComplete, iscallbackNull:%d", (curPlayer->callback_ != nullptr));
        if (curPlayer != nullptr && curPlayer->callback_ != nullptr) {
            curPlayer->callback_->OnPlaybackComplete();
        }
        return;
    }
    curPlayer->Rewind(0, PLAYER_SEEK_PREVIOUS_SYNC);
}

void Player::PlayerImpl::NotifySeekComplete(PlayerImpl *curPlayer)
{
    if (curPlayer == nullptr) {
        return;
    }
    if (curPlayer->rewindMode_ != curPlayer->currentRewindMode_) {
        curPlayer->rewindPosition_ = -1;
        curPlayer->rewindMode_ = PLAYER_SEEK_PREVIOUS_SYNC;
        curPlayer->RewindInner(curPlayer->currentPosition_, curPlayer->currentRewindMode_);
    } else {
        curPlayer->rewindPosition_ = -1;
        curPlayer->currentRewindMode_ = curPlayer->rewindMode_ = PLAYER_SEEK_PREVIOUS_SYNC;
    }
    if (curPlayer->callback_ != nullptr) {
        curPlayer->callback_->OnRewindToComplete();
    }
}

int32_t Player::PlayerImpl::SetLoop(bool loop)
{
    std::lock_guard<std::mutex> valueLock(lock_);
    CHECK_FAILED_RETURN(released_, false, -1, "have released or not create");
    CHK_NULL_RETURN(player_);
    isSingleLoop_ = loop;
    return 0;
}

int32_t Player::PlayerImpl::SetUriSource(const Source &source)
{
    MEDIA_INFO_LOG("process in");
    const std::string uri = source.GetSourceUri();
    if (uri.empty()) {
        MEDIA_ERR_LOG("SetUriSource failed, uri source do not set uri parameter");
        return -1;
    }
    char filePath[PATH_MAX];
    if (realpath(uri.c_str(), filePath) == nullptr) {
        MEDIA_ERR_LOG("Realpath input file failed");
        return -1;
    }
    if (access(filePath, R_OK) == -1) {
        MEDIA_ERR_LOG("No permission to read the file");
        return -1;
    }
    int32_t ret = player_->SetDataSource(uri.c_str());
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret is %d, uri is %s", ret, uri.c_str());
        return ret;
    }
    return 0;
}

AdapterStreamCallback::AdapterStreamCallback(std::shared_ptr<StreamSource> &stream,
    std::shared_ptr<BufferSource> &buffer)
    : streamProcess_(0),
      isRunning_(false)
{
    streamSource_ = stream;
    bufferSource_ = buffer;
    pthread_mutex_init(&mutex_, nullptr);
}

AdapterStreamCallback::~AdapterStreamCallback(void)
{
    DeInit();
    MEDIA_INFO_LOG("process out");
}

void* AdapterStreamCallback::IdleBufferProcess(void* arg)
{
    int ret;
    QueBuffer buffer;
    BufferInfo info;
    if (memset_s(&info, sizeof(info), 0x00, sizeof(info)) != EOK) {
        return nullptr;
    }
    AdapterStreamCallback *process = (AdapterStreamCallback*)arg;
    if (process == nullptr) {
        return nullptr;
    }

    prctl(PR_SET_NAME, "IdlbufProc", 0, 0, 0);
    MEDIA_INFO_LOG("process start");
    while (true) {
        pthread_mutex_lock(&process->mutex_);
        if (process->isRunning_ == false) {
            pthread_mutex_unlock(&process->mutex_);
            break;
        }
        pthread_mutex_unlock(&process->mutex_);
        if (process->bufferSource_ == nullptr) {
            MEDIA_ERR_LOG("bufferSource_ null break");
            break;
        }
        if (process->bufferSource_->GetIdleQueSize() == 0) {
            usleep(IDLE_QUEQUE_SLEEP_TIME_US);
            continue;
        }
        ret = process->bufferSource_->DequeIdleBuffer(&buffer, 0);
        if (ret == 0) {
            process->bufferSource_->GetBufferInfo(buffer.idx, &info);
            std::shared_ptr<StreamSource> stream = process->streamSource_.lock();
            if (stream == nullptr) {
                MEDIA_ERR_LOG("stream not exist break");
                break;
            }
            stream->OnBufferAvailable(buffer.idx, 0, info.bufLen);
        }
    };
    pthread_mutex_lock(&process->mutex_);
    process->isRunning_ = false;
    pthread_mutex_unlock(&process->mutex_);
    MEDIA_INFO_LOG("work end");
    return nullptr;
}

int AdapterStreamCallback::Init(void)
{
    MEDIA_INFO_LOG("process in");
    pthread_mutex_lock(&mutex_);
    isRunning_ = true;
    pthread_mutex_unlock(&mutex_);
    int32_t ret = pthread_create(&streamProcess_, nullptr, IdleBufferProcess, this);
    if (ret != 0) {
        MEDIA_ERR_LOG("pthread_create failed %d", ret);
        pthread_mutex_lock(&mutex_);
        isRunning_ = false;
        pthread_mutex_unlock(&mutex_);
        return -1;
    }
    return 0;
}

void AdapterStreamCallback::DeInit(void)
{
    MEDIA_INFO_LOG("process in");
    pthread_mutex_lock(&mutex_);
    isRunning_ = false;
    pthread_mutex_unlock(&mutex_);
    if (streamProcess_ != 0) {
        pthread_join(streamProcess_, nullptr);
    }
    pthread_mutex_destroy(&mutex_);
}

uint8_t* AdapterStreamCallback::GetBuffer(size_t index)
{
    BufferInfo info;
    if (bufferSource_ == nullptr) {
        MEDIA_ERR_LOG("bufferSource null");
        return nullptr;
    }
    if (bufferSource_->GetBufferInfo(index, &info) != 0) {
        MEDIA_ERR_LOG("GetBufferInfo failed");
        return nullptr;
    }
    return (uint8_t*)info.virAddr;
}

void AdapterStreamCallback::QueueBuffer(size_t index, size_t offset, size_t size, int64_t timestampUs, uint32_t flags)
{
    QueBuffer buffer;
    if (bufferSource_ == nullptr) {
        MEDIA_ERR_LOG("bufferSource null");
        return;
    }

    buffer.idx = index;
    buffer.flag = flags;
    buffer.offset = offset;
    buffer.size = size;
    buffer.timestamp = timestampUs;
    if (bufferSource_->QueFilledBuffer(&buffer) != 0) {
        MEDIA_ERR_LOG("QueFilledBuffer failed");
    }
}

void AdapterStreamCallback::SetParameters(const Format &params)
{
    MEDIA_ERR_LOG("process, not support");
}

int32_t Player::PlayerImpl::GetReadableSize(const void *handle)
{
    const PlayerImpl *playImpl = (const PlayerImpl*)handle;
    if (playImpl == nullptr) {
        MEDIA_ERR_LOG("handle null");
        return -1;
    }
    if (playImpl->bufferSource_ == nullptr) {
        MEDIA_ERR_LOG("bufferSource null");
        return -1;
    }
    return playImpl->bufferSource_->GetFilledQueDataSize();
}

int32_t Player::PlayerImpl::ReadData(void *handle, uint8_t *data, int32_t size, int32_t timeOutMs, DataFlags *flags)
{
    PlayerImpl *playImpl = (PlayerImpl*)handle;

    if (playImpl == nullptr || playImpl->bufferSource_ == nullptr) {
        MEDIA_ERR_LOG("bufferSource null");
        return -1;
    }
    if (data == nullptr || size < 0  || flags == nullptr) {
        MEDIA_ERR_LOG("data null or buffer size < 0");
        return -1;
    }
    BufferInfo info;
    int readLen;
    if (playImpl->buffer_.idx != -1) {
        goto READ_BUFFER_DATA;
    }

    if (playImpl->bufferSource_->GetFilledQueSize() <= 0) {
        return 0;
    }
    if (playImpl->bufferSource_->DequeFilledBuffer(&playImpl->buffer_, 0) != 0) {
        playImpl->buffer_.idx = -1;
        return 0;
    }
READ_BUFFER_DATA:
    if (playImpl->bufferSource_->GetBufferInfo(playImpl->buffer_.idx, &info) != 0) {
        return 0;
    }
    /* read all buffer data */
    if (playImpl->buffer_.size <= size) {
        if (playImpl->buffer_.size == 0 && playImpl->buffer_.flag == BUFFER_FLAG_EOS) {;
            playImpl->buffer_.offset = 0;
            playImpl->buffer_.size = info.size;
            playImpl->bufferSource_->QueIdleBuffer(&playImpl->buffer_);
            playImpl->buffer_.idx = -1;
            *flags = DATA_FLAG_EOS;
            return 0;
        }
        if (memcpy_s(data, size, (unsigned char*)(info.virAddr) + playImpl->buffer_.offset,
            playImpl->buffer_.size) != EOK) {
            return -1;
        }
        *flags = (playImpl->buffer_.flag == BUFFER_FLAG_EOS) ? DATA_FLAG_EOS : DATA_FLAG_PARTIAL_FRAME;
        readLen = playImpl->buffer_.size;
        playImpl->buffer_.offset = 0;
        playImpl->buffer_.size = info.size;
        playImpl->bufferSource_->QueIdleBuffer(&playImpl->buffer_);
        playImpl->buffer_.idx = -1;
    } else {
        if (memcpy_s(data, size, (unsigned char*)(info.virAddr) + playImpl->buffer_.offset, size) != EOK) {
            return -1;
        }
        playImpl->buffer_.offset += size;
        playImpl->buffer_.size -= size;
        *flags = DATA_FLAG_PARTIAL_FRAME;
        readLen = size;
    }
    return readLen;
}

int32_t Player::PlayerImpl::SetStreamSource(const Source &source)
{
    MEDIA_INFO_LOG("process in");
    std::string mimeType;
    Format format;
    isStreamSource_ = true;
    format.CopyFrom(source.GetSourceStreamFormat());
    if (format.GetStringValue(CODEC_MIME, mimeType) != true || mimeType.length() == 0) {
        MEDIA_ERR_LOG("get mime type failed");
        return -1;
    }
    if (strcmp(mimeType.c_str(), MIME_AUDIO_AAC) != 0) {
        MEDIA_ERR_LOG("mime_type[%s] error, current only support:%s", mimeType.c_str(), MIME_AUDIO_AAC);
        return -1;
    }
    bufferSource_ = std::make_shared<BufferSource>();
    if (bufferSource_ == nullptr) {
        MEDIA_ERR_LOG("new BufferSource failed");
        return -1;
    }
    bufferSource_->Init();
    std::shared_ptr<StreamSource> stream = source.GetSourceStream();
    streamCallback_ = std::make_shared<AdapterStreamCallback>(stream, bufferSource_);
    if (streamCallback_ == nullptr || streamCallback_.get() == nullptr) {
        MEDIA_ERR_LOG("new AdapterStreamCallback failed");
        return -1;
    }
    streamCallback_->Init();
    stream->SetStreamCallback(streamCallback_);

    BufferStream sourceTmp;
    sourceTmp.handle = this;
    sourceTmp.ReadData = ReadData;
    sourceTmp.GetReadableSize = GetReadableSize;
    int32_t ret = player_->SetDataSource(sourceTmp);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetDataSource  exec failed");
        return -1;
    }
    return 0;
}
}  // namespace Media
}  // namespace OHOS
