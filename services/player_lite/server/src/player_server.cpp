/*
 * Copyright (c) 2020 Huawei Device Co., Ltd.
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

#include "player_server.h"
#include "media_info.h"
#include "media_log.h"
#include "format.h"
#include "player_type.h"
#include "source.h"
#include "surface.h"
#include "surface_impl.h"
#include "player_factory.h"
#include "rpc_errno.h"
extern "C"
{
#include "codec_interface.h"
#include "pthread.h"
#include <sys/prctl.h>
#include "securec.h"
}

#define READ_LEN  (1024)

typedef struct TagIdleBuffer {
    size_t idx;
    size_t offset;
    size_t size;
} IdleBuffer;

namespace OHOS {
namespace Media {
void PlayerServer::PlayerServerRequestHandle(int funcId, void *origin, IpcIo *req, IpcIo *reply)
{
    switch (funcId) {
        case PLAYER_SERVER_SET_SOURCE:
            PlayerServer::GetInstance()->SetSource(req, reply);
            break;
        case PLAYER_SERVER_PREPARE:
            PlayerServer::GetInstance()->Prepare(req, reply);
            break;
        case PLAYER_SERVER_PLAY:
            PlayerServer::GetInstance()->Play(req, reply);
            break;
        case PLAYER_SERVER_IS_PLAYING:
            PlayerServer::GetInstance()->IsPlaying(req, reply);
            break;
        case PLAYER_SERVER_PAUSE:
            PlayerServer::GetInstance()->Pause(req, reply);
            break;
        case PLAYER_SERVER_STOP:
            PlayerServer::GetInstance()->Stop(req, reply);
            break;
        case PLAYER_SERVER_REWIND:
            PlayerServer::GetInstance()->Rewind(req, reply);
            break;
        case PLAYER_SERVER_SET_VOLUME:
            PlayerServer::GetInstance()->SetVolume(req, reply);
            break;
        case PLAYER_SERVER_SET_VIDEO_SURFACE:
            PlayerServer::GetInstance()->SetSurface(req, reply);
            break;
        case PLAYER_SERVER_ENABLE_SINGLE_LOOPING:
            PlayerServer::GetInstance()->SetLoop(req, reply);
            break;
        case PLAYER_SERVER_IS_SINGLE_LOOPING:
            PlayerServer::GetInstance()->IsSingleLooping(req, reply);
            break;
        case PLAYER_SERVER_GET_CURRENT_TIME:
            PlayerServer::GetInstance()->GetCurrentPosition(req, reply);
            break;
        case PLAYER_SERVER_GET_DURATION:
            PlayerServer::GetInstance()->GetDuration(req, reply);
            break;
        case PLAYER_SERVER_GET_VIDEO_WIDTH:
            PlayerServer::GetInstance()->GetVideoWidth(req, reply);
            break;
        case PLAYER_SERVER_GET_VIDEO_HEIGHT:
            PlayerServer::GetInstance()->GetVideoHeight(req, reply);
            break;
        case PLAYER_SERVER_RESET:
            PlayerServer::GetInstance()->Reset(req, reply);
            break;
        case PLAYER_SERVER_RELEASE:
            PlayerServer::GetInstance()->Release(req, reply);
            break;
        case PLAYER_SERVER_SET_PLAYER_CALLBACK:
            PlayerServer::GetInstance()->SetPlayerCallback(req, reply);
            break;
        case PLAYER_SERVER_GET_STATE:
            PlayerServer::GetInstance()->GetPlayerState(req, reply);
            break;
        case PLAYER_SERVER_SET_SPEED:
            PlayerServer::GetInstance()->SetPlaybackSpeed(req, reply);
            break;
        case PLAYER_SERVER_GET_SPEED:
            PlayerServer::GetInstance()->GetPlaybackSpeed(req, reply);
            break;
        case PLAYER_SERVER_SET_PARAMETER:
            PlayerServer::GetInstance()->SetParameter(req, reply);
            break;
        case PLAYER_SERVER_SET_AUDIO_STREAM_TYPE:
            PlayerServer::GetInstance()->SetAudioStreamType(req, reply);
            break;
        case PLAYER_SERVER_GET_AUDIO_STREAM_TYPE:
            PlayerServer::GetInstance()->GetAudioStreamType(req, reply);
            break;
        default:
            MEDIA_ERR_LOG("code not support: %d", funcId);
            break;
    }
}

int32_t PlayerServer::PlayerServerInit()
{
    return 0;
}

class ServerStreamSource : public StreamSource {
public:
    ServerStreamSource(void);
    virtual ~ServerStreamSource(void);
    void OnBufferAvailable(size_t index, size_t offset, size_t size);
    void SetStreamCallback(const std::shared_ptr<StreamCallback> &callback);
    uint8_t *GetBufferAddress(size_t idx);
    void QueueBuffer(size_t index, size_t offset, size_t size, int64_t timestampUs, uint32_t flags);
    int GetAvailableBuffer(IdleBuffer* buffer);
    bool threadRuning;

private:
    std::weak_ptr<StreamCallback> m_callBack;
    std::vector<IdleBuffer> availableBuffer;
    pthread_mutex_t m_mutex;
};

struct StreamThreadControl {
    pthread_t process;
    pthread_mutex_t mutex;
};
StreamThreadControl g_streamThreadControl;

ServerStreamSource::ServerStreamSource(void)
{
    availableBuffer.clear();
    pthread_mutex_init(&m_mutex, nullptr);
}

ServerStreamSource::~ServerStreamSource(void)
{
    availableBuffer.clear();
    pthread_mutex_destroy(&m_mutex);
}

void ServerStreamSource::SetStreamCallback(const std::shared_ptr<StreamCallback> &callback)
{
    m_callBack = callback;
}

uint8_t *ServerStreamSource::GetBufferAddress(size_t idx)
{
    std::shared_ptr<StreamCallback> callback = m_callBack.lock();
    if (callback == nullptr) {
        return nullptr;
    }
    return callback->GetBuffer(idx);
}

void ServerStreamSource::QueueBuffer(size_t index, size_t offset, size_t size, int64_t timestampUs, uint32_t flags)
{
    std::shared_ptr<StreamCallback> callback = m_callBack.lock();
    if (callback == nullptr) {
        return;
    }
    callback->QueueBuffer(index, offset, size, timestampUs, flags);
}

void ServerStreamSource::OnBufferAvailable(size_t index, size_t offset, size_t size)
{
    IdleBuffer buffer;
    pthread_mutex_lock(&m_mutex);
    buffer.idx = index;
    buffer.offset = offset;
    buffer.size = size;
    availableBuffer.push_back(buffer);
    pthread_mutex_unlock(&m_mutex);
}

int ServerStreamSource::GetAvailableBuffer(IdleBuffer* buffer)
{
    pthread_mutex_lock(&m_mutex);
    if (availableBuffer.empty()) {
        pthread_mutex_unlock(&m_mutex);
        return -1;
    }
    *buffer = availableBuffer[0];
    availableBuffer.erase(availableBuffer.begin());
    pthread_mutex_unlock(&m_mutex);
    return 0;
}

static void* streamProcess(void* arg)
{
    IdleBuffer buffer;
    int ret;
    uint8_t *data = nullptr;
    int32_t readLen;
    ServerStreamSource *stream = (ServerStreamSource *)arg;
    SurfaceBuffer* acquireBuffer = nullptr;
    prctl(PR_SET_NAME, "StreamProc_server", 0, 0, 0);
    MEDIA_INFO_LOG("[%s %d]", __func__, __LINE__);
    int sleepTime = 20000;
    while (true) {
        pthread_mutex_lock(&g_streamThreadControl.mutex);
        if (!(stream->threadRuning)) {
            pthread_mutex_unlock(&g_streamThreadControl.mutex);
            break;
        }
        pthread_mutex_unlock(&g_streamThreadControl.mutex);
        ret = stream->GetAvailableBuffer(&buffer);
        if (ret != 0) {
            usleep(sleepTime);
            continue;
        }
        data = stream->GetBufferAddress(buffer.idx);
        if (data == nullptr) {
            break;
        }
        while (true) {
            pthread_mutex_lock(&g_streamThreadControl.mutex);
            if (!(stream->threadRuning)) {
                pthread_mutex_unlock(&g_streamThreadControl.mutex);
                break;
            }
            pthread_mutex_unlock(&g_streamThreadControl.mutex);
            acquireBuffer = stream->GetSurface()->AcquireBuffer();
            if (acquireBuffer != nullptr) {
                ret = acquireBuffer->GetInt32(0, readLen);
                if (ret != 0) {
                    MEDIA_ERR_LOG("[%s,%d] acquireBuffer GetInt32 failed", __func__, __LINE__);
                    readLen = 0;
                }
                if (readLen != 0) {
                    void* acquireBufVirAddr = acquireBuffer->GetVirAddr();
                    if (acquireBufVirAddr != nullptr) {
                        if (buffer.size < static_cast<uint32_t>(readLen)) {
                            MEDIA_ERR_LOG("[%s,%d] error:buffer.size < readLen", __func__, __LINE__);
                        }
                        if (memcpy_s(data + buffer.offset, buffer.size, acquireBufVirAddr, readLen) != EOK) {
                            MEDIA_ERR_LOG("memcpy_s error!");
                        }
                    } else {
                        MEDIA_ERR_LOG("[%s,%d]", __func__, __LINE__);
                    }
                }
                stream->GetSurface()->ReleaseBuffer(acquireBuffer);
                break;
            } else {
                usleep(sleepTime);
                continue;
            }
        }
        if (readLen > 0) {
            int flags = 8;
            stream->QueueBuffer(buffer.idx, buffer.offset, readLen, 0, flags);
        } else {
            int flags = 4;
            stream->QueueBuffer(buffer.idx, buffer.offset, readLen, 0, flags);
            break;
        }
    }
    return nullptr;
}

int32_t SurfaceRequestHandler(uint32_t code, IpcIo* data, IpcIo* reply, MessageOption option)
{
    Surface* surface =  reinterpret_cast<Surface*>(option.args);
    SurfaceImpl* liteSurface = reinterpret_cast<SurfaceImpl*>(surface);
    liteSurface->DoIpcMsg(code, data, reply, option);
    return 0;
}

void PlayerServer::SetStreamSource(IpcIo *reply)
{
    MEDIA_INFO_LOG("process in");
    stream_ = std::make_shared<ServerStreamSource>();
    Format formats;
    formats.PutStringValue(CODEC_MIME, MIME_AUDIO_AAC);
    Source streamSource(stream_, formats);
    int32_t ret = player_->SetSource(streamSource);
    WriteInt32(reply, ret);
    if (ret == 0) {
        Surface* surface = Surface::CreateSurface();
        surface->SetUsage(BUFFER_CONSUMER_USAGE_HARDWARE);
        surface->SetSize(READ_LEN);
        if (sid_ == nullptr) {
            sid_ = new SvcIdentity();
        }
        objectStub_.func = SurfaceRequestHandler;
        objectStub_.args = surface;
        objectStub_.isRemote = false;
        sid_->handle = IPC_INVALID_HANDLE;
        sid_->token = SERVICE_TYPE_ANONYMOUS;
        sid_->cookie = reinterpret_cast<uintptr_t>(&objectStub_);
        stream_->SetSurface(surface);
        bool writeRemote = WriteRemoteObject(reply, sid_);
        if (!writeRemote) {
            MEDIA_ERR_LOG("WriteRemoteObject failed.");
        }
        pthread_attr_t attr;
        pthread_mutex_init(&g_streamThreadControl.mutex, nullptr);
        ServerStreamSource* serverStream = reinterpret_cast<ServerStreamSource*>(stream_.get());
        serverStream->threadRuning = true;
        pthread_attr_init(&attr);
        pthread_create(&g_streamThreadControl.process, &attr, streamProcess, stream_.get());
    }
}

void PlayerServer::SetSource(IpcIo *req, IpcIo *reply)
{
    MEDIA_INFO_LOG("process in");
    int32_t sourceType;
    ReadInt32(req, &sourceType);
    if (player_ == nullptr) {
        MEDIA_INFO_LOG("player nullptr");
        player_ = PlayerFactory::CreatePlayer();
    }

    int32_t state = 0;
    /* only support set source at state idle */
    if (player_ == nullptr || player_->GetPlayerState(state) != 0 || state != static_cast<int32_t> (PLAYER_IDLE)) {
        WriteInt32(reply, -1);
        return;
    }
    switch ((SourceType)sourceType) {
        case SourceType::SOURCE_TYPE_URI: {
            size_t size;
            char* str = (char*)ReadString(req, &size);
            if (str != nullptr) {
                std::string uri(str);
                Source sourceUri(uri);
                WriteInt32(reply, player_->SetSource(sourceUri));
            }
            break;
        }
        case SourceType::SOURCE_TYPE_FD:
            MEDIA_ERR_LOG("unsupported now: SOURCE_TYPE_FD");
            WriteInt32(reply, -1);
            break;
        case SourceType::SOURCE_TYPE_STREAM: {
            SetStreamSource(reply);
            break;
        }
        default:
            break;
    }
    MEDIA_INFO_LOG("PlayerServer::SetSource out");
}

void PlayerServer::Prepare(IpcIo *req, IpcIo *reply)
{
    MEDIA_INFO_LOG("process in");
    if (player_ != nullptr) {
        WriteInt32(reply, player_->Prepare());
        return;
    }
    WriteInt32(reply, -1);
}

void PlayerServer::Play(IpcIo *req, IpcIo *reply)
{
    MEDIA_INFO_LOG("process in");
    if (player_ != nullptr) {
        WriteInt32(reply, player_->Play());
        return;
    }
    WriteInt32(reply, -1);
}

void PlayerServer::IsPlaying(IpcIo *req, IpcIo *reply)
{
    MEDIA_INFO_LOG("process in");
    if (player_ != nullptr) {
        WriteBool(reply, player_->IsPlaying());
        return;
    }
    WriteBool(reply, false);
}

void PlayerServer::Pause(IpcIo *req, IpcIo *reply)
{
    MEDIA_INFO_LOG("process in");
    if (player_ != nullptr) {
        WriteInt32(reply, player_->Pause());
        return;
    }
    WriteInt32(reply, -1);
}

void PlayerServer::Stop(IpcIo *req, IpcIo *reply)
{
    MEDIA_INFO_LOG("process in");
    if (player_ != nullptr) {
        int32_t ret = player_->Stop();
        if (stream_.get() != nullptr) {
            ServerStreamSource* serverStream = reinterpret_cast<ServerStreamSource*>(stream_.get());
            pthread_mutex_lock(&g_streamThreadControl.mutex);
            serverStream->threadRuning = false;
            pthread_mutex_unlock(&g_streamThreadControl.mutex);
        }
        WriteInt32(reply, ret);
        return;
    }
    WriteInt32(reply, -1);
}

