/*
 *  Copyright 2014 Czech Technical University in Prague
 *
 *  Authors: Michal Sojka <sojkam1@fel.cvut.cz>
 *           Radek Matějka <radek.matejka@gmail.com>
 *           Ondřej Kulatý <kulatond@fel.cvut.cz>
 *
 *  This file is part of MaCAN.
 *
 *  MaCAN is free software: you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  MaCAN is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with MaCAN.	If not, see <http://www.gnu.org/licenses/>.
 */

#include <macan.h>
#include "macan_private.h"
#include "debug.h"
#include "cryptlib.h"
#include <assert.h>
#include <unistd.h> /* FIXME: get rid of this */

/**
 * ts_receive_challenge() - serves the request for signed time
 * @s:  socket handle
 * @cf: received can frame with TS challenge
 *
 * This function responds to a challenge message from a general node (i.e. the
 * request for a signed time). It prepares a message containing the recent time
 * and signs it. Subsequently, the message is sent.
 */
static
int ts_receive_challenge(struct macan_ctx *ctx, struct can_frame *cf)
{
	struct can_frame canf;
	struct macan_challenge *ch = (struct macan_challenge *)cf->data;
	struct macan_key skey;
	uint8_t plain[12];
	macan_ecuid dst_id;
	struct com_part *cp;

	if(!(cp = canid2cpart(ctx, cf->can_id)))
		return -1;

	dst_id = (uint8_t) (cp->ecu_id);

	if (!is_skey_ready(ctx, dst_id)) {
		print_msg(ctx, MSG_FAIL,"cannot send time, because don't have key\n");
		return -1;
	}

	skey = cp->skey;

	memcpy(plain, &ctx->ts.bcast_time, 4);
	memcpy(plain + 4, ch->chg, 6);
	memcpy(plain + 10, &CANID(ctx, ctx->config->time_server_id), 2);

	canf.can_id = CANID(ctx,ctx->config->time_server_id);
	canf.can_dlc = 8;
	memcpy(canf.data, &ctx->ts.bcast_time, 4);
	macan_sign(&skey, canf.data + 4, plain, 12);

	macan_send(ctx, &canf);

	print_msg(ctx, MSG_INFO,"signed time sent\n");
	return 0;
}

static
void macan_rx_cb_ts(macan_ev_loop *loop, macan_ev_can *w, int revents)
{
	(void)loop; (void)revents; /* suppress warnings */
	struct macan_ctx *ctx = w->data;
	struct can_frame cf;

	macan_read(ctx, &cf);
	enum macan_process_status status;

	status = macan_process_frame(ctx, &cf);

	if (status == MACAN_FRAME_CHALLENGE)
		ts_receive_challenge(ctx, &cf);
}

static void
time_broadcast_cb(macan_ev_loop *loop, macan_ev_timer *w, int revents)
{
	(void)loop; (void)revents;
	struct macan_ctx *ctx = w->data;
	struct can_frame cf = {0};
	uint64_t usec;

	usec = read_time();
	ctx->ts.bcast_time = usec / ctx->config->time_div;

	cf.can_id = CANID(ctx,ctx->config->time_server_id);
	cf.can_dlc = 4;
	memcpy(cf.data, &ctx->ts.bcast_time, 4);

	macan_send(ctx, &cf);
}


int macan_init_ts(struct macan_ctx *ctx, const struct macan_config *config, macan_ev_loop *loop, int sockfd)
{
	assert(config->node_id == config->time_server_id);

	int ret = __macan_init(ctx, config, sockfd);

	macan_ev_can_init (&ctx->can_watcher, macan_rx_cb_ts, sockfd, EV_READ);
	ctx->can_watcher.data = ctx;
	macan_ev_can_start (loop, &ctx->can_watcher);

	macan_ev_timer_init (&ctx->housekeeping, macan_housekeeping_cb, 0, 1000);
	ctx->housekeeping.data = ctx;
	macan_ev_timer_start(loop, &ctx->housekeeping);

	macan_ev_timer_init (&ctx->ts.time_bcast, time_broadcast_cb, 0, 1000);
	ctx->ts.time_bcast.data = ctx;
	macan_ev_timer_start(loop, &ctx->ts.time_bcast);

	return ret;
}
