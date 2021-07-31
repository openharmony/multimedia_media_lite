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

#include "recorder_client.h"

#include "media_log.h"
#include "pms_interface.h"
#include "recorder_service.h"
#include "samgr_lite.h"
#include "securec.h"
#include "surface_impl.h"

extern "C" void __attribute__((weak)) OHOS_SystemInit(void)
{
    SAMGR_Bootstrap();
}

namespace OHOS {
namespace Media {
struct CallBackPara {
    RecFunc funcId;
    int32_t ret;
    int32_t data;
};

static int32_t SimpleCallback(void *owner, int code, IpcIo *reply)
{
    if (code != 0) {
        MEDIA_ERR_LOG("callback error, code = %d", code);
        return -1;
    }

    if (owner == nullptr) {
        return -1;
    }
    CallBackPara *para = (CallBackPara *)owner;
    MEDIA_INFO_LOG("Callback, funcId = %d", para->funcId);
    para->ret = IpcIoPopInt32(reply);
    return 0;
}

static int32_t RecorderCallbackSvc(const IpcContext *context, void *ipcMsg, IpcIo *io, void *arg)
{
    if (ipcMsg == nullptr || arg == nullptr) {
        MEDIA_ERR_LOG("call back error, ipcMsg is null\n");
        return MEDIA_ERR;
    }

    /* Not need to check if callback is valid because message will not arrive before callback ready */
    std::shared_ptr<RecorderCallback> *callback = (static_cast<std::shared_ptr<RecorderCallback> *>(arg));

    uint32_t funcId;
    (void)GetCode(ipcMsg, &funcId);
    MEDIA_INFO_LOG("DeviceCallback, funcId=%d", funcId);
    switch (funcId) {
        case REC_ANONYMOUS_FUNC_ON_ERROR: {
            int32_t type = IpcIoPopInt32(io);
            int32_t code = IpcIoPopInt32(io);
            (*callback)->OnError(type, code);
            break;
        }
        case REC_ANONYMOUS_FUNC_ON_INFO: {
            int32_t type = IpcIoPopInt32(io);
            int32_t code = IpcIoPopInt32(io);
            (*callback)->OnInfo(type, code);
            break;
        }
        default: {
            MEDIA_ERR_LOG("Unsupport callback service.(fundId=%d)", funcId);
            break;
        }
    }
    FreeBuffer(nullptr, ipcMsg);
    return MEDIA_OK;
}

Recorder::RecorderImpl::RecorderImpl()
{
    OHOS_SystemInit();

    IUnknown *iUnknown = SAMGR_GetInstance()->GetDefaultFeatureApi(RECORDER_SERVICE_NAME);
    if (iUnknown == nullptr) {
        MEDIA_ERR_LOG("iUnknown is NULL");
        throw runtime_error("Ipc proxy GetDefaultFeatureApi failed.");
    }

    (void)iUnknown->QueryInterface(iUnknown, CLIENT_PROXY_VER, (void **)&proxy_);
    if (proxy_ == nullptr) {
        MEDIA_ERR_LOG("QueryInterface failed");
        throw runtime_error("Ipc proxy init failed.");
    }

    CallBackPara para = {.funcId = REC_FUNC_CONNECT, .ret = MEDIA_IPC_FAILED};
    int32_t ret = proxy_->Invoke(proxy_, REC_FUNC_CONNECT, nullptr, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("Connect recorder server failed, ret=%d", ret);
        throw runtime_error("Ipc proxy Invoke failed.");
    }

    ret = RegisterIpcCallback(RecorderCallbackSvc, 0, IPC_WAIT_FOREVER, &sid_, &callback_);
    if (ret != LITEIPC_OK) {
        MEDIA_ERR_LOG("RegisteIpcCallback failed, ret=%d", ret);
        throw runtime_error("Ipc proxy RegisterIpcCallback failed.");
    }
    MEDIA_ERR_LOG("Create recorder client succeed.");
}

Recorder::RecorderImpl::~RecorderImpl()
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_DISCONNECT, &io, nullptr, nullptr);
    if (ret != 0) {
        MEDIA_ERR_LOG("Disconnect recorder server failed, ret=%d", ret);
    }
    UnregisterIpcCallback(sid_);
}

