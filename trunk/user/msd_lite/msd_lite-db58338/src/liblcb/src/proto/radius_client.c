/*-
 * Copyright (c) 2014-2024 Rozhuk Ivan <rozhuk.im@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Author: Rozhuk Ivan <rozhuk.im@gmail.com>
 *
 */


#include <sys/param.h>
#include <sys/types.h>
#include <inttypes.h>
#include <unistd.h> /* close, write, sysconf */
#include <string.h> /* memcpy, memmove, memset, strerror... */
#include <pthread.h>
#include <errno.h>
#include <stdio.h> /* snprintf, fprintf */
#include <time.h>

#include "math/crc32.h"
#include "proto/radius.h"

#include "utils/macro.h"
#include "threadpool/threadpool_task.h"
#include "net/socket.h"
#include "net/socket_address.h"
#include "net/utils.h"
#include "utils/buf_str.h"
#include "proto/radius_client.h"
#ifdef RADIUS_CLIENT_XML_CONFIG
#	include "utils/xml.h"
#endif



#define RADIUS_CLIENT_ALLOC_CNT		4
#define RADIUS_CLIENT_SKT_RCV_SIZE	(128 * 1024)
#define RADIUS_CLIENT_SKT_SND_SIZE	(128 * 1024)



typedef struct radius_cli_server_s	*radius_cli_srv_p;
typedef struct radius_cli_socket_s	*radius_cli_skt_p;
typedef struct radius_cli_sockets_s	*radius_cli_skts_p;
typedef struct radius_cli_thread_s	*radius_cli_thr_p;


typedef struct radius_cli_query_s {
	radius_cli_p	rad_cli;	/* Linking... */
	radius_cli_skt_p skt;		/* Linking... */
	size_t		cur_srv_idx;	/* Idx of cur server */
	size_t		retrans_count;	/* Retransmission count. */
	uint64_t	retrans_time;	/* Current retransmission time in ms. */
	uint64_t	retrans_duration; /* Summ of retrans_time. */
	tpt_p		tpt;		/* Need for correct callback. */
	int		query_id_any;	/* QUERY_ID_AUTO was in query_id, use any avail num in socket. */
	size_t		query_id;	/* ID in msg and Index in queries_tmr array. */
	io_buf_p	buf;		/* Query. */
	radius_cli_cb	cb_func;	/* Called after resolv done. */
	void		*udata;		/* Passed as arg to check and done funcs. */
	rad_pkt_hdr_p	pkt;		/* Point to buf or local buf, depend on thread context. */
	int		error;		/* Remember error for async call back. */
} radius_cli_query_t;

typedef struct radius_cli_socket_s {
	tp_task_p	io_pkt_rcvr;	/* Packet receiver. */
	uintptr_t	ident;		/* UDP socket. */
	io_buf_t	buf;		/* Buffer for recv reply. */
	radius_cli_thr_p thr;		/* Linking... */
	radius_cli_skts_p skts;		/* Linking... */
	size_t		queries_count;	/* Now quering ... */
	size_t		queries_index;	/* Next query item index. */
	tp_udata_t	queries_tmr[RADIUS_PKT_HDR_ID_MAX_COUNT]; /* Index in this array used as ID in msg. */
	uint8_t		buf_data[RADIUS_PKT_MAX_SIZE];
} radius_cli_skt_t;

typedef struct radius_cli_sockets_s { /* Static. */
	size_t		queries_count;	/* ...total. */
	size_t		skt_count;	/* ... */
	radius_cli_skt_p *skt;		/* Socket and queries. */
} radius_cli_skts_t;

typedef struct radius_cli_thread_s { /* Static. */
	tpt_p		tpt;		/* Thread for IO and timers. */
	radius_cli_p	rad_cli;	/* Linking... */
	radius_cli_skts_t skts4;
	radius_cli_skts_t skts6;
} radius_cli_thr_t;

typedef struct radius_cli_server_s { /* Static. */
	int		enabled;
	radius_cli_srv_settings_t s;	/* Server connection settings. */
} radius_cli_srv_t;


/* Shared betwin all threads/servers. */
typedef struct radius_cli_s { /* Static. */
	tp_p		tp;		/* Need for timers. */
	radius_cli_settings_t s;
	volatile size_t	thr_count;	/* Threads count. */
	radius_cli_thr_p thr;
	MTX_S		cli_srv_mtx;
	volatile size_t	cli_srv_count;	/* Servers count. */
	radius_cli_srv_p srv;
} radius_cli_t;


uint64_t	radius_client_rnd_factor(tpt_p tpt, uint64_t data);
static void	radius_client_destroy_tpt_msg_cb(tpt_p tpt, void *udata);

void		radius_client_server_remove(radius_cli_p rad_cli,
		    radius_cli_srv_p srv);
int		radius_client_socket_alloc(uint16_t family, radius_cli_thr_p thr);
void		radius_client_socket_free(radius_cli_skt_p skt);

int		radius_client_query_alloc(radius_cli_p rad_cli, tpt_p tpt,
		    size_t query_id, io_buf_p buf, radius_cli_cb cb_func, void *arg,
		    radius_cli_query_p *query_ret);
void		radius_client_query_free(radius_cli_query_p query);
void		radius_client_query_unlink_skt(radius_cli_query_p query);
static void	radius_client_query_done(tpt_p tpt, radius_cli_query_p query,
		    io_buf_p buf, int error);
