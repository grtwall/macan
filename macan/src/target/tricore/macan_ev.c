/*
 *  Copyright 2014 Czech Technical University in Prague
 *
 *  Authors: Michal Sojka <sojkam1@fel.cvut.cz>
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

#include "macan_ev.h"
#include "macan_private.h"

macan_ev_loop macan_ev_loop_default;

void
macan_ev_can_init(macan_ev_can *ev,
		  void (*cb) (macan_ev_loop *loop,  macan_ev_can *w, int revents),
		  int canfd, int events)
{
	ev->cb = cb;
	ev->canfd = canfd;
}

void
macan_ev_can_start(macan_ev_loop *loop, macan_ev_can *w)
{
	w->next = loop->cans;
	loop->cans = w;
}

void
macan_ev_timer_init(macan_ev_timer *ev,
		    void (*cb) (macan_ev_loop *loop,  macan_ev_timer *w, int revents),
		    int after_ms, int repeat_ms)
{
	ev->cb = cb;
	ev->after_us = after_ms * 1000;
	ev->repeat_us = repeat_ms * 1000;
}

void
macan_ev_timer_start(macan_ev_loop *loop, macan_ev_timer *w)
{
	w->expire_us = read_time() + w->after_us;
	w->next = loop->timers;
	loop->timers = w;
}

void
macan_ev_timer_again(macan_ev_loop *loop, macan_ev_timer *w)
{
	w->expire_us = read_time() + w->repeat_us;
}

bool macan_read(struct macan_ctx *ctx, struct can_frame *cf)
{
	if (ctx->loop->cans->received) {
		*cf = *ctx->loop->cans->received;
		ctx->loop->cans->received = NULL;
		return true;
	} else
		return false;
}

void macan_ev_recv_cb(struct can_frame *cf, void *data)
{
	macan_ev_loop *loop = data;

	macan_ev_can *can = loop->cans;
	can->received = cf; /* Store the frame in the watcher, where
			     * macan_read() (presumably invoked from
			     * can->cb) looks for it. */
	can->cb(loop, can, MACAN_EV_READ);
}

bool
macan_ev_run(macan_ev_loop *loop)
{
	while (1) {
		uint64_t now = read_time();

		poll_can_fifo(macan_ev_recv_cb, loop);

		for (macan_ev_timer *t = loop->timers; t; t = t->next) {
			if (t->expire_us >= now) {
				t->cb(loop, t, MACAN_EV_TIMER);
				t->expire_us = now + t->repeat_us;
				// TODO: one shot timers
			}
		}
	}
	return true;
}
