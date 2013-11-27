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

#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>
#include <errno.h>
#include <inttypes.h>
#include "common.h"
#include "aes_keywrap.h"
#ifdef __CPU_TC1798__
#include "can_frame.h"
#include "Std_Types.h"
#include "Mcu.h"
#include "Port.h"
#include "Can.h"
#include "EcuM.h"
#include "Test_Print.h"
#include "Os.h"
#include "she.h"
#else
#include <unistd.h>
#include <net/if.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <nettle/aes.h>
#include "aes_cmac.h"
#endif /* __CPU_TC1798__ */
#include <macan.h>
#include "macan_private.h"

/**
 * Initialize communication partner.
 *
 * Allocates com_part and sets it to the default value, i.e. wait
 * for this node and its counterpart to share the key.
 */
void init_cpart(struct macan_ctx *ctx, uint8_t i)
{
	struct com_part **cpart = ctx->cpart;
	cpart[i] = malloc(sizeof(struct com_part));
	memset(cpart[i], 0, sizeof(struct com_part));
	cpart[i]->wait_for = 1 << i | 1 << ctx->config->node_id;
}

/**
 * Initialize MaCAN context.
 *
 * This function fills the macan_ctx structure. This function should be
 * called before using the MaCAN library.
 */
int macan_init(struct macan_ctx *ctx, const struct macan_config *config)
{
	int i;
	uint8_t cp;

	memset(ctx, 0, sizeof(struct macan_ctx));
	ctx->config = config;
	ctx->cpart = malloc(config->node_count * sizeof(struct com_part *));
	memset(ctx->cpart, 0, config->node_count * sizeof(struct com_part *));
	ctx->sighand = malloc(config->sig_count * sizeof(struct sig_handle *));
	memset(ctx->sighand, 0, config->sig_count * sizeof(struct sig_handle *));


	for (i = 0; i < config->sig_count; i++) {
		if (config->sigspec[i].src_id == config->node_id) {
			cp = config->sigspec[i].dst_id;

			if (ctx->cpart[cp] == NULL) {
				init_cpart(ctx, cp);
			}
		}
		if (config->sigspec[i].dst_id == config->node_id) {
			cp = config->sigspec[i].src_id;

			if (ctx->cpart[cp] == NULL) {
				init_cpart(ctx, cp);
			}
		}

		ctx->sighand[i] = malloc(sizeof(struct sig_handle));
		ctx->sighand[i]->presc = SIG_DONTSIGN;
		ctx->sighand[i]->presc_cnt = 0;
		ctx->sighand[i]->flags = 0;
		ctx->sighand[i]->cback = NULL;
	}

	return 0;
}

/**
 * Establishes authenticated channel.
 *
 * The ACK messages are broadcasted in order to create the authenticated
 * communication channel. Once there is such a channel the AUTH_REQ is
 * send.
 */
/* ToDo: reconsider name or move signal requests */
int macan_wait_for_key_acks(struct macan_ctx *ctx, int s)
{
	int i;
	int r = 0;
	uint8_t cp, presc;
	struct com_part **cpart;
	struct sig_handle **sighand;
	const struct macan_sig_spec *sigspec;

	cpart = ctx->cpart;
	sighand = ctx->sighand;
	sigspec = ctx->config->sigspec;

	if (ctx->ack_timeout_abs > read_time())
		return -1;
	ctx->ack_timeout_abs = read_time() + ctx->config->ack_timeout;

	for (i = 2; i < ctx->config->node_count; i++) {
		if (cpart[i] == NULL)
			continue;

		if (!is_skey_ready(ctx, i))
			continue;

		if (!is_channel_ready(ctx, i)) {
			r++;
			send_ack(ctx, s, i);
			continue;
		}
	}

	/* ToDo: repeat auth_req for the case the signal source will restart */
	for (i = 0; i < ctx->config->sig_count; i++) {
		if (sigspec[i].dst_id != ctx->config->node_id)
			continue;

		cp = sigspec[i].src_id;
		presc = sigspec[i].presc;

		if (!is_channel_ready(ctx, cp))
			continue;

		if (!(sighand[i]->flags & AUTHREQ_SENT)) {
			sighand[i]->flags |= AUTHREQ_SENT;
			send_auth_req(ctx, s, cp, i, presc);
		}
	}

	return r;
}

