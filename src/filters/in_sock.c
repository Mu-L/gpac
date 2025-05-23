/*
 *			GPAC - Multimedia Framework C SDK
 *
 *			Authors: Jean Le Feuvre
 *			Copyright (c) Telecom ParisTech 2017-2024
 *					All rights reserved
 *
 *  This file is part of GPAC / generic TCP/UDP input filter
 *
 *  GPAC is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  GPAC is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */


#include <gpac/filters.h>

#ifndef GPAC_DISABLE_NETWORK

#include <gpac/constants.h>
#include <gpac/network.h>

#ifndef GPAC_DISABLE_STREAMING
#include <gpac/internal/ietf_dev.h>
#endif

typedef struct
{
	GF_FilterPid *pid;
	GF_Socket *socket;
	Bool pck_out;
#ifndef GPAC_DISABLE_STREAMING
	GF_RTPReorder *rtp_reorder;
#else
	Bool is_rtp;
#endif
	char address[GF_MAX_IP_NAME_LEN];

	u32 init_time;
	Bool done, first_pck;
	//stats
	u64 nb_bytes;
	u64 start_time, last_stats_time;
} GF_SockInClient;

typedef struct
{
	//options
	const char *src;
	u32 block_size;
	u32 port, maxc;
	char *ifce;
	const char *ext;
	const char *mime;
	Bool tsprobe, listen, ka, block;
	u32 timeout;
#ifndef GPAC_DISABLE_STREAMING
	u32 reorder_pck;
	u32 reorder_delay;
#endif
	GF_PropStringList ssm, ssmx;
	GF_PropVec2i mwait;

	GF_SockInClient sock_c;
	GF_List *clients;
	Bool had_clients;
	Bool is_udp;
	Bool is_stop;

	char *buffer;

	GF_SockGroup *active_sockets;
	u32 last_rcv_time;
	u32 last_timeout_sec;

	u64 rcv_time_diff, last_pck_time;
} GF_SockInCtx;



