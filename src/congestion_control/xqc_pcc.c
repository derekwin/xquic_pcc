/**
 * @copyright Copyright (c) 2022, Jinyao Liu(JaceLau), 
 *
 * An implementation of the PCC (NSDI'18) Vivace algorithm. 
 */

/**
 * Performance-oriented Congestion Control (PCC)
 *
 * PCC is rate-based congestion control algorithm that chooses its sending rate
 * based on an explicit utility function computed over rtt-length intervals.
 *
 *
 * After slow start, on each call to cong_control, PCC:
 *	 - Adds new acks to the current monitor interval.
 *	 - If the monitor interval is finished, computes a new rate based on the
 *		 interval's rate and observed utility.
 *
 * PCC begins in a slow start state, doubling the sending rate each RTT until
 * it observes a decrease in utility.
 *
 * After slow start, PCC repeatedly computes the gradient of utility w.r.t.
 * sending rate and changes sending rate toward the direction of greater
 * utility.
 *
 * For more information on PCC, see:
 *	 "PCC Vivace: Online-Learning Congestion Control"
 *	 Mo Dong, Tong Meng, Doron Zarchy, Engin Arslan, Yossi Gilad,
 *	 Brighten Godfrey and Michael Schapria, NSDI '18, Apr, 2018
 *
 */

#include <stdlib.h>
#include <time.h>
#include "src/congestion_control/xqc_pcc.h"
#include <xquic/xquic.h>
#include <xquic/xquic_typedef.h>
#include "src/congestion_control/xqc_sample.h"
#include "src/common/xqc_time.h"
#include "src/transport/xqc_packet.h"


#define XQC_PCC_MAX_DATAGRAMSIZE    XQC_MSS
#define XQC_PCC_MIN_WINDOW          (4 * XQC_PCC_MAX_DATAGRAMSIZE)
#define XQC_PCC_INITIAL_WINDOW  	(32 * XQC_PCC_MAX_DATAGRAMSIZE) 

/* Probing changes rate by 5% up and down of current rate. */
#define PCC_PROBING_EPS 			5
#define PCC_PROBING_EPS_PART 		100
/* scale for fractions, utilities, gradients, ... */
#define PCC_SCALE 					1000 
#define PCC_RATE_MIN 				1024u
#define PCC_RATE_MIN_PACKETS_PER_RTT 2
#define PCC_INVALID_INTERVAL 		-1
#define PCC_IGNORE_PACKETS 			10
#define PCC_INTERVAL_MIN_PACKETS 	50
#define PCC_ALPHA 					100
/* defaults step size for gradient ascent */
#define PCC_GRAD_STEP_SIZE 			25
/* number of RTTs to dampen gradient ascent */
#define PCC_MAX_SWING_BUFFER 		2
/* latency inflation below 3% is ignored */
#define PCC_LAT_INFL_FILTER 		30

/* Rates must differ by at least 2% or gradients are very noisy. */
#define PCC_MIN_RATE_DIFF_RATIO_FOR_GRAD 20
/* first rate change is at most 10% of rate */
#define PCC_MIN_CHANGE_BOUND 		100
/* consecutive rate changes can go up by 7% */ 
#define PCC_CHANGE_BOUND_STEP 		70
#define PCC_AMP_MIN					2


/* keep cwnd_bytes don't overflow */
const uint64_t max_snd_cwnd_bytes = ~0;
const uint64_t max_send_rate =~0;


/**
 * untility function
 */

#define PCC_LOSS_MARGIN 5
#define PCC_MAX_LOSS 10
/* get x = number * PCC_SCALE, return (e^number)*PCC_SCALE */
static uint32_t
pcc_exp(int32_t x)
{
	int64_ttemp = PCC_SCALE;
	int64_te = PCC_SCALE;
	int i;

	for (i = 1; temp != 0; i++) {
		temp *= x;
		temp /= i;
		temp /= PCC_SCALE;
		e += temp;
	}
	return e;
}

/* Calculate the graident of utility w.r.t. sending rate, but only if the rates
 * are far enough apart for the measurment to have low noise.
 */
