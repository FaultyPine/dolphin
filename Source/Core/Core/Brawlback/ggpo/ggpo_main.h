/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#ifndef GGPO_MAIN_H
#define GGPO_MAIN_H

#include "types.h"
#include "backends/p2p.h"
#include "backends/synctest.h"
#include "backends/spectator.h"
#include "../include/ggponet.h"


namespace GGPO {

void
ggpo_log(GGPOSession *ggpo, const char *fmt, ...);

void
ggpo_logv(GGPOSession *ggpo, const char *fmt, va_list args);

GGPOErrorCode
ggpo_start_session(GGPOSession **session,
                   GGPOSessionCallbacks *cb,
                   const char *game,
                   int num_players,
                   int input_size,
                   unsigned short localport);

GGPOErrorCode
ggpo_add_player(GGPOSession *ggpo,
                GGPOPlayer *player,
                GGPOPlayerHandle *handle);



GGPOErrorCode
ggpo_start_synctest(GGPOSession **ggpo,
                    GGPOSessionCallbacks *cb,
                    char *game,
                    int num_players,
                    int input_size,
                    int frames);

GGPOErrorCode
ggpo_set_frame_delay(GGPOSession *ggpo,
                     GGPOPlayerHandle player,
                     int frame_delay);

GGPOErrorCode
ggpo_idle(GGPOSession *ggpo, int timeout);

GGPOErrorCode
ggpo_add_local_input(GGPOSession *ggpo,
                     GGPOPlayerHandle player,
                     void *values,
                     int size);

GGPOErrorCode
ggpo_synchronize_input(GGPOSession *ggpo,
                       void *values,
                       int size,
                       int *disconnect_flags);

GGPOErrorCode ggpo_disconnect_player(GGPOSession *ggpo,
                                     GGPOPlayerHandle player);

GGPOErrorCode
ggpo_advance_frame(GGPOSession *ggpo);

GGPOErrorCode
ggpo_client_chat(GGPOSession *ggpo, char *text);

GGPOErrorCode
ggpo_get_network_stats(GGPOSession *ggpo,
                       GGPOPlayerHandle player,
                       GGPONetworkStats *stats);


GGPOErrorCode
ggpo_close_session(GGPOSession *ggpo);

GGPOErrorCode
ggpo_set_disconnect_timeout(GGPOSession *ggpo, int timeout);

GGPOErrorCode
ggpo_set_disconnect_notify_start(GGPOSession *ggpo, int timeout);

GGPOErrorCode ggpo_start_spectating(GGPOSession **session,
                                    GGPOSessionCallbacks *cb,
                                    const char *game,
                                    int num_players,
                                    int input_size,
                                    unsigned short local_port,
                                    char *host_ip,
                                    unsigned short host_port);


} // namespace GGPO


#endif