static int32_t SetSourceCallback(void *owner, int code, IpcIo *reply)
{

    if (code != 0) {
        MEDIA_ERR_LOG("callback error, code = %d", code);
        return -1;
    }

    if (owner == nullptr) {
        return -1;
    }
    CallBackPara *para = (CallBackPara *)owner;
    MEDIA_INFO_LOG("Callback, funcId = %d", para->funcId);
    para->ret = IpcIoPopInt32(reply);
    para->data = IpcIoPopInt32(reply);

    return 0;
}

int32_t Recorder::RecorderImpl::SetVideoSource(VideoSourceType source, int32_t &sourceId)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    IpcIoPushFlatObj(&io, &source, sizeof(source));
    CallBackPara para = {.funcId = REC_FUNC_SET_VIDEOSOURCE, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_SET_VIDEOSOURCE, &io, &para, SetSourceCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%d", ret);
    }
    if (para.ret == 0) {
        sourceId = para.data;
    };
    return para.ret;
}

int32_t Recorder::RecorderImpl::SetVideoEncoder(int32_t sourceId, VideoCodecFormat encoder)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    IpcIoPushInt32(&io, sourceId);
    IpcIoPushFlatObj(&io, &encoder, sizeof(encoder));
    CallBackPara para = {.funcId = REC_FUNC_SET_VIDEOENCODER, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_SET_VIDEOENCODER, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%d", ret);
    }
    return para.ret;
}

int32_t Recorder::RecorderImpl::SetVideoSize(int32_t sourceId, int32_t width, int32_t height)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    MEDIA_DEBUG_LOG("RecorderClient SetVideoSize. (sourceId=%d, width=%d, height=%d)", sourceId, width, height);
    IpcIoPushInt32(&io, sourceId);
    IpcIoPushInt32(&io, width);
    IpcIoPushInt32(&io, height);
    CallBackPara para = {.funcId = REC_FUNC_SET_VIDEOSIZE, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_SET_VIDEOSIZE, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%d", ret);
    }
    return para.ret;
}

int32_t Recorder::RecorderImpl::SetVideoFrameRate(int32_t sourceId, int32_t frameRate)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    IpcIoPushInt32(&io, sourceId);
    IpcIoPushInt32(&io, frameRate);
    CallBackPara para = {.funcId = REC_FUNC_SET_VIDEOFRAMERATE, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_SET_VIDEOFRAMERATE, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%d", ret);
    }
    return para.ret;
}

int32_t Recorder::RecorderImpl::SetVideoEncodingBitRate(int32_t sourceId, int32_t rate)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    IpcIoPushInt32(&io, sourceId);
    IpcIoPushInt32(&io, rate);
    CallBackPara para = {.funcId = REC_FUNC_SET_VIDEOENCODINGBITRATE, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_SET_VIDEOENCODINGBITRATE, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%d", ret);
    }
    return para.ret;
}

int32_t Recorder::RecorderImpl::SetCaptureRate(int32_t sourceId, double fps)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    IpcIoPushInt32(&io, sourceId);
    IpcIoPushFlatObj(&io, &fps, sizeof(fps));
    CallBackPara para = {.funcId = REC_FUNC_SET_CAPTURERATE, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_SET_CAPTURERATE, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%d", ret);
    }
    return para.ret;
}

static int32_t GetSurfaceCallback(void *owner, int code, IpcIo *io)
{
    if (code != 0) {
        MEDIA_ERR_LOG("callback error, code = %d", code);
        return -1;
    }

    if (owner == nullptr) {
        return -1;
    }
    Surface **surface = (Surface **)owner;
    *surface = OHOS::SurfaceImpl::GenericSurfaceByIpcIo(*io);
    return 0;
}