/**
 * Register a callback function.
 *
 * The callback should serve signal reception.
 *
 * @param sig_num  signal id number
 * @param fnc      pointer to the signal callback function
 */
int macan_reg_callback(struct macan_ctx *ctx, uint8_t sig_num, sig_cback fnc)
{
	ctx->sighand[sig_num]->cback = fnc;

	return 0;
}

/**
 * Sends an ACK message.
 */
void send_ack(struct macan_ctx *ctx, int s, uint8_t dst_id)
{
	struct macan_ack ack = {2, dst_id, {0}, {0}};
	uint8_t plain[8] = {0};
	uint32_t time;
	uint8_t *skey;
	struct can_frame cf = {0};
	volatile int res;
	struct com_part **cpart;

	cpart = ctx->cpart;

	if (!is_skey_ready(ctx, dst_id))
		return;

	skey = cpart[dst_id]->skey;
	memcpy(&ack.group, &cpart[dst_id]->group_field, 3);
	time = macan_get_time(ctx);

	memcpy(plain, &time, 4);
	plain[4] = ack.dst_id;
	memcpy(plain + 5, ack.group, 3);

#ifdef DEBUG_TS
	memcpy(ack.cmac, &time, 4);
#else
	sign(skey, ack.cmac, plain, sizeof(plain));
#endif

	cf.can_id = CANID(ctx, ctx->config->node_id);
	cf.can_dlc = 8;
	memcpy(cf.data, &ack, 8);

	res = write(s, &cf, sizeof(struct can_frame));
	if (res != 16) {
		perror("send ack");
	}

	return;
}

/**
 * Receives an ACK message.
 *
 * Returns 1 if the incoming ack does not contain this node in its
 * group field. Therefore, the updated ack should be broadcasted.
 */
int receive_ack(struct macan_ctx *ctx, const struct can_frame *cf)
{
	int8_t id;
	struct com_part *cp;
	struct macan_ack *ack = (struct macan_ack *)cf->data;
	uint8_t plain[8];
	uint8_t *skey;
	struct com_part **cpart;

	cpart = ctx->cpart;

	id = canid2ecuid(ctx, cf->can_id);
	assert(0 <= id && id < ctx->config->node_count);

	/* ToDo: overflow check */
	/* ToDo: what if ack contains me */
	/* ToDo: add groups communication */
	if (cpart[id] == NULL)
		return -1;

	cp = cpart[id];
	skey = cp->skey;

	plain[4] = ack->dst_id;
	memcpy(plain + 5, ack->group, 3);

#ifdef DEBUG_TS
#ifdef __CPU_TC1798__
	uint32_t cmac;
	memcpy_bw(&cmac, ack->cmac, 4);
	printf("time check: (local=%llu, in msg=%u)\n", get_macan_time(ctx) / TIME_DIV, cmac);
#else
	printf("time check: (local=%llu, in msg=%u)\n", get_macan_time(ctx) / TIME_DIV, *(uint32_t *)ack->cmac);
#endif /* __CPU_TC1798__ */
#endif

	/* ToDo: make difference between wrong CMAC and not having the key */
	if (!check_cmac(ctx, skey, ack->cmac, plain, plain, sizeof(plain))) {
		printf("error: ACK CMAC failed\n");
		return -1;
	}

	uint32_t is_present = 1 << ctx->config->node_id;
	uint32_t ack_group = 0;
	memcpy(&ack_group, ack->group, 3);

	is_present &= ack_group;
	cp->group_field |= ack_group;

	if (is_present)
		return 0;

	return 1;
}

