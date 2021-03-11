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

#include <unistd.h>
#include <sys/time.h>
#include "player_video_sink.h"
#include "media_log.h"
extern "C"
{
#include "hal_display.h"
}

namespace OHOS {
namespace Media {

const int32_t MAX_VIDEO_QUEUE_BUF_NUM = 32;
const float VIDEO_FRAME_RATE_MAX = 120.0f;
const int32_t FLOAT_INT_SCALE = 1000;
const int32_t SS2US = 1000000;
const int32_t US2MS = 1000;

#define CHECK_FAILED_RETURN(value, target, ret, printfString) \
do { \
    if ((value) != (target)) { \
        MEDIA_ERR_LOG(" %s ", (printfString != nullptr) ? (printfString) : " "); \
        return (ret); \
    } \
} while (0)

#define CHECK_NULL_RETURN(value, ret, printfString) \
do { \
    if ((value) == nullptr) { \
        MEDIA_ERR_LOG(" %s ", (printfString != nullptr) ? (printfString) : " "); \
        return (ret); \
    } \
} while (0)

static int64_t GetCurTimeUs()
{
    struct timeval ts;
    ts.tv_sec = 0;
    ts.tv_usec = 0;
    gettimeofday(&ts, nullptr);
    return (((int64_t)ts.tv_sec) * SS2US) + ((int64_t)ts.tv_usec);
}

static int64_t GetCurTimeMs()
{
    int64_t curTimeUs = GetCurTimeUs();
    return (int64_t)(curTimeUs / US2MS);
}

VideoSink::VideoSink(void)
    : handle_(0), speed_(1.0f), paused_(false), started_(false), syncHdl_(nullptr),
      renderFrameCnt_(0), renderMode_(RENDER_MODE_NORMAL), rendStartTime_(-1), lastRendPts_(AV_INVALID_PTS),
      recievedEos_(false), EosPts_(AV_INVALID_PTS), pauseAfterPlay_(false), firstVidRend_(false),
      lastRendCnt_(0), vidRendStartTime_(AV_INVALID_PTS), eosSended_(false)
{
    ResetRendStartTime();
    attr_.sinkType = SINK_TYPE_BUT;
    attr_.trackId = 0;
    attr_.vidAttr.format = 0;
    attr_.vidAttr.width = 0;
    attr_.vidAttr.height = 0;
    attr_.vidAttr.frameRate = 0.0;
    attr_.vidAttr.surface = nullptr;
    lastRendSysTimeMs_ = GetCurTimeMs();
    callBack_.onEventCallback = nullptr;
    callBack_.priv = nullptr;
    frameCacheQue_.clear();
    frameReleaseQue_.clear();
}

VideoSink::~VideoSink()
{
    (void)DeInit();
}

int32_t VideoSink::DeInit()
{
    int32_t ret = HI_SUCCESS;
    HalPlayerVoDeinit(handle_);
    return ret;
}

int32_t VideoSink::Init(SinkAttr &attr)
{
    attr_ = attr;
    return HalPlayerVoInit(&handle_);
}

void VideoSink::GetStatus(VideoSinkStatus &status)
{
    status.vidFrameCount = renderFrameCnt_;
    status.decHeight = attr_.vidAttr.height;
    status.decWidth = attr_.vidAttr.width;
    if (vidRendStartTime_ != AV_INVALID_PTS) {
        int64_t diffTimeMs = GetCurTimeMs() - vidRendStartTime_;
        if (diffTimeMs > MS_SCALE) {
            double frameCnt = lastRendCnt_;
            double time = diffTimeMs;
            double frameRate = frameCnt / time;
            if (frameRate >= VIDEO_FRAME_RATE_MAX || frameRate < 0) {
                status.fpsInteger = 0;
                status.fpsDecimal = 0;
                return;
            }

            uint32_t tmp = frameRate * FLOAT_INT_SCALE;
            status.fpsInteger = (tmp / FLOAT_INT_SCALE);
            status.fpsDecimal = (tmp % FLOAT_INT_SCALE);
            return;
        }
    }
    status.fpsInteger = 0;
    status.fpsDecimal = 0;
}

int32_t VideoSink::Start()
{
    vidRendStartTime_ = GetCurTimeMs();
    CHECK_FAILED_RETURN(HalStartVideoOutput(handle_), 0, -1, "HalStartVideoOutput failed");
    if (syncHdl_ != nullptr) {
        syncHdl_->Start(SYNC_CHN_VID);
    }
    started_ = true;
    return HI_SUCCESS;
}

void VideoSink::SetRenderMode(RenderMode mode)
{
    // use to control report first video frame
    renderMode_ = mode;
}

int32_t VideoSink::Stop()
{
    RelaseQueAllFrame();
    ResetRendStartTime();
    renderFrameCnt_ = 0;
    CHECK_FAILED_RETURN(HalStopVideoOutput(handle_), 0, -1, "HalStopVideoOutput failed");
    started_ = false;
    return HI_SUCCESS;
}

int32_t VideoSink::Pause()
{
    if (paused_) {
        MEDIA_ERR_LOG("avRender already paused");
        return HI_SUCCESS;
    }
    if (started_ == false) {
        MEDIA_ERR_LOG("avRender not in running");
        return HI_FAILURE;
    }

    ResetRendStartTime();
    paused_ = true;
    return HI_SUCCESS;
}

int32_t VideoSink::Resume(void)
{
    pauseAfterPlay_ = false;
    if (paused_ == false) {
        MEDIA_ERR_LOG("avRender not in pause");
        return HI_FAILURE;
    }

    vidRendStartTime_ = GetCurTimeMs();
    lastRendSysTimeMs_ = GetCurTimeMs();
    paused_ = false;
    return HI_SUCCESS;
}

int32_t VideoSink::Flush(void)
{
    if (started_ == false) {
        MEDIA_ERR_LOG("avRender not in started");
        return HI_FAILURE;
    }
    return HI_SUCCESS;
}

void VideoSink::ResetRendStartTime()
{
    lastRendCnt_ = 0;
    lastRendPts_ = AV_INVALID_PTS;
    vidRendStartTime_ = AV_INVALID_PTS;
}

int32_t VideoSink::Reset()
{
    RelaseQueAllFrame();
    Flush();
    ResetRendStartTime();
    recievedEos_ = false;
    firstVidRend_ = false;
    return HI_SUCCESS;
}


int32_t VideoSink::RegisterCallBack(PlayEventCallback &callback)
{
    callBack_ = callback;
    return 0;
}

int32_t VideoSink::GetRenderFrame(OutputInfo &renderFrame, OutputInfo &frame)
{
    std::lock_guard<std::mutex> lock(mutex_);
    int32_t ret = SINK_QUE_EMPTY;
    if (frame.type == VIDEO_DECODER && frame.bufferCnt != 0) {
        frameCacheQue_.push_back(frame);
    }
    if (frameCacheQue_.size() != 0) {
        renderFrame = frameCacheQue_[0];
        ret = SINK_SUCCESS;
    }
    return ret;
}

void VideoSink::RelaseQueHeadFrame(void)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (frameCacheQue_.size() != 0) {
        OutputInfo frame = frameCacheQue_[0];
        frameCacheQue_.erase(frameCacheQue_.begin());
        frameReleaseQue_.push_back(frame);
    }
}

void VideoSink::RelaseQueAllFrame(void)
{
    size_t i;
    size_t queSize;
    std::lock_guard<std::mutex> lock(mutex_);
    queSize = frameCacheQue_.size();
    if (queSize > MAX_VIDEO_QUEUE_BUF_NUM) {
        return;
    }
    for (i = 0; i < queSize; i++) {
        frameReleaseQue_.push_back(frameCacheQue_[i]);
    }
    frameCacheQue_.clear();
}

int32_t VideoSink::DequeReleaseFrame(OutputInfo &frame)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (frameReleaseQue_.size() == 0) {
        return SINK_QUE_EMPTY;
    }
    frame = frameReleaseQue_[0];
    frameReleaseQue_.erase(frameReleaseQue_.begin());
    return SINK_SUCCESS;
}