static void	radius_client_query_done_tpt_msg_cb(tpt_p tpt, void *udata);
static void	radius_client_query_tpt_msg_cb(tpt_p tpt, void *udata);

static int	radius_client_send_new(tpt_p tpt,
		    radius_cli_query_p query);
static int	radius_client_send(radius_cli_query_p query);
static void	radius_client_query_timeout_cb(tp_event_p ev, tp_udata_p tp_udata);
static int	radius_client_recv_cb(tp_task_p ioquery, int error,
		    sockaddr_storage_p addr, io_buf_p buf,
		    size_t transfered_size, void *arg);




void
radius_client_def_settings(radius_cli_settings_p s) {

	memset(s, 0x00, sizeof(radius_cli_settings_t));
	s->servers_max = RADIUS_CLIENT_S_DEF_SERVERS_MAX;
	s->thr_queue_max = RADIUS_CLIENT_S_DEF_THR_QUEUE_MAX;
	s->thr_sockets_min = RADIUS_CLIENT_S_DEF_THR_SOCKETS_MIN;
	s->thr_sockets_max = RADIUS_CLIENT_S_DEF_THR_SOCKETS_MAX;
	s->skt_rcv_buf = RADIUS_CLIENT_S_DEF_SKT_RCV_BUF;
	s->skt_snd_buf = RADIUS_CLIENT_S_DEF_SKT_SND_BUF;
}

void
radius_client_server_def_settings(radius_cli_srv_settings_p s) {

	memset(s, 0x00, sizeof(radius_cli_srv_settings_t));
	s->retrans_time_init = RADIUS_CLIENT_SRV_S_DEF_IRT;
	s->retrans_time_max = RADIUS_CLIENT_SRV_S_DEF_MRT;
	s->retrans_duration_max = RADIUS_CLIENT_SRV_S_DEF_MRD;
	s->retrans_count_max = RADIUS_CLIENT_SRV_S_DEF_MRC;
}

#ifdef RADIUS_CLIENT_XML_CONFIG
int
radius_client_xml_load_settings(const uint8_t *buf, size_t buf_size,
    radius_cli_settings_p s) {
	const uint8_t *ptm;
	size_t tm;

	if (NULL == buf || 0 == buf_size || NULL == s)
		return (EINVAL);
	/* Read from config. */
	s->servers_max = xml_calc_tag_count_args(buf, buf_size,
	    (const uint8_t*)"serverList", "server", NULL);
	xml_get_val_size_t_args(buf, buf_size, NULL, &s->thr_queue_max,
	    (const uint8_t*)"queueMax", NULL);
	xml_get_val_size_t_args(buf, buf_size, NULL, &s->thr_sockets_min,
	    (const uint8_t*)"poolMin", NULL);
	xml_get_val_size_t_args(buf, buf_size, NULL, &s->thr_sockets_max,
	    (const uint8_t*)"poolMax", NULL);
	xml_get_val_uint32_args(buf, buf_size, NULL, &s->skt_rcv_buf,
	    (const uint8_t*)"skt", "rcvBuf", NULL);
	xml_get_val_uint32_args(buf, buf_size, NULL, &s->skt_snd_buf,
	    (const uint8_t*)"skt", "sndBuf", NULL);
	if (0 == xml_get_val_args(buf, buf_size, NULL, NULL, NULL,
	    &ptm, &tm, (const uint8_t*)"nasIdentifier", NULL) &&
	    RADIUS_ATTR_DATA_SIZE_MAX > tm) {
		memcpy(&s->NAS_Identifier, ptm, tm);
		s->NAS_Identifier_size = tm;
	}
	return (0);
}

int
radius_client_server_xml_load_settings(const uint8_t *buf, size_t buf_size,
    radius_cli_srv_settings_p s) {
	const uint8_t *ptm;
	size_t tm;
	char straddr[STR_ADDR_LEN];

	if (NULL == buf || 0 == buf_size || NULL == s)
		return (EINVAL);

	/* Load and add servers. */
	/* address */
	if (0 != xml_get_val_args(buf, buf_size, NULL, NULL, NULL,
	    &ptm, &tm, (const uint8_t*)"address", NULL)) {
		syslog(LOG_CRIT, "Radius client: server addr not set.");
		return (EINVAL);
	}
	if (0 != sa_addr_port_from_str(&s->addr, (const char*)ptm, tm)) {
		memcpy(straddr, ptm, MIN(STR_ADDR_LEN, tm));
		straddr[MIN(STR_ADDR_LEN, tm)] = 0;
		syslog(LOG_CRIT, "Radius client: invalid server addr: %s", straddr);
		return (EINVAL);
	}
	sa_addr_port_to_str(&s->addr, straddr, sizeof(straddr), NULL);
	/* Shared secret. */
	if (0 != xml_get_val_args(buf, buf_size, NULL, NULL, NULL,
	    &ptm, &tm, (const uint8_t*)"secret", NULL) ||
	    RADIUS_A_T_USER_PASSWORD_MAX_LEN <= tm) {
		syslog(LOG_CRIT, "Radius client: shared secret not set for server: %s", straddr);
		return (EINVAL);
	}
	memcpy(&s->shared_secret, ptm, tm);
	s->shared_secret_size = tm;
	/* Other. */
	xml_get_val_uint64_args(buf, buf_size, NULL,
	    &s->retrans_time_init,
	    (const uint8_t*)"retransTimeInit", NULL);
	xml_get_val_uint64_args(buf, buf_size, NULL,
	    &s->retrans_time_max,
	    (const uint8_t*)"retransTimeMax", NULL);
	xml_get_val_uint64_args(buf, buf_size, NULL,
	    &s->retrans_duration_max,
	    (const uint8_t*)"retransDurationMax", NULL);
	xml_get_val_size_t_args(buf, buf_size, NULL,
	    &s->retrans_count_max,
	    (const uint8_t*)"retransCountMax", NULL);

	return (0);
}