static uint64_t
pcc_calc_util_grad(uint64_t rate_1, uint64_t util_1, uint64_t rate_2, uint64_t util_2) {
	uint64_t rate_diff_ratio = (PCC_SCALE * (rate_2 - rate_1)) / rate_1;
	if (rate_diff_ratio < PCC_MIN_RATE_DIFF_RATIO_FOR_GRAD && 
		rate_diff_ratio > -1 * PCC_MIN_RATE_DIFF_RATIO_FOR_GRAD)s
		return 0;

	return (PCC_SCALE * PCC_SCALE * (util_2 - util_1)) / (rate_2 - rate_1);
}

void
pcc_calc_utility_vivace(xqc_pcc_t *pcc)
{
	int64_t loss_ratio, delivered, lost, mss, rate, throughput, util;
	int64_t lat_infl = 0;
    int64_t rtt_diff;
    int64_t rtt_diff_thresh = 0;
	int64_t send_dur = interval->send_end - interval->send_start;
	int64_t recv_dur = interval->recv_end - interval->recv_start;

	lost = interval->lost;
	delivered = interval->delivered;
	// mss = tcp_sk(sk)->mss_cache;
	mss = xqc_conn_get_mss(pcc->send_ctl->ctl_conn); /* calculate MSS according to settings : commit 697401ff2630dfcaf4c19ecf70e5c8e3e0f06f64 */
	rate = interval->rate;
	throughput = 0;
	if (recv_dur > 0)
		throughput = (USEC_PER_SEC * delivered * mss) / recv_dur;
	if (delivered == 0) {
        printk(KERN_INFO "No packets delivered\n");
		//interval->utility = S64_MIN;
		interval->utility = 0;
		return;
	}

	rtt_diff = interval->end_rtt - interval->start_rtt;
    if (throughput > 0)
	    rtt_diff_thresh = (2 * USEC_PER_SEC * mss) / throughput;
	if (send_dur > 0)
		lat_infl = (PCC_SCALE * rtt_diff) / send_dur;
	
	printk(KERN_INFO
		"ucalc: lat (%lld->%lld) lat_infl %lld\n",
		 interval->start_rtt / USEC_PER_MSEC, interval->end_rtt / USEC_PER_MSEC,
		 lat_infl);

	if (rtt_diff < rtt_diff_thresh && rtt_diff > -1 * rtt_diff_thresh)
		lat_infl = 0;

	if (lat_infl < PCC_LAT_INFL_FILTER && lat_infl > -1 * PCC_LAT_INFL_FILTER)
		lat_infl = 0;
	
	if (lat_infl < 0 && pcc->start_mode)
		lat_infl = 0;

	/* loss rate = lost packets / all packets counted*/
	loss_ratio = (lost * PCC_SCALE) / (lost + delivered);

    if (pcc->start_mode && loss_ratio < 100)
        loss_ratio = 0;

	util = /* int_sqrt((u64)rate)*/ rate - (rate * (900 * lat_infl + 11 * loss_ratio)) / PCC_SCALE;

	printk(KERN_INFO
		"%d ucalc: rate %lld sent %u delv %lld lost %lld lat (%lld->%lld) util %lld rate %lld thpt %lld\n",
		 pcc->id, rate, interval->packets_ended - interval->packets_sent_base,
		 delivered, lost, interval->start_rtt / USEC_PER_MSEC, interval->end_rtt / USEC_PER_MSEC, util, rate, throughput);
	interval->utility = util;

}