void gen_challenge(uint8_t *chal)
{
	int i;

	for (i = 0; i < 6; i++)
		chal[i] = rand();
}

/**
 * Receive a session key.
 *
 * Processes one frame of the session key transmission protocol.
 * Returns node id if a complete key was sucessfully received.
 *
 * @return -1 if key receive failed. ID of the node the key is shared
 * with. Or 0 if the reception process is in progress.
 */
int receive_skey(struct macan_ctx *ctx, const struct can_frame *cf)
{
	struct macan_sess_key *sk;
	uint8_t fwd_id;
	static uint8_t keywrap[32];
	uint8_t skey[24];
	uint8_t seq, len;
	struct com_part **cpart;

	cpart = ctx->cpart;
	sk = (struct macan_sess_key *)cf->data;
	seq = sk->seq;
	len = sk->len;

	assert(0 <= seq && seq <= 5);
	assert((seq != 5 && len == 6) || (seq == 5 && len == 2));

	memcpy(keywrap + 6 * seq, sk->data, sk->len);

	if (seq == 5) {
		print_hexn(keywrap, 32);
		unwrap_key(ctx->config->ltk, 32, skey, keywrap);
		print_hexn(skey, 24);

		fwd_id = skey[6];
		if (fwd_id <= 0 || ctx->config->node_count <= fwd_id || cpart[fwd_id] == NULL) {
			printf("receive session key \033[1;31mFAIL\033[0;0m: unexpected fwd_id\n");
			return -1;
		}

		if(!memchk(skey, cpart[fwd_id]->chg, 6)) {
			printf("receive session key \033[1;31mFAIL\033[0;0m: check cmac\n");
			return -1;
		}

		cpart[fwd_id]->valid_until = read_time() + ctx->config->skey_validity;
		memcpy(cpart[fwd_id]->skey, skey + 8, 16);
		cpart[fwd_id]->group_field |= 1 << ctx->config->node_id;
		printf("receive session key (%d->%"PRIu8") \033[1;32mOK\033[0;0m\n", ctx->config->node_id, fwd_id);

		return fwd_id;
	}

	return 0;
}

/**
 * send_challenge() - requests key from KS or signed time from TS
 * @s:      socket fd
 * @dst_id: destination node id (e.g. KS)
 * @fwd_id: id of a node I wish to share a key with
 *          (i.e. communication partner)
 * @chg:    challenge (a 6-byte nonce) will be generated and written
 *          here
 *
 * This function sends the CHALLENGE message to the socket s. It is
 * used to request a key from KS or to request a signed time from TS.
 */
void send_challenge(struct macan_ctx *ctx, int s, uint8_t dst_id, uint8_t fwd_id, uint8_t *chg)
{
	struct can_frame cf = {0};
	struct macan_challenge chal = {1, dst_id, fwd_id, {0}};

	if (chg) {
		gen_challenge(chg);
		memcpy(chal.chg, chg, 6);
	}

	cf.can_id = CANID(ctx, ctx->config->node_id);
	cf.can_dlc = 8;
	memcpy(cf.data, &chal, sizeof(struct macan_challenge));
	write(s, &cf, sizeof(struct can_frame));
}

/**
 * receive_challenge() - responds to REQ_CHALLENGE from KS
 */
void receive_challenge(struct macan_ctx *ctx, int s, const struct can_frame *cf)
{
	uint8_t fwd_id;
	struct macan_challenge *ch = (struct macan_challenge *)cf->data;
	struct com_part **cpart;

	cpart = ctx->cpart;
	fwd_id = ch->fwd_id;
	assert(0 < fwd_id && fwd_id < ctx->config->node_count);

	if (cpart[fwd_id] == NULL) {
		cpart[fwd_id] = malloc(sizeof(struct com_part));
		memset(cpart[fwd_id], 0, sizeof(struct com_part));

		cpart[fwd_id]->wait_for = 1 << fwd_id | 1 << ctx->config->node_id;
	}

	cpart[fwd_id]->valid_until = read_time() + ctx->config->skey_chg_timeout;
	send_challenge(ctx, s, ctx->config->key_server_id, ch->fwd_id, cpart[ch->fwd_id]->chg);
}

