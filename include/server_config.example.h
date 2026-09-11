#pragma once

// Copy this file to `server_config.h` and fill in your Mac LAN IP.
// The real `server_config.h` file is ignored by Git.

#define POST_SERVER_HOST "YOUR_MAC_LAN_IP"
#define POST_SERVER_PORT 3000
#define POST_SERVER_PATH "/upload"

#ifndef SPEAKER_PULL_PATH
#define SPEAKER_PULL_PATH "/speaker/pull"
#endif

#ifndef SPEAKER_FLUSH_PATH
#define SPEAKER_FLUSH_PATH "/speaker/flush"
#endif

#ifndef UTTERANCE_END_PATH
#define UTTERANCE_END_PATH "/utterance/end"
#endif