void
pcc_calc_utility_allgero(xqc_pcc_t *pcc)
{
	int64_t loss_ratio, delivered, lost, mss, rate, throughput, util;

	lost = interval->lost;
	delivered = interval->delivered;
	// mss = tcp_sk(sk)->mss_cache;
	mss = xqc_conn_get_mss(pcc->send_ctl->ctl_conn); /* calculate MSS according to settings : commit 697401ff2630dfcaf4c19ecf70e5c8e3e0f06f64 */
	rate = interval->rate;
	throughput = 0;
	if (interval->recv_start < interval->recv_end)
		throughput = (USEC_PER_SEC * delivered * mss) / (interval->recv_end - interval->recv_start);
	if (!(lost+ delivered)) {
		interval->utility = S64_MIN;
		return;
	}

	/* loss rate = lost packets / all packets counted *100 * FACTOR_1 */
	loss_ratio = (lost * PCC_SCALE * PCC_ALPHA) / (lost+ delivered);
	
	/* util = delivered rate / (1 + e^(100*loss_rate)) - lost_ratio * rate
	 */
	util = loss_ratio- (PCC_LOSS_MARGIN * PCC_SCALE);
	if (util < PCC_MAX_LOSS*PCC_SCALE)
		util = (throughput /* rate */ * PCC_SCALE) / (pcc_exp(util) + PCC_SCALE);
	else
		util = 0;

	/* util *= goodput */
	util *= (PCC_SCALE * PCC_ALPHA) - loss_ratio;
	util /= PCC_SCALE * PCC_ALPHA;
	/* util -= "wasted rate" */
	util -= (rate * loss_ratio) / (PCC_ALPHA * PCC_SCALE);

	printk(KERN_INFO
		"rate %lld sent %u delv %lld lost %lld util %lld\n",
		 rate, interval->packets_ended - interval->packets_sent_base,
		 delivered, lost, util);
	interval->utility = util;
}


/**
 * setter getter
 */

static uint32_t
pcc_get_rtt(xqc_pcc_t *pcc)
{
	/* Get initial RTT - as measured by SYN -> SYN-ACK.
	 * If information does not exist - use 1ms as a "LAN RTT".
	 * (originally from BBR).
	 */
	if (pcc->srtt_us) {
		return max(pcc->srtt_us >> 3, 1U); /* why? email ask njay */
	} else {
		return USEC_PER_MSEC;
	}
}

/* was the pcc struct fully inited */
bool
pcc_valid(xqc_pcc_t *pcc)
{
	return (pcc && pcc->intervals && pcc->intervals[0].rate);
}

/* xquic's cwnd is cwnd_bytes */
static void
pcc_set_cwnd(xqc_pcc_t *pcc)
{
		uint64_t cwnd = pcc->rate;
		cwnd *= pcc_get_rtt(pcc);
	
		cwnd /= USEC_PER_SEC;
		cwnd *= 2;	/* why? bigger cwnd for safe tranmit? */

		cwnd = xqc_clamp(cwnd, XQC_PCC_MIN_WINDOW, max_snd_cwnd_bytes);
		pcc->cwnd_bytes = cwnd;
}


/**
 * intervals related functions 
 */

/* Set the target rates of all intervals and reset statistics. */
static void pcc_setup_intervals_probing(xqc_pcc_t *pcc)
{
	uint64_t rate_low, rate_high;
	char rand_num;
	int i;
	time_t t;

	srand((unsigned) time(&t));
	rand_num = rand()%10;
	rate_high = pcc->rate * (PCC_PROBING_EPS_PART + PCC_PROBING_EPS);
	rate_low = pcc->rate * (PCC_PROBING_EPS_PART - PCC_PROBING_EPS);

	rate_high /= PCC_PROBING_EPS_PART;
	rate_low /= PCC_PROBING_EPS_PART;

	/* random make 4 micro experiments */
	for (i = 0; i < PCC_INTERVALS; i += 2) {
		if ((rand_num >> (i / 2)) & 1) {
			pcc->intervals[i].rate = rate_low;
			pcc->intervals[i + 1].rate = rate_high;
		} else {
			pcc->intervals[i].rate = rate_high;
			pcc->intervals[i + 1].rate = rate_low;
		}

		pcc->intervals[i].packets_sent_base = 0;
		pcc->intervals[i + 1].packets_sent_base = 0;
	}

	pcc->send_index = 0;
	pcc->recive_index = 0;
	pcc->wait = XQC_FALSE;
}

