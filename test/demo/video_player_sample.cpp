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
#include "common/task_manager.h"

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
const int ONE_HUNDRED_MICROSECONDS = 100000;

class PlayerCallbackImpl : public PlayerCallback {
public:
    PlayerCallbackImpl();
    ~PlayerCallbackImpl();
    void OnPlaybackComplete();
    void OnError(int32_t errorType, int32_t errorCode);
    void OnInfo(int type, int extra);
    void OnVideoSizeChanged(int width, int height);
    void OnRewindToComplete();
};

struct TestSample {
    std::shared_ptr<Player> adapter;
    std::shared_ptr<PlayerCallbackImpl> cb;
    Surface *surface;
    std::string filepath;
    int64_t totalDuration;
    int64_t currentPosition;
};

PlayerCallbackImpl::PlayerCallbackImpl()
{
    printf("ctor\n");
}

PlayerCallbackImpl::~PlayerCallbackImpl()
{
    printf("dtor\n");
}

void PlayerCallbackImpl::OnPlaybackComplete(void)
{
    printf("OnPlaybackComplete test\n");
}

void PlayerCallbackImpl::OnError(int32_t errorType, int32_t errorCode)
{
    printf("OnError test, errorType:%d, errorCode:%d\n", errorType, errorCode);
}

void PlayerCallbackImpl::OnInfo(int type, int extra)
{
    printf("OnInfo test, type:%d, extra:%d\n", type, extra);
}

void PlayerCallbackImpl::OnVideoSizeChanged(int width, int height)
{
    printf("OnVideoSizeChanged test width:%d, height:%d\n", width, height);
}

void PlayerCallbackImpl::OnRewindToComplete(void)
{
    printf("OnRewindToComplete test\n");
}

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

void VideoPlay(TestSample &testSample)
{
    if (testSample.adapter == nullptr) {
        printf("Player adapter is null, please check.\n");
        return;
    }
    printf("VideoPlay\n");
    testSample.adapter->Play();
}

void VideoPause(TestSample &testSample)
{
    if (testSample.adapter == nullptr) {
        printf("Player adapter is null, please check.\n");
        return;
    }
    printf("VideoPause\n");
    testSample.adapter->Pause();
}

void VideoStop(TestSample &testSample)
{
    if (testSample.adapter == nullptr) {
        printf("Player adapter is null, please check.\n");
        return;
    }
    printf("VideoStop\n");
    testSample.adapter->Stop();
    testSample.adapter->Release();
    testSample.adapter = nullptr;
    testSample.adapter = std::make_shared<Player>();
    std::map<std::string, std::string> header;
    Source source(testSample.filepath, header);
    testSample.adapter->SetSource(source);
    testSample.adapter->Prepare();
    testSample.adapter->SetPlayerCallback(testSample.cb);
    testSample.adapter->SetVideoSurface(testSample.surface);
}

void VideoSeek(TestSample &testSample)
{
    if (testSample.adapter == nullptr) {
        printf("Player adapter is null, please check.\n");
        return;
    }
    int32_t playerState = 0;
    testSample.adapter->GetPlayerState(playerState);
    if (playerState != PlayerStates::PLAYER_PAUSED || playerState != PlayerStates::PLAYER_STARTED) {
        VideoStop(testSample);
    }
    printf("VideoSeek\n");
    testSample.adapter->GetDuration(testSample.totalDuration);
    testSample.adapter->GetCurrentTime(testSample.currentPosition);
    printf("please input seek position(0-%lld): currentPosition:%lld\n", \
        testSample.totalDuration, testSample.currentPosition);
    int64_t seekPos = 0;
    cin >> seekPos;
    testSample.adapter->Rewind(seekPos, PlayerSeekMode::PLAYER_SEEK_PREVIOUS_SYNC);
    sleep(0x1);
    testSample.adapter->Play();
}

void VideoSpeed(TestSample &testSample)
{
    if (testSample.adapter == nullptr) {
        printf("Player adapter is null, please check.\n");
        return;
    }
    printf("VideoSpeed\n");
    float speed = 1.0f;
    cin >> speed;
    if (speed > 1.1f) {
        speed = 2.0f;
    }
    printf("speed:%f\n", speed);
    int32_t playerState = 0;
    testSample.adapter->GetPlayerState(playerState);
    if (playerState != PlayerStates::PLAYER_STARTED) {
        if (playerState == PlayerStates::PLAYER_PAUSED) {
            VideoPlay(testSample);
        } else {
            VideoStop(testSample);
            VideoPlay(testSample);
        }
    }
    testSample.adapter->SetPlaybackSpeed(speed);
}

void VideSetVolume(TestSample &testSample)
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
            VideoPlay(testSample);
        } else {
            VideoStop(testSample);
            VideoPlay(testSample);
        }
    }
    int32_t ret = testSample.adapter->SetVolume(lvolume, rvolume);
    if (ret != 0) {
        printf("[%s: %d] ret:%d\n", __func__, __LINE__, ret);
    }
}