static GF_Err sockin_initialize(GF_Filter *filter)
{
	char *str, *url;
	u16 port;
	u32 sock_type = 0;
	GF_Err e = GF_OK;
	GF_SockInCtx *ctx = (GF_SockInCtx *) gf_filter_get_udta(filter);

	if (!ctx || !ctx->src) return GF_BAD_PARAM;
	if ((ctx->mwait.y < ctx->mwait.x) || (ctx->mwait.x<0) || (ctx->mwait.y<0)) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_NETWORK, ("[SockIn] Invalid `mwait`, max %d must be greater than min %d\n", ctx->mwait.y, ctx->mwait.x));
		return GF_IO_ERR;
	}
	ctx->active_sockets = gf_sk_group_new();
	if (!ctx->active_sockets) return GF_OUT_OF_MEM;

	if (!strnicmp(ctx->src, "udp://", 6)) {
		sock_type = GF_SOCK_TYPE_UDP;
		ctx->listen = GF_FALSE;
		ctx->is_udp = GF_TRUE;
	} else if (!strnicmp(ctx->src, "tcp://", 6)) {
		sock_type = GF_SOCK_TYPE_TCP;
#ifdef GPAC_HAS_SOCK_UN
	} else if (!strnicmp(ctx->src, "tcpu://", 7) ) {
		sock_type = GF_SOCK_TYPE_TCP_UN;
	} else if (!strnicmp(ctx->src, "udpu://", 7) ) {
		sock_type = GF_SOCK_TYPE_UDP_UN;
		ctx->listen = GF_FALSE;
#endif
	} else {
		return GF_NOT_SUPPORTED;
	}

	url = strchr(ctx->src, ':');
	url += 3;
	if (!url[0]) return GF_IP_ADDRESS_NOT_FOUND;

	ctx->sock_c.socket = gf_sk_new_ex(sock_type, gf_filter_get_netcap_id(filter));
	if (! ctx->sock_c.socket ) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_NETWORK, ("[SockIn] Failed to open socket for %s\n", ctx->src));
		return GF_IO_ERR;
	}
	ctx->sock_c.first_pck = GF_TRUE;

	/*setup port and src*/
	port = ctx->port;
	str = strrchr(url, ':');
	/*take care of IPv6 address*/
	if (str && strchr(str, ']')) str = strchr(url, ':');
	if (str) {
		port = atoi(str+1);
		str[0] = 0;
	}


	/*do we have a source ?*/
	if (gf_sk_is_multicast_address(url)) {
		e = gf_sk_setup_multicast_ex(ctx->sock_c.socket, url, port, 0, 0, ctx->ifce, (const char **)ctx->ssm.vals, ctx->ssm.nb_items, (const char **)ctx->ssmx.vals, ctx->ssmx.nb_items);
		ctx->listen = GF_FALSE;
	} else if ((sock_type==GF_SOCK_TYPE_UDP) 
#ifdef GPAC_HAS_SOCK_UN 
		|| (sock_type==GF_SOCK_TYPE_UDP_UN)
#endif
		) {
		e = gf_sk_bind(ctx->sock_c.socket, ctx->ifce, port, url, port, GF_SOCK_REUSE_PORT);
		ctx->listen = GF_FALSE;
		if (!e)
			e = gf_sk_connect(ctx->sock_c.socket, url, port, NULL);
	} else if (ctx->listen) {
		e = gf_sk_bind(ctx->sock_c.socket, NULL, port, url, 0, GF_SOCK_REUSE_PORT);
		if (!e)
			e = gf_sk_listen(ctx->sock_c.socket, ctx->maxc);
		if (!e) {
			gf_filter_post_process_task(filter);
			gf_sk_server_mode(ctx->sock_c.socket, GF_TRUE);
		}

	} else {
		e = gf_sk_connect(ctx->sock_c.socket, url, port, NULL);
	}

	strcpy(ctx->sock_c.address, "unknown");
	gf_sk_get_remote_address(ctx->sock_c.socket, ctx->sock_c.address);

	if (str) str[0] = ':';

	if (e) {
		gf_sk_del(ctx->sock_c.socket);
		ctx->sock_c.socket = NULL;
		return e;
	}

	gf_sk_group_register(ctx->active_sockets, ctx->sock_c.socket);

	GF_LOG(GF_LOG_INFO, GF_LOG_NETWORK, ("[SockIn] opening %s%s\n", ctx->src, ctx->listen ? " in server mode" : ""));

	if (ctx->block_size<2000)
		ctx->block_size = 2000;

	if (ctx->is_udp) {
		ctx->block_size = (ctx->block_size / 188) * 188;

		gf_filter_prevent_blocking(filter, GF_TRUE);
	}
	gf_sk_set_buffer_size(ctx->sock_c.socket, 0, ctx->block_size);
	gf_sk_set_block_mode(ctx->sock_c.socket, (!ctx->is_udp && ctx->block) ? GF_FALSE : GF_TRUE);

	if (!ctx->is_udp)
		gf_filter_set_blocking(filter, GF_TRUE);

	ctx->buffer = gf_malloc(ctx->block_size + 1);
	if (!ctx->buffer) return GF_OUT_OF_MEM;
	//ext/mime given and not mpeg2, disable probe
	if (ctx->ext && !strstr("ts|m2t|mts|dmb|trp", ctx->ext)) ctx->tsprobe = GF_FALSE;
	if (ctx->mime && !strstr(ctx->mime, "mpeg-2") && !strstr(ctx->mime, "mp2t")) ctx->tsprobe = GF_FALSE;

	if (ctx->listen) {
		ctx->clients = gf_list_new();
		if (!ctx->clients) return GF_OUT_OF_MEM;
	}

	ctx->sock_c.init_time = gf_sys_clock();

	return GF_OK;
}

static void sockin_client_reset(GF_SockInClient *sc)
{
	if (sc->socket) gf_sk_del(sc->socket);
	sc->socket = NULL;
#ifndef GPAC_DISABLE_STREAMING
	if (sc->rtp_reorder) gf_rtp_reorderer_del(sc->rtp_reorder);
	sc->rtp_reorder = NULL;
#endif
}

static void sockin_finalize(GF_Filter *filter)
{
	GF_SockInCtx *ctx = (GF_SockInCtx *) gf_filter_get_udta(filter);

	if (ctx->clients) {
		while (gf_list_count(ctx->clients)) {
			GF_SockInClient *sc = gf_list_pop_back(ctx->clients);
			sockin_client_reset(sc);
			gf_free(sc);
		}
		gf_list_del(ctx->clients);
	}
	sockin_client_reset(&ctx->sock_c);
	if (ctx->buffer) gf_free(ctx->buffer);
	if (ctx->active_sockets) gf_sk_group_del(ctx->active_sockets);
}

static GF_FilterProbeScore sockin_probe_url(const char *url, const char *mime_type)
{
	if (!strnicmp(url, "udp://", 6)) return GF_FPROBE_SUPPORTED;
	if (!strnicmp(url, "tcp://", 6)) return GF_FPROBE_SUPPORTED;
#ifdef GPAC_HAS_SOCK_UN
	if (!strnicmp(url, "udpu://", 7)) return GF_FPROBE_SUPPORTED;
	if (!strnicmp(url, "tcpu://", 7)) return GF_FPROBE_SUPPORTED;
#endif
	return GF_FPROBE_NOT_SUPPORTED;
}

#ifndef GPAC_DISABLE_STREAMING
static void sockin_rtp_destructor(GF_Filter *filter, GF_FilterPid *pid, GF_FilterPacket *pck)
{
	u32 size;
	char *data;
	GF_SockInClient *sc = (GF_SockInClient *) gf_filter_pid_get_udta(pid);
	sc->pck_out = GF_FALSE;
	data = (char *) gf_filter_pck_get_data(pck, &size);
	if (data) {
		data-=12;
		gf_free(data);
	}
}
#endif

static Bool sockin_process_event(GF_Filter *filter, const GF_FilterEvent *evt)
{
	if (!evt->base.on_pid) return GF_FALSE;
	GF_SockInCtx *ctx = (GF_SockInCtx *) gf_filter_get_udta(filter);

	switch (evt->base.type) {
	case GF_FEVT_PLAY:
		ctx->is_stop = GF_FALSE;
		return GF_TRUE;
	case GF_FEVT_STOP:
		ctx->is_stop = GF_TRUE;
		return GF_TRUE;
	default:
		break;
	}
	return GF_FALSE;
}