/* Set the pacing rate and cwnd base on the currently-sending interval */
static void
start_interval(xqc_pcc_t *pcc)
{
	uint64_t rate = pcc->rate;
	xqc_pcc_interval_t *interval;

	if (!pcc->wait) {
		interval = &pcc->intervals[pcc->send_index];
		interval->packets_ended = 0;
		interval->lost = 0;
		interval->delivered = 0;
		interval->packets_sent_base = max(pcc->packets_sent_count, 1U);
		interval->send_start = xqc_monotonic_timestamp();
		rate = interval->rate;
	}

	rate = xqc_clamp(rate, PCC_RATE_MIN, max_send_rate);
	pcc->rate = rate;
	pcc_set_cwnd(pcc);
}

/**
 * PCC PROCESS
 */

/*
 * pcc decide action
 */
/* Double target rate until the link utility doesn't increase accordingly. Then,
 * cut the rate in half and change to the gradient ascent moving stage.
 */
static void pcc_decide_slow_start(xqc_pcc_t *pcc)
{
	struct xqc_pcc_interval_t *interval = &pcc->intervals[0];
	int64_t utility, prev_utility, adjust_utility, prev_adjust_utility, tmp_rate;
	uint32_t extra_rate;

	prev_utility = interval->utility;
	(*pcc->util_func)(pcc);   
	utility = interval->utility;    // 1.18 TODO

	/* The new utiltiy should be at least 75% of the expected utility given
	 * a significant increase. If the utility isn't as high as expected, then
	 * we end slow start.
	 */
	adjust_utility = utility * (utility > 0 ? 1000 : 750) / pcc->rate;
	prev_adjust_utility = prev_utility * (prev_utility > 0 ? 750 : 1000) /
				pcc->last_rate;

	printk(KERN_INFO "%d: start mode: r %lld u %lld pr %lld pu %lld\n",
		pcc->id, pcc->rate, utility, pcc->last_rate, prev_utility);
	//if (adjust_utility > prev_adjust_utility) {
	if (utility > prev_utility) {
		pcc->last_rate = pcc->rate;
		extra_rate = pcc->intervals[0].delivered *
				 tcp_sk(sk)->mss_cache;
		extra_rate = min(extra_rate, pcc->rate / 2);

		// njay -> nogah: I don't really understand why we limited increase to
		// the number of bytes we know were delivered. This just seems to cause
		// us to start slower.
		pcc->rate += pcc->rate / 2; //extra_rate;
		interval->utility = utility;
		interval->rate = pcc->rate;
		pcc->send_index = 0;
		pcc->recive_index = 0;
		pcc->wait = false;
	} else {
		tmp_rate = pcc->last_rate;
		pcc->last_rate = pcc->rate;
		pcc->rate = tmp_rate;
		pcc->start_mode = false;
		printk(KERN_INFO "%d: start mode ended\n", pcc->id);
		
		// njay -> nogah: I've commented out the setup for the 4-RTT decision
		// process and just directly used the "moving" stage. We may not really
		// need the 4-RTT process. I'm not sure how/if it helps us.
#ifdef USE_PROBING
        pcc_setup_intervals_probing(pcc);
#else
		pcc->moving = true;
		pcc_setup_intervals_moving(pcc);
#endif
    }
	start_interval(sk, pcc);
}



/**************************
 * intervals & sample:
 * was started, was ended,
 * find interval per sample
 * ************************/

/* Have we sent all the data we need to for this interval? Must have at least
 * the minimum number of packets and should have sent 1 RTT worth of data.
 */
bool send_interval_ended(xqc_pcc_interval_t *interval, xqc_pcc_t *pcc)
{
	int packets_sent = pcc->packets_sent_count - interval->packets_sent_base;

	if (packets_sent < PCC_INTERVAL_MIN_PACKETS)
		return false;

	if (pcc->packets_counted > interval->packets_sent_base ) {
		interval->packets_ended = pcc->packets_sent_count;
		return true;
	}
	return false;
}

/* Have we accounted for (acked or lost) enough of the packets that we sent to
 * calculate summary statistics?
 */