void PlayerServer::Rewind(IpcIo *req, IpcIo *reply)
{
    MEDIA_INFO_LOG("process in");
    int64_t mSecond;
    ReadInt64(req, &mSecond);
    int32_t mode;
    ReadInt32(req, &mode);
    if (player_ != nullptr) {
        WriteInt32(reply, player_->Rewind(mSecond, mode));
        return;
    }
    WriteInt32(reply, -1);
}

void PlayerServer::SetVolume(IpcIo *req, IpcIo *reply)
{
    MEDIA_INFO_LOG("process in");
    float *leftVolume = static_cast<float *>(ReadRawData(req, sizeof(float)));
    float *rightVolume = static_cast<float *>(ReadRawData(req, sizeof(float)));
    if (player_ != nullptr) {
        WriteInt32(reply, player_->SetVolume(*leftVolume, *rightVolume));
        return;
    }
    WriteInt32(reply, -1);
}

void PlayerServer::SetSurface(IpcIo *req, IpcIo *reply)
{
    MEDIA_INFO_LOG("process in");
    size_t size;
    char* str_x = (char*)ReadString(req, &size);
    char* str_y = (char*)ReadString(req, &size);
    char* str_width = (char*)ReadString(req, &size);
    char* str_height = (char*)ReadString(req, &size);
    Surface* surface = Surface::CreateSurface();
    if (surface != nullptr) {
        surface->SetUserData("region_position_x", std::string(str_x));
        surface->SetUserData("region_position_y", std::string(str_y));
        surface->SetUserData("region_width", std::string(str_width));
        surface->SetUserData("region_height", std::string(str_height));
        if (player_ != nullptr) {
            WriteInt32(reply, player_->SetSurface(surface));
            return;
        }
    }
    WriteInt32(reply, -1);
}