static GF_Err sockin_read_client(GF_Filter *filter, GF_SockInCtx *ctx, GF_SockInClient *sock_c)
{
	u32 nb_read, pos, nb_pck=100;
	u64 now;
	GF_Err e;
	GF_FilterPacket *dst_pck;
	u8 *out_data, *in_data;

	if (!sock_c->socket)
		return GF_EOS;
	if (sock_c->pck_out)
		return GF_OK;

	if (sock_c->pid && !ctx->is_udp && gf_filter_pid_would_block(sock_c->pid)) {
		return GF_OK;
	}

	if (!sock_c->start_time) sock_c->start_time = gf_sys_clock_high_res();

refetch:
	pos = 0;
	nb_read=0;
	while (pos < ctx->block_size) {
		u32 read=0;
		e = gf_sk_receive_no_select(sock_c->socket, ctx->buffer+pos, ctx->block_size - pos, &read);
		if (e) {
			if (nb_read) break;
			switch (e) {
			case GF_IP_NETWORK_EMPTY:
				return GF_OK;
			case GF_OK:
				break;
			case GF_IP_CONNECTION_CLOSED:
				if (!sock_c->done) {
					sock_c->done = GF_TRUE;
					if (ctx->ka) {
						gf_filter_pid_send_flush(sock_c->pid);
						return GF_IP_CONNECTION_CLOSED;
					}
					gf_filter_pid_set_eos(sock_c->pid);
				}
				return GF_EOS;
			default:
				return e;
			}
		}
		nb_read+=read;
		if (!ctx->is_udp
#ifndef GPAC_DISABLE_STREAMING
		 || sock_c->rtp_reorder
#else
		 || sock_c->is_rtp
#endif
		 )
			break;
		pos += read;
	}
	if (!nb_read) return GF_OK;

	if (sock_c->first_pck) {
		GF_LOG(GF_LOG_INFO, GF_LOG_NETWORK, ("[SockIn] Reception started after %u ms\n", gf_sys_clock() - sock_c->init_time));
	}

	sock_c->nb_bytes += nb_read;
	sock_c->done = GF_FALSE;

	//we allocated one more byte for that
	ctx->buffer[nb_read] = 0;

	//first run, probe data
	if (!sock_c->pid) {
		const char *mime = ctx->mime;
		const char *ext = ctx->ext;
		//probe MPEG-2
		if (ctx->tsprobe) {
			/*TS over RTP signaled as udp */
			if ((ctx->buffer[0] != 0x47) && ((ctx->buffer[1] & 0x7F) == 33) ) {
#ifndef GPAC_DISABLE_STREAMING
				sock_c->rtp_reorder = gf_rtp_reorderer_new(ctx->reorder_pck, ctx->reorder_delay, 90000);
#else
				sock_c->is_rtp = GF_TRUE;
#endif
				mime = "video/mp2t";
				ext = "ts";
			} else if (ctx->buffer[0] == 0x47) {
				mime = "video/mp2t";
				ext = "ts";
			}
		}

		e = gf_filter_pid_raw_new(filter, ctx->src, NULL, mime, ext, ctx->buffer, nb_read, GF_TRUE, &sock_c->pid);
		if (e) return e;

//		if (ctx->is_udp) gf_filter_pid_set_property(sock_c->pid, GF_PROP_PID_UDP, &PROP_BOOL(GF_TRUE) );

		gf_filter_pid_set_udta(sock_c->pid, sock_c);

#ifdef GPAC_ENABLE_COVERAGE
		if (gf_sys_is_cov_mode()) {
			GF_FilterEvent evt;
			memset(&evt, 0, sizeof(GF_FilterEvent));
			evt.base.type = GF_FEVT_PLAY;
			evt.base.on_pid = sock_c->pid;
			sockin_process_event(filter, &evt);
		}
#endif

	}

	in_data = ctx->buffer;

#ifndef GPAC_DISABLE_STREAMING
	if (sock_c->rtp_reorder) {
		char *pck;
		u16 seq_num = ((ctx->buffer[2] << 8) & 0xFF00) | (ctx->buffer[3] & 0xFF);
		gf_rtp_reorderer_add(sock_c->rtp_reorder, (void *) ctx->buffer, nb_read, seq_num);

		pck = (char *) gf_rtp_reorderer_get(sock_c->rtp_reorder, &nb_read, GF_FALSE, NULL);
		if (pck) {
			dst_pck = gf_filter_pck_new_shared(sock_c->pid, pck+12, nb_read-12, sockin_rtp_destructor);
			if (dst_pck) {
				gf_filter_pck_set_framing(dst_pck, sock_c->first_pck, GF_FALSE);
				gf_filter_pck_send(dst_pck);
				sock_c->first_pck = GF_FALSE;
			}
		}
		goto do_stats;
	}
#else
	if (sock_c->is_rtp) {
		in_data = ctx->buffer + 12;
		nb_read -= 12;
	}
#endif

	dst_pck = gf_filter_pck_new_alloc(sock_c->pid, nb_read, &out_data);
	if (!dst_pck) return GF_OUT_OF_MEM;

	memcpy(out_data, in_data, nb_read);

	gf_filter_pck_set_framing(dst_pck, sock_c->first_pck, GF_FALSE);
	gf_filter_pck_send(dst_pck);
	sock_c->first_pck = GF_FALSE;

do_stats:
	//send bitrate ever half sec
	now = gf_sys_clock_high_res();
	if (now > sock_c->last_stats_time + 500000) {
		sock_c->last_stats_time = now;
		u64 bitrate = (now - sock_c->start_time );
		if (bitrate) {
			bitrate = (sock_c->nb_bytes * 8 * 500000) / bitrate;
			gf_filter_pid_set_info(sock_c->pid, GF_PROP_PID_DOWN_RATE, &PROP_UINT((u32) bitrate) );
			GF_LOG(GF_LOG_INFO, GF_LOG_NETWORK, ("[SockIn] Receiving from %s at %d kbps\r", sock_c->address, (u32) (bitrate/1000)));
		}
		sock_c->nb_bytes = 0;
	}

	if (e || (!ctx->is_udp && ctx->block)) return e;
	nb_pck--;
	if (nb_pck) goto refetch;

	return e;
}

