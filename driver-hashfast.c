/*
 * Copyright 2013-2014 Luke Dashjr
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#include "config.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <utlist.h>

#include "deviceapi.h"
#include "logging.h"
#include "lowlevel.h"
#include "lowl-vcom.h"
#include "util.h"

BFG_REGISTER_DRIVER(hashfast_ums_drv)

#define HASHFAST_QUEUE_MEMORY 0x20

#define HASHFAST_ALL_CHIPS 0xff
#define HASHFAST_ALL_CORES 0xff

#define HASHFAST_HEADER_SIZE 8
#define HASHFAST_MAX_DATA 0x3fc
#define HASHFAST_HASH_SIZE (0x20 + 0xc + 4 + 4 + 2 + 1 + 1)

enum hashfast_opcode {
	HFOP_NULL          =    0,
	HFOP_ROOT          =    1,
	HFOP_RESET         =    2,
	HFOP_PLL_CONFIG    =    3,
	HFOP_ADDRESS       =    4,
	HFOP_READDRESS     =    5,
	HFOP_HIGHEST       =    6,
	HFOP_BAUD          =    7,
	HFOP_UNROOT        =    8,
	HFOP_HASH          =    9,
	HFOP_NONCE         = 0x0a,
	HFOP_ABORT         = 0x0b,
	HFOP_STATUS        = 0x0c,
	HFOP_GPIO          = 0x0d,
	HFOP_CONFIG        = 0x0e,
	HFOP_STATISTICS    = 0x0f,
	HFOP_GROUP         = 0x10,
	HFOP_CLOCKGATE     = 0x11,
	
	HFOP_USB_INIT      = 0x80,
	HFOP_GET_TRACE     = 0x81,
	HFOP_LOOPBACK_USB  = 0x82,
	HFOP_LOOPBACK_UART = 0x83,
	HFOP_DFU           = 0x84,
	HFOP_USB_SHUTDOWN  = 0x85,
	HFOP_DIE_STATUS    = 0x86,
	HFOP_GWQ_STATUS    = 0x87,
	HFOP_WORK_RESTART  = 0x88,
	HFOP_USB_STATS1    = 0x89,
	HFOP_USB_GWQSTATS  = 0x8a,
	HFOP_USB_NOTICE    = 0x8b,
	HFOP_USB_DEBUG     = 0xff,
};

typedef unsigned long hashfast_isn_t;

struct hashfast_parsed_msg {
	uint8_t opcode;
	uint8_t chipaddr;
	uint8_t coreaddr;
	uint16_t hdata;
	uint8_t data[HASHFAST_MAX_DATA];
	size_t datalen;
};

static
ssize_t hashfast_write(const int fd, void * const buf, size_t bufsz)
{
	const ssize_t rv = write(fd, buf, bufsz);
	if (opt_debug && opt_dev_protocol)
	{
		char hex[(bufsz * 2) + 1];
		bin2hex(hex, buf, bufsz);
		if (rv < 0)
			applog(LOG_DEBUG, "%s fd=%d: SEND (%s) => %d",
			       "hashfast", fd, hex, (int)rv);
		else
		if (rv < bufsz)
			applog(LOG_DEBUG, "%s fd=%d: SEND %.*s(%s)",
			       "hashfast", fd, (int)(rv * 2), hex, &hex[rv * 2]);
		else
		if (rv > bufsz)
			applog(LOG_DEBUG, "%s fd=%d: SEND %s => +%d",
			       "hashfast", fd, hex, (int)(rv - bufsz));
		else
			applog(LOG_DEBUG, "%s fd=%d: SEND %s",
			       "hashfast", fd, hex);
	}
	return rv;
}

static
ssize_t hashfast_read(const int fd, void * const buf, size_t bufsz)
{
	const ssize_t rv = serial_read(fd, buf, bufsz);
	if (opt_debug && opt_dev_protocol && rv)
	{
		char hex[(rv * 2) + 1];
		bin2hex(hex, buf, rv);
		applog(LOG_DEBUG, "%s fd=%d: RECV %s",
		       "hashfast", fd, hex);
	}
	return rv;
}

static
bool hashfast_prepare_msg(uint8_t * const buf, const uint8_t opcode, const uint8_t chipaddr, const uint8_t coreaddr, const uint16_t hdata, const size_t datalen)
{
	buf[0] = '\xaa';
	buf[1] = opcode;
	buf[2] = chipaddr;
	buf[3] = coreaddr;
	buf[4] = hdata & 0xff;
	buf[5] = hdata >> 8;
	if (datalen > 1020 || datalen % 4)
		return false;
	buf[6] = datalen / 4;
	buf[7] = crc8ccitt(&buf[1], 6);
	return true;
}

static
bool hashfast_send_msg(const int fd, uint8_t * const buf, const uint8_t opcode, const uint8_t chipaddr, const uint8_t coreaddr, const uint16_t hdata, const size_t datalen)
{
	if (!hashfast_prepare_msg(buf, opcode, chipaddr, coreaddr, hdata, datalen))
		return false;
	const size_t buflen = HASHFAST_HEADER_SIZE + datalen;
	return (buflen == hashfast_write(fd, buf, buflen));
}

static
bool hashfast_parse_msg(const int fd, struct hashfast_parsed_msg * const out_msg)
{
	uint8_t buf[HASHFAST_HEADER_SIZE];
startover:
	if (HASHFAST_HEADER_SIZE != hashfast_read(fd, buf, HASHFAST_HEADER_SIZE))
		return false;
	uint8_t *p = memchr(buf, '\xaa', HASHFAST_HEADER_SIZE);
	if (p != buf)
	{
ignoresome:
		if (!p)
			goto startover;
		int moreneeded = p - buf;
		int alreadyhave = HASHFAST_HEADER_SIZE - moreneeded;
		memmove(buf, p, alreadyhave);
		if (moreneeded != hashfast_read(fd, &buf[alreadyhave], moreneeded))
			return false;
	}
	const uint8_t correct_crc8 = crc8ccitt(&buf[1], 6);
	if (buf[7] != correct_crc8)
	{
		p = memchr(&buf[1], '\xaa', HASHFAST_HEADER_SIZE - 1);
		goto ignoresome;
	}
	out_msg->opcode   = buf[1];
	out_msg->chipaddr = buf[2];
	out_msg->coreaddr = buf[3];
	out_msg->hdata    = (uint16_t)buf[4] | ((uint16_t)buf[5] << 8);
	out_msg->datalen  = buf[6] * 4;
	return (out_msg->datalen == hashfast_read(fd, &out_msg->data[0], out_msg->datalen));
}

static
bool hashfast_lowl_match(const struct lowlevel_device_info * const info)
{
	if (!lowlevel_match_id(info, &lowl_vcom, 0, 0))
		return false;
	return (info->manufacturer && strstr(info->manufacturer, "HashFast"));
}

static
bool hashfast_detect_one(const char * const devpath)
{
	uint16_t clock = 550;
	uint8_t buf[HASHFAST_HEADER_SIZE];
	const int fd = serial_open(devpath, 0, 100, true);
	if (fd == -1)
	{
		applog(LOG_DEBUG, "%s: Failed to open %s", __func__, devpath);
		return false;
	}
	struct hashfast_parsed_msg * const pmsg = malloc(sizeof(*pmsg));
	hashfast_send_msg(fd, buf, HFOP_USB_INIT, 0, 0, clock, 0);
	do {
		if (!hashfast_parse_msg(fd, pmsg))
		{
			applog(LOG_DEBUG, "%s: Failed to parse response on %s",
			        __func__, devpath);
			serial_close(fd);
			goto err;
		}
	} while (pmsg->opcode != HFOP_USB_INIT);
	serial_close(fd);
	const int expectlen = 0x20 + (pmsg->chipaddr * pmsg->coreaddr) / 8;
	if (pmsg->datalen < expectlen)
	{
		applog(LOG_DEBUG, "%s: USB_INIT response too short on %s (%d < %d)",
		       __func__, devpath, (int)pmsg->datalen, expectlen);
		goto err;
	}
	if (pmsg->data[8] != 0)
	{
		applog(LOG_DEBUG, "%s: USB_INIT failed on %s (err=%d)",
		        __func__, devpath, pmsg->data[8]);
		goto err;
	}
	
	if (serial_claim_v(devpath, &hashfast_ums_drv))
		return false;
	
	struct cgpu_info * const cgpu = malloc(sizeof(*cgpu));
	*cgpu = (struct cgpu_info){
		.drv = &hashfast_ums_drv,
		.device_path = strdup(devpath),
		.deven = DEV_ENABLED,
		.procs = (pmsg->chipaddr * pmsg->coreaddr),
		.threads = 1,
		.device_data = pmsg,
	};
	return add_cgpu(cgpu);

err:
	free(pmsg);
	return false;
}

static
bool hashfast_lowl_probe(const struct lowlevel_device_info * const info)
{
	return vcom_lowl_probe_wrapper(info, hashfast_detect_one);
}

struct hashfast_dev_state {
	uint8_t cores_per_chip;
	int fd;
	struct hashfast_chip_state *chipstates;
};

struct hashfast_chip_state {
	struct cgpu_info **coreprocs;
	hashfast_isn_t last_isn;
};

struct hashfast_core_state {
	uint8_t chipaddr;
	uint8_t coreaddr;
	int next_device_id;
	uint8_t last_seq;
	hashfast_isn_t last_isn;
	hashfast_isn_t last2_isn;
	bool has_pending;
	unsigned queued;
};

static
bool hashfast_init(struct thr_info * const master_thr)
{
	struct cgpu_info * const dev = master_thr->cgpu, *proc;
	struct hashfast_parsed_msg * const pmsg = dev->device_data;
	struct hashfast_dev_state * const devstate = malloc(sizeof(*devstate));
	struct hashfast_chip_state * const chipstates = malloc(sizeof(*chipstates) * pmsg->chipaddr), *chipstate;
	struct hashfast_core_state * const corestates = malloc(sizeof(*corestates) * dev->procs), *cs;
	int i;
	
	*devstate = (struct hashfast_dev_state){
		.chipstates = chipstates,
		.cores_per_chip = pmsg->coreaddr,
		.fd = serial_open(dev->device_path, 0, 1, true),
	};
	
	for (i = 0; i < pmsg->chipaddr; ++i)
	{
		chipstate = &chipstates[i];
		*chipstate = (struct hashfast_chip_state){
			.coreprocs = malloc(sizeof(struct cgpu_info *) * pmsg->coreaddr),
		};
	}
	
	for ((i = 0), (proc = dev); proc; ++i, (proc = proc->next_proc))
	{
		struct thr_info * const thr = proc->thr[0];
		const bool core_is_working = pmsg->data[0x20 + (i / 8)] & (1 << (i % 8));
		
		if (!core_is_working)
			proc->deven = DEV_RECOVER_DRV;
		proc->device_data = devstate;
		thr->cgpu_data = cs = &corestates[i];
		*cs = (struct hashfast_core_state){
			.chipaddr = i / pmsg->coreaddr,
			.coreaddr = i % pmsg->coreaddr,
		};
		chipstates[cs->chipaddr].coreprocs[cs->coreaddr] = proc;
	}
	free(pmsg);
	
	// TODO: actual clock = [12,13]
	
	for_each_managed_proc(proc, dev)
	{
		proc->status = LIFE_INIT2;
	}
	
	timer_set_now(&master_thr->tv_poll);
	return true;
}

static
bool hashfast_queue_append(struct thr_info * const thr, struct work * const work)
{
	struct cgpu_info * const proc = thr->cgpu;
	struct hashfast_dev_state * const devstate = proc->device_data;
	const int fd = devstate->fd;
	struct hashfast_core_state * const cs = thr->cgpu_data;
	struct hashfast_chip_state * const chipstate = &devstate->chipstates[cs->chipaddr];
	const size_t cmdlen = HASHFAST_HEADER_SIZE + HASHFAST_HASH_SIZE;
	uint8_t cmd[cmdlen];
	uint8_t * const hashdata = &cmd[HASHFAST_HEADER_SIZE];
	hashfast_isn_t isn;
	uint8_t seq;
	
	if (cs->has_pending)
	{
		thr->queue_full = true;
		return false;
	}
	
	isn = ++chipstate->last_isn;
	seq = ++cs->last_seq;
	work->device_id = seq;
	cs->last2_isn = cs->last_isn;
	cs->last_isn = isn;
	hashfast_prepare_msg(cmd, HFOP_HASH, cs->chipaddr, cs->coreaddr, (cs->coreaddr << 8) | seq, 56);
	memcpy(&hashdata[   0], work->midstate, 0x20);
	memcpy(&hashdata[0x20], &work->data[64], 0xc);
	memset(&hashdata[0x2c], '\0', 0xa);  // starting_nonce, nonce_loops, ntime_loops
	hashdata[0x36] = 32;  // search target (number of zero bits)
	hashdata[0x37] = 0;
	cs->has_pending = true;
	
	if (cmdlen != hashfast_write(fd, cmd, cmdlen))
		return false;
	
	DL_APPEND(thr->work, work);
	if (cs->queued > HASHFAST_QUEUE_MEMORY)
	{
		struct work * const old_work = thr->work;
		DL_DELETE(thr->work, old_work);
		free_work(old_work);
	}
	else
		++cs->queued;
	
	return true;
}

static
void hashfast_queue_flush(struct thr_info * const thr)
{
	struct cgpu_info * const proc = thr->cgpu;
	struct hashfast_dev_state * const devstate = proc->device_data;
	const int fd = devstate->fd;
	struct hashfast_core_state * const cs = thr->cgpu_data;
	uint8_t cmd[HASHFAST_HEADER_SIZE];
	uint16_t hdata = 2;
	if ((!thr->work) || stale_work(thr->work->prev, true))
	{
		applog(LOG_DEBUG, "%"PRIpreprv": Flushing both active and pending work",
		       proc->proc_repr);
		hdata |= 1;
	}
	else
		applog(LOG_DEBUG, "%"PRIpreprv": Flushing pending work",
		       proc->proc_repr);
	hashfast_send_msg(fd, cmd, HFOP_ABORT, cs->chipaddr, cs->coreaddr, hdata, 0);
}

static
struct cgpu_info *hashfast_find_proc(struct thr_info * const master_thr, int chipaddr, int coreaddr)
{
	struct cgpu_info *proc = master_thr->cgpu;
	struct hashfast_dev_state * const devstate = proc->device_data;
	if (coreaddr >= devstate->cores_per_chip)
		return NULL;
	const unsigned chip_count = proc->procs / devstate->cores_per_chip;
	if (chipaddr >= chip_count)
		return NULL;
	struct hashfast_chip_state * const chipstate = &devstate->chipstates[chipaddr];
	return chipstate->coreprocs[coreaddr];
}

static
hashfast_isn_t hashfast_get_isn(struct hashfast_chip_state * const chipstate, uint16_t hfseq)
{
	const uint8_t coreaddr = hfseq >> 8;
	const uint8_t seq = hfseq & 0xff;
	struct cgpu_info * const proc = chipstate->coreprocs[coreaddr];
	struct thr_info * const thr = proc->thr[0];
	struct hashfast_core_state * const cs = thr->cgpu_data;
	if (cs->last_seq == seq)
		return cs->last_isn;
	if (cs->last_seq == (uint8_t)(seq + 1))
		return cs->last2_isn;
	return 0;
}

static
void hashfast_submit_nonce(struct thr_info * const thr, struct work * const work, const uint32_t nonce, const bool searched)
{
	struct cgpu_info * const proc = thr->cgpu;
	struct hashfast_core_state * const cs = thr->cgpu_data;
	
	applog(LOG_DEBUG, "%"PRIpreprv": Found nonce for seq %02x (last=%02x): %08lx%s",
	       proc->proc_repr, (unsigned)work->device_id, (unsigned)cs->last_seq,
	       (unsigned long)nonce, searched ? " (searched)" : "");
	submit_nonce(thr, work, nonce);
}

static
bool hashfast_poll_msg(struct thr_info * const master_thr)
{
	struct cgpu_info * const dev = master_thr->cgpu;
	struct hashfast_dev_state * const devstate = dev->device_data;
	const int fd = devstate->fd;
	
	struct hashfast_parsed_msg msg;
	if (!hashfast_parse_msg(fd, &msg))
		return false;
	
	switch (msg.opcode)
	{
		case HFOP_NONCE:
		{
			const uint8_t *data = msg.data;
			for (int i = msg.datalen / 8; i; --i, (data = &data[8]))
			{
				const uint32_t nonce = (data[0] <<  0)
				                     | (data[1] <<  8)
				                     | (data[2] << 16)
				                     | (data[3] << 24);
				const uint8_t seq = data[4];
				const uint8_t coreaddr = data[5];
				// uint32_t ntime = data[6] | ((data[7] & 0xf) << 8);
				const bool search = data[7] & 0x10;
				struct cgpu_info * const proc = hashfast_find_proc(master_thr, msg.chipaddr, coreaddr);
				if (unlikely(!proc))
				{
					applog(LOG_ERR, "%s: Unknown chip/core address %u/%u",
					       dev->dev_repr, (unsigned)msg.chipaddr, (unsigned)coreaddr);
					inc_hw_errors_only(master_thr);
					continue;
				}
				struct thr_info * const thr = proc->thr[0];
				struct hashfast_core_state * const cs = thr->cgpu_data;
				struct work *work;
				
				DL_SEARCH_SCALAR(thr->work, work, device_id, seq);
				if (unlikely(!work))
				{
					applog(LOG_WARNING, "%"PRIpreprv": Unknown seq %02x (last=%02x)",
					       proc->proc_repr, (unsigned)seq, (unsigned)cs->last_seq);
					inc_hw_errors2(thr, NULL, &nonce);
					continue;
				}
				
				unsigned nonces_found = 1;
				
				hashfast_submit_nonce(thr, work, nonce, false);
				if (search)
				{
					for (int noffset = 1; noffset <= 0x80; ++noffset)
					{
						const uint32_t nonce2 = nonce + noffset;
						if (test_nonce(work, nonce2, false))
						{
							hashfast_submit_nonce(thr, work, nonce2, true);
							++nonces_found;
						}
					}
					if (!nonces_found)
					{
						inc_hw_errors_only(thr);
						applog(LOG_WARNING, "%"PRIpreprv": search=1, but failed to turn up any additional solutions",
						       proc->proc_repr);
					}
				}
				
				hashes_done2(thr, 0x100000000 * nonces_found, NULL);
			}
			break;
		}
		case HFOP_STATUS:
		{
			const uint8_t *data = &msg.data[8];
			struct cgpu_info *proc = hashfast_find_proc(master_thr, msg.chipaddr, 0);
			if (unlikely(!proc))
			{
				applog(LOG_ERR, "%s: Unknown chip address %u",
				       dev->dev_repr, (unsigned)msg.chipaddr);
				inc_hw_errors_only(master_thr);
				break;
			}
			struct hashfast_chip_state * const chipstate = &devstate->chipstates[msg.chipaddr];
			hashfast_isn_t isn = hashfast_get_isn(chipstate, msg.hdata);
			int cores_uptodate, cores_active, cores_pending, cores_transitioned;
			cores_uptodate = cores_active = cores_pending = cores_transitioned = 0;
			for (int i = 0; i < devstate->cores_per_chip; ++i, (proc = proc->next_proc))
			{
				struct thr_info * const thr = proc->thr[0];
				struct hashfast_core_state * const cs = thr->cgpu_data;
				const uint8_t bits = data[i / 4] >> (2 * (i % 4));
				const bool has_active  = bits & 1;
				const bool has_pending = bits & 2;
				bool try_transition = true;
				
				if (cs->last_isn <= isn)
					++cores_uptodate;
				else
					try_transition = false;
				
				if (has_active)
					++cores_active;
				
				if (has_pending)
					++cores_pending;
				else
				if (try_transition)
				{
					++cores_transitioned;
					cs->has_pending = false;
					thr->queue_full = false;
				}
			}
			applog(LOG_DEBUG, "%s: STATUS from chipaddr=0x%02x with hdata=0x%04x (isn=0x%lx): total=%d uptodate=%d active=%d pending=%d transitioned=%d",
			       dev->dev_repr, (unsigned)msg.chipaddr, (unsigned)msg.hdata, isn,
			       devstate->cores_per_chip, cores_uptodate,
			       cores_active, cores_pending, cores_transitioned);
			break;
		}
	}
	return true;
}

static
void hashfast_poll(struct thr_info * const master_thr)
{
	struct cgpu_info * const dev = master_thr->cgpu;
	struct timeval tv_timeout;
	timer_set_delay_from_now(&tv_timeout, 10000);
	while (true)
	{
		if (!hashfast_poll_msg(master_thr))
		{
			applog(LOG_DEBUG, "%s poll: No more messages", dev->dev_repr);
			break;
		}
		if (timer_passed(&tv_timeout, NULL))
		{
			applog(LOG_DEBUG, "%s poll: 10ms timeout met", dev->dev_repr);
			break;
		}
	}
	
	timer_set_delay_from_now(&master_thr->tv_poll, 100000);
}

struct device_drv hashfast_ums_drv = {
	.dname = "hashfast_ums",
	.name = "HFA",
	
	.lowl_match = hashfast_lowl_match,
	.lowl_probe = hashfast_lowl_probe,
	
	.thread_init = hashfast_init,
	
	.minerloop = minerloop_queue,
	.queue_append = hashfast_queue_append,
	.queue_flush = hashfast_queue_flush,
	.poll = hashfast_poll,
};
