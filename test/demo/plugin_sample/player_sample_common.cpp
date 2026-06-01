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

#include "player_sample_common.h"
#include <pthread.h>
#include <unistd.h>
#include <ctime>
#include <sys/time.h>

#define WAIT_TIME_MS 1000

using namespace OHOS;
using namespace OHOS::Media;
using OHOS::Media::PlayerSource;
using OHOS::Media::Decoder;

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

static void InputFrameToDecoder(CommonData* data, FormatFrame& formatPacket)
{
    bool isInputted = false;
    int ret = 0;
    while (!isInputted) {
        InputInfo inputData;
        CodecBufferInfo inBufInfo;
        inputData.bufferCnt = 0;
        inputData.buffers = nullptr;
        inputData.pts = 0;
        inputData.flag = 0;
        ret = data->videoDecoder->DequeInputBuffer(inputData, 0);
        if (ret != 0) {
            printf("DequeInputBuffer failed ret %d\n", ret);
            usleep(WAIT_TIME_MS);
            continue;
        }
        inBufInfo.addr = formatPacket.data;
        inBufInfo.length = formatPacket.len;
        inputData.bufferCnt = 1;
        inputData.buffers = &inBufInfo;
        inputData.pts = formatPacket.timestampUs;
        inputData.flag = 0;
        ret = data->videoDecoder->QueueInputBuffer(inputData, 0);
        if (ret != 0) {
            printf("QueueInputBuffer failed ret %d\n", ret);
            usleep(WAIT_TIME_MS);
            continue;
        }
        if (!data->isSourceStarted) {
            data->isSourceStarted = true;
        }
        isInputted = true;
    }
}

void* SourceFunction(void* arg)
{
    CommonData* data = static_cast<CommonData*>(arg);
    FormatFrame formatPacket_;
    data->isEos = false;
    data->isSourceStarted = false;
    data->isSourceEnd = false;
    bool sourceEos = false;
    while (!sourceEos) {
        int ret = data->playerSource->ReadFrame(formatPacket_);
        if (ret == HI_RET_FILE_EOF) {
            printf("ReadFrame EOF\n");
            memset_s(&formatPacket_, sizeof(formatPacket_), 0, sizeof(FormatFrame));
            data->isSourceEnd = true;
        }
        if (formatPacket_.frameType == FRAME_TYPE_VIDEO || data->isSourceEnd) {
            InputFrameToDecoder(data, formatPacket_);
        }
        if (formatPacket_.data != nullptr) {
            data->playerSource->FreeFrame(formatPacket_);
            formatPacket_.data = nullptr;
            formatPacket_.len = 0;
            formatPacket_.trackId = -1;
        }
        if (data->isSourceEnd) {
            sourceEos = true;
        }
    }
    printf("SourceFunction thread exit\n");
    return nullptr;
}

void* DecoderFunction(void* arg)
{
    CommonData* data = static_cast<CommonData*>(arg);
    while (!data->isSourceStarted) {
        printf("DecoderFunction wait source start\n");
        usleep(WAIT_TIME_MS);
    }
    while (!data->isEos) {
        OutputInfo outInfo;
        int ret = data->videoDecoder->DequeueOutputBuffer(outInfo, 0);
        if (ret != 0) {
            if (ret == CODEC_RECEIVE_EOS) {
                printf("DequeueOutputBuffer receive EOS\n");
                data->isEos = true;
            }
            usleep(WAIT_TIME_MS);
        } else {
            pthread_rwlock_wrlock(&data->queueLock);
            data->frameBufferQueue.push(outInfo);
            pthread_rwlock_unlock(&data->queueLock);
        }
    }
    printf("DecoderFunction thread exit\n");
    return nullptr;
}