int
radius_client_xml_load_start(const uint8_t *buf, size_t buf_size, tp_p tp,
    radius_cli_settings_p cli_settings,
    radius_cli_srv_settings_p cli_srv_settings,
    radius_cli_p *rad_cli) {
	const uint8_t *data, *cur_pos;
	size_t data_size;
	char straddr[STR_ADDR_LEN];
	radius_cli_settings_t cli_s;
	radius_cli_srv_settings_t srv_s;
	int error;

	if (NULL == buf || 0 == buf_size || NULL == rad_cli)
		return (EINVAL);

	if (NULL == cli_settings) { /* Default settings. */
		radius_client_def_settings(&cli_s);
	} else {
		memcpy(&cli_s, cli_settings, sizeof(cli_s));
	}
	/* Read from config. */
	radius_client_xml_load_settings(buf, buf_size, &cli_s);

	error = radius_client_create(tp, &cli_s, rad_cli);
	if (0 != error)
		return (error);
	/* Load and add servers. */
	cur_pos = NULL;
	while (0 == xml_get_val_args(buf, buf_size, &cur_pos, NULL, NULL,
	    &data, &data_size, (const uint8_t*)"serverList", "server", NULL)) {
		if (NULL == cli_srv_settings) { /* Default settings. */
			radius_client_server_def_settings(&srv_s);
		} else {
			memcpy(&srv_s, cli_srv_settings, sizeof(srv_s));
		}
		/* Read from config. */
		if (0 != radius_client_server_xml_load_settings(data,
		    data_size, &srv_s))
			continue;
		sa_addr_port_to_str(&srv_s.addr, straddr, sizeof(straddr), NULL);
		/* Add server. */
		error = radius_client_server_add((*rad_cli), &srv_s);
		if (0 != error) {
			SYSLOG_ERR(LOG_ERR, error, "radius_client_server_add(): %s",
			    straddr);
			continue;
		}
		syslog(LOG_INFO, "Radius client: server %s", straddr);
	}

	return (0);
}
#endif /* RADIUS_CLIENT_XML_CONFIG */


uint64_t
radius_client_rnd_factor(tpt_p tpt, uint64_t data) {
	uint64_t ret;
	uint32_t tm;
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC_FAST, &ts);
	tm = crc32cksum((uint8_t*)&ts, sizeof(struct timespec));
	tm ^= crc32cksum((uint8_t*)&data, sizeof(uint64_t));
	tm = data_xor8(&tm, sizeof(uint32_t));
	if (0 == (tm & 0x7f)) {
		tm ++; /* Prevent division by zero. */
	}

	ret = (((uint64_t)data) / (tm & 0x7f));
	if (0 != (tm & 0x80)) {
		ret = - ret;
	}
	return (ret);
}


int
radius_client_create(tp_p tp, radius_cli_settings_p s,
    radius_cli_p *rad_cli_ret) {
	int error;
	size_t i;
	radius_cli_p rad_cli;

	if (NULL == tp || NULL == s || NULL == rad_cli_ret)
		return (EINVAL);
	rad_cli = calloc(1, sizeof(radius_cli_t));
	if (NULL == rad_cli)
		return (ENOMEM);
	rad_cli->tp = tp;
	memcpy(&rad_cli->s, s, sizeof(radius_cli_settings_t));
	/* Convert settings to per thread limits. */
	if (0 == rad_cli->s.thr_sockets_min) {
		rad_cli->s.thr_sockets_min ++;
	}
	if (rad_cli->s.thr_sockets_max < rad_cli->s.thr_sockets_min) {
		rad_cli->s.thr_sockets_max = rad_cli->s.thr_sockets_min;
	}
	rad_cli->s.servers_max += (RADIUS_CLIENT_ALLOC_CNT - 1);
	rad_cli->s.servers_max &= ~((size_t)(RADIUS_CLIENT_ALLOC_CNT - 1));
	/* Per threads sockets. */
	rad_cli->thr_count = tp_thread_count_max_get(tp);
	rad_cli->thr = calloc(rad_cli->thr_count, sizeof(radius_cli_thr_t));
	if (NULL == rad_cli->thr) {
		error = ENOMEM;
		goto err_out;
	}
	for (i = 0; i < rad_cli->thr_count; i ++) {
		rad_cli->thr[i].tpt = tp_thread_get(tp, i);
		rad_cli->thr[i].rad_cli = rad_cli;
		rad_cli->thr[i].skts4.skt = calloc(rad_cli->s.thr_sockets_max,
		    sizeof(radius_cli_skt_p));
		rad_cli->thr[i].skts6.skt = calloc(rad_cli->s.thr_sockets_max,
		    sizeof(radius_cli_skt_p));
		if (NULL == rad_cli->thr[i].skts4.skt ||
		    NULL == rad_cli->thr[i].skts6.skt) {
			error = ENOMEM;
			goto err_out;
		}
	}
	MTX_INIT(&rad_cli->cli_srv_mtx);
	//rad_cli->cli_srv_count = 0;
	//rad_cli->cli_srv_allocated = 0;
	rad_cli->srv = calloc(rad_cli->s.servers_max, sizeof(radius_cli_srv_t));
	if (NULL == rad_cli->srv) {
		error = ENOMEM;
		goto err_out;
	}
	(*rad_cli_ret) = rad_cli;
	return (0);
err_out:
	MTX_DESTROY(&rad_cli->cli_srv_mtx);
	if (NULL != rad_cli->srv) {
		free(rad_cli->srv);
	}
	if (NULL != rad_cli->thr) {
		for (i = 0; i < rad_cli->thr_count; i ++) {
			if (NULL != rad_cli->thr[i].skts4.skt) {
				free(rad_cli->thr[i].skts4.skt);
			}
			if (NULL != rad_cli->thr[i].skts6.skt) {
				free(rad_cli->thr[i].skts6.skt);
			}
		}
		free(rad_cli->thr);
	}
	free(rad_cli);
	return (error);
}