std::shared_ptr<OHOS::Surface> Recorder::RecorderImpl::GetSurface(int32_t sourceId)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    IpcIoPushInt32(&io, sourceId);
    Surface *surface = nullptr;
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_GET_SURFACE, &io, &surface, GetSurfaceCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%d", ret);
    }
    printf("Recorder::RecorderImpl::GetSurface surface=0x%x", surface);
    return std::shared_ptr<OHOS::Surface>(surface);
}

int32_t Recorder::RecorderImpl::SetAudioSource(AudioSourceType source, int32_t &sourceId)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    IpcIoPushFlatObj(&io, &source, sizeof(source));
    CallBackPara para = {.funcId = REC_FUNC_SET_AUDIOSOURCE, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_SET_AUDIOSOURCE, &io, &para, SetSourceCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%d", ret);
    }
    if (para.ret == 0) {
        sourceId = para.data;
    };
    return para.ret;
}

int32_t Recorder::RecorderImpl::SetAudioEncoder(int32_t sourceId, AudioCodecFormat encoder)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    IpcIoPushInt32(&io, sourceId);
    IpcIoPushFlatObj(&io, &encoder, sizeof(encoder));
    CallBackPara para = {.funcId = REC_FUNC_SET_AUDIOENCODER, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_SET_AUDIOENCODER, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%d", ret);
    }
    return para.ret;
}

int32_t Recorder::RecorderImpl::SetAudioSampleRate(int32_t sourceId, int32_t rate)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    IpcIoPushInt32(&io, sourceId);
    IpcIoPushInt32(&io, rate);
    CallBackPara para = {.funcId = REC_FUNC_SET_AUDIOSAMPLERATE, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_SET_AUDIOSAMPLERATE, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%d", ret);
    }
    return para.ret;
}

int32_t Recorder::RecorderImpl::SetAudioChannels(int32_t sourceId, int32_t num)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    IpcIoPushInt32(&io, sourceId);
    IpcIoPushInt32(&io, num);
    CallBackPara para = {.funcId = REC_FUNC_SET_AUDIOCHANNELS, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_SET_AUDIOCHANNELS, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%d", ret);
    }
    return para.ret;
}

int32_t Recorder::RecorderImpl::SetAudioEncodingBitRate(int32_t sourceId, int32_t bitRate)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    IpcIoPushInt32(&io, sourceId);
    IpcIoPushInt32(&io, bitRate);
    CallBackPara para = {.funcId = REC_FUNC_SET_AUDIOENCODINGBITRATE, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_SET_AUDIOENCODINGBITRATE, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%d", ret);
    }
    return para.ret;
}

int32_t Recorder::RecorderImpl::SetMaxDuration(int32_t duration)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    IpcIoPushInt32(&io, duration);
    CallBackPara para = {.funcId = REC_FUNC_SET_MAXDURATION, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_SET_MAXDURATION, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%d", ret);
    }
    return para.ret;
}

int32_t Recorder::RecorderImpl::SetOutputFormat(OutputFormatType format)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    IpcIoPushFlatObj(&io, &format, sizeof(format));
    CallBackPara para = {.funcId = REC_FUNC_SET_MAXDURATION, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_SET_MAXDURATION, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%d", ret);
    }
    return para.ret;
}

int32_t Recorder::RecorderImpl::SetOutputPath(const string &path)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    IpcIoPushString(&io, path.c_str());
    CallBackPara para = {.funcId = REC_FUNC_SET_OUTPUTPATH, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_SET_OUTPUTPATH, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%d", ret);
    }
    return para.ret;
}