bool recive_interval_ended(xqc_pcc_interval_t *interval, xqc_pcc_t *pcc)
{
	return interval->packets_ended && interval->packets_ended - 10 < pcc->packets_counted;
}

/* Start the next interval's sending stage. If there is no interval scheduled
 * to send (we have enough for probing, or we are in slow start or moving),
 * then we will be maintaining our rate while we wait for acks.
 */
static void start_next_send_interval(xqc_pcc_t *pcc)
{
	pcc->send_index++;
	if (pcc->send_index == PCC_INTERVALS || pcc->start_mode || pcc->moving) {
		pcc->wait = true;
	}

	start_interval(sk, pcc);
}

/* Update the receiving time window and the number of packets lost/delivered
 * based on socket statistics.
 */
static void
pcc_update_interval(xqc_pcc_interval_t *interval, xqc_pcc_t *pcc)
{
//	if (pcc_interval_in_ignore(interval))
//		return;

	// njay -> nogah: Here I'm keeping track of the starting and ending
	// timestamps and rtts of the interval to calculate latency inflation and
	// throughput.
	interval->recv_end = xqc_monotonic_timestamp();
	interval->end_rtt = pcc->srtt_us >> 3;
	if (interval->lost + interval->delivered == 0) {
		interval->recv_start = xqc_monotonic_timestamp();
		interval->start_rtt = pcc->srtt_us >> 3;
	}

	interval->lost += pcc->lost - pcc->lost_base;
	interval->delivered += pcc->delivered - pcc->delivered_base;
}

static void
pcc_process(xqc_pcc_t *pcc, xqc_sample_t *sampler)
{
	xqc_pcc_interval_t *interval;
	int 		index;
	uint32_t	before;
	
	if(!pcc_valid(pcc)) 
		return;

	pcc_set_cwnd(pcc);
	if(pcc->loss_state) 
		goto end;
	if(!pcc->wait){
		interval = &pcc->intervals[pcc->send_index];
		if (send_interval_ended(interval, pcc)) {
			interval->send_end = xqc_monotonic_timestamp();
			start_next_send_interval(pcc);
		}
	}

	index = pcc->recive_index;
	interval = &pcc->intervals[index];
	before = pcc->packets_counted;
	/* tcp_sk delivered : Total data packets delivered incl. rexmits 
	 * tcp_sk lost : Total data packets lost incl. rexmits
	 * pcc->spare : ?
	 * 
	 * xquic rate_sampler :  prior_delivered(before this ack) delivered(this ack)
	 * 
	 * issues
	 * send_ctl : ctl_recv_count : is this is all recv_count, is this equal to delivered_count? ++itself every on_recv
	 * send_ctl : ctl_lost_count : is this is all lost_count, is this equal to lost_count? ++itself every lost
	 */
	pcc->packets_counted = sampler->send_ctl->ctl_recv_count + sampler->send_ctl->ctl_lost_count - pcc->spare;

	if (!interval->packets_sent_base)
		goto end;

	if (before > 10 + interval->packets_sent_base) {
		pcc_update_interval(interval, pcc);
	}
	if (recive_interval_ended(interval, pcc)) {
		pcc->recive_index++;
		if (pcc->start_mode)
			pcc_decide_slow_start(pcc);
		else if (pcc->moving)
			pcc_decide_moving(sk, pcc);
		else if (pcc->recive_index == PCC_INTERVALS)
			pcc_decide(pcc, sk);
	}


end:
	pcc->lost_base = sampler->send_ctl->ctl_lost_count;
	pcc->delivered_base = sampler->send_ctl->ctl_recv_count;

}


static void 
xqc_pcc_on_ack(void *cong_ctl, xqc_sample_t *sampler)
{	
	xqc_pcc_t *pcc = (xqc_pcc_t *)(cong_ctl);

	/* update params every on ack */
	pcc->srtt_us = sampler->srtt;
	pcc->packets_sent_count = sampler->send_ctl->ctl_send_count;
	
	/* pcc process */
	pcc_process(pcc);
}