static GF_Err sockin_check_eos(GF_Filter *filter, GF_SockInCtx *ctx)
{
	u32 now;
	if (!ctx->timeout) return GF_OK;

	now = gf_sys_clock();
	if (!ctx->last_rcv_time) {
		ctx->last_rcv_time = now;
		return GF_OK;
	}
	if (now - ctx->last_rcv_time < ctx->timeout) {
		u32 tout = (ctx->timeout - (now - ctx->last_rcv_time)) / 1000;
		if (tout != ctx->last_timeout_sec) {
			ctx->last_timeout_sec = tout;
			GF_LOG(GF_LOG_INFO, GF_LOG_NETWORK, ("[SockIn] Waiting for %u seconds\r", tout));
		}
		return GF_OK;
	}
	if (!ctx->sock_c.done) {
		if (ctx->sock_c.pid)
			gf_filter_pid_set_eos(ctx->sock_c.pid);
		ctx->sock_c.done = GF_TRUE;
		if (!ctx->sock_c.first_pck) {
			GF_LOG(GF_LOG_INFO, GF_LOG_NETWORK, ("[SockIn] No data received for %d ms, assuming end of stream\n", ctx->timeout));
		} else {
			GF_LOG(GF_LOG_WARNING, GF_LOG_NETWORK, ("[SockIn] No data received after %d ms, aborting\n", ctx->timeout));
			gf_filter_setup_failure(filter, GF_IP_NETWORK_FAILURE);
			return GF_IP_NETWORK_FAILURE;
		}
	}
	return GF_EOS;
}

static GF_Err sockin_process(GF_Filter *filter)
{
	GF_Socket *new_conn=NULL;
	GF_Err e;
	u32 i, count;
	GF_SockInCtx *ctx = (GF_SockInCtx *) gf_filter_get_udta(filter);

	if (ctx->is_stop) return GF_EOS;

	e = gf_sk_group_select(ctx->active_sockets, 1, GF_SK_SELECT_READ);
	if (e==GF_IP_NETWORK_EMPTY) {
		if (ctx->is_udp) {
			e = sockin_check_eos(filter, ctx);
			if (e) return e;
			if (ctx->sock_c.first_pck) {
				gf_filter_ask_rt_reschedule(filter, 10000);
				return GF_OK;
			}
		} else if (!gf_list_count(ctx->clients)) {
			gf_filter_ask_rt_reschedule(filter, 5000);
			return GF_OK;
		}
		u64 sleep_for = 2*ctx->rcv_time_diff/3000;
		if (sleep_for > ctx->mwait.y) sleep_for = ctx->mwait.y;
		if (sleep_for < ctx->mwait.x) sleep_for = ctx->mwait.x;
		GF_LOG(GF_LOG_DEBUG, GF_LOG_NETWORK, ("[SockIn] empty - sleeping for "LLU" ms\n", sleep_for ));
		gf_filter_ask_rt_reschedule(filter, (u32) (sleep_for*1000) );
		return GF_OK;
	}
	else if ((e==GF_IP_CONNECTION_CLOSED) || (e==GF_EOS)) {
		ctx->is_stop = GF_TRUE;
		if (ctx->sock_c.pid)
			gf_filter_pid_set_eos(ctx->sock_c.pid);
		return e<0 ? e : 0;
	}
	else if (e) {
		return e;
	}

	ctx->last_rcv_time = 0;
	if (gf_sk_group_sock_is_set(ctx->active_sockets, ctx->sock_c.socket, GF_SK_SELECT_READ)) {
		u64 rcv_time = gf_sys_clock_high_res();
		if (ctx->last_pck_time) {
			ctx->rcv_time_diff = rcv_time - ctx->last_pck_time;
		}
		ctx->last_pck_time = rcv_time;
		if (!ctx->listen) {
			e = sockin_read_client(filter, ctx, &ctx->sock_c);
			if (e==GF_IP_NETWORK_EMPTY) return GF_OK;
			return e;
		}

		if (gf_sk_group_sock_is_set(ctx->active_sockets, ctx->sock_c.socket, GF_SK_SELECT_READ)) {
			e = gf_sk_accept(ctx->sock_c.socket, &new_conn);
			if ((e==GF_OK) && new_conn) {
				GF_SockInClient *sc=NULL;
				if (ctx->ka) {
					sc = gf_list_get(ctx->clients, 0);
					if (sc && sc->socket) {
						gf_sk_del(new_conn);
						GF_LOG(GF_LOG_INFO, GF_LOG_NETWORK, ("[SockIn] Rejecting connection since one client is already connected and keep-alive is enabled\n", sc->address));
						return GF_OK;
					}
				}
				if (!sc) {
					GF_SAFEALLOC(sc, GF_SockInClient);
					if (!sc) return GF_OUT_OF_MEM;
					gf_list_add(ctx->clients, sc);
					sc->first_pck = GF_TRUE;
				}
				sc->done = GF_FALSE;

				sc->socket = new_conn;
				strcpy(sc->address, "unknown");
				gf_sk_get_remote_address(new_conn, sc->address);
				gf_sk_set_block_mode(new_conn, !ctx->block);

				GF_LOG(GF_LOG_INFO, GF_LOG_NETWORK, ("[SockIn] Accepting new connection from %s\n", sc->address));
				ctx->had_clients = GF_TRUE;
				gf_sk_group_register(ctx->active_sockets, sc->socket);
				sc->init_time = gf_sys_clock();
			}
		}
	}
	if (!ctx->listen) return GF_OK;

	count = gf_list_count(ctx->clients);
	for (i=0; i<count; i++) {
		GF_SockInClient *sc = gf_list_get(ctx->clients, i);
		if (!sc->socket) continue;

		if (!gf_sk_group_sock_is_set(ctx->active_sockets, sc->socket, GF_SK_SELECT_READ)) continue;

	 	e = sockin_read_client(filter, ctx, sc);
	 	if (e == GF_IP_CONNECTION_CLOSED) {
			GF_LOG(ctx->ka ? GF_LOG_INFO : GF_LOG_WARNING, GF_LOG_NETWORK, ("[SockIn] Connection to %s lost, %s\n", sc->address, ctx->ka ? "entering keepalive" : "removing input"));
			if (sc->socket)
				gf_sk_group_unregister(ctx->active_sockets, sc->socket);

			sockin_client_reset(sc);
			if (ctx->ka) continue;
	 		if (sc->pid) {
	 			gf_filter_pid_set_eos(sc->pid);
	 			gf_filter_pid_remove(sc->pid);
	 		}
	 		gf_free(sc);
	 		gf_list_del_item(ctx->clients, sc);
	 		i--;
	 		count--;
		} else {
			if (e) return e;
		}
	}
	if (!ctx->had_clients) {
		//we should use socket groups and selects !
		gf_filter_ask_rt_reschedule(filter, 100000);
		return GF_OK;
	}

	if (!count) {
		if (ctx->ka) {
			//keep alive, ask for real-time reschedule of 100 ms - we should use socket groups and selects !
			gf_filter_ask_rt_reschedule(filter, 100000);
		} else {
			return GF_EOS;
		}
	}
	return GF_OK;
}



