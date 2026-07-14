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

#include "liteplayer_comm.h"
#include <sys/time.h>
#include <unistd.h>
#ifdef __LITE__
#include <los_ld_elflib.h>
#else
#include <dlfcn.h>
#endif
#include "liteplayer.h"
#include "media_log.h"
#include "hi_liteplayer_err.h"

using OHOS::Media::PlayerControl;

const long long AV_NS2MS_SCALE = 1000000LL;
const long long AV_SEC2MS_SCALE = 1000LL;
const int32_t SS2US = 1000000;
/* microseconds to nanoseconds */
const int32_t US2NS = 1000;

int32_t PlayerControlOnEvent(void* priv, EventCbType event, int32_t ext1, int32_t ext2)
{
    PlayerControl *player = reinterpret_cast<PlayerControl *>(priv);
    if (player == nullptr) {
        return -1;
    }
    return player->OnPlayControlEvent(priv, event);
}

uint64_t PlayerControlGetCurRelativeTime()
{
    struct timespec ts = { 0, 0 };
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t curTime = (static_cast<uint64_t>(ts.tv_sec)) * AV_SEC2MS_SCALE +
        (static_cast<uint64_t>(ts.tv_nsec)) / AV_NS2MS_SCALE;
    return curTime;
}

namespace OHOS {
namespace Media {
struct CodecFormatAndMimePair {
    CodecFormat format;
    AvCodecMime mime;
};

static CodecFormatAndMimePair g_avCodecFormatInfo[CODEC_BUT + 1] = {
    {CODEC_H264, MEDIA_MIMETYPE_VIDEO_AVC},
    {CODEC_H265, MEDIA_MIMETYPE_VIDEO_HEVC},
    {CODEC_JPEG, MEDIA_MIMETYPE_IMAGE_JPEG},
    {CODEC_AAC, MEDIA_MIMETYPE_AUDIO_AAC},
    {CODEC_G711A, MEDIA_MIMETYPE_AUDIO_G711A},
    {CODEC_G711U, MEDIA_MIMETYPE_AUDIO_G711U},
    {CODEC_PCM, MEDIA_MIMETYPE_AUDIO_PCM},
    {CODEC_MP3, MEDIA_MIMETYPE_AUDIO_MP3},
    {CODEC_G726, MEDIA_MIMETYPE_AUDIO_G726},
    {CODEC_BUT, MEDIA_MIMETYPE_INVALID},
};

void CondTimeWait(pthread_cond_t &cond, pthread_mutex_t &mutex, uint32_t delayUs)
{
    uint32_t tmpUs;
    struct timeval ts;
    struct timespec outtime;

    ts.tv_sec = 0;
    ts.tv_usec = 0;
    gettimeofday(&ts, nullptr);
    ts.tv_sec += (delayUs / SS2US);
    tmpUs = delayUs % SS2US;

    if (ts.tv_usec + tmpUs > SS2US) {
        outtime.tv_sec = ts.tv_sec + 1;
        outtime.tv_nsec = ((ts.tv_usec + tmpUs) - SS2US) * US2NS;
    } else {
        outtime.tv_sec = ts.tv_sec;
        outtime.tv_nsec = (ts.tv_usec + tmpUs) * US2NS;
    }
    pthread_cond_timedwait(&cond, &mutex, &outtime);
}

void GetCurVideoSolution(FormatFileInfo &info, uint32_t &width, uint32_t &height)
{
    for (int i = 0; i < HI_DEMUXER_RESOLUTION_CNT; i++) {
        if (info.stStreamResolution[i].s32VideoStreamIndex == info.s32UsedVideoStreamIndex) {
            width = info.stStreamResolution[i].u32Width;
            height = info.stStreamResolution[i].u32Height;
            break;
        }
    }
}

AvCodecMime TransformCodecFormatToAvCodecMime(CodecFormat format)
{
    AvCodecMime mime = MEDIA_MIMETYPE_INVALID;
    uint32_t size = sizeof(g_avCodecFormatInfo) / sizeof(CodecFormatAndMimePair);

    for (uint32_t i = 0; i < size; i++) {
        if (g_avCodecFormatInfo[i].format == format) {
            mime = g_avCodecFormatInfo[i].mime;
            break;
        }
    }

    return mime;
}

std::string GetAudioNameByAvCodecMime(AvCodecMime mime)
{
    std::string audioName = "codec.unknow.soft.decoder";
    switch (mime) {
        case MEDIA_MIMETYPE_AUDIO_AAC:
            audioName = "codec.aac.soft.decoder";
            break;
        case MEDIA_MIMETYPE_AUDIO_MP3:
            audioName = "codec.mp3.soft.decoder";
            break;
        case MEDIA_MIMETYPE_AUDIO_PCM:
            audioName = "codec.pcm16s.soft.decoder";
            break;
        default:
            MEDIA_ERR_LOG("not support codec type:%d", mime);
            break;
    }
    return audioName;
}

void InitOutputBuffer(OutputInfo &outInfo, CodecType type)
{
#ifdef MEDIA_INTERFACE_V1_0
    outInfo.bufferCnt = 0;
    outInfo.buffers = nullptr;
    outInfo.timeStamp = -1;
    outInfo.sequence = 0;
    outInfo.flag = 0;
    outInfo.type = type;
    outInfo.vendorPrivate = nullptr;
#else
    outInfo.bufferCnt = 0;
    outInfo.timeStamp = -1;
    outInfo.flag = 0;
    (void)type;
#endif
}

bool IsValidPacket(FormatFrame &packet)
{
    return (packet.data != nullptr && packet.len != 0) ? true : false;
}

// util bigein
void PlayerControl::EventCallback(PlayerControlEvent event, const void *data)
{
    if (CheckIsNull(eventCallback_.callbackFun, "callbackFun nullptr")) {
        return;
    }
    eventCallback_.callbackFun(eventCallback_.player, event, data);
}

void PlayerControl::NotifyError(PlayerControlError playerError)
{
    EventCallback(PLAYERCONTROL_EVENT_ERROR, reinterpret_cast<void *>(&playerError));
}

void PlayerControl::StateChangeCallback(PlayerStatus state)
{
    EventCallback(PLAYERCONTROL_EVENT_STATE_CHANGED, reinterpret_cast<void *>(&state));
}

void PlayerControl::UpdateProgressNotify()
{
    if (CheckIsNull(stateMachine_, "stateMachine nullptr")) {
        return;
    }
    int64_t lastRendPts;
    PlayerStreamInfo streamInfo;
    uint64_t curTime = PlayerControlGetCurRelativeTime();

    if (!isPlayEnd_) {
        // First progress is not sended to reduce cpu
        lastNotifyTime_ = (!lastNotifyTime_) ? curTime : lastNotifyTime_;
        if (lastNotifyTime_ &&
            (curTime - lastNotifyTime_) < playerParam_.u32PlayPosNotifyIntervalMs) {
            return;
        }
    }
    int32_t ret = GetStreamInfo(streamInfo);
    if (ret != HI_SUCCESS) {
        MEDIA_ERR_LOG("GetStreamInfo failed , ret:%x", ret);
        return;
    }
    if (fmtFileInfo_.s32UsedAudioStreamIndex != HI_DEMUXER_NO_MEDIA_STREAM &&
        stateMachine_->GetCurState() != PLAY_STATUS_TPLAY) {
        lastRendPts = streamInfo.avStatus.syncStatus.lastAudPts;
        if (lastRendPts < fmtFileInfo_.s64StartTime) {
            return;
        }
        lastRendPts -= fmtFileInfo_.s64StartTime;
        if (!isAudPlayEos_) {
            EventCallback(PLAYERCONTROL_EVENT_PROGRESS, &lastRendPts);
        }
        lastNotifyTime_ = curTime;
        lastRendPos_ = lastRendPts;
    } else if (!isVidPlayEos_) {
        lastRendPts = streamInfo.avStatus.syncStatus.lastVidPts;
        if (lastRendPts < fmtFileInfo_.s64StartTime) {
            return;
        }
        lastRendPts -= fmtFileInfo_.s64StartTime;
        EventCallback(PLAYERCONTROL_EVENT_PROGRESS, &lastRendPts);
        lastNotifyTime_ = curTime;
        lastRendPos_ = lastRendPts;
    }
}

void PlayerControl::DealPlayEnd()
{
    if (CheckIsNull(stateMachine_, "stateMachine nullptr")) {
        return;
    }
    PlayerStatus playState = stateMachine_->GetCurState();
    if (tplayAttr_.direction == TPLAY_DIRECT_BACKWARD && playState == PLAY_STATUS_TPLAY) {
        EventCallback(PLAYERCONTROL_EVENT_SOF, nullptr);
    } else {
        if (fmtFileInfo_.s64Duration != -1) {
            EventCallback(PLAYERCONTROL_EVENT_PROGRESS, &fmtFileInfo_.s64Duration);
        }
        EventCallback(PLAYERCONTROL_EVENT_EOF, nullptr);
    }
}

PlayerTplayMode PlayerControl::TPlayGetPlayMode()
{
    PlayerTplayMode tplayMode = PLAYER_TPLAY_ONLY_I_FRAME;
    StreamResolution resolution = { 0 };
    if (GetVideoResolution(fmtFileInfo_.s32UsedVideoStreamIndex, resolution) != HI_SUCCESS) {
        MEDIA_ERR_LOG("GetVideoResolution failed");
        return PLAYER_TPLAY_ONLY_I_FRAME;
    }

    if (tplayAttr_.direction == TPLAY_DIRECT_FORWARD && tplayAttr_.speed == PLAY_SPEED_2X_FAST) {
        if ((resolution.u32Width * resolution.u32Height) <= FULL_TPLAY_RESULITON_LIMIT &&
            fmtFileInfo_.fFrameRate <= FULL_TPLAY_FRAMERATE_LIMIT &&
            fmtFileInfo_.u32Bitrate <= FULL_TPLAY_BITRATE_LIMIT) {
            tplayMode = PLAYER_TPLAY_FULL_PLAY;
        }
    }
    return tplayMode;
}

int32_t PlayerControl::TPlayGetSeekOffset(float playSpeed, TplayDirect direction)
{
    int32_t seekOffset = 0;
    switch (static_cast<int>(playSpeed)) {
        case PLAY_SPEED_2X_FAST:
            seekOffset = static_cast<int32_t>(TPLAY_SEEK_OFFSET_2X);
            break;
        case PLAY_SPEED_4X_FAST:
            seekOffset = static_cast<int32_t>(TPLAY_SEEK_OFFSET_4X);
            break;
        case PLAY_SPEED_8X_FAST:
            seekOffset = static_cast<int32_t>(TPLAY_SEEK_OFFSET_8X);
            break;
        case PLAY_SPEED_16X_FAST:
            seekOffset = static_cast<int32_t>(TPLAY_SEEK_OFFSET_16X);
            break;
        case PLAY_SPEED_32X_FAST:
            seekOffset = static_cast<int32_t>(TPLAY_SEEK_OFFSET_32X);
            break;
        case PLAY_SPEED_64X_FAST:
            seekOffset = static_cast<int32_t>(TPLAY_SEEK_OFFSET_64X);
            break;
        case PLAY_SPEED_128X_FAST:
            seekOffset = static_cast<int32_t>(TPLAY_SEEK_OFFSET_128X);
            break;
        default:
            MEDIA_ERR_LOG("unsupporteded play speed: %f", playSpeed);
            break;
    }
    seekOffset = (direction == TPLAY_DIRECT_BACKWARD) ? (-seekOffset) : seekOffset;
    return seekOffset;
}

int32_t PlayerControl::TPlayResetBuffer()
{
    int32_t ret;
    PlayerStreamInfo streamInfo;

    if (memset_s(&streamInfo, sizeof(streamInfo), 0x00, sizeof(PlayerStreamInfo)) != EOK) {
        return HI_FAILURE;
    }
    ret = GetStreamInfo(streamInfo);
    if (CheckIsFailed(ret, HI_SUCCESS, "GetStreamInfo failed")) {
        return ret;
    }
    lastReadPktPts_ = currentPosition_;
    isTplayLastFrame_ = false;
    ClearCachePacket();
    ret = DecoderAndSinkReset();
    if (CheckIsFailed(ret, HI_SUCCESS, "DecoderAndSinkReset failed")) {
        return ret;
    }
    return HI_SUCCESS;
}

int32_t PlayerControl::TPlayCheckContinueLost()
{
    int32_t ret = HI_SUCCESS;
    if (isVidContinueLost_) {
        if (curSeekOffset_ < INT32_MAX / OFFSET_INCREASE_FOR_FRAME_LOST) {
            curSeekOffset_ *= OFFSET_INCREASE_FOR_FRAME_LOST;
        }
        MEDIA_ERR_LOG("vid dec frame slow, increase seekoffset to %d", curSeekOffset_);
        ret = TPlayResetBuffer();
        if (CheckIsFailed(ret, HI_SUCCESS, "TPlayResetBuffer failed")) {
            return ret;
        }
        isVidContinueLost_ = false;
    }
    return HI_SUCCESS;
}

bool PlayerControl::TPlayIsFileReadEnd()
{
    if (lastReadPktPts_ == 0 && tplayAttr_.direction == TPLAY_DIRECT_BACKWARD) {
        MEDIA_DEBUG_LOG("backward last seek pts %lld", lastReadPktPts_);
        return true;
    } else if (isTplayLastFrame_ == true && (tplayAttr_.direction == TPLAY_DIRECT_FORWARD)) {
        MEDIA_DEBUG_LOG("forward last seek pts %lld fmtFileInfo_.s64Duration:%lld", lastReadPktPts_,
            fmtFileInfo_.s64Duration);
        return true;
    }
    return false;
}

int32_t PlayerControl::SeekInTplayMode(int64_t seekTimeInMs, FormatSeekMode seekFlag)
{
    if (CheckIsNull(playerSource_, "playerSource_ nullptr")) {
        return HI_FAILURE;
    }
    int32_t ret = playerSource_->Seek(fmtFileInfo_.s32UsedVideoStreamIndex, seekTimeInMs, seekFlag);
    if (ret != HI_SUCCESS) {
        // if forward tplay not find the last i frame, then seek again for backword
        if (tplayAttr_.direction == TPLAY_DIRECT_FORWARD) {
            seekFlag = FORMAT_SEEK_MODE_FORWARD_KEY;
            isTplayLastFrame_ = true;
            ret = playerSource_->Seek(fmtFileInfo_.s32UsedVideoStreamIndex, seekTimeInMs, seekFlag);
        }
        if (ret != HI_SUCCESS) {
            MEDIA_DEBUG_LOG("playerSource_ seek failed maybe seek to file end, ret:%d", ret);
            /* read end */
            return HI_RET_FILE_EOF;
        }
    }
    return ret;
}
int32_t PlayerControl::TPlayBeforeFrameRead()
{
    int32_t ret = HI_SUCCESS;
    if (tplayMode_ != PLAYER_TPLAY_ONLY_I_FRAME) {
        return ret;
    }

    ret = TPlayCheckContinueLost();
    if (ret != HI_SUCCESS) {
        return ret;
    }
    /* last packet should be skip if streamidx is not playing video stream */
    if (lastReadPktStrmIdx_ == (uint32_t)fmtFileInfo_.s32UsedVideoStreamIndex) {
        int64_t seekTimeInMs = lastReadPktPts_ + curSeekOffset_;
        if (TPlayIsFileReadEnd()) {
            return HI_RET_FILE_EOF;
        }
        FormatSeekMode seekFlag = (tplayAttr_.direction == TPLAY_DIRECT_BACKWARD) ? FORMAT_SEEK_MODE_BACKWARD_KEY :
            FORMAT_SEEK_MODE_FORWARD_KEY;
        if (seekTimeInMs < 0 && tplayAttr_.direction == TPLAY_DIRECT_BACKWARD) {
            seekTimeInMs = 0;
            isTplayLastFrame_ = true;
        } else if (seekTimeInMs > fmtFileInfo_.s64Duration && tplayAttr_.direction == TPLAY_DIRECT_FORWARD) {
            seekTimeInMs = fmtFileInfo_.s64Duration;
            seekFlag = FORMAT_SEEK_MODE_BACKWARD_KEY;
            isTplayLastFrame_ = true;
        } else if (lastReadPktPts_ == 0 && isTplayStartRead_ == false) {
            seekTimeInMs = 0;
            seekFlag = FORMAT_SEEK_MODE_BACKWARD_KEY;
            isTplayLastFrame_ = false;
        }
        ret = SeekInTplayMode(seekTimeInMs, seekFlag);
        if (ret != HI_SUCCESS) {
            return ret;
        }
    }
    return HI_SUCCESS;
}

int32_t PlayerControl::TPlayAfterFrameRead(FormatFrame &packet)
{
    int32_t ret = HI_SUCCESS;
    bool isSkipPkt = false;

    if ((int)packet.trackId == fmtFileInfo_.s32UsedVideoStreamIndex) {
        if ((packet.timestampUs == lastSendPktPts_) && (isTplayStartRead_ == true)) {
            lastReadPktPts_ += curSeekOffset_;
            isSkipPkt = true;
        } else {
            lastReadPktPts_ = packet.timestampUs;
        }
    } else {
        lastReadPktPts_ = packet.timestampUs;
        isSkipPkt = true;
    }
    if (isSkipPkt) {
        ret = HI_RET_SKIP_PACKET;
    }
    return ret;
}

void PlayerControl::FlushDecoder(void)
{
    ReleaseADecoderOutputFrame();
    ReleaseVDecoderOutputFrame();
    if (audioDecoder_ != nullptr) {
        audioDecoder_->FlushDec();
    }
    if (videoDecoder_ != nullptr) {
        videoDecoder_->FlushDec();
    }
}

int32_t PlayerControl::DecoderAndSinkReset(void)
{
    int32_t ret;
    bool isNeedResume = false;
    ret = sinkManager_->Pause();
    if ((ret == HI_SUCCESS) && (paused_ == false)) {
        isNeedResume = true;
    }
    ret = sinkManager_->Reset();
    if (ret != HI_SUCCESS) {
        MEDIA_ERR_LOG("m_render reset failed");
        (void)sinkManager_->Resume();
        return ret;
    }
    FlushDecoder();
    if (isNeedResume) {
        ret = sinkManager_->Resume();
        if (CheckIsFailed(ret, HI_SUCCESS, "sinkManager_ Resume failed")) {
            return HI_FAILURE;
        }
    }
    return HI_SUCCESS;
}

int32_t PlayerControl::AyncSeek(int64_t seekTime)
{
    if (CheckIsNull(playerSource_, "playerSource_ nullptr")) {
        return HI_FAILURE;
    }
    int64_t seekTimeInMs = seekTime;
    int32_t ret = DecoderAndSinkReset();
    if (CheckIsFailed(ret, HI_SUCCESS, "DecoderAndSinkReset failed")) {
        return ret;
    }
    ClearCachePacket();
    if (fmtFileInfo_.s32UsedVideoStreamIndex != HI_DEMUXER_NO_MEDIA_STREAM) {
        ret = playerSource_->Seek(fmtFileInfo_.s32UsedVideoStreamIndex, seekTimeInMs, FORMAT_SEEK_MODE_BACKWARD_KEY);
        if (ret != HI_SUCCESS) {
            MEDIA_INFO_LOG("exec fmt_seek video stream failed, ret:%d", ret);
            seekTimeInMs = currentPosition_;
        }
    } else if (fmtFileInfo_.s32UsedAudioStreamIndex != HI_DEMUXER_NO_MEDIA_STREAM) {
        ret = playerSource_->Seek(fmtFileInfo_.s32UsedAudioStreamIndex, seekTimeInMs, FORMAT_SEEK_MODE_BACKWARD_KEY);
        if (ret != HI_SUCCESS) {
            MEDIA_INFO_LOG("exec fmt_seek audio stream failed, ret:%d", ret);
            seekTimeInMs = currentPosition_;
        }
    }
    currentPosition_ = seekTimeInMs;

    if (tplayAttr_.speed != 1.0f) {
        lastReadPktPts_ = currentPosition_;
        isTplayStartRead_ = (currentPosition_ == 0) ? false : true;
        isTplayLastFrame_ = false;
    }

    EventCallback(PLAYERCONTROL_EVENT_PROGRESS, &currentPosition_);
    EventCallback(PLAYERCONTROL_EVENT_SEEK_END, reinterpret_cast<void *>(&seekTimeInMs));
    return HI_SUCCESS;
}

void PlayerControl::GetPlayElementEventCallBack(PlayEventCallback &callback)
{
    callback.onEventCallback = PlayerControlOnEvent;
    callback.priv = reinterpret_cast<void *>(this);
}
int32_t PlayerControl::SyncPrepare()
{
    int ret;

    playerSource_ = std::make_shared<PlayerSource>();
    if (CheckIsNull(playerSource_, "new playerSource_ nullptr")) {
        return HI_FAILURE;
    }
    playerSource_->Init();

    if (sourceType_ == SOURCE_TYPE_FD) {
        playerSource_->SetSource(fd_);
    } else if (sourceType_ == SOURCE_TYPE_STREAM) {
        playerSource_->SetSource(stream_);
    } else {
        playerSource_->SetSource(filePath_.c_str());
    }

    PlayEventCallback callback;
    GetPlayElementEventCallBack(callback);
    ret = playerSource_->SetCallBack(callback);
    if (CheckIsFailed(ret, HI_SUCCESS, "SetCallBack failed")) {
        return ret;
    }

    ret = playerSource_->Prepare();
    if (CheckIsFailed(ret, HI_SUCCESS, "Prepare failed")) {
        return ret;
    }
    ret = playerSource_->GetFileInfo(fmtFileInfo_);
    if (CheckIsFailed(ret, HI_SUCCESS, "GetFileInfo failed")) {
        return ret;
    }
    MEDIA_INFO_LOG("used audiostream index %d", fmtFileInfo_.s32UsedAudioStreamIndex);
    MEDIA_INFO_LOG("used videostream index %d", fmtFileInfo_.s32UsedVideoStreamIndex);
    return HI_SUCCESS;
}

bool PlayerControl::IsPlayEos()
{
    if (CheckIsNull(stateMachine_, "stateMachine_ nullptr")) {
        return false;
    }
    PlayerStatus playerState = stateMachine_->GetCurState();
    if (playerState == PLAY_STATUS_TPLAY && hasRenderVideoEos_) {
        return true;
    }
    if ((!isAudioStarted_ || hasRenderAudioEos_) && (!isVideoStarted_ || hasRenderVideoEos_)) {
        return true;
    }
    return false;
}

int32_t PlayerControl::CheckMediaType(FormatFileInfo &fmtFileInfo)
{
    if (fmtFileInfo.s32UsedVideoStreamIndex == HI_DEMUXER_NO_MEDIA_STREAM) {
        return HI_SUCCESS;
    }
    if ((fmtFileInfo.enVideoType == CODEC_H264)
        || (fmtFileInfo.enVideoType == CODEC_H265)
        || (fmtFileInfo.enVideoType == CODEC_JPEG)) {
        return HI_SUCCESS;
    }
    MEDIA_ERR_LOG("video type: %d not supported", fmtFileInfo.enVideoType);
    return HI_ERR_PLAYERCONTROL_NOT_SUPPORT;
}

int32_t PlayerControl::Invoke(PlayerInvoke invokeId, void *param)
{
    if (CheckIsNull(stateMachine_, "stateMachine_ nullptr")) {
        return HI_ERR_PLAYERCONTROL_NULL_PTR;
    }
    MsgInfo msg;
    InvokeParameter invokeParam;
    invokeParam.id = invokeId;
    invokeParam.param = param;

    msg.what = PLAYERCONTROL_MSG_INVOKE;
    msg.msgData = &invokeParam;
    msg.msgDataLen = sizeof(InvokeParameter);
    return stateMachine_->Send(msg);
}

int32_t PlayerControl::EnablePauseAfterPlay(bool pauseAfterPlay)
{
    PlayerStatus playerState = stateMachine_->GetCurState();
    if (playerState != PLAY_STATUS_IDLE && playerState != PLAY_STATUS_INIT) {
        MEDIA_ERR_LOG("unsupported set play mode, state:%d\n", playerState);
        return -1;
    }
    pauseMode_ = pauseAfterPlay;
    return 0;
}

int32_t PlayerControl::DoInvoke(InvokeParameter& invokeParam)
{
    int32_t ret = -1;

    switch (invokeParam.id) {
        case INVOKE_ENABLE_PAUSE_AFTER_PLAYER:
            if (invokeParam.param == nullptr) {
                break;
            }
            ret = EnablePauseAfterPlay((*((uint32_t *)invokeParam.param)) > 0 ? true : false);
            break;
        default:
            MEDIA_ERR_LOG("unsupported invoke:0x%x\n", invokeParam.id);
            break;
    }
    return ret;
}

int32_t PlayerControl::SetAudioStreamType(int32_t type)
{
    if (CheckIsNull(stateMachine_, "stateMachine_ nullptr")) {
        return HI_ERR_PLAYERCONTROL_NULL_PTR;
    }
    MsgInfo msg;
    int32_t audioStreamType = type;

    msg.what = PLAYERCONTROL_MSG_SET_AUDIOSTREAM_TYPE;
    msg.msgData = &audioStreamType;
    msg.msgDataLen = sizeof(int32_t);
    return stateMachine_->Send(msg);
}

int32_t PlayerControl::DoSetAudioStreamType(int32_t type)
{
    audioStreamType_ = type;
    if (sinkManager_ != nullptr) {
        sinkManager_->SetAudioStreamType(type);
    }
    return 0;
}

int32_t PlayerControl::SetLayerPriority(uint32_t priority)
{
    if (sinkManager_ != nullptr) {
        sinkManager_->SetParam(LAYER_PRIORITYS, DATA_TYPE_U32, &priority);
    }
    return 0;
}
}
}