void PlayerServer::SetLoop(IpcIo *req, IpcIo *reply)
{
    MEDIA_INFO_LOG("process in");
    bool loop;
    ReadBool(req, &loop);
    if (player_ != nullptr) {
        WriteBool(reply, player_->SetLoop(loop));
        return;
    }
    WriteInt32(reply, -1);
}

void PlayerServer::IsSingleLooping(IpcIo *req, IpcIo *reply)
{
    MEDIA_INFO_LOG("process in");
    if (player_ != nullptr) {
        WriteBool(reply, player_->IsSingleLooping());
        return;
    }
    WriteBool(reply, false);
}

void PlayerServer::GetCurrentPosition(IpcIo *req, IpcIo *reply)
{
    int64_t time = 0;
    if (player_ != nullptr) {
        WriteInt32(reply, player_->GetCurrentPosition(time));
        WriteInt64(reply, time);
        return;
    }
    WriteInt32(reply, -1);
    WriteInt64(reply, time);
}

void PlayerServer::GetDuration(IpcIo *req, IpcIo *reply)
{
    MEDIA_INFO_LOG("process in");
    int64_t duration = 0;
    if (player_ != nullptr) {
        WriteInt32(reply, player_->GetDuration(duration));
        WriteInt64(reply, duration);
        return;
    }
    WriteInt32(reply, -1);
    WriteInt64(reply, duration);
}

void PlayerServer::GetVideoWidth(IpcIo *req, IpcIo *reply)
{
    MEDIA_INFO_LOG("process in");
    int32_t width = 0;
    if (player_ != nullptr) {
        WriteInt32(reply, player_->GetVideoWidth(width));
        WriteInt32(reply, width);
        return;
    }
    WriteInt32(reply, -1);
    WriteInt32(reply, width);
}

void PlayerServer::GetVideoHeight(IpcIo *req, IpcIo *reply)
{
    MEDIA_INFO_LOG("process in");
    int32_t hight = 0;
    if (player_ != nullptr) {
        WriteInt32(reply, player_->GetVideoHeight(hight));
        WriteInt32(reply, hight);
        return;
    }
    WriteInt32(reply, -1);
    WriteInt32(reply, hight);
}

void PlayerServer::Reset(IpcIo *req, IpcIo *reply)
{
    MEDIA_INFO_LOG("process in");
    if (player_ != nullptr) {
        WriteInt32(reply, player_->Reset());
        return;
    }
    WriteInt32(reply, -1);
}

void PlayerServer::Release(IpcIo *req, IpcIo *reply)
{
    MEDIA_INFO_LOG("process in");
    if (player_ != nullptr) {
        int32_t ret = player_->Release();
        if (stream_ != nullptr) {
            ServerStreamSource* serverStream = reinterpret_cast<ServerStreamSource*>(stream_.get());
            pthread_mutex_lock(&g_streamThreadControl.mutex);
            serverStream->threadRuning = false;
            pthread_mutex_unlock(&g_streamThreadControl.mutex);
            pthread_join(g_streamThreadControl.process, nullptr);
            pthread_mutex_destroy(&g_streamThreadControl.mutex);
            stream_.reset();
            stream_ = nullptr;
        }
        if (sid_ != nullptr) {
            delete sid_;
            sid_ = nullptr;
        }
        playerCallback_.reset();
        player_ = nullptr;
        WriteInt32(reply, ret);
        return;
    }
    WriteInt32(reply, -1);
}

void PlayerServer::SetPlayerCallback(IpcIo *req, IpcIo *reply)
{
    MEDIA_INFO_LOG("process in");
    SvcIdentity sid;
    if (ReadRemoteObject(req, &sid)) {
        playerCallback_ = std::make_shared<PalyerCallbackImpl>(sid);
        if (player_ != nullptr) {
            player_->SetPlayerCallback(playerCallback_);
            return;
        }
    }
}

void PlayerServer::GetPlayerState(IpcIo *req, IpcIo *reply)
{
    MEDIA_INFO_LOG("process in");
    int32_t state = 0;
    if (player_ != nullptr) {
        WriteInt32(reply, player_->GetPlayerState(state));
        WriteInt32(reply, state);
        return;
    }
    WriteInt32(reply, -1);
    WriteInt32(reply, state);
}

void PlayerServer::SetPlaybackSpeed(IpcIo *req, IpcIo *reply)
{
    MEDIA_INFO_LOG("process in");
    float speed;
    ReadFloat(req, &speed);
    if (player_ != nullptr) {
        WriteInt32(reply, player_->SetPlaybackSpeed(speed));
        return;
    }
    WriteInt32(reply, -1);
}

void PlayerServer::GetPlaybackSpeed(IpcIo *req, IpcIo *reply)
{
    MEDIA_INFO_LOG("process in");
    float speed = 1.0;
    if (player_ != nullptr) {
        WriteInt32(reply, player_->GetPlaybackSpeed(speed));
        WriteFloat(reply, speed);
        return;
    }
    WriteInt32(reply, -1);
    WriteFloat(reply, 1.0);
}