#define OFFS(_n)	#_n, offsetof(GF_SockInCtx, _n)

static const GF_FilterArgs SockInArgs[] =
{
	{ OFFS(src), "address of source content", GF_PROP_NAME, NULL, NULL, 0},
	{ OFFS(block_size), "block size used to read socket", GF_PROP_UINT, "0x60000", NULL, GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(port), "default port if not specified", GF_PROP_UINT, "1234", NULL, 0},
	{ OFFS(ifce), "default multicast interface", GF_PROP_NAME, NULL, NULL, GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(listen), "indicate the input socket works in server mode", GF_PROP_BOOL, "false", NULL, 0},
	{ OFFS(ka), "keep socket alive if no more connections", GF_PROP_BOOL, "false", NULL, GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(maxc), "max number of concurrent connections", GF_PROP_UINT, "+I", NULL, 0},
	{ OFFS(tsprobe), "probe for MPEG-2 TS data, either RTP or raw UDP. Disabled if mime or ext are given and do not match MPEG-2 TS mimes/extensions", GF_PROP_BOOL, "true", NULL, GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(ext), "indicate file extension of udp data", GF_PROP_STRING, NULL, NULL, 0},
	{ OFFS(mime), "indicate mime type of udp data", GF_PROP_STRING, NULL, NULL, 0},
	{ OFFS(block), "set blocking mode for socket(s)", GF_PROP_BOOL, "false", NULL, GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(timeout), "set timeout in ms for UDP socket(s), 0 to disable timeout", GF_PROP_UINT, "10000", NULL, GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(mwait), "set min and max wait times in ms to avoid too frequent polling", GF_PROP_VEC2I, "1x30", NULL, GF_FS_ARG_HINT_ADVANCED},

#ifndef GPAC_DISABLE_STREAMING
	{ OFFS(reorder_pck), "number of packets delay for RTP reordering (M2TS over RTP) ", GF_PROP_UINT, "100", NULL, GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(reorder_delay), "number of ms delay for RTP reordering (M2TS over RTP)", GF_PROP_UINT, "10", NULL, GF_FS_ARG_HINT_ADVANCED},
#endif

	{ OFFS(ssm), "list of IP to include for source-specific multicast", GF_PROP_STRING_LIST, NULL, NULL, GF_FS_ARG_HINT_EXPERT},
	{ OFFS(ssmx), "list of IP to exclude for source-specific multicast", GF_PROP_STRING_LIST, NULL, NULL, GF_FS_ARG_HINT_EXPERT},
	{0}
};

