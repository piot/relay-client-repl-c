/*----------------------------------------------------------------------------------------------------------
 *  Copyright (c) Peter Bjorklund. All rights reserved. https://github.com/piot/relay-client-repl-c
 *  Licensed under the MIT License. See LICENSE in the project root for license information.
 *--------------------------------------------------------------------------------------------------------*/
#include <clash/clash.h>
#include <clash/response.h>
#include <clog/clog.h>
#include <clog/console.h>
#include <errno.h>
#include <flood/out_stream.h>
#include <guise-client-udp/client.h>
#include <guise-client-udp/read_secret.h>
#include <imprint/default_setup.h>
#include <inttypes.h>
#include <redline/edit.h>
#include <relay-client-udp/client.h>
#include <relay-client/debug.h>
#include <signal.h>

clog_config g_clog;

static int g_quit = 0;

static void interruptHandler(int sig)
{
    (void)sig;

    g_quit = 1;
}

#include <time.h>
static void sleepMs(size_t milliseconds)
{
    struct timespec ts;
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = (milliseconds % 1000) * 1000000;

    int err = nanosleep(&ts, &ts);
    if (err != 0) {
        CLOG_ERROR("NOT WORKING:%d", errno)
    }
}

static void drawPrompt(RedlineEdit* edit)
{
    redlineEditPrompt(edit, "relay> ");
}

typedef struct App {
    const char* secret;
    RelayClientUdp relayClient;
    bool hasStartedRelayClient;
    Clog log;
} App;

static void onState(void* _self, const void* data, ClashResponse* response)
{
    (void)data;
    (void)response;

    App* self = (App*)_self;
    if (!self->hasStartedRelayClient) {
        clashResponseWritecf(response, 4, "relay not started yet\n");
        return;
    }
}

static ClashCommand mainCommands[] = {
    { "state", "show state on relay client", 0, 0, 0, 0, 0, onState },
};

static ClashDefinition commands = { mainCommands, sizeof(mainCommands) / sizeof(mainCommands[0]) };

static void outputChangesIfAny(App* app, RedlineEdit* edit)
{
    (void)app;
    (void)edit;
}

int main(int argc, char** argv)
{
    g_clog.log = clog_console;
    g_clog.level = CLOG_TYPE_VERBOSE;

    signal(SIGINT, interruptHandler);

    GuiseClientUdpSecret guiseSecret;

    size_t indexToRead = 0;
    if (argc > 1) {
        indexToRead = (size_t)atoi(argv[1]);
    }

    guiseClientUdpReadSecret(&guiseSecret, indexToRead);

    ImprintDefaultSetup imprint;
    imprintDefaultSetupInit(&imprint, 128 * 1024);

    GuiseClientUdp guiseClient;
    guiseClientUdpInit(&guiseClient, 0, "127.0.0.1", 27004, &guiseSecret);

    RedlineEdit edit;

    redlineEditInit(&edit);

    drawPrompt(&edit);

    FldOutStream outStream;
    uint8_t buf[1024];
    fldOutStreamInit(&outStream, buf, 1024);

    Clog relayClientUdpLog;
    relayClientUdpLog.config = &g_clog;
    relayClientUdpLog.constantPrefix = "relayClientUdp";

    const char* relayHost = "127.0.0.1";
    const uint16_t relayPort = 27003;

    App app;
    app.secret = "working";
    app.hasStartedRelayClient = false;
    app.log.config = &g_clog;
    app.log.constantPrefix = "app";

    while (!g_quit) {
        MonotonicTimeMs now = monotonicTimeMsNow();
        guiseClientUdpUpdate(&guiseClient, now);
        if (!app.hasStartedRelayClient
            && guiseClient.guiseClient.state == GuiseClientStateLoggedIn) {
            CLOG_INFO("relay init")
            relayClientUdpInit(&app.relayClient, relayHost, relayPort,
                guiseClient.guiseClient.mainUserSessionId, monotonicTimeMsNow(),
                &imprint.tagAllocator.info, relayClientUdpLog);
            app.hasStartedRelayClient = true;
        }
        if (app.hasStartedRelayClient) {
            int updateResult = relayClientUdpUpdate(&app.relayClient, now);
            if (updateResult < 0) {
                return updateResult;
            }
            outputChangesIfAny(&app, &edit);
        }
        int result = redlineEditUpdate(&edit);
        if (result == -1) {
            printf("\n");
            const char* textInput = redlineEditLine(&edit);
            if (tc_str_equal(textInput, "quit")) {
                break;
            } else if (tc_str_equal(textInput, "help")) {
                outStream.p = outStream.octets;
                outStream.pos = 0;
                clashUsageToStream(&commands, &outStream);
                puts((const char*)outStream.octets);
                outStream.p = outStream.octets;
                outStream.pos = 0;
            } else {
                outStream.p = outStream.octets;
                outStream.pos = 0;
                int parseResult = clashParseString(&commands, textInput, &app, &outStream);
                if (parseResult < 0) {
                    printf("unknown command %d\n", parseResult);
                }

                if (outStream.pos > 0) {
                    fputs((const char*)outStream.octets, stdout);
                }
                outStream.p = outStream.octets;
                outStream.pos = 0;
            }
            redlineEditClear(&edit);
            drawPrompt(&edit);
            redlineEditReset(&edit);
        }
        sleepMs(16);
    }

    redlineEditClose(&edit);

    return 0;
}