void
radius_client_destroy(radius_cli_p rad_cli) {

	if (NULL == rad_cli)
		return;
	/* Destroy all threads sockets. */
	if (NULL != rad_cli->thr) {
		tpt_msg_bsend(rad_cli->tp, NULL,
		    (TP_MSG_F_FORCE | TP_MSG_F_SELF_DIRECT |
		    TP_MSG_F_FAIL_DIRECT | TP_BMSG_F_SYNC),
		    radius_client_destroy_tpt_msg_cb, rad_cli);
		free(rad_cli->thr);
	}
	/* Destroy all server "connections". */
	if (NULL != rad_cli->srv) {
		MTX_LOCK(&rad_cli->cli_srv_mtx);
		free(rad_cli->srv);
		MTX_UNLOCK(&rad_cli->cli_srv_mtx);
	}
	MTX_DESTROY(&rad_cli->cli_srv_mtx);
	free(rad_cli);
}
static void
radius_client_destroy_tpt_msg_cb(tpt_p tpt, void *udata) {
	radius_cli_p rad_cli = (radius_cli_p)udata;
	radius_cli_thr_p thr;
	size_t i;

	thr = &rad_cli->thr[tp_thread_get_num(tpt)];

	if (NULL != thr->skts4.skt) {
		for (i = 0; i < thr->skts4.skt_count; i ++) {
			radius_client_socket_free(thr->skts4.skt[i]);
		}
		free(thr->skts4.skt);
	}
	if (NULL != thr->skts6.skt) {
		for (i = 0; i < thr->skts6.skt_count; i ++) {
			radius_client_socket_free(thr->skts6.skt[i]);
		}
		free(thr->skts6.skt);
	}
}


int
radius_client_server_add(radius_cli_p rad_cli, radius_cli_srv_settings_p s) {
	radius_cli_srv_p srv;

	if (NULL == rad_cli || NULL == s)
		return (EINVAL);
	if (rad_cli->s.servers_max == rad_cli->cli_srv_count)
		return (EMLINK);
	MTX_LOCK(&rad_cli->cli_srv_mtx);
	srv = &rad_cli->srv[rad_cli->cli_srv_count];
	srv->enabled = 1;
	memcpy(&srv->s, s, sizeof(radius_cli_srv_settings_t));
	rad_cli->cli_srv_count ++;
	MTX_UNLOCK(&rad_cli->cli_srv_mtx);

	return (0);
}

void
radius_client_server_remove(radius_cli_p rad_cli, radius_cli_srv_p srv) {
	size_t i;

	if (NULL == rad_cli || NULL == srv)
		return;
	/* Remove from list. */
	MTX_LOCK(&rad_cli->cli_srv_mtx);
	for (i = 0; i < rad_cli->cli_srv_count; i ++) {
		if (&rad_cli->srv[i] != srv)
			continue;
		memmove(&rad_cli->srv[i], &rad_cli->srv[(i + 1)],
		    (sizeof(radius_cli_srv_p) * (rad_cli->cli_srv_count - (i + 1))));
		rad_cli->cli_srv_count --;
		memset(&rad_cli->srv[(rad_cli->cli_srv_count - 1)], 0x00,
		    sizeof(radius_cli_srv_t));
		break;
	}
	MTX_UNLOCK(&rad_cli->cli_srv_mtx);
}
void
radius_client_server_remove_by_addr(radius_cli_p rad_cli,
    sockaddr_storage_p addr) {
	size_t i;

	if (NULL == rad_cli || NULL == addr)
		return;
	/* Remove from list. */
	MTX_LOCK(&rad_cli->cli_srv_mtx);
	for (i = 0; i < rad_cli->cli_srv_count; i ++) {
		if (0 != sa_addr_port_is_eq(&rad_cli->srv[i].s.addr, addr))
			continue;
		memmove(&rad_cli->srv[i], &rad_cli->srv[(i + 1)],
		    (sizeof(radius_cli_srv_p) * (rad_cli->cli_srv_count - (i + 1))));
		rad_cli->cli_srv_count --;
		memset(&rad_cli->srv[(rad_cli->cli_srv_count - 1)], 0x00,
		    sizeof(radius_cli_srv_t));
		break;
	}
	MTX_UNLOCK(&rad_cli->cli_srv_mtx);
}