/* init */
static void
xqc_pcc_init(void *cong_ctl, xqc_send_ctl_t *ctl_ctx, xqc_cc_params_t cc_params)
{
	xqc_pcc_t *pcc = (xqc_pcc_t *)(cong_ctl);
	memset(pcc, 0, sizeof(*pcc));

	pcc->amplifier = PCC_AMP_MIN;
	pcc->swing_buffer = 0;
	pcc->change_bound = PCC_MIN_CHANGE_BOUND;
	pcc->rate = PCC_RATE_MIN*512;
	pcc->last_rate = pcc->rate;
	pcc->start_mode = XQC_TRUE;
	pcc->moving = XQC_FALSE;
	pcc->intervals[0].utility = S64_MIN;
	pcc->packets_sent_count = 0;

	pcc->srtt_us = 0; /* set zero, start_interval->pcc_set_cwnd->xqc_clamp->XQC_PCC_MIN_WINDOW */

	pcc->util_func = &pcc_calc_utility_vivace;
	pcc->send_ctl = ctl_ctx;

	/* random set 4 micro experiments */
	pcc_setup_intervals_probing(pcc);
	start_interval(pcc);
}

/* maybe used for kernel version's pcc_set_state */
static void
xqc_pcc_on_lost(void *cong_ctl, xqc_usec_t lost_sent_time)
{
	xqc_pcc_t *pcc = (xqc_pcc_t *)(cong_ctl);
	return;
}


/**
 * others
 */

size_t
xqc_pcc_size()
{
	return sizeof(xqc_pcc_t);
}

static uint64_t
xqc_pcc_get_cwnd(void *cong_ctl)
{
	xqc_pcc_t *pcc = (xqc_pcc_t *)(cong_ctl);
	return pcc->cwnd_bytes;
}

static void
xqc_pcc_reset_cwnd(void *cong_ctl)
{
	xqc_pcc_t *pcc = (xqc_pcc_t *)(cong_ctl);
	pcc->cwnd_bytes = XQC_PCC_MIN_WINDOW;
	return;
}

static uint32_t
xqc_pcc_get_pacing_rate(void *cong_ctl)
{
	xqc_pcc_t *pcc = (xqc_pcc_t *)(cong_ctl);
	return pcc->rate;
}

static int
xqc_pcc_in_recovery(void *cong)
{
	xqc_pcc_t *pcc = (xqc_pcc_t *)(cong);
	return XQC_FALSE; /* all state controlled by pcc state machine */
}

int32_t
xqc_pcc_in_slow_start(void *cong_ctl)
{
	xqc_pcc_t *pcc = (xqc_pcc_t *)(cong_ctl);
	return pcc->start_mode; /* controled by pcc state machine */
}

static void
xqc_pcc_restart_from_idle(void *cong_ctl, uint64_t conn_delivered)
{
	return;
}

const xqc_cong_ctrl_callback_t xqc_rlcc_cb = {
	.xqc_cong_ctl_size = xqc_pcc_size,
	.xqc_cong_ctl_init = xqc_pcc_init,
	.xqc_cong_ctl_on_lost = xqc_pcc_on_lost,
	.xqc_cong_ctl_on_ack_multiple_pkts = xqc_pcc_on_ack, // bind with change pacing rate
	.xqc_cong_ctl_get_cwnd = xqc_pcc_get_cwnd,
	.xqc_cong_ctl_reset_cwnd = xqc_pcc_reset_cwnd,
	.xqc_cong_ctl_restart_from_idle = xqc_pcc_restart_from_idle,
	.xqc_cong_ctl_in_recovery = xqc_pcc_in_recovery,
	.xqc_cong_ctl_get_pacing_rate = xqc_pcc_get_pacing_rate,
};

// TODO : https://github1s.com/PCCproject/PCC-Kernel/blob/vivace/src/tcp_pcc.c
// https://github1s.com/NetExperimentEasy/xquic_forrlcc/blob/HEAD/src/congestion_control/rlcc.c#L275-L277
 