void VideoReplay(TestSample &testSample)
{
    if (testSample.adapter == nullptr) {
        printf("Player adapter is null, please check.\n");
        return;
    }
    printf("VideoReplay\n");
    VideoStop(testSample);
    VideoPlay(testSample);
}

void VideoShow(TestSample &testSample)
{
    printf("now show player info\n");

    printf("player IsPlaying:%d IsSingleLooping:%d\n", testSample.adapter->IsPlaying(),
        testSample.adapter->IsSingleLooping());

    int64_t duration;
    int32_t ret = testSample.adapter->GetDuration(duration);
    if (ret != 0) {
        printf("[%s: %d] ret:%d\n", __func__, __LINE__, ret);
    }

    int64_t currentPosition;
    ret = testSample.adapter->GetCurrentTime(currentPosition);
    if (ret != 0) {
        printf("[%s: %d] ret:%d\n", __func__, __LINE__, ret);
    }
    printf("player duration:%lld GetCurrentTime:%lld\n", duration, currentPosition);

    int32_t videoWidth;
    int32_t videoHeight;
    ret = testSample.adapter->GetVideoWidth(videoWidth);
    if (ret != 0) {
        printf("[%s: %d] ret:%d\n", __func__, __LINE__, ret);
    }
    ret = testSample.adapter->GetVideoHeight(videoHeight);
    if (ret != 0) {
        printf("[%s: %d] ret:%d\n", __func__, __LINE__, ret);
    }
    printf("player videoWidth:%d videoHeight:%d\n", videoWidth, videoHeight);
    int32_t state;
    ret = testSample.adapter->GetPlayerState(state);
    if (ret != 0) {
        printf("[%s: %d] ret:%d\n", __func__, __LINE__, ret);
    }
    printf("player current state:%d\n", state);
}

void VideoLoop(TestSample &testSample)
{
    int32_t loop = 0;
    printf("try set loop %d\n", loop);
    cin >> loop;
    int32_t playerState = 0;
    testSample.adapter->GetPlayerState(playerState);
    if (playerState != PlayerStates::PLAYER_STARTED) {
        if (playerState == PlayerStates::PLAYER_PAUSED) {
            VideoPlay(testSample);
        } else {
            VideoStop(testSample);
            VideoPlay(testSample);
        }
    }
    int32_t ret = testSample.adapter->EnableSingleLooping(loop == 1);
    if (ret != 0) {
        printf("[%s: %d] ret:%d\n", __func__, __LINE__, ret);
    }
    printf("Set loop %d\n", loop);
}

void VideoNext(TestSample &testSample)
{
    if (testSample.adapter == nullptr) {
        printf("Player adapter is null, please check.\n");
        return;
    }
    printf("VideoNext\n");
    VideoStop(testSample);
    VideoPlay(testSample);
}

void VideoPressurePull(TestSample &testSample)
{
    if (testSample.adapter == nullptr) {
        printf("Player adapter is null, please check.\n");
        return;
    }
    int32_t playerState = 0;
    testSample.adapter->GetPlayerState(playerState);
    if (playerState != PlayerStates::PLAYER_STARTED) {
        if (playerState == PlayerStates::PLAYER_PAUSED) {
            VideoPlay(testSample);
        } else {
            VideoStop(testSample);
            VideoPlay(testSample);
        }
    }
    printf("VideoPressurePull\n");
    int maxPressure = 10;
    for (int i = 0; i < maxPressure; i++) {
        VideoPause(testSample);
        usleep(ONE_HUNDRED_MICROSECONDS);
        VideoPlay(testSample);
        usleep(ONE_HUNDRED_MICROSECONDS);
    }
}

void VideoPressureSeek(TestSample &testSample)
{
    if (testSample.adapter == nullptr) {
        printf("Player adapter is null, please check.\n");
        return;
    }
    int32_t playerState = 0;
    testSample.adapter->GetPlayerState(playerState);
    if (playerState != PlayerStates::PLAYER_STARTED || playerState != PlayerStates::PLAYER_PAUSED) {
        VideoStop(testSample);
        VideoPlay(testSample);
    }
    int maxPressure = 10;
    for (int i = 0; i < maxPressure; i++) {
        testSample.adapter->Rewind(0, PlayerSeekMode::PLAYER_SEEK_PREVIOUS_SYNC);
        sleep(1);
    }
}


std::map<std::string, std::function<void(TestSample&)>> g_mediaTypeMap = {
    {"play", VideoPlay},
    {"pause", VideoPause},
    {"stop", VideoStop},
    {"replay", VideoReplay},
    {"seek", VideoSeek},
    {"speed", VideoSpeed},
    {"volume", VideSetVolume},
    {"loop", VideoLoop},
    {"show", VideoShow},
    {"next", VideoNext},
    {"pressure_pull", VideoPressurePull},
    {"pressure_seek", VideoPressureSeek},
    {"quit", [](TestSample&) {
        printf("quit\n");
        g_terminate = true;
    }}
};

