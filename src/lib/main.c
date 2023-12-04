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
#include <redline/edit.h>
#include <relay-client-udp/client.h>
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

typedef struct AppListenerConnection {
    uint8_t connectionId;
} AppListenerConnection;

typedef struct AppListenerConnections {
    AppListenerConnection connections[8];
    uint8_t count;
} AppListenerConnections;

static AppListenerConnection* appListenerConnectionsFind(
    AppListenerConnections* self, uint8_t index)
{
    for (size_t i = 0; i < self->count; ++i) {
        if (self->connections[i].connectionId == index) {
            return &self->connections[i];
        }
    }

    return 0;
}

static void appListenerConnectionsAdd(AppListenerConnections* self, uint8_t index)
{
    self->connections[self->count++].connectionId = index;
}

typedef struct App {
    const char* secret;
    RelayClientUdp relayClient;
    bool hasStartedRelayClient;
    RelayListener* listener;
    AppListenerConnections listenerConnections;
    RelayConnector* connector;

    Clog log;
#define tempBufSize (1200)
    uint8_t tempBuf[tempBufSize];
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

typedef struct ListenCmd {
    int verbose;
    uint64_t applicationId;
    int channelId;
} ListenCmd;

static ClashOption listenOptions[] = { { "applicationId", 'i', "the application ID",
                                           ClashTypeUInt64 | ClashTypeArg, "42",
                                           offsetof(ListenCmd, applicationId) },
    { "channel", 'c', "channel to listen to", ClashTypeInt, "8", offsetof(ListenCmd, channelId) } };

static void onListen(void* _self, const void* _data, ClashResponse* response)
{
    App* self = (App*)_self;
    if (self->listener != 0) {
        CLOG_C_WARN(&self->log, "listener is already set")
        return;
    }
    const ListenCmd* data = (const ListenCmd*)_data;
    RelaySerializeApplicationId applicationId = data->applicationId;
    RelaySerializeChannelId channelId = (RelaySerializeChannelId)data->channelId;

    self->listener
        = relayClientStartListen(&self->relayClient.relayClient, applicationId, channelId);

    (void)response;
}

typedef struct ConnectCmd {
    int verbose;
    uint64_t userId;
    uint64_t applicationId;
    int channelId;
} ConnectCmd;

static ClashOption connectOptions[] = {
    { "userId", 'i', "the Guise userId to connect to", ClashTypeUInt64 | ClashTypeArg, "",
        offsetof(ConnectCmd, userId) },
    { "applicationId", 'i', "the application ID", ClashTypeUInt64, "42",
        offsetof(ConnectCmd, applicationId) },
    { "channel", 'c', "channel to connect to", ClashTypeInt, "8", offsetof(ConnectCmd, channelId) }
};

static void onConnect(void* _self, const void* _data, ClashResponse* response)
{
    App* self = (App*)_self;
    if (self->connector != 0) {
        CLOG_C_WARN(&self->log, "connector is already set")
        return;
    }

    const ConnectCmd* data = (const ConnectCmd*)_data;

    RelaySerializeApplicationId applicationId = data->applicationId;
    RelaySerializeChannelId channelId = (RelaySerializeChannelId)data->channelId;
    RelaySerializeUserId userId = (RelaySerializeUserId)data->userId;

    self->connector
        = relayClientStartConnect(&self->relayClient.relayClient, userId, applicationId, channelId);

    (void)response;
}

typedef struct ListenerReadCmd {
    int verbose;
} ListenerReadCmd;

typedef struct ListenerWriteCmd {
    int connectionIndex;
    const char* payload;
} ListenerWriteCmd;

static void onListenerRead(void* _self, const void* _data, ClashResponse* response)
{
    App* self = (App*)_self;
    if (self->listener == 0) {
        CLOG_C_WARN(&self->log, "no listener")
        return;
    }

    (void)_data;
    //const ListenerReadCmd* data = (const ListenerReadCmd*)_data;
    uint8_t connectionIndex;

    ssize_t octetCount
        = relayListenerReceivePacket(self->listener, &connectionIndex, self->tempBuf, tempBufSize);
    if (octetCount <= 0) {
        return;
    }

    self->tempBuf[octetCount] = 0;

    CLOG_C_DEBUG(&self->log, "received packet '%s'", (const char*)self->tempBuf)

    (void)response;
}

static void onListenerWrite(void* _self, const void* _data, ClashResponse* response)
{
    App* self = (App*)_self;
    if (self->listener == 0) {
        CLOG_C_WARN(&self->log, "no listener")
        return;
    }

    const ListenerWriteCmd* data = (const ListenerWriteCmd*)_data;

    ssize_t result = relayListenerSendToConnectionIndex(self->listener,
        (size_t)data->connectionIndex, (const uint8_t*)data->payload, tc_strlen(data->payload));
    if (result < 0) {
        CLOG_C_WARN(&self->log, "could not send to connection index %d", data->connectionIndex)
        return;
    }

    CLOG_C_DEBUG(
        &self->log, "sent '%s' to connection index %d", data->payload, data->connectionIndex)

    (void)response;
}

typedef struct ConnectorReadCmd {
    int verbose;
} ConnectorReadCmd;

typedef struct ConnectorWriteCmd {
    const char* payload;
} ConnectorWriteCmd;

static void onConnectorRead(void* _self, const void* _data, ClashResponse* response)
{
    App* self = (App*)_self;
    if (self->listener == 0) {
        CLOG_C_WARN(&self->log, "no listener")
        return;
    }

    (void)_data;

    //const ConnectorReadCmd* data = (const ConnectorReadCmd*)_data;

    for (size_t i = 0; i < 3; ++i) {
        ssize_t octetsReceived = datagramTransportReceive(
            &self->connector->connectorTransport, self->tempBuf, tempBufSize);
        if (octetsReceived <= 0) {
            return;
        }
        self->tempBuf[octetsReceived] = 0;

        printf("received: '%s'", self->tempBuf);
    }

    (void)response;
}

static void onConnectorWrite(void* _self, const void* _data, ClashResponse* response)
{
    App* self = (App*)_self;
    if (self->connector == 0) {
        CLOG_C_WARN(&self->log, "no connector")
        return;
    }

    const ConnectorWriteCmd* data = (const ConnectorWriteCmd*)_data;

    int result = datagramTransportSend(&self->connector->connectorTransport,
        (const uint8_t*)data->payload, tc_strlen(data->payload));
    if (result < 0) {
        CLOG_C_WARN(&self->log, "result %d", result)
        return;
    }
    CLOG_C_DEBUG(&self->log, "sent '%s'", data->payload)
    (void)response;
}

static ClashOption listenerWriteOptions[] = {
    { "connection", 'c', "the connection index to write to", ClashTypeInt | ClashTypeArg, "0",
        offsetof(ListenerWriteCmd, connectionIndex) },
    { "payload", 'p', "the application ID", ClashTypeString | ClashTypeArg, "data",
        offsetof(ListenerWriteCmd, payload) },
};

static ClashCommand listenerCommands[] = {
    { "read", "Read from listener", sizeof(struct ListenerReadCmd), 0, 0, 0, 0,
        (ClashFn)onListenerRead },
    { "write", "Write to a listener", sizeof(struct ListenerWriteCmd), listenerWriteOptions,
        sizeof(listenerWriteOptions) / sizeof(listenerWriteOptions[0]), 0, 0,
        (ClashFn)onListenerWrite },
};

static ClashOption connectorWriteOptions[] = {
    { "payload", 'p', "the application ID", ClashTypeString | ClashTypeArg, "data",
        offsetof(ConnectorWriteCmd, payload) },
};

static ClashCommand connectorCommands[] = {
    { "read", "Read from connector", sizeof(struct ConnectorReadCmd), 0, 0, 0, 0,
        (ClashFn)onConnectorRead },
    { "write", "Write to a connector", sizeof(struct ConnectorWriteCmd), connectorWriteOptions,
        sizeof(connectorWriteOptions) / sizeof(connectorWriteOptions[0]), 0, 0,
        (ClashFn)onConnectorWrite },
};

static ClashCommand mainCommands[] = {
    { "listener", "use listener", 0, 0, 0, listenerCommands,
        sizeof(listenerCommands) / sizeof(listenerCommands[0]), 0 },
    { "connector", "use connector", 0, 0, 0, connectorCommands,
        sizeof(connectorCommands) / sizeof(connectorCommands[0]), 0 },

    { "listen", "start listening on relay server", sizeof(ListenCmd), listenOptions,
        sizeof(listenOptions) / sizeof(listenOptions[0]), 0, 0, onListen },
    { "connect", "start connecting to listener on relay server", sizeof(ConnectCmd), connectOptions,
        sizeof(connectOptions) / sizeof(connectOptions[0]), 0, 0, onConnect },
    { "state", "show state on relay client", 0, 0, 0, 0, 0, onState },
};

static ClashDefinition commands = { mainCommands, sizeof(mainCommands) / sizeof(mainCommands[0]) };
static void outputChangesIfAny(App* app, RedlineEdit* edit)
{
    static const size_t RECEIVE_COUNT = 32;

    if (app->listener != 0) {
        uint8_t connectionIndex;

        for (size_t i = 0; i < RECEIVE_COUNT; ++i) {
            ssize_t octetsReceived = relayListenerReceivePacket(
                app->listener, &connectionIndex, app->tempBuf, tempBufSize);
            if (octetsReceived <= 0) {
                break;
            }

            AppListenerConnection* connection
                = appListenerConnectionsFind(&app->listenerConnections, connectionIndex);
            if (connection == 0) {
                CLOG_C_DEBUG(
                    &app->log, "listener accepting incoming connection %hhu", connectionIndex)
                relayListenerSendToConnectionIndex(
                    app->listener, connectionIndex, (const uint8_t*)"accepted", 9);
                appListenerConnectionsAdd(&app->listenerConnections, connectionIndex);
            }
            app->tempBuf[octetsReceived] = 0;
            redlineEditRemove(edit);

            printf("listener received: connectionIndex:%hhu data:'%s'\n", connectionIndex,
                app->tempBuf);

            drawPrompt(edit);
            redlineEditBringback(edit);
        }
    }

    if (app->connector != 0) {
        for (size_t i = 0; i < RECEIVE_COUNT; ++i) {
            ssize_t octetsReceived = datagramTransportReceive(
                &app->connector->connectorTransport, app->tempBuf, tempBufSize);
            if (octetsReceived <= 0) {
                break;
            }
            app->tempBuf[octetsReceived] = 0;

            redlineEditRemove(edit);
            printf("connector received: '%s'\n", app->tempBuf);

            drawPrompt(edit);
            redlineEditBringback(edit);
        }
    }
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
    imprintDefaultSetupInit(&imprint, 8 * 1024 * 1024); // TODO: Investigate high memory usage(!)

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
    const uint16_t relayPort = 27005;

    App app;
    app.secret = "working";
    app.hasStartedRelayClient = false;
    app.log.config = &g_clog;
    app.log.constantPrefix = "app";
    app.listener = 0;
    app.connector = 0;
    app.listenerConnections.count = 0;
    app.listenerConnections.connections[0].connectionId = 0;

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