void PlayerServer::SetParameter(IpcIo *req, IpcIo *reply)
{
    MEDIA_INFO_LOG("process in");
    Format formats;
    if (player_ == nullptr) {
        WriteInt32(reply, -1);
        return;
    }

    int32_t count;
    ReadInt32(req, &count);
    for (int32_t i = 0; i < count; i++) {
        uint32_t size;
        char *key = (char *)ReadString(req, &size);
        FormatDataType type;
        ReadInt32(req, (int32_t *)&type);
        if (type == FORMAT_TYPE_INT32) {
            int32_t value;
            ReadInt32(req, &value);
            formats.PutIntValue(key, value);
        } else if (type == FORMAT_TYPE_INT64) {
            int64_t value;
            ReadInt64(req, &value);
            formats.PutLongValue(key, value);
        } else if (type == FORMAT_TYPE_FLOAT) {
            float value;
            ReadFloat(req, &value);
            formats.PutFloatValue(key, value);
        } else if (type == FORMAT_TYPE_DOUBLE) {
            double value;
            ReadDouble(req, &value);
            formats.PutDoubleValue(key, value);
        } else if (type == FORMAT_TYPE_STRING) {
            char *value = (char *)ReadString(req, &size);
            formats.PutStringValue(key, value);
        } else {
            MEDIA_ERR_LOG("SetParameter failed, type:%d\n", type);
            WriteInt32(reply, -1);
            return;
        }
    }

    WriteInt32(reply, player_->SetParameter(formats));
}

void PlayerServer::SetAudioStreamType(IpcIo *req, IpcIo *reply)
{
    MEDIA_INFO_LOG("process in");
    int32_t type;
    ReadInt32(req, &type);
    if (player_ != nullptr) {
        WriteInt32(reply, player_->SetAudioStreamType(type));
        return;
    }
    WriteInt32(reply, -1);
}

void PlayerServer::GetAudioStreamType(IpcIo *req, IpcIo *reply)
{
    MEDIA_INFO_LOG("process in");
    int32_t type = TYPE_MEDIA;
    if (player_ != nullptr) {
        player_->GetAudioStreamType(type);
        WriteInt32(reply, 0);
        WriteFloat(reply, type);
        return;
    }
    WriteInt32(reply, -1);
    WriteFloat(reply, type);
}

void PalyerCallbackImpl::OnPlaybackComplete()
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    MessageOption option;
    MessageOptionInit(&option);
    option.flags = TF_OP_ASYNC;
    int32_t ret = SendRequest(sid_, ON_PLAYBACK_COMPLETE, &io, nullptr, option, nullptr);
    if (ret != ERR_NONE) {
        MEDIA_ERR_LOG("PalyerCallbackImpl::OnPlaybackComplete failed\n");
    }
}

void PalyerCallbackImpl::OnError(int32_t errorType, int32_t errorCode)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    WriteInt32(&io, errorType);
    WriteInt32(&io, errorCode);
    MessageOption option;
    MessageOptionInit(&option);
    option.flags = TF_OP_ASYNC;
    int32_t ret = SendRequest(sid_, ON_ERROR, &io, nullptr, option, nullptr);
    if (ret != ERR_NONE) {
        MEDIA_ERR_LOG("PalyerCallbackImpl::OnError failed\n");
    }
}

void PalyerCallbackImpl::OnInfo(int type, int extra)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    WriteInt32(&io, (int32_t)type);
    WriteInt32(&io, (int32_t)extra);
    MessageOption option;
    MessageOptionInit(&option);
    option.flags = TF_OP_ASYNC;
    int32_t ret = SendRequest(sid_, ON_INFO, &io, nullptr, option, nullptr);
    if (ret != ERR_NONE) {
        MEDIA_ERR_LOG("PalyerCallbackImpl::OnInfo failed\n");
    }
}

void PalyerCallbackImpl::OnVideoSizeChanged(int width, int height)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    WriteInt32(&io, (int32_t)width);
    WriteInt32(&io, (int32_t)height);
    MessageOption option;
    MessageOptionInit(&option);
    option.flags = TF_OP_ASYNC;
    int32_t ret = SendRequest(sid_, ON_INFO, &io, nullptr, option, nullptr);
    if (ret != ERR_NONE) {
        MEDIA_ERR_LOG("PalyerCallbackImpl::OnInfo failed\n");
    }
}

void PalyerCallbackImpl::OnRewindToComplete()
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    MessageOption option;
    MessageOptionInit(&option);
    option.flags = TF_OP_ASYNC;
    int32_t ret = SendRequest(sid_, ON_REWIND_TO_COMPLETE, &io, nullptr, option, nullptr);
    if (ret != ERR_NONE) {
        MEDIA_ERR_LOG("PalyerCallbackImpl::OnRewindToComplete failed\n");
    }
}
}
}