int
radius_client_socket_alloc(uint16_t family, radius_cli_thr_p thr) {
	radius_cli_skt_p skt;
	radius_cli_skts_p skts;
	int error;
	size_t i;

	if (NULL == thr)
		return (EINVAL);
	if (tp_thread_get_current() != thr->tpt) {
		syslog(LOG_DEBUG, "tpt MISSMATCH!!!!!!!!!");
		return (0);
	}
	skts = ((AF_INET == family) ? &thr->skts4 : &thr->skts6);
	if (skts->skt_count >= thr->rad_cli->s.thr_sockets_max)
		return (E2BIG);
	skt = calloc(1, sizeof(radius_cli_skt_t));
	if (NULL == skt)
		return (ENOMEM);
	error = skt_create(family, SOCK_DGRAM, IPPROTO_UDP,
	    SO_F_NONBLOCK, &skt->ident);
	if (0 != error)
		goto err_out;
	/* Tune socket. */
	error = skt_snd_tune(skt->ident, thr->rad_cli->s.skt_snd_buf, 1);
	if (0 != error)
		goto err_out;
	error = skt_rcv_tune(skt->ident, thr->rad_cli->s.skt_rcv_buf, 1);
	if (0 != error)
		goto err_out;

	io_buf_init(&skt->buf, 0, skt->buf_data, sizeof(skt->buf_data));
	IO_BUF_MARK_TRANSFER_ALL_FREE(&skt->buf);
	skt->thr = thr;
	skt->skts = skts;
	//skt->queries_count = 0;
	//skt->queries_index = 0;
	for (i = 0; i < RADIUS_PKT_HDR_ID_MAX_COUNT; i ++) {
		skt->queries_tmr[i].cb_func = radius_client_query_timeout_cb;
	}
	/* Add socket to server list. */
	skts->skt[skts->skt_count] = skt;
	skts->skt_count ++;

	error = tp_task_pkt_rcvr_create(thr->tpt, skt->ident, 0, 0,
	    &skt->buf, radius_client_recv_cb, skt, &skt->io_pkt_rcvr);
	if (0 != error)
		goto err_out;
	return (0);

err_out:
	/* Error. */
	radius_client_socket_free(skt);
	return (error);
}

void
radius_client_socket_free(radius_cli_skt_p skt) {
	size_t i;
	tpt_p tpt;

	if (NULL == skt)
		return;
	tpt = skt->thr->tpt;
	if (tp_thread_get_current() != tpt) {
		syslog(LOG_DEBUG, "tpt MISSMATCH!!!!!!!!!");
		return;
	}
	tp_task_destroy(skt->io_pkt_rcvr);
	close((int)skt->ident);
	io_buf_free(&skt->buf);

	/* Remove from skts list. */
	if (NULL != skt->skts) {
		for (i = 0; i < skt->skts->skt_count; i ++) {
			if (skt != skt->skts->skt[i])
				continue;
			skt->skts->queries_count -= skt->queries_count;
			skt->skts->skt_count --;
			skt->skts->skt[i] = NULL;
			skt->skts = NULL;
			break;
		}
	}
	/* Destroy all queries. */
	for (i = 0; i < RADIUS_PKT_HDR_ID_MAX_COUNT; i ++) {
		if (0 == skt->queries_tmr[i].ident)
			continue;
		/* Prevent loop on free query. */
		((radius_cli_query_p)skt->queries_tmr[i].ident)->skt = NULL;
		radius_client_query_done(tpt,
		    (radius_cli_query_p)skt->queries_tmr[i].ident, NULL, EINTR);
	}
	free(skt);
}


int
radius_client_query_alloc(radius_cli_p rad_cli, tpt_p tpt, size_t query_id,
    io_buf_p buf, radius_cli_cb cb_func, void *arg, radius_cli_query_p *query_ret) {
	radius_cli_query_p query;

	SYSLOGD_EX(LOG_DEBUG, "...");
	if (NULL == rad_cli || NULL == tpt || NULL == cb_func || NULL == query_ret)
		return (EINVAL);
	if (RADIUS_PKT_HDR_ID_MAX_COUNT <= query_id &&
	    RADIUS_CLIENT_QUERY_ID_AUTO != query_id)
		return (EINVAL);
	query = calloc(1, sizeof(radius_cli_query_t));
	if (NULL == query)
		return (ENOMEM);
	query->rad_cli = rad_cli;
	//query->skt = skt;
	//query->cur_srv_idx = 0;
	//query->retrans_count = 0;
	query->tpt = tpt;
	if (RADIUS_CLIENT_QUERY_ID_AUTO == query_id) {
		query->query_id_any = 1;
	}
	query->query_id = query_id;
	query->buf = buf;
	query->cb_func = cb_func;
	query->udata = arg;
	query->pkt = (rad_pkt_hdr_p)buf->data;
	//query->error = 0;
	(*query_ret) = query;
	return (0);
}