/**
 * Receive unsigned time.
 *
 * Receives time and checks whether local clock is in sync. If local clock
 * is out of sync, it sends a request for signed time.
 */
void receive_time(struct macan_ctx *ctx, int s, const struct can_frame *cf)
{
	uint32_t time_ts;
	uint64_t recent;

	if (!is_skey_ready(ctx, ctx->config->time_server_id))
		return;

	memcpy(&time_ts, cf->data, 4);
	recent = read_time() + ctx->time.offs;

	if (ctx->time.chal_ts) {
		if ((recent - ctx->time.chal_ts) < ctx->config->time_timeout)
			return;
	}

	printf("time received = %u\n", time_ts);

	if (abs(recent - time_ts) > ctx->config->time_delta) {
		printf("error: time out of sync (%"PRIu64" = %"PRIu64" - %"PRIu32")\n", (uint64_t)abs(recent - time_ts), recent, time_ts);

		ctx->time.chal_ts = recent;
		send_challenge(ctx, s, ctx->config->time_server_id, 0, ctx->time.chg);
	}
}

/**
 * Receive signed time.
 *
 * Receives time and sets local clock according to it.
 */
void receive_signed_time(struct macan_ctx *ctx, int s, const struct can_frame *cf)
{
	uint32_t time_ts;
	uint8_t plain[10];
	uint8_t *skey;
	struct com_part **cpart;

	cpart = ctx->cpart;

	memcpy(&time_ts, cf->data, 4);
	printf("signed time received = %u\n", time_ts);

	if (!is_skey_ready(ctx, ctx->config->time_server_id)) {
		ctx->time.chal_ts = 0;
		return;
	}

	skey = cpart[ctx->config->time_server_id]->skey;

	memcpy(plain, ctx->time.chg, 6);
	memcpy(plain + 6, &time_ts, 4);
	if (!check_cmac(ctx, skey, cf->data + 4, plain, NULL, sizeof(plain))) {
		printf("CMAC \033[1;31merror\033[0;0m\n");
		return;
	}
	printf("CMAC OK\n");

	ctx->time.offs += (time_ts - ctx->time.chal_ts);
	ctx->time.chal_ts = 0;
}

/**
 * Send authorization request.
 *
 * Requests an authorized signal from communication partner.
 *
 * @param dst_id   com. partner id, the signal source
 * @param sig_num  signal id
 * @param prescaler
 */
void send_auth_req(struct macan_ctx *ctx, int s, uint8_t dst_id, uint8_t sig_num, uint8_t prescaler)
{
	uint64_t t;
	uint8_t plain[8];
	uint8_t *skey;
	struct can_frame cf = {0};
	struct macan_sig_auth_req areq;
	struct com_part **cpart;

	cpart = ctx->cpart;
	t = macan_get_time(ctx);
	skey = cpart[dst_id]->skey;

	memcpy(plain, &t, 4);
	plain[4] = ctx->config->node_id;
	plain[5] = dst_id;
	plain[6] = sig_num;
	plain[7] = prescaler;

	areq.flags = 3;
	areq.dst_id = dst_id;
	areq.sig_num = sig_num;
	areq.prescaler = prescaler;
	sign(skey, areq.cmac, plain, sizeof(plain));

	cf.can_id = CANID(ctx, ctx->config->node_id);
	cf.can_dlc = 7;
	memcpy(&cf.data, &areq, 7);

	write(s, &cf, sizeof(cf));
}