int32_t Recorder::RecorderImpl::SetOutputFile(int32_t fd)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 1);
    IpcIoPushFd(&io, fd);
    CallBackPara para = {.funcId = REC_FUNC_SET_OUTPUTFILE};
    int32_t ret = proxy_->Invoke(proxy_, REC_FUNC_SET_OUTPUTFILE, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetOutputFile failed, ret=%d", ret);
    }

    return para.ret;
}

int32_t Recorder::RecorderImpl::SetNextOutputFile(int32_t fd)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 1);
    IpcIoPushFd(&io, fd);
    CallBackPara para = {.funcId = REC_FUNC_SET_NEXTOUTPUTFILE};
    int32_t ret = proxy_->Invoke(proxy_, REC_FUNC_SET_NEXTOUTPUTFILE, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetNextOutputFile failed, ret=%d", ret);
    }

    return para.ret;
}

int32_t Recorder::RecorderImpl::SetMaxFileSize(int64_t size)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    IpcIoPushInt64(&io, size);
    CallBackPara para = {.funcId = REC_FUNC_SET_MAXFILESIZE, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_SET_MAXFILESIZE, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%d", ret);
    }
    return para.ret;
}

int32_t Recorder::RecorderImpl::SetRecorderCallback(const std::shared_ptr<RecorderCallback> &callback)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 1);
    IpcIoPushSvc(&io, &sid_);
    CallBackPara para = {.funcId = REC_FUNC_SET_RECORDERCALLBACK, .ret = MEDIA_IPC_FAILED};
    int32_t ret = proxy_->Invoke(proxy_, REC_FUNC_SET_RECORDERCALLBACK, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetRecorderCallback failed, ret=%d", ret);
        return -1;
    }
    if (para.ret == 0) {
        callback_ = callback;
    }

    return para.ret;
}

int32_t Recorder::RecorderImpl::Prepare()
{
    CallBackPara para = {.funcId = REC_FUNC_PREPARE, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_PREPARE, nullptr, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%d", ret);
    }
    return para.ret;
}

int32_t Recorder::RecorderImpl::Start()
{
    CallBackPara para = {.funcId = REC_FUNC_START, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_START, nullptr, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%d", ret);
    }
    return para.ret;
}

int32_t Recorder::RecorderImpl::Pause()
{
    CallBackPara para = {.funcId = REC_FUNC_PAUSE, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_PAUSE, nullptr, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%d", ret);
    }
    return para.ret;
}

int32_t Recorder::RecorderImpl::Resume()
{
    CallBackPara para = {.funcId = REC_FUNC_RESUME, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_RESUME, nullptr, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%d", ret);
    }
    return para.ret;
}

int32_t Recorder::RecorderImpl::Stop(bool block)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    IpcIoPushBool(&io, block);
    CallBackPara para = {.funcId = REC_FUNC_STOP, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_STOP, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%d", ret);
    }
    return para.ret;
}

int32_t Recorder::RecorderImpl::Reset()
{
    CallBackPara para = {.funcId = REC_FUNC_RESET, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_RESET, nullptr, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%d", ret);
    }
    return para.ret;
}

int32_t Recorder::RecorderImpl::Release()
{
    CallBackPara para = {.funcId = REC_FUNC_RELEASE, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_RELEASE, nullptr, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%d", ret);
    }
    return para.ret;
}

int32_t Recorder::RecorderImpl::SetFileSplitDuration(FileSplitType type, int64_t timestamp, uint32_t duration)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    IpcIoPushFlatObj(&io, &type, sizeof(type));
    IpcIoPushInt64(&io, timestamp);
    IpcIoPushUint32(&io, duration);
    CallBackPara para = {.funcId = REC_FUNC_SET_FILESPLITDURATION, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_SET_FILESPLITDURATION, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%d", ret);
    }
    return para.ret;
}

int32_t Recorder::RecorderImpl::SetParameter(int32_t sourceId, const Format &format)
{
    return -1;
}
} /* namespace Media */
} /* namespace OHOS */
