/**
 * @copyright Copyright (c) 2022, Jinyao Liu, 
 *
 * An implementation of the PCC (NSDI'18) Vivace algorithm. 
 */

#ifndef _XQC_PCC_H_INCLUDED_
#define _XQC_PCC_H_INCLUDED_

#include <xquic/xquic.h>
#include <xquic/xquic_typedef.h>
#include "src/congestion_control/xqc_window_filter.h"

typedef char bool;
#define USEC_PER_SEC	1000000L
#define USEC_PER_MSEC	1000L

#define u64             uint64_t
#define s64             int64_t
#define U64_MAX		    ((u64)~0ULL)
#define S64_MAX		    ((s64)(U64_MAX >> 1))
#define S64_MIN		    ((s64)(-S64_MAX - 1))
/* pcc micro experiment times */
#define PCC_INTERVALS 4

typedef enum {
    PCC_STARTUO = 0,
    PCC_PROBE = 1,
    PCC_MOVING = 2,
} xqc_pcc_state_t;

typedef enum {
    PCC_RATE_UP = 0,
    PCC_RATE_DOWN = 1,
    PCC_RATE_STAY = 2,
} xqc_pcc_decision_t;

/* micro-experiment, monitor interval */
typedef struct xqc_pcc_interval_s {
    uint64_t    rate;   /* send rate of this interval */

    xqc_usec_t  recv_start; /* timestamps for when interval was waiting for acks */
	xqc_usec_t  recv_end;

	xqc_usec_t  send_start; /* timestamps for when interval data was being sent */
	xqc_usec_t  send_end;

	uint32_t    start_rtt; /* smoothed RTT at start and end of this interval */
	uint32_t    end_rtt;

    /* linux kernel version pcc use tcp_sock.data_segs_out, total number of data segments sent, not bytes sent */
	unsigned    packets_sent_base; /* here use send_ctl.ctl_send_count, packets sent before this interval started */
	unsigned    packets_ended;

	int64_t    utility; /* observed utility of this interval */
	uint32_t    lost; /* packets sent during this interval that were lost */
	uint32_t    delivered; /* packets sent during this interval that were delivered */
} xqc_pcc_interval_t;

typedef struct xqc_pcc_s {
    /*
     * Origin infomations similar to kernel version pcc 
     */
    xqc_pcc_interval_t  intervals[PCC_INTERVALS]; // kernel version kzalloc PCC_INTERVALS*2 space, why? ask njay
    uint8_t     send_index; /* index of interval currently being sent */
    uint8_t     recive_index; /* index of interval currently receiving acks */

    uint64_t    rate; /* current send rate */
	uint64_t    last_rate; /* previous sending rate */

    /* utility function pointer (can be loss- or latency-based) */
	void (*util_func)(struct xqc_pcc_s *, struct xqc_pcc_interval_s *);

    bool        start_mode; /* in slow start? */
    bool        moving; /* using gradient ascent to move to a new rate? */
    bool        loss_state; /* is loss? */
    bool        wait; /* wait to start next interal */

    xqc_pcc_decision_t  last_decision; /* last rate change decision */

    uint32_t    lost_base; /* previously lost packets */
	uint32_t    delivered_base; /* previously delivered packets */

    uint32_t    decisions_count; /* record times of this direction */

    uint32_t    packets_counted; /* kernel version = sk->delivered + sk->lost - spare */
    uint32_t    spare; /* i dont kown this val's mearning , kernel version has a set_state interface */

    uint32_t    amplifier; /* multiplier on the current step size */
    uint32_t    swing_buffer; /* number of RTTs left before step size can grow */
	uint32_t    change_bound; /* maximum change as a proportion of the current rate */

    /*
     * new fields added for xquic 
     */
    uint64_t    cwnd_bytes; /* current cwnd */
    /* update these infomation from on_ack(sampler) by sample->send_ctl */
    uint32_t    srtt_us; /* crrent srtt : use xquic sampler's srtt instead */
    unsigned    packets_sent_count; /* sent packets num, update every on_sent*/
    uint32_t    lost; /* sampler->send_ctl->ctl_lost_count */
    uint32_t    delivered; /* sampler->send_ctl->ctl_recv_count */
    xqc_send_ctl_s *send_ctl;
} xqc_pcc_t;

extern const xqc_cong_ctrl_callback_t xqc_pcc_cb;

#endif


/* 
https://github.com/PCCProject/PCC-Kernel

All other files are dual licensed under BSD/GPLv2.
Copyright 2018 Hebrew University and University of Illinois at Urbana-Champaign

GPL License summary:
Copyright (C) 2018 Hebrew University and University of Illinois at Urbana-Champaign

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.


BSD License summary:
Copyright (c) 2018 Hebrew University and University of Illinois at Urbana-Champaign, All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

    Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
    Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
    Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission. 

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */