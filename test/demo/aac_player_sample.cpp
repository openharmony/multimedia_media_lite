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

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <csignal>
#include <unistd.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/prctl.h>
#include <iostream>
#include <string>
#include <map>
#include <functional>
#include <climits>
#include "securec.h"
#include "source.h"
#include "player.h"
#include "format.h"
#include "components/ui_image_view.h"
#include "components/root_view.h"
#include "components/ui_label.h"
#include "components/ui_slider.h"
#include "components/ui_surface_view.h"
#include "components/ui_toggle_button.h"
#include "window/window.h"
#include "common/graphic_startup.h"

using OHOS::Surface;

using OHOS::Media::Player;
using OHOS::Media::PlayerSeekMode;
using OHOS::Media::Source;
using OHOS::Media::Format;
using OHOS::Media::StreamSource;
using OHOS::Media::StreamCallback;
using namespace std;
using namespace OHOS;
using namespace OHOS::Media;

struct TestSample {
    std::shared_ptr<StreamSource> streamSample;
    std::shared_ptr<Player> adapter;
    std::string filepath;
    pthread_t streamThreadId;
};

volatile sig_atomic_t g_terminate = 0;

static void SignalHandler(int signum)
{
    printf("[%s, %d], recv signum:%d\n", __func__, __LINE__, signum);
    switch (signum) {
        case SIGINT:  // Ctrl+C
            printf("Ctrl+C pressed, terminating program...\n");
            g_terminate = 1;
            break;
        case SIGTSTP: // Ctrl+Z
            printf("Ctrl+Z pressed, terminating program...\n");
            g_terminate = 1;
            break;
        default:
            break;
    }
}

void* StreamProcessThreadFunc(void* arg)
{
    TestSample* sample = static_cast<TestSample*>(arg);
    size_t bufferSize;
    uint8_t *data = nullptr;
    size_t readLen;
    const int32_t streamProcessSleepTimeUs = 20000;
    FILE* pFile = fopen(sample->filepath.c_str(), "rb");
    if (pFile == nullptr) {
        printf("Failed to open file: %s\n", sample->filepath.c_str());
        return nullptr;
    }
    printf("Stream processing thread started, processing file: %s\n", sample->filepath.c_str());
    bool isEOS = false;
    while (!isEOS) {
        if (g_terminate) {
            printf("Stream processing thread terminating due to signal\n");
            isEOS = true;
        }
        data = sample->streamSample->GetSharedBuffer(bufferSize);
        if (data == nullptr) {
            printf("GetSharedBuffer failed, bufferSize = %zu\n", bufferSize);
            usleep(streamProcessSleepTimeUs);
            continue;
        }

        printf("GetSharedBuffer success, bufferSize = %zu\n", bufferSize);
        readLen = fread(data, 1, bufferSize, pFile);
        if (readLen > 0) {
            sample->streamSample->QueueSharedBuffer(data, readLen);
        } else {
            sample->streamSample->QueueSharedBuffer(data, 0);
            isEOS = true;
        }
    }
    if (pFile != nullptr) {
        fclose(pFile);
        printf("Stream processing thread finished, file closed\n");
    }
    return nullptr;
}


void Play(TestSample &testSample)
{
    if (testSample.adapter == nullptr) {
        printf("Player adapter is null, please check.\n");
        return;
    }
    printf("Play\n");
    testSample.adapter->Play();
}

void Pause(TestSample &testSample)
{
    if (testSample.adapter == nullptr) {
        printf("Player adapter is null, please check.\n");
        return;
    }
    printf("Pause\n");
    testSample.adapter->Pause();
}

void Stop(TestSample &testSample)
{
    if (testSample.adapter == nullptr) {
        printf("Player adapter is null, please check.\n");
        return;
    }
    printf("Stop\n");
    testSample.adapter->Stop();
    testSample.adapter->Release();
    testSample.adapter.reset();
    testSample.streamSample.reset();
    testSample.adapter = std::make_shared<Player>();
    testSample.streamSample = std::make_shared<StreamSource>();

    int threadRet = pthread_create(&testSample.streamThreadId, nullptr, StreamProcessThreadFunc, &testSample);
    if (threadRet != 0) {
        printf("Failed to create stream processing thread, error: %d\n", threadRet);
    }
    Format formats;
    formats.PutStringValue(CODEC_MIME, MIME_AUDIO_AAC);
    Source source(testSample.streamSample, formats);
    testSample.adapter->SetSource(source);
    testSample.adapter->Prepare();
}

