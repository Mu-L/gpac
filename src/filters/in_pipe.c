/*
 *			GPAC - Multimedia Framework C SDK
 *
 *			Authors: Jean Le Feuvre
 *			Copyright (c) Telecom ParisTech 2018-2024
 *					All rights reserved
 *
 *  This file is part of GPAC / pipe input filter
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
#include <gpac/constants.h>
#include <gpac/network.h>

#ifndef GPAC_DISABLE_PIN

#ifdef WIN32

#include <windows.h>

#else

#include <fcntl.h>
#include <unistd.h>

#if defined(GPAC_CONFIG_LINUX) || defined(GPAC_CONFIG_EMSCRIPTEN)
#include <sys/types.h>
#include <sys/stat.h>
#endif

#ifndef __BEOS__
#include <errno.h>
#endif

#endif


typedef struct
{
	//options
	char *src;
	char *ext;
	char *mime;
	u32 block_size, bpcnt, timeout;
	Bool blk, ka, mkp, sigflush, marker;

	u32 read_block_size;
	//only one output pid declared
	GF_FilterPid *pid;

#ifdef WIN32
	HANDLE pipe;
	HANDLE event;
	OVERLAPPED overlap;
#else
	int fd;
#endif
	u64 bytes_read;

	Bool is_end, pck_out, is_first, owns_pipe;
	Bool do_reconfigure;
	char *buffer;
	Bool is_stdin;
	u32 left_over, copy_offset;
	u8 store_char;
	Bool has_recfg;
	u32 last_active_ms;
} GF_PipeInCtx;

static Bool pipein_process_event(GF_Filter *filter, const GF_FilterEvent *evt);

#ifdef WIN32
#include <io.h>
#include <fcntl.h>
#endif //WIN32

static GF_Err pipein_initialize(GF_Filter *filter)
{
	GF_Err e = GF_OK;
	GF_PipeInCtx *ctx = (GF_PipeInCtx *) gf_filter_get_udta(filter);
	char *frag_par = NULL;
	char *cgi_par = NULL;
	char *src;

	if (!ctx->src) return GF_BAD_PARAM;

#ifdef WIN32
	ctx->pipe = INVALID_HANDLE_VALUE;
#else
	ctx->fd = -1;
#endif

	if (!strcmp(ctx->src, "-") || !strcmp(ctx->src, "stdin")) {
		ctx->is_stdin = GF_TRUE;
		ctx->mkp = GF_FALSE;
		if (!ctx->timeout) ctx->timeout = 10000;
#ifdef WIN32
		_setmode(_fileno(stdin), _O_BINARY);
#endif
	}
	else if (strnicmp(ctx->src, "pipe:/", 6) && strstr(ctx->src, "://"))  {
		gf_filter_setup_failure(filter, GF_NOT_SUPPORTED);
		return GF_NOT_SUPPORTED;
	}

	//strip any fragment identifier
	frag_par = strchr(ctx->src, '#');
	if (frag_par) frag_par[0] = 0;
	cgi_par = strchr(ctx->src, '?');
	if (cgi_par) cgi_par[0] = 0;

	src = (char *) ctx->src;
	if (!strnicmp(ctx->src, "pipe://", 7)) src += 7;
	else if (!strnicmp(ctx->src, "pipe:", 5)) src += 5;

	ctx->read_block_size = MIN(8192, ctx->block_size);

	if (ctx->is_stdin) {
		e = GF_OK;
		goto setup_done;
	}

	if (ctx->blk) {
		gf_filter_set_blocking(filter, GF_TRUE);
	}

#ifdef WIN32
	char szNamedPipe[GF_MAX_PATH];
	if (!strncmp(src, "\\\\", 2)) {
		strcpy(szNamedPipe, src);
	}
	else {
		strcpy(szNamedPipe, "\\\\.\\pipe\\gpac\\");
		strcat(szNamedPipe, src);
	}
	if (strchr(szNamedPipe, '/')) {
		u32 i, len = (u32)strlen(szNamedPipe);
		for (i = 0; i < len; i++) {
			if (szNamedPipe[i] == '/')
				szNamedPipe[i] = '\\';
		}
	}

	if (WaitNamedPipeA(szNamedPipe, 1) == FALSE) {
		if (!ctx->mkp) {
			GF_LOG(GF_LOG_ERROR, GF_LOG_MMIO, ("[PipeIn] Failed to open %s: %d\n", szNamedPipe, GetLastError()));
			e = GF_URL_ERROR;
			goto err_exit;
		}
		DWORD pflags = PIPE_ACCESS_INBOUND;
		DWORD flags = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE;
		if (ctx->blk) flags |= PIPE_WAIT;
		else {
			flags |= PIPE_NOWAIT;
			pflags |= FILE_FLAG_OVERLAPPED;
			if (!ctx->event) ctx->event = CreateEvent(NULL, TRUE, FALSE, NULL);
			if (!ctx->event) {
				e = GF_IO_ERR;
				goto err_exit;
			}
			ctx->overlap.hEvent = ctx->event;
		}
		ctx->pipe = CreateNamedPipe(szNamedPipe, pflags, flags, 10, ctx->read_block_size, ctx->read_block_size, 0, NULL);

		if (ctx->pipe == INVALID_HANDLE_VALUE) {
			GF_LOG(GF_LOG_ERROR, GF_LOG_MMIO, ("[PipeIn] Failed to create named pipe %s: %d\n", szNamedPipe, GetLastError()));
			e = GF_URL_ERROR;
			goto err_exit;
		}
		if (ctx->blk) {
			GF_LOG(GF_LOG_WARNING, GF_LOG_MMIO, ("[PipeIn] Waiting for client connection for %s, blocking\n", szNamedPipe));
		}
		if (!ConnectNamedPipe(ctx->pipe, ctx->blk ? NULL : &ctx->overlap) && (GetLastError() != ERROR_PIPE_CONNECTED) && (GetLastError() != ERROR_PIPE_LISTENING)) {
			GF_LOG(GF_LOG_ERROR, GF_LOG_MMIO, ("[PipeOut] Failed to connect named pipe %s: %d\n", szNamedPipe, GetLastError()));
			e = GF_IO_ERR;
			CloseHandle(ctx->pipe);
			ctx->pipe = INVALID_HANDLE_VALUE;
		}
		else {
			ctx->owns_pipe = GF_TRUE;
		}
	}
	else {
		ctx->pipe = CreateFile(szNamedPipe, GENERIC_READ, ctx->blk ? PIPE_WAIT : PIPE_NOWAIT, NULL, OPEN_EXISTING, 0, NULL);
		if (ctx->pipe == INVALID_HANDLE_VALUE) {
			GF_LOG(GF_LOG_ERROR, GF_LOG_MMIO, ("[PipeIn] Failed to open %s: %d\n", szNamedPipe, GetLastError()));
			e = GF_URL_ERROR;
		}
	}

err_exit:

#else
	if (!gf_file_exists(src) && ctx->mkp) {

#ifdef GPAC_CONFIG_DARWIN
		mknod(src,S_IFIFO | 0666, 0);
#else
		mkfifo(src, 0666);
#endif
		ctx->owns_pipe = GF_TRUE;
	}

	ctx->fd = open(src, ctx->blk ? O_RDONLY : O_RDONLY|O_NONBLOCK );

	if (ctx->fd < 0) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_MMIO, ("[PipeIn] Failed to open %s: %s\n", src, gf_errno_str(errno)));
		e = GF_URL_ERROR;
	}
#endif

setup_done:

	if (e) {
		if (frag_par) frag_par[0] = '#';
		if (cgi_par) cgi_par[0] = '?';

		gf_filter_setup_failure(filter, GF_URL_ERROR);
		ctx->owns_pipe = GF_FALSE;
		return GF_URL_ERROR;
	}
	GF_LOG(GF_LOG_INFO, GF_LOG_MMIO, ("[PipeIn] opening %s\n", src));

	ctx->is_end = GF_FALSE;

	if (frag_par) frag_par[0] = '#';
	if (cgi_par) cgi_par[0] = '?';

	ctx->is_first = GF_TRUE;
	if (!ctx->buffer)
		ctx->buffer = gf_malloc(ctx->block_size +1);

	gf_filter_post_process_task(filter);

#ifdef GPAC_ENABLE_COVERAGE
	if (gf_sys_is_cov_mode()) {
		pipein_process_event(NULL, NULL);
	}
#endif
	return GF_OK;
}


static void pipein_finalize(GF_Filter *filter)
{
	GF_PipeInCtx *ctx = (GF_PipeInCtx *) gf_filter_get_udta(filter);

	if (!ctx->is_stdin) {
#ifdef WIN32
		if (ctx->pipe != INVALID_HANDLE_VALUE) CloseHandle(ctx->pipe);
#else
		if (ctx->fd>=0) close(ctx->fd);
#endif
		if (ctx->owns_pipe)
			gf_file_delete(ctx->src);
	}
	if (ctx->buffer) gf_free(ctx->buffer);

}

static GF_FilterProbeScore pipein_probe_url(const char *url, const char *mime_type)
{
	if (!strnicmp(url, "pipe://", 7)) return GF_FPROBE_SUPPORTED;
	else if (!strnicmp(url, "pipe:", 5)) return GF_FPROBE_SUPPORTED;
	else if (!strcmp(url, "-") || !strcmp(url, "stdin")) return GF_FPROBE_SUPPORTED;

	return GF_FPROBE_NOT_SUPPORTED;
}

static Bool pipein_process_event(GF_Filter *filter, const GF_FilterEvent *evt)
{
	GF_PipeInCtx *ctx;
	if (!filter || !evt) return GF_TRUE;

	ctx = (GF_PipeInCtx *) gf_filter_get_udta(filter);
	if (evt->base.on_pid && (evt->base.on_pid != ctx->pid))
		return GF_TRUE;

	switch (evt->base.type) {
	case GF_FEVT_PLAY:
		return GF_TRUE;
	case GF_FEVT_STOP:
		//stop sending data
		ctx->is_end = GF_TRUE;
		gf_filter_pid_set_eos(ctx->pid);
		return GF_TRUE;
	case GF_FEVT_SOURCE_SEEK:
		GF_LOG(GF_LOG_WARNING, GF_LOG_MMIO, ("[PipeIn] Seek request not possible on pipes, ignoring\n"));
		return GF_TRUE;
	case GF_FEVT_SOURCE_SWITCH:
		gf_fatal_assert(ctx->is_end);
		if (evt->seek.source_switch) {
			GF_LOG(GF_LOG_WARNING, GF_LOG_MMIO, ("[PipeIn] source switch request not possible on pipes, ignoring\n"));
		}
		pipein_initialize(filter);
		gf_filter_post_process_task(filter);
		break;
	default:
		break;
	}
	return GF_TRUE;
}

#define PIPE_FLUSH_MARKER	"GPACPIF"
#define PIPE_RECFG_MARKER	"GPACPIR"
#define PIPE_CLOSE_MARKER	"GPACPIC"

static void pipein_pck_destructor(GF_Filter *filter, GF_FilterPid *pid, GF_FilterPacket *pck)
{
	GF_PipeInCtx *ctx = (GF_PipeInCtx *) gf_filter_get_udta(filter);
	u32 size;
	gf_filter_pck_get_data(pck, &size);
	ctx->buffer[size] = ctx->store_char;
	ctx->pck_out = GF_FALSE;
	//ready to process again
	gf_filter_post_process_task(filter);
}

static GF_Err pipein_process(GF_Filter *filter)
{
	GF_Err e;
	u32 total_read;
	s32 nb_read;
	GF_FilterPacket *pck;
	GF_PipeInCtx *ctx = (GF_PipeInCtx *) gf_filter_get_udta(filter);

	if (ctx->is_end)
		return GF_EOS;

	//until packet is released we return EOS (no processing), and ask for processing again upon release
	if (ctx->pck_out)
		return GF_EOS;

	if (ctx->pid && gf_filter_pid_would_block(ctx->pid)) {
		gf_assert(0);
		return GF_OK;
	}

	total_read = 0;
	if (ctx->left_over) {
		if (ctx->copy_offset)
			memmove(ctx->buffer, ctx->buffer + ctx->copy_offset, ctx->left_over);
		total_read = ctx->left_over;
		ctx->left_over = 0;
		ctx->copy_offset = 0;
	}
	if (ctx->has_recfg) {
		ctx->do_reconfigure = GF_TRUE;
		ctx->has_recfg = GF_FALSE;
	}

	if (!total_read && ctx->timeout) {
		u32 now = gf_sys_clock();
		if (!ctx->last_active_ms) {
			ctx->last_active_ms = now;
		} else if (now - ctx->last_active_ms > ctx->timeout) {
			GF_LOG(GF_LOG_WARNING, GF_LOG_MMIO, ("[PipeIn] Timeout detected after %d ms, aborting\n", now - ctx->last_active_ms ));
			if (ctx->pid) {
				gf_filter_pid_set_eos(ctx->pid);
			} else {
				gf_filter_setup_failure(filter, GF_SERVICE_ERROR);
			}
			ctx->is_end = GF_TRUE;
			return GF_EOS;
		}
	}


refill:

	if (ctx->is_stdin) {
		nb_read = 0;
		if (feof(stdin)) {
			if (!ctx->ka) {
				gf_filter_pid_set_eos(ctx->pid);
				return GF_EOS;
			} else if (ctx->sigflush) {
				gf_filter_pid_send_flush(ctx->pid);
				ctx->bytes_read = 0;
			}
		} else {
			nb_read = (s32) fread(ctx->buffer + total_read, 1, ctx->read_block_size-total_read, stdin);
			if (!total_read && (nb_read<0)) {
				if (!ctx->ka) {
					gf_filter_pid_set_eos(ctx->pid);
					return GF_EOS;
				}
			}
		}
	} else {
		errno = 0;
#ifdef WIN32
		nb_read = -1;
		if (!ctx->blk && ctx->mkp) {
			DWORD res = WaitForMultipleObjects(1, &ctx->event, FALSE, 1);
			ResetEvent(ctx->event);
			if (res == WAIT_FAILED) {
				GF_LOG(GF_LOG_ERROR, GF_LOG_MMIO, ("[PipeIn] WaitForMultipleObjects failed!\n"));
				return GF_IO_ERR;
			}
			if (GetOverlappedResult(ctx->pipe, &ctx->overlap, &res, FALSE) == 0) {
				s32 error = GetLastError();
				if (error == ERROR_IO_INCOMPLETE) {
				}
				else {
					CloseHandle(ctx->pipe);
					ctx->pipe = INVALID_HANDLE_VALUE;
					if (!ctx->ka) {
						GF_LOG(GF_LOG_DEBUG, GF_LOG_MMIO, ("[PipeIn] end of stream detected\n"));
						gf_filter_pid_set_eos(ctx->pid);
						return GF_EOS;
					}
					GF_LOG(GF_LOG_INFO, GF_LOG_MMIO, ("[PipeIn] Pipe closed by remote side, reopening!\n"));
					if (ctx->sigflush) {
						gf_filter_pid_send_flush(ctx->pid);
						ctx->bytes_read = 0;
					}
					return pipein_initialize(filter);
				}
			}
		}
		if (! ReadFile(ctx->pipe, ctx->buffer + total_read, ctx->read_block_size-total_read, (LPDWORD) &nb_read, ctx->blk ? NULL : &ctx->overlap)) {
			if (total_read) {
				nb_read = 0;
			} else {
				s32 error = GetLastError();
				if (error == ERROR_PIPE_LISTENING) return GF_OK;
				else if ((error == ERROR_IO_PENDING) || (error== ERROR_MORE_DATA)) {
					//non blocking pipe with writers active
				}
				else if (nb_read<0) {
					GF_LOG(GF_LOG_ERROR, GF_LOG_MMIO, ("[PipeIn] Failed to read, error %d\n", error));
					return GF_IO_ERR;
				}
				else if (!ctx->ka && ctx->blk) {
					GF_LOG(GF_LOG_DEBUG, GF_LOG_MMIO, ("[PipeIn] end of stream detected\n"));
					gf_filter_pid_set_eos(ctx->pid);
					CloseHandle(ctx->pipe);
					ctx->pipe = INVALID_HANDLE_VALUE;
					ctx->is_end = GF_TRUE;
					return GF_EOS;
				}
				else if (error == ERROR_BROKEN_PIPE) {
					if (ctx->ka) {
						if (ctx->bpcnt) {
							ctx->bpcnt--;
							if (!ctx->bpcnt) {
								gf_filter_pid_set_eos(ctx->pid);
								return GF_EOS;
							}
						}
						GF_LOG(GF_LOG_INFO, GF_LOG_MMIO, ("[PipeIn] Pipe closed by remote side, reopening!\n"));
						CloseHandle(ctx->pipe);
						ctx->pipe = INVALID_HANDLE_VALUE;
						if (ctx->sigflush) {
							gf_filter_pid_send_flush(ctx->pid);
							ctx->bytes_read = 0;
						}
						return pipein_initialize(filter);
					} else {
						gf_filter_pid_set_eos(ctx->pid);
						return GF_EOS;
					}
				}
				if (!ctx->bytes_read)
					gf_filter_ask_rt_reschedule(filter, 10000);
				else
					gf_filter_ask_rt_reschedule(filter, 1000);
				return GF_OK;
			}
		}
#else
		nb_read = (s32) read(ctx->fd, ctx->buffer + total_read, ctx->read_block_size-total_read);
		if (nb_read <= 0) {
			if (total_read) {
				nb_read = 0;
			} else {
				s32 res = errno;
				//writers still active
				if (res == EAGAIN) {
				}
				//broken pipe
				else if (nb_read < 0) {
					GF_LOG(GF_LOG_ERROR, GF_LOG_MMIO, ("[PipeIn] Failed to read, error %s\n", gf_errno_str(res) ));
					return GF_IO_ERR;
				}
				//wait for data
				else if (ctx->bytes_read) {
					if (ctx->ka && ctx->bpcnt) {
						ctx->bpcnt--;
						if (!ctx->bpcnt) {
							GF_LOG(GF_LOG_INFO, GF_LOG_MMIO, ("[PipeIn] exiting keep-alive mode\n"));
							ctx->ka = GF_FALSE;
						}
					}
					if (!ctx->ka) {
						GF_LOG(GF_LOG_INFO, GF_LOG_MMIO, ("[PipeIn] end of stream detected\n"));
						if (ctx->pid) gf_filter_pid_set_eos(ctx->pid);
						close(ctx->fd);
						ctx->fd=-1;
						ctx->is_end = GF_TRUE;
						return GF_EOS;
					}

					//signal flush
					if (ctx->sigflush && ctx->pid) {
						gf_filter_pid_send_flush(ctx->pid);
					}
					//reset for longer reschedule time
					ctx->bytes_read = 0;
				}
				if (!ctx->bytes_read) gf_filter_ask_rt_reschedule(filter, 10000);
				else gf_filter_ask_rt_reschedule(filter, 1000);
				return GF_OK;
			}
		}
#endif
	}

	if (nb_read) {
		total_read += nb_read;
		if (!ctx->left_over && (total_read + ctx->read_block_size < ctx->block_size)) {
			nb_read = 0;
			goto refill;
		}
		ctx->last_active_ms = 0;
	}
	nb_read = total_read;

	Bool has_marker=GF_FALSE;
	if (ctx->marker) {
		u8 *start = ctx->buffer;
		u32 avail = nb_read;
		if (nb_read<8) {
			ctx->left_over = nb_read;
			nb_read = 0;
			ctx->copy_offset = 0;
		}
		while (nb_read) {
			u8 *m = memchr(start, PIPE_FLUSH_MARKER[0], avail);
			if (!m) break;
			u32 remain = (u32) ((u8*) ctx->buffer + nb_read - m);
			if (remain<8) {
				//check if we start with 'GPACPI', if not move to next 'G'
				u32 check = MIN(remain, 6);
				if (memcmp(m, PIPE_FLUSH_MARKER, check)) {
					start = m+1;
					avail = (u32) ((u8*)ctx->buffer+nb_read - start);
					continue;
				}
				ctx->left_over = remain;
				nb_read -= remain;
				ctx->copy_offset = (u32) (m - (u8*)ctx->buffer);
				break;
			}
			if (!memcmp(m, PIPE_FLUSH_MARKER, 7) && ((m[7]=='\0') || (m[7]=='\n'))) {
				ctx->left_over = remain-8;
				nb_read = (u32) (m - (u8*)ctx->buffer);
				ctx->copy_offset = nb_read+8;
				has_marker = GF_TRUE;
				GF_LOG(GF_LOG_INFO, GF_LOG_MMIO, ("[PipeIn] Found flush marker\n"));
				break;
			}
			if (!memcmp(m, PIPE_RECFG_MARKER, 7) && ((m[7]=='\0') || (m[7]=='\n'))) {
				ctx->left_over = remain-8;
				nb_read = (u32) (m - (u8*)ctx->buffer);
				ctx->copy_offset = nb_read+8;
				ctx->has_recfg = GF_TRUE;
				GF_LOG(GF_LOG_INFO, GF_LOG_MMIO, ("[PipeIn] Found reconfig marker\n"));
				break;
			}
			if (!memcmp(m, PIPE_CLOSE_MARKER, 7) && ((m[7]=='\0') || (m[7]=='\n'))) {
				nb_read = (u32) (m - (u8*)ctx->buffer);
				ctx->copy_offset = 0;
				ctx->left_over=0;
				ctx->bytes_read=8; //for above test
				ctx->ka = GF_FALSE;
				GF_LOG(GF_LOG_INFO, GF_LOG_MMIO, ("[PipeIn] Found close marker\n"));
				break;
			}
			start = m+1;
			avail = (u32) ((u8*)ctx->buffer+nb_read - start);
		}
	}

	if (!nb_read) {
		if (has_marker) {
			gf_filter_pid_send_flush(ctx->pid);
		}
		if (!ctx->bytes_read)
			gf_filter_ask_rt_reschedule(filter, 10000);
		else if (!total_read)
			gf_filter_ask_rt_reschedule(filter, 1000);
		return GF_OK;
	}

	ctx->store_char = ctx->buffer[nb_read];
	ctx->buffer[nb_read] = 0;
	if (!ctx->pid || ctx->do_reconfigure) {
		GF_LOG(GF_LOG_INFO, GF_LOG_MMIO, ("[PipeIn] configuring stream %d probe bytes\n", nb_read));
		ctx->do_reconfigure = GF_FALSE;
		e = gf_filter_pid_raw_new(filter, ctx->src, NULL, ctx->mime, ctx->ext, ctx->buffer, nb_read, GF_TRUE, &ctx->pid);
		if (e) {
			GF_LOG(GF_LOG_WARNING, GF_LOG_MMIO, ("[PipeIn] failed to configure stream: %s\n", gf_error_to_string(e) ));
			return e;
		}
		gf_filter_pid_set_property(ctx->pid, GF_PROP_PID_FILE_CACHED, &PROP_BOOL(GF_FALSE) );
		gf_filter_pid_set_property(ctx->pid, GF_PROP_PID_PLAYBACK_MODE, &PROP_UINT(GF_PLAYBACK_MODE_NONE) );
	}
	pck = gf_filter_pck_new_shared(ctx->pid, ctx->buffer, nb_read, pipein_pck_destructor);
	if (!pck) return GF_OUT_OF_MEM;

	GF_LOG(GF_LOG_DEBUG, GF_LOG_MMIO, ("[PipeIn] Got %d bytes\n", nb_read));
	gf_filter_pck_set_framing(pck, ctx->is_first, ctx->is_end);
	gf_filter_pck_set_sap(pck, GF_FILTER_SAP_1);

	ctx->is_first = GF_FALSE;
	ctx->pck_out = GF_TRUE;
	gf_filter_pck_send(pck);
	ctx->bytes_read += nb_read;

	if (has_marker) {
		gf_filter_pid_send_flush(ctx->pid);
	}
	if (ctx->is_end) {
		gf_filter_pid_set_eos(ctx->pid);
		return GF_EOS;
	}
	return ctx->pck_out ? GF_EOS : GF_OK;
}



#define OFFS(_n)	#_n, offsetof(GF_PipeInCtx, _n)

static const GF_FilterArgs PipeInArgs[] =
{
	{ OFFS(src), "name of source pipe", GF_PROP_NAME, NULL, NULL, 0},
	{ OFFS(block_size), "buffer size used to read pipe", GF_PROP_UINT, "5000", NULL, GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(ext), "indicate file extension of pipe data", GF_PROP_STRING, NULL, NULL, 0},
	{ OFFS(mime), "indicate mime type of pipe data", GF_PROP_STRING, NULL, NULL, 0},
	{ OFFS(blk), "open pipe in block mode", GF_PROP_BOOL, "false", NULL, GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(ka), "keep-alive pipe when end of input is detected", GF_PROP_BOOL, "false", NULL, GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(mkp), "create pipe if not found", GF_PROP_BOOL, "false", NULL, 0},
	{ OFFS(sigflush), "signal end of stream upon pipe close - cf filter help", GF_PROP_BOOL, "false", NULL, GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(marker), "inspect payload for flush and reconfigure signals - cf filter help", GF_PROP_BOOL, "false", NULL, GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(bpcnt), "number of broken pipe allowed before exiting, 0 means forever", GF_PROP_UINT, "0", NULL, GF_FS_ARG_HINT_EXPERT},
	{ OFFS(timeout), "timeout in ms before considering input is in end of stream (0: no timeout)", GF_PROP_UINT, "0", NULL, GF_FS_ARG_HINT_ADVANCED},

	{0}
};

static const GF_FilterCapability PipeInCaps[] =
{
	CAP_UINT(GF_CAPS_OUTPUT,  GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
};

GF_FilterRegister PipeInRegister = {
	.name = "pin",
	GF_FS_SET_DESCRIPTION("Pipe input")
	GF_FS_SET_HELP( "This filter handles generic input pipes (mono-directional) in blocking or non blocking mode.\n"
		"Warning: Input pipes cannot seek.\n"
		"Data format of the pipe may be specified using extension (either in file name or through [-ext]()) or MIME type through [-mime]().\n"
		"Note: Unless disabled at session level (see [-no-probe](CORE) ), file extensions are usually ignored and format probing is done on the first data block.\n"
		"\n"
		"# stdin pipe\n"
		"The filter can handle reading from stdin, by using `-` or `stdin` as input file name.\n"
		"EX gpac -i - vout\n"
		"EX gpac -i stdin vout\n"
		"\n"
		"When reading from stdin, the default [timeout]() is 10 seconds.\n"
		"# Named pipes\n"
		"The filter can handle reading from named pipes. The associated protocol scheme is `pipe://` when loaded as a generic input (e.g. `-i pipe://URL` where URL is a relative or absolute pipe name).\n"
		"On Windows hosts, the default pipe prefix is `\\\\.\\pipe\\gpac\\` if no prefix is set.\n"
		"`dst=mypipe` resolves in `\\\\.\\pipe\\gpac\\mypipe`\n"
		"`dst=\\\\.\\pipe\\myapp\\mypipe` resolves in `\\\\.\\pipe\\myapp\\mypipe`\n"
		"Any destination name starting with `\\\\` is used as is, with `\\` translated in `/`.\n"
		"\n"
		"Input pipes are created by default in non-blocking mode.\n"
		"\n"
		"The filter can create the pipe if not found using [-mkp](). On windows hosts, this will create a pipe server.\n"
		"On non windows hosts, the created pipe will delete the pipe file upon filter destruction.\n"
		"  \n"
		"Input pipes can be setup to run forever using [-ka](). In this case:\n"
		"- any potential pipe close on the writing side will be ignored\n"
		"- pipeline flushing will be triggered upon pipe close if [-sigflush]() is set\n"
		"- final end of stream will be triggered upon session close.\n"
		"  \n"
		"This can be useful to pipe raw streams from different process into gpac:\n"
		"- Receiver side: `gpac -i pipe://mypipe:ext=.264:mkp:ka`\n"
		"- Sender side: `cat raw1.264 > mypipe && gpac -i raw2.264 -o pipe://mypipe:ext=.264`"
		"  \n"
		"The pipeline flush is signaled as EOS while keeping the stream active.\n"
		"This is typically needed for mux filters waiting for EOS to flush their data.\n"
		"  \n"
		"If [-marker]() is set, the following strings (all 8-bytes with `\0` or `\n` terminator) will be scanned:\n"
		"- `GPACPIF`: triggers a pipeline flush event\n"
		"- `GPACPIR`: triggers a reconfiguration of the format (used to signal mux type changes)\n"
		"- `GPACPIC`: triggers a regular end of stream and aborts keepalive\n"
		"The [-marker]() mode should be used carefully as it will slow down pipe processing (higher CPU usage and delayed output).\n"
		"Warning: Usage of pipeline flushing may not be properly supported by some filters.\n"
		"  \n"
		"The pipe input can be created in blocking mode or non-blocking mode.\n"
	"")
	.private_size = sizeof(GF_PipeInCtx),
	.args = PipeInArgs,
	SETCAPS(PipeInCaps),
	.initialize = pipein_initialize,
	.finalize = pipein_finalize,
	.process = pipein_process,
	.process_event = pipein_process_event,
	.probe_url = pipein_probe_url,
	.hint_class_type = GF_FS_CLASS_NETWORK_IO
};


const GF_FilterRegister *pin_register(GF_FilterSession *session)
{
	if (gf_opts_get_bool("temp", "get_proto_schemes")) {
		gf_opts_set_key("temp_in_proto", PipeInRegister.name, "pipe");
	}
	return &PipeInRegister;
}
#else
const GF_FilterRegister *pin_register(GF_FilterSession *session)
{
	return NULL;
}
#endif // GPAC_DISABLE_PIN

