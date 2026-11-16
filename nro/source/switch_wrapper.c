/*
    Copyright 2019 natinusala
    Copyright 2019 WerWolv
    Copyright 2019 p-sam

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#include <stdio.h>
#include <switch.h>
#include <unistd.h>

static int nxlink_sock = -1;

void userAppInit()
{
    printf("userAppInit\n");
    appletLockExit();

    // SwitchMusic has one HTTP worker; two BSD sessions suffice. Avoid the
    // demo wrapper's twelve-session network pool and its larger buffers.
    SocketInitConfig cfg = *(socketGetDefaultInitConfig());
    cfg.num_bsd_sessions = 2;
    cfg.sb_efficiency = 2;
    Result network = socketInitialize(&cfg);
    if (R_FAILED(network))
        printf("SwitchMusic network initialization failed: 0x%08x\n", network);

#ifdef DEBUG
    nxlink_sock = nxlinkStdio();
#endif

    romfsInit();
    plInitialize(PlServiceType_User);
    setsysInitialize();
    setInitialize();
    psmInitialize();
    nifmInitialize(NifmServiceType_User);
    lblInitialize();
}

void userAppExit()
{
    printf("userAppExit\n");

    // backlight
    lblExit();
    // network state
    nifmExit();
    // power state
    psmExit();
    // system language...
    setExit();
    // system theme, system version...
    setsysExit();
    // system font
    plExit();

    romfsExit();

    if (nxlink_sock != -1)
        close(nxlink_sock);

    socketExit();

    appletUnlockExit();
}