void SetVolume(TestSample &testSample)
{
    float lvolume;
    float rvolume;
    printf("please input volume(0-100): ");
    cin >> lvolume >> rvolume;
    printf("try set volume %f %f\n", lvolume, rvolume);
    int32_t playerState = 0;
    testSample.adapter->GetPlayerState(playerState);
    if (playerState != PlayerStates::PLAYER_STARTED) {
        if (playerState == PlayerStates::PLAYER_PAUSED) {
            Play(testSample);
        } else {
            Stop(testSample);
            Play(testSample);
        }
    }
    int32_t ret = testSample.adapter->SetVolume(lvolume, rvolume);
    if (ret != 0) {
        printf("[%s: %d] ret:%d\n", __func__, __LINE__, ret);
    }
}

void Replay(TestSample &testSample)
{
    if (testSample.adapter == nullptr) {
        printf("Player adapter is null, please check.\n");
        return;
    }
    printf("Replay\n");
    Stop(testSample);
    Play(testSample);
}

std::map<std::string, std::function<void(TestSample&)>> g_mediaTypeMap = {
    {"play", Play},
    {"pause", Pause},
    {"stop", Stop},
    {"replay", Replay},
    {"volume", SetVolume},
    {"quit", [](TestSample&) {
        printf("quit\n");
        g_terminate = true;
    }}
};

static void OperationLoop(TestSample& testSample)
{
    fd_set readfds;
    struct timeval tv;
    string input1;
    while (!g_terminate) {
        cout << "**********************"<< endl;
        cout << "Please select operation:" << endl;
        cout << "play" << endl;
        cout << "pause" << endl;
        cout << "stop" << endl;
        cout << "replay" << endl;
        cout << "volume" << endl;
        cout << "quit" << endl;
        cout << "**********************"<< endl;
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        int timeout = 10;
        tv.tv_sec = timeout;
        tv.tv_usec = 0;
        int ret = select(STDIN_FILENO + 1, &readfds, nullptr, nullptr, &tv);
        if (ret == -1 && errno != EINTR) {
            perror("select");
            break;
        }
        if (ret > 0 && FD_ISSET(STDIN_FILENO, &readfds)) {
            cin >> input1;
            if (g_mediaTypeMap.find(input1) != g_mediaTypeMap.end()) {
                g_mediaTypeMap[input1](testSample);
                int32_t playerState = 0;
                testSample.adapter->GetPlayerState(playerState);
                printf("playerState:%d\n", playerState);
            } else {
                cout << "Invalid input." << endl;
            }
        }
    }
}
int main(int argc, char *argv[])
{
    int maxArgc = 2;
    if (signal(SIGINT, SignalHandler) == SIG_ERR) {
        perror("signal SIGINT");
        return -1;
    }
    if (signal(SIGTSTP, SignalHandler) == SIG_ERR) {
        perror("signal SIGTSTP");
        return -1;
    }
    prctl(PR_SET_NAME, "mainProc", 0, 0, 0);
    if (argc < maxArgc) {
        printf("Usage: %s <filepath>\n", argv[0]);
        return -1;
    }
    std::string filepath = argv[1];
    struct TestSample testSample;
    testSample.adapter = std::make_shared<Player>();
    testSample.filepath = filepath;
    testSample.streamSample = std::make_shared<StreamSource>();

    int threadRet = pthread_create(&testSample.streamThreadId, nullptr, StreamProcessThreadFunc, &testSample);
    if (threadRet != 0) {
        printf("Failed to create stream processing thread, error: %d\n", threadRet);
        return -1;
    }
    Format formats;
    formats.PutStringValue(CODEC_MIME, MIME_AUDIO_AAC);
    Source source(testSample.streamSample, formats);
    testSample.adapter->SetSource(source);
    testSample.adapter->Prepare();
    testSample.adapter->Play();

    OperationLoop(testSample);

    printf("Waiting for stream processing thread to finish...\n");
    pthread_join(testSample.streamThreadId, nullptr);
    printf("Stream processing thread has finished\n");
    testSample.adapter->Stop();
    testSample.adapter->Release();
    testSample.streamSample.reset();

    return 0;
}