static void SampleWindowInit(RootView* rootView, \
    UISurfaceView* surfaceView, int width, int height)
{
    rootView->SetWidth(width);
    rootView->SetHeight(height);
    rootView->SetPosition(0, 0);
    rootView->SetStyle(STYLE_BACKGROUND_COLOR, Color::White().full);

    surfaceView->SetPosition(0, 0);
    surfaceView->SetWidth(width);
    surfaceView->SetHeight(height);
    rootView->Add(surfaceView);
    rootView->Invalidate();
}

static void SampleWindowShow(RootView* rootView, int posX, int posY)
{
    if (rootView != nullptr) {
        WindowConfig config = {};
        config.rect = rootView->GetRect();
        config.rect.SetPosition(posX, posY);
        Window* window = Window::CreateWindow(config);
        if (window != nullptr) {
            window->BindRootView(rootView);
            window->Show();
        } else {
            printf("Create window false!\n");
        }
    }
}

static void SampleWindowDeinit(RootView* rootView, UISurfaceView* surfaceView)
{
    rootView->Remove(surfaceView);
    if (surfaceView != nullptr) {
        delete surfaceView;
        surfaceView = nullptr;
    }
    rootView->Invalidate();
    OHOS::TaskManager::GetInstance()->TaskHandler();
    if (rootView != nullptr) {
        Window::DestroyWindow(rootView->GetBoundWindow());
        RootView::DestroyWindowRootView(rootView);
        rootView = nullptr;
    }
}

static int SampleInputCheck(int argc, char *argv[], std::string &filepath)
{
    int maxArgc = 2;
    prctl(PR_SET_NAME, "mainProc", 0, 0, 0);
    if (signal(SIGINT, SignalHandler) == SIG_ERR) {
        perror("signal SIGINT");
        return -1;
    }
    if (signal(SIGTSTP, SignalHandler) == SIG_ERR) {
        perror("signal SIGTSTP");
        return -1;
    }
    if (argc < maxArgc) {
        printf("Usage: %s <filepath>\n", argv[0]);
        return -1;
    }
    filepath = argv[1];
    return 0;
}

static void SampleWindowSelect(int *width, int *height)
{
    const int HEIGHT_360P = 360;
    const int WIDTH_640P = 640;
    const int HEIGHT_1080P = 1080;
    const int WIDTH_1920P = 1920;
    cout << "Please select preview resolution:" << endl;
    cout << "1. 640x360" << endl;
    cout << "2. 1920x1080" << endl;
    char input;
    cin >> input;
    switch (input) {
        case '1':
            *width = WIDTH_640P;
            *height = HEIGHT_360P;
            break;
        case '2':
            *width = WIDTH_1920P;
            *height = HEIGHT_1080P;
            break;
        default:
            *width = WIDTH_640P;
            *height = HEIGHT_360P;
            cout << "Invalid input." << endl;
        }
}

static void SampleProcessing(TestSample &testSample)
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
        cout << "seek" << endl;
        cout << "speed" << endl;
        cout << "volume" << endl;
        cout << "loop" << endl;
        cout << "show" << endl;
        cout << "next" << endl;
        cout << "pressure_pull" << endl;
        cout << "pressure_seek" << endl;
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
    std::string filepath;
    int ret = SampleInputCheck(argc, argv, filepath);
    if (ret != 0) {
        printf("SampleInputCheck failed, ret = %d\n", ret);
        return -1;
    }
    int posX = 0;
    int posY = 0;
    int width = 0;
    int height = 0;
    SampleWindowSelect(&width, &height);
    RootView* rootView = nullptr;
    UISurfaceView* surfaceView = nullptr;
    GraphicStartUp::Init();
    rootView = RootView::GetWindowRootView();
    surfaceView = new UISurfaceView();
    SampleWindowInit(rootView, surfaceView, width, height);
    struct TestSample testSample;
    testSample.adapter = std::make_shared<Player>();
    std::map<std::string, std::string> header;
    testSample.filepath = filepath;
    Source source(testSample.filepath, header);
    testSample.adapter->SetSource(source);
    testSample.adapter->Prepare();
    testSample.cb = std::make_shared<PlayerCallbackImpl>();
    testSample.adapter->SetPlayerCallback(testSample.cb);
    testSample.surface = surfaceView->GetSurface();
    ret = testSample.adapter->SetVideoSurface(testSample.surface);
    if (ret != 0) {
        printf("SetVideoSurface failed, ret = %d\n", ret);
        return -1;
    }
    testSample.adapter->Play();
    SampleWindowShow(rootView, posX, posY);

    SampleProcessing(testSample);

    SampleWindowDeinit(rootView, surfaceView);
    printf("Stop\n");
    testSample.adapter->Stop();
    printf("Release\n");
    testSample.adapter->Release();
    testSample.adapter = nullptr;

    if (testSample.cb != nullptr) {
        testSample.cb.reset();
        testSample.cb = nullptr;
    }
    return 0;
}
