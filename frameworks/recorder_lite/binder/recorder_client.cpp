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

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include "ipc_skeleton.h"
#include "media_log.h"
#include "pms_interface.h"
#include "recorder_common.h"
#include "samgr_lite.h"
#include "securec.h"
#include "surface_impl.h"

using namespace std;

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
    ReadInt32(reply, &(para->ret));
    return 0;
}

static int32_t RecorderCallbackSvc(uint32_t code, IpcIo *data, IpcIo *reply, MessageOption option)
{
    if (option.args == nullptr) {
        MEDIA_ERR_LOG("call back error, option.args is null\n");
        return MEDIA_ERR;
    }
    /* Not need to check if callback is valid because message will not arrive before callback ready */
    RecorderCallback *callback = static_cast<RecorderCallback *>(option.args);

    MEDIA_INFO_LOG("DeviceCallback, code=%d", code);
    switch (code) {
        case REC_ANONYMOUS_FUNC_ON_ERROR: {
            int32_t type;
            ReadInt32(data, &type);
            int32_t code;
            ReadInt32(data, &code);
            callback->OnError(type, code);
            break;
        }
        case REC_ANONYMOUS_FUNC_ON_INFO: {
            int32_t type;
            ReadInt32(data, &type);
            int32_t code;
            ReadInt32(data, &code);
            callback->OnInfo(type, code);
            break;
        }
        default: {
            MEDIA_ERR_LOG("Unsupport callback service.(code=%d)", code);
            break;
        }
    }
    return MEDIA_OK;
}

Recorder::RecorderClient::RecorderClient()
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
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    CallBackPara para = {.funcId = REC_FUNC_CONNECT, .ret = MEDIA_IPC_FAILED};
    int32_t ret = proxy_->Invoke(proxy_, REC_FUNC_CONNECT, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("Connect recorder server failed, ret=%d", ret);
        throw runtime_error("Ipc proxy Invoke failed.");
    }
    MEDIA_ERR_LOG("Create recorder client succeed.");
}

static int32_t DisConnectCallback(void *owner, int code, IpcIo *reply)
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
    return 0;
}


Recorder::RecorderClient::~RecorderClient()
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    CallBackPara para = {.funcId = REC_FUNC_DISCONNECT, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_DISCONNECT, &io, &para, DisConnectCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("Disconnect recorder server failed, ret=%u", ret);
    }
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
    ReadInt32(reply, &(para->ret));
    ReadInt32(reply, &(para->data));
    return 0;
}

int32_t Recorder::RecorderClient::SetVideoSource(VideoSourceType source, int32_t &sourceId)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);

    WriteRawData(&io, &source, sizeof(source));
    CallBackPara para = {.funcId = REC_FUNC_SET_VIDEOSOURCE, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_SET_VIDEOSOURCE, &io, &para, SetSourceCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%u", ret);
    }
    if (para.ret == 0) {
        sourceId = para.data;
    };
    return para.ret;
}

int32_t Recorder::RecorderClient::SetVideoEncoder(int32_t sourceId, VideoCodecFormat encoder)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    WriteInt32(&io, sourceId);
    WriteRawData(&io, &encoder, sizeof(encoder));

    CallBackPara para = {.funcId = REC_FUNC_SET_VIDEOENCODER, .ret = MEDIA_IPC_FAILED};
    int32_t ret = proxy_->Invoke(proxy_, REC_FUNC_SET_VIDEOENCODER, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%d", ret);
    }
    return para.ret;
}

int32_t Recorder::RecorderClient::SetVideoSize(int32_t sourceId, int32_t width, int32_t height)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    MEDIA_DEBUG_LOG("RecorderClient SetVideoSize. (sourceId=%d, width=%d, height=%d)", sourceId, width, height);
    WriteInt32(&io, sourceId);
    WriteInt32(&io, width);
    WriteInt32(&io, height);
    CallBackPara para = {.funcId = REC_FUNC_SET_VIDEOSIZE, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_SET_VIDEOSIZE, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%u", ret);
    }
    return para.ret;
}

int32_t Recorder::RecorderClient::SetVideoFrameRate(int32_t sourceId, int32_t frameRate)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    WriteInt32(&io, sourceId);
    WriteInt32(&io, frameRate);
    CallBackPara para = {.funcId = REC_FUNC_SET_VIDEOFRAMERATE, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_SET_VIDEOFRAMERATE, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%u", ret);
    }
    return para.ret;
}

int32_t Recorder::RecorderClient::SetVideoEncodingBitRate(int32_t sourceId, int32_t rate)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    WriteInt32(&io, sourceId);
    WriteInt32(&io, rate);
    CallBackPara para = {.funcId = REC_FUNC_SET_VIDEOENCODINGBITRATE, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_SET_VIDEOENCODINGBITRATE, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%u", ret);
    }
    return para.ret;
}

