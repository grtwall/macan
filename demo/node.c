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
#ifdef TC1798
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
#endif /* TC1798 */
#include "helper.h"
#include "macan.h"
#include "macan_config.h"

/* ToDo
 *   implement groups
 *   some error processing
 */

#ifdef TC1798
# define NODE_ID 3
#endif

#define WRITE_DELAY 0.5
#define NODE_KS 0
#define NODE_TS 1
#ifndef NODE_ID
# error NODE_TS not defined
#endif /* NODE_ID */

uint8_t recv_skey_pending = 0;
uint8_t g_fwd = 0;
#define NODE_HAS_KEY 1

/* ltk stands for long term key; it is a key shared with the key server */
uint8_t ltk[] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
  	0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
};

void can_recv_cb(int s, struct can_frame *cf)
{
	macan_process_frame(s, cf);
}

void operate_ecu(int s)
{
	uint64_t signal_time = read_time();
	uint64_t ack_time = read_time();

	while(1) {
#ifdef TC1798
		poll_can_fifo();
#else
		helper_read_can(s, can_recv_cb);
#endif /* TC1798 */
		macan_request_keys(s);
		macan_wait_for_key_acks(s, demo_sig_spec, &ack_time);

		if (signal_time + 1000000 < read_time()) {
			signal_time = read_time();
			macan_send_sig(s, ENGINE, demo_sig_spec, 55);
			macan_send_sig(s, BRAKE, demo_sig_spec, 66);
		}

		//send_auth_req(s, NODE_OTHER, ENGINE, 0);
#ifndef TC1798
		usleep(250);
#endif /* TC1798 */
	}
}

void sig_callback(uint8_t sig_num, uint32_t sig_val)
{
	printf("received authorized signal(%"PRIu8") = %"PRIu32"\n", sig_num, sig_val);
}

int main(int argc, char *argv[])
{
	int s;

	s = helper_init();
	macan_init(s, demo_sig_spec);
	macan_set_ltk(ltk);
	macan_reg_callback(ENGINE, sig_callback);
	macan_reg_callback(BRAKE, sig_callback);
	operate_ecu(s);

	return 0;
}