void VideoSink::RenderRptEvent(EventCbType event)
{
    if (callBack_.onEventCallback != nullptr) {
        if (event == EVNET_VIDEO_PLAY_EOS && eosSended_ == true) {
            return;
        }
        callBack_.onEventCallback(callBack_.priv, event, 0, 0);
        if (event == EVNET_VIDEO_PLAY_EOS) {
            eosSended_ = true;
        }
    }
}

int32_t VideoSink::WriteToVideoDevice(OutputInfo &renderFrame)
{
    int ret;
    Surface *surface = attr_.vidAttr.surface;
    if (surface != nullptr) {
        int32_t x = std::stoi(surface->GetUserData("region_position_x"));
        int32_t y = std::stoi(surface->GetUserData("region_position_y"));
        int32_t w = std::stoi(surface->GetUserData("region_width"));
        int32_t h = std::stoi(surface->GetUserData("region_height"));
        int32_t right = x + w - 1;
        int32_t botttom = y + h - 1;
        /* Make sure the coordinates are even */
        x = x - x % 2;
        y = y - y % 2;
        w = right - x + 1;
        h = botttom - y + 1;
        w = w + w % 2;
        h = h + h % 2;
        HalVideoOutputAttr voAttr = {x, y, w, h, 0};
        ret = HalConfigVideoOutput(handle_, voAttr);
        if (ret != 0) {
            MEDIA_ERR_LOG("ConfigVideoOutput failed:%d, x:%d, y:%d, w:%d, h:%d", ret, x, y, w, h);
        }
    }
    ret = HalWriteVo(handle_, renderFrame.vendorPrivate);
    RelaseQueHeadFrame();
    CHECK_FAILED_RETURN(ret, 0, -1, "HalWriteVo failed");
    return HI_SUCCESS;
}