static const GF_FilterCapability SockInCaps[] =
{
	CAP_UINT(GF_CAPS_OUTPUT,  GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
};

GF_FilterRegister SockInRegister = {
	.name = "sockin",
	GF_FS_SET_DESCRIPTION("UDP/TCP input")
#ifndef GPAC_DISABLE_DOC
	.help = "This filter handles generic TCP and UDP input sockets. It can also probe for MPEG-2 TS over RTP input. Probing of MPEG-2 TS over UDP/RTP is enabled by default but can be turned off.\n"
		"\nData format can be specified by setting either [-ext]() or [-mime]() options. If not set, the format will be guessed by probing the first data packet\n"
		"\n"
		"- UDP sockets are used for source URLs formatted as `udp://NAME`\n"
		"- TCP sockets are used for source URLs formatted as `tcp://NAME`\n"
#ifdef GPAC_HAS_SOCK_UN
		"- UDP unix domain sockets are used for source URLs formatted as `udpu://NAME`\n"
		"- TCP unix domain sockets are used for source URLs formatted as `tcpu://NAME`\n"
#else
		"Your platform does not supports unix domain sockets, udpu:// and tcpu:// schemes not supported."
#endif
		"\n"
		"When ports are specified in the URL and the default option separators are used (see `gpac -h doc`), the URL must either:\n"
		"- have a trailing '/', e.g. `udp://localhost:1234/[:opts]`\n"
		"- use `gpac` separator, e.g. `udp://localhost:1234[:gpac:opts]`\n"
		"\n"
		"When the socket is listening in keep-alive [-ka]() mode:\n"
		"- a single connection is allowed and a single output PID will be produced\n"
		"- each connection close event will triger a pipeline flush\n"
		"\n"
#ifdef GPAC_CONFIG_DARWIN
		"\nOn OSX with VM packet replay you will need to force multicast routing, e.g. `route add -net 239.255.1.4/32 -interface vboxnet0`"
#endif
		"\n"
		"# Time Regulation\n"
		"The filter uses the time between the last two received packets to estimates how often it should check for inputs. The maximum and minimum times to wait between two calls is given by the [-mwait]() option. The maximum time may need to be reduced for very high bitrates sources.\n"
	""
	,
#endif //GPAC_DISABLE_DOC
	.private_size = sizeof(GF_SockInCtx),
	.args = SockInArgs,
	SETCAPS(SockInCaps),
	.initialize = sockin_initialize,
	.finalize = sockin_finalize,
	.process = sockin_process,
	.process_event = sockin_process_event,
	.probe_url = sockin_probe_url,
	.hint_class_type = GF_FS_CLASS_NETWORK_IO
};


const GF_FilterRegister *sockin_register(GF_FilterSession *session)
{
	if (gf_opts_get_bool("temp", "get_proto_schemes")) {
#ifdef GPAC_HAS_SOCK_UN
		gf_opts_set_key("temp_in_proto", SockInRegister.name, "tcp,udp,tcpu,udpu");
#else
		gf_opts_set_key("temp_in_proto", SockInRegister.name, "tcp,udp");
#endif
	}
	return &SockInRegister;
}

#else
const GF_FilterRegister *sockin_register(GF_FilterSession *session)
{
	return NULL;
}
#endif