void
radius_client_query_free(radius_cli_query_p query) {

	SYSLOGD_EX(LOG_DEBUG, "...");
	if (NULL == query)
		return;
	freezero(query, sizeof(radius_cli_query_t));
}

void
radius_client_query_unlink_skt(radius_cli_query_p query) {
	radius_cli_skt_p skt;

	SYSLOGD_EX(LOG_DEBUG, "...");
	if (NULL == query || NULL == query->skt)
		return;
	/* Remove query from socket. */
	skt = query->skt;
	query->skt = NULL;
	tpt_ev_del_args1(TP_EV_TIMER, &skt->queries_tmr[query->query_id]);
	skt->queries_tmr[query->query_id].ident = 0;
	skt->queries_count --;
	skt->skts->queries_count --;
	/* Does we steel need this socket? */
	if (0 == skt->queries_count &&
	    skt->skts->skt_count > query->rad_cli->s.thr_sockets_min &&
	    skt->skts->skt[(skt->skts->skt_count - 1)] == skt) {
		radius_client_socket_free(skt);
	}
}

static void
radius_client_query_done(tpt_p tpt, radius_cli_query_p query, io_buf_p buf,
    int error) {

	if (NULL == query)
		return;

	radius_client_query_unlink_skt(query);

	if (NULL == query->cb_func) {
		radius_client_query_free(query);
		return;
	}
	query->error = error;
	if (tpt == query->tpt) { /* No need to shedule, direct call cb. */
		//query->pkt = (rad_pkt_hdr_p)buf->data;
		radius_client_query_done_tpt_msg_cb(tpt, query);
		return;
	}
	/* Shedule callback from thread that make query. */
	if (0 == error && NULL != buf) { /* Copy answer to requester buf. */
		io_buf_copy_buf(query->buf, buf);
		query->pkt = (rad_pkt_hdr_p)query->buf->data;
	}
	/* XXX: This is bad on fail, but we have no choice. */
	tpt_msg_send(query->tpt, tpt,
	    (TP_MSG_F_SELF_DIRECT | TP_MSG_F_FAIL_DIRECT),
	    radius_client_query_done_tpt_msg_cb, query);
}
static void
radius_client_query_done_tpt_msg_cb(tpt_p tpt, void *udata) {
	radius_cli_query_p query = (radius_cli_query_p)udata;

	SYSLOGD_EX(LOG_DEBUG, "...");
	if (tp_thread_get_current() != tpt) {
		syslog(LOG_DEBUG, "tpt MISSMATCH!!!!!!!!!");
		return;
	}
	query->cb_func(query, query->pkt, query->error, query->buf, query->udata);
	radius_client_query_free(query);
}


int
radius_client_query(radius_cli_p rad_cli, tpt_p tpt, size_t query_id,
    io_buf_p buf, radius_cli_cb cb_func, void *arg, radius_cli_query_p *query_ret) {
	radius_cli_query_p query = NULL;
	int error;

	if (NULL == rad_cli || NULL == tpt || NULL == buf || NULL == cb_func)
		return (EINVAL);
	/* Add NAS-Identifier to Access-Request.*/
	if (RADIUS_PKT_TYPE_ACCESS_REQUEST == ((rad_pkt_hdr_p)buf)->code) {
		error = radius_pkt_attr_add(((rad_pkt_hdr_p)buf->data),
		    buf->size, &buf->used, RADIUS_ATTR_TYPE_NAS_IDENTIFIER,
		    (uint8_t)rad_cli->s.NAS_Identifier_size,
		    (uint8_t*)&rad_cli->s.NAS_Identifier, NULL);
		if (0 != error)
			return (error);
	}
	error = radius_client_query_alloc(rad_cli, tpt, query_id, buf, cb_func, arg,
	    &query);
	if (0 != error)
		return (error);
	/* Switch thread. */
	//if (tp_thread_get_current() == tpt) {  /* No need to shedule, direct call cb. */
	//	radius_client_query_tpt_msg_cb(tpt, query);
	//} else {
		/* Try send to thread message for server "connections" terminate. */
		error = tpt_msg_send(tpt, NULL, 0,
		    radius_client_query_tpt_msg_cb, query);
	//}
	if (0 != error) {
		SYSLOG_ERR(LOG_ERR, error, "tpt_msg_send() failed.");
		radius_client_query_free(query);
		return (error);
	}
	if (NULL != query_ret) {
		(*query_ret) = query;
	}
	return (0);
}
static void
radius_client_query_tpt_msg_cb(tpt_p tpt, void *udata) {
	int error;
	radius_cli_query_p query = (radius_cli_query_p)udata;

	if (tp_thread_get_current() != tpt) {
		syslog(LOG_DEBUG, "tpt MISSMATCH!!!!!!!!!");
		return;
	}
	error = radius_client_send_new(tpt, query);
	if (0 != error) {
		radius_client_query_done(tpt, query, NULL, error);
	}
}

void
radius_client_query_cancel(radius_cli_query_p query) {

	if (NULL == query)
		return;

	query->cb_func = NULL;
	query->udata = NULL;
}


