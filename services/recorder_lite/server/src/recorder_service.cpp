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
#include "recorder_service.h"
#include "media_log.h"
#include "surface_impl.h"
#include "rpc_errno.h"

namespace OHOS {
namespace Media {
RecorderClientMng *RecorderClientMng::GetInstance()
{
    static RecorderClientMng mng;
    return &mng;
}

RecorderImpl *RecorderClientMng::GetRecorder(pid_t pid)
{
    return (pid == client_) ? rec_ : nullptr;
}

bool RecorderClientMng::AcceptClient(pid_t pid)
{
    if (client_ == -1) {
        rec_ = new RecorderImpl;
        client_ = pid;
        return true;
    }
    return false;
}

void RecorderClientMng::DropClient(pid_t pid)
{
    if (pid == client_) {
        delete rec_;
        rec_ = nullptr;
        client_ = -1;
    }
}

void RecorderClientMng::Dispatch(int32_t funcId, pid_t pid, IpcIo *req, IpcIo *reply)
{
    if (funcId == REC_FUNC_CONNECT) {
        AcceptClient(pid);
        return;
    }
    auto recorder = GetRecorder(pid);
    if (recorder == nullptr) {
        MEDIA_ERR_LOG("Cannot find client object.(pid=%d)", pid);
        WriteInt32(reply, MEDIA_IPC_FAILED);
        return;
    }
    switch (funcId) {
        case REC_FUNC_DISCONNECT:
            DropClient(pid);
            break;
        case REC_FUNC_SET_VIDEOSOURCE: {
            VideoSourceType *src = (VideoSourceType*)ReadRawData(req, sizeof(VideoSourceType));
            int32_t srcId;
            int32_t ret = recorder->SetVideoSource(*src, srcId);
            WriteInt32(reply, ret);
            WriteInt32(reply, srcId);
            break;
        }
        case REC_FUNC_SET_VIDEOENCODER: {
            int32_t src;
            ReadInt32(req, &src);
            VideoCodecFormat *enc = (VideoCodecFormat*)ReadRawData(req, sizeof(VideoCodecFormat));
            int32_t ret = recorder->SetVideoEncoder(src, *enc);
            WriteInt32(reply, ret);
            break;
        }
        case REC_FUNC_SET_VIDEOSIZE: {
            int32_t src;
            ReadInt32(req, &src);
            int32_t width;
            ReadInt32(req, &width);
            int32_t height;
            ReadInt32(req, &height);
            int32_t ret = recorder->SetVideoSize(src, width, height);
            WriteInt32(reply, ret);
            break;
        }
        case REC_FUNC_SET_VIDEOFRAMERATE: {
            int32_t sourceId;
            ReadInt32(req, &sourceId);
            int32_t frameRate;
            ReadInt32(req, &frameRate);
            int32_t ret = recorder->SetVideoFrameRate(sourceId, frameRate);
            WriteInt32(reply, ret);
            break;
        }
        case REC_FUNC_SET_VIDEOENCODINGBITRATE: {
            int32_t sourceId;
            ReadInt32(req, &sourceId);
            int32_t rate;
            ReadInt32(req, &rate);
            int32_t ret = recorder->SetVideoEncodingBitRate(sourceId, rate);
            WriteInt32(reply, ret);
            break;
        }
        case REC_FUNC_SET_CAPTURERATE: {
            int32_t sourceId;
            ReadInt32(req, &sourceId);
            double *fps = (double*)ReadRawData(req, sizeof(double));
            int32_t ret = recorder->SetCaptureRate(sourceId, *fps);
            WriteInt32(reply, ret);
            break;
        }
        case REC_FUNC_GET_SURFACE: {
            int32_t sourceId;
            ReadInt32(req, &sourceId);
            std::shared_ptr<OHOS::Surface> surface = recorder->GetSurface(sourceId);
            if (surface != nullptr) {
                dynamic_cast<OHOS::SurfaceImpl *>(surface.get())->WriteIoIpcIo(*reply);
            }
            break;
        }
        case REC_FUNC_SET_AUDIOSOURCE: {
            AudioSourceType *src = (AudioSourceType *)ReadRawData(req, sizeof(AudioSourceType));
            int32_t srcId;
            int32_t ret = recorder->SetAudioSource(*src, srcId);
            WriteInt32(reply, ret);
            WriteInt32(reply, srcId);
            break;
        }
        case REC_FUNC_SET_AUDIOENCODER: {
            int32_t sourceId;
            ReadInt32(req, &sourceId);
            AudioCodecFormat *enc = (AudioCodecFormat *)ReadRawData(req, sizeof(AudioCodecFormat));
            int32_t ret = recorder->SetAudioEncoder(sourceId, *enc);
            WriteInt32(reply, ret);
            break;
        }
        case REC_FUNC_SET_AUDIOSAMPLERATE: {
            int32_t sourceId;
            ReadInt32(req, &sourceId);
            int32_t rate;
            ReadInt32(req, &rate);
            int32_t ret = recorder->SetAudioSampleRate(sourceId, rate);
            WriteInt32(reply, ret);
            break;
        }
        case REC_FUNC_SET_AUDIOCHANNELS: {
            int32_t sourceId;
            ReadInt32(req, &sourceId);
            int32_t num;
            ReadInt32(req, &num);
            int32_t ret = recorder->SetAudioChannels(sourceId, num);
            WriteInt32(reply, ret);
            break;
        }
        case REC_FUNC_SET_AUDIOENCODINGBITRATE: {
            int32_t sourceId;
            ReadInt32(req, &sourceId);
            int32_t bitRate;
            ReadInt32(req, &bitRate);
            int32_t ret = recorder->SetAudioEncodingBitRate(sourceId, bitRate);
            WriteInt32(reply, ret);
            break;
        }
        case REC_FUNC_SET_MAXDURATION: {
            int32_t duration;
            ReadInt32(req, &duration);
            int32_t ret = recorder->SetMaxDuration(duration);
            WriteInt32(reply, ret);
            break;
        }
        case REC_FUNC_SET_OUTPUTFORMAT: {
            OutputFormatType *format = (OutputFormatType *)ReadRawData(req, sizeof(OutputFormatType));
            int32_t ret = recorder->SetOutputFormat(*format);
            WriteInt32(reply, ret);
            break;
        }
        case REC_FUNC_SET_OUTPUTPATH: {
            size_t strSize;
            char *path = (char *)ReadString(req, &strSize);
            int32_t ret = recorder->SetOutputPath(string(path));
            WriteInt32(reply, ret);
            break;
        }
        case REC_FUNC_SET_OUTPUTFILE: {
            int32_t fd = ReadFileDescriptor(req);
            int32_t ret = recorder->SetOutputFile(fd);
            WriteInt32(reply, ret);
            break;
        }
        case REC_FUNC_SET_NEXTOUTPUTFILE: {
            int32_t fd = ReadFileDescriptor(req);
            int32_t ret = recorder->SetNextOutputFile(fd);
            WriteInt32(reply, ret);
            break;
        }
        case REC_FUNC_SET_MAXFILESIZE: {
            int64_t size;
            ReadInt64(req, &size);
            int32_t ret = recorder->SetMaxFileSize(size);
            WriteInt32(reply, ret);
            break;
        }
        case REC_FUNC_SET_RECORDERCALLBACK: {
            SvcIdentity svc;
            ReadRemoteObject(req, &svc);
            std::shared_ptr<RecorderCallbackClient> client(new RecorderCallbackClient(&svc));
            int32_t ret = recorder->SetRecorderCallback(client);
            WriteInt32(reply, ret);
            break;
        }
        case REC_FUNC_PREPARE: {
            int32_t ret = recorder->Prepare();
            WriteInt32(reply, ret);
            break;
        }
        case REC_FUNC_START: {
            int32_t ret = recorder->Start();
            WriteInt32(reply, ret);
            break;
        }
        case REC_FUNC_PAUSE: {
            int32_t ret = recorder->Pause();
            WriteInt32(reply, ret);
            break;
        }
        case REC_FUNC_RESUME: {
            int32_t ret = recorder->Resume();
            WriteInt32(reply, ret);
            break;
        }
        case REC_FUNC_STOP: {
            bool block;
            ReadBool(req, &block);
            int32_t ret = recorder->Stop(block);
            WriteInt32(reply, ret);
            break;
        }
        case REC_FUNC_RESET: {
            int32_t ret = recorder->Reset();
            WriteInt32(reply, ret);
            break;
        }
        case REC_FUNC_RELEASE: {
            int32_t ret = recorder->Release();
            WriteInt32(reply, ret);
            break;
        }
        case REC_FUNC_SET_FILESPLITDURATION: {
            FileSplitType *type = (FileSplitType *)ReadRawData(req, sizeof(FileSplitType));
            int64_t timestamp;
            ReadInt64(req, &timestamp);
            uint32_t duration;
            ReadUint32(req, &duration);
            int32_t ret = recorder->SetFileSplitDuration(*type, timestamp, duration);
            WriteInt32(reply, ret);
            break;
        }
        case REC_FUNC_SET_PARAMETER: {
            break;
        }
        case REC_FUNC_SET_DATASOURCE: {
            DataSourceType *src = (DataSourceType *)ReadRawData(req, sizeof(DataSourceType));
            int32_t srcId;
            int32_t ret = recorder->SetDataSource(*src, srcId);
            WriteInt32(reply, ret);
            WriteInt32(reply, srcId);
            break;
        }
        default:
            break;
    }
}

void RecorderCallbackClient::OnError(int32_t errorType, int32_t errorCode)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    WriteInt32(&io, errorType);
    WriteInt32(&io, errorCode);
    MessageOption option;
    MessageOptionInit(&option);
    option.flags = TF_OP_ASYNC;
    int32_t ret = SendRequest(sid_, REC_ANONYMOUS_FUNC_ON_ERROR, &io, nullptr, option, nullptr);
    if (ret != ERR_NONE) {
        MEDIA_ERR_LOG("Recorder server SendRequest OnError failed.(ret=%d)", ret);
    }
}

void RecorderCallbackClient::OnInfo(int32_t type, int32_t extra)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    WriteInt32(&io, type);
    WriteInt32(&io, extra);
    MessageOption option;
    MessageOptionInit(&option);
    option.flags = TF_OP_ASYNC;
    int32_t ret = SendRequest(sid_, REC_ANONYMOUS_FUNC_ON_INFO, &io, nullptr, option, nullptr);
    if (ret != ERR_NONE) {
        MEDIA_ERR_LOG("Recorder server SendRequest OnInfo failed.(ret=%d)", ret);
    }
}
} // namespace Media
} // namespace OHOS