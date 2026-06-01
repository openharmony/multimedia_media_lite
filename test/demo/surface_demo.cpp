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

using namespace std;
using namespace OHOS;

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

int main(int argc, char *argv[])
{
    int posX = 0;
    int posY = 0;
    int width = 640;
    int height = 360;
    RootView* rootView = nullptr;
    UISurfaceView* surfaceView = nullptr;
    GraphicStartUp::Init();
    rootView = RootView::GetWindowRootView();
    surfaceView = new UISurfaceView();
    SampleWindowInit(rootView, surfaceView, width, height);
    Window* window = nullptr;
    if (rootView != nullptr) {
        printf("rootView: %p\n", rootView);
        WindowConfig config = {};
        config.rect = rootView->GetRect();
        config.rect.SetPosition(posX, posY);
        window = Window::CreateWindow(config);
        if (window != nullptr) {
            window->BindRootView(rootView);
            window->Show();
        } else {
            printf("Create window false!\n");
        }
    }
    int sleepTime = 5;
    sleep(sleepTime);
    printf("delete surfaceView: %p\n", surfaceView);
    rootView->Remove(surfaceView);
    if (surfaceView != nullptr) {
        delete surfaceView;
        surfaceView = nullptr;
    }
    rootView->Invalidate();
    OHOS::TaskManager::GetInstance()->TaskHandler();
    sleep(sleepTime);
    printf("delete rootView: %p\n", rootView);
    if (rootView != nullptr) {
        Window::DestroyWindow(rootView->GetBoundWindow());
        RootView::DestroyWindowRootView(rootView);
        rootView = nullptr;
    }
    return 0;
}