static int
radius_client_send_new(tpt_p tpt, radius_cli_query_p query) {
	radius_cli_p rad_cli;
	radius_cli_thr_p thr;
	radius_cli_srv_p srv;
	radius_cli_skt_p skt;
	radius_cli_skts_p skts;
	size_t i, query_id = 0;
	int error;

	SYSLOGD_EX(LOG_DEBUG, "...");
	if (NULL == query)
		return (EINVAL);
	if (tp_thread_get_current() != tpt) {
		syslog(LOG_DEBUG, "tpt MISSMATCH!!!!!!!!!");
		return (0);
	}

	rad_cli = query->rad_cli;
	MTX_LOCK(&rad_cli->cli_srv_mtx);
	if (NULL == rad_cli->srv || 0 == rad_cli->cli_srv_count) {
		MTX_UNLOCK(&rad_cli->cli_srv_mtx);
		return (EDESTADDRREQ);
	}
	/* Find next enabled server. */
	srv = NULL;
	for (; query->cur_srv_idx < rad_cli->cli_srv_count; query->cur_srv_idx ++) {
		if (0 == rad_cli->srv[query->cur_srv_idx].enabled)
			continue;
		srv = &rad_cli->srv[query->cur_srv_idx];
		break;
	}
	MTX_UNLOCK(&rad_cli->cli_srv_mtx);
	if (NULL == srv)
		return (ECONNREFUSED);
	thr = &rad_cli->thr[tp_thread_get_num(tpt)];
	skts = ((AF_INET == srv->s.addr.ss_family) ? &thr->skts4 : &thr->skts6);

	if (NULL != query->skt && skts == query->skt->skts)
		goto sign_and_send;
	radius_client_query_unlink_skt(query);
	/* Allocate "slot" in socket. */
	skt = NULL;
	for (i = 0; i < skts->skt_count; i ++) {
		if (0 == query->query_id_any) {
			if (0 != skts->skt[i]->queries_tmr[query->query_id].ident)
				continue;
			skt = skts->skt[i];
			break;
		} else {
			if (RADIUS_PKT_HDR_ID_MAX_COUNT == skts->skt[i]->queries_count)
				continue;
			for (query_id = skts->skt[i]->queries_index;
			    query_id < RADIUS_PKT_HDR_ID_MAX_COUNT; query_id ++) {
				if (0 != skts->skt[i]->queries_tmr[query_id].ident)
					continue;
				skt = skts->skt[i];
				break;
			}
			if (NULL != skt)
				break;
			for (query_id = 0;
			    query_id < skts->skt[i]->queries_index; query_id ++) {
				if (0 != skts->skt[i]->queries_tmr[query_id].ident)
					continue;
				skt = skts->skt[i];
				break;
			}
			if (NULL != skt)
				break;
		}	
	}
	if (NULL == skt) { /* Allocate additional socket. */
		if (skts->skt_count >= rad_cli->s.thr_sockets_max)
			return (EAGAIN); /* No free query slot - QUEUE. */
		error = radius_client_socket_alloc(srv->s.addr.ss_family, thr);
		if (0 != error)
			return (error);
		skt = skts->skt[(skts->skt_count - 1)];
		if (0 != query->query_id_any) {
			query_id = skt->queries_index;
		}
	}
	/* Add/link query to socket. */
	if (0 != query->query_id_any) { /* Update last free index and overwrite ID. */
		skt->queries_index = (query_id + 1);
		query->query_id = query_id;
		((rad_pkt_hdr_p)query->buf->data)->id = (uint8_t)query_id;
	}
	skt->queries_tmr[query->query_id].ident = (uintptr_t)query;
	skt->queries_count ++;
	skt->skts->queries_count ++;
	query->skt = skt;
	error = tpt_ev_add_args(tpt, TP_EV_TIMER, TP_F_DISPATCH, TP_FF_T_MSEC,
	    srv->s.retrans_time_init, &query->skt->queries_tmr[query->query_id]);
	if (0 != error)
		return (error);

sign_and_send:
	/* Sign packet here, once for server before send. */
	error = radius_pkt_sign((rad_pkt_hdr_p)query->buf->data, query->buf->size,
	    &query->buf->used, (uint8_t*)srv->s.shared_secret,
	    srv->s.shared_secret_size, 1);
	if (0 != error) {
		SYSLOG_ERR(LOG_ERR, error, "radius_pkt_sign()");
		return (error);
	}
	/* Gen new retrans time. */
	query->retrans_time = (srv->s.retrans_time_init -
	    radius_client_rnd_factor(thr->tpt, srv->s.retrans_time_init));
	if (0 != srv->s.retrans_time_max &&
	    query->retrans_time > srv->s.retrans_time_max) { /* Limit time. */
		query->retrans_time = (srv->s.retrans_time_max -
		    radius_client_rnd_factor(thr->tpt, srv->s.retrans_time_max));
	}
	query->retrans_count = 0; /* Reset timeouts counter. */
	query->retrans_duration = 0; /* Reset timeouts total time. */
	error = radius_client_send(query);
	SYSLOG_ERR(LOG_ERR, error, "radius_client_send().");
	return (error);
}