int32_t Recorder::RecorderClient::SetCaptureRate(int32_t sourceId, double fps)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    WriteInt32(&io, sourceId);
    WriteRawData(&io, &fps, sizeof(fps));
    CallBackPara para = {.funcId = REC_FUNC_SET_CAPTURERATE, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_SET_CAPTURERATE, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%u", ret);
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

std::shared_ptr<OHOS::Surface> Recorder::RecorderClient::GetSurface(int32_t sourceId)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    WriteInt32(&io, sourceId);
    Surface *surface = nullptr;
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_GET_SURFACE, &io, &surface, GetSurfaceCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%u", ret);
    }

    return std::shared_ptr<OHOS::Surface>(surface);
}

int32_t Recorder::RecorderClient::SetAudioSource(AudioSourceType source, int32_t &sourceId)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    WriteRawData(&io, &source, sizeof(source));
    CallBackPara para = {.funcId = REC_FUNC_SET_AUDIOSOURCE, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_SET_AUDIOSOURCE, &io, &para, SetSourceCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%u", ret);
    }
    if (para.ret == 0) {
        sourceId = para.data;
    };
    return para.ret;
}

int32_t Recorder::RecorderClient::SetAudioEncoder(int32_t sourceId, AudioCodecFormat encoder)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    WriteInt32(&io, sourceId);
    WriteRawData(&io, &encoder, sizeof(encoder));
    CallBackPara para = {.funcId = REC_FUNC_SET_AUDIOENCODER, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_SET_AUDIOENCODER, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%u", ret);
    }
    return para.ret;
}

int32_t Recorder::RecorderClient::SetAudioSampleRate(int32_t sourceId, int32_t rate)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    WriteInt32(&io, sourceId);
    WriteInt32(&io, rate);
    CallBackPara para = {.funcId = REC_FUNC_SET_AUDIOSAMPLERATE, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_SET_AUDIOSAMPLERATE, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%u", ret);
    }
    return para.ret;
}

int32_t Recorder::RecorderClient::SetAudioChannels(int32_t sourceId, int32_t num)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    WriteInt32(&io, sourceId);
    WriteInt32(&io, num);
    CallBackPara para = {.funcId = REC_FUNC_SET_AUDIOCHANNELS, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_SET_AUDIOCHANNELS, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%u", ret);
    }
    return para.ret;
}

int32_t Recorder::RecorderClient::SetAudioEncodingBitRate(int32_t sourceId, int32_t bitRate)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    WriteInt32(&io, sourceId);
    WriteInt32(&io, bitRate);
    CallBackPara para = {.funcId = REC_FUNC_SET_AUDIOENCODINGBITRATE, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_SET_AUDIOENCODINGBITRATE, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%u", ret);
    }
    return para.ret;
}

int32_t Recorder::RecorderClient::SetMaxDuration(int32_t duration)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    WriteInt32(&io, duration);
    CallBackPara para = {.funcId = REC_FUNC_SET_MAXDURATION, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_SET_MAXDURATION, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%u", ret);
    }
    return para.ret;
}

int32_t Recorder::RecorderClient::SetOutputFormat(OutputFormatType format)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    WriteRawData(&io, &format, sizeof(format));

    CallBackPara para = {.funcId = REC_FUNC_SET_OUTPUTFORMAT, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_SET_OUTPUTFORMAT, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%u", ret);
    }
    return para.ret;
}

int32_t Recorder::RecorderClient::SetOutputPath(const string &path)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    WriteString(&io, path.c_str());
    CallBackPara para = {.funcId = REC_FUNC_SET_OUTPUTPATH, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_SET_OUTPUTPATH, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%u", ret);
    }
    return para.ret;
}

static int32_t IsValidFileFd(int32_t fd)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags == -1) {
        MEDIA_ERR_LOG("Fail to get File Status Flags err: %d", errno);
        return ERR_INVALID_OPERATION;
    }
    // fd must be in read-write mode or write-only mode.
    uint32_t flagsCheck = static_cast<uint32_t>(flags);
    if ((flagsCheck & (O_RDWR | O_WRONLY)) == 0) {
        MEDIA_ERR_LOG("File descriptor is not in read-write mode or write-only mode fd:%d flag:%x", fd, flagsCheck);
        return ERR_INVALID_OPERATION;
    }
    return SUCCESS;
}

int32_t Recorder::RecorderClient::SetOutputFile(int32_t fd)
{
    if (fd < 0 || IsValidFileFd(fd) != SUCCESS) {
        MEDIA_ERR_LOG("Fail to get File Status Flags from fd: %d", fd);
        return ERR_INVALID_PARAM;
    }

    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 1);
    WriteFileDescriptor(&io, fd);
    CallBackPara para = {.funcId = REC_FUNC_SET_OUTPUTFILE, .ret = MEDIA_IPC_FAILED};
    int32_t ret = proxy_->Invoke(proxy_, REC_FUNC_SET_OUTPUTFILE, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetOutputFile failed, ret=%d", ret);
    }

    return para.ret;
}

