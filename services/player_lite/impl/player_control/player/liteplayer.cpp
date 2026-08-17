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

#include "liteplayer.h"

#include "hi_liteplayer_err.h"
#include "liteplayer_comm.h"
#include <sys/prctl.h>
#include <unistd.h>
#include "decoder.h"

using OHOS::Media::PlayerSource;
using OHOS::Media::SinkManager;
using OHOS::Media::Decoder;

namespace OHOS {
namespace Media {
/* DEFAULT_VID_BUFSIZE: 2 * 1024 * 1024 */
const uint32_t DEFAULT_VID_BUFSIZE = 2097152;
/* DEFAULT_AUD_BUFSIZE: 256 * 1024 */
const uint32_t DEFAULT_AUD_BUFSIZE = 262144;

const uint32_t GET_BUFFER_TIMEOUT_MS = 0u;
const uint32_t RENDER_FULL_SLEEP_TIME_US = 5000u;
const uint32_t RENDER_EOS_SLEEP_TIME_US = 3000000u;
const uint32_t PAUSE_WAIT_TIME_US = 10000u;
const uint32_t DECODER_DEFAULT_WIDTH = 1920;
const uint32_t DECODER_DEFAULT_HEIGHT = 1080;
const uint32_t QUEUE_BUFFER_FULL_SLEEP_TIME_US = 5000u;
const uint32_t NO_DATA_READ_SLEEP_TIME_US = 5000u;
const uint32_t MAX_EVENT_MESSAGE_NUM = 128;
const uint32_t MAX_AUDIO_SEND_FRAME_FULL_NUM = 10;

namespace {
    /* playing position notify interval in ms */
    const uint32_t DEFAULT_POS_NOFITY_INTERVAL = 300;
}

PlayerControl::PlayerControl() : stateMachine_(nullptr), observer_(nullptr), isInited_(false), isNeedPause_(false),
    pauseMode_(false), isPlayEnd_(false), isVidPlayEos_(false),
    isAudPlayEos_(false), cachedPacket_(false), lastRendPos_(0), lastNotifyTime_(0),
    tplayMode_(PLAYER_TPLAY_ONLY_I_FRAME), lastReadPktStrmIdx_(-1), lastReadPktPts_(0), lastSendPktPts_(0),
    curSeekOffset_(0), isVideoStarted_(false), isAudioStarted_(false), isVidContinueLost_(false),
    strmReadEnd_(false), isPlayErr_(false), isTplayLastFrame_(false), isTplayStartRead_(false),
    lastSendVdecPts_(AV_INVALID_PTS), lastSendAdecPts_(AV_INVALID_PTS), currentPosition_(0), paused_(false),
    schThreadExit_(false), loop_(false), hasRenderAudioEos_(false), hasRenderVideoEos_(false), renderSleepTime_(0),
    leftVolume_(-1.0f), rightVolume_(-1.0f), schProcess_(0), seekToTimeMs_(-1), firstAudioFrameAfterSeek_(false),
    firstVideoFrameAfterSeek_(false), sourceType_(SOURCE_TYPE_BUT), fd_(-1), playerSource_(nullptr),
    sinkManager_(nullptr), audioDecoder_(nullptr), videoDecoder_(nullptr), audioStreamType_(0), continuousAudFull_(0),
    isPaused_(false)
{
    eventCallback_.player = nullptr;
    eventCallback_.callbackFun = nullptr;
    (void)memset_s(&fmtFileInfo_, sizeof(fmtFileInfo_), 0, sizeof(FormatFileInfo));
    fmtFileInfo_.s32UsedAudioStreamIndex = HI_DEMUXER_NO_MEDIA_STREAM;
    fmtFileInfo_.s32UsedVideoStreamIndex = HI_DEMUXER_NO_MEDIA_STREAM;
    playerParam_.u32PlayPosNotifyIntervalMs = DEFAULT_POS_NOFITY_INTERVAL;
    playerParam_.u32VideoEsBufSize = DEFAULT_POS_NOFITY_INTERVAL;
    playerParam_.u32AudioEsBufSize = DEFAULT_POS_NOFITY_INTERVAL;
    playerParam_.u32VdecFrameBufCnt = DEFAULT_POS_NOFITY_INTERVAL;
    playerParam_.bPauseMode = DEFAULT_POS_NOFITY_INTERVAL;
    (void)memset_s(&formatPacket_, sizeof(formatPacket_), 0, sizeof(FormatFrame));
    tplayAttr_.direction = TPLAY_DIRECT_BUTT;
    tplayAttr_.speed = 1.0;
    streamAttr_.playMode = RENDER_MODE_NORMAL;
    streamAttr_.videoBufSize = 0;
    streamAttr_.audioBufSize = 0;
    vdecAttr_.type = 0;
    vdecAttr_.maxWidth = 0;
    vdecAttr_.maxHeight = 0;
    vdecAttr_.frameBufCnt = 0;
    vdecAttr_.errRecover = 0;
    adecAttr_.type = 0;
    adecAttr_.sampleRate = 0;
    adecAttr_.channel = 0;
    adecAttr_.bitWidth = 0;
    stream_.ReadData = nullptr;
    stream_.GetReadableSize = nullptr;
    stream_.handle = nullptr;
    surface_ = nullptr;
    eventQueue.clear();
}

PlayerControl::~PlayerControl()
{
    (void)Deinit();
    if (stateMachine_ !=  nullptr) {
        delete stateMachine_;
        stateMachine_ = nullptr;
    }
    if (observer_ !=  nullptr) {
        delete observer_;
        observer_ = nullptr;
    }
    eventQueue.clear();
}

PlayerStatus PlayerControl::GetState(void)
{
    if (CheckIsNull(stateMachine_, "stateMachine_ nullptr")) {
        return PLAY_STATUS_BUTT;
    }
    return stateMachine_->GetCurState();
}

int32_t PlayerControl::InitAttr(const PlayerControlParam &createParam)
{
    if (memset_s(&fmtFileInfo_, sizeof(fmtFileInfo_), 0x00, sizeof(FormatFileInfo)) != EOK) {
        MEDIA_ERR_LOG("set fmtFileInfo_ fail\n");
        return HI_ERR_PLAYERCONTROL_MEM_SET;
    }
    fmtFileInfo_.s32UsedAudioStreamIndex = HI_DEMUXER_NO_MEDIA_STREAM;
    fmtFileInfo_.s32UsedVideoStreamIndex = HI_DEMUXER_NO_MEDIA_STREAM;
    streamAttr_.playMode = RENDER_MODE_NORMAL;
    streamAttr_.videoBufSize = DEFAULT_VID_BUFSIZE;
    streamAttr_.audioBufSize = DEFAULT_AUD_BUFSIZE;

    lastReadPktPts_ = 0;
    lastSendPktPts_ = 0;
    pauseMode_ = createParam.bPauseMode;
    (void)CheckIsFailed(memcpy_s(&playerParam_, sizeof(playerParam_), &createParam,
        sizeof(PlayerControlParam)), EOK, "copy playerParam_ fail");
    playerParam_.u32PlayPosNotifyIntervalMs = (createParam.u32PlayPosNotifyIntervalMs == 0) ?
        DEFAULT_POS_NOFITY_INTERVAL : createParam.u32PlayPosNotifyIntervalMs;
    return HI_SUCCESS;
}

int32_t PlayerControl::Init(const PlayerControlParam &createParam)
{
    int32_t ret;
    MEDIA_INFO_LOG("Init in");
    if (CheckIsFailed(isInited_, false, "already be inited")) {
        return HI_SUCCESS;
    }
    if (CheckIsFailed(InitAttr(createParam), HI_SUCCESS, "InitAttr failed")) {
        return HI_FAILURE;
    }
    stateMachine_ = new(std::nothrow) PlayerControlStateMachine(*this);
    if (CheckIsNull(stateMachine_, "new PlayerControlStateMachine failed")) {
        return HI_FAILURE;
    }
    observer_ = new(std::nothrow) PlayerControlSMObserver(*this);
    if (observer_ == nullptr) {
        MEDIA_ERR_LOG("new PlayerControlSMObserver failed\n");
        goto DEL_STATE;
    }
    (void)stateMachine_->RegisterObserver(*observer_);
    ret = stateMachine_->Init(MAX_MSG_QUEUE_SIZE, MAX_MSG_QUEUE_PAYLOAD_SIZE);
    if (ret != HI_SUCCESS) {
        MEDIA_ERR_LOG("init PlayerControlStateMachine failed\n");
        goto DEL_OBSERVER;
    }
    ret = stateMachine_->Start();
    if (ret != HI_SUCCESS) {
        MEDIA_ERR_LOG("start PlayerControlStateMachine failed\n");
        goto DEINIT_STATE;
    }
    pthread_mutex_init(&schMutex_, nullptr);
    pthread_cond_init(&schCond_, nullptr);

    isInited_ = true;
    return HI_SUCCESS;
DEINIT_STATE:
        (void)stateMachine_->Deinit();
DEL_OBSERVER:
        delete observer_;
        observer_ = nullptr;
DEL_STATE:
        delete stateMachine_;
        stateMachine_ = nullptr;
    return HI_FAILURE;
}

int32_t PlayerControl::Deinit()
{
    if (isInited_ == false) {
        return HI_SUCCESS;
    }
    DestroyDecoder();
    if (stateMachine_ != nullptr) {
        MsgInfo msgInfo;
        (void)CheckIsFailed(memset_s(&msgInfo, sizeof(msgInfo), 0, sizeof(MsgInfo)), EOK, "memset_s failed");
        msgInfo.what = PLAYERCONTROL_MSG_HANDLEDATA;
        (void)CheckIsFailed(stateMachine_->RemoveEvent(msgInfo), HI_SUCCESS, "RemoveEvent failed");
        (void)CheckIsFailed(stateMachine_->Stop(), HI_SUCCESS, "Stop failed");
        (void)CheckIsFailed(stateMachine_->Deinit(), HI_SUCCESS, "Deinit failed");
        if (observer_ != nullptr) {
            delete observer_;
            observer_ = nullptr;
        }
        delete stateMachine_;
        stateMachine_ = nullptr;
    }
    pthread_mutex_destroy(&schMutex_);
    pthread_cond_destroy(&schCond_);
    isInited_ = false;
    return HI_SUCCESS;
}

int32_t PlayerControl::RegCallback(PlayerCtrlCallbackParam &eventObserver)
{
    if (CheckIsNull(stateMachine_, "stateMachine_ nullptr")) {
        return HI_ERR_PLAYERCONTROL_NULL_PTR;
    }
    MsgInfo msg;
    msg.what = PLAYERCONTROL_MSG_REGCALLBACK;
    msg.msgData = reinterpret_cast<void *>(&eventObserver);
    msg.msgDataLen = 0;
    return stateMachine_->Send(msg);
}

int32_t PlayerControl::SetDataSource(const std::string filePath)
{
    if (CheckIsNull(stateMachine_, "stateMachine_ nullptr")) {
        return HI_ERR_PLAYERCONTROL_NULL_PTR;
    }
    MsgInfo msg;
    msg.what = PLAYERCONTROL_MSG_SET_DATASOURCE_URI;
    msg.msgData = const_cast<void *>(reinterpret_cast<const void *>(filePath.c_str()));
    msg.msgDataLen = 0;
    return stateMachine_->Send(msg);
}

int32_t PlayerControl::SetDataSource(const int fd)
{
    if (CheckIsNull(stateMachine_, "stateMachine_ nullptr")) {
        return HI_ERR_PLAYERCONTROL_NULL_PTR;
    }
    int mediaFd = fd;
    MsgInfo msg;
    msg.what = PLAYERCONTROL_MSG_SET_DATASOURCE_FD;
    msg.msgData = reinterpret_cast<void *>(&mediaFd);
    msg.msgDataLen = 0;
    return stateMachine_->Send(msg);
}

int32_t PlayerControl::SetDataSource(BufferStream &stream)
{
    if (CheckIsNull(stateMachine_, "stateMachine_ nullptr")) {
        return HI_ERR_PLAYERCONTROL_NULL_PTR;
    }
    MsgInfo msg;
    msg.what = PLAYERCONTROL_MSG_SET_DATASOURCE_STREAM;
    msg.msgData = reinterpret_cast<void *>(&stream);
    msg.msgDataLen = 0;
    return stateMachine_->Send(msg);
}

int32_t PlayerControl::Prepare()
{
    if (CheckIsNull(stateMachine_, "stateMachine_ nullptr")) {
        return HI_ERR_PLAYERCONTROL_NULL_PTR;
    }
    return stateMachine_->Send(PLAYERCONTROL_MSG_PREPARE);
}

int32_t PlayerControl::Play()
{
    if (CheckIsNull(stateMachine_, "stateMachine_ nullptr")) {
        return HI_ERR_PLAYERCONTROL_NULL_PTR;
    }
    isPaused_ = false;
    int32_t ret = stateMachine_->Send(PLAYERCONTROL_MSG_PLAY);
    (void)CheckIsFailed(ret, HI_SUCCESS, "play failed");
    return ret;
}

bool PlayerControl::IsPaused(void)
{
    return isPaused_;
}

int32_t PlayerControl::SetVolume(VolumeAttr &volumeAttr)
{
    if (CheckIsNull(stateMachine_, "stateMachine_ nullptr")) {
        return HI_ERR_PLAYERCONTROL_NULL_PTR;
    }
    VolumeAttr attr = volumeAttr;
    MsgInfo msg;
    msg.what = PLAYERCONTROL_MSG_SET_VOLUME;
    msg.msgData = reinterpret_cast<void *>(&attr);
    msg.msgDataLen = 0;
    return stateMachine_->Send(msg);
}

int32_t PlayerControl::Stop()
{
    if (CheckIsNull(stateMachine_, "stateMachine_ nullptr")) {
        return HI_ERR_PLAYERCONTROL_NULL_PTR;
    }
    return stateMachine_->Send(PLAYERCONTROL_MSG_STOP);
}

int32_t PlayerControl::Pause()
{
    if (CheckIsNull(stateMachine_, "stateMachine_ nullptr")) {
        return HI_ERR_PLAYERCONTROL_NULL_PTR;
    }
    return stateMachine_->Send(PLAYERCONTROL_MSG_PAUSE);
}

int32_t PlayerControl::Seek(int64_t timeInMs)
{
    if (CheckIsNull(stateMachine_, "stateMachine_ nullptr")) {
        return HI_ERR_PLAYERCONTROL_NULL_PTR;
    }
    if (fmtFileInfo_.enVideoType == CODEC_JPEG) {
        MEDIA_ERR_LOG("seek action not support for play picture");
        return HI_ERR_PLAYERCONTROL_NOT_SUPPORT;
    }
    if ((fmtFileInfo_.s64Duration > 0) && (fmtFileInfo_.s64Duration < timeInMs)) {
        return HI_ERR_PLAYERCONTROL_ILLEGAL_PARAM;
    }
    if (!stateMachine_->EventValidAtCurState(PLAYERCONTROL_MSG_SEEK)) {
        MEDIA_ERR_LOG("invalid event seek at current state");
        return HI_ERR_PLAYERCONTROL_ILLEGAL_STATE_ACTION;
    }

    if (seekTabel_[0] == -1) {
        seekTabel_[0] = timeInMs;
    } else if (seekTabel_[1] == -1) {
        seekTabel_[1] = timeInMs;
    }
    MEDIA_INFO_LOG("seek out %lld", timeInMs);
    return HI_SUCCESS;
}

int32_t PlayerControl::GetFileInfo(FormatFileInfo &formatInfo)
{
    if (CheckIsNull(stateMachine_, "stateMachine_ nullptr")) {
        return HI_ERR_PLAYERCONTROL_NULL_PTR;
    }
    MsgInfo msg;
    msg.what = PLAYERCONTROL_MSG_GETFILEINFO;
    msg.msgData = &formatInfo;
    msg.msgDataLen = sizeof(FormatFileInfo);
    return stateMachine_->Send(msg);
}

int32_t PlayerControl::SetMedia(PlayerControlStreamAttr &mediaAttr)
{
    if (CheckIsNull(stateMachine_, "stateMachine_ nullptr")) {
        return HI_ERR_PLAYERCONTROL_NULL_PTR;
    }
    MsgInfo msg;
    msg.what = PLAYERCONTROL_MSG_SETATTR;
    msg.msgData = &mediaAttr;
    msg.msgDataLen = sizeof(PlayerControlStreamAttr);
    return stateMachine_->Send(msg);
}

int32_t PlayerControl::SetSurface(Surface *surface)
{
    surface_ = surface;
    return 0;
}

int32_t PlayerControl::TPlay(TplayAttr tplayAttr)
{
    if (CheckIsNull(stateMachine_, "stateMachine_ nullptr")) {
        return HI_ERR_PLAYERCONTROL_NULL_PTR;
    }
    MsgInfo msg;
    msg.what = PLAYERCONTROL_MSG_TPLAY;
    msg.msgData = &tplayAttr;
    msg.msgDataLen = sizeof(TplayAttr);
    return stateMachine_->Send(msg);
}

void PlayerControl::ClearCachePacket()
{
    if (cachedPacket_) {
        if (formatPacket_.data != nullptr) {
            if (playerSource_!= nullptr && playerSource_->FreeFrame(formatPacket_) != 0) {
                MEDIA_ERR_LOG("FreeFrame failed");
            }
            formatPacket_.data = nullptr;
            formatPacket_.len = 0;
            formatPacket_.trackId = -1;
        }
        /* end packet is null do not need to demux free */
        cachedPacket_ = false;
    }
}

void PlayerControl::OnVideoEndOfStream()
{
    if (CheckIsNull(stateMachine_, "stateMachine_ nullptr")) {
        return;
    }
    if ((fmtFileInfo_.s32UsedAudioStreamIndex == HI_DEMUXER_NO_MEDIA_STREAM ||
        stateMachine_->GetCurState() == PLAY_STATUS_TPLAY) &&
        lastSendVdecPts_ > AV_INVALID_PTS) {
        EventCallback(PLAYERCONTROL_EVENT_PROGRESS, &lastSendVdecPts_);
    }
    isVidPlayEos_ = true;
    isPlayEnd_ = IsPlayEos() ? true : isPlayEnd_;
}

void PlayerControl::OnAudioEndOfStream()
{
    if (CheckIsNull(stateMachine_, "stateMachine_ nullptr")) {
        return;
    }
    if (stateMachine_->GetCurState() != PLAY_STATUS_TPLAY) {
        isAudPlayEos_ = true;
        isPlayEnd_ = IsPlayEos() ? true : isPlayEnd_;
        if (lastSendAdecPts_ > AV_INVALID_PTS) {
            EventCallback(PLAYERCONTROL_EVENT_PROGRESS, &lastSendAdecPts_);
        }
    }
}

void PlayerControl::EventProcess(EventCbType event)
{
    PlayerControlError playerError;
    MEDIA_DEBUG_LOG("handleEvent %d", event);

    switch (event) {
        case EVNET_VIDEO_PLAY_EOS:
            OnVideoEndOfStream();
            break;
        case EVNET_VIDEO_PLAY_SOS:
            MEDIA_INFO_LOG("video sos recv");
            break;
        case EVNET_AUDIO_PLAY_EOS:
            OnAudioEndOfStream();
            break;
        case EVNET_VIDEO_RUNNING_ERR:
        case EVNET_AUDIO_RUNNING_ERR:
            playerError = PLAYERCONTROL_ERROR_VID_PLAY_FAIL;
            isPlayErr_ = true;
            EventCallback(PLAYERCONTROL_EVENT_ERROR, &playerError);
            break;
        case EVNET_FRAME_CONTINUE_LOST:
            isVidContinueLost_ = true;
            MEDIA_INFO_LOG("receive frame continue lost");
            break;
        case EVNET_ON_JPEG_FRAME_RENDED:
            break;
        case EVNET_FIRST_VIDEO_REND:
            if (pauseMode_) {
                MEDIA_INFO_LOG("first video rended");
                isNeedPause_ = true;
                EventCallback(PLAYERCONTROL_FIRST_VIDEO_FRAME, NULL);
            }
            break;
        case EVNET_FIRST_AUDIO_REND:
            if (pauseMode_ && fmtFileInfo_.s32UsedVideoStreamIndex == -1) {
                MEDIA_INFO_LOG("first audio rended");
                isNeedPause_ = true;
                EventCallback(PLAYERCONTROL_FIRST_AUDIO_FRAME, NULL);
            }
            break;
        default:
            break;
    }
}

void PlayerControl::EventQueueProcess(void)
{
    size_t queSize;
    PalayControlEventItem *item = nullptr;

    queSize = eventQueue.size();
    if (queSize > MAX_EVENT_MESSAGE_NUM) {
        MEDIA_WARNING_LOG("mesaage except, num:%u", queSize);
    }
    for (size_t i = 0; i < queSize && i < MAX_EVENT_MESSAGE_NUM; i++) {
        item = &eventQueue[i];
        EventProcess(item->event);
    }
    eventQueue.clear();
}

int32_t PlayerControl::OnPlayControlEvent(void *priv, const EventCbType event)
{
    PalayControlEventItem item;
    PlayerControl *player = reinterpret_cast<PlayerControl *>(priv);

    item.event = event;
    player->eventQueue.push_back(item);
    return HI_SUCCESS;
}

int32_t PlayerControl::PauseResume(void)
{
    if (CheckIsNull(sinkManager_, "sinkManager_ nullptr")) {
        return HI_ERR_PLAYERCONTROL_NULL_PTR;
    }
    int32_t ret = sinkManager_->Resume();
    if (ret != HI_SUCCESS) {
        MEDIA_ERR_LOG("resume failed");
    }
    pthread_mutex_lock(&schMutex_);
    paused_ = false;
    pthread_cond_signal(&schCond_);
    pthread_mutex_unlock(&schMutex_);
    return HI_SUCCESS;
}

int32_t PlayerControl::TPlayResume(void)
{
    if (CheckIsNull(sinkManager_, "sinkManager_ nullptr")) {
        return HI_ERR_PLAYERCONTROL_NULL_PTR;
    }
    int32_t ret = sinkManager_->SetSpeed(1.0, tplayAttr_.direction);
    if (CheckIsFailed(ret, HI_SUCCESS, "SetSpeed failed")) {
        return ret;
    }
    return HI_SUCCESS;
}

int32_t PlayerControl::OnSwitchTPlay2Play()
{
    int32_t ret;
    if (CheckIsNull(playerSource_, "playerSource_ nullptr")) {
        return HI_FAILURE;
    }

    pthread_mutex_lock(&schMutex_);
    if (tplayMode_ != PLAYER_TPLAY_FULL_PLAY) {
        ret = TPlayResetBuffer();
        if (ret != HI_SUCCESS) {
            MEDIA_ERR_LOG("TPlayResetBuffer failed , ret:%d", ret);
            pthread_mutex_unlock(&schMutex_);
            return ret;
        }
        ret = playerSource_->Seek(fmtFileInfo_.s32UsedVideoStreamIndex, lastReadPktPts_, FORMAT_SEEK_MODE_FORWARD_KEY);
        if (ret != HI_SUCCESS) {
            MEDIA_ERR_LOG("playerSource_ seek failed , ret:%d", ret);
            pthread_mutex_unlock(&schMutex_);
            return ret;
        }
        strmReadEnd_ = false;
        isVidPlayEos_ = false;
        isAudPlayEos_ = false;
        hasRenderVideoEos_ = false;
        hasRenderAudioEos_ = false;
    }
    firstAudioFrameAfterSeek_ = true;
    firstVideoFrameAfterSeek_ = true;

    ret = TPlayResume();
    pthread_mutex_unlock(&schMutex_);
    if (CheckIsFailed(ret, HI_SUCCESS, "TPlayResume failed")) {
        return ret;
    }
    return HI_SUCCESS;
}

int32_t PlayerControl::CheckMediaInfo(void)
{
    if (fmtFileInfo_.s32UsedAudioStreamIndex >= HI_DEMUXER_RESOLUTION_CNT
        || fmtFileInfo_.s32UsedVideoStreamIndex >= HI_DEMUXER_RESOLUTION_CNT) {
        MEDIA_ERR_LOG("input file error video track: %d audio %d",
                      fmtFileInfo_.s32UsedVideoStreamIndex, fmtFileInfo_.s32UsedAudioStreamIndex);
        return HI_ERR_PLAYERCONTROL_DEMUX_ERROR;
    }
    if (CheckMediaType(fmtFileInfo_) != HI_SUCCESS) {
        return HI_ERR_PLAYERCONTROL_NOT_SUPPORT;
    }
    return HI_SUCCESS;
}

int32_t PlayerControl::SetVDecAttr(void)
{
    StreamResolution resolution = { 0 };
    if (GetVideoResolution(fmtFileInfo_.s32UsedVideoStreamIndex, resolution) != HI_SUCCESS) {
        MEDIA_ERR_LOG("GetVideoResolution failed");
        return HI_FAILURE;
    }
    vdecAttr_.maxWidth = resolution.u32Width;
    vdecAttr_.maxHeight = resolution.u32Height;
    vdecAttr_.type = resolution.enVideoType;
    vdecAttr_.frameBufCnt = playerParam_.u32VdecFrameBufCnt;
    return HI_SUCCESS;
}

int32_t PlayerControl::SetADecAttr(void)
{
    adecAttr_.bitWidth = AUDIO_BITWIDTH;
    adecAttr_.channel = fmtFileInfo_.u32AudioChannelCnt;
    adecAttr_.sampleRate = fmtFileInfo_.u32SampleRate;
    adecAttr_.type = fmtFileInfo_.enAudioType;
    return HI_SUCCESS;
}

int32_t PlayerControl::SetStreamAttr()
{
    if (pauseMode_) {
        streamAttr_.playMode = RENDER_MODE_PAUSE_AFTER_PLAY;
    }
    if (fmtFileInfo_.s32UsedVideoStreamIndex != HI_DEMUXER_NO_MEDIA_STREAM) {
        StreamResolution resolution = { 0 };
        if (GetVideoResolution(fmtFileInfo_.s32UsedVideoStreamIndex, resolution) != HI_SUCCESS) {
            MEDIA_ERR_LOG("GetVideoResolution failed");
            return HI_ERR_PLAYERCONTROL_ILLEGAL_PARAM;
        }
        streamAttr_.videoBufSize = playerParam_.u32VideoEsBufSize;
    }
    if (fmtFileInfo_.s32UsedAudioStreamIndex != HI_DEMUXER_NO_MEDIA_STREAM) {
        streamAttr_.audioBufSize = (playerParam_.u32AudioEsBufSize) ? (playerParam_.u32AudioEsBufSize) :
            streamAttr_.audioBufSize;
    }

    if (sinkManager_ != nullptr) {
        sinkManager_->SetRenderMode(streamAttr_.playMode);
    }
    return HI_SUCCESS;
}

int32_t PlayerControl::SetDecoderAndStreamAttr(void)
{
    int ret;

    if (fmtFileInfo_.s32UsedAudioStreamIndex != HI_DEMUXER_NO_MEDIA_STREAM) {
        ret = SetADecAttr();
        if (CheckIsFailed(ret, HI_SUCCESS, "SetADecAttr failed")) {
            return ret;
        }
    }
    if (fmtFileInfo_.s32UsedVideoStreamIndex != HI_DEMUXER_NO_MEDIA_STREAM) {
        ret = SetVDecAttr();
        if (CheckIsFailed(ret, HI_SUCCESS, "SetVDecAttr failed")) {
            return ret;
        }
    }
    ret = SetStreamAttr();
    if (CheckIsFailed(ret, HI_SUCCESS, "SetStreamAttr failed")) {
        return ret;
    }
    return HI_SUCCESS;
}

int32_t PlayerControl::AudioDecoderStart(void)
{
    AvCodecMime mime = TransformCodecFormatToAvCodecMime(fmtFileInfo_.enAudioType);
    if (mime == MEDIA_MIMETYPE_INVALID) {
        MEDIA_ERR_LOG("DecoderStart not support codec:%d", fmtFileInfo_.enAudioType);
        return -1;
    }
    audioDecoder_ = std::make_shared<Decoder>();
    if (CheckIsNull(audioDecoder_, "new decoder failed")) {
        return -1;
    }
    AvAttribute attr;
    attr.type = AUDIO_DECODER;
    attr.adecAttr.mime = mime;
    attr.adecAttr.priv = nullptr;
    attr.adecAttr.bufSize = 0x400; /* 1024 */
    attr.adecAttr.channelCnt = (mime == MEDIA_MIMETYPE_AUDIO_PCM) ? fmtFileInfo_.u32AudioChannelCnt : 0;
    const std::string audioName = GetAudioNameByAvCodecMime(mime);
    int32_t ret = audioDecoder_->CreateHandle(audioName, attr);
    if (CheckIsFailed(ret, 0, "create audio decoder failed")) {
        return -1;
    }
    ret = audioDecoder_->StartDec();
    if (CheckIsFailed(ret, 0, "start audio decoder failed")) {
        return -1;
    }
    MEDIA_INFO_LOG("audio decoder started");

    return 0;
}

int32_t PlayerControl::VideoDecoderStart(void)
{
    AvAttribute attr;
    uint32_t width = DECODER_DEFAULT_WIDTH;
    uint32_t height = DECODER_DEFAULT_HEIGHT;

    AvCodecMime mime = TransformCodecFormatToAvCodecMime(fmtFileInfo_.enVideoType);
    if (mime == MEDIA_MIMETYPE_INVALID) {
        MEDIA_ERR_LOG("DecoderStart not support codec:%d", fmtFileInfo_.enVideoType);
        return -1;
    }
    GetCurVideoSolution(fmtFileInfo_, width, height);

    videoDecoder_ = std::make_shared<Decoder>();
    if (CheckIsNull(videoDecoder_, "new decoder failed")) {
        return -1;
    }
    attr.type = VIDEO_DECODER;
    attr.vdecAttr.mime = mime;
    attr.vdecAttr.priv = nullptr;
    attr.vdecAttr.bufSize = 0;
    attr.vdecAttr.maxWidth = width;
    attr.vdecAttr.maxHeight = height;
    const std::string videoName = "codec.avc.soft.decoder";
    int32_t ret = videoDecoder_->CreateHandle(videoName, attr);
    if (CheckIsFailed(ret, 0, "create video decoder failed")) {
        return -1;
    }
    ret = videoDecoder_->StartDec();
    if (CheckIsFailed(ret, 0, "start video decoder failed")) {
        return -1;
    }
    MEDIA_INFO_LOG("video decoder started");

    return 0;
}

int32_t PlayerControl::DecoderStart(void)
{
    if (fmtFileInfo_.s32UsedAudioStreamIndex == -1 && fmtFileInfo_.s32UsedVideoStreamIndex == -1) {
        MEDIA_ERR_LOG("not decoder need start");
        return -1;
    }
    MEDIA_INFO_LOG("PlayerControl::DecoderStart streamid:%d-%d",
        fmtFileInfo_.s32UsedAudioStreamIndex, fmtFileInfo_.s32UsedVideoStreamIndex);
    if (fmtFileInfo_.s32UsedAudioStreamIndex != -1) {
        if (AudioDecoderStart() != 0) {
            return -1;
        }
    }
    if (fmtFileInfo_.s32UsedVideoStreamIndex != -1) {
        if (VideoDecoderStart() != 0) {
            return -1;
        }
    }
    return 0;
}

void PlayerControl::DestroyDecoder()
{
    if (audioDecoder_ != nullptr) {
        audioDecoder_->DestroyHandle();
    }
    if (videoDecoder_ != nullptr) {
        videoDecoder_->DestroyHandle();
    }
}

void PlayerControl::StopSinkAndDecoder()
{
#ifdef MEDIA_INTERFACE_V1_0
    OutputInfo outInfo;
#else
    PlayerBufferInfo outInfo;
#endif

    if (sinkManager_ != nullptr) {
        sinkManager_->Stop();
    }
    if (audioDecoder_ != nullptr && sinkManager_ != nullptr) {
        while (sinkManager_->DequeReleaseFrame(true, outInfo) == 0) {
#ifdef MEDIA_INTERFACE_V1_0
            audioDecoder_->QueueOutputBuffer(outInfo, GET_BUFFER_TIMEOUT_MS);
#else
            audioDecoder_->QueueOutputBuffer(&outInfo.info, GET_BUFFER_TIMEOUT_MS);
#endif
        }
        audioDecoder_->StopDec();
    }
    if (videoDecoder_ != nullptr && sinkManager_ != nullptr) {
        while (sinkManager_->DequeReleaseFrame(false, outInfo) == 0) {
#ifdef MEDIA_INTERFACE_V1_0
            videoDecoder_->QueueOutputBuffer(outInfo, GET_BUFFER_TIMEOUT_MS);
#else
            videoDecoder_->QueueOutputBuffer(&outInfo.info, GET_BUFFER_TIMEOUT_MS);
#endif
        }
        videoDecoder_->StopDec();
    }
    isVideoStarted_ = false;
    isAudioStarted_ = false;
}


int32_t PlayerControl::AddAudioSink(void)
{
    if (CheckIsFailed(isAudioStarted_, false, "already started")) {
        return HI_SUCCESS;
    }
    SinkAttr attr;
    attr.sinkType = SINK_TYPE_AUDIO;
    attr.trackId = fmtFileInfo_.s32UsedAudioStreamIndex;
    attr.audAttr.format = fmtFileInfo_.enAudioType;
    attr.audAttr.sampleFmt = DATA_TYPE_S16;
    attr.audAttr.sampleRate = adecAttr_.sampleRate;
    attr.audAttr.channel = fmtFileInfo_.u32AudioChannelCnt;
    attr.audAttr.volume = 0x6;

    if (sinkManager_->AddNewSink(attr) != 0) {
        MEDIA_ERR_LOG("AddNewSink  failed");
        return -1;
    }
    isAudioStarted_ = true;
    return HI_SUCCESS;
}

int32_t PlayerControl::AddVideoSink()
{
    if (CheckIsFailed(isVideoStarted_, false, "already started")) {
        return HI_SUCCESS;
    }
    SinkAttr attr;
    attr.sinkType = SINK_TYPE_VIDEO;
    attr.trackId = fmtFileInfo_.s32UsedVideoStreamIndex;
    attr.vidAttr.width = fmtFileInfo_.u32Width;
    attr.vidAttr.height = fmtFileInfo_.u32Height;
    attr.vidAttr.frameRate = fmtFileInfo_.fFrameRate;
    attr.vidAttr.format = fmtFileInfo_.enVideoType;
    attr.vidAttr.surface = surface_;
    if (sinkManager_->AddNewSink(attr) != 0) {
        MEDIA_ERR_LOG("AddNewSink  failed");
        return -1;
    }
    isVideoStarted_ = true;
    return HI_SUCCESS;
}


int32_t PlayerControl::SinkStart(void)
{
    int32_t ret = HI_SUCCESS;
    sinkManager_ = std::make_shared<SinkManager>();
    if (CheckIsNull(sinkManager_, "new sinkManager_ nullptr")) {
        return HI_FAILURE;
    }
    if (fmtFileInfo_.s32UsedVideoStreamIndex != HI_DEMUXER_NO_MEDIA_STREAM && !isVideoStarted_) {
        ret = AddVideoSink();
        if (CheckIsFailed(ret, HI_SUCCESS, "AddVideoSink failed")) {
            return ret;
        }
        isVideoStarted_ = true;
    }
    if (fmtFileInfo_.s32UsedAudioStreamIndex != HI_DEMUXER_NO_MEDIA_STREAM && !isAudioStarted_) {
        ret = AddAudioSink();
        if (CheckIsFailed(ret, HI_SUCCESS, "AddAudioSink failed")) {
            return ret;
        }
        isAudioStarted_ = true;
        sinkManager_->SetAudioStreamType(audioStreamType_);
    }

    PlayEventCallback callback;
    GetPlayElementEventCallBack(callback);
    ret = sinkManager_->RegisterCallBack(callback);
    if (CheckIsFailed(ret, HI_SUCCESS, "RegisterCallBack failed")) {
        return ret;
    }
    sinkManager_->SetRenderMode((pauseMode_ == true) ? RENDER_MODE_PAUSE_AFTER_PLAY : RENDER_MODE_NORMAL);
    ret = sinkManager_->Start();
    if (CheckIsFailed(ret, HI_SUCCESS, "sinkManager_ Start failed")) {
        return ret;
    }

    if (leftVolume_ >= 0.0f || rightVolume_ >= 0.0f) {
        sinkManager_->SetVolume(leftVolume_, rightVolume_);
    }
    return HI_SUCCESS;
}

int32_t PlayerControl::ReadFrameFromSource(FormatFrame &fmtFrame)
{
    if (CheckIsNull(playerSource_, "playerSource_ nullptr")) {
        return HI_ERR_PLAYERCONTROL_NULL_PTR;
    }
    int ret = playerSource_->ReadFrame(fmtFrame);
    // Check consistency of fmtFrame
    if ((ret == HI_SUCCESS) && (fmtFrame.data != nullptr) && (fmtFrame.len == 0)) {
        MEDIA_ERR_LOG("ReadFrame ERROR : u32Size is invalid");
        ret = HI_FAILURE;
    }
    return ret;
}

int32_t PlayerControl::GetVideoResolution(int32_t streamIdx, StreamResolution &resolution)
{
    if (streamIdx == HI_DEMUXER_NO_MEDIA_STREAM) {
        MEDIA_ERR_LOG("stream id [%d] in invalid", streamIdx);
        return HI_FAILURE;
    }

    for (uint32_t i = 0; i < HI_DEMUXER_RESOLUTION_CNT; i++) {
        if (fmtFileInfo_.stStreamResolution[i].s32VideoStreamIndex == streamIdx) {
            resolution.enVideoType = fmtFileInfo_.stStreamResolution[i].enVideoType;
            resolution.s32VideoStreamIndex = fmtFileInfo_.stStreamResolution[i].s32VideoStreamIndex;
            resolution.u32Width = fmtFileInfo_.stStreamResolution[i].u32Width;
            resolution.u32Height = fmtFileInfo_.stStreamResolution[i].u32Height;
            return HI_SUCCESS;
        }
    }

    MEDIA_ERR_LOG("not found stream[%d] in file", streamIdx);
    return HI_FAILURE;
}

// message begin
int32_t PlayerControl::DoRegCallback(PlayerCtrlCallbackParam &eventCallback)
{
    if (eventCallback_.callbackFun != nullptr) {
        MEDIA_WARNING_LOG("you have reg callback,now the before callback fun pointer will be overwrite");
    }
    eventCallback_ = eventCallback;
    return HI_SUCCESS;
}

int32_t PlayerControl::DoSetDataSource(const std::string filepath)
{
    filePath_ = filepath;
    sourceType_ = SOURCE_TYPE_URI;
    return HI_SUCCESS;
}

int32_t PlayerControl::DoSetDataSource(const int32_t fd)
{
    fd_ = fd;
    sourceType_ = SOURCE_TYPE_FD;
    return HI_SUCCESS;
}

int32_t PlayerControl::DoSetDataSource(BufferStream &stream)
{
    stream_ = stream;
    sourceType_ = SOURCE_TYPE_STREAM;
    return HI_SUCCESS;
}

int32_t PlayerControl::DoPrepare(void)
{
    MEDIA_DEBUG_LOG("Process in");
    if (CheckIsNull(stateMachine_, "stateMachine_ nullptr")) {
        return HI_FAILURE;
    }
    PlayerStatus playerState = stateMachine_->GetCurState();
    if (CheckStateSame(playerState, PLAY_STATUS_PREPARED)) {
        return HI_SUCCESS;
    }
    if (CheckIsNull(eventCallback_.callbackFun, "callbackFun nullptr")) {
        return HI_ERR_PLAYERCONTROL_CALLBACK_ERROR;
    }
    isPlayEnd_ = false;
    isTplayStartRead_ = false;
    int32_t ret = SyncPrepare();
    if (CheckIsFailed(ret, HI_SUCCESS, "SyncPrepare failed")) {
        return HI_ERR_PLAYERCONTROL_OTHER;
    }
    if (playerParam_.u32PlayPosNotifyIntervalMs >= fmtFileInfo_.s64Duration) {
        MEDIA_WARNING_LOG(
            "play postion notify interval %d ms oversize file duration, user will never receive notify",
            playerParam_.u32PlayPosNotifyIntervalMs);
    }
    MEDIA_DEBUG_LOG("Process out");
    return HI_SUCCESS;
}

void PlayerControl::ReleaseADecoderOutputFrame(void)
{
    if (audioDecoder_ == nullptr || sinkManager_ == nullptr) {
        return;
    }
    while (true) {
#ifdef MEDIA_INTERFACE_V1_0
        OutputInfo outInfo;
        if (sinkManager_->DequeReleaseFrame(true, outInfo) != 0) {
            break;
        }
        audioDecoder_->QueueOutputBuffer(outInfo, GET_BUFFER_TIMEOUT_MS);
#else
        PlayerBufferInfo outInfo;
        if (sinkManager_->DequeReleaseFrame(true, outInfo) != 0) {
            break;
        }
        audioDecoder_->QueueOutputBuffer(&outInfo.info, GET_BUFFER_TIMEOUT_MS);
#endif
    }
}

void PlayerControl::RenderAudioFrame(void)
{
    if (audioDecoder_ == nullptr || sinkManager_ == nullptr || hasRenderAudioEos_) {
        return;
    }

#ifdef MEDIA_INTERFACE_V1_0
    OutputInfo outInfo;
    int ret = audioDecoder_->DequeueOutputBuffer(outInfo, GET_BUFFER_TIMEOUT_MS);
    if (ret != 0) {
        InitOutputBuffer(outInfo, AUDIO_DECODER);
        if (ret == CODEC_RECEIVE_EOS && strmReadEnd_) {
            sinkManager_->RenderEos(true);  /* all frame have been send to audio sink */
        }
    }
    ret = sinkManager_->RenderFrame(outInfo);
#else
    PlayerBufferInfo outInfo;
    int ret = audioDecoder_->DequeueOutputBuffer(&outInfo.info, GET_BUFFER_TIMEOUT_MS);
    if (ret != 0) {
        InitOutputBuffer(outInfo.info, AUDIO_DECODER);
        if (ret == CODEC_RECEIVE_EOS && strmReadEnd_) {
            sinkManager_->RenderEos(true);  /* all frame have been send to audio sink */
        }
    }
    ret = sinkManager_->RenderFrame(outInfo, AUDIO_DECODER);
#endif
    if (ret == SINK_RENDER_FULL || ret == SINK_RENDER_DELAY) {
        renderSleepTime_ = RENDER_FULL_SLEEP_TIME_US;
    } else if (ret == SINK_QUE_EMPTY) {
        renderSleepTime_ = 0;
    }
    ReleaseADecoderOutputFrame();
    /* fuction RenderFrame will return SINK_RENDER_EOS when all frame in queue that have been processed */
    if (ret == SINK_RENDER_EOS) {
        hasRenderAudioEos_ = true;
        MEDIA_INFO_LOG("have render audio eos");
        if (IsPlayEos() == true) {
            isPlayEnd_ = true;
        }
    } else if (ret == SINK_RENDER_PAUSED) {
        MEDIA_INFO_LOG("audio sink render paused, pause player");
        isPaused_ = true;
    }
}

void PlayerControl::ReleaseVDecoderOutputFrame(void)
{
    if (videoDecoder_ == nullptr || sinkManager_ == nullptr) {
        return;
    }
    while (true) {
#ifdef MEDIA_INTERFACE_V1_0
        OutputInfo outInfo;
        if (sinkManager_->DequeReleaseFrame(false, outInfo) != 0) {
            break;
        }
        videoDecoder_->QueueOutputBuffer(outInfo, GET_BUFFER_TIMEOUT_MS);
#else
        PlayerBufferInfo outInfo;
        if (sinkManager_->DequeReleaseFrame(false, outInfo) != 0) {
            break;
        }
        videoDecoder_->QueueOutputBuffer(&outInfo.info, GET_BUFFER_TIMEOUT_MS);
#endif
    }
}

void PlayerControl::RenderVideoFrame(void)
{
    if (videoDecoder_ == nullptr || sinkManager_ == nullptr || hasRenderVideoEos_) {
        return;
    }

#ifdef MEDIA_INTERFACE_V1_0
    OutputInfo outInfo;
    int ret = videoDecoder_->DequeueOutputBuffer(outInfo, GET_BUFFER_TIMEOUT_MS);
    if (ret != 0) {
        InitOutputBuffer(outInfo, VIDEO_DECODER);
        if (ret == CODEC_RECEIVE_EOS) {
            sinkManager_->RenderEos(false); /* all frame have been send to video sink */
        }
    }
    ret = sinkManager_->RenderFrame(outInfo);
#else
    PlayerBufferInfo outInfo;
    outInfo.info.bufferCnt = 1;
    int ret = videoDecoder_->DequeueOutputBuffer(&outInfo.info, GET_BUFFER_TIMEOUT_MS);
    if (ret != 0) {
        InitOutputBuffer(outInfo.info, VIDEO_DECODER);
        if (ret == CODEC_RECEIVE_EOS) {
            sinkManager_->RenderEos(false); /* all frame have been send to video sink */
        }
    }
    ret = sinkManager_->RenderFrame(outInfo, VIDEO_DECODER);
#endif
    if (ret == SINK_RENDER_FULL || ret == SINK_RENDER_DELAY) {
        renderSleepTime_ = RENDER_FULL_SLEEP_TIME_US;
    } else if (ret == SINK_QUE_EMPTY) {
        renderSleepTime_ = 0;
    }
    ReleaseVDecoderOutputFrame();
    /* fuction RenderFrame will return SINK_RENDER_EOS when all frame in queue that have been processed */
    if (ret == SINK_RENDER_EOS) {
        hasRenderVideoEos_ = true;
        MEDIA_INFO_LOG("have rendered video eos");
        if (IsPlayEos()) {
            isPlayEnd_ = true;
        }
    }
}

void PlayerControl::ReortRenderPosition(void)
{
    int64_t position = -1;
    if (CheckIsNull(sinkManager_, "sinkManager_ nullptr")) {
        return;
    }
    sinkManager_->GetRenderPosition(position);
    if (position >= 0 && currentPosition_ != position) {
        currentPosition_ = position;
        EventCallback(PLAYERCONTROL_EVENT_PROGRESS, &currentPosition_);
    }
}

void *PlayerControl::DataSchProcess(void *priv)
{
    PlayerControl *play = (PlayerControl*)priv;
    if (CheckIsNull(play, "play nullptr")) {
        return nullptr;
    }

    prctl(PR_SET_NAME, "PlaySch", 0, 0, 0);
    MEDIA_INFO_LOG("start work");
    while (true) {
        pthread_mutex_lock(&play->schMutex_);
        if (play->schThreadExit_ == true) {
            pthread_mutex_unlock(&play->schMutex_);
            break;
        }
        play->DoSeekIfNeed();
        if (play->paused_) {
            CondTimeWait(play->schCond_, play->schMutex_, PAUSE_WAIT_TIME_US);
            pthread_mutex_unlock(&play->schMutex_);
            continue;
        }
        play->renderSleepTime_ = 0;
        play->ReadPacketAndPushToDecoder();

        /* get and render frame */
        play->RenderAudioFrame();
        play->RenderVideoFrame();
        pthread_mutex_unlock(&play->schMutex_);
        play->EventQueueProcess();
        play->ReortRenderPosition();
        if (play->isPaused_) {
            play->Pause();
        }
        if (play->isPlayEnd_) {
            play->DealPlayEnd();
            play->isPlayEnd_ = false;
        }
        if (play->IsPlayEos()) {
            play->renderSleepTime_ = RENDER_EOS_SLEEP_TIME_US;
        }
        if (play->renderSleepTime_ > 0) {
            pthread_mutex_lock(&play->schMutex_);
            CondTimeWait(play->schCond_, play->schMutex_, play->renderSleepTime_);
            pthread_mutex_unlock(&play->schMutex_);
        }
    }
    pthread_mutex_lock(&play->schMutex_);
    play->schThreadExit_ = true;
    pthread_mutex_unlock(&play->schMutex_);
    MEDIA_INFO_LOG("end work");
    return nullptr;
}

int32_t PlayerControl::DoPlayFromPrepared(void)
{
    int32_t ret = CheckMediaInfo();
    if (CheckIsFailed(ret, HI_SUCCESS, "CheckMediaInfo failed")) {
        return ret;
    }
    ret = SetDecoderAndStreamAttr();
    if (CheckIsFailed(ret, HI_SUCCESS, "SetDecoderAndStreamAttr failed")) {
        return ret;
    }
    ret = DecoderStart();
    if (CheckIsFailed(ret, HI_SUCCESS, "DecoderStart failed")) {
        return ret;
    }
    ret = SinkStart();
    if (CheckIsFailed(ret, HI_SUCCESS, "SinkStart failed")) {
        return ret;
    }
    ret = playerSource_->Start();
    if (CheckIsFailed(ret, HI_SUCCESS, "playerSource_ Start failed")) {
        return ret;
    }
    pthread_mutex_lock(&schMutex_);
    paused_ = true;
    pthread_mutex_unlock(&schMutex_);
    ret = pthread_create(&schProcess_, nullptr, DataSchProcess, this);
    if (ret != 0) {
        MEDIA_ERR_LOG("pthread_create failed %d", ret);
        pthread_mutex_lock(&schMutex_);
        schThreadExit_ = true;
        paused_ = false;
        pthread_mutex_unlock(&schMutex_);
        schProcess_ = 0;
        StopSinkAndDecoder();
        ClearCachePacket();
        if (playerSource_ != nullptr) {
            (void)playerSource_->Stop();
        }
        return -1;
    }
    pthread_mutex_lock(&schMutex_);
    paused_ = false;
    pthread_cond_signal(&schCond_);
    pthread_mutex_unlock(&schMutex_);
    return HI_SUCCESS;
}

int32_t PlayerControl::DoPlay()
{
    MEDIA_DEBUG_LOG("Process in");
    if (CheckIsNull(stateMachine_, "stateMachine_ nullptr")) {
        return HI_ERR_PLAYERCONTROL_NULL_PTR;
    }
    if (CheckIsNull(playerSource_, "playerSource_ nullptr")) {
        return HI_ERR_PLAYERCONTROL_NULL_PTR;
    }

    int32_t ret = HI_SUCCESS;
    PlayerStatus playerState = stateMachine_->GetCurState();
    if (CheckStateSame(playerState, PLAY_STATUS_PLAY)) {
        return HI_SUCCESS;
    }
    if (playerState == PLAY_STATUS_TPLAY) {
        ret = OnSwitchTPlay2Play();
        if (CheckIsFailed(ret, HI_SUCCESS, "OnSwitchTPlay2Play failed")) {
            return ret;
        }
    } else if (playerState == PLAY_STATUS_PREPARED) {
        ret = DoPlayFromPrepared();
        if (CheckIsFailed(ret, HI_SUCCESS, "DoPlayFromPrepared failed")) {
            return ret;
        }
    } else if (playerState == PLAY_STATUS_PAUSE) {
        ret = PauseResume();
        if (CheckIsFailed(ret, HI_SUCCESS, "PauseResume failed")) {
            return ret;
        }
    } else {
        return HI_ERR_PLAYERCONTROL_ILLEGAL_STATE_ACTION;
    }

    MEDIA_DEBUG_LOG("Process out");
    return ret;
}

int32_t PlayerControl::DoSetVolume(VolumeAttr &volumeAttr)
{
    leftVolume_ = volumeAttr.leftVolume;
    rightVolume_ = volumeAttr.rightVolume;
    if (CheckIsNull(sinkManager_, "sinkManager_ nullptr, it will be setted after play")) {
        return HI_SUCCESS;
    }
    return sinkManager_->SetVolume(volumeAttr.leftVolume, volumeAttr.rightVolume);
}

int32_t PlayerControl::ReadPacket()
{
    int32_t ret = HI_SUCCESS;
    if (cachedPacket_) {
        return ret;
    }
    ret = ReadFrameFromSource(formatPacket_);
    if (ret != HI_RET_FILE_EOF && (formatPacket_.data == nullptr || formatPacket_.len == 0)) {
        return HI_RET_NODATA;
    }
    if (ret == HI_RET_FILE_EOF) {
        if (CheckIsFailed(memset_s(&formatPacket_, sizeof(formatPacket_), 0, sizeof(FormatFrame)),
            EOK, "memset_s failed")) {
            return -1;
        }
        ret = HI_SUCCESS;
    } else if (ret == HI_RET_NODATA) {
        if (CheckIsFailed(memset_s(&formatPacket_, sizeof(formatPacket_), 0, sizeof(FormatFrame)),
            EOK, "memset_s failed")) {
            return -1;
        }
        strmReadEnd_ = false;
        return ret;
    } else if (ret != HI_SUCCESS) {
        MEDIA_ERR_LOG("ReadFrameFromSource failed , ret:%d", ret);
        NotifyError(PLAYERCONTROL_ERROR_DEMUX_FAIL);
        return HI_FAILURE;
    }
    strmReadEnd_ = false;
    cachedPacket_ = true;

    return ret;
}

#ifdef MEDIA_INTERFACE_V1_0
void PlayerControl::PushPacketToADecoderInner(void)
{
    InputInfo inputData;
    CodecBufferInfo inBufInfo;
    inputData.bufferCnt = 0;
    inputData.buffers = nullptr;
    inputData.pts = 0;
    inputData.flag = 0;
    if (memset_s(&inBufInfo, sizeof(inBufInfo), 0, sizeof(CodecBufferInfo)) != EOK) {
        return;
    }
    int32_t ret = audioDecoder_->DequeInputBuffer(inputData, GET_BUFFER_TIMEOUT_MS);
    if (ret != 0) {
        return;
    }
    inBufInfo.addr = formatPacket_.data;
    inBufInfo.length = formatPacket_.len;
    inputData.bufferCnt = 1;
    inputData.buffers = &inBufInfo;
    inputData.pts = formatPacket_.timestampUs;
    inputData.flag = 0;
    ret = audioDecoder_->QueueInputBuffer(inputData, GET_BUFFER_TIMEOUT_MS);
    if (ret == CODEC_ERR_STREAM_BUF_FULL) {
        continuousAudFull_++;
        if (continuousAudFull_ == MAX_AUDIO_SEND_FRAME_FULL_NUM) {
            MEDIA_INFO_LOG("adec continue full reach to max num %d \n", MAX_AUDIO_SEND_FRAME_FULL_NUM);
            continuousAudFull_ = 0;
            ClearCachePacket();
        } else {
            renderSleepTime_ = QUEUE_BUFFER_FULL_SLEEP_TIME_US;
        }
        return;
    }
    continuousAudFull_ = 0;
    if (firstAudioFrameAfterSeek_ && IsValidPacket(formatPacket_)) {
        firstAudioFrameAfterSeek_ = false;
        MEDIA_INFO_LOG("push firstAudioFrameAfterSeek_ success, pts:%lld", inputData.pts);
    }
    if (formatPacket_.data != nullptr || formatPacket_.len != 0) {
        lastSendAdecPts_ = formatPacket_.timestampUs;
    }
    ClearCachePacket();
#ifdef ENABLE_DFX
    MEDIA_DFX_LOG("send frame data success, dataSize = %d", inBufInfo.length);
#endif
}
#else
void PlayerControl::PushPacketToADecoderInner(void)
{
    PlayerBufferInfo inputData = {};
    CodecBufferInfo inBufInfo;
    if (memset_s(&inBufInfo, sizeof(inBufInfo), 0, sizeof(CodecBufferInfo)) != EOK) {
        return;
    }

    inBufInfo.type = BUFFER_TYPE_VIRTUAL;
    inBufInfo.buf = reinterpret_cast<intptr_t>(formatPacket_.data);
    inBufInfo.length = formatPacket_.len;
    inputData.info.bufferCnt = 1;
    inputData.info.buffer[0] = inBufInfo;
    inputData.info.timeStamp = formatPacket_.timestampUs;
    inputData.info.flag = 0;
    int32_t ret = audioDecoder_->QueueInputBuffer(&inputData.info, GET_BUFFER_TIMEOUT_MS);
    if (ret == CODEC_ERR_UNKOWN) { // CODEC_ERR_STREAM_BUF_FULL
        renderSleepTime_ = QUEUE_BUFFER_FULL_SLEEP_TIME_US;
        return;
    }
    PlayerBufferInfo outData;
    ret = audioDecoder_->DequeInputBuffer(&outData.info, GET_BUFFER_TIMEOUT_MS);
    if (ret != 0) {
        MEDIA_DEBUG_LOG("audio DequeInputBuffer failed");
        return;
    }
    if (firstAudioFrameAfterSeek_ && IsValidPacket(formatPacket_)) {
        firstAudioFrameAfterSeek_ = false;
        MEDIA_INFO_LOG("push firstAudioFrameAfterSeek_ success, pts:%lld", inputData.info.timeStamp);
    }
    if (formatPacket_.data != nullptr || formatPacket_.len != 0) {
        lastSendAdecPts_ = formatPacket_.timestampUs;
    }
    ClearCachePacket();
#ifdef ENABLE_DFX
    MEDIA_DFX_LOG("send frame data success, dataSize = %d", inBufInfo.length);
#endif
}
#endif

void PlayerControl::PushPacketToADecoder(void)
{
    if (audioDecoder_ == nullptr) {
        return;
    }
    PushPacketToADecoderInner();
}

#ifdef MEDIA_INTERFACE_V1_0
void PlayerControl::PushPacketToVDecoderInner(void)
{
    InputInfo inputData;
    CodecBufferInfo inBufInfo;
    inputData.bufferCnt = 0;
    inputData.buffers = nullptr;
    inputData.pts = 0;
    inputData.flag = 0;
    if (memset_s(&inBufInfo, sizeof(inBufInfo), 0, sizeof(CodecBufferInfo)) != EOK) {
        return;
    }
    int32_t ret = videoDecoder_->DequeInputBuffer(inputData, GET_BUFFER_TIMEOUT_MS);
    if (ret != 0) {
        return;
    }
    inBufInfo.addr = formatPacket_.data;
    inBufInfo.length = formatPacket_.len;
    inputData.bufferCnt = 1;
    inputData.buffers = &inBufInfo;
    inputData.pts = formatPacket_.timestampUs;
    inputData.flag = 0;
    ret = videoDecoder_->QueueInputBuffer(inputData, GET_BUFFER_TIMEOUT_MS);
    if (ret == CODEC_ERR_STREAM_BUF_FULL) {
        renderSleepTime_ = QUEUE_BUFFER_FULL_SLEEP_TIME_US;
        return;
    }
    if (firstVideoFrameAfterSeek_ && IsValidPacket(formatPacket_)) {
        firstVideoFrameAfterSeek_ = false;
        MEDIA_INFO_LOG("push firstVideoFrameAfterSeek_ success, pts:%lld", inputData.pts);
        EventCallback(PLAYERCONTROL_FIRST_VIDEO_FRAME, reinterpret_cast<void *>(&inputData.pts));
    }
    if (formatPacket_.data != nullptr && formatPacket_.len != 0) {
        lastSendVdecPts_ = formatPacket_.timestampUs;
    }
    ClearCachePacket();
}
#else
void PlayerControl::PushPacketToVDecoderInner(void)
{
    PlayerBufferInfo inputData = {};
    CodecBufferInfo inBufInfo;
    if (memset_s(&inBufInfo, sizeof(inBufInfo), 0, sizeof(CodecBufferInfo)) != EOK) {
        return;
    }
    inBufInfo.type = BUFFER_TYPE_VIRTUAL;
    inBufInfo.buf = reinterpret_cast<intptr_t>(formatPacket_.data);
    inBufInfo.length = formatPacket_.len;
    inputData.info.bufferCnt = 1;
    inputData.info.buffer[0] = inBufInfo;
    inputData.info.timeStamp = formatPacket_.timestampUs;
    inputData.info.flag = 0;
    int32_t ret = videoDecoder_->QueueInputBuffer(&inputData.info, GET_BUFFER_TIMEOUT_MS);
    if (ret == CODEC_ERR_UNKOWN) { // CODEC_ERR_STREAM_BUF_FULL
        renderSleepTime_ = QUEUE_BUFFER_FULL_SLEEP_TIME_US;
        return;
    }
    PlayerBufferInfo outData;
    ret = videoDecoder_->DequeInputBuffer(&outData.info, GET_BUFFER_TIMEOUT_MS);
    if (ret != 0) {
        MEDIA_DEBUG_LOG("video DequeInputBuffer failed");
        return;
    }
    if (firstVideoFrameAfterSeek_ && IsValidPacket(formatPacket_)) {
        firstVideoFrameAfterSeek_ = false;
        MEDIA_INFO_LOG("push firstVideoFrameAfterSeek_ success, pts:%lld", inputData.info.timeStamp);
    }
    if (formatPacket_.data != nullptr && formatPacket_.len != 0) {
        lastSendVdecPts_ = formatPacket_.timestampUs;
    }
    ClearCachePacket();
}
#endif

void PlayerControl::PushPacketToVDecoder(void)
{
    if (videoDecoder_ == nullptr) {
        return;
    }
    PushPacketToVDecoderInner();
}

int32_t PlayerControl::ReadPacketAndPushToDecoder()
{
    int32_t ret = HI_SUCCESS;
    PlayerStatus playerState;
    if (stateMachine_ == nullptr) {
        return HI_FAILURE;
    }
    playerState = stateMachine_->GetCurState();
    if ((playerState != PLAY_STATUS_PLAY) && (playerState != PLAY_STATUS_TPLAY)) {
        return HI_SUCCESS;
    }
    if (isNeedPause_ == true) {
        isNeedPause_ = false;
        return HI_SUCCESS;
    }
    if (strmReadEnd_ == true) {
        return HI_SUCCESS;
    }
    if (playerState == PLAY_STATUS_PLAY) {
        ret = ReadPacket();
    } else {
        ret = ReadTplayData();
    }
    if (ret == HI_RET_NODATA) {
        renderSleepTime_ = NO_DATA_READ_SLEEP_TIME_US;
        ClearCachePacket();
        return ret;
    } else if (ret != HI_SUCCESS) {
        ClearCachePacket();
        return ret;
    }
    if (formatPacket_.data == nullptr && formatPacket_.len == 0) {
        PushPacketToADecoder();
        PushPacketToVDecoder();
        strmReadEnd_ = true;
        ClearCachePacket();
        return HI_SUCCESS;
    }
    if (formatPacket_.frameType == FRAME_TYPE_AUDIO && audioDecoder_ != nullptr) {
        PushPacketToADecoder();
    } else if (formatPacket_.frameType == FRAME_TYPE_VIDEO  && videoDecoder_ != nullptr) {
        PushPacketToVDecoder();
    }
    return HI_SUCCESS;
}

int32_t PlayerControl::ReadTplayData()
{
    int32_t ret = HI_SUCCESS;
    if (cachedPacket_) {
        return ret;
    }

    ret = TPlayBeforeFrameRead();
    if (ret == HI_RET_FILE_EOF) {
        if (memset_s(&formatPacket_, sizeof(formatPacket_), 0, sizeof(FormatFrame)) != EOK) {
            MEDIA_ERR_LOG("SET  formatPacket_ failed");
            return HI_FAILURE;
        }
        cachedPacket_ = true;  // send eos
        return HI_SUCCESS;
    } else if (ret != HI_SUCCESS) {
        NotifyError(PLAYERCONTROL_ERROR_DEMUX_FAIL);
        return HI_FAILURE;
    }
    ret = ReadFrameFromSource(formatPacket_);
    if (ret == HI_RET_FILE_EOF) {
        if (memset_s(&formatPacket_, sizeof(formatPacket_), 0, sizeof(FormatFrame)) != EOK) {
            MEDIA_ERR_LOG("SET  formatPacket_ failed");
            return HI_FAILURE;
        }
    } else if (ret != HI_SUCCESS) {
        MEDIA_ERR_LOG("ReadFrameFromSource failed , ret:%d", ret);
        NotifyError(PLAYERCONTROL_ERROR_DEMUX_FAIL);
        return HI_FAILURE;
    }
    cachedPacket_ = true;
    lastReadPktStrmIdx_ = formatPacket_.trackId;
    ret = TPlayAfterFrameRead(formatPacket_);
    if (ret == HI_RET_SKIP_PACKET) {
        ClearCachePacket();
        return ret;
    }
    isTplayStartRead_ = true;
    strmReadEnd_ = false;
    return ret;
}

int32_t PlayerControl::DoStop()
{
    if (CheckIsNull(stateMachine_, "stateMachine_ nullptr")) {
        return HI_ERR_PLAYERCONTROL_NULL_PTR;
    }
    int32_t ret = HI_SUCCESS;
    PlayerStatus playerState = stateMachine_->GetCurState();
    if (CheckStateSame(playerState, PLAY_STATUS_IDLE)) {
        return HI_SUCCESS;
    }

    MsgInfo msgInfo;
    if (memset_s(&msgInfo, sizeof(MsgInfo), 0x00, sizeof(msgInfo)) != EOK) {
        MEDIA_ERR_LOG("memset_s msgInfo failed");
        return HI_ERR_PLAYERCONTROL_MEM_SET;
    }
    msgInfo.what = PLAYERCONTROL_MSG_SEEK;
    (void)stateMachine_->RemoveEvent(msgInfo);
    if (schProcess_ != 0) {
        pthread_mutex_lock(&schMutex_);
        schThreadExit_ = true;
        pthread_cond_signal(&schCond_);
        pthread_mutex_unlock(&schMutex_);
        pthread_join(schProcess_, nullptr);
    }
    StopSinkAndDecoder();
    ClearCachePacket();
    if (playerSource_ != nullptr) {
        ret = playerSource_->Stop();
        (void)CheckIsFailed(ret, HI_SUCCESS, "playerSource_ stop failed");
    }
    isPlayEnd_ = false;
    strmReadEnd_ = false;
    isPlayErr_ = false;
    lastRendPos_ = 0;
    lastReadPktPts_ = 0;
    lastSendPktPts_ = 0;
    isAudPlayEos_ = false;
    isVidPlayEos_ = false;
    hasRenderVideoEos_ = false;
    hasRenderAudioEos_ = false;
    lastNotifyTime_ = 0;
    seekToTimeMs_ = -1;
    if (memset_s(&(fmtFileInfo_), sizeof(fmtFileInfo_), 0, sizeof(FormatFrame)) != EOK) {
        MEDIA_ERR_LOG("SET fmtFileInfo_ failed ");
        return HI_ERR_PLAYERCONTROL_MEM_SET;
    }
    return ret;
}

int32_t PlayerControl::DoPause(void)
{
    if (CheckIsNull(stateMachine_, "stateMachine_ nullptr")) {
        return HI_ERR_PLAYERCONTROL_NULL_PTR;
    }
    PlayerStatus playerState = stateMachine_->GetCurState();
    if (CheckStateSame(playerState, PLAY_STATUS_PAUSE)) {
        return HI_SUCCESS;
    }
    if (CheckIsNull(sinkManager_, "sinkManager_ nullptr")) {
        return HI_FAILURE;
    }

    if (playerState == PLAY_STATUS_PLAY || (playerState == PLAY_STATUS_TPLAY)) {
        pthread_mutex_lock(&schMutex_);
        paused_ = true;
        pthread_mutex_unlock(&schMutex_);
        int32_t ret = sinkManager_->Pause();
        if (CheckIsFailed(ret, HI_SUCCESS, "Pause failed")) {
            return ret;
        }
    } else {
        return HI_ERR_PLAYERCONTROL_ILLEGAL_STATE_ACTION;
    }
    return HI_SUCCESS;
}

int32_t PlayerControl::DoSeekIfNeed(void)
{
    uint32_t index = 0;
    uint32_t seekCnt = 0;
    if (seekTabel_[0] == -1 && seekTabel_[1] == -1) {
        return HI_SUCCESS;
    }

    while (seekCnt < 0x2) {
        if (seekTabel_[index] != -1) {
            int32_t ret = AyncSeek(seekTabel_[index]);
            if (ret != HI_SUCCESS) {
                MEDIA_ERR_LOG(" AyncSeek  failed , ret:%d", ret);
                NotifyError(PLAYERCONTROL_ERROR_VID_PLAY_FAIL);
                return ret;
            }
            seekCnt++;
        }
        seekTabel_[index] = -1;
        if (seekTabel_[0] == -1 && seekTabel_[1] == -1) {
            break;
        }
        index = (index + 1) & 0x1;
    }
    strmReadEnd_ = false;
    isAudPlayEos_ = false;
    isVidPlayEos_ = false;
    hasRenderVideoEos_ = false;
    hasRenderAudioEos_ = false;
    isPlayEnd_ = false;

    firstAudioFrameAfterSeek_ = true;
    firstVideoFrameAfterSeek_ = true;
    MEDIA_INFO_LOG("seek end");
    return HI_SUCCESS;
}

int32_t PlayerControl::DoSeek(int64_t timeInMs)
{
    seekToTimeMs_ = timeInMs;
    return DoSeekIfNeed();
}

int32_t PlayerControl::DoGetFileInfo(FormatFileInfo &fileInfo)
{
    return memcpy_s(&fileInfo, sizeof(FormatFileInfo), &fmtFileInfo_, sizeof(fmtFileInfo_));
}

int32_t PlayerControl::DoSetMedia(PlayerControlStreamAttr &mediaAttr)
{
    if (CheckIsNull(playerSource_, "playerSource_ nullptr")) {
        return HI_ERR_PLAYERCONTROL_NULL_PTR;
    }
    int32_t ret = HI_SUCCESS;
    if (mediaAttr.s32VidStreamId != -1) {
        ret = playerSource_->SelectTrack(0, mediaAttr.s32VidStreamId);
        if (CheckIsFailed(ret, 0, "SelectTrack failed")) {
            return HI_ERR_PLAYERCONTROL_DEMUX_ERROR;
        }
        fmtFileInfo_.s32UsedVideoStreamIndex = mediaAttr.s32VidStreamId;
    }
    if (mediaAttr.s32AudStreamId != -1) {
        ret = playerSource_->SelectTrack(0, mediaAttr.s32AudStreamId);
        if (CheckIsFailed(ret, 0, "SelectTrack failed")) {
            return HI_ERR_PLAYERCONTROL_DEMUX_ERROR;
        }
        fmtFileInfo_.s32UsedAudioStreamIndex = mediaAttr.s32AudStreamId;
    }

    return HI_SUCCESS;
}

int32_t PlayerControl::GetStreamInfo(PlayerStreamInfo &streamInfo)
{
    if (CheckIsNull(stateMachine_, "stateMachine_ nullptr")) {
        return HI_ERR_PLAYERCONTROL_NULL_PTR;
    }
    streamInfo.avStatus.syncStatus.lastAudPts = AV_INVALID_PTS;
    streamInfo.avStatus.syncStatus.lastVidPts = AV_INVALID_PTS;
    if (sinkManager_->GetStatus(streamInfo) != HI_SUCCESS) {
        return HI_FAILURE;
    }
    PlayerStatus status = stateMachine_->GetCurState();
    if (status == PLAY_STATUS_PLAY) {
        streamInfo.avStatus.vidStatus = (isVideoStarted_) ? (DecoderStatus)status : DECODER_STATUS_IDLE;
        streamInfo.avStatus.audStatus = (isAudioStarted_) ? (DecoderStatus)status : DECODER_STATUS_IDLE;
    } else {
        streamInfo.avStatus.audStatus = (DecoderStatus)status;
    }

    streamInfo.audioInfo.channel = adecAttr_.channel;
    streamInfo.audioInfo.bitwith = adecAttr_.bitWidth;
    streamInfo.audioInfo.sampleRate = adecAttr_.sampleRate;
    streamInfo.audioInfo.esBufSize = streamAttr_.audioBufSize;
    streamInfo.audioInfo.freeEsBufSize = streamAttr_.audioBufSize >> 1;
    streamInfo.videoInfo.esBufSize = streamAttr_.videoBufSize;
    streamInfo.videoInfo.freeEsBufSize = streamAttr_.videoBufSize >> 1;
    return HI_SUCCESS;
}


int32_t PlayerControl::IsRepeatTplayReq(TplayAttr &tplayAttr, bool &isRepeat)
{
    if (CheckIsNull(stateMachine_, "stateMachine_ nullptr")) {
        return HI_ERR_PLAYERCONTROL_NULL_PTR;
    }
    isRepeat = false;
    if (stateMachine_->GetCurState() == PLAY_STATUS_TPLAY &&
        tplayAttr_.direction == tplayAttr.direction &&
        tplayAttr_.speed == tplayAttr.speed) {
        MEDIA_ERR_LOG("TPlay input Attr same with current");
        isRepeat = true;
        return HI_SUCCESS;
    }
    PlayerStreamInfo streamInfo;
    int64_t lastVidRendPts;
    int32_t ret = GetStreamInfo(streamInfo);
    if (CheckIsFailed(ret, HI_SUCCESS, "GetStreamInfo failed")) {
        return ret;
    }
    lastVidRendPts = streamInfo.avStatus.syncStatus.lastVidPts;
    if (tplayAttr_.direction == TPLAY_DIRECT_BACKWARD  &&
        tplayAttr.direction == TPLAY_DIRECT_BACKWARD &&
        lastVidRendPts == 0) {
        MEDIA_DEBUG_LOG("TPlay already played to start");
        isRepeat = true;
        return HI_SUCCESS;
    }
    return HI_SUCCESS;
}

int32_t PlayerControl::DoTPlay(TplayAttr &trickPlayAttr)
{
    isTplayLastFrame_ = false;
    isTplayStartRead_ = false;
    if (fmtFileInfo_.enVideoType == CODEC_JPEG) {
        MEDIA_ERR_LOG("jpeg file do not support tplay");
        return HI_ERR_PLAYERCONTROL_NOT_SUPPORT;
    }
    if (fmtFileInfo_.s32UsedVideoStreamIndex == HI_DEMUXER_NO_MEDIA_STREAM) {
        MEDIA_ERR_LOG("only audio file do not support tplay");
        return HI_ERR_PLAYERCONTROL_NOT_SUPPORT;
    }
    bool isRepeat = false;
    int32_t ret = IsRepeatTplayReq(trickPlayAttr, isRepeat);
    if (ret != HI_SUCCESS) {
        return ret;
    }
    if (isRepeat) {
        return HI_SUCCESS;
    }
    if (memcpy_s(&tplayAttr_, sizeof(tplayAttr_), &trickPlayAttr, sizeof(trickPlayAttr)) != EOK) {
        MEDIA_ERR_LOG("copy tplayAttr_ fail");
        return HI_FAILURE;
    }
    pthread_mutex_lock(&schMutex_);
    ret = TPlayResetBuffer();
    if (ret != HI_SUCCESS) {
        MEDIA_ERR_LOG("TPlayResetBuffer failed, ret:%d", ret);
        pthread_mutex_unlock(&schMutex_);
        return ret;
    }
    isVidContinueLost_ = false;
    ret = sinkManager_->SetSpeed(tplayAttr_.speed, tplayAttr_.direction);
    if (ret != HI_SUCCESS) {
        MEDIA_ERR_LOG("TPlayResetBuffer failed, ret:%d", ret);
    }
    curSeekOffset_ = TPlayGetSeekOffset(tplayAttr_.speed, tplayAttr_.direction);
    /* for ts file, double step */
    if (fmtFileInfo_.formatName != nullptr && strstr(fmtFileInfo_.formatName, "mpegts") != NULL) {
        curSeekOffset_ = curSeekOffset_ * 0x2;
    }

    tplayMode_ = TPlayGetPlayMode();

    lastReadPktStrmIdx_ = (uint32_t)fmtFileInfo_.s32UsedVideoStreamIndex;
    strmReadEnd_ = false;
    isVidPlayEos_ = false;
    isAudPlayEos_ = false;
    hasRenderVideoEos_ = false;
    hasRenderAudioEos_ = false;
    lastSendAdecPts_ = AV_INVALID_PTS;
    pthread_mutex_unlock(&schMutex_);
    return HI_SUCCESS;
}

}
}