int32_t VideoSink::RenderFrame(OutputInfo &frame)
{
    SyncRet syncRet = SYNC_RET_PLAY;
    int64_t crtPlayPts = 0;
    OutputInfo renderFrame;
    CHECK_FAILED_RETURN(started_, true, SINK_RENDER_ERROR, "not started");
    CHECK_FAILED_RETURN(paused_, false, SINK_RENDER_ERROR, "paused");
    if (GetRenderFrame(renderFrame, frame) != SINK_SUCCESS) {
        if (recievedEos_ == true) {
            RenderRptEvent(EVNET_VIDEO_PLAY_EOS);
            return SINK_RENDER_EOS;
        }
        return SINK_QUE_EMPTY;
    }
    if (pauseAfterPlay_) {
        return SINK_SUCCESS;
    }
    crtPlayPts = renderFrame.timeStamp;
    int32_t ret = (syncHdl_ != nullptr) ? syncHdl_->ProcVidFrame(crtPlayPts, syncRet) : HI_SUCCESS;
    if (ret != HI_SUCCESS) {
        MEDIA_ERR_LOG("AVPLAY_SYNC_Proc_VidFrame pts: %llu failed", renderFrame.timeStamp);
        return SINK_RENDER_ERROR;
    }
    lastRendPts_ = (renderFrame.timeStamp > lastRendPts_) ? renderFrame.timeStamp : lastRendPts_;
    if (syncRet == SYNC_RET_PLAY) {
        ret = WriteToVideoDevice(renderFrame);
    } else if (syncRet == SYNC_RET_DROP) {
        RelaseQueHeadFrame();
        ret = SINK_SUCCESS;
    } else if (syncRet == SYNC_RET_REPEAT) {
        ret = SINK_RENDER_DELAY;
    } else {
        MEDIA_ERR_LOG("aud invalid sync ret: %d", syncRet);
        ret =  SINK_RENDER_ERROR;
    }
    return ret;
}

void VideoSink::SetSync(PlayerSync *sync)
{
    syncHdl_ = sync;
}

void VideoSink::RenderEos(void)
{
    recievedEos_ = true;
    EosPts_ = lastRendPts_;
}

void VideoSink::GetRenderPosition(int64_t &position)
{
    position = lastRendPts_;
}
}
}
