#include "Limelight-internal.h"
#include "Platform.h"

static int stage = STAGE_NONE;
static ConnListenerConnectionTerminated originalTerminationCallback;
static int alreadyTerminated;

// Common globals
struct sockaddr_storage RemoteAddr;
SOCKADDR_LEN RemoteAddrLen;
int ServerMajorVersion;
STREAM_CONFIGURATION StreamConfig;
PLATFORM_CALLBACKS PlatformCallbacks;
CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;
DECODER_RENDERER_CALLBACKS VideoCallbacks;
AUDIO_RENDERER_CALLBACKS AudioCallbacks;

/* Connection stages */
static const char* stageNames[STAGE_MAX] = {
	"none",
	"platform initialization",
    "name resolution",
	"RTSP handshake",
	"control stream initialization",
	"video stream initialization",
	"audio stream initialization",
	"input stream initialization",
	"control stream establishment",
	"video stream establishment",
	"audio stream establishment",
	"input stream establishment"
};

/* Get the name of the current stage based on its number */
const char* LiGetStageName(int stage) {
	return stageNames[stage];
}

/* Stop the connection by undoing the step at the current stage and those before it */
void LiStopConnection(void) {
    // Disable termination callbacks now
    alreadyTerminated = 1;
    
	if (stage == STAGE_INPUT_STREAM_START) {
		Limelog("Stopping input stream...");
		stopInputStream();
		stage--;
		Limelog("done\n");
	}
	if (stage == STAGE_AUDIO_STREAM_START) {
		Limelog("Stopping audio stream...");
		stopAudioStream();
		stage--;
		Limelog("done\n");
	}
	if (stage == STAGE_VIDEO_STREAM_START) {
		Limelog("Stopping video stream...");
		stopVideoStream();
		stage--;
		Limelog("done\n");
	}
	if (stage == STAGE_CONTROL_STREAM_START) {
		Limelog("Stopping control stream...");
		stopControlStream();
		stage--;
		Limelog("done\n");
	}
	if (stage == STAGE_INPUT_STREAM_INIT) {
		Limelog("Cleaning up input stream...");
		destroyInputStream();
		stage--;
		Limelog("done\n");
	}
	if (stage == STAGE_AUDIO_STREAM_INIT) {
		Limelog("Cleaning up audio stream...");
		destroyAudioStream();
		stage--;
		Limelog("done\n");
	}
	if (stage == STAGE_VIDEO_STREAM_INIT) {
		Limelog("Cleaning up video stream...");
		destroyVideoStream();
		stage--;
		Limelog("done\n");
	}
	if (stage == STAGE_CONTROL_STREAM_INIT) {
		Limelog("Cleaning up control stream...");
		destroyControlStream();
		stage--;
		Limelog("done\n");
	}
	if (stage == STAGE_RTSP_HANDSHAKE) {
		Limelog("Terminating RTSP handshake...");
		terminateRtspHandshake();
		stage--;
		Limelog("done\n");
	}
    if (stage == STAGE_NAME_RESOLUTION) {
        // Nothing to do
        stage--;
    }
	if (stage == STAGE_PLATFORM_INIT) {
		Limelog("Cleaning up platform...");
		cleanupPlatform();
		stage--;
		Limelog("done\n");
	}
	LC_ASSERT(stage == STAGE_NONE);
}

static void ClInternalConnectionTerminated(long errorCode)
{
    // Avoid recursion and issuing multiple callbacks
    if (alreadyTerminated) {
        return;
    }
    
    alreadyTerminated = 1;
    originalTerminationCallback(errorCode);
}

void LiCompleteThreadStart(void)
{
	PltRunThreadProc();
}

