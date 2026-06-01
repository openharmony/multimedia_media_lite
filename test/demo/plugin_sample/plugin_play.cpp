
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

#include "hal_camera.h"
#include "camera_server.h"
#include "player_sample_common.h"

int main(int argc, char *argv[])
{
    CameraServer::GetInstance()->InitCameraServer();
    CodecInit();
    int sleepTime = 3;
    sleep(sleepTime);
    int maxArgc = 2;
    if (argc < maxArgc) {
        printf("Usage: %s <filepath>\n", argv[0]);
        return -1;
    }
    int height480p = 480;
    int width640p = 640;
    std::string filepath = argv[1];
    CommonData data;
    data.fileName = filepath;
    data.x = 0;
    data.y = 0;
    data.width = width640p;
    data.height = height480p;

    InitPlayer(data);
    DeinitPlayer(data);
    return 0;
}