void* SinkFunction(void* arg)
{
    CommonData* data = static_cast<CommonData*>(arg);
    struct timeval startTime;
    int64_t startTimeMs = 0;
    while (!data->isEos) {
        pthread_rwlock_wrlock(&data->queueLock);
        if (!data->frameBufferQueue.empty()) {
            int timeMultiplier = 1000;
            OutputInfo outInfo = data->frameBufferQueue.front();
            if (outInfo.timeStamp == 0) {
                gettimeofday(&startTime, nullptr);
                startTimeMs = startTime.tv_sec * timeMultiplier + startTime.tv_usec / timeMultiplier;
            }
            struct timeval currentTime;
            gettimeofday(&currentTime, nullptr);
            int64_t currentTimeMs = currentTime.tv_sec * timeMultiplier + currentTime.tv_usec / timeMultiplier;
            int64_t elapsedTimeMs = 0;
            if (startTimeMs != 0) {
                elapsedTimeMs = currentTimeMs - startTimeMs;
            }
            while (elapsedTimeMs < outInfo.timeStamp) {
                int sleepTimeUs = 100;
                usleep(sleepTimeUs);
                gettimeofday(&currentTime, nullptr);
                currentTimeMs = currentTime.tv_sec * timeMultiplier + currentTime.tv_usec / timeMultiplier;
                elapsedTimeMs = currentTimeMs - startTimeMs;
            }
            LayerBuffer layerBuf;
            layerBuf.data.virAddr = outInfo.vendorPrivate;
            data->layerFuncs->Flush(0, data->layerId, &layerBuf);
            int decTimeout = 0;
            data->videoDecoder->QueueOutputBuffer(outInfo, decTimeout);
            data->frameBufferQueue.pop();
            pthread_rwlock_unlock(&data->queueLock);
        } else {
            pthread_rwlock_unlock(&data->queueLock);
            usleep(WAIT_TIME_MS);
        }
    }
    printf("SinkFunction thread exit\n");
    return nullptr;
}
int SinkInit(CommonData &data)
{
    int32_t devId = 0;
    IRect attr;
    attr.x = data.x;
    attr.y = data.y;
    attr.w = data.width;
    attr.h = data.height;
    LayerInfo lInfo;
    lInfo.width = data.width;
    lInfo.height = data.height;
    lInfo.type = LAYER_TYPE_OVERLAY;
    int align = 8;
    lInfo.bpp = align;
    lInfo.pixFormat = PIXEL_FMT_YCRCB_420_SP;
    (void)LayerInitialize(&data.layerFuncs);
    data.layerFuncs->CreateLayer(devId, &lInfo, &data.layerId);
    data.layerFuncs->SetLayerSize(devId, data.layerId, &attr);
    int maxLayerPriority = 3;
    data.layerFuncs->SetLayerPriority(maxLayerPriority);
    return 0;
}

static void PrintVideoResolution(const FormatFileInfo &fmtFileInfo, uint32_t &width, uint32_t &height)
{
    printf("used audiostream index %d\n", fmtFileInfo.s32UsedAudioStreamIndex);
    printf("used videostream index %d\n", fmtFileInfo.s32UsedVideoStreamIndex);
    for (uint32_t i = 0; i < HI_DEMUXER_RESOLUTION_CNT; i++) {
        if (fmtFileInfo.stStreamResolution[i].s32VideoStreamIndex == fmtFileInfo.s32UsedVideoStreamIndex) {
            printf("video enVideoType %d resolution %d %d\n", \
                fmtFileInfo.stStreamResolution[i].enVideoType, \
                fmtFileInfo.stStreamResolution[i].u32Width, \
                fmtFileInfo.stStreamResolution[i].u32Height);
            width = fmtFileInfo.stStreamResolution[i].u32Width;
            height = fmtFileInfo.stStreamResolution[i].u32Height;
        }
    }
}

int InitPlayer(CommonData &data)
{
    printf("InitPlayer fileName %s\n", data.fileName.c_str());
    if (pthread_rwlock_init(&data.queueLock, nullptr) != 0) {
        printf("Failed to initialize queue lock\n");
        return -1;
    }
    data.playerSource = std::make_shared<PlayerSource>();
    data.videoDecoder = std::make_shared<Decoder>();

    data.playerSource->Init();
    data.playerSource->SetSource(data.fileName.c_str());
    int32_t ret = data.playerSource->Prepare();
    FormatFileInfo fmtFileInfo;
    ret = data.playerSource->GetFileInfo(fmtFileInfo);
    uint32_t width = 0;
    uint32_t height = 0;
    PrintVideoResolution(fmtFileInfo, width, height);
    AvCodecMime mime = g_avCodecFormatInfo[fmtFileInfo.enVideoType].mime;
    OHOS::Media::AvAttribute attr;
    attr.type = VIDEO_DECODER;
    attr.vdecAttr.mime = mime;
    attr.vdecAttr.priv = nullptr;
    attr.vdecAttr.bufSize = 0;
    attr.vdecAttr.maxWidth = width;
    attr.vdecAttr.maxHeight = height;
    const std::string videoName = "codec.avc.soft.decoder";
    ret = data.videoDecoder->CreateHandle(videoName, attr);
    ret = data.videoDecoder->StartDec();
    data.playerSource->Start();
    ret = SinkInit(data);

    ret = pthread_create(&data.threadSource, nullptr, SourceFunction, &data);
    if (ret != 0) {
        printf("SourceFunction create failed\n");
        return -1;
    }
    ret = pthread_create(&data.threadDecoder, nullptr, DecoderFunction, &data);
    if (ret != 0) {
        printf("DecoderFunction create failed\n");
        return -1;
    }
    ret = pthread_create(&data.threadSink, nullptr, SinkFunction, &data);
    if (ret != 0) {
        printf("SinkFunction create failed\n");
        return -1;
    }
    return 0;
}

int DeinitPlayer(CommonData &data)
{
    pthread_join(data.threadSource, nullptr);
    printf("SourceFunction thread join\n");
    pthread_join(data.threadDecoder, nullptr);
    printf("DecoderFunction thread join\n");
    pthread_join(data.threadSink, nullptr);
    printf("SinkFunction thread join\n");
    if (data.layerFuncs != nullptr) {
        data.layerFuncs->CloseLayer(0, data.layerId);
    }
    data.playerSource->Stop();
    data.videoDecoder->StopDec();
    data.videoDecoder->DestroyHandle();
    pthread_rwlock_destroy(&data.queueLock);
    return 0;
}