void receive_auth_req(struct macan_ctx *ctx, const struct can_frame *cf)
{
	uint8_t *skey;
	uint8_t plain[8];
	uint8_t sig_num;
    int can_sid,can_nsid;
	struct macan_sig_auth_req *areq;
	struct com_part **cpart;
	struct sig_handle **sighand;

	cpart = ctx->cpart;
	sighand = ctx->sighand;

	int8_t ecuid = canid2ecuid(ctx, cf->can_id);
	assert(ecuid >= 0);

	if (cpart[ecuid] == NULL)
		return;

	areq = (struct macan_sig_auth_req *)cf->data;
	skey = cpart[ecuid]->skey;

	plain[4] = ecuid;
	plain[5] = ctx->config->node_id;
	plain[6] = areq->sig_num;
	plain[7] = areq->prescaler;

	if (!check_cmac(ctx, skey, areq->cmac, plain, plain, sizeof(plain))) {
		printf("error: sig_auth cmac is incorrect\n");
	}

	sig_num = areq->sig_num;
    
    can_sid = ctx->config->sigspec[sig_num].can_sid;
    can_nsid = ctx->config->sigspec[sig_num].can_nsid;
    
    if((can_nsid == 0 && can_sid == 0) ||
       (can_nsid == 0 && can_sid != 0)) {
        // ignore prescaler
        sighand[sig_num]->presc = 1;
        sighand[sig_num]->presc_cnt = 0;
    } else {
        /* ToDo: assert sig_num range */
        sighand[sig_num]->presc = areq->prescaler;
        sighand[sig_num]->presc_cnt = areq->prescaler - 1;
    }

}

/**
 * Send MaCAN signal frame.
 *
 * Signs signal using CMAC and transmits it.
 */
int macan_write(struct macan_ctx *ctx, int s, uint8_t dst_id, uint8_t sig_num, uint32_t signal)
{
	struct can_frame cf = {0};
	uint8_t plain[10],sig[8];
    int plain_length;
	uint32_t t;
	uint8_t *skey;
    uint8_t *cmac;
	struct com_part **cpart;

	cpart = ctx->cpart;

	if (!is_channel_ready(ctx, dst_id))
		return -1;

	skey = cpart[dst_id]->skey;
	t = macan_get_time(ctx);

	memcpy(plain, &t, 4);
	plain[4] = ctx->config->node_id;
	plain[5] = dst_id;

    if(is_32bit_signal(ctx,sig_num)) {
        struct macan_signal *sig32 = (struct macan_signal *) sig;
        memcpy(plain + 6, &signal, 4);
        memcpy(sig32->sig, &signal, 4);
        cf.can_id = ctx->config->sigspec[sig_num].can_sid;
        plain_length = 10;
        cmac = sig32->cmac;
    } else {
        struct macan_signal_ex *sig16 = (struct macan_signal_ex *) sig;
        sig16->flags = 3;
        sig16->dst_id = dst_id;
        sig16->sig_num = sig_num;
        memcpy(plain + 6, &signal, 2);
        memcpy(&sig16->signal, &signal, 2);
        cf.can_id = CANID(ctx, ctx->config->node_id);
        plain_length = 8;
        cmac = sig16->cmac;
    }


#ifdef DEBUG_TS
	memcpy(cmac, &time, 4);
#else
	sign(skey, cmac, plain, plain_length);
#endif

	cf.can_dlc = 8;
	memcpy(&cf.data, &sig, 8);

	/* ToDo: assure success */
	write(s, &cf, sizeof(cf));

	return 0;
}

/**
 * Dispatch a signal.
 *
 * The prescaler settings are considered and the signal is send.
 *
 * @param s        socket handle
 * @param sig_num  signal id
 * @param signal   signal value
 */
