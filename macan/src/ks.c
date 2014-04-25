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

#include "cryptlib.h"
#include "common.h"
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <assert.h>

#define NODE_COUNT 64

struct sess_key {
	bool valid;
	struct macan_key key;
};

void generate_skey(struct macan_ctx *ctx, struct sess_key *skey)
{
	skey->valid = true;
	if(!gen_rand_data(skey->key.data, sizeof(skey->key.data))) {
		print_msg(ctx, MSG_FAIL,"Failed to read enough random bytes.\n");
		exit(1);
	}
}

uint8_t lookup_skey(struct macan_ctx *ctx, macan_ecuid src_id, macan_ecuid dst_id, struct macan_key **key_ret)
{
	static struct sess_key skey_map[NODE_COUNT - 1][NODE_COUNT] = {{{0}}};

	macan_ecuid tmp;
	struct sess_key *key;

	if (src_id == dst_id)
		return 0;

	if (src_id > dst_id) {
		tmp = src_id;
		src_id = dst_id;
		dst_id = tmp;
	}

	key = &skey_map[src_id][dst_id];
	*key_ret = &key->key;

	if (!key->valid) {
		generate_skey(ctx, key);
		return 1;
	}
	return 0;
}

void send_skey(struct macan_ctx *ctx, const struct macan_key *key, macan_ecuid dst_id, macan_ecuid fwd_id, uint8_t *chal)
{
	uint8_t wrap[32];
	uint8_t plain[24];
	struct macan_key *skey;
	struct can_frame cf = {0};
	struct macan_sess_key macan_skey;
	int i;

	/* ToDo: solve name inconsistency - key */
	if (lookup_skey(ctx, dst_id, fwd_id, &skey)) {
		macan_send_challenge(ctx, fwd_id, dst_id, NULL);
	}

	memcpy(plain, skey->data, sizeof(skey->data));
	plain[16] = dst_id;
	plain[17] = fwd_id;
	memcpy(plain + 18, chal, 6);
	macan_aes_wrap(key, 24, wrap, plain);

/* 	print_msg(ctx, MSG_INFO,"send KEY (wrap, plain):\n"); */
/* 	print_hexn(wrap, 32); */
/* 	print_hexn(plain, 24); */

	macan_skey.flags_and_dst_id = (macan_ecuid)(FL_SESS_KEY << 6 | (dst_id & 0x3F));

	cf.can_id = CANID(ctx, ctx->config->key_server_id);
	cf.can_dlc = 8;

	for (i = 0; i < 6; i++) {
		macan_skey.seq_and_len = (uint8_t)((i << 4) /* seq */ | ((i == 5) ? 2 : 6) /* len */);
		memcpy(macan_skey.data, wrap + (6 * i), 6);
		memcpy(cf.data, &macan_skey, 8);

		/* ToDo: check all writes for success */
		macan_send(ctx, &cf);
	}
}

/**
 * ks_receive_challenge() - responds to challenge message
 * @s:   socket fd
 * @cf:  received can frame
 *
 * This function responds with session key to the challenge sender
 * and also sends REQ_CHALLENGE to communication partner of the sender.
 */
void ks_receive_challenge(struct macan_ctx *ctx, struct can_frame *cf)
{
	struct macan_challenge *chal;
	macan_ecuid dst_id, fwd_id;
	uint8_t *chg;

	chal = (struct macan_challenge *)cf->data;

	if(!macan_canid2ecuid(ctx, cf->can_id, &dst_id)) {
		return;
	}

	fwd_id = chal->fwd_id;
	chg = chal->chg;

	if (fwd_id >= ctx->config->node_count)
		return;

	const struct macan_key *ltk = ctx->ks.ltk[dst_id];
/* 	print_hexn(ltk, 16); */
	send_skey(ctx, ltk, dst_id, fwd_id, chg);
}

static void
can_cb_ks (macan_ev_loop *loop, macan_ev_can *w, int revents)
{
	(void)loop; (void)revents; /* suppress warnings */
	struct macan_ctx *ctx = w->data;
	struct can_frame cf;

	macan_read(ctx, &cf);

	/* Simple sanity checks first */
	if (cf.can_dlc < 8 ||
	    macan_crypt_dst(&cf) != ctx->config->key_server_id ||
	    macan_crypt_flags(&cf) != FL_CHALLENGE)
		return;

	/* All other checks are done in ks_receive_challenge() */
	ks_receive_challenge(ctx, &cf);
}

int macan_init_ks(struct macan_ctx *ctx, const struct macan_config *config, macan_ev_loop *loop, int sockfd,
	struct macan_key **ltks)
{
	assert(config->node_id == config->key_server_id);

	int ret = __macan_init(ctx, config, sockfd);

	macan_ev_can_init (&ctx->can_watcher, can_cb_ks, sockfd, EV_READ);
	ctx->can_watcher.data = ctx;
	macan_ev_can_start (loop, &ctx->can_watcher);

	ctx->ks.ltk = ltks;

	return ret;
}
