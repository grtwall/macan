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

#include "macan_config.h"

/* ToDo: more recipients */
struct macan_sig_spec demo_sig_spec[] = {
	[TIME_DUMMY3] = {.can_nsid = 0,   .can_sid = 0,   .src_id = TIME_SERVER, .dst_id = 3, .presc = 0},
	[TIME_DUMMY2] = {.can_nsid = 0,   .can_sid = 0,   .src_id = TIME_SERVER, .dst_id = 2, .presc = 0},
	[SIGNAL_A]    = {.can_nsid = 0,   .can_sid = 0,   .src_id = NODE1,       .dst_id = NODE2, .presc = 0}, // signal 1
	[SIGNAL_B]    = {.can_nsid = 0,   .can_sid = 202, .src_id = NODE1,       .dst_id = NODE2, .presc = 0}, // signal 2
	[SIGNAL_C]    = {.can_nsid = 103, .can_sid = 0,   .src_id = NODE1,       .dst_id = NODE2, .presc = 2}, // signal 3
	[SIGNAL_D]    = {.can_nsid = 104, .can_sid = 204, .src_id = NODE1,       .dst_id = NODE2, .presc = 5}, // signal 4
};

const uint32_t ecu2canid_map[] = {
	[KEY_SERVER] = 0,
	[TIME_SERVER] = 1,
	[NODE1] = 2,
	[NODE2] = 3,
};


#define SIG_TIME 4
#define TIME_DELTA 2000   /* tolerated time divergency from TS in usecs */
#define TIME_DIV 500000
#define TIME_TIMEOUT 5000000	/* usec */
#define SKEY_TIMEOUT 6000000000u /* usec */
#define SKEY_CHG_TIMEOUT 30000000u /* usec */
#define ACK_TIMEOUT 10000000	  /* usec */

struct macan_config config = {
	.node_id = 2,
	.ltk = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F },
	.sig_count = SIG_COUNT,
	.sigspec = demo_sig_spec,
	.node_count = NODE_COUNT,
	.ecu2canid = ecu2canid_map,
	.key_server_id = KEY_SERVER,
	.time_server_id = TIME_SERVER,
	.can_id_time = SIG_TIME,
	.time_div = TIME_DIV,
	.ack_timeout = ACK_TIMEOUT,
	.skey_validity = SKEY_TIMEOUT,
	.skey_chg_timeout = SKEY_CHG_TIMEOUT,
	.time_timeout = ACK_TIMEOUT,
	.time_delta = TIME_DELTA,
};