static int resolveHostName(const char *host)
{
    struct addrinfo hints, *res;
    int err;
    
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags = AI_ADDRCONFIG;
    err = getaddrinfo(host, NULL, &hints, &res);
    if (err != 0) {
        Limelog("getaddrinfo() failed: %d\n", err);
        return err;
    }
    
    if (res == NULL) {
        Limelog("getaddrinfo() returned success without addresses\n");
        return -1;
    }
    
    // Use the first address in the list
    memcpy(&RemoteAddr, res->ai_addr, res->ai_addrlen);
    RemoteAddrLen = res->ai_addrlen;
    
    freeaddrinfo(res);
    return 0;
}

/* Starts the connection to the streaming machine */
int LiStartConnection(const char* host, PSTREAM_CONFIGURATION streamConfig, PCONNECTION_LISTENER_CALLBACKS clCallbacks,
	PDECODER_RENDERER_CALLBACKS drCallbacks, PAUDIO_RENDERER_CALLBACKS arCallbacks, PPLATFORM_CALLBACKS plCallbacks,
	void* renderContext, int drFlags, int _serverMajorVersion) {
	int err;

    ServerMajorVersion = _serverMajorVersion;
    memcpy(&StreamConfig, streamConfig, sizeof(StreamConfig));

	// Replace missing callbacks with placeholders
	fixupMissingCallbacks(&drCallbacks, &arCallbacks, &clCallbacks, &plCallbacks);
	memcpy(&PlatformCallbacks, plCallbacks, sizeof(PlatformCallbacks));
    memcpy(&VideoCallbacks, drCallbacks, sizeof(VideoCallbacks));
    memcpy(&AudioCallbacks, arCallbacks, sizeof(AudioCallbacks));

	// Hook the termination callback so we can avoid issuing a termination callback
	// after LiStopConnection() is called
	originalTerminationCallback = clCallbacks->connectionTerminated;
	memcpy(&ListenerCallbacks, clCallbacks, sizeof(ListenerCallbacks));
    ListenerCallbacks.connectionTerminated = ClInternalConnectionTerminated;
    
    alreadyTerminated = 0;

	Limelog("Initializing platform...");
	ListenerCallbacks.stageStarting(STAGE_PLATFORM_INIT);
	err = initializePlatform();
	if (err != 0) {
		Limelog("failed: %d\n", err);
		ListenerCallbacks.stageFailed(STAGE_PLATFORM_INIT, err);
		goto Cleanup;
	}
	stage++;
	LC_ASSERT(stage == STAGE_PLATFORM_INIT);
	ListenerCallbacks.stageComplete(STAGE_PLATFORM_INIT);
	Limelog("done\n");
    
    Limelog("Resolving host name...");
    ListenerCallbacks.stageStarting(STAGE_NAME_RESOLUTION);
    err = resolveHostName(host);
    if (err != 0) {
        Limelog("failed: %d\n", err);
        ListenerCallbacks.stageFailed(STAGE_NAME_RESOLUTION, err);
        goto Cleanup;
    }
    stage++;
    LC_ASSERT(stage == STAGE_NAME_RESOLUTION);
    ListenerCallbacks.stageComplete(STAGE_NAME_RESOLUTION);
    Limelog("done\n");

	Limelog("Starting RTSP handshake...");
	ListenerCallbacks.stageStarting(STAGE_RTSP_HANDSHAKE);
	err = performRtspHandshake();
	if (err != 0) {
		Limelog("failed: %d\n", err);
		ListenerCallbacks.stageFailed(STAGE_RTSP_HANDSHAKE, err);
		goto Cleanup;
	}
	stage++;
	LC_ASSERT(stage == STAGE_RTSP_HANDSHAKE);
	ListenerCallbacks.stageComplete(STAGE_RTSP_HANDSHAKE);
	Limelog("done\n");

	Limelog("Initializing control stream...");
	ListenerCallbacks.stageStarting(STAGE_CONTROL_STREAM_INIT);
	err = initializeControlStream();
	if (err != 0) {
		Limelog("failed: %d\n", err);
		ListenerCallbacks.stageFailed(STAGE_CONTROL_STREAM_INIT, err);
		goto Cleanup;
	}
	stage++;
	LC_ASSERT(stage == STAGE_CONTROL_STREAM_INIT);
	ListenerCallbacks.stageComplete(STAGE_CONTROL_STREAM_INIT);
	Limelog("done\n");

	Limelog("Initializing video stream...");
	ListenerCallbacks.stageStarting(STAGE_VIDEO_STREAM_INIT);
	initializeVideoStream();
	stage++;
	LC_ASSERT(stage == STAGE_VIDEO_STREAM_INIT);
	ListenerCallbacks.stageComplete(STAGE_VIDEO_STREAM_INIT);
	Limelog("done\n");

	Limelog("Initializing audio stream...");
	ListenerCallbacks.stageStarting(STAGE_AUDIO_STREAM_INIT);
	initializeAudioStream();
	stage++;
	LC_ASSERT(stage == STAGE_AUDIO_STREAM_INIT);
	ListenerCallbacks.stageComplete(STAGE_AUDIO_STREAM_INIT);
	Limelog("done\n");

	Limelog("Initializing input stream...");
	ListenerCallbacks.stageStarting(STAGE_INPUT_STREAM_INIT);
	initializeInputStream(streamConfig->remoteInputAesKey, sizeof(streamConfig->remoteInputAesKey),
		streamConfig->remoteInputAesIv, sizeof(streamConfig->remoteInputAesIv));
	stage++;
	LC_ASSERT(stage == STAGE_INPUT_STREAM_INIT);
	ListenerCallbacks.stageComplete(STAGE_INPUT_STREAM_INIT);
	Limelog("done\n");

	Limelog("Starting control stream...");
	ListenerCallbacks.stageStarting(STAGE_CONTROL_STREAM_START);
	err = startControlStream();
	if (err != 0) {
		Limelog("failed: %d\n", err);
		ListenerCallbacks.stageFailed(STAGE_CONTROL_STREAM_START, err);
		goto Cleanup;
	}
	stage++;
	LC_ASSERT(stage == STAGE_CONTROL_STREAM_START);
	ListenerCallbacks.stageComplete(STAGE_CONTROL_STREAM_START);
	Limelog("done\n");

	Limelog("Starting video stream...");
	ListenerCallbacks.stageStarting(STAGE_VIDEO_STREAM_START);
	err = startVideoStream(renderContext, drFlags);
	if (err != 0) {
		Limelog("Video stream start failed: %d\n", err);
		ListenerCallbacks.stageFailed(STAGE_VIDEO_STREAM_START, err);
		goto Cleanup;
	}
	stage++;
	LC_ASSERT(stage == STAGE_VIDEO_STREAM_START);
	ListenerCallbacks.stageComplete(STAGE_VIDEO_STREAM_START);
	Limelog("done\n");

	Limelog("Starting audio stream...");
	ListenerCallbacks.stageStarting(STAGE_AUDIO_STREAM_START);
	err = startAudioStream();
	if (err != 0) {
		Limelog("Audio stream start failed: %d\n", err);
		ListenerCallbacks.stageFailed(STAGE_AUDIO_STREAM_START, err);
		goto Cleanup;
	}
	stage++;
	LC_ASSERT(stage == STAGE_AUDIO_STREAM_START);
	ListenerCallbacks.stageComplete(STAGE_AUDIO_STREAM_START);
	Limelog("done\n");

	Limelog("Starting input stream...");
	ListenerCallbacks.stageStarting(STAGE_INPUT_STREAM_START);
	err = startInputStream();
	if (err != 0) {
		Limelog("Input stream start failed: %d\n", err);
		ListenerCallbacks.stageFailed(STAGE_INPUT_STREAM_START, err);
		goto Cleanup;
	}
	stage++;
	LC_ASSERT(stage == STAGE_INPUT_STREAM_START);
	ListenerCallbacks.stageComplete(STAGE_INPUT_STREAM_START);
	Limelog("done\n");

	ListenerCallbacks.connectionStarted();

Cleanup:
	return err;
}