/* ToDo: return result */
void macan_send_sig(struct macan_ctx *ctx, int s, uint8_t sig_num, uint32_t signal)
{
	uint8_t dst_id;
	struct sig_handle **sighand;
	const struct macan_sig_spec *sigspec;

	sighand = ctx->sighand;
	sigspec = ctx->config->sigspec;

	dst_id = sigspec[sig_num].dst_id;
	if (!is_channel_ready(ctx, dst_id)) {
		printf("Channel not ready\n"); /* FIXME: return error instead of printing this */
		return;
	}

	switch (sighand[sig_num]->presc) {
	case SIG_DONTSIGN:
		break;
	case SIG_SIGNONCE:
		macan_write(ctx, s, dst_id, sig_num, signal);
		sighand[sig_num]->presc = SIG_DONTSIGN;
		break;
	default:
		if (sighand[sig_num]->presc_cnt > 0) {
			sighand[sig_num]->presc_cnt--;
		} else {
			macan_write(ctx, s, dst_id, sig_num, signal);
			sighand[sig_num]->presc_cnt = sighand[sig_num]->presc - 1;
		}
		break;
	}
}

/**
 * Receive signal.
 *
 * Receives signal, checks its CMAC and calls appropriate callback.
 */
/* ToDo: receive signal frame with 32bits */
void receive_sig(struct macan_ctx *ctx, const struct can_frame *cf, int sig32_num)
{
	uint8_t plain[8];
    uint8_t sig_num;
	uint32_t sig_val = 0;
	uint8_t *skey;
    uint8_t *cmac;
	struct com_part **cpart;
	struct sig_handle **sighand;
    int plain_length;
	int8_t ecuid = canid2ecuid(ctx, cf->can_id);
	assert(ecuid >= 0);

	cpart = ctx->cpart;
	sighand = ctx->sighand;

    
    if(sig32_num >= 0) {
        // we have received 32 bit signal
        struct macan_signal *sig32 = (struct macan_signal *)cf->data;
        sig_num = sig32_num;
        plain[4] = ctx->config->sigspec[sig_num].src_id;
        memcpy(plain + 6, sig32->sig, 4);
        skey = cpart[ctx->config->sigspec[sig_num].src_id]->skey;
        plain_length = 10;
        cmac = sig32->cmac;
        memcpy(&sig_val, sig32->sig, 4);
        
    } else {
        // we have received 16 bit signal
	    struct macan_signal_ex *sig16 = (struct macan_signal_ex *)cf->data;
	    plain[4] = ecuid;
	    memcpy(plain + 6, sig16->signal, 2);
        skey = cpart[ecuid]->skey;
        plain_length = 8;
        cmac = sig16->cmac;
        sig_num = sig16->sig_num;
        memcpy(&sig_val, sig16->signal, 2);
    }

	plain[5] = ctx->config->node_id;


#ifdef DEBUG_TS
	printf("receive_sig: (local=%d, in msg=%d)\n", get_macan_time(ctx) / TIME_DIV, *(uint32_t *)sig->cmac);
#endif
	if (!check_cmac(ctx, skey, cmac, plain, plain, plain_length)) {
		printf("signal CMAC \033[1;31merror\033[0;0m\n");
		return;
	}

	if (sighand[sig_num]->cback)
		sighand[sig_num]->cback(sig_num, sig_val);
}

/**
 * Check if has the session key.
 *
 * @param dst_id  id of a node to whom check if has the key
 * @return        1 if has the key, otherwise 0
 */
int is_skey_ready(struct macan_ctx *ctx, uint8_t dst_id)
{
	if (ctx->cpart[dst_id] == NULL)
		return 0;

	return (ctx->cpart[dst_id]->group_field & 1 << ctx->config->node_id);
}

/**
 * Check if has the authorized channel.
 *
 * Checks if has the session key and if the communication partner
 * has acknowledged the communication with an ACK message.
 */