static int
radius_client_send(radius_cli_query_p query) {
	int error;
	radius_cli_srv_p srv;

	SYSLOGD_EX(LOG_DEBUG, "...");
	if (NULL == query)
		return (EINVAL);
	if (tp_thread_get_current() != query->skt->thr->tpt) {
		syslog(LOG_DEBUG, "tpt MISSMATCH!!!!!!!!!");
		return (0);
	}

	srv = &query->rad_cli->srv[query->cur_srv_idx];
	error = tpt_ev_enable_args(1, TP_EV_TIMER, TP_F_DISPATCH, TP_FF_T_MSEC,
	    query->retrans_time, &query->skt->queries_tmr[query->query_id]);
	if (0 != error)
		return (error);
	if ((ssize_t)query->buf->used != sendto((int)query->skt->ident,
	    query->buf->data, query->buf->used, (MSG_DONTWAIT | MSG_NOSIGNAL),
	    (sockaddr_p)&srv->s.addr, sa_size(&srv->s.addr))) {
		tpt_ev_enable_args1(0, TP_EV_TIMER,
		    &query->skt->queries_tmr[query->query_id]);
		return (errno);
	}
	return (0);
}

static void
radius_client_query_timeout_cb(tp_event_p ev __unused, tp_udata_p tp_udata) {
	radius_cli_query_p query = (radius_cli_query_p)tp_udata->ident;
	radius_cli_p rad_cli;
	radius_cli_srv_p srv;
	int error;

	SYSLOGD_EX(LOG_DEBUG, "...");
	tpt_ev_enable_args1(0, TP_EV_TIMER, tp_udata);
	if (NULL == query) /* Task already done/removed. */
		return;
	if (tp_thread_get_current() != query->skt->thr->tpt) {
		syslog(LOG_DEBUG, "tpt MISSMATCH!!!!!!!!!");
		return;
	}

	SYSLOGD(LOG_DEBUG, "Query %i - %s.",
	    query->query_id, query->cache_entry->name);
	rad_cli = query->rad_cli;
	srv = &query->rad_cli->srv[query->cur_srv_idx];
	error = ETIMEDOUT;
	query->retrans_count ++;
	query->retrans_duration += query->retrans_time;

	if (0 != srv->s.retrans_count_max &&
	    query->retrans_count >= srv->s.retrans_count_max) /* Retry count exeed. */
		goto err_out;
	if (0 != srv->s.retrans_duration_max &&
	    query->retrans_duration >= srv->s.retrans_duration_max) /* Retry total time exeed. */
		goto err_out;
	/* Gen new retrans time. */
	query->retrans_time = ((2 * query->retrans_time) -
	    radius_client_rnd_factor(query->skt->thr->tpt, query->retrans_time));
	if (0 != srv->s.retrans_time_max &&
	    query->retrans_time > srv->s.retrans_time_max) { /* Limit time. */
		query->retrans_time = (srv->s.retrans_time_max -
		    radius_client_rnd_factor(query->skt->thr->tpt, srv->s.retrans_time_max));
	}
	if (0 != srv->s.retrans_duration_max && /* Total retrans time will exeed. */
	    (query->retrans_duration + query->retrans_time) >= srv->s.retrans_duration_max) {
		query->retrans_time = (srv->s.retrans_duration_max - query->retrans_duration);
		if (query->retrans_time < srv->s.retrans_time_init)
			goto err_out; /* Not enough time for one retry. */
	}
	error = radius_client_send(query);

	/* If timeout retry exeed or error on send - try next server. */
	while ((query->cur_srv_idx + 1) < rad_cli->cli_srv_count &&
	    0 != error) {
		/* Try next server. */
		query->cur_srv_idx ++;
		error = radius_client_send_new(tp_udata->tpt, query);
	}
err_out:
	if (0 != error) { /* Report about error and destroy query. */
		radius_client_query_done(tp_udata->tpt, query, NULL, error);
	}
}


static int
radius_client_recv_cb(tp_task_p tptask __unused, int error,
    sockaddr_storage_p addr, io_buf_p buf, size_t transfered_size __unused,
    void *arg) {
	radius_cli_skt_p skt = arg;
	radius_cli_srv_p srv;
	radius_cli_query_p query;
	rad_pkt_hdr_p pkt;

	SYSLOGD_EX(LOG_DEBUG, "...");
	if (tp_thread_get_current() != skt->thr->tpt) {
		syslog(LOG_DEBUG, "tpt MISSMATCH!!!!!!!!!");
		goto rcv_next;
	}
	if (0 != error)
		goto rcv_next;
	pkt = (rad_pkt_hdr_p)buf->data;
	/* Is packet format OK? */
	error = radius_pkt_chk(pkt, buf->used);
	if (0 != error)
		goto rcv_next;
	/* query_id */
	query = (radius_cli_query_p)skt->queries_tmr[pkt->id].ident;
	if (NULL == query)
		goto rcv_next;
	srv = &skt->thr->rad_cli->srv[query->cur_srv_idx];
	/* Filter packets by from addr. */
	if (0 == sa_addr_port_is_eq(addr, &srv->s.addr))
		goto rcv_next;
	/* Is packet valid? */
	if (0 != radius_pkt_verify(pkt, (uint8_t*)&srv->s.shared_secret,
	    srv->s.shared_secret_size, (rad_pkt_hdr_p)query->buf->data))
		goto rcv_next;
	/* Looks like answer for query... */
	radius_client_query_done(skt->thr->tpt, query, buf, 0);

rcv_next:
	IO_BUF_MARK_AS_EMPTY(buf);
	IO_BUF_MARK_TRANSFER_ALL_FREE(buf);
	return (TP_TASK_CB_CONTINUE);
}