int32_t Recorder::RecorderClient::SetNextOutputFile(int32_t fd)
{
    if (fd < 0 || IsValidFileFd(fd) != SUCCESS) {
        MEDIA_ERR_LOG("Fail to get File Status Flags from fd: %d", fd);
        return ERR_INVALID_PARAM;
    }

    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 1);
    WriteFileDescriptor(&io, fd);
    CallBackPara para = {.funcId = REC_FUNC_SET_NEXTOUTPUTFILE, .ret = MEDIA_IPC_FAILED};
    int32_t ret = proxy_->Invoke(proxy_, REC_FUNC_SET_NEXTOUTPUTFILE, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetNextOutputFile failed, ret=%d", ret);
    }

    return para.ret;
}

int32_t Recorder::RecorderClient::SetMaxFileSize(int64_t size)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    WriteInt64(&io, size);
    CallBackPara para = {.funcId = REC_FUNC_SET_MAXFILESIZE, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_SET_MAXFILESIZE, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%u", ret);
    }
    return para.ret;
}

int32_t Recorder::RecorderClient::SetRecorderCallback(const std::shared_ptr<RecorderCallback> &callback)
{
    if (callback == nullptr || callback.get() == nullptr) {
        MEDIA_ERR_LOG("SetRecorderCallback callback is nullptr");
        return ERR_INVALID_PARAM;
    }

    callback_ = callback;
    objectStub_.func = RecorderCallbackSvc;
    objectStub_.args = callback_.get();
    objectStub_.isRemote = false;
    sid_.handle = IPC_INVALID_HANDLE;
    sid_.token = SERVICE_TYPE_ANONYMOUS;
    sid_.cookie = reinterpret_cast<uintptr_t>(&objectStub_);
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 1);
    bool writeRemote = WriteRemoteObject(&io, &sid_);
    if (!writeRemote) {
        return -1;
    }
    CallBackPara para = {.funcId = REC_FUNC_SET_RECORDERCALLBACK, .ret = MEDIA_IPC_FAILED};
    int32_t ret = proxy_->Invoke(proxy_, REC_FUNC_SET_RECORDERCALLBACK, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetRecorderCallback failed, ret=%d", ret);
        return -1;
    }
    if (para.ret != 0) {
        callback_ = NULL;
    }

    return para.ret;
}

int32_t Recorder::RecorderClient::Prepare()
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    CallBackPara para = {.funcId = REC_FUNC_PREPARE, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_PREPARE, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%u", ret);
    }
    return para.ret;
}

int32_t Recorder::RecorderClient::Start()
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    CallBackPara para = {.funcId = REC_FUNC_START, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_START, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%u", ret);
    }
    return para.ret;
}

int32_t Recorder::RecorderClient::Pause()
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    CallBackPara para = {.funcId = REC_FUNC_PAUSE, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_PAUSE, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%u", ret);
    }
    return para.ret;
}

int32_t Recorder::RecorderClient::Resume()
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    CallBackPara para = {.funcId = REC_FUNC_RESUME, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_RESUME, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%u", ret);
    }
    return para.ret;
}

int32_t Recorder::RecorderClient::Stop(bool block)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    WriteBool(&io, block);
    CallBackPara para = {.funcId = REC_FUNC_STOP, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_STOP, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%u", ret);
    }
    return para.ret;
}

int32_t Recorder::RecorderClient::Reset()
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    CallBackPara para = {.funcId = REC_FUNC_RESET, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_RESET, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%u", ret);
    }
    return para.ret;
}

int32_t Recorder::RecorderClient::Release()
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    CallBackPara para = {.funcId = REC_FUNC_RELEASE, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_RELEASE, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%u", ret);
    }
    return para.ret;
}

int32_t Recorder::RecorderClient::SetFileSplitDuration(FileSplitType type, int64_t timestamp, uint32_t duration)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    WriteRawData(&io, &type, sizeof(type));
    WriteInt64(&io, timestamp);
    WriteUint32(&io, duration);
    CallBackPara para = {.funcId = REC_FUNC_SET_FILESPLITDURATION, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_SET_FILESPLITDURATION, &io, &para, SimpleCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetSource failed, ret=%u", ret);
    }
    return para.ret;
}

int32_t Recorder::RecorderClient::SetParameter(int32_t sourceId, const Format &format)
{
    return -1;
}

int32_t Recorder::RecorderClient::SetDataSource(DataSourceType source, int32_t &sourceId)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    WriteRawData(&io, &source, sizeof(source));
    CallBackPara para = {.funcId = REC_FUNC_SET_DATASOURCE, .ret = MEDIA_IPC_FAILED};
    uint32_t ret = proxy_->Invoke(proxy_, REC_FUNC_SET_DATASOURCE, &io, &para, SetSourceCallback);
    if (ret != 0) {
        MEDIA_ERR_LOG("SetDataSource failed, ret=%u", ret);
    }
    if (para.ret == 0) {
        sourceId = para.data;
    };
    return para.ret;
}
} /* namespace Media */
} /* namespace OHOS */