int is_channel_ready(struct macan_ctx *ctx, uint8_t dst)
{
	struct com_part **cpart;

	cpart = ctx->cpart;

	if (cpart[dst] == NULL)
		return 0;

	uint32_t grp = (*((uint32_t *)&cpart[dst]->group_field)) & 0x00ffffff;
	uint32_t wf = (*((uint32_t *)&cpart[dst]->wait_for)) & 0x00ffffff;

	return ((grp & wf) == wf);
}

/**
 * Request keys to all comunication partners.
 *
 * Requests keys and repeats until success.
 */
void macan_request_keys(struct macan_ctx *ctx, int s)
{
	int i;
	struct com_part **cpart;

	cpart = ctx->cpart;

	for (i = 0; i < ctx->config->node_count; i++) {
		if (cpart[i] == NULL)
			continue;

		if (cpart[i]->valid_until > read_time())
			continue;

		send_challenge(ctx, s, ctx->config->key_server_id, i, cpart[i]->chg);
		cpart[i]->valid_until = read_time() + ctx->config->skey_chg_timeout;
	}

	return;
}

/**
 * Get time of the MaCAN protocol.
 */
uint64_t macan_get_time(struct macan_ctx *ctx)
{
	return (read_time() + ctx->time.offs) / ctx->config->time_div;
}

/**
 * Process can frame.
 *
 * This function should be called for incoming can frames. It extracts MaCAN messages
 * and operates the MaCAN library.
 */
int macan_process_frame(struct macan_ctx *ctx, int s, const struct can_frame *cf)
{
    
    // test if can_id is can_sid of any signal
    int sig32_num = can_sid_to_sig_num(ctx,cf->can_id);
    if(sig32_num >= 0) {
        // received frame is 32bit signal frame
        receive_sig(ctx,cf,sig32_num);    
        return 1;
    }

	struct macan_crypt_frame *cryf = (struct macan_crypt_frame *)cf->data;
	int fwd;

	/* ToDo: make sure all branches end ASAP */
	/* ToDo: macan or plain can */
	/* ToDo: crypto frame or else */
	if(cf->can_id == CANID(ctx, ctx->config->node_id))
		return 1;
	if (cf->can_id == ctx->config->can_id_time) {
		switch(cf->can_dlc) {
		case 4:
			receive_time(ctx, s, cf);
			return 1;
		case 8:
			receive_signed_time(ctx, s, cf);
			return 1;
		}
	}

	if (cryf->dst_id != ctx->config->node_id)
		return 1;

	switch (cryf->flags) {
	case 1:
		receive_challenge(ctx, s, cf);
		break;
	case 2:
		if (cf->can_id == CANID(ctx, ctx->config->key_server_id)) {
			fwd = receive_skey(ctx, cf);
			if (fwd > 1) {
				send_ack(ctx, s, fwd);
			}
			break;
		}

		/* ToDo: what if ack CMAC fails, there should be no response */
		if (receive_ack(ctx, cf) == 1)
			send_ack(ctx, s, canid2ecuid(ctx, cf->can_id));
		break;
	case 3:
		if (cf->can_dlc == 7)
			receive_auth_req(ctx, cf);
		else
			receive_sig(ctx, cf, -1);
		break;
	}

	return 0;
}

int is_32bit_signal(struct macan_ctx *ctx, uint8_t sig_num) {
    int can_nsid, can_sid;

    can_nsid = ctx->config->sigspec[sig_num].can_nsid;
    can_sid = ctx->config->sigspec[sig_num].can_sid;

    if((can_nsid == 0 && can_sid != 0) ||
       (can_nsid != 0 && can_sid != 0)) {
        return 1;
    }
    return 0;
}
int can_sid_to_sig_num(struct macan_ctx *ctx, uint8_t can_id) {
    int i;
   
    if(can_id == 0)
       return -1; 

    for(i = 0; i < ctx->config->sig_count; i++) {
       if(ctx->config->sigspec[i].can_sid == can_id) {
           return i;
       }
    } 
    return -1;

}

