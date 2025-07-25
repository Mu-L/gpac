/*
 *			GPAC - Multimedia Framework C SDK
 *
 *			Authors: Jean Le Feuvre
 *			Copyright (c) Telecom ParisTech 2017-2025
 *					All rights reserved
 *
 *  This file is part of GPAC / filters sub-project
 *
 *  GPAC is free software; you can redistribute it and/or modify
 *  it under the terfsess of the GNU Lesser General Public License as published by
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

#include "filter_session.h"
#include <gpac/network.h>

//helper functions
void gf_void_del(void *p)
{
	gf_free(p);
}

void gf_filterpacket_del(void *p)
{
	GF_FilterPacket *pck=(GF_FilterPacket *)p;
	if (pck->data) gf_free(pck->data);
	gf_free(p);
}

static void gf_filter_parse_args(GF_Filter *filter, const char *args, GF_FilterArgType arg_type, Bool for_script);

const char *gf_fs_path_escape_colon_ex(GF_FilterSession *sess, const char *path, Bool *needs_escape, Bool for_source)
{
	const char *res;
	char szEsc[6];
	if (needs_escape) *needs_escape = GF_FALSE;
	if (!path) return NULL;
	if (sess->sep_args != ':') {
		res = strchr(path, sess->sep_args);
	} else {

		res = gf_url_colon_suffix(path, sess->sep_name);
		//if path is one of this proto, check if we have a port specified
		if (!strncmp(path, "tcp://", 6)
			|| !strncmp(path, "udp://", 6)
			|| !strncmp(path, "tcpu://", 7)
			|| !strncmp(path, "udpu://", 7)
			|| !strncmp(path, "rtp://", 6)
			|| !strncmp(path, "route://", 8)
			|| !strncmp(path, "mabr://", 7)
		) {
			char *sep2 = res ? strchr(res+1, ':') : NULL;
			char *sep3 = res ? strchr(res+1, '/') : NULL;
			if (sep2 && sep3 && (sep2>sep3)) {
				sep2 = strchr(sep3, ':');
			}
			if (sep2 || sep3 || res) {
				u32 port = 0;
				if (sep2) {
					sep2[0] = 0;
					if (sep3) sep3[0] = 0;
				}
				else if (sep3) sep3[0] = 0;
				if (sscanf(res+1, "%d", &port)==1) {
					char szPort[20];
					snprintf(szPort, 20, "%d", port);
					if (strcmp(res+1, szPort))
						port = 0;
				}
				if (sep2) sep2[0] = ':';
				if (sep3) sep3[0] = '/';

				if (port) res = sep2;
			}
		}
	}
	if (!res) return NULL;
	//double sep, allways consider this is a forced escape
	if (res[1]==sess->sep_args) return res;

	sprintf(szEsc, "%cgpac", sess->sep_args);
	//if we have an explicit :gpac: separator, don't further analyze what is before
	char *sep = strstr(path, szEsc);
	if (sep && ((sep[5]==sess->sep_args) || (sep[5]==0)))
		return sep;
	//for local files, check if file/dir exists for each ':' specified
	//this allows for file path with ':'
	if (!strncmp(path, "file://", 7) || !strstr(path, "://")) {
		sep = (char*)res;
		while (1) {
			if (sep) sep[0] = 0;
			Bool ok;
			//for source check if file exists
			if (for_source) {
				char *frag = strrchr(path, '#');
				if (frag) frag[0] = 0;
				ok = gf_file_exists(path);
				if (frag) frag[0] = '#';
			}
			//for dest check if target dir exists
			else {
				char *frag = strrchr(path, '/');
				if (frag) frag[0] = 0;
				ok = gf_dir_exists(path);
				if (frag) frag[0] = '/';
			}
			//file/dir exists and has ':' in its name, we will need to escape options
			if (ok && needs_escape && strchr(path, sess->sep_args))
				*needs_escape = GF_TRUE;
			if (sep) sep[0] = sess->sep_args;
			if (ok) {
				return sep;
			}
			if (!sep) break;
			sep = strchr(sep+1, sess->sep_args);
		}
	}
	return res;
}
const char *gf_fs_path_escape_colon(GF_FilterSession *sess, const char *path)
{
	return gf_fs_path_escape_colon_ex(sess, path, NULL, GF_FALSE);
}
const char *gf_filter_path_escape_colon(GF_Filter *f, const char *path)
{
	if (!f) return NULL;
	return gf_fs_path_escape_colon_ex(f->session, path, NULL, GF_FALSE);
}

static const char *gf_filter_get_args_stripped(GF_FilterSession *fsess, const char *in_args, Bool is_dst)
{
	char szEscape[7];
	char *args_striped = NULL;
	if (in_args) {
		const char *key;
		if (is_dst) {
			key = "dst";
		} else {
			key = "src";
		}
		if (!strncmp(in_args, key, 3) && (in_args[3]==fsess->sep_name)) {
			args_striped = (char *) in_args;
		} else {
			char szDst[6];
			sprintf(szDst, "%c%s%c", fsess->sep_name, key, fsess->sep_name);
			args_striped = strstr(in_args, szDst);
		}

		if (args_striped) {
			args_striped += 4;
			if (!strncmp(args_striped, "gcryp://", 8))
				args_striped += 8;
			args_striped = (char *)gf_fs_path_escape_colon(fsess, args_striped);
			if (args_striped) args_striped ++;
		} else {
			args_striped = (char *)in_args;
		}
	}
	sprintf(szEscape, "gpac%c", fsess->sep_args);
	if (args_striped && !strncmp(args_striped, szEscape, 5))
		return args_striped + 5;

	return args_striped;
}

const char *gf_filter_get_dst_args(GF_Filter *filter)
{
	return gf_filter_get_args_stripped(filter->session, filter->dst_args, GF_TRUE);
}
const char *gf_filter_get_src_args(GF_Filter *filter)
{
	return filter->orig_args ? filter->orig_args : filter->src_args;
}

char *gf_filter_get_dst_name(GF_Filter *filter)
{
	char szDst[5];
	char *dst, *arg_sep, *res, *dst_args;
	sprintf(szDst, "dst%c", filter->session->sep_name);

	dst_args = filter->dst_args;
	if (!dst_args) {
		GF_FilterPid *outpid = gf_list_get(filter->output_pids, 0);
		if (outpid) dst_args = outpid->filter->dst_args;

		if (!dst_args) {
			GF_Filter *outf = gf_list_get(filter->destination_links, 0);
			if (!outf || !outf->dst_args)
				outf = gf_list_get(filter->destination_filters, 0);
			if (outf)
				dst_args = filter->dst_args;
		}
	}
	dst = dst_args ? strstr(dst_args, szDst) : NULL;
	if (!dst) return NULL;

	arg_sep = (char*) gf_fs_path_escape_colon(filter->session, dst+4);

	if (arg_sep) {
		arg_sep[0] = 0;
		res = gf_strdup(dst+4);
		arg_sep[0] = filter->session->sep_args;
	} else {
		res = gf_strdup(dst+4);
	}
	return res;
}

/*push arguments from source and dest into the final argument string. We strip the following
- FID: will be inherited from last explicit filter in the chain
- SID: is cloned during the resolution while loading the filter chain
- TAG: TAG is never inherited
- RSID: requesting sourceID on target filters is is never inherited
- local options: by definition these options only apply to the loaded filter, and are never inherited
- user-assigned PID properties on destination (they only apply after the destination, not in the new chain).

Note that user-assigned PID properties assigned on source are inherited so that
	-i SRC:#PidProp=PidVal -o dst

will have all intermediate filters loaded for the resolution (eg demux) set this property.

*/
static void filter_push_args(GF_FilterSession *fsess, char **out_args, char *in_args, Bool is_src, Bool first_sep_inserted)
{
	char szSep[2];
	Bool prev_is_db_sep = GF_FALSE;
	szSep[0] = fsess->sep_args;
	szSep[1] = 0;
	while (in_args) {
		char *sep = strchr(in_args, fsess->sep_args);
		if (sep) sep[0] = 0;

		if (!strncmp(in_args, "gfloc", 5) && (!in_args[5] || (in_args[5]==fsess->sep_args))) {
			if (sep) sep[0] = fsess->sep_args;
			return;
		}
		if (!strncmp(in_args, "FID", 3) && (in_args[3]==fsess->sep_name)) {
		}
		else if (!strncmp(in_args, "SID", 3) && (in_args[3]==fsess->sep_name)) {
		}
		else if (!strncmp(in_args, "TAG", 3) && (in_args[3]==fsess->sep_name)) {
		}
		else if (!strncmp(in_args, "FS", 2) && (in_args[2]==fsess->sep_name)) {
		}
		else if (!strncmp(in_args, "RSID", 4) && (!in_args[4] || (in_args[4]==fsess->sep_args))) {
		}
		else if (!strncmp(in_args, "FBT", 3) && (in_args[3]==fsess->sep_name)) {
		}
		else if (!strncmp(in_args, "FBU", 3) && (in_args[3]==fsess->sep_name)) {
		}
		else if (!strncmp(in_args, "DL", 2) && (in_args[2]==fsess->sep_name)) {
		}
		else if (!strncmp(in_args, "LT", 2) && (in_args[2]==fsess->sep_name)) {
		}
		else if (!is_src && (in_args[0]==fsess->sep_frag)) {

		} else {
			if (*out_args && !first_sep_inserted) {
				gf_dynstrcat(out_args, szSep, NULL);
				if (prev_is_db_sep)
					gf_dynstrcat(out_args, szSep, NULL);
			}
			gf_dynstrcat(out_args, in_args, NULL);
			first_sep_inserted = GF_FALSE;
		}
		if (!sep) break;
		sep[0] = fsess->sep_args;
		in_args = sep+1;
		prev_is_db_sep = GF_FALSE;
		if (in_args[0] == fsess->sep_args) {
			in_args ++;
			prev_is_db_sep = GF_TRUE;
		}
	}
}

GF_Filter *gf_filter_new(GF_FilterSession *fsess, const GF_FilterRegister *freg, const char *src_args, const char *dst_args, GF_FilterArgType arg_type, GF_Err *err, GF_Filter *multi_sink_target, Bool is_dynamic_filter)
{
	char szName[200];
	const char *dst_striped = NULL;
	const char *src_striped = NULL;
	GF_Filter *filter;
	GF_Err e;
	u32 i;
	if (!fsess) return NULL;

	//check if this is a sink, if so move to GF_FILTER_ARG_EXPLICIT_SINK (for force_demux setup)
	if (src_args && (arg_type == GF_FILTER_ARG_EXPLICIT)) {
		char *dst_arg = strstr(src_args, "dst");
		if (dst_arg && (dst_arg[3]==fsess->sep_name))
			arg_type = GF_FILTER_ARG_EXPLICIT_SINK;
	}

	GF_SAFEALLOC(filter, GF_Filter);
	if (!filter) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Failed to alloc filter for %s\n", freg->name));
		return NULL;
	}
	filter->freg = freg;
	filter->session = fsess;
	filter->max_extra_pids = freg->max_extra_pids;
	filter->dynamic_filter = is_dynamic_filter ? 1 : 0;
	filter->require_source_id = (fsess->flags & GF_FS_FLAG_REQUIRE_SOURCE_ID) ? GF_TRUE : GF_FALSE;

#ifdef GPAC_HAS_QJS
	filter->jsval = JS_UNDEFINED;
#endif

	if (fsess->use_locks) {
		snprintf(szName, 200, "Filter%sPackets", filter->freg->name);
		filter->pcks_mx = gf_mx_new(szName);
	}

	//for now we always use a lock on the filter task lists
	//this mutex protects the task list and the number of process virtual tasks
	//we cannot remove it in non-threaded mode since we have no garantee that a filter won't use threading on its own
	snprintf(szName, 200, "Filter%sTasks", filter->freg->name);
	filter->tasks_mx = gf_mx_new(szName);

	filter->tasks = gf_fq_new(filter->tasks_mx);

	if (! (filter->session->flags & GF_FS_FLAG_NO_RESERVOIR)) {
		filter->pcks_shared_reservoir = gf_fq_new(filter->pcks_mx);
		filter->pcks_alloc_reservoir = gf_fq_new(filter->pcks_mx);
		filter->pcks_inst_reservoir = gf_fq_new(filter->pcks_mx);
	}

	filter->pending_pids = gf_fq_new(NULL);

	filter->blacklisted = gf_list_new();
	filter->destination_filters = gf_list_new();
	filter->destination_links = gf_list_new();
	filter->temp_input_pids = gf_list_new();

	filter->bundle_idx_at_resolution = -1;
	filter->cap_idx_at_resolution = -1;

	gf_mx_p(fsess->filters_mx);
	gf_list_add(fsess->filters, filter);
	gf_mx_v(fsess->filters_mx);

	filter->multi_sink_target = multi_sink_target;

	src_striped = src_args;
	dst_striped = NULL;
	switch (arg_type) {
	case GF_FILTER_ARG_EXPLICIT_SOURCE_NO_DST_INHERIT:
		filter->arg_type = arg_type = GF_FILTER_ARG_EXPLICIT_SOURCE;
		filter->no_dst_arg_inherit = GF_TRUE;
		break;
	case GF_FILTER_ARG_INHERIT_SOURCE_ONLY:
		filter->arg_type = arg_type = GF_FILTER_ARG_INHERIT;
		filter->no_dst_arg_inherit = GF_TRUE;
		break;
	default:
		filter->arg_type = arg_type;
		dst_striped = gf_filter_get_args_stripped(fsess, dst_args, GF_TRUE);
		break;
	}

	if ((arg_type!=GF_FILTER_ARG_EXPLICIT_SOURCE) && (arg_type!=GF_FILTER_ARG_EXPLICIT)) {
		src_striped = gf_filter_get_args_stripped(fsess, src_args, GF_FALSE);
	}

	//if we already concatenated our dst args to this source filter (eg this is an intermediate dynamically loaded one)
	//don't reappend the args
	if (dst_striped && src_striped && strstr(src_striped, dst_striped) != NULL) {
		dst_striped = NULL;
	}

	if (src_striped && dst_striped) {
		char *all_args;
		const char *dbsep;
		char *localarg_marker;
		u32 nb_db_sep=0, src_arg_len;
		char szDBSep[3];
		Bool insert_escape = GF_FALSE;
		Bool dst_sep_inserted = GF_FALSE;
		//source has a URL (local or not), escape it to make sure we don't pass dst args as params to the URL
		if ((strstr(src_striped, "src=") || strstr(src_striped, "dst=")) && strstr(src_striped, "://")){
			char szEscape[10];
			sprintf(szEscape, "%cgpac", fsess->sep_args);
			if (strstr(src_striped, szEscape)==NULL) {
				insert_escape = GF_TRUE;
			}
		}

		szDBSep[0] = szDBSep[1] = filter->session->sep_args;
		szDBSep[2] = 0;

		//handle the case where the first src arg was escaped ("filter::opt"), src args is now ":opt
		//consider we have one double sep
		if (src_striped[0] == filter->session->sep_args)
			nb_db_sep = 1;

		dbsep = src_striped;
		while (dbsep) {
			char *next_dbsep = strstr(dbsep, szDBSep);
			if (!next_dbsep) break;
			nb_db_sep++;
			dbsep = next_dbsep+2;
			//this happens when we had cat :opt1 and ::opt2, this results in :opt1:::opt2
			//consider that this is a new escaped option
			while (dbsep[0] == filter->session->sep_args) {
				nb_db_sep++;
				dbsep++;
			}
		}
		if (nb_db_sep % 2) nb_db_sep=1;
		else nb_db_sep=0;


		if (!nb_db_sep) {
			szDBSep[1] = 0;
		}
		//src_striped is ending with our separator, don't insert a new one
		src_arg_len = (u32)strlen(src_striped);
		if (src_arg_len && (src_striped[src_arg_len-1] == filter->session->sep_args)) {
			szDBSep[0] = 0;
		}

		//push src args
		all_args = NULL;
		filter_push_args(fsess, &all_args, (char *) src_striped, GF_TRUE, GF_FALSE);

		if (all_args && insert_escape) {
			gf_dynstrcat(&all_args, szDBSep, NULL);
			gf_dynstrcat(&all_args, "gpac", NULL);
			dst_sep_inserted = GF_TRUE;
		} else if (all_args) {
			if (strlen(all_args))
				gf_dynstrcat(&all_args, szDBSep, NULL);
			dst_sep_inserted = GF_TRUE;
		}
		//push dst args
		filter_push_args(fsess, &all_args, (char *) dst_striped, GF_FALSE, dst_sep_inserted);
		if (all_args && (!strcmp(all_args, szDBSep) || !all_args[0])) {
			gf_free(all_args);
			all_args=NULL;
		}


		localarg_marker = all_args ? strstr(all_args, "gfloc") : NULL;
		if (localarg_marker) {
			localarg_marker[0]=0;
			if (strlen(all_args) && (localarg_marker[-1]==fsess->sep_args))
				localarg_marker[-1]=0;
		}
		e = gf_filter_new_finalize(filter, all_args, arg_type);
		filter->orig_args = all_args;
		src_striped = NULL;
	} else if (dst_striped) {
		//remove local args from dst
		char *localarg_marker = strstr(dst_striped, "gfloc");
		if (localarg_marker) {
			localarg_marker--;
			if (localarg_marker[0]!=filter->session->sep_args)
				localarg_marker = NULL;
			else
				localarg_marker[0] = 0;
		}
		e = gf_filter_new_finalize(filter, dst_striped, arg_type);
		filter->orig_args = gf_strdup(dst_striped);
		if (localarg_marker) localarg_marker[0] = filter->session->sep_args;
		src_striped = NULL;
	} else {
		e = gf_filter_new_finalize(filter, src_striped, arg_type);
	}
	filter->dst_args = dst_args ? gf_strdup(dst_args) : NULL;

	if (e) {
		if (!filter->setup_notified && (e<0) ) {
			GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Error %s while instantiating filter %s\n", gf_error_to_string(e),freg->name));
			gf_filter_setup_failure(filter, e);
		}
		if (err) *err = e;
		//filter requested cancelation of filter session upon init
		if (e==GF_EOS) {
			fsess->run_status = GF_EOS;
			GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Filter %s requested cancelation of filter session\n", freg->name));
		}
		return NULL;
	}
	if (filter && src_striped)
		filter->orig_args = gf_strdup(src_striped);

	for (i=0; i<freg->nb_caps; i++) {
		if (freg->caps[i].flags & GF_CAPFLAG_OUTPUT) {
			filter->has_out_caps = GF_TRUE;
			break;
		}
	}
	if (filter) {
		GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Created filter register %s (%p) args %s\n", freg->name, filter, filter->orig_args ? filter->orig_args : "none"));
	}

#ifndef GPAC_DISABLE_THREADS
	if ((freg->flags & GF_FS_REG_SINGLE_THREAD) && gf_list_count(filter->session->threads)) {
		u32 count = gf_list_count(filter->session->threads);
		GF_SessionThread *ft;
		u32 idx=0;
		u32 min_th_assigned = 0;
		for (i=0; i<count; i++) {
			ft = gf_list_get(filter->session->threads, i);
			if (!idx || (min_th_assigned>ft->nb_filters_pinned)) {
				idx = i+1;
				min_th_assigned = ft->nb_filters_pinned;
			}
		}
		ft = gf_list_get(filter->session->threads, idx-1);
		safe_int_inc(&ft->nb_filters_pinned);
		filter->restrict_th_idx = idx;
	}
#endif
	return filter;
}

void gf_filter_check_pending_pids(GF_Filter *filter)
{
	//flush all pending pid init requests
	if (filter->has_pending_pids && !filter->deferred_link) {
		filter->has_pending_pids=GF_FALSE;
		while (gf_fq_count(filter->pending_pids)) {
			GF_FilterPid *pid=gf_fq_pop(filter->pending_pids);
			gf_filter_pid_post_init_task(filter, pid);
		}
	}
}

GF_Err gf_filter_new_finalize(GF_Filter *filter, const char *args, GF_FilterArgType arg_type)
{
	gf_filter_set_name(filter, NULL);

	gf_filter_parse_args(filter, args, arg_type, GF_FALSE);

	if (! filter->dynamic_filter && (filter->session->flags & GF_FS_FLAG_FORCE_DEFER_LINK)) {
		filter->deferred_link = GF_TRUE;
	}

#ifdef GPAC_CONFIG_EMSCRIPTEN
	//not runing as worker and using sync read, force main thread
	if ((filter->freg->flags & GF_FS_REG_USE_SYNC_READ) && !filter->session->is_worker)
		gf_filter_force_main_thread(filter, GF_TRUE);
#endif

	if (filter->removed) {
		if (!(filter->freg->flags & (GF_FS_REG_TEMP_INIT|GF_FS_REG_META))) {
			filter->finalized = GF_TRUE;
			return GF_OK;
		}
	}

	if (filter->freg->initialize) {
		GF_Err e;
		FSESS_CHECK_THREAD(filter)
		if (((arg_type==GF_FILTER_ARG_EXPLICIT_SOURCE) || (arg_type==GF_FILTER_ARG_EXPLICIT_SINK)) && !filter->orig_args) {
			filter->orig_args = (char *)args;
			e = filter->freg->initialize(filter);
			filter->orig_args = NULL;
		} else {
			e = filter->freg->initialize(filter);
		}
		if (e) return e;
	}
	if ((filter->freg->flags & GF_FS_REG_SCRIPT) && filter->freg->update_arg) {
		GF_Err e;
		gf_filter_parse_args(filter, args, arg_type, GF_TRUE);
		char *next_args = strchr(args, filter->session->sep_args);
		filter->orig_args = (char*)next_args;
		e = filter->freg->update_arg(filter, NULL, NULL);
		filter->orig_args = NULL;
		if (e) return e;
	}

	//flush all pending pid init requests
	gf_filter_check_pending_pids(filter);

#ifdef GPAC_HAS_QJS
	jsfs_on_filter_created(filter);
#endif
	if (filter->session->on_filter_create_destroy)
		filter->session->on_filter_create_destroy(filter->session->rt_udta, filter, GF_FALSE);
	return GF_OK;
}

void gf_filter_reset_pending_packets(GF_Filter *filter)
{
	//may happen when a filter is removed from the chain
	if (filter->postponed_packets) {
		while (gf_list_count(filter->postponed_packets)) {
			GF_FilterPacket *pck = gf_list_pop_front(filter->postponed_packets);
			gf_filter_packet_destroy(pck);
		}
		gf_list_del(filter->postponed_packets);
		filter->postponed_packets = NULL;
	}
}

static void reset_filter_args(GF_Filter *filter);

//when destroying the filter queue we have to skip tasks marked as notified, since they are also present in the
//session task list
void task_del(void *_task)
{
	GF_FSTask *task = _task;
	if (!task->notified) gf_free(task);
}

void gf_filter_del(GF_Filter *filter)
{
	gf_assert(filter);
	GF_LOG(GF_LOG_INFO, GF_LOG_FILTER, ("Filter %s destruction\n", filter->name));
	gf_assert(!filter->detach_pid_tasks_pending);
	gf_assert(!filter->swap_pidinst_src);


#ifdef GPAC_HAS_QJS
	jsfs_on_filter_destroyed(filter);
#endif

	if (filter->session->on_filter_create_destroy)
		filter->session->on_filter_create_destroy(filter->session->rt_udta, filter, GF_TRUE);

#ifndef GPAC_DISABLE_3D
	gf_list_del_item(filter->session->gl_providers, filter);
	gf_fs_check_gl_provider(filter->session);
#endif

#ifdef GPAC_MEMORY_TRACKING
	if (filter->session->check_allocs) {
		if (filter->max_nb_process>10 && (filter->max_nb_consecutive_process * 10 < filter->max_nb_process)) {
			GF_LOG(GF_LOG_WARNING, GF_LOG_FILTER, ("\nFilter %s extensively uses memory alloc/free in process(): \n", filter->name));
			GF_LOG(GF_LOG_WARNING, GF_LOG_FILTER, ("\tmax stats of over %d calls (%d consecutive calls with no alloc/free):\n", filter->max_nb_process, filter->max_nb_consecutive_process));
			GF_LOG(GF_LOG_WARNING, GF_LOG_FILTER, ("\t\t%d allocs %d callocs %d reallocs %d free\n", filter->max_stats_nb_alloc, filter->max_stats_nb_calloc, filter->max_stats_nb_realloc, filter->max_stats_nb_free));
			GF_LOG(GF_LOG_WARNING, GF_LOG_FILTER, ("\tPlease consider rewriting the code\n"));
		}
	}
#endif

	//may happen when a filter is removed from the chain
	gf_filter_reset_pending_packets(filter);

	//delete output pids before the packet reservoir
	while (gf_list_count(filter->output_pids)) {
		gf_filter_pid_del(gf_list_pop_back(filter->output_pids));
	}
	gf_list_del(filter->output_pids);

	//delete input pids not yet destroyed (may happen upon setup failure)
	while (gf_list_count(filter->input_pids)) {
		gf_filter_pid_inst_del(gf_list_pop_back(filter->input_pids));
	}
	gf_list_del(filter->input_pids);


	gf_list_del(filter->blacklisted);
	gf_list_del(filter->destination_filters);
	gf_list_del(filter->destination_links);
	gf_list_del(filter->source_filters);
	gf_list_del(filter->temp_input_pids);

	gf_fq_del(filter->tasks, task_del);
	gf_fq_del(filter->pending_pids, NULL);

	reset_filter_args(filter);
	if (filter->src_args) gf_free(filter->src_args);

	if (filter->pcks_shared_reservoir)
		gf_fq_del(filter->pcks_shared_reservoir, gf_void_del);
	if (filter->pcks_inst_reservoir)
		gf_fq_del(filter->pcks_inst_reservoir, gf_void_del);
	if (filter->pcks_alloc_reservoir)
		gf_fq_del(filter->pcks_alloc_reservoir, gf_filterpacket_del);

	gf_mx_del(filter->pcks_mx);
	if (filter->tasks_mx)
		gf_mx_del(filter->tasks_mx);

	if (filter->id) gf_free(filter->id);
	if (filter->source_ids) gf_free(filter->source_ids);
	if (filter->dynamic_source_ids) gf_free(filter->dynamic_source_ids);
	if (filter->filter_udta) gf_free(filter->filter_udta);
	if (filter->orig_args) gf_free(filter->orig_args);
	if (filter->dst_args) gf_free(filter->dst_args);
	if (filter->name) gf_free(filter->name);
	if (filter->status_str) gf_free(filter->status_str);
	if (filter->restricted_source_id) gf_free(filter->restricted_source_id);
	if (filter->tag) gf_free(filter->tag);
	if (filter->itag) gf_free(filter->itag);

	if (!filter->session->in_final_flush && !filter->session->run_status) {
		u32 i, count;
		gf_mx_p(filter->session->filters_mx);
		count = gf_list_count(filter->session->filters);
		for (i=0; i<count; i++) {
			GF_Filter *a_filter = gf_list_get(filter->session->filters, i);
			gf_mx_p(a_filter->tasks_mx);
			gf_list_del_item(a_filter->destination_filters, filter);
			gf_list_del_item(a_filter->destination_links, filter);
			gf_list_del_item(a_filter->source_filters, filter);
			if (a_filter->cap_dst_filter==filter)
				a_filter->cap_dst_filter = NULL;
			if (a_filter->cloned_from == filter)
				a_filter->cloned_from = NULL;
			if (a_filter->cloned_instance == filter)
				a_filter->cloned_instance = NULL;
			if (a_filter->on_setup_error_filter == filter)
				a_filter->on_setup_error_filter = NULL;
			if (a_filter->target_filter == filter)
				a_filter->target_filter = NULL;
			if (a_filter->dst_filter == filter)
				a_filter->dst_filter = NULL;
			gf_mx_v(a_filter->tasks_mx);
		}
		gf_mx_v(filter->session->filters_mx);
	}
	if (filter->skip_cids.vals) {
		GF_PropertyValue prop;
		prop.value.string_list = filter->skip_cids;
		prop.type = GF_PROP_STRING_LIST;
		gf_props_reset_single(&prop);
	}

	if (filter->instance_description)
		gf_free(filter->instance_description);
	if (filter->instance_version)
		gf_free(filter->instance_version);
	if (filter->instance_author)
		gf_free(filter->instance_author);
	if (filter->instance_help)
		gf_free(filter->instance_help);

	if (filter->meta_instances)
		gf_free(filter->meta_instances);

	if (filter->netcap_id)
		gf_free(filter->netcap_id);

#ifndef GPAC_DISABLE_LOG
	if (filter->logs) {
		gf_log_pop_extra(filter->logs);
		if (filter->logs->tools) gf_free(filter->logs->tools);
		if (filter->logs->levels) gf_free(filter->logs->levels);
		gf_free(filter->logs);
	}
#endif

#ifdef GPAC_HAS_QJS
	if (filter->iname)
		gf_free(filter->iname);
#endif

#ifndef GPAC_DISABLE_THREADS
	if (filter->restrict_th_idx) {
		GF_SessionThread *ft = gf_list_get(filter->session->threads, filter->restrict_th_idx-1);
		safe_int_dec(&ft->nb_filters_pinned);
	}
#endif

	if (filter->freg && (filter->freg->flags & GF_FS_REG_CUSTOM)) {
		//external custom filters
		if (! (filter->freg->flags & GF_FS_REG_SCRIPT) && filter->forced_caps) {
			u32 i;
			for (i=0; i<filter->nb_forced_caps; i++) {
				if (filter->forced_caps[i].name)
					gf_free((char *) filter->forced_caps[i].name);
				gf_props_reset_single( (GF_PropertyValue *) & filter->forced_caps[i].val);
			}
			gf_free( (void *) filter->forced_caps);
		}
		gf_filter_sess_reset_graph(filter->session, filter->freg);
		gf_free( (char *) filter->freg->name);
		gf_free( (void *) filter->freg);
	}
	gf_free(filter);
}

GF_EXPORT
void *gf_filter_get_udta(GF_Filter *filter)
{
	gf_assert(filter);

	return filter->filter_udta;
}

GF_EXPORT
const char * gf_filter_get_name(GF_Filter *filter)
{
	gf_assert(filter);
	if (filter->name)
		return (const char *)filter->name;
	return (const char *)filter->freg->name;
}

GF_EXPORT
void gf_filter_set_name(GF_Filter *filter, const char *name)
{
	gf_assert(filter);

	if (filter->name) gf_free(filter->name);
	filter->name = gf_strdup(name ? name : filter->freg->name);
}

void gf_filter_set_id(GF_Filter *filter, const char *ID)
{
	gf_assert(filter);

	if (filter->id) gf_free(filter->id);
	filter->id = ID ? gf_strdup(ID) : NULL;
}

GF_EXPORT
const char * gf_filter_get_status(GF_Filter *filter)
{
	gf_assert(filter);
	return filter->status_str ? (const char *) filter->status_str : "" ;
}

GF_EXPORT
u64 gf_filter_get_bytes_done(GF_Filter *filter)
{
	gf_assert(filter);
	return filter->nb_bytes_processed ;
}


GF_EXPORT
void gf_filter_reset_source(GF_Filter *filter)
{
	if (filter && filter->source_ids) {
		gf_mx_p(filter->session->filters_mx);
		gf_free(filter->source_ids);
		filter->source_ids = NULL;
		gf_mx_v(filter->session->filters_mx);
	}
}

static void gf_filter_set_sources(GF_Filter *filter, const char *sources_ID)
{
	gf_assert(filter);

	gf_mx_p(filter->session->filters_mx);

	if (!sources_ID) {
		if (filter->source_ids) gf_free(filter->source_ids);
		filter->source_ids = NULL;
	} else if (!filter->source_ids) {
		filter->source_ids = gf_strdup(sources_ID);
	} else {
		char *found = strstr(filter->source_ids, sources_ID);
		if (found) {
			u32 len = (u32) strlen(sources_ID);
			if ((found[len]==0) || (found[len]==',')) {
				gf_mx_v(filter->session->filters_mx);
				return;
			}
		}
		gf_dynstrcat(&filter->source_ids, sources_ID, ",");
	}

	gf_mx_v(filter->session->filters_mx);
}

static void gf_filter_set_arg(GF_Filter *filter, const GF_FilterArgs *a, GF_PropertyValue *argv)
{
#ifdef WIN32
	void *ptr = (void *) (((char*) filter->filter_udta) + a->offset_in_private);
#else
	void *ptr = filter->filter_udta + a->offset_in_private;
#endif
	Bool res = GF_FALSE;
	if (a->offset_in_private<0) return;

	switch (argv->type) {
	case GF_PROP_BOOL:
		if (a->offset_in_private + sizeof(Bool) <= filter->freg->private_size) {
			*(Bool *)ptr = argv->value.boolean;
			res = GF_TRUE;
		}
		break;
	case GF_PROP_SINT:
		if (a->offset_in_private + sizeof(s32) <= filter->freg->private_size) {
			*(s32 *)ptr = argv->value.sint;
			res = GF_TRUE;
		}
		break;
	case GF_PROP_UINT:
	case GF_PROP_4CC:
		if (a->offset_in_private + sizeof(u32) <= filter->freg->private_size) {
			*(u32 *)ptr = argv->value.uint;
			res = GF_TRUE;
		}
		break;
	case GF_PROP_LSINT:
		if (a->offset_in_private + sizeof(s64) <= filter->freg->private_size) {
			*(s64 *)ptr = argv->value.longsint;
			res = GF_TRUE;
		}
		break;
	case GF_PROP_LUINT:
		if (a->offset_in_private + sizeof(u64) <= filter->freg->private_size) {
			*(u64 *)ptr = argv->value.longuint;
			res = GF_TRUE;
		}
		break;
	case GF_PROP_FLOAT:
		if (a->offset_in_private + sizeof(Fixed) <= filter->freg->private_size) {
			*(Fixed *)ptr = argv->value.fnumber;
			res = GF_TRUE;
		}
		break;
	case GF_PROP_DOUBLE:
		if (a->offset_in_private + sizeof(Double) <= filter->freg->private_size) {
			*(Double *)ptr = argv->value.number;
			res = GF_TRUE;
		}
		break;
	case GF_PROP_FRACTION:
		if (a->offset_in_private + sizeof(GF_Fraction) <= filter->freg->private_size) {
			*(GF_Fraction *)ptr = argv->value.frac;
			res = GF_TRUE;
		}
		break;
	case GF_PROP_FRACTION64:
		if (a->offset_in_private + sizeof(GF_Fraction64) <= filter->freg->private_size) {
			*(GF_Fraction64 *)ptr = argv->value.lfrac;
			res = GF_TRUE;
		}
		break;
	case GF_PROP_VEC2I:
		if (a->offset_in_private + sizeof(GF_PropVec2i) <= filter->freg->private_size) {
			*(GF_PropVec2i *)ptr = argv->value.vec2i;
			res = GF_TRUE;
		}
		break;
	case GF_PROP_VEC2:
		if (a->offset_in_private + sizeof(GF_PropVec2) <= filter->freg->private_size) {
			*(GF_PropVec2 *)ptr = argv->value.vec2;
			res = GF_TRUE;
		}
		break;
	case GF_PROP_VEC3I:
		if (a->offset_in_private + sizeof(GF_PropVec3i) <= filter->freg->private_size) {
			*(GF_PropVec3i *)ptr = argv->value.vec3i;
			res = GF_TRUE;
		}
		break;
	case GF_PROP_VEC4I:
		if (a->offset_in_private + sizeof(GF_PropVec4i) <= filter->freg->private_size) {
			*(GF_PropVec4i *)ptr = argv->value.vec4i;
			res = GF_TRUE;
		}
		break;

	case GF_PROP_NAME:
	case GF_PROP_STRING:
		if (a->offset_in_private + sizeof(char *) <= filter->freg->private_size) {
			if (*(char **)ptr) gf_free( * (char **)ptr);
			//we don't strdup since we don't free the string at the caller site
			*(char **)ptr = argv->value.string;
			res = GF_TRUE;
		}
		if (argv->value.string && !strncmp(argv->value.string, "gfio://", 7)) {
			if (gf_fileio_is_main_thread(argv->value.string)) {
				gf_filter_force_main_thread(filter, GF_TRUE);
			}
		}
		break;
	case GF_PROP_DATA:
	case GF_PROP_DATA_NO_COPY:
	case GF_PROP_CONST_DATA:
		if (a->offset_in_private + sizeof(GF_PropData) <= filter->freg->private_size) {
			GF_PropData *pd = (GF_PropData *) ptr;
			if ((argv->type!=GF_PROP_CONST_DATA) && pd->ptr) gf_free(pd->ptr);
			//we don't free/alloc  since we don't free the string at the caller site
			pd->size = argv->value.data.size;
			pd->ptr = argv->value.data.ptr;
			res = GF_TRUE;
		}
		break;
	case GF_PROP_POINTER:
		if (a->offset_in_private + sizeof(void *) <= filter->freg->private_size) {
			*(void **)ptr = argv->value.ptr;
			res = GF_TRUE;
		}
		break;
	case GF_PROP_STRING_LIST:
		if (a->offset_in_private + sizeof(void *) <= filter->freg->private_size) {
			u32 k;
			GF_PropStringList *l = (GF_PropStringList *)ptr;
			for (k=0; k<l->nb_items; k++) {
				gf_free(l->vals[k]);
			}
			if (l->vals) gf_free(l->vals);
			//we don't clone since we don't free the string at the caller site
			*l = argv->value.string_list;
			res = GF_TRUE;
		}
		break;
	case GF_PROP_UINT_LIST:
	case GF_PROP_4CC_LIST:
	case GF_PROP_SINT_LIST:
	case GF_PROP_VEC2I_LIST:
		//use uint_list as base type for lists
		if (a->offset_in_private + sizeof(void *) <= filter->freg->private_size) {
			GF_PropUIntList *l = (GF_PropUIntList *)ptr;
			if (l->vals) gf_free(l->vals);
			*l = argv->value.uint_list;
			res = GF_TRUE;
		}
		break;
	default:
		if (gf_props_type_is_enum(argv->type)) {
			if (a->offset_in_private + sizeof(u32) <= filter->freg->private_size) {
				*(u32 *)ptr = argv->value.uint;
				res = GF_TRUE;
			}
			break;
		}
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Property type %s not supported for filter argument\n", gf_props_get_type_name(argv->type) ));
		return;
	}
	if (!res) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Failed to set argument %s: memory offset %d overwrite structure size %f\n", a->arg_name, a->offset_in_private, filter->freg->private_size));
	}
}


void filter_solve_prop_template(GF_Filter *filter, GF_FilterPid *pid, char **value)
{
	char ref_prop_dump[GF_PROP_DUMP_ARG_SIZE];
	u32 ainc_crc, i;
	GF_FSAutoIncNum *auto_int=NULL;
	u32 inc_count;
	Bool assigned=GF_FALSE;
	s32 max_int = 0;
	s32 increment=1;
	char szInt[100];

	char *search_str = *value;
	while (1) {
		char *step_sep;
		char *inc_end, *inc_sep;
		char *s1 = strchr(search_str, '$');
		char *s2 = strchr(search_str, '@');
		if (s1 && s2 && (s2<s1)) s1 = NULL;
		inc_sep = s1 ? s1 : s2;

		if (!inc_sep) return;
		if (strncmp(inc_sep+1, "GINC(", 5)) {
			char *next = pid ?  strchr(inc_sep+1, inc_sep[0]) : NULL;
			if (!next) {
				search_str = inc_sep+1;
				continue;
			}
			//check for prop
			next[0] = 0;

			const GF_PropertyValue *src_prop=NULL;
			u32 ref_p4cc = gf_props_get_id(inc_sep+1);

			if (ref_p4cc)
				src_prop = gf_filter_pid_get_property(pid, ref_p4cc);
			else
				src_prop = gf_filter_pid_get_property_str(pid, inc_sep+1);

			char *solved = src_prop ? (char*) gf_props_dump(ref_p4cc, src_prop, ref_prop_dump, GF_PROP_DUMP_DATA_INFO) : "";
			inc_sep[0] = 0;
			char *new_val = gf_strdup(*value);
			gf_dynstrcat(&new_val, solved, NULL);
			u32 len = (u32) strlen(new_val);
			gf_dynstrcat(&new_val, next+1, NULL);
			gf_free(*value);
			*value = new_val;
			search_str = new_val + len;
			continue;
		}

		inc_end = strstr(inc_sep, ")");
		if (!inc_end) return;

		inc_sep[0] = 0;
		inc_end[0] = 0;
		inc_end+=1;
		strncpy(szInt, inc_sep+6, GF_ARRAY_LENGTH(szInt));
		szInt[ GF_ARRAY_LENGTH(szInt) - 1 ] = 0;
		ainc_crc = (u32) gf_crc_32(szInt, (u32) strlen(szInt) );
		step_sep = strchr(szInt, ',');
		if (step_sep) {
			step_sep[0] = 0;
			sscanf(step_sep+1, "%d", &increment);
		}

		inc_count = gf_list_count(filter->session->auto_inc_nums);
		for (i=0; i<inc_count; i++) {
			auto_int = gf_list_get(filter->session->auto_inc_nums, i);
			if (auto_int->crc != ainc_crc) {
				auto_int=NULL;
				continue;
			}
			if ((auto_int->filter == filter) && (auto_int->pid==pid)) {
				sprintf(szInt, "%d", auto_int->inc_val);
				break;
			}

			if (!assigned)
				max_int = auto_int->inc_val;
			else if ((increment>0) && (max_int < auto_int->inc_val))
				max_int = auto_int->inc_val;
			else if ((increment<0) && (max_int > auto_int->inc_val))
				max_int = auto_int->inc_val;

			assigned = GF_TRUE;
			auto_int = NULL;
		}
		if (!auto_int) {
			GF_SAFEALLOC(auto_int, GF_FSAutoIncNum);
			if (auto_int) {
				auto_int->filter = filter;
				auto_int->pid = pid;
				auto_int->crc = ainc_crc;
				if (assigned) auto_int->inc_val = max_int + increment;
				else sscanf(szInt, "%d", &auto_int->inc_val);
				gf_list_add(filter->session->auto_inc_nums, auto_int);
			}
		}
		if (auto_int) {
			sprintf(szInt, "%d", auto_int->inc_val);
			char *new_val = gf_strdup(*value);
			gf_dynstrcat(&new_val, szInt, NULL);
			gf_dynstrcat(&new_val, inc_end, NULL);
			u32 len = (u32) strlen(new_val);
			gf_free(*value);
			*value = new_val;
			search_str = new_val + len;
		} else {
			search_str = inc_sep + 1;
		}
	}
}

GF_PropertyValue gf_filter_parse_prop_solve_env_var(GF_FilterSession *fs, GF_Filter *f, u32 type, const char *name, const char *value, const char *enum_values)
{
	char szPath[GF_MAX_PATH];
	GF_PropertyValue argv;

	if (!value) return gf_props_parse_value(type, name, NULL, enum_values, fs->sep_list);

	if (f && strstr(value, "$GINC(")) {
		char *a_value = gf_strdup(value);
		filter_solve_prop_template(f, NULL, &a_value);
		argv = gf_props_parse_value(type, name, a_value, enum_values, fs->sep_list);
		gf_free(a_value);
		return argv;
	}
	if (value[0]=='$') {
		if (!strnicmp(value, "$GSHARE", 7)) {
			if (gf_opts_default_shared_directory(szPath)) {
				strcat(szPath, value+7);
				value = szPath;
			} else {
				GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Failed to query GPAC shared resource directory location\n"));
			}
		}
		else if (!strnicmp(value, "$GDOCS", 6) || !strnicmp(value, "$GCFG", 5)) {
			if (gf_sys_solve_path(value, szPath)) {
				value = szPath;
			} else {
				GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Failed to query GPAC user document directory location\n"));
			}
		}
		else if (!strnicmp(value, "$GJS", 4)) {
			Bool gf_fs_solve_js_script(char *szPath, const char *file_name, const char *file_ext);

			Bool found = gf_fs_solve_js_script(szPath, value+4, NULL);

			if (!found) {
				GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Failed solve to %s in GPAC script directories, file not found\n", value));
			}
		}
		else if (!strnicmp(value, "$GLANG", 6)) {
			value = gf_opts_get_key("core", "lang");
			if (!value) value = "en";
		}
		else if (!strnicmp(value, "$GUA", 4)) {
			value = gf_opts_get_key("core", "user-agent");
			if (!value) value = "GPAC " GPAC_VERSION;
		}
	}
	argv = gf_props_parse_value(type, name, value, enum_values, fs->sep_list);
	return argv;
}

Bool gf_filter_update_arg_apply(GF_Filter *filter, const char *arg_name, const char *arg_value, Bool is_sync_call)
{
	u32 i=0;
	//find arg
	while (filter->freg->args) {
		GF_PropertyValue argv;
		const GF_FilterArgs *a = &filter->freg->args[i];
		i++;
		Bool is_meta = GF_FALSE;
		if (!a || !a->arg_name) break;

		if ((a->flags & GF_FS_ARG_META) && !strcmp(a->arg_name, "*")) {
			if (!filter->freg->update_arg)
				continue;
			is_meta = GF_TRUE;
		} else if (strcmp(a->arg_name, arg_name)) {
			continue;
		}
		//we found the argument

		if (!is_meta && ! (a->flags & (GF_FS_ARG_UPDATE|GF_FS_ARG_UPDATE_SYNC) ) ) {
			GF_LOG(GF_LOG_WARNING, GF_LOG_FILTER, ("Argument %s of filter %s is not updatable - ignoring\n", a->arg_name, filter->name));
			return GF_TRUE;
		}

		if (a->flags & GF_FS_ARG_UPDATE_SYNC) {
			if (!is_sync_call) return GF_TRUE;
		}

		argv = gf_filter_parse_prop_solve_env_var(filter->session, filter, a->arg_type, a->arg_name, arg_value, a->min_max_enum);

		if (argv.type != GF_PROP_FORBIDDEN) {
			GF_Err e = GF_OK;
			if (!is_sync_call) {
				FSESS_CHECK_THREAD(filter)
			}
			//if no update function consider the arg OK
			if (filter->freg->update_arg) {
				e = filter->freg->update_arg(filter, arg_name, &argv);
			}
			if (e==GF_OK) {
				if (!is_meta)
					gf_filter_set_arg(filter, a, &argv);
			} else if (e!=GF_NOT_FOUND) {
				GF_LOG(GF_LOG_WARNING, GF_LOG_FILTER, ("Filter %s did not accept update of arg %s to value %s: %s\n", filter->name, arg_name, arg_value, gf_error_to_string(e) ));
			}
		} else {
			GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Failed to parse argument %s value %s\n", a->arg_name, a->arg_default_val));
		}
		return GF_TRUE;
	}
	return GF_FALSE;
}
void gf_filter_update_arg_task(GF_FSTask *task)
{
	GF_FilterUpdate *arg=task->udta;

	Bool found = gf_filter_update_arg_apply(task->filter, arg->name, arg->val, GF_FALSE);

	if (!found) {
		if (arg->recursive) {
			u32 i;
			GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Failed to locate argument %s in filter %s, propagating %s the filter chain\n", arg->name, task->filter->freg->name,
				(arg->recursive & (GF_FILTER_UPDATE_UPSTREAM|GF_FILTER_UPDATE_DOWNSTREAM)) ? "up and down" : ((arg->recursive & GF_FILTER_UPDATE_UPSTREAM) ? "up" : "down") ));

			GF_List *flist = gf_list_new();
			if (arg->recursive & GF_FILTER_UPDATE_UPSTREAM) {
				gf_mx_p(task->filter->tasks_mx);
				for (i=0; i<task->filter->num_output_pids; i++) {
					u32 j;
					GF_FilterPid *pid = gf_list_get(task->filter->output_pids, i);
					for (j=0; j<pid->num_destinations; j++) {
						GF_FilterPidInst *pidi = gf_list_get(pid->destinations, j);
						if (gf_list_find(flist, pidi->filter)<0) gf_list_add(flist, pidi->filter);
					}
				}
				for (i=0; i<gf_list_count(flist); i++) {
					GF_Filter *a_f = gf_list_get(flist, i);
					//only allow upstream propagation
					gf_fs_send_update(task->filter->session, NULL, a_f, arg->name, arg->val, GF_FILTER_UPDATE_UPSTREAM);
				}
				gf_list_reset(flist);
				gf_mx_v(task->filter->tasks_mx);
			}
			if (arg->recursive & GF_FILTER_UPDATE_DOWNSTREAM) {
				gf_mx_p(task->filter->tasks_mx);
				for (i=0; i<task->filter->num_input_pids; i++) {
					GF_FilterPidInst *pidi = gf_list_get(task->filter->input_pids, i);
					if (gf_list_find(flist, pidi->pid->filter)<0) gf_list_add(flist, pidi->pid->filter);
				}

				for (i=0; i<gf_list_count(flist); i++) {
					GF_Filter *a_f = gf_list_get(flist, i);
					//only allow downstream propagation
					gf_fs_send_update(task->filter->session, NULL, a_f, arg->name, arg->val, GF_FILTER_UPDATE_DOWNSTREAM);
				}

				gf_mx_v(task->filter->tasks_mx);
			}

			gf_list_del(flist);
		} else {
			GF_LOG(GF_LOG_WARNING, GF_LOG_FILTER, ("Failed to locate argument %s in filter %s\n", arg->name, task->filter->freg->name));
		}
	}
	gf_free(arg->name);
	gf_free(arg->val);
	gf_free(arg);
}

#ifndef GPAC_DISABLE_LOG
u32 gf_log_parse_tool(const char *logs);
void filter_parse_logs(GF_Filter *filter, const char *_logs)
{
	if (filter->logs) {
		if (filter->logs->tools) gf_free(filter->logs->tools);
		if (filter->logs->levels) gf_free(filter->logs->levels);
		gf_log_pop_extra(filter->logs);
		gf_free(filter->logs);
	}
	GF_SAFEALLOC(filter->logs, GF_LogExtra);
	if (!filter->logs) return;

	GF_LogExtra *lf = filter->logs;
	char *c_logs = gf_strdup(_logs);
	char *logs=c_logs;

	while (logs) {
		u32 i, level = 0;
		char *l_str=NULL;
		char *l_tool=NULL;
		char *l_strict=NULL;

		char *sep = strchr(logs, '@');
		char *next = sep ? strchr(sep, ':') : NULL;
		if (next) next[0] = 0;

		l_str = logs;
		if (sep) {
			sep[0] = 0;
			l_str = sep+1;
			l_tool = logs;
			l_strict = strstr(l_str, "+strict");
			if (l_strict) l_strict[0] = 0;
		} else {
			l_tool = "all";
		}

		if (!strcmp(l_str, "error")) level = GF_LOG_ERROR;
		else if (!strcmp(l_str, "warning")) level = GF_LOG_WARNING;
		else if (!strcmp(l_str, "info")) level = GF_LOG_INFO;
		else if (!strcmp(l_str, "debug")) level = GF_LOG_DEBUG;
		else if (!strcmp(l_str, "quiet")) level = GF_LOG_QUIET;
		else if (!strcmp(l_str, "ncl") || !strcmp(l_str, "cl")) {
			if (!next) {
				if (l_strict) l_strict[0] = '+';
				break;
			}
			logs = next+1;
		} else if (!strcmp(l_str, "strict")) {
			lf->strict = GF_TRUE;
		} else {
			GF_LOG(GF_LOG_WARNING, GF_LOG_FILTER, ("Unsupported log level %s, ignoring\n", l_str));
			l_tool=NULL;
		}
		if (l_strict) {
			lf->strict = GF_TRUE;
			l_strict[0] = '+';
		}

		while (l_tool) {
			char *n_tool = strchr(l_tool, ':');
			if (n_tool) n_tool[0]=0;

			u32 found=0, tool = gf_log_parse_tool(l_tool);
			if (tool==GF_LOG_TOOL_UNDEFINED) {
				GF_LOG(GF_LOG_WARNING, GF_LOG_FILTER, ("Unsupported log tool %s, ignoring\n", l_tool));
			} else {
				for (i=0; i<lf->nb_tools; i++) {
					if (lf->tools[i] == tool) {
						lf->levels[i] = level;
						found=1;
						break;
					}
				}
				if (!found) {
					lf->tools = gf_realloc(lf->tools, sizeof(u32) * (lf->nb_tools+1));
					lf->levels = gf_realloc(lf->levels, sizeof(u32) * (lf->nb_tools+1));
					lf->tools[lf->nb_tools] = tool;
					lf->levels[lf->nb_tools] = level;
					lf->nb_tools++;
				}
			}

			if (!n_tool) break;
			l_tool = n_tool+1;
		}

		if (!next) break;
		logs = next+1;
	}
	gf_free(c_logs);
}
#endif

static const char *gf_filter_load_arg_config(GF_Filter *filter, const char *sec_name, const char *arg_name, const char *arg_val, Bool first_arg)
{
	char szArg[101];
	Bool gf_sys_has_filter_global_args();
	const char *opt;
	GF_FilterSession *session = filter->session;

	//look in global args
	if (gf_sys_has_filter_global_args()) {
		u32 alen = (u32) strlen(arg_name);
		u32 i, nb_args = gf_sys_get_argc();
		for (i=0; i<nb_args; i++) {
			u32 len;
			u32 flen = 0;
			u32 is_ok, loc_alen;
			const char *per_filter, *sep2, *sep;
			const char *o_arg, *arg = gf_sys_get_arg(i);
			if (arg[0]!='-') continue;
			if (arg[1]!='-') continue;

			arg += 2;
			o_arg = arg;
			//allow filter@opt= and filter:opt=
			sep = strchr(arg, '=');
			per_filter = strchr(arg, '@');
			sep2 = strchr(arg, ':');
			if (sep && per_filter && (sep<per_filter)) per_filter = NULL;
			if (sep && sep2 && (sep<sep2)) sep2 = NULL;
			if (per_filter && sep2 && (sep2<per_filter)) per_filter = sep2;
			else if (!per_filter) per_filter = sep2;

			if (per_filter) {
				flen = (u32) (per_filter - arg);
				if (!flen || strncmp(filter->freg->name, arg, flen))
					continue;
				flen++;
				arg += flen;
			}
			gf_sys_mark_arg_used(i, GF_TRUE);

			if (sep) {
				len = (u32) (sep - (arg));
			} else {
				len = (u32) strlen(arg);
			}
			is_ok = 0;
			if (!strncmp(arg, arg_name, alen)) {
				if (len == alen) is_ok = 1;
				loc_alen = alen;
			} else if (first_arg && sep) {
				if (!strncmp(arg, "FBT", len)) {
					GF_PropertyValue ap = gf_props_parse_value(GF_PROP_UINT, "FBT", sep+1, NULL, filter->session->sep_list);
					filter->pid_buffer_max_us = ap.value.uint;
					is_ok = 2;
					loc_alen=3;
				}
				else if (!strncmp(arg, "FBU", len)) {
					GF_PropertyValue ap = gf_props_parse_value(GF_PROP_UINT, "FBU", sep+1, NULL, filter->session->sep_list);
					filter->pid_buffer_max_units = ap.value.uint;
					is_ok = 2;
					loc_alen=3;
				}
				else if (!strncmp(arg, "FBD", len)) {
					GF_PropertyValue ap = gf_props_parse_value(GF_PROP_UINT, "FBD", sep+1, NULL, filter->session->sep_list);
					filter->pid_decode_buffer_max_us = ap.value.uint;
					is_ok = 2;
					loc_alen=3;
				}
#ifdef GPAC_ENABLE_DEBUG
				else if (!strncmp(arg, "DBG", len)) {
					const char *val = sep+1;
					if (val && !stricmp(val, "pid")) filter->prop_dump = 1;
					else if (val && !stricmp(val, "pck")) filter->prop_dump = 2;
					else if (val && !stricmp(val, "all")) filter->prop_dump = 3;
					else if (!val) filter->prop_dump = 3;
					else {
						GF_LOG(GF_LOG_WARNING, GF_LOG_FILTER, ("Invalid DBG param syntax %s, expecting pid, pck or all\n", arg));
					}
					is_ok = 2;
					loc_alen=3;
				}
#endif
				else if (!strncmp(arg, "LT", len)) {
					is_ok = 2;
					loc_alen=2;
#ifndef GPAC_DISABLE_LOG
					filter_parse_logs(filter, sep+1);
#endif
				}

			}
			if (!is_ok) continue;

			strncpy(szArg, o_arg, 100);
			szArg[ MIN(flen + loc_alen, 100) ] = 0;
			gf_fs_push_arg(session, szArg, GF_TRUE, GF_ARGTYPE_LOCAL, NULL, NULL);

			if (is_ok==1) {
				if (sep) return sep+1;
				//no arg value means boolean true
				else return "true";
			}
		}
	}

	//look in config file
	opt = gf_opts_get_key(sec_name, arg_name);
	if (opt)
		return opt;

	if (first_arg) {
		GF_PropertyValue ap;
		opt = gf_opts_get_key(sec_name, "FBT");
		if (opt) {
			ap = gf_props_parse_value(GF_PROP_UINT, "FBT", opt, NULL, filter->session->sep_list);
			filter->pid_buffer_max_us = ap.value.uint;
		}
		opt = gf_opts_get_key(sec_name, "FBU");
		if (opt) {
			ap = gf_props_parse_value(GF_PROP_UINT, "FBU", opt, NULL, filter->session->sep_list);
			filter->pid_buffer_max_units = ap.value.uint;
		}
		opt = gf_opts_get_key(sec_name, "FBD");
		if (opt) {
			ap = gf_props_parse_value(GF_PROP_UINT, "FBD", opt, NULL, filter->session->sep_list);
			filter->pid_decode_buffer_max_us = ap.value.uint;
		}
#ifndef GPAC_DISABLE_LOG
		opt = gf_opts_get_key(sec_name, "LT");
		if (opt) filter_parse_logs(filter, opt);
#endif
	}

	//ifce (used by socket and other filters), use core default
	if (!strcmp(arg_name, "ifce")) {
		opt = gf_opts_get_key("core", "ifce");
		if (opt)
			return opt;
		return NULL;
	}

	return arg_val;
}

static void gf_filter_load_meta_args_config(const char *sec_name, GF_Filter *filter)
{
	GF_PropertyValue argv;
	Bool gf_sys_has_filter_global_meta_args();
	Bool gf_sys_has_filter_global_args();
	u32 i, key_count = gf_opts_get_key_count(sec_name);

	FSESS_CHECK_THREAD(filter)

	for (i=0; i<key_count; i++) {
		Bool arg_found = GF_FALSE;
		u32 k=0;
		const char *arg_val, *arg_name = gf_opts_get_key_name(sec_name, i);
		//check if this is a regular arg, if so don't process it
		while (filter->freg->args) {
			const GF_FilterArgs *a = &filter->freg->args[k];
			if (!a || !a->arg_name) break;
			k++;
			if (!strcmp(a->arg_name, arg_name)) {
				arg_found = GF_TRUE;
				break;
			}
		}
		if (arg_found) continue;

		arg_val = gf_opts_get_key(sec_name, arg_name);
		if (!arg_val) continue;

		memset(&argv, 0, sizeof(GF_PropertyValue));
		argv.type = GF_PROP_STRING;
		argv.value.string = (char *) arg_val;
		filter->freg->update_arg(filter, arg_name, &argv);
	}
	if (!gf_sys_has_filter_global_meta_args()
		//allow -- syntax as well
		&& !gf_sys_has_filter_global_args()
	) {
		return;
	}

	key_count = gf_sys_get_argc();
	for (i=0; i<key_count; i++) {
#define META_MAX_ARG	1000
		char szArg[META_MAX_ARG+1];
		GF_Err e;
		u32 len = 0;
		const char *per_filter;
		const char *sep, *sep2, *o_arg, *arg = gf_sys_get_arg(i);
		if (arg[0] != '-') continue;
		if ((arg[1] != '+') && (arg[1] != '-')) continue;
		arg+=2;

		o_arg = arg;

		//allow filter@opt= and filter:opt=
		sep = strchr(arg, '=');
		per_filter = strchr(arg, '@');
		sep2 = strchr(arg, ':');
		if (sep && per_filter && (sep<per_filter)) per_filter = NULL;
		if (sep && sep2 && (sep<sep2)) sep2 = NULL;
		if (per_filter && sep2 && (sep2<per_filter)) per_filter = sep2;
		else if (!per_filter) per_filter = sep2;

		if (per_filter) {
			len = (u32) (per_filter - arg);
			if (!len || strncmp(filter->freg->name, arg, len))
				continue;
			len++;
			arg += len;
		}

		memset(&argv, 0, sizeof(GF_PropertyValue));
		argv.type = GF_PROP_STRING;
		if (sep) {
			u32 cplen = (u32) (sep - o_arg);
			if (cplen>=META_MAX_ARG) cplen=META_MAX_ARG;
			strncpy(szArg, o_arg, cplen);
			szArg[cplen] = 0;
			argv.value.string = (char *) sep+1;
		} else {
			u32 cplen = (u32) strlen(o_arg);
			if (cplen>=META_MAX_ARG) cplen=META_MAX_ARG;
			memcpy(szArg, o_arg, cplen);
			szArg[cplen] = 0;
		}
#undef META_MAX_ARG

		e = filter->freg->update_arg(filter, szArg + len, &argv);
		if (e) {
			GF_LOG(GF_LOG_WARNING, GF_LOG_FILTER, ("Error assigning argument %s to filter %s: %s\n", szArg, filter->name, gf_errno_str(e) ));
		}

		//no need to push the arg, global args are always pushed when creating the session,
		//and meta filters must report used/unused options
	}
}

static void filter_parse_dyn_args(GF_Filter *filter, const char *args, GF_FilterArgType arg_type, Bool for_script, char *szSrc, char *szDst, char *szEscape, char *szSecName, Bool has_meta_args, u32 argfile_level)
{
	char *szArg=NULL;
	u32 i=0;
	u32 alloc_len=1024;
	const GF_FilterArgs *f_args = NULL;
	Bool opts_optional = GF_FALSE;

	if (args)
		szArg = gf_malloc(sizeof(char)*1024);

	//by default always force a remux
	if ((arg_type==GF_FILTER_ARG_EXPLICIT_SINK)
		&& !filter->dynamic_filter
		&& !filter->multi_sink_target
		&& (filter->freg->flags & GF_FS_REG_FORCE_REMUX)
	) {
		filter->force_demux = 1;
	}
	//implicit linking mode: if not a script or if script init (initialized called) and no extra pid set, enable clonable
	if ( (filter->session->flags & GF_FS_FLAG_IMPLICIT_MODE)
		&& !filter->max_extra_pids
		&& (for_script || !(filter->freg->flags&GF_FS_REG_SCRIPT))
		&& ((arg_type==GF_FILTER_ARG_EXPLICIT_SINK) || (arg_type==GF_FILTER_ARG_EXPLICIT))
	) {
		filter->clonable = GF_FILTER_CLONE_PROBE;
	}

	//parse each arg
	while (args) {
		char *value;
		u32 len;
		Bool found=GF_FALSE;
		char *escaped = NULL;
		Bool opaque_arg = GF_FALSE;
		Bool absolute_url = GF_FALSE;
		Bool internal_url = GF_FALSE;
		Bool internal_arg = GF_FALSE;
		char *xml_start = NULL;
		char *sep = NULL;

		//look for our arg separator - if arg[0] is also a separator, consider the entire string until next double sep as the parameter
		if (args[0] != filter->session->sep_args)
			sep = strchr(args, filter->session->sep_args);
		else {
			while (args[0] == filter->session->sep_args) {
				args++;
			}
			if (!args[0])
				break;

			sep = (char *) args + 1;
			while (1) {
				sep = strchr(sep, filter->session->sep_args);
				if (!sep) break;
				if (sep[1]==filter->session->sep_args) {
					break;
				}
				sep = sep+1;
			}
			opaque_arg = GF_TRUE;
		}

		if (!opaque_arg) {
			Bool check_url_esc=GF_FALSE;
			//we don't use gf_fs_path_escape_colon here because we also analyse whether the URL is internal or not, and we don't want to do that on each arg
			if (sep) {
				//escape XML inputs: simply search for ">:" (: being the arg sep), if not found consider the entire string the arg value
				xml_start = strchr(args, '<');
				if (xml_start && (xml_start<sep)) {
					char szEnd[3];
					szEnd[0] = '>';
					szEnd[1] = filter->session->sep_args;
					szEnd[2] = 0;
					char *xml_end = strstr(xml_start, szEnd);
					if (!xml_end) {
						len = (u32) strlen(args);
						sep = NULL;
					} else {
						sep = xml_end+1;
						len = (u32) (sep-args);
					}
				}
			}

			if (filter->session->sep_args == ':') {
				if (sep && !strncmp(args, szSrc, 4) && !strncmp(args+4, "gcryp://", 8)) {
					sep = strstr(args+12, "://");
				}
				while (sep && !strncmp(sep, "://", 3)) {
					absolute_url = GF_TRUE;

					//filter internal url schemes
					if ((!strncmp(args, szSrc, 4) || !strncmp(args, szDst, 4) ) &&
						(!strncmp(args+4, "video://", 8)
						|| !strncmp(args+4, "audio://", 8)
						|| !strncmp(args+4, "av://", 5)
						|| !strncmp(args+4, "gmem://", 7)
						|| !strncmp(args+4, "gpac://", 7)
						|| !strncmp(args+4, "pipe://", 7)
						|| !strncmp(args+4, "tcp://", 6)
						|| !strncmp(args+4, "udp://", 6)
						|| !strncmp(args+4, "tcpu://", 7)
						|| !strncmp(args+4, "udpu://", 7)
						|| !strncmp(args+4, "rtp://", 6)
						|| !strncmp(args+4, "atsc://", 7)
						|| !strncmp(args+4, "gfio://", 7)
						|| !strncmp(args+4, "route://", 8)
						|| !strncmp(args+4, "mabr://", 7)
						)
					) {
						internal_url = GF_TRUE;
						sep = strchr(sep+3, ':');
						if (!strncmp(args+4, "tcp://", 6)
							|| !strncmp(args+4, "udp://", 6)
							|| !strncmp(args+4, "tcpu://", 7)
							|| !strncmp(args+4, "udpu://", 7)
							|| !strncmp(args+4, "rtp://", 6)
							|| !strncmp(args+4, "route://", 8)
							|| !strncmp(args+4, "mabr://", 7)
						) {
							char *sep2 = sep ? strchr(sep+1, ':') : NULL;
							char *sep3 = sep ? strchr(sep+1, '/') : NULL;
							if (sep2 && sep3 && (sep2>sep3)) {
								sep2 = strchr(sep3, ':');
							}
							//if in the form scheme://FOO/:, don't inspect FOO. This allows escaping port nmber or IPv6 double colon
							if (sep3 && ((sep3[1]==':') || (sep3[1]==0)) ) {
								sep = sep3+1;
								if (!sep[0]) sep = NULL;
							} else if (sep2 || sep3 || sep) {
								u32 port = 0;
								if (sep2) {
									sep2[0] = 0;
									if (sep3) sep3[0] = 0;
								}
								else if (sep3) sep3[0] = 0;
								if (sscanf(sep+1, "%d", &port)==1) {
									char szPort[20];
									snprintf(szPort, 20, "%d", port);
									if (strcmp(sep+1, szPort))
										port = 0;
								}
								if (sep2) sep2[0] = ':';
								if (sep3) sep3[0] = '/';

								if (port) sep = sep2;
							}
						}

					} else {
						//look for '::' vs ':gfopt' and ':gpac:' - if '::' appears before these, jump to '::'
						char *sep2 = strstr(sep+3, ":gfopt:");
						char *sep3 = strstr(sep+3, ":gfloc:");
						if (sep2 && sep3 && sep2>sep3)
							sep2 = sep3;
						else if (!sep2)
							sep2 = sep3;

						//keep first of :gfopt:, :gfloc: or :gpac: in sep3
						sep3 = strstr(sep+3, szEscape);
						if (sep2 && sep3 && sep2<sep3)
							sep3 = sep2;
						else if (!sep3)
							sep3 = sep2;

						sep2 = strstr(sep+3, "::");
						if (sep2 && sep3 && sep3<sep2)
							sep2 = sep3;
						else if (sep2)
							opaque_arg = GF_TRUE; //skip an extra ':' at the end of the arg parsing
						else {
							//first occurence of our internal separator if any
							sep2 = sep3;
						}

						//escape sequence present after this argument, use it
						if (sep2) {
							sep = sep2;
						} else {
							//get root /
							sep = strchr(sep+3, '/');
							//get first : after root
							if (sep) sep = strchr(sep+1, ':');
						}
						check_url_esc = GF_TRUE;
					}
				}

				//watchout for "C:\\" or "C:/"
				while (sep && (sep[1]=='\\' || sep[1]=='/')) {
					sep = strchr(sep+1, ':');
				}
				//escape data/time
				if (sep) {
					char *prev_date = NULL;
					if ((u32) (sep - args)>=3) {
						prev_date = sep-3;
						if (prev_date[0]=='T') {}
						else if (prev_date[0]=='C') { prev_date ++; }
						else if (prev_date[1]=='T') { prev_date ++; }
						else { prev_date = NULL; }
					}

skip_date:
					if (prev_date) {
						u32 char_idx=1;
						u32 nb_date_seps=0;
						Bool last_non_num=GF_FALSE;
						char *search_after = NULL;
						while (1) {
							char dc = prev_date[char_idx];

							if ((dc>='0') && (dc<='9')) {
								search_after = prev_date+char_idx;
								char_idx++;
								continue;
							}
							if (dc == ':') {
								search_after = prev_date+char_idx;
								if (nb_date_seps>=3)
									break;
								char_idx++;
								nb_date_seps++;
								continue;
							}
							if ((dc == '.') || (dc == ';')) {
								if (last_non_num || !nb_date_seps) {
									search_after = NULL;
									break;
								}
								last_non_num = GF_TRUE;
								search_after = prev_date+char_idx;
								char_idx++;
								continue;
							}
							//not a valid char in date, stop
							break;
						}

						if (search_after) {
							//take care of lists
							char *next_date = strchr(search_after, 'T');
							char *next_sep = strchr(search_after, ':');
							if (next_date && next_sep && (next_date<next_sep)) {
								prev_date = next_date - 1;
								if (prev_date[0] == filter->session->sep_list) {
									prev_date = next_date;
									goto skip_date;
								}
							}
							sep = strchr(search_after, ':');
						}
					}
				}
			}
			if (sep) {
				escaped = (sep[1] == filter->session->sep_args) ? NULL : strstr(sep, szEscape);
				if (escaped && xml_start && (escaped>xml_start)) escaped = NULL;
				if ((u32) (escaped-sep)>2) escaped = NULL;
				//if we have a :gfopt: or :gfloc: set without :gpac: on a source, consider this as a valid escape pattern
				if (check_url_esc && !escaped && !strncmp(args, szSrc, 4) && (!strncmp(sep, ":gfopt:", 7) ||!strncmp(sep, ":gfloc:", 7)))
					escaped = sep;

				if (escaped) {
					sep = escaped;
				}
				/*no escape, special case for src= and dst= where we need to detect if this is a filename with an option
				separator in the name, i.e. differentiate
					foo:bar=toto.mp4:bar2=z
				and
					foo:toto.mp4:bar2=z

				We do that by checking that if a file extension is present after the option sep, we don't have an assign ('=')
				in-between the option sep and the extension.
				If no assign is persent, ignore all option separators before the file extension
				cf #1942

				Note that this only works if file extension is present. If not, escape mechanism (eg :gpac:) must be used
				*/
				else if (!strncmp(args, szSrc, 4) || !strncmp(args, szDst, 4)) {
					char *ext_sep = strchr(args, '.');
					if (ext_sep && (ext_sep>sep)) {
						char *assign = strchr(args+4, filter->session->sep_name);
						if (!assign || (assign>ext_sep)) {
							sep = strchr(ext_sep+1, filter->session->sep_args);
						}
					}
				}
			}

			if (sep && !strncmp(args, szSrc, 4) && !escaped && absolute_url && !internal_url) {
				Bool file_exists;
				sep[0]=0;
				if (!strcmp(args+4, "null")) file_exists = GF_TRUE;
				else if (!strncmp(args+4, "tcp://", 6)) file_exists = GF_TRUE;
				else if (!strncmp(args+4, "udp://", 6)) file_exists = GF_TRUE;
				else if (!strncmp(args+4, "route://", 8)) file_exists = GF_TRUE;
				else if (!strncmp(args+4, "mabr://", 7)) file_exists = GF_TRUE;
				else file_exists = gf_file_exists(args+4);

				if (!file_exists) {
					char *fsep = strchr(args+4, filter->session->sep_frag);
					if (fsep) {
						fsep[0] = 0;
						file_exists = gf_file_exists(args+4);
						fsep[0] = filter->session->sep_frag;
					}
				}
				sep[0]= filter->session->sep_args;
				if (!file_exists) {
					GF_LOG(GF_LOG_WARNING, GF_LOG_FILTER, ("Non-escaped argument pattern \"%s\" in src %s, assuming arguments are part of source URL. Use src=PATH:gpac:ARGS to differentiate, or change separators\n", sep, args));
					sep = NULL;
				}
			}
		}


		if (sep) {
			len = (u32) (sep-args);
		} else {
			len = (u32) strlen(args);
		}

		if (len>=alloc_len) {
			alloc_len = len+1;
			szArg = gf_realloc(szArg, sizeof(char)*alloc_len);
		}
		strncpy(szArg, args, len);
		szArg[len]=0;

		value = strchr(szArg, filter->session->sep_name);
		if (value) {
			value[0] = 0;
			value++;
		}

		//arg is a PID property assignment
		if (szArg[0] == filter->session->sep_frag) {
			filter->user_pid_props = GF_TRUE;
			goto skip_arg;
		}

		if ((arg_type == GF_FILTER_ARG_INHERIT) && (!strcmp(szArg, "src") || !strcmp(szArg, "dst")) )
			goto skip_arg;

		i=0;
		f_args = filter->freg->args;
		if (for_script)
		 	f_args = filter->instance_args;

		Bool is_my_arg = GF_FALSE;
		u32 count_enum_val = 0;
		const char *restricted=NULL;
		Bool reverse_bool = GF_FALSE;
		const GF_FilterArgs *save_a = NULL;

		while (filter->filter_udta && f_args) {

			const GF_FilterArgs *a = &f_args[i];
			i++;
			if (!a || !a->arg_name) break;

			if (!strcmp(a->arg_name, szArg)) {
				is_my_arg = GF_TRUE;
				save_a = a;
			} else if ((szArg[0]==filter->session->sep_neg) && !strcmp(a->arg_name, szArg+1)) {
				is_my_arg = GF_TRUE;
				reverse_bool = GF_TRUE;
				save_a = a;
			}
			//little optim here: if no value provided, check if argument name is exactly one of the possible enums
			//only do this for explicit filters, not for inheritance
			else if (a->min_max_enum && strchr(a->min_max_enum, '|') && strstr(a->min_max_enum, szArg)) {
				const char *enums = a->min_max_enum;
				while (enums) {
					if (!strncmp(enums, szArg, len)) {
						char c = enums[len];
						if (!c || (c=='|')) {
							count_enum_val++;
							value = szArg;
							save_a = a;
							break;
						}
					}
					char *enext = strchr(enums, '|');
					if (!enext) break;
					enums=enext+1;
				}
			}

			if (is_my_arg || count_enum_val>1) break;
		}

		if(is_my_arg && count_enum_val>0) {
			GF_LOG(GF_LOG_WARNING, GF_LOG_FILTER, ("Ambiguous argument %s in filter %s: both an argument and an enum value share the name \"%s\", ignoring\n", szArg, filter->freg->name, szArg));
		} else if(count_enum_val>1) {
			//only warn for explicit filters
			if (!filter->dynamic_filter) {
				GF_LOG(GF_LOG_WARNING, GF_LOG_FILTER, ("Argument %s of filter %s is ambiguous (multiple enum arguments have \"%s\" as possible value), ignoring\n", szArg, filter->freg->name, szArg));
			}
		} else if (is_my_arg || count_enum_val==1) {
			restricted = gf_opts_get_key_restricted(szSecName, save_a->arg_name);
			found=GF_TRUE;
			if (restricted) {
				GF_LOG(GF_LOG_WARNING, GF_LOG_FILTER, ("Argument %s of filter %s is restricted to %s by system-wide configuration, ignoring\n", szArg, filter->freg->name, restricted));
			} else {
				GF_PropertyValue argv;
				argv = gf_filter_parse_prop_solve_env_var(filter->session, filter, (save_a->flags & GF_FS_ARG_META) ? GF_PROP_STRING : save_a->arg_type, save_a->arg_name, value, save_a->min_max_enum);

				if (reverse_bool && (argv.type==GF_PROP_BOOL))
					argv.value.boolean = !argv.value.boolean;

				if (argv.type != GF_PROP_FORBIDDEN) {
					if (!for_script && (save_a->offset_in_private>=0)) {
						gf_filter_set_arg(filter, save_a, &argv);
					} else if (filter->freg->update_arg) {
						FSESS_CHECK_THREAD(filter)
						filter->freg->update_arg(filter, save_a->arg_name, &argv);
						opaque_arg = GF_FALSE;
						if ((argv.type==GF_PROP_STRING) || (argv.type==GF_PROP_STRING_LIST))
							gf_props_reset_single(&argv);
					}
				}
			}
		}

		GF_Filter *meta_filter = NULL;
		if (!strlen(szArg)) {
			found = GF_TRUE;
		} else if (!found) {
			//filter ID
			if (!strcmp("FID", szArg)) {
				if (arg_type != GF_FILTER_ARG_INHERIT)
					gf_filter_set_id(filter, value);
				found = GF_TRUE;
				internal_arg = GF_TRUE;
			}
			//filter sources
			else if (!strcmp("SID", szArg)) {
				if (arg_type!=GF_FILTER_ARG_INHERIT)
					gf_filter_set_sources(filter, value);
				found = GF_TRUE;
				internal_arg = GF_TRUE;
			}
			//clonable filter
			else if (!strcmp("clone", szArg)) {
				if ((arg_type==GF_FILTER_ARG_EXPLICIT_SINK) || (arg_type==GF_FILTER_ARG_EXPLICIT)) {
					if (value && (!strcmp(value, "0") || !strcmp(value, "false") || !strcmp(value, "no"))) {
						filter->clonable = GF_FILTER_NO_CLONE;
					} else {
						filter->clonable = GF_FILTER_CLONE;
					}
				}
				found = GF_TRUE;
				internal_arg = GF_TRUE;
			}
			//filter name
			else if (!strcmp("N", szArg)) {
				if ((arg_type==GF_FILTER_ARG_EXPLICIT_SINK) || (arg_type==GF_FILTER_ARG_EXPLICIT) || (arg_type==GF_FILTER_ARG_EXPLICIT_SOURCE)) {
					gf_filter_set_name(filter, value);
				}
				found = GF_TRUE;
				internal_arg = GF_TRUE;
			}
			if (!strcmp("FS", szArg)) {
				if (value && arg_type != GF_FILTER_ARG_INHERIT)
					filter->subsession_id = atoi(value);

				found = GF_TRUE;
				internal_arg = GF_TRUE;
			}
			//filter sources
			else if (!strcmp("RSID", szArg)) {
				filter->require_source_id = GF_TRUE;
				found = GF_TRUE;
				internal_arg = GF_TRUE;
			}
			//per-filter buffer times
			else if (!strcmp("FBT", szArg)) {
				if (value && arg_type!=GF_FILTER_ARG_INHERIT)
					filter->pid_buffer_max_us = atoi(value);
				found = GF_TRUE;
				internal_arg = GF_TRUE;
			}
			//per-filter buffer units
			else if (!strcmp("FBU", szArg)) {
				if (value && arg_type!=GF_FILTER_ARG_INHERIT)
					filter->pid_buffer_max_units = atoi(value);
				found = GF_TRUE;
				internal_arg = GF_TRUE;
			}
			else if (!strcmp("DL", szArg)) {
				if (! filter->dynamic_filter) {
					filter->deferred_link = GF_TRUE;
					GF_LOG(GF_LOG_INFO, GF_LOG_FILTER, ("Deferred linking enabled for filter %s\n", filter->freg->name));
				}
				found = GF_TRUE;
				internal_arg = GF_TRUE;
			}
			else if (!strcmp("LT", szArg)) {
				found = GF_TRUE;
				internal_arg = GF_TRUE;
#ifndef GPAC_DISABLE_LOG
				if (value)
					filter_parse_logs(filter, value);
#endif
			}


			//internal options, nothing to do here
			else if (
				//generic encoder load
				!strcmp("c", szArg)
				//preferred registry to use
				|| !strcmp("gfreg", szArg)
				//non inherited options
				|| !strcmp("gfloc", szArg)
				//sep
				|| !strcmp("gpac", szArg)
			) {
				found = GF_TRUE;
				internal_arg = GF_TRUE;
			}
			else if (!strcmp("ccp", szArg)) {
				found = GF_TRUE;
				internal_arg = GF_TRUE;
				if (!filter->dynamic_filter) {
					if (value) {
						GF_PropertyValue res = gf_filter_parse_prop_solve_env_var(filter->session, filter, GF_PROP_STRING_LIST, "ccp", value, NULL);
						filter->skip_cids = res.value.string_list;
					} else {
						if (filter->skip_cids.vals) {
							GF_PropertyValue prop;
							prop.value.string_list = filter->skip_cids;
							prop.type = GF_PROP_STRING_LIST;
							gf_props_reset_single(&prop);
						}
						filter->skip_cids.nb_items = 1;
						filter->skip_cids.vals = gf_malloc(sizeof(char*));
						filter->skip_cids.vals[0] = gf_strdup("AUTO");
					}
				}
			}
			//non tracked options
			else if (!strcmp("gfopt", szArg)) {
				found = GF_TRUE;
				internal_arg = GF_TRUE;
				opts_optional = GF_TRUE;
			}
			//filter tag
			else if (!strcmp("TAG", szArg)) {
				if (! filter->dynamic_filter) {
					if (filter->tag) gf_free(filter->tag);
					filter->tag = value ? gf_strdup(value) : NULL;
				}
				found = GF_TRUE;
				internal_arg = GF_TRUE;
			}
			//filter itag
			else if (!strcmp("ITAG", szArg)) {
				if (filter->itag) gf_free(filter->itag);
				filter->itag = value ? gf_strdup(value) : NULL;
				found = GF_TRUE;
				internal_arg = GF_TRUE;
			}
			//temporary filter
			else if (!strcmp("_GFTMP", szArg)) {
				filter->removed = 1;
				found = GF_TRUE;
				internal_arg = GF_TRUE;
			}
			//allow direct copy
			else if (!strcmp("nomux", szArg)) {
				//only apply fo explicit sink, not dynamic and no multi-sink target
				if ((arg_type==GF_FILTER_ARG_EXPLICIT_SINK) && !filter->dynamic_filter && !filter->multi_sink_target) {
					if (value && (!strcmp(value, "0") || !strcmp(value, "false") || !strcmp(value, "no"))) {
						filter->force_demux = 2;
					} else {
						filter->force_demux = 0;
					}
				}
				found = GF_TRUE;
				internal_arg = GF_TRUE;
			}
			else if (!strcmp("NCID", szArg)) {
				if (filter->netcap_id) gf_free(filter->netcap_id);
				filter->netcap_id = value ? gf_strdup(value) : NULL;
				found = GF_TRUE;
				internal_arg = GF_TRUE;
			}
#ifdef GPAC_ENABLE_DEBUG
			else if (!strcmp("DBG", szArg)) {
				if (value && !stricmp(value, "pid")) filter->prop_dump = 1;
				else if (value && !stricmp(value, "pck")) filter->prop_dump = 2;
				else if (value && !stricmp(value, "all")) filter->prop_dump = 3;
				else if (!value) filter->prop_dump = 3;
				found = GF_TRUE;
				internal_arg = GF_TRUE;
			}
#endif
			else if (!value && gf_file_exists(szArg)) {
				internal_arg = GF_TRUE;
				if (!for_script && (argfile_level<5) ) {
					char szLine[2001];
					FILE *arg_file = gf_fopen(szArg, "rt");
					szLine[2000]=0;
					while (arg_file && !gf_feof(arg_file)) {
						u32 llen;
						char *subarg, *res_line;
						szLine[0] = 0;
						res_line = gf_fgets(szLine, 2000, arg_file);
						if (!res_line) break;
						llen = (u32) strlen(szLine);
						//make sure we have a legal UTF8 file
						if (! gf_utf8_is_legal(szLine, llen)) {
							GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Filter argument file \"%s\" is not a valid UTF-8 file, ignoring\n", szArg));
							internal_arg = GF_FALSE;
							break;
						}

						while (llen && strchr(" \n\r\t", szLine[llen-1])) {
							szLine[llen-1]=0;
							llen--;
						}
						if (!llen)
							continue;

						subarg = szLine;
						while (subarg[0] && strchr(" \n\r\t", subarg[0]))
							subarg++;
						if ((subarg[0] == '/') && (subarg[1] == '/'))
							continue;

						filter_parse_dyn_args(filter, subarg, arg_type, for_script, szSrc, szDst, szEscape, szSecName, has_meta_args, argfile_level+1);
					}
					if (arg_file) {
						gf_fclose(arg_file);
					} else {
						GF_LOG(GF_LOG_WARNING, GF_LOG_FILTER, ("Failed to open argument file %s, ignoring\n", szArg));
					}
				} else if (!for_script) {
					GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Filter argument file has too many nested levels of sub-files, maximum allowed is 5\n"));
				}
			}
			else if (has_meta_args && filter->freg->update_arg) {
				GF_Err e = GF_OK;
				if (for_script || !(filter->freg->flags&GF_FS_REG_SCRIPT) ) {
					GF_PropertyValue argv = gf_props_parse_value(GF_PROP_STRING, szArg, value, NULL, filter->session->sep_list);
					FSESS_CHECK_THREAD(filter)
					e = filter->freg->update_arg(filter, szArg, &argv);
					if (argv.value.string) gf_free(argv.value.string);
					//opaque arg not found for meta, report it
					if ((e==GF_NOT_FOUND) && opaque_arg) {
						opaque_arg = GF_FALSE;
						found = GF_FALSE;
					}
				}
				if (!(filter->freg->flags&GF_FS_REG_SCRIPT) && (e==GF_OK) ) {
					found = GF_TRUE;
					meta_filter = filter;
				}
			}
		}
		//push non-internal args - optional args are skipped if not found
		if (!internal_arg && (!has_meta_args || !opaque_arg) && (found || !opts_optional))
			gf_fs_push_arg(filter->session, szArg, found, GF_ARGTYPE_LOCAL, meta_filter, NULL);

skip_arg:
		if (escaped) {
			args=sep+6;
		} else if (sep) {
			args=sep+1;
			if (opaque_arg)
				args += 1;
		} else {
			args=NULL;
		}
	}
	if (szArg) gf_free(szArg);
}


static void gf_filter_parse_args(GF_Filter *filter, const char *args, GF_FilterArgType arg_type, Bool for_script)
{
	Bool first = GF_TRUE;
	u32 i=0;
	char szSecName[200];
	char szEscape[7];
	char szSrc[5], szDst[5];
	Bool has_meta_args = GF_FALSE;
	const GF_FilterArgs *f_args = NULL;
	if (!filter) return;

	if (!for_script) {
		if (!filter->freg->private_size) {
			if (filter->freg->args && filter->freg->args[0].arg_name) {
				GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Filter with arguments but no private stack size, no arg passing\n"));
			}
		} else {
			filter->filter_udta = gf_malloc(filter->freg->private_size);
			if (!filter->filter_udta) {
				GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Failed to allocate private data stack\n"));
				return;
			}
			memset(filter->filter_udta, 0, filter->freg->private_size);
		}
	}

	sprintf(szEscape, "%cgpac%c", filter->session->sep_args, filter->session->sep_args);
	sprintf(szSrc, "src%c", filter->session->sep_name);
	sprintf(szDst, "dst%c", filter->session->sep_name);

	snprintf(szSecName, 200, "filter@%s", filter->freg->name);

#ifdef GPAC_CONFIG_EMSCRIPTEN
	const GF_FilterArgs *index_arg = NULL;
#endif

	//instantiate all args with defaults value
	f_args = filter->freg->args;
	if (for_script)
		f_args = filter->instance_args;
	i=0;
	while (f_args) {
		GF_PropertyValue argv;
		const char *def_val;
		const GF_FilterArgs *a = &f_args[i];
		if (!a || !a->arg_name) break;
		i++;

#ifdef GPAC_CONFIG_EMSCRIPTEN
		if (!strcmp(a->arg_name, "index") && (a->arg_type==GF_PROP_SINT) && (a->offset_in_private>=0))
			index_arg = a;
#endif

		if (a->flags & GF_FS_ARG_META) {
			has_meta_args = GF_TRUE;
			continue;
		}

		def_val = gf_filter_load_arg_config(filter, szSecName, a->arg_name, a->arg_default_val, first);
		first = GF_FALSE;

		if (!def_val) continue;

		argv = gf_filter_parse_prop_solve_env_var(filter->session, filter, a->arg_type, a->arg_name, def_val, a->min_max_enum);

		if (argv.type != GF_PROP_FORBIDDEN) {
			if (!for_script && (a->offset_in_private>=0)) {
				gf_filter_set_arg(filter, a, &argv);
			} else if (filter->freg->update_arg) {
				FSESS_CHECK_THREAD(filter)
				filter->freg->update_arg(filter, a->arg_name, &argv);
				gf_props_reset_single(&argv);
			}
		} else {
			GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Failed to parse argument %s value %s\n", a->arg_name, a->arg_default_val));
		}
	}
	//handle meta filter options, not exposed in registry
	if (has_meta_args && filter->freg->update_arg && !for_script)
		gf_filter_load_meta_args_config(szSecName, filter);


	filter_parse_dyn_args(filter, args, arg_type, for_script, szSrc, szDst, szEscape, szSecName, has_meta_args, 0);

#ifdef GPAC_CONFIG_EMSCRIPTEN
	//index arg present on filter: if not 0, reframer will open file in read sync mode, force using main thread
	if (index_arg) {
#ifdef WIN32
		void *ptr = (void *) (((char*) filter->filter_udta) + index_arg->offset_in_private);
#else
		void *ptr = filter->filter_udta + index_arg->offset_in_private;
#endif
		if (*(s32 *)ptr != 0)
			gf_filter_force_main_thread(filter, GF_TRUE);
	}
#endif
}

static void reset_filter_args(GF_Filter *filter)
{
	u32 i=0;
	//removed or no stack
	if (!filter->filter_udta) return;

	//instantiate all args with defaults value
	while (filter->freg->args) {
		GF_PropertyValue argv;
		const GF_FilterArgs *a = &filter->freg->args[i];
		i++;
		if (!a || !a->arg_name) break;

		if (a->arg_type != GF_PROP_FORBIDDEN) {
			memset(&argv, 0, sizeof(GF_PropertyValue));
			argv.type = a->arg_type;
			gf_filter_set_arg(filter, a, &argv);
		}
	}
}

void gf_filter_check_output_reconfig(GF_Filter *filter)
{
	u32 i, j;
	//not needed
	if (!filter->reconfigure_outputs) return;
	filter->reconfigure_outputs = GF_FALSE;
	//check destinations of all output pids
	for (i=0; i<filter->num_output_pids; i++) {
		GF_FilterPid *pid = gf_list_get(filter->output_pids, i);
		for (j=0; j<pid->num_destinations; j++) {
			GF_FilterPidInst *pidi = gf_list_get(pid->destinations, j);
			//PID was reconfigured, update props
			if (pidi->reconfig_pid_props) {
				gf_assert(pidi->props);
				if (pidi->props != pidi->reconfig_pid_props) {
					//unassign old property list and set the new one
					gf_assert(pidi->props->reference_count);
					if (safe_int_dec(& pidi->props->reference_count) == 0) {
						//see \ref gf_filter_pid_merge_properties_internal for mutex
						gf_mx_p(pidi->pid->filter->tasks_mx);
						gf_list_del_item(pidi->pid->properties, pidi->props);
						gf_mx_v(pidi->pid->filter->tasks_mx);
						gf_props_del(pidi->props);
					}
					pidi->props = pidi->reconfig_pid_props;
					safe_int_inc( & pidi->props->reference_count );
				}
				pidi->reconfig_pid_props = NULL;
				gf_fs_post_task(filter->session, gf_filter_pid_reconfigure_task, pidi->filter, pid, "pidinst_reconfigure", NULL);
			}
		}
	}
}

static GF_FilterPidInst *filter_relink_get_upper_pid(GF_FilterPidInst *src_pidinst, Bool *needs_flush)
{
	GF_FilterPidInst *pidinst = src_pidinst;
	*needs_flush = GF_FALSE;
	//locate the true destination
	while (1) {
		GF_FilterPid *opid;
		if (pidinst->filter->num_input_pids != 1) break;
		if (pidinst->filter->num_output_pids != 1) break;
		//filter was explicitly loaded, cannot go beyond
		if (! pidinst->filter->dynamic_filter && !pidinst->filter->encoder_codec_id) break;
		opid = gf_list_get(pidinst->filter->output_pids, 0);
		if (!opid) break;
		//we have a fan-out, we cannot replace the filter graph after that point
		//this would affect the other branches of the upper graph
		if (opid->num_destinations != 1) break;
		GF_FilterPidInst *cur_pidinst = gf_list_get(opid->destinations, 0);
		//target is sink, abort if not direct target of the pid instance we want to remove - this prevents replacing muxer
		if ((pidinst != src_pidinst) && (cur_pidinst->filter->num_output_pids==0))
			break;

		pidinst = cur_pidinst;
		if (gf_fq_count(pidinst->packets))
			(*needs_flush) = GF_TRUE;
	}
	return pidinst;
}

void gf_filter_relink_task(GF_FSTask *task)
{
	Bool needs_flush;
	GF_Err e;
	GF_FilterPidInst *cur_pidinst = task->udta;
	/*GF_FilterPidInst *pidinst = */filter_relink_get_upper_pid(cur_pidinst, &needs_flush);
	if (needs_flush) {
		task->requeue_request = GF_TRUE;
		return;
	}
	//good do go, unprotect pid
	gf_assert(cur_pidinst->detach_pending);
	safe_int_dec(&cur_pidinst->detach_pending);
	task->filter->removed = 0;
	e = cur_pidinst->loss_rate;
	cur_pidinst->loss_rate = 0;

	gf_filter_relink_dst(cur_pidinst, e);
}

void gf_filter_relink_dst(GF_FilterPidInst *from_pidinst, GF_Err reason)
{
	GF_Filter *filter_dst;
	GF_FilterPid *link_from_pid = NULL;
	u32 min_chain_len = 0;
	Bool is_encoder = GF_FALSE;
	Bool needs_flush = GF_FALSE;
	GF_FilterPidInst *src_pidinst = from_pidinst;
	GF_FilterPidInst *dst_pidinst = NULL;
	GF_Filter *cur_filter = from_pidinst->filter;

	if (from_pidinst->filter->encoder_codec_id) {
		is_encoder = GF_TRUE;
	}
	//locate the true destination
	dst_pidinst = filter_relink_get_upper_pid(src_pidinst, &needs_flush);
	gf_fatal_assert(dst_pidinst);

	//make sure we flush the end of the pipeline  !
	if (needs_flush) {
		cur_filter->removed = 2;
		//prevent any fetch from pid
		safe_int_inc(&src_pidinst->detach_pending);
		src_pidinst->loss_rate = reason;
		gf_fs_post_task(cur_filter->session, gf_filter_relink_task, cur_filter, NULL, "relink_dst", src_pidinst);
		return;
	}
	filter_dst = dst_pidinst->filter;

	gf_fs_check_graph_load(cur_filter->session, GF_TRUE);

	//walk down the filter chain and find the shortest path to our destination
	//stop when the current filter is not a one-to-one filter
	while (1) {
		u32 fchain_len;
		GF_FilterPidInst *an_inpid = NULL;
		gf_mx_p(cur_filter->tasks_mx);
		if ((cur_filter->num_input_pids>1) || (cur_filter->num_output_pids>1)) {
			gf_mx_v(cur_filter->tasks_mx);
			break;
		}

		an_inpid = gf_list_get(cur_filter->input_pids, 0);
		if (!an_inpid) {
			gf_mx_v(cur_filter->tasks_mx);
			break;
		}
		if (is_encoder || gf_filter_pid_caps_match((GF_FilterPid *)an_inpid, filter_dst->freg, filter_dst, NULL, NULL, NULL, -1)) {
			link_from_pid = an_inpid->pid;
			gf_mx_v(cur_filter->tasks_mx);
			break;
		}
		fchain_len = gf_filter_pid_resolve_link_length(an_inpid->pid, filter_dst);
		if (fchain_len && (!min_chain_len || (min_chain_len > fchain_len))) {
			min_chain_len = fchain_len;
			link_from_pid = an_inpid->pid;
		}
		gf_mx_v(cur_filter->tasks_mx);
		cur_filter = an_inpid->pid->filter;

		if (!cur_filter->dynamic_filter) {
			break;
		}
	}

	//PID is already a relink target for destination, silently ignore - this may happen when reconfigure tasks are triggered
	//before the relinking is done
	if (src_pidinst == filter_dst->swap_pidinst_dst) {
		gf_fs_check_graph_load(cur_filter->session, GF_FALSE);
		return;
	}

	if (!link_from_pid) {
		gf_fs_check_graph_load(cur_filter->session, GF_FALSE);

		if (from_pidinst->pid->num_destinations==1) {
			GF_FilterEvent evt;
			GF_FilterPid *ipid = (GF_FilterPid *) from_pidinst;
			GF_FEVT_INIT(evt, GF_FEVT_PLAY, ipid);
			gf_filter_pid_send_event_internal(ipid, &evt, GF_TRUE);
			GF_FEVT_INIT(evt, GF_FEVT_STOP, ipid);
			gf_filter_pid_send_event_internal(ipid, &evt, GF_TRUE);
			from_pidinst->filter->session->last_connect_error = reason;
		}
		gf_fs_post_disconnect_task(cur_filter->session, from_pidinst->filter, from_pidinst->pid);
		return;
	}
	//detach the pidinst, and relink from the new input pid
	gf_filter_renegotiate_output_dst(link_from_pid, link_from_pid->filter, filter_dst, dst_pidinst, src_pidinst);
}

GF_Filter *gf_fs_load_encoder(GF_FilterSession *fsess, const char *args, GF_List *filter_blacklist, GF_Err *err_code);

void gf_filter_renegotiate_output_dst(GF_FilterPid *pid, GF_Filter *filter, GF_Filter *filter_dst, GF_FilterPidInst *dst_pidi, GF_FilterPidInst *src_pidi)
{
	Bool is_new_chain = GF_TRUE;
	GF_Filter *new_f, *src_f;
	Bool reconfig_only = src_pidi ? GF_FALSE: GF_TRUE;

	gf_assert(filter);

	if (!filter_dst) {
		//if no destinations, the filter was removed while negotiating
		//this happens for example when doing 'src enc_audio enc_video VIDEOONLY_DST'
		//the removal of enc_audio is triggered after potential capacity negotiation with an adaptation filter
		if (pid->num_destinations) {
			GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Internal error, lost destination for pid %s in filter %s while negotiating caps !!\n", pid->name, filter->name));
		}
		return;
	}

	src_f = src_pidi ? src_pidi->pid->filter : pid->pid->filter;

	if (src_pidi && src_pidi->filter->encoder_codec_id) {
		new_f = gf_fs_load_encoder(filter->session, src_pidi->filter->orig_args, filter->blacklisted, NULL);

		//store destination
		if (new_f) {
			gf_free(new_f->name);
			new_f->name = gf_strdup("SOMETEST");
			gf_list_add(new_f->destination_filters, filter_dst);
		}
	}
	//try to load filters to reconnect output pid
	//we pass the source pid if given, so that we are sure the active property set is used to match the caps
	else if (!reconfig_only
		&& (gf_list_find(src_f->blacklisted, (void*)filter_dst->freg)<0)
		&& gf_filter_pid_caps_match(src_pidi ? (GF_FilterPid *)src_pidi : pid, filter_dst->freg, filter_dst, NULL, NULL, NULL, -1)
	) {
		GF_FilterPidInst *a_dst_pidi;
		new_f = pid->filter;
		gf_assert(pid->num_destinations==1);
		a_dst_pidi = gf_list_get(pid->destinations, 0);
		//we are replacing the chain, remove filters until dest, keeping the final PID connected since we will detach
		// and reattach it
		if (!filter_dst->sticky) filter_dst->sticky = 2;
		gf_filter_remove_internal(a_dst_pidi->filter, filter_dst, GF_TRUE);
		is_new_chain = GF_FALSE;

		//we will reassign packets from that pid instance to the new connection
		gf_assert(!filter_dst->swap_pidinst_dst);
		filter_dst->swap_pidinst_dst = a_dst_pidi;
		filter_dst->swap_pending = GF_TRUE;

		src_pidi->filter->removed = 2;

	}
	//we are inserting a new chain for reconfiguration only
	else if (reconfig_only) {
		gf_fs_check_graph_load(filter_dst->session, GF_TRUE);
		//make sure we don't try the PID parent filter since we just failed reconfiguring it
		gf_list_add(pid->filter->blacklisted, (void *) pid->filter->freg);
		new_f = gf_filter_pid_resolve_link_for_caps(pid, filter_dst, GF_TRUE);

		gf_list_del_item(pid->filter->blacklisted, (void *)pid->filter->freg);

		//special case: no adaptation filter found but destination filter has forced caps set, try to load a filter chain allowing for new caps
		if (!new_f && filter_dst->forced_caps) {
			new_f = gf_filter_pid_resolve_link_for_caps(pid, filter_dst, GF_FALSE);
			if (new_f) {
				//drop caps negotiate
				reconfig_only = GF_FALSE;
			}
		}
	}
	//we are inserting a new chain
	else {
		Bool reassigned;
		gf_fs_check_graph_load(filter_dst->session, GF_TRUE);
		new_f = gf_filter_pid_resolve_link(pid, filter_dst, &reassigned);
	}

	gf_fs_check_graph_load(filter_dst->session, GF_FALSE);

	if (! new_f) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("No suitable filter to adapt caps between pid %s in filter %s to filter %s, disconnecting pid!\n", pid->name, filter->name, filter_dst->name));
		filter->session->last_connect_error = GF_FILTER_NOT_FOUND;

		if (pid->adapters_blacklist) {
			gf_list_del(pid->adapters_blacklist);
			pid->adapters_blacklist = NULL;
		}
		if (pid->num_destinations==1) {
			GF_FilterEvent evt;
			GF_FEVT_INIT(evt, GF_FEVT_PLAY, pid);
			gf_filter_pid_send_event_internal(pid, &evt, GF_TRUE);
			GF_FEVT_INIT(evt, GF_FEVT_STOP, pid);
			gf_filter_pid_send_event_internal(pid, &evt, GF_TRUE);
		}
		if (dst_pidi) {
			gf_fs_post_disconnect_task(filter->session, dst_pidi->filter, dst_pidi->pid);
		}
		return;
	}
	//detach pid instance from its source pid
	if (dst_pidi) {
		//signal as detached, this will prevent any further packet access
		safe_int_inc(&dst_pidi->detach_pending);

		//we need to first reconnect the pid and then detach the output
		if (is_new_chain) {
			//signal a stream reset is pending to prevent filter entering endless loop
			safe_int_inc(&dst_pidi->filter->stream_reset_pending);
			gf_assert(!new_f->swap_pidinst_dst);
			gf_assert(!new_f->swap_pidinst_src);
			//keep track of the pidinst being detached in the target filter
			new_f->swap_pidinst_dst = dst_pidi;
			//keep track of the pidinst being detached from the source filter
			new_f->swap_pidinst_src = src_pidi;
			//remember the new filter for the swap - cf gf_filter_pid_connect_task
			if (src_pidi)
				src_pidi->swap_source = new_f;
			new_f->swap_needs_init = GF_TRUE;
			new_f->swap_pending = GF_TRUE;
		}
		//we directly detach the pid
		else {
			safe_int_inc(&dst_pidi->pid->filter->detach_pid_tasks_pending);
			safe_int_inc(&filter_dst->detach_pid_tasks_pending);
			gf_fs_post_task(filter->session, gf_filter_pid_detach_task, filter_dst, dst_pidi->pid, "pidinst_detach", filter_dst);
		}
	}

	if (reconfig_only) {
		gf_fatal_assert(pid->caps_negotiate);
		new_f->caps_negotiate = pid->caps_negotiate;
		safe_int_inc(&new_f->caps_negotiate->reference_count);
	}

	if (is_new_chain) {
		//mark this filter has having pid connection pending to prevent packet dispatch until the connection is done
		safe_int_inc(&pid->filter->out_pid_connection_pending);
		gf_filter_pid_post_connect_task(new_f, pid);
	} else {
		gf_fs_post_task(filter->session, gf_filter_pid_reconfigure_task, filter_dst, pid, "pidinst_reconfigure", NULL);
	}
}

Bool gf_filter_reconf_output(GF_Filter *filter, GF_FilterPid *pid)
{
	GF_Err e;
	GF_FilterPidInst *src_pidi;
	GF_FilterPid *src_pid;

	gf_mx_p(filter->tasks_mx);
	src_pidi = gf_list_get(filter->input_pids, 0);
	src_pid = src_pidi->pid;
	if (filter->is_pid_adaptation_filter) {
		//do not remove from destination_filters, needed for end of pid_init task
		if (!filter->dst_filter) filter->dst_filter = gf_list_get(filter->destination_filters, 0);
		//in case the adaptation filter is not defining an explicit stream type or codec type
		if (!filter->dst_filter && filter->cap_dst_filter) filter->dst_filter = filter->cap_dst_filter;
		gf_assert(filter->dst_filter);
		gf_assert(filter->num_input_pids==1);
	}
	//swap to pid
	pid->caps_negotiate = filter->caps_negotiate;
	filter->caps_negotiate = NULL;
	if (filter->freg->reconfigure_output) {
		e = filter->freg->reconfigure_output(filter, pid);
	} else {
		//happens for decoders
		e = GF_OK;
	}

	if (e!=GF_OK) {
		GF_LOG(GF_LOG_WARNING, GF_LOG_FILTER, ("PID Adaptation Filter %s output reconfiguration error %s, discarding filter and reloading new adaptation chain\n", filter->name, gf_error_to_string(e)));
		gf_filter_pid_retry_caps_negotiate(src_pid, pid, filter->dst_filter);
		gf_mx_v(filter->tasks_mx);
		return GF_FALSE;
	}
	GF_LOG(GF_LOG_INFO, GF_LOG_FILTER, ("PID Adaptation Filter %s output reconfiguration OK (between filters %s and %s)\n", filter->name, src_pid->filter->name, filter->dst_filter->name));

	gf_filter_check_output_reconfig(filter);

	//success !
	if (src_pid->adapters_blacklist) {
		gf_list_del(src_pid->adapters_blacklist);
		src_pid->adapters_blacklist = NULL;
	}
	gf_assert(pid->caps_negotiate->reference_count);
	if (safe_int_dec(&pid->caps_negotiate->reference_count) == 0) {
		gf_props_del(pid->caps_negotiate);
	}
	pid->caps_negotiate = NULL;
	if (filter->is_pid_adaptation_filter) {
		filter->dst_filter = NULL;
	}
	gf_mx_v(filter->tasks_mx);
	return GF_TRUE;
}

static void gf_filter_renegotiate_output(GF_Filter *filter, Bool force_afchain_insert)
{
	u32 i, j;
	gf_assert(filter->nb_caps_renegotiate );
	safe_int_dec(& filter->nb_caps_renegotiate );

	gf_mx_p(filter->tasks_mx);

	for (i=0; i<filter->num_output_pids; i++) {
		GF_FilterPid *pid = gf_list_get(filter->output_pids, i);
		if (pid->caps_negotiate) {
			Bool is_ok = GF_FALSE;
			Bool reconfig_direct = GF_FALSE;

			//the caps_negotiate property map is create with ref count 1

			//no fanout, we can try direct reconfigure of the filter
			if (pid->num_destinations<=1)
				reconfig_direct = GF_TRUE;
			//fanout but we have as many pid instances being negotiated as there are destinations, we can try direct reconfigure
			else if (pid->num_destinations==gf_list_count(pid->caps_negotiate_pidi_list) && pid->caps_negotiate_direct)
				reconfig_direct = GF_TRUE;

			if (!force_afchain_insert && reconfig_direct && !gf_filter_pid_caps_negociate_match(pid, filter->freg)) {
				reconfig_direct = GF_FALSE;
			}

			//we cannot reconfigure output if more than one destination
			if (reconfig_direct && filter->freg->reconfigure_output && !force_afchain_insert) {
				GF_Err e = filter->freg->reconfigure_output(filter, pid);
				if (e) {
					if (filter->is_pid_adaptation_filter) {
						GF_FilterPidInst *src_pidi = gf_list_get(filter->input_pids, 0);
						GF_FilterPidInst *pidi = gf_list_get(pid->destinations, 0);

						GF_LOG(GF_LOG_WARNING, GF_LOG_FILTER, ("PID Adaptation Filter %s output reconfiguration error %s, discarding filter and reloading new adaptation chain\n", filter->name, gf_error_to_string(e)));

						gf_assert(filter->num_input_pids==1);

						gf_filter_pid_retry_caps_negotiate(src_pidi->pid, pid, pidi->filter);

						continue;
					}
					GF_LOG(GF_LOG_WARNING, GF_LOG_FILTER, ("Filter %s output reconfiguration error %s, loading filter chain for renegotiation\n", filter->name, gf_error_to_string(e)));
				} else {
					is_ok = GF_TRUE;
					gf_filter_check_output_reconfig(filter);
				}
			} else {
				GF_LOG(GF_LOG_INFO, GF_LOG_FILTER, ("Filter %s cannot reconfigure output pids, loading filter chain for renegotiation\n", filter->name));
			}

			if (!is_ok) {
				GF_Filter *filter_dst;
				//we are currently connected to output
				if (pid->num_destinations) {
					for (j=0; j<pid->num_destinations; j++) {
						GF_FilterPidInst *pidi = gf_list_get(pid->destinations, j);
						if (gf_list_find(pid->caps_negotiate_pidi_list, pidi)<0)
							continue;
						filter_dst = pidi->filter;

						//prevent filter from unloading in case we have to disconnect the pid
						if (!filter_dst->sticky) filter_dst->sticky = 2;
						gf_filter_renegotiate_output_dst(pid, filter, filter_dst, pidi, NULL);

					}
				}
				//we are disconnected (unload of a previous adaptation filter)
				else {
					filter_dst = pid->caps_dst_filter;
					gf_assert(pid->num_destinations==0);
					pid->caps_dst_filter = NULL;
					gf_filter_renegotiate_output_dst(pid, filter, filter_dst, NULL, NULL);
				}
			}
			gf_assert(pid->caps_negotiate->reference_count);
			if (safe_int_dec(&pid->caps_negotiate->reference_count) == 0) {
				gf_props_del(pid->caps_negotiate);
			}
			pid->caps_negotiate = NULL;
			if (pid->caps_negotiate_pidi_list) {
				gf_list_del(pid->caps_negotiate_pidi_list);
				pid->caps_negotiate_pidi_list = NULL;
			}
		}
	}
	gf_mx_v(filter->tasks_mx);
}

void gf_filter_renegotiate_output_task(GF_FSTask *task)
{
	//it is possible that the cap renegotiation was already done at the time we process this task
	if (task->filter->nb_caps_renegotiate)
		gf_filter_renegotiate_output(task->filter, GF_TRUE);
}

static Bool session_should_abort(GF_FilterSession *fs)
{
	if (fs->run_status<GF_OK) return GF_TRUE;
	if (!fs->run_status) return GF_FALSE;
	return fs->in_final_flush;
}

static void gf_filter_check_pending_tasks(GF_Filter *filter, GF_FSTask *task)
{
	if (session_should_abort(filter->session)) {
		return;
	}

	//lock task mx to take the decision whether to requeue a new task or not (cf gf_filter_post_process_task)
	//TODO: find a way to bypass this mutex ?
	gf_mx_p(filter->tasks_mx);

	gf_assert(filter->scheduled_for_next_task || filter->session->direct_mode);
	gf_assert(filter->process_task_queued);
	if (safe_int_dec(&filter->process_task_queued) == 0) {
		//we have pending packets, auto-post and requeue
		if (filter->pending_packets && filter->num_input_pids
			&& (!filter->num_output_pids || filter->nb_pids_playing)
			//do NOT do this if running in prevent play mode and blocking mode, as user is likely expecting fs_run() to return until the play event is sent
			&& !filter->session->non_blocking && !(filter->session->flags & GF_FS_FLAG_PREVENT_PLAY)
		) {
			safe_int_inc(&filter->process_task_queued);
			task->requeue_request = GF_TRUE;
		}
		//special case here: no more pending packets, filter has detected eos on one of its input but is still generating packet,
		//we reschedule (typically flush of decoder)
		else if (filter->eos_probe_state == 2) {
			safe_int_inc(&filter->process_task_queued);
			task->requeue_request = GF_TRUE;
			filter->eos_probe_state = 0;
		}
		//we are done for now
		else {
			task->requeue_request = GF_FALSE;
		}
	} else {
		//we have some more process() requests queued, requeue
		task->requeue_request = GF_TRUE;
	}
	if (task->requeue_request) {
		GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("[Filter] %s kept in scheduler blocking %d\n", filter->name, filter->would_block));
	} else {
		GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("[Filter] %s removed from scheduler - blocking %d\n", filter->name, filter->would_block));
	}
	gf_mx_v(filter->tasks_mx);
}

#ifdef GPAC_MEMORY_TRACKING
static GF_Err gf_filter_process_check_alloc(GF_Filter *filter)
{
	GF_Err e;
	u32 nb_allocs=0, nb_callocs=0, nb_reallocs=0, nb_free=0;
	u32 prev_nb_allocs=0, prev_nb_callocs=0, prev_nb_reallocs=0, prev_nb_free=0;

	//reset alloc/realloc stats of filter
	filter->session->nb_alloc_pck = 0;
	filter->session->nb_realloc_pck = 0;
	//get current alloc state
	gf_mem_get_stats(&prev_nb_allocs, &prev_nb_callocs, &prev_nb_reallocs, &prev_nb_free);
	e = filter->freg->process(filter);

	//get new alloc state
	gf_mem_get_stats(&nb_allocs, &nb_callocs, &nb_reallocs, &nb_free);
	//remove prev alloc stats
	nb_allocs -= prev_nb_allocs;
	nb_callocs -= prev_nb_callocs;
	nb_reallocs -= prev_nb_reallocs;
	nb_free -= prev_nb_free;

	//remove internal allocs/reallocs due to filter lib
	if (nb_allocs>filter->session->nb_alloc_pck)
		nb_allocs -= filter->session->nb_alloc_pck;
	else
		nb_allocs = 0;

	if (nb_reallocs>filter->session->nb_realloc_pck)
		nb_reallocs -= filter->session->nb_realloc_pck;
	else
		nb_reallocs = 0;

	//we now have nomber of allocs/realloc used by the filter internally during its process
	if (nb_allocs || nb_callocs || nb_reallocs /* || nb_free */) {
		filter->stats_nb_alloc += nb_allocs;
		filter->stats_nb_calloc += nb_callocs;
		filter->stats_nb_realloc += nb_reallocs;
		filter->stats_nb_free += nb_free;
	} else {
		filter->nb_consecutive_process ++;
	}
	filter->nb_process_since_reset++;
	return e;

}
#endif

static GFINLINE void check_filter_error(GF_Filter *filter, GF_Err e, Bool for_reconnection)
{
	GF_Err out_e = e;
	Bool kill_filter = GF_FALSE;
	if (e>GF_OK) e = GF_OK;
	else if (e==GF_IP_NETWORK_EMPTY) e = GF_OK;

	if (e) {
		u64 diff;
		filter->session->last_process_error = e;

		filter->nb_errors ++;
		if (!filter->nb_consecutive_errors) filter->time_at_first_error = gf_sys_clock_high_res();

		filter->nb_consecutive_errors ++;
		if (filter->nb_pck_io && !filter->session->in_final_flush)
			filter->nb_consecutive_errors = 0;
		//give it at most one second
		diff = gf_sys_clock_high_res() - filter->time_at_first_error;
		if (diff >= 1000000) {
			GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("[Filter] %s (idx %u) in error / not responding properly: %d consecutive errors in "LLU" us with no packet discarded or sent\n\tdiscarding all inputs and notifying end of stream on all outputs\n", filter->name, 1+gf_list_find(filter->session->filters, filter), filter->nb_consecutive_errors, diff));
			kill_filter = GF_TRUE;
		}
	} else {
		if ((!filter->nb_pck_io && filter->pending_packets && (filter->nb_pids_playing>0) && !gf_filter_connections_pending(filter)) || for_reconnection) {
			if (!filter->nb_consecutive_errors)
				filter->time_at_first_error = gf_sys_clock_high_res();
			filter->nb_consecutive_errors++;

			out_e = GF_SERVICE_ERROR;
			if (filter->nb_consecutive_errors >= 100000) {
				if (for_reconnection) {
					GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("[Filter] %s (idx %u) not responding properly: %d consecutive attempts at reconfiguring\n\tdiscarding all inputs and notifying end of stream on all outputs\n", filter->name, 1+gf_list_find(filter->session->filters, filter), filter->nb_consecutive_errors));
				} else if (!filter->session->in_final_flush) {
					GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("[Filter] %s (idx %u) not responding properly: %d consecutive process with no packet discarded or sent, but %d packets pending\n\tdiscarding all inputs and notifying end of stream on all outputs\n", filter->name, 1+gf_list_find(filter->session->filters, filter), filter->nb_consecutive_errors, filter->pending_packets));
				} else {
					out_e = GF_OK;
				}
				kill_filter = GF_TRUE;
			}
		} else {
			filter->nb_consecutive_errors = 0;
			filter->nb_pck_io = 0;
		}
	}

	if (kill_filter) {
		u32 i;
		gf_mx_p(filter->tasks_mx);
		for (i=0; i<filter->num_input_pids; i++) {
			GF_FilterPidInst *pidi = gf_list_get(filter->input_pids, i);
			gf_filter_pid_set_discard((GF_FilterPid *)pidi, GF_TRUE);
		}
		for (i=0; i<filter->num_output_pids; i++) {
			GF_FilterPid *pid = gf_list_get(filter->output_pids, i);
			gf_filter_pid_set_eos(pid);
		}
		gf_mx_v(filter->tasks_mx);
		filter->session->last_process_error = out_e;
		filter->disabled = GF_FILTER_DISABLED;
	}
}

static void gf_filter_process_task(GF_FSTask *task)
{
	GF_Err e;
	Bool skip_block_mode = GF_FALSE;
	GF_Filter *filter = task->filter;
	Bool force_block_state_check=GF_FALSE;
	gf_assert(task->filter);
	gf_assert(filter->freg);
	gf_assert(filter->freg->process);
	task->can_swap = 1;

	filter->schedule_next_time = 0;

	if (filter->disabled) {
		GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Filter %s is disabled, cancelling process\n", filter->name));
		gf_mx_p(task->filter->tasks_mx);
		task->filter->process_task_queued = 0;
		gf_mx_v(task->filter->tasks_mx);
		return;
	}

	if (filter->out_pid_connection_pending || filter->detached_pid_inst || filter->caps_negotiate) {
		GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Filter %s has %s pending, requeuing process\n", filter->name, filter->out_pid_connection_pending ? "connections" : filter->caps_negotiate ? "caps negotiation" : "input pid reassignments"));
		//do not cancel the process task since it might have been triggered by the filter itself,
		//we would not longer call it
		task->requeue_request = GF_TRUE;

 		gf_assert(filter->process_task_queued);
		//in we are during the graph resolution phase, to not ask for RT reschedule: this can post-pone process tasks and change their initial
		//scheduling order, resulting in random change of input pid declaration, for example:
		//fin1 -> reframe1 -> fA
		//fin2 -> reframe2 -> fA
		//if we postpone by 10 us finX process while wating for rfX->fA setup, depending on the CPU charge fin2 might be rescheduled before fin1
		//leading to pushing new/pending packets ro reframe2 before reframe1, and having fA declare its pid in the reverse order as the one expected
		//note that this is only valid for single-thread case, as in multithread we do not guarantee PID declaration order
		if (!filter->out_pid_connection_pending) {
			task->schedule_next_time = gf_sys_clock_high_res() + 10000;
			check_filter_error(filter, GF_OK, GF_TRUE);
		}
		return;
	}
	if (filter->removed || filter->finalized) {
		GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Filter %s has been %s, skipping process\n", filter->name, filter->finalized ? "finalized" : "removed"));
		return;
	}

	if (filter->prevent_blocking) skip_block_mode = GF_TRUE;
	else if (filter->in_eos_resume) skip_block_mode = GF_TRUE;
	else if (filter->session->in_final_flush) skip_block_mode = GF_TRUE;

	//blocking filter: remove filter process task - task will be reinserted upon unblock()
	if (!skip_block_mode && filter->would_block && (filter->would_block + filter->num_out_pids_not_connected == filter->num_output_pids ) ) {
		gf_mx_p(task->filter->tasks_mx);
		//it may happen that by the time we get the lock, the filter has been unblocked by another thread. If so, don't skip task
		if (filter->would_block) {
			filter->nb_tasks_done--;
			task->filter->process_task_queued = 0;
			GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Filter %s blocked, skipping process\n", filter->name));
			gf_mx_v(task->filter->tasks_mx);
			return;
		}
		gf_mx_v(task->filter->tasks_mx);
	}
	if (filter->stream_reset_pending) {
		GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Filter %s has stream reset pending, postponing process\n", filter->name));
		filter->nb_tasks_done--;
		task->requeue_request = GF_TRUE;
		gf_assert(filter->process_task_queued);
		return;
	}
	gf_assert(filter->process_task_queued);
	if (filter->multi_sink_target) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Filter %s is a multi-sink target, process disabled\n", filter->name));
		return;
	}

	//the following breaks demuxers where PIDs are not all known from start: if we filter some pids due to user request,
	//we may end up with the following test true but not all PIDs yet declared, hence no more processing
	//we could add a filter cap for that, but for now we simply rely on the blocking mode algo only
#if 0
	if (filter->num_output_pids && (filter->num_out_pids_not_connected==filter->num_output_pids)) {
		GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Filter %s has no valid connected outputs, skipping process\n", filter->name));
		return;
	}
#endif

	//we have postponed packets on filter, flush them
	if (task->filter->postponed_packets) {
		while (gf_list_count(task->filter->postponed_packets)) {
			GF_FilterPacket *pck = gf_list_pop_front(task->filter->postponed_packets);
			e = gf_filter_pck_send_internal(pck, GF_FALSE);
			if (e==GF_PENDING_PACKET) {
				//packet is pending so was added at the end of our postponed queue - remove from queue and reinsert in front
				gf_list_del_item(task->filter->postponed_packets, pck);
				gf_list_insert(task->filter->postponed_packets, pck, 0);
				task->requeue_request = filter->deferred_link ? GF_FALSE : GF_TRUE;
				GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Filter %s still has postponed packets, postponing process\n", filter->name));
				return;
			}
		}
		gf_list_del(task->filter->postponed_packets);
		task->filter->postponed_packets = NULL;
	}
	FSESS_CHECK_THREAD(filter)

	filter->nb_pck_io = 0;

	if (filter->nb_caps_renegotiate) {
		gf_filter_renegotiate_output(filter, GF_FALSE);
	}

	GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Filter %s process\n", filter->name));

	filter->in_process_callback = GF_TRUE;

#ifdef GPAC_MEMORY_TRACKING
	if (filter->session->check_allocs)
		e = gf_filter_process_check_alloc(filter);
	else
#endif
		e = filter->freg->process(filter);

	filter->in_process_callback = GF_FALSE;
	GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Filter %s process done\n", filter->name));

	//flush all pending pid init requests following the call to init
	gf_filter_check_pending_pids(filter);


	//no requeue if end of session
	if (session_should_abort(filter->session)) {
		return;
	}
	//if eos but we still have pending packets or process tasks queued, move to GF_OK so that
	//we evaluate the blocking state
	if (e==GF_EOS) {
		if (filter->postponed_packets && filter->num_input_pids) {
		 	e = GF_OK;
		} else if (filter->process_task_queued) {
			e = GF_OK;
			force_block_state_check = GF_TRUE;
		}
	}

	if ((e==GF_EOS) || filter->removed || filter->finalized) {
		gf_mx_p(filter->tasks_mx);
		filter->process_task_queued = 0;
		gf_mx_v(filter->tasks_mx);
		return;
	}

	if ((e==GF_PROFILE_NOT_SUPPORTED) && filter->has_out_caps && !(filter->session->flags & GF_FS_FLAG_NO_REASSIGN)) {
		u32 i;
		//disconnect all other inputs, and post a re-init
		gf_mx_p(filter->tasks_mx);
		for (i=0; i<filter->num_input_pids; i++) {
			GF_FilterPidInst *a_pidinst = gf_list_get(filter->input_pids, i);

			GF_LOG(GF_LOG_WARNING, GF_LOG_FILTER, ("Codec/Profile not supported for filter %s - blacklisting as output from %s and retrying connections\n", filter->name, a_pidinst->pid->filter->name));

			gf_list_add(a_pidinst->pid->filter->blacklisted, (void *) filter->freg);

			gf_filter_relink_dst(a_pidinst, e);
		}
		filter->process_task_queued = 0;
		gf_mx_v(filter->tasks_mx);
		return;
	}
	check_filter_error(filter, e, GF_FALSE);

	//source filters, flush data if enough space available.
	if ( (!filter->num_output_pids || (filter->would_block + filter->num_out_pids_not_connected < filter->num_output_pids) )
		&& !filter->input_pids
		&& (e!=GF_EOS)
		&& !force_block_state_check
	) {
		if (filter->schedule_next_time)
			task->schedule_next_time = filter->schedule_next_time;
		task->requeue_request = GF_TRUE;
		gf_assert(filter->process_task_queued);
	}
	//filter requested a requeue
	else if (filter->schedule_next_time) {
		if (!filter->session->in_final_flush) {
			task->schedule_next_time = filter->schedule_next_time;
			task->requeue_request = GF_TRUE;
			gf_assert(filter->process_task_queued);
		}
	}
	//last task for filter but pending packets and not blocking, requeue in main scheduler
	else if ((filter->would_block < filter->num_output_pids)
			&& filter->pending_packets
			&& (filter->nb_pids_playing>0)
			&& (gf_fq_count(filter->tasks)<=1)
	) {
		//prune eos packets that could still be present
		if (filter->pending_packets && filter->session->in_final_flush) {
			u32 i;
			for (i=0; i<filter->num_input_pids; i++) {
				GF_FilterPidInst *pidi = gf_list_get(filter->input_pids, i);
				gf_filter_pid_get_packet((GF_FilterPid *)pidi);
			}
			if (!filter->num_input_pids)
				filter->pending_packets = 0;
		}
		task->requeue_request = GF_TRUE;
		task->can_swap = 2;
		gf_assert(filter->process_task_queued);
	}
	else {
		gf_assert (!filter->schedule_next_time);
		gf_filter_check_pending_tasks(filter, task);
		if (task->requeue_request) {
			task->can_swap = 2;
			gf_assert(filter->process_task_queued);
		}
	}
}

void gf_filter_process_inline(GF_Filter *filter)
{
	GF_Err e;
	if (filter->out_pid_connection_pending || filter->removed || filter->stream_reset_pending || filter->multi_sink_target) {
		return;
	}
	if (filter->would_block && (filter->would_block == filter->num_output_pids) ) {
		return;
	}
	if (filter->in_process || filter->in_process_callback) {
		return;
	}
	GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Filter %s inline process\n", filter->name));

	if (filter->postponed_packets) {
		while (gf_list_count(filter->postponed_packets)) {
			GF_FilterPacket *pck = gf_list_pop_front(filter->postponed_packets);
			gf_filter_pck_send(pck);
		}
		gf_list_del(filter->postponed_packets);
		filter->postponed_packets = NULL;
		if (filter->process_task_queued==1) {
			//do not touch process_task_queued, we are outside regular fs task calls
			return;
		}
	}
	FSESS_CHECK_THREAD(filter)

	filter->in_process = GF_TRUE;
	filter->in_process_callback = GF_TRUE;

#ifdef GPAC_MEMORY_TRACKING
	if (filter->session->check_allocs)
		e = gf_filter_process_check_alloc(filter);
	else
#endif
		e = filter->freg->process(filter);

	filter->in_process_callback = GF_FALSE;
	filter->in_process = GF_FALSE;

	//flush all pending pid init requests following the call to init
	gf_filter_check_pending_pids(filter);


	//no requeue if end of session
	if (session_should_abort(filter->session)) {
		return;
	}
	if ((e==GF_EOS) || filter->removed || filter->finalized) {
		//do not touch process_task_queued, we are outside regular fs task calls
		return;
	}
	check_filter_error(filter, e, GF_FALSE);
}

GF_EXPORT
void gf_filter_send_update(GF_Filter *filter, const char *fid, const char *name, const char *val, GF_EventPropagateType propagate_mask)
{
	if (filter) gf_fs_send_update(filter->session, fid, fid ? NULL : filter, name, val, propagate_mask);
}

GF_Filter *gf_filter_clone(GF_Filter *filter, GF_Filter *source_filter)
{
	GF_Filter *new_filter;

	if (source_filter) {
		GF_Filter *old_source;
		GF_FilterPidInst *first_in = gf_list_get(filter->input_pids, 0);
		//if source filter is set, this is a clone due to a new instance request, so we have at least one input
		if (!first_in) return NULL;
		old_source = first_in->pid->filter;
		//get source arguments for new source filter connecting to the clone
		const char *args_src_new = gf_filter_get_args_stripped(filter->session, source_filter->src_args ? source_filter->src_args : source_filter->orig_args, GF_FALSE);
		//get source arguments for previous source filter connected to the clone
		const char *args_src_old = gf_filter_get_args_stripped(filter->session, old_source->src_args ? old_source->src_args : old_source->orig_args, GF_FALSE);

		//remove all old source args and append new source args
		//this allows dealing with -i live.mpd:OPT1 -i live2.mpd:OPT2
		//where the filter (dashin here) will request a clone when fin:live2.mpd connects, we must use the options from live2.mpd
		char *args = gf_strdup(filter->orig_args ? filter->orig_args : "");
		u32 arg_len = (u32) strlen(args);
		char *old_args = args_src_old ? strstr(args, args_src_old) : NULL;
		if (old_args) {
			u32 offset = (u32) (old_args - args);
			u32 old_args_len = (u32) strlen(args_src_old);
			memmove(old_args, old_args+old_args_len, arg_len - old_args_len - offset);
			old_args[arg_len - old_args_len - offset]=0;
		}
		if (args_src_new) {
			char szSep[2];
			szSep[0] = filter->session->sep_args;
			szSep[1] = 0;
			gf_dynstrcat(&args, args_src_new, szSep);
		}

		new_filter = gf_filter_new(filter->session, filter->freg, args, NULL, filter->arg_type, NULL, NULL, GF_FALSE);
		gf_free(args);
	} else {
		new_filter = gf_filter_new(filter->session, filter->freg, filter->orig_args, NULL, filter->arg_type, NULL, NULL, GF_FALSE);
	}
	if (!new_filter) return NULL;
	new_filter->cloned_from = filter;
	new_filter->dynamic_filter = filter->dynamic_filter ? 1 : 0;
	GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Filter cloned (register %s, args %s)\n", filter->freg->name, filter->orig_args ? filter->orig_args : "none"));

	return new_filter;
}

GF_EXPORT
u32 gf_filter_get_ipid_count(GF_Filter *filter)
{
	return filter->num_input_pids;
}

GF_EXPORT
GF_FilterPid *gf_filter_get_ipid(GF_Filter *filter, u32 idx)
{
	return gf_list_get(filter->input_pids, idx);
}

GF_EXPORT
u32 gf_filter_get_opid_count(GF_Filter *filter)
{
	return filter->num_output_pids;
}

GF_EXPORT
GF_FilterPid *gf_filter_get_opid(GF_Filter *filter, u32 idx)
{
	return gf_list_get(filter->output_pids, idx);
}

void gf_filter_post_process_task_internal(GF_Filter *filter, Bool use_direct_dispatch)
{
	if (filter->finalized || filter->removed)
		return;

	//although this is theoretically OK, it breaks quite some tests. Need further investigation
#if 0
	//if regular posting (not direct) and our caller is the main process function, no need to lock task mutex, just increase
	//the next scheduled time
	if (!use_direct_dispatch && filter->in_process_callback) {
		filter->schedule_next_time = 1 + gf_sys_clock_high_res();
		return;
	}
#endif

	//lock task mx to take the decision whether to post a new task or not (cf gf_filter_check_pending_tasks)
	gf_mx_p(filter->tasks_mx);
	gf_assert((s32)filter->process_task_queued>=0);

	if (use_direct_dispatch) {
		safe_int_inc(&filter->process_task_queued);
		gf_fs_post_task_ex(filter->session, gf_filter_process_task, filter, NULL, "process", NULL, GF_FALSE, GF_FALSE, GF_TRUE, TASK_TYPE_NONE, 0);
	} else if (safe_int_inc(&filter->process_task_queued) <= 1) {
		GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Filter %s added to scheduler\n", filter->name));
//		gf_fs_post_task(filter->session, gf_filter_process_task, filter, NULL, "process", NULL);
		gf_fs_post_task_ex(filter->session, gf_filter_process_task, filter, NULL, "process", NULL, GF_FALSE, GF_FALSE, GF_FALSE, TASK_TYPE_NONE, 0);
	} else {
		GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Filter %s skip post process task\n", filter->name));
		gf_assert(filter->session->run_status
		 		|| filter->session->in_final_flush
		 		|| filter->disabled
				|| (filter->scheduled_for_next_task==GF_FILTER_SCHEDULED)
				|| filter->session->direct_mode
		 		|| gf_fq_count(filter->tasks)
		);
	}
	if (!filter->session->direct_mode && !use_direct_dispatch) {
		gf_assert(filter->process_task_queued);
	}
	gf_mx_v(filter->tasks_mx);
}

GF_EXPORT
void gf_filter_post_process_task(GF_Filter *filter)
{
	gf_filter_post_process_task_internal(filter, GF_FALSE);
}
GF_EXPORT
void gf_filter_ask_rt_reschedule(GF_Filter *filter, u32 us_until_next)
{
	u64 next_time;
	if (filter->removed) return;

	if (!filter->in_process_callback) {
		if (filter->session->direct_mode) return;
		if (filter->session->in_final_flush) {
			filter->schedule_next_time = 0;
			return;
		}
		//allow reschedule if not called from process
		if (us_until_next) {
			next_time = 1+us_until_next + gf_sys_clock_high_res();
			if (!filter->schedule_next_time || (filter->schedule_next_time > next_time))
				filter->schedule_next_time = next_time;
		} else {
			filter->schedule_next_time = 0;
		}

		gf_filter_post_process_task(filter);
		return;
	}
	if (filter->session->in_final_flush)
		us_until_next = 0;

	//if the filter requests rescheduling, consider it is in a valid state and increment pck IOs to avoid flagging it as broken
	filter->nb_pck_io++;
	if (!us_until_next) {
		filter->schedule_next_time = 0;
		return;
	}
	next_time = 1+us_until_next + gf_sys_clock_high_res();
	if (!filter->schedule_next_time || (filter->schedule_next_time > next_time))
		filter->schedule_next_time = next_time;

	GF_LOG(GF_LOG_DEBUG, GF_LOG_SCHEDULER, ("Filter %s real-time reschedule in %d us (at "LLU" sys clock)\n", filter->name, us_until_next, filter->schedule_next_time));
}

GF_EXPORT
void gf_filter_set_setup_failure_callback(GF_Filter *filter, GF_Filter *source_filter, Bool (*on_setup_error)(GF_Filter *f, void *on_setup_error_udta, GF_Err e), void *udta)
{
	if (!filter) return;
	if (!source_filter) return;
	Bool detach = (filter->disabled && !on_setup_error && source_filter->on_setup_error) ? GF_TRUE : GF_FALSE;
	source_filter->on_setup_error = on_setup_error;
	source_filter->on_setup_error_filter = filter;
	source_filter->on_setup_error_udta = udta;

	if (detach) {
		gf_filter_post_remove(filter);
	}
}

struct _gf_filter_setup_failure
{
	GF_Err e;
	GF_Filter *filter;
	GF_Filter *notify_filter;
	Bool do_disconnect;
} filter_setup_failure;

static void gf_filter_setup_failure_task(GF_FSTask *task)
{
	s32 res;
	GF_Err e;
	GF_Filter *f = ((struct _gf_filter_setup_failure *)task->udta)->filter;
	if (task->udta) {
		e = ((struct _gf_filter_setup_failure *)task->udta)->e;
		gf_free(task->udta);
		if (e)
			f->session->last_connect_error = e;
	}

	if (!f->finalized && f->freg->finalize) {
		FSESS_CHECK_THREAD(f)
		f->freg->finalize(f);
	}
	gf_mx_p(f->session->filters_mx);

	res = gf_list_del_item(f->session->filters, f);
	if (res < 0) {
		GF_LOG(GF_LOG_WARNING, GF_LOG_FILTER, ("Filter %s task failure callback on already removed filter!\n", f->name));
	}

	//we will detach output pids, so drop any pending packets before
	gf_filter_reset_pending_packets(f);

	gf_mx_v(f->session->filters_mx);

	gf_mx_p(f->tasks_mx);
	//detach all input pids
	while (gf_list_count(f->input_pids)) {
		GF_FilterPidInst *pidinst = gf_list_pop_back(f->input_pids);
		gf_filter_instance_detach_pid(pidinst);
	}
	//detach all output pids
	while (gf_list_count(f->output_pids)) {
		u32 j;
		GF_FilterPid *pid = gf_list_pop_back(f->output_pids);
		for (j=0; j<pid->num_destinations; j++) {
			GF_FilterPidInst *pidinst = gf_list_get(pid->destinations, j);
			//pid instance already detached, remove it
			if (!pidinst->filter) {
				gf_list_rem(pid->destinations, j);
				pid->num_destinations--;
				j--;
				gf_filter_pid_inst_check_delete(pidinst);
			}
			//marked as detached
			else {
				pidinst->pid = NULL;
			}
		}
		gf_list_reset(pid->destinations);
		gf_filter_pid_del(pid);
	}
	gf_mx_v(f->tasks_mx);
	//avoid destruction of the current task (ourselves)
	gf_fq_pop(f->tasks);

	gf_filter_del(f);
	task->filter = NULL;
	task->requeue_request = GF_FALSE;
}

static void gf_filter_setup_failure_notify_task(GF_FSTask *task)
{
	struct _gf_filter_setup_failure *st = (struct _gf_filter_setup_failure *)task->udta;
	if (st->notify_filter && st->filter->on_setup_error) {
		Bool cancel = st->filter->on_setup_error(st->filter, st->filter->on_setup_error_udta, st->e);
		if (cancel) st->e = GF_OK;
	}

	if (st->do_disconnect) {
		//post setup_failure task ON THE FILTER, otherwise we might end up having 2 threads on the active filter
		gf_fs_post_task_class(st->filter->session, gf_filter_setup_failure_task, st->filter, NULL, "setup_failure", st, TASK_TYPE_SETUP);
	} else {
		gf_free(st);
	}
}

GF_EXPORT
void gf_filter_notification_failure(GF_Filter *filter, GF_Err reason, Bool force_disconnect)
{
	struct _gf_filter_setup_failure *stack;
	if (!filter->on_setup_error_filter && !force_disconnect) return;

	stack = gf_malloc(sizeof(struct _gf_filter_setup_failure));
	stack->e = reason;
	stack->notify_filter = filter->on_setup_error_filter;
	stack->filter = filter;
	stack->do_disconnect = force_disconnect;
	if (force_disconnect) {
		filter->removed = 1;
	}
	if (filter->on_setup_error_filter) {
		gf_fs_post_task_class(filter->session, gf_filter_setup_failure_notify_task, filter->on_setup_error_filter, NULL, "setup_failure_notify", stack, TASK_TYPE_SETUP);
	} else if (force_disconnect) {
		//post setup_failure task ON THE FILTER, otherwise we might end up having 2 threads on the active filter
		gf_fs_post_task_class(filter->session, gf_filter_setup_failure_task, filter, NULL, "setup_failure", stack, TASK_TYPE_SETUP);
	}
}

GF_EXPORT
void gf_filter_setup_failure(GF_Filter *filter, GF_Err reason)
{
	GF_FilterPidInst *pidinst;
	GF_Filter *sfilter;
	GF_Filter *notif_filter;
	if (filter->in_connect_err) {
		filter->in_connect_err = reason;
		return;
	}
	notif_filter = filter;

	//special cases for demux filters, if the source has a setup error callback and
	//was not notified, use the source filter
	pidinst = (filter->num_input_pids==1) ? gf_list_get(filter->input_pids, 0) : NULL;
	sfilter = pidinst ? pidinst->pid->filter : NULL;
	if (sfilter && sfilter->on_setup_error && !sfilter->setup_notified) {
		notif_filter = sfilter;
	}
	//filter was already connected, trigger removal of all pid instances
	else if (filter->num_input_pids) {
		gf_filter_reset_pending_packets(filter);
		filter->removed = 1;
		gf_mx_p(filter->tasks_mx);

		while (filter->num_input_pids) {
			GF_FilterPidInst *a_pidi = gf_list_get(filter->input_pids, 0);
			GF_Filter *a_filter = a_pidi->pid->filter;

			gf_list_del_item(filter->input_pids, a_pidi);

			gf_filter_instance_detach_pid(a_pidi);

			filter->num_input_pids = gf_list_count(filter->input_pids);
			if (!filter->num_input_pids)
				filter->single_source = NULL;

			//post a pid_delete task to also trigger removal of the filter if needed
			gf_fs_post_pid_instance_delete_task(filter->session, a_filter, a_pidi->pid, a_pidi);
		}
		gf_mx_v(filter->tasks_mx);
		if (reason)
			filter->session->last_connect_error = reason;
	}

	//don't accept twice a notif
	if (notif_filter->setup_notified) return;
	notif_filter->setup_notified = GF_TRUE;

	GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Filter %s failed to setup: %s\n", notif_filter->name, gf_error_to_string(reason)));

	gf_filter_notification_failure(notif_filter, reason, GF_TRUE);
	//if we used the source, also send a notif failure on the filter (to trigger removal)
	if (notif_filter != filter) {
		filter->setup_notified = GF_TRUE;
		gf_filter_notification_failure(filter, reason, GF_TRUE);
	}
}

//#define DUMP_TASKS
#if !defined(GPAC_DISABLE_LOG) && defined DUMP_TASKS
static void task_postponed_log(void *udta, void *item)
{
	GF_FSTask *at = (GF_FSTask *)item;
	GF_LOG(GF_LOG_INFO, GF_LOG_FILTER, ("\ttask %s\n", at->log_name));
}
#endif

void gf_filter_remove_task(GF_FSTask *task)
{
	s32 res;
	GF_Filter *f = task->filter;
	u32 count = gf_fq_count(f->tasks);

	//do not destroy filters if tasks for this filter are pending or some ref packets are still present
	if (f->out_pid_connection_pending || f->detach_pid_tasks_pending || f->nb_ref_packets || f->nb_shared_packets_out) {
		task->requeue_request = GF_TRUE;
		return;
	}

	gf_assert(f->finalized);

	if (count!=1) {
		task->requeue_request = GF_TRUE;
		task->can_swap = 1;
#if !defined(GPAC_DISABLE_LOG) && defined DUMP_TASKS
		if (gf_log_tool_level_on(GF_LOG_FILTER, GF_LOG_DEBUG) ) {
			gf_fq_enum(f->tasks, task_postponed_log, NULL);
		}
#endif //GPAC_DISABLE_LOG
		return;
	}
	GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Filter %s destruction task\n", f->name));
	safe_int_dec(&f->session->remove_tasks);

	//avoid destruction of the current task
	gf_fq_pop(f->tasks);

	if (f->freg->finalize) {
		FSESS_CHECK_THREAD(f)
		f->freg->finalize(f);
	}

	gf_mx_p(f->session->filters_mx);

	res = gf_list_del_item(f->session->filters, f);
	if (res<0) {
		GF_LOG(GF_LOG_WARNING, GF_LOG_FILTER, ("Filter %s destruction task on already removed filter\n", f->name));
	}

	gf_mx_v(f->session->filters_mx);

	gf_mx_p(f->tasks_mx);
	//detach all input pids
	while (gf_list_count(f->input_pids)) {
		GF_FilterPidInst *pidinst = gf_list_pop_back(f->input_pids);
		gf_filter_instance_detach_pid(pidinst);
	}
	gf_mx_v(f->tasks_mx);

	gf_filter_del(f);
	task->filter = NULL;
	task->requeue_request = GF_FALSE;
}

void gf_filter_post_remove(GF_Filter *filter)
{
	//session about to be destroy, don't post task
	if (filter->session->run_status==GF_EOS) return;
	gf_assert(!filter->swap_pidinst_dst);
	gf_assert(!filter->swap_pidinst_src);
	gf_assert(!filter->finalized);
	filter->finalized = GF_TRUE;
	safe_int_inc(&filter->session->remove_tasks);
	//post remove task ON THE FILTER, otherwise we might end up having 2 threads on the active filter
	gf_fs_post_task_ex(filter->session, gf_filter_remove_task, filter, NULL, "filter_destroy", NULL, GF_FALSE, filter->session->force_main_thread_tasks, GF_FALSE, TASK_TYPE_NONE, 0);
}

static void gf_filter_tag_remove(GF_Filter *filter, GF_Filter *source_filter, GF_Filter *until_filter, Bool keep_end_connections)
{
	u32 i, count, j, nb_inst;
	u32 nb_rem_inst=0;
	Bool mark_only = GF_FALSE;
	Bool do_unlock;
	if (filter==until_filter) return;

	//we do a try-lock here, as the filter could be locked by another thread
	//this typically happens upon compositor source disconnection while an event or packet drop is still being processed on that filter
	do_unlock = gf_mx_try_lock(filter->tasks_mx);
	for (i=0; i<filter->num_input_pids; i++) {
		GF_FilterPidInst *pidi = gf_list_get(filter->input_pids, i);
		if (pidi->pid->filter==source_filter) nb_rem_inst++;
	}
	if (!nb_rem_inst) {
		if (do_unlock) gf_mx_v(filter->tasks_mx);
		return;
	}
	filter->marked_for_removal = GF_TRUE;
	if (nb_rem_inst != filter->num_input_pids)
		mark_only = GF_TRUE;

	//already removed
	if (filter->removed) {
		if (do_unlock) gf_mx_v(filter->tasks_mx);
		return;
	}
	//filter will be removed, propagate on all output pids
	if (!mark_only)
		filter->removed = 1;

	count = gf_list_count(filter->output_pids);
	for (i=0; i<count; i++) {
		GF_FilterPid *pid = gf_list_get(filter->output_pids, i);
		pid->has_seen_eos = GF_TRUE;
		nb_inst = pid->num_destinations;
		//happens if the pid was disconnected
		if (!nb_inst && !mark_only && pid->not_connected) {
			gf_list_rem(filter->output_pids, i);
			i--;
			count--;
			filter->num_output_pids = gf_list_count(filter->output_pids);
			gf_filter_pid_del(pid);
			continue;
		}
		for (j=0; j<nb_inst; j++) {
			GF_FilterPidInst *pidi = gf_list_get(pid->destinations, j);
			gf_filter_tag_remove(pidi->filter, filter, until_filter, keep_end_connections);
			if (!mark_only && (!keep_end_connections || (pidi->filter != until_filter)) ) {
				//unlock filter before posting remove task on other filter
				if (do_unlock) gf_mx_v(filter->tasks_mx);
				gf_fs_post_disconnect_task(filter->session, pidi->filter, pid);
				do_unlock = gf_mx_try_lock(filter->tasks_mx);
			}
		}
	}
	if (do_unlock) gf_mx_v(filter->tasks_mx);
}

void gf_filter_remove_internal(GF_Filter *filter, GF_Filter *until_filter, Bool keep_end_connections)
{
	u32 i, j, count;

	if (!filter) return;

	if (filter->removed)
		return;

	if (filter==until_filter)
		return;

	if (until_filter) {
		//check if filter has not been removed
		s32 res = gf_list_find(until_filter->session->filters, filter);
		if (res<0)
			return;
		GF_LOG(GF_LOG_INFO, GF_LOG_FILTER, ("Disconnecting filter %s up to %s\n", filter->name, until_filter->name));
	} else {
		GF_LOG(GF_LOG_INFO, GF_LOG_FILTER, ("Disconnecting filter %s from session\n", filter->name));
	}
	//get all dest pids, post disconnect and mark filters as removed
	gf_assert(!filter->removed);
	filter->removed = 1;
	for (i=0; i<filter->num_output_pids; i++) {
		GF_FilterPid *pid = gf_list_get(filter->output_pids, i);
		count = pid->num_destinations;
		for (j=0; j<count; j++) {
			GF_FilterPidInst *pidi = gf_list_get(pid->destinations, j);

			if (until_filter) {
				gf_filter_tag_remove(pidi->filter, filter, until_filter, keep_end_connections);
			}

			if (keep_end_connections && (pidi->filter == until_filter)) {

			} else {
				gf_fs_post_disconnect_task(filter->session, pidi->filter, pid);
			}
		}
	}
	gf_mx_p(filter->tasks_mx);

	if (!filter->num_output_pids && !filter->num_input_pids) {
		gf_filter_post_remove(filter);
		gf_mx_v(filter->tasks_mx);
		return;
	}

	if (keep_end_connections) {
		gf_mx_v(filter->tasks_mx);
		return;
	}

	//check all pids connected to this filter, ensure their owner is only connected to this filter
	for (i=0; i<filter->num_input_pids; i++) {
		GF_FilterPid *pid;
		GF_FilterPidInst *pidi = gf_list_get(filter->input_pids, i);
		Bool can_remove = GF_TRUE;
		//check all output pids of the filter owning this pid are connected to ourselves
		pid = pidi->pid;
		count = pid->num_destinations;
		for (j=0; j<count; j++) {
			GF_FilterPidInst *pidi_o = gf_list_get(pid->destinations, j);
			if (pidi_o->filter != filter) {
				can_remove = GF_FALSE;
				break;
			}
		}
		if (can_remove && !pid->filter->removed) {
			gf_filter_remove_internal(pid->filter, NULL, GF_FALSE);
		}
	}
	gf_mx_v(filter->tasks_mx);
}

GF_EXPORT
void gf_filter_remove_src(GF_Filter *filter, GF_Filter *src_filter)
{
	gf_filter_remove_internal(src_filter, filter, GF_FALSE);
}

static void gf_filter_remove_local(GF_FSTask *task)
{
	u32 i;
	Bool has_pending=GF_FALSE;
	GF_Filter *filter = task->filter;

	gf_mx_p(filter->tasks_mx);
	//check the sources for filter does not have any pending PID init task or PID configure task
	//this avoids deleting the filter before a new filter is inserted at the same source, triggering
	//destruction of the chain
	//we don't check for any other pending connections at the session level
	for (i=0; i<filter->num_input_pids; i++) {
		GF_FilterPidInst *pidi = gf_list_get(filter->input_pids, i);
		if (pidi->pid->init_task_pending || pidi->pid->filter->out_pid_connection_pending) {
			has_pending=GF_TRUE;
			break;
		}
	}

	if (has_pending) {
		task->can_swap = GF_TRUE;
		task->requeue_request = GF_TRUE;
		gf_mx_v(filter->tasks_mx);
		return;
	}
	safe_int_dec(&filter->session->remove_tasks);

	Bool can_unload = GF_TRUE;
	//disconnect all output pids, this will remove all filters up the chain if no more inputs and outputs
	for (i=0; i<filter->num_output_pids; i++) {
		GF_FilterPid *pid = gf_list_get(filter->output_pids, i);
		gf_filter_pid_remove(pid);
		can_unload = GF_FALSE;
	}
	//locate source filter(s)
	for (i=0; i<filter->num_input_pids; i++) {
		GF_FilterPidInst *pidi = gf_list_get(filter->input_pids, i);
		can_unload = GF_FALSE;
		//fanout, only disconnect this pid instance
		if (pidi->pid->num_destinations>1) {
			//post STOP and disconnect
			GF_FilterEvent fevt;
			GF_FEVT_INIT(fevt, GF_FEVT_STOP, (GF_FilterPid *) pidi);
			gf_filter_pid_send_event((GF_FilterPid *) pidi, &fevt);

			gf_fs_post_disconnect_task(filter->session, filter, pidi->pid);
		}
		//this is a source for the chain
		else if (!pidi->pid->filter->num_input_pids) {
			gf_filter_remove_internal(pidi->pid->filter, NULL, GF_FALSE);
		}
		//otherwise walk down the chain if we have one-to-one
		else if (pidi->pid->filter->num_output_pids==1) {
			//PID will be removed, set discard right away to:
			//- release any shared packet on this PID
			//- trash any GF_PCK_CMD_PID_REM packet to decrement session->remove_tasks
			gf_filter_pid_set_discard((GF_FilterPid*)pidi, GF_TRUE);
			//set marked_for_removal to force filter_pid_remove() to post task and not use packet queue
			pidi->pid->filter->marked_for_removal = GF_TRUE;
			gf_filter_remove(pidi->pid->filter);
		} else {
			GF_FilterEvent fevt;
			//source filter still active, mark output pid as not connected, send a stop and post disconnect
			gf_assert(pidi->pid->num_destinations==1);
			pidi->pid->not_connected = 1;
			GF_FEVT_INIT(fevt, GF_FEVT_STOP, (GF_FilterPid *) pidi);
			fevt.play.initial_broadcast_play = 2;
			gf_filter_pid_send_event((GF_FilterPid *) pidi, &fevt);
			gf_fs_post_disconnect_task(filter->session, filter, pidi->pid);
		}
	}
	filter->sticky = 0;
	if (can_unload && !filter->removed && !filter->finalized) {
		gf_filter_post_remove(filter);
	}
	filter->removed = 1;
	gf_mx_v(filter->tasks_mx);
}

GF_EXPORT
void gf_filter_remove(GF_Filter *filter)
{
	if (!filter) return;
	safe_int_inc(&filter->session->remove_tasks);
	//always post a task for remove, this allows users to do remove() followed by add filter() without triggering stops
	gf_fs_post_task(filter->session, gf_filter_remove_local, filter, NULL, "filter_remove", NULL);
}

#if 0
GF_EXPORT
void gf_filter_remove_dst(GF_Filter *filter, GF_Filter *dst_filter)
{
	u32 i, j;
	u32 removed;
	if (!filter) return;
	removed = filter->removed;
	filter->removed = 1;
	gf_filter_remove_internal(dst_filter, filter, GF_FALSE);
	filter->removed = removed;

	for (i=0; i<filter->num_output_pids; i++) {
		GF_FilterPid *pid = gf_list_get(filter->output_pids, i);
		for (j=0; j<pid->num_destinations; j++) {
			GF_FilterPidInst *pidi = gf_list_get(pid->destinations, j);
			if (pidi->filter->removed) {
				gf_fs_post_disconnect_task(pidi->filter->session, pidi->filter, pidi->pid);
			}
		}
	}
}
#endif

Bool gf_filter_swap_source_register(GF_Filter *filter)
{
	u32 i;
	char *src_url=NULL;
	char *src_args=NULL;
	GF_Filter *target_filter=NULL;
	GF_Err e;
	const GF_FilterArgs *src_arg=NULL;

	gf_filter_reset_pending_packets(filter);

	while (gf_list_count(filter->output_pids)) {
		GF_FilterPid *pid = gf_list_pop_back(filter->output_pids);
		pid->destroyed = GF_TRUE;
		gf_fs_post_task(filter->session, gf_filter_pid_del_task, filter, pid, "pid_delete", NULL);
	}
	gf_mx_p(filter->tasks_mx);
	filter->num_output_pids = 0;
	gf_mx_v(filter->tasks_mx);

	if (filter->freg->finalize) {
		FSESS_CHECK_THREAD(filter)
		filter->freg->finalize(filter);
		filter->finalized = GF_TRUE;
	}
	gf_list_add(filter->blacklisted, (void *)filter->freg);

	i=0;
	while (filter->freg->args) {
		src_arg = &filter->freg->args[i];
		if (!src_arg || !src_arg->arg_name) {
			src_arg=NULL;
			break;
		}
		i++;
		if (strcmp(src_arg->arg_name, "src")) continue;
		//found it, get the url
		if (src_arg->offset_in_private<0) continue;

#ifdef WIN32
		src_url = *(char **)( ((char *)filter->filter_udta) + src_arg->offset_in_private);
		*(char **)(((char *)filter->filter_udta) + src_arg->offset_in_private) = NULL;
#else
		src_url = *(char **) (filter->filter_udta + src_arg->offset_in_private);
		*(char **)(filter->filter_udta + src_arg->offset_in_private) = NULL;
#endif
		 break;
	}
	reset_filter_args(filter);
	gf_free(filter->filter_udta);
	filter->filter_udta = NULL;
	if (!src_url) return GF_FALSE;
	GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Swaping source filter for URL %s\n", src_url));

	target_filter = filter->target_filter;
	filter->finalized = GF_FALSE;

	//reload using same args
	src_args = NULL;
	if (filter->src_args) {
		src_args = filter->src_args;
		filter->src_args = NULL;
	}
	else if (filter->orig_args) {
		src_args = filter->orig_args;
		filter->orig_args = NULL;
	}
	if (filter->orig_args) {
		gf_free(filter->orig_args);
		filter->orig_args = NULL;
	}

	gf_fs_load_source_dest_internal(filter->session, src_url, src_args, NULL, &e, filter, filter->target_filter ? filter->target_filter : filter->dst_filter, GF_TRUE, filter->no_dst_arg_inherit, NULL, NULL);
	if (src_args) gf_free(src_args);

	//we manage to reassign an input registry
	if (e==GF_OK) {
		gf_free(src_url);
		if (target_filter) filter->dst_filter = NULL;
		return GF_TRUE;
	}
	if (!filter->finalized) {
		gf_free(src_url);
		return gf_filter_swap_source_register(filter);
	}

	for (i=0; i<gf_list_count(filter->destination_links); i++) {
		GF_Filter *af = gf_list_get(filter->destination_links, i);
		gf_mx_p(af->tasks_mx);
		if (af->num_input_pids) {
			u32 j;
			for (j=0; j<af->num_input_pids; j++) {
				GF_FilterPidInst *pidi = gf_list_get(af->input_pids, j);
				pidi->is_end_of_stream = GF_TRUE;
			}
		}
		gf_mx_v(af->tasks_mx);
		if (af->sticky) {
			GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Failed to find any filter for URL %s\n", src_url));
		} else {
			GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Failed to find any filter for URL %s, disabling destination filter %s\n", src_url, af->name));
			af->removed = 1;
		}
	}
	if (e==GF_NOT_SUPPORTED)
		e = GF_FILTER_NOT_FOUND;
	//nope ...
	gf_filter_setup_failure(filter, e);
	gf_free(src_url);
	return GF_FALSE;
}

void gf_filter_forward_clock(GF_Filter *filter)
{
	u32 i;
	u64 clock_val;
	if (!filter->next_clock_dispatch_type) return;
	if (!filter->num_output_pids) return;

	for (i=0; i<filter->num_output_pids; i++) {
		GF_FilterPacket *pck;
		Bool req_props_map, info_modified;
		GF_FilterPid *pid = gf_list_get(filter->output_pids, i);
		GF_PropertyMap *map;

		//see \ref gf_filter_pid_merge_properties_internal for mutex
		gf_mx_p(pid->filter->tasks_mx);
		map = gf_list_last(pid->properties);
		gf_mx_v(pid->filter->tasks_mx);

		clock_val = filter->next_clock_dispatch;
		if (map->timescale != filter->next_clock_dispatch_timescale) {
			clock_val = gf_timestamp_rescale(clock_val, filter->next_clock_dispatch_timescale, map->timescale);
		}
		GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Filter %s PID %s internal forward of clock reference\n", pid->filter->name, pid->name));
		pck = gf_filter_pck_new_shared((GF_FilterPid *)pid, NULL, 0, NULL);
		if (!pck) {
			GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Filter %s PID %s failted to allocate packet for clock reference forward\n", pid->filter->name, pid->name));
			continue;
		}
		gf_filter_pck_set_cts(pck, clock_val);
		gf_filter_pck_set_clock_type(pck, filter->next_clock_dispatch_type);

		//do not let the clock packet carry the props/info change flags since it is an internal
		//packet discarded before processing these flags
		req_props_map = pid->request_property_map;
		pid->request_property_map = GF_TRUE;
		info_modified = pid->pid_info_changed;
		pid->pid_info_changed = GF_FALSE;

		gf_filter_pck_send(pck);
		pid->request_property_map = req_props_map;
		pid->pid_info_changed = info_modified;
	}
	filter->next_clock_dispatch_type = 0;
}

GF_EXPORT
Bool gf_filter_is_supported_source(GF_Filter *filter, const char *url, const char *parent_url)
{
	GF_Err e;
	Bool is_supported = GF_FALSE;
	gf_fs_load_source_dest_internal(filter->session, url, NULL, parent_url, &e, NULL, filter, GF_TRUE, GF_TRUE, &is_supported, NULL);
	return is_supported;
}

GF_EXPORT
Bool gf_filter_url_is_filter(GF_Filter *filter, const char *url, Bool *act_as_source)
{
	char *sep = strchr(url, filter->session->sep_args);
	u32 len = (u32) ( sep ? (sep - url - 1) : strlen(url) );
	u32 i, count = gf_list_count(filter->session->registry);
	for (i=0; i<count; i++) {
		const GF_FilterRegister *freg = gf_list_get(filter->session->registry, i);
		if (!freg) continue;
		u32 flen = (u32) strlen(freg->name);
		if ((len!=flen) || strncmp(freg->name, url, len)) continue;

		if (act_as_source) {
			if (freg->flags & GF_FS_REG_ACT_AS_SOURCE)
				*act_as_source = GF_TRUE;
			i=0;
			while (freg->args && freg->args[i].arg_name) {
				if (!strcmp(freg->args[i].arg_name, "src")) {
					*act_as_source = GF_TRUE;
					break;
				}
				i++;
			}
		}
		return GF_TRUE;
	}
	return GF_FALSE;
}

GF_EXPORT
GF_Filter *gf_filter_connect_source_internal(GF_Filter *filter, const char *url, const char *parent_url, Bool inherit_args, Bool is_src_add, GF_Err *err)
{
	GF_Filter *filter_src;
	GF_Filter *src_orig=NULL;
	const char *args = NULL;
	char *full_args = NULL;
	if (!filter) {
		if (err) *err = GF_BAD_PARAM;
		return NULL;
	}
	if (is_src_add) {
		GF_FilterPidInst *pidi = gf_list_get(filter->input_pids, 0);
		src_orig = (pidi && pidi->pid) ? pidi->pid->filter : NULL;
		while (src_orig && src_orig->num_input_pids) {
			src_orig = pidi->pid->filter;
			pidi = gf_list_get(src_orig->input_pids, 0);
		}
		if (!src_orig) {
			if (err) *err = GF_BAD_PARAM;
			return NULL;
		}
		args = inherit_args ? gf_filter_get_args_stripped(filter->session, src_orig->orig_args, GF_FALSE) : NULL;
	} else {
		args = inherit_args ? gf_filter_get_dst_args(filter) : NULL;
	}

	if (args) {
		char *rem_opts[] = {"FID", "SID", "N", "RSID", "clone", "DL", NULL};
		char szSep[10];
		char *loc_args;
		u32 opt_idx;
		u32 dst_offset = 0;
		u32 len = (u32) strlen(args);
		sprintf(szSep, "%cgfloc%c", filter->session->sep_args, filter->session->sep_args);
		loc_args = strstr(args, szSep);
		if (loc_args) {
			len = (u32) (ptrdiff_t) (loc_args - args);
		}
		if (len) {
			gf_dynstrcat(&full_args, url, NULL);
			sprintf(szSep, "%cgpac%c", filter->session->sep_args, filter->session->sep_args);
			if ((filter->session->sep_args==':') && strstr(url, "://") && !strstr(url, szSep)) {
				gf_dynstrcat(&full_args, szSep, NULL);
			} else {
				sprintf(szSep, "%c", filter->session->sep_args);
				gf_dynstrcat(&full_args, szSep, NULL);
			}
			if (full_args)
				dst_offset = (u32) strlen(full_args);

			gf_dynstrcat(&full_args, args, NULL);
			sprintf(szSep, "%cgfloc%c", filter->session->sep_args, filter->session->sep_args);
			loc_args = strstr(full_args, "gfloc");
			if (loc_args) loc_args[0] = 0;

			//remove all internal options FIS, SID, N
			opt_idx = 0;
			while (rem_opts[opt_idx]) {
				sprintf(szSep, "%c%s%c", filter->session->sep_args, rem_opts[opt_idx], filter->session->sep_name);
				loc_args = strstr(full_args + dst_offset, szSep);
				if (loc_args) {
					char *sep = strchr(loc_args+1, filter->session->sep_args);
					if (sep) memmove(loc_args, sep, strlen(sep)+1);
					else loc_args[0] = 0;
				}
				opt_idx++;
			}
			url = full_args;
		}
	}

	if (gf_filter_url_is_filter(filter, url, NULL)) {
		filter_src = gf_fs_load_filter(filter->session, url, err);
	} else {
		filter_src = gf_fs_load_source_dest_internal(filter->session, url, NULL, parent_url, err, NULL, is_src_add ? NULL : filter, GF_TRUE, is_src_add ? GF_FALSE : GF_TRUE, NULL, NULL);
	}
	if (full_args) gf_free(full_args);

	if (!filter_src) return NULL;

	if (src_orig) {
		gf_filter_set_id(filter_src, src_orig->id);
		gf_filter_set_name(filter_src, src_orig->name);
		filter_src->require_source_id = src_orig->require_source_id;
		filter_src->subsource_id = src_orig->subsource_id;
		filter_src->subsession_id = src_orig->subsession_id;
		return filter_src;
	}

	gf_mx_p(filter->tasks_mx);
	if (!filter->source_filters)
		filter->source_filters = gf_list_new();
	gf_list_add(filter->source_filters, filter_src);
	gf_mx_v(filter->tasks_mx);
	return filter_src;
}

GF_EXPORT
GF_Filter *gf_filter_connect_source(GF_Filter *filter, const char *url, const char *parent_url, Bool inherit_args, GF_Err *err)
{
	return gf_filter_connect_source_internal(filter, url, parent_url, inherit_args, GF_FALSE, err);
}
GF_EXPORT
GF_Filter *gf_filter_add_source(GF_Filter *filter, const char *url, const char *parent_url, Bool inherit_args, GF_Err *err)
{
	return gf_filter_connect_source_internal(filter, url, parent_url, inherit_args, GF_TRUE, err);
}

GF_EXPORT
GF_Filter *gf_filter_connect_destination(GF_Filter *filter, const char *url, GF_Err *err)
{
	if (!filter) return NULL;
	return gf_fs_load_source_dest_internal(filter->session, url, NULL, NULL, err, NULL, filter, GF_FALSE, GF_FALSE, NULL, NULL);
}


GF_EXPORT
void gf_filter_get_output_buffer_max(GF_Filter *filter, u32 *max_buf, u32 *max_playout_buf)
{
	u32 i;
	u32 buf_max = 0;
	u32 buf_play_max = 0;
	for (i=0; i<filter->num_output_pids; i++) {
		u32 j;
		GF_FilterPid *pid = gf_list_get(filter->output_pids, i);
		if (buf_max < pid->user_max_buffer_time) buf_max = (u32) pid->user_max_buffer_time;
		if (buf_max < pid->max_buffer_time) buf_max = (u32) pid->max_buffer_time;

		if (buf_play_max < pid->user_max_playout_time) buf_play_max = pid->user_max_playout_time;
		if (buf_play_max < pid->max_buffer_time) buf_play_max = (u32) pid->max_buffer_time;

		for (j=0; j<pid->num_destinations; j++) {
			u32 mb, pb;
			GF_FilterPidInst *pidi = gf_list_get(pid->destinations, j);
			gf_filter_get_output_buffer_max(pidi->filter, &mb, &pb);
			if (buf_max < mb) buf_max = mb;
			if (buf_play_max < pb) buf_play_max = pb;
		}
	}
	if (max_buf) *max_buf = buf_max;
	if (max_playout_buf) *max_playout_buf = buf_play_max;
	return;
}

GF_EXPORT
void gf_filter_make_sticky(GF_Filter *filter)
{
	if (filter) filter->sticky = 1;
}

static u32 gf_filter_get_num_events_queued_internal(GF_Filter *filter)
{
	u32 i;
	u32 nb_events = 0;
	if (!filter) return 0;
	nb_events = filter->num_events_queued;

	for (i=0; i<filter->num_output_pids; i++) {
		u32 k;
		GF_FilterPid *pid = gf_list_get(filter->output_pids, i);
		for (k=0; k<pid->num_destinations; k++) {
			GF_FilterPidInst *pidi = gf_list_get(pid->destinations, k);
			nb_events += gf_filter_get_num_events_queued(pidi->filter);
		}
	}
	return nb_events;
}
GF_EXPORT
u32 gf_filter_get_num_events_queued(GF_Filter *filter)
{
	if (!filter) return 0;
#ifndef GPAC_DISABLE_THREADS
	GF_FilterSession *fsess = filter->session;
	gf_mx_p(fsess->filters_mx);
	u32 res = gf_filter_get_num_events_queued_internal(filter);
	gf_mx_v(fsess->filters_mx);
	return res;
#else
	return gf_filter_get_num_events_queued_internal(filter);
#endif
}

GF_EXPORT
void gf_filter_hint_single_clock(GF_Filter *filter, u64 time_in_us, GF_Fraction64 media_timestamp)
{
	//for now only one clock hint possible ...
	filter->session->hint_clock_us = time_in_us;
	filter->session->hint_timestamp = media_timestamp;
}

GF_EXPORT
void gf_filter_get_clock_hint(GF_Filter *filter, u64 *time_in_us, GF_Fraction64 *media_timestamp)
{
	//for now only one clock hint possible ...
	if (time_in_us) *time_in_us = filter->session->hint_clock_us;
	if (media_timestamp) *media_timestamp = filter->session->hint_timestamp;
}

GF_EXPORT
GF_Err gf_filter_assign_id(GF_Filter *filter, const char *id)
{
	if (!filter || filter->id) return GF_BAD_PARAM;

	if (!id) {
		char szID[1024];
		sprintf(szID, "_%p_", filter);
		filter->id = gf_strdup(szID);
	} else {
		filter->id = gf_strdup(id);
	}
	return GF_OK;
}

GF_EXPORT
const char *gf_filter_get_id(GF_Filter *filter)
{
	if (filter) return filter->id;
	return NULL;
}

GF_EXPORT
GF_Err gf_filter_set_source(GF_Filter *filter, GF_Filter *link_from, const char *link_ext)
{
	if (!filter || !link_from) return GF_BAD_PARAM;
	if (filter == link_from) return GF_OK;

	//don't allow loops
	if (gf_filter_in_parent_chain(filter, link_from)) return GF_BAD_PARAM;

	if (!link_from->id) {
		gf_filter_assign_id(link_from, NULL);
	}

	if (link_ext) {
		char szSep[2];
		char *id = NULL;
		gf_dynstrcat(&id, link_from->id, NULL);
		szSep[0] = link_from->session->sep_frag;
		szSep[1] = 0;
		gf_dynstrcat(&id, link_ext, szSep);
		gf_filter_set_sources(filter, id);
		gf_free(id);
	} else {
		gf_filter_set_sources(filter, link_from->id);
	}

	if (link_from->target_filter != filter) {
		filter->target_filter = link_from->target_filter;
		link_from->target_filter = NULL;
	}
	return GF_OK;
}

GF_EXPORT
GF_Err gf_filter_set_source_restricted(GF_Filter *filter, GF_Filter *link_from, const char *link_ext)
{
	GF_Err e =  gf_filter_set_source(filter, link_from, link_ext);
	if (e) return e;
	if (link_from->restricted_source_id)
		gf_free(link_from->restricted_source_id);

	link_from->restricted_source_id = gf_strdup(link_from->id);
	return GF_OK;
}


GF_EXPORT
GF_Err gf_filter_override_caps(GF_Filter *filter, const GF_FilterCapability *caps, u32 nb_caps )
{
	if (!filter) return GF_BAD_PARAM;
	//we accept caps override event on a running filter, this will only impact the next link solving
	filter->forced_caps = nb_caps ? caps : NULL;
	filter->nb_forced_caps = nb_caps;
	filter->nb_forced_bundles = nb_caps ? gf_filter_caps_bundle_count(caps, nb_caps) : 0;

	filter->bundle_idx_at_resolution = -1;
	return GF_OK;
}

GF_EXPORT
GF_Err gf_filter_act_as_sink(GF_Filter *filter)
{
	if (!filter) return GF_BAD_PARAM;
	filter->act_as_sink = GF_TRUE;
	return GF_OK;
}


GF_EXPORT
void gf_filter_pid_init_play_event(GF_FilterPid *pid, GF_FilterEvent *evt, Double start, Double speed, const char *log_name)
{
	u32 pmode = GF_PLAYBACK_MODE_NONE;
	const GF_PropertyValue *p;
	Bool was_end = GF_FALSE;
	memset(evt, 0, sizeof(GF_FilterEvent));
	evt->base.type = GF_FEVT_PLAY;
	evt->base.on_pid = pid;

	evt->play.speed = 1.0;

	if ((speed<0) && !start) start = -1.0;

	p = gf_filter_pid_get_property_first(pid, GF_PROP_PID_PLAYBACK_MODE);
	if (p) pmode = p->value.uint;

	evt->play.start_range = start;
	if (start<0) {
		was_end = GF_TRUE;
		p = gf_filter_pid_get_property_first(pid, GF_PROP_PID_DURATION);
		if (p && p->value.lfrac.den) {
			evt->play.start_range *= -100;
			evt->play.start_range *= (p->value.lfrac.num<0) ? -p->value.lfrac.num : p->value.lfrac.num;
			evt->play.start_range /= 100 * p->value.lfrac.den;
		}
	}
	switch (pmode) {
	case GF_PLAYBACK_MODE_NONE:
		evt->play.start_range = 0;
		if (start) {
			GF_LOG(GF_LOG_WARNING, GF_LOG_FILTER, ("[%s] Media PID does not support seek, ignoring start directive\n", log_name));
		}
		break;
	case GF_PLAYBACK_MODE_SEEK:
		if (speed != 1.0) {
			GF_LOG(GF_LOG_WARNING, GF_LOG_FILTER, ("[%s] Media PID does not support speed, ignoring speed directive\n", log_name));
		}
		break;
	case GF_PLAYBACK_MODE_FASTFORWARD:
		if (speed<0) {
			GF_LOG(GF_LOG_WARNING, GF_LOG_FILTER, ("[%s] Media PID does not support negative speed, ignoring speed directive\n", log_name));
			if (was_end) evt->play.start_range = 0;
		} else {
			evt->play.speed = speed;
		}
		break;
	default:
		evt->play.speed = speed;
		break;
	}
}

GF_EXPORT
void gf_filter_set_max_extra_input_pids(GF_Filter *filter, u32 max_extra_pids)
{
	if (filter) filter->max_extra_pids = max_extra_pids;
}
GF_EXPORT
u32 gf_filter_get_max_extra_input_pids(GF_Filter *filter)
{
	if (filter) return filter->max_extra_pids;
	return 0;
}
GF_EXPORT
Bool gf_filter_block_enabled(GF_Filter *filter)
{
	if (!filter) return GF_FALSE;
	return (filter->session->blocking_mode==GF_FS_NOBLOCK) ? GF_FALSE : GF_TRUE;
}

static void filter_guess_file_ext(GF_FilterSession *sess, GF_FilterPid *pid, const char *for_mime)
{
	u32 i, j, count = gf_list_count(sess->registry);
	for (i=0; i<count; i++) {
		const GF_FilterRegister *reg = gf_list_get(sess->registry, i);
		const char *mime=NULL;
		const char *ext=NULL;

		for (j=0; j<reg->nb_caps; j++) {
			char szExt[20];
			const GF_FilterCapability *cap = &reg->caps[j];
			if ((cap->val.type != GF_PROP_NAME)
				&& (cap->val.type != GF_PROP_STRING)
				&& (cap->val.type != GF_PROP_STRING_NO_COPY)
			) {
				continue;
			}
			if (reg->caps[j].code == GF_PROP_PID_FILE_EXT) ext = cap->val.value.string;
			else if (reg->caps[j].code == GF_PROP_PID_MIME) mime = cap->val.value.string;
			else continue;

			if (!mime || !ext) continue;
			if (!strstr(mime, for_mime)) continue;
			char *sep = strchr(ext, '|');
			u32 len = (u32) strlen(ext);
			if (sep) len = (u32) (sep - ext);
			if (len>19) len=19;
			strncpy(szExt, ext, len);
			szExt[len] = 0;
			gf_filter_pid_set_property(pid, GF_PROP_PID_FILE_EXT, &PROP_STRING(szExt));
			return;
		}
	}
}

GF_EXPORT
GF_Err gf_filter_pid_raw_new(GF_Filter *filter, const char *url, const char *local_file, const char *mime_type, const char *fext, const u8 *probe_data, u32 probe_size, Bool trust_mime, GF_FilterPid **out_pid)
{
	char tmp_ext[50];
	u32 ext_len=0;
	Bool ext_not_trusted, is_new_pid = GF_FALSE;
	GF_FilterPid *pid = *out_pid;
	if (!pid) {
		pid = gf_filter_pid_new(filter);
		if (!pid) {
			return GF_OUT_OF_MEM;
		}
		pid->max_buffer_unit = 1;
		is_new_pid = GF_TRUE;
		*out_pid = pid;
	}

	gf_filter_pid_set_property(pid, GF_PROP_PID_STREAM_TYPE, &PROP_UINT(GF_STREAM_FILE) );

	gf_filter_pid_set_property(pid, GF_PROP_PID_FILEPATH, local_file ? &PROP_STRING(local_file) : NULL);


	if (url) {
		//force reconfigure
		gf_filter_pid_set_property(pid, GF_PROP_PID_URL, NULL);
		gf_filter_pid_set_property(pid, GF_PROP_PID_URL, &PROP_STRING(url));

		if (!strnicmp(url, "isobmff://", 10)) {
			gf_filter_pid_set_name(pid, "isobmff://");
			fext = "mp4";
			mime_type = "video/mp4";
			if (is_new_pid)
				gf_filter_pid_set_eos(pid);
		} else {
			char *sep = gf_file_basename(url);

			//for fileIO, fetch the underlying resource name, this avoids having memory address in pid name for inspect
			if (!strncmp(url, "gfio://", 7)) {
				const char *res_url = gf_fileio_translate_url(url);
				if (res_url) sep = gf_file_basename(res_url);
				else sep="Unknown_URL";
			}
			gf_filter_pid_set_name(pid, sep);
		}

		if (fext) {
			strncpy(tmp_ext, fext, 20);
			tmp_ext[20] = 0;
			strlwr(tmp_ext);
			gf_filter_pid_set_property(pid, GF_PROP_PID_FILE_EXT, &PROP_STRING(tmp_ext));
			ext_len = (u32) strlen(tmp_ext);
		} else {
			char *ext=NULL;
			char *scheme = strncmp(url, "gfio://", 7) ? strstr(url, "://") : NULL;
			if (scheme) {
				scheme = strchr(scheme+3, '/');
				if (scheme)
					ext = gf_file_ext_start(scheme);
			} else {
				ext = gf_file_ext_start(url);
			}
			if (ext) ext++;

			if (ext) {
				char *s = strchr(ext, '#');
				if (s) s[0] = 0;

				strncpy(tmp_ext, ext, 20);
				tmp_ext[20] = 0;
				strlwr(tmp_ext);
				gf_filter_pid_set_property(pid, GF_PROP_PID_FILE_EXT, &PROP_STRING(tmp_ext));
				ext_len = (u32) strlen(tmp_ext);
				if (s) s[0] = '#';
			}
		}
	}

	ext_not_trusted = GF_FALSE;
	//probe data
	if ((!mime_type || !trust_mime) && !filter->no_probe && is_new_pid && probe_data && probe_size && !(filter->session->flags & GF_FS_FLAG_NO_PROBE)) {
		u32 i, count;
		GF_FilterProbeScore score, max_score = GF_FPROBE_NOT_SUPPORTED;
		const char *probe_mime = NULL;
		gf_mx_p(filter->session->filters_mx);
		count = gf_list_count(filter->session->registry);
		for (i=0; i<count; i++) {
			const char *a_mime;
			const GF_FilterRegister *freg = gf_list_get(filter->session->registry, i);
			if (!freg || !freg->probe_data) continue;
			score = GF_FPROBE_NOT_SUPPORTED;
			a_mime = freg->probe_data(probe_data, probe_size, &score);
			if (score==GF_FPROBE_NOT_SUPPORTED) {
				u32 k;
				for (k=0;k<freg->nb_caps && !ext_not_trusted && ext_len; k++) {
					const char *value;
					const GF_FilterCapability *cap = &freg->caps[k];
					if (cap->flags & GF_CAPFLAG_RECONFIG) break;
					if (!(cap->flags & GF_CAPFLAG_IN_BUNDLE)) continue;
					if (!(cap->flags & GF_CAPFLAG_INPUT)) continue;
					if (cap->code != GF_PROP_PID_FILE_EXT) continue;
					value = cap->val.value.string;
					while (value && ext_len) {
						const char *match = strstr(value, tmp_ext);
						if (!match) break;
						if (!match[ext_len] || (match[ext_len]=='|')) {
							ext_not_trusted = GF_TRUE;
							break;
						}
						value = match+ext_len;
					}
				}
			} else if (score==GF_FPROBE_EXT_MATCH) {
				if (a_mime && ext_len) {
					char *has_ext = strstr(a_mime, tmp_ext);
					if (has_ext && (has_ext>a_mime) && (has_ext[-1] != '|')) has_ext = NULL;
					if (has_ext && (has_ext[ext_len]!=',') && (has_ext[ext_len]!='|')) has_ext = NULL;
					if (has_ext) {
						ext_not_trusted = GF_FALSE;
						probe_mime = NULL;
						break;
					}
				}
			} else {
				if (a_mime) {
					GF_LOG(GF_LOG_INFO, GF_LOG_FILTER, ("Data Prober (filter %s) detected format is%s mime %s\n", freg->name, (score==GF_FPROBE_SUPPORTED) ? "" :  " maybe", a_mime));
				}
				if (a_mime && (score > max_score)) {
					probe_mime = a_mime;
					max_score = score;
				}
			}
		}
		gf_mx_v(filter->session->filters_mx);

		pid->ext_not_trusted = ext_not_trusted;

		if (probe_mime) {
			mime_type = probe_mime;
		}
	}
	//we ignore mime type on pid reconfigure - some server will overwrite valid type to application/octet-string or binary/octet-string
	//on a byte-range request or secondary fetch
	//this is safe for now as the only reconfig we do is in HTTP streaming context
	//we furthermore blacklist *octet-* mimes
	if (mime_type && is_new_pid && !strstr(mime_type, "/octet-")) {
		strncpy(tmp_ext, mime_type, 50);
		tmp_ext[49] = 0;
		//keep case for mime type
		gf_filter_pid_set_property(pid, GF_PROP_PID_MIME, &PROP_STRING( tmp_ext ));
		//we have a mime, disable extension checking
		pid->ext_not_trusted = GF_TRUE;
		if (!ext_len && !fext) {
			filter_guess_file_ext(filter->session, pid, mime_type);
		}
	}

	return GF_OK;
}

GF_EXPORT
const char *gf_filter_probe_data(GF_Filter *filter, u8 *data, u32 size, GF_FilterProbeScore *pscore)
{
	u32 i, count;
	GF_FilterProbeScore score, max_score = GF_FPROBE_NOT_SUPPORTED;
	const char *probe_mime = NULL;
	if (pscore) *pscore = GF_FPROBE_NOT_SUPPORTED;
	if (!size) return NULL;
	gf_mx_p(filter->session->filters_mx);
	count = gf_list_count(filter->session->registry);
	for (i=0; i<count; i++) {
		const char *a_mime;
		const GF_FilterRegister *freg = gf_list_get(filter->session->registry, i);
		if (!freg || !freg->probe_data) continue;
		score = GF_FPROBE_NOT_SUPPORTED;
		a_mime = freg->probe_data(data, size, &score);
		if (score==GF_FPROBE_NOT_SUPPORTED) {
		} else if (score==GF_FPROBE_EXT_MATCH) {
//			probe_mime = NULL;
		} else {
			if (a_mime && (score > max_score)) {
				probe_mime = a_mime;
				max_score = score;
			}
		}
	}
	gf_mx_v(filter->session->filters_mx);
	if (pscore) *pscore = max_score;
	return probe_mime;
}

static Bool gf_filter_get_arg_internal(GF_Filter *filter, const char *arg_name, GF_PropertyValue *prop, const char **min_max_enum)
{
	u32 i=0;
	if (!filter || !arg_name) return GF_FALSE;

	while (1) {
		GF_PropertyValue p;
		const GF_FilterArgs *arg = &filter->freg->args[i];
		if (!arg || !arg->arg_name) break;
		i++;

		if (strcmp(arg->arg_name, arg_name)) continue;
		if (arg->offset_in_private < 0) continue;

		p.type = arg->arg_type;
		switch (arg->arg_type) {
		case GF_PROP_BOOL:
			p.value.boolean = * (Bool *) ((char *)filter->filter_udta + arg->offset_in_private);
			break;
		case GF_PROP_UINT:
		case GF_PROP_4CC:
			p.value.uint = * (u32 *) ((char *)filter->filter_udta + arg->offset_in_private);
			break;
		case GF_PROP_SINT:
			p.value.sint = * (s32 *) ((char *)filter->filter_udta + arg->offset_in_private);
			break;
		case GF_PROP_LUINT:
			p.value.longuint = * (u64 *) ((char *)filter->filter_udta + arg->offset_in_private);
			break;
		case GF_PROP_LSINT:
			p.value.longsint = * (s64 *) ((char *)filter->filter_udta + arg->offset_in_private);
			break;
		case GF_PROP_FLOAT:
			p.value.fnumber = * (Fixed *) ((char *)filter->filter_udta + arg->offset_in_private);
			break;
		case GF_PROP_DOUBLE:
			p.value.number = * (Double *) ((char *)filter->filter_udta + arg->offset_in_private);
			break;
		case GF_PROP_VEC2I:
			p.value.vec2i = * (GF_PropVec2i *) ((char *)filter->filter_udta + arg->offset_in_private);
			break;
		case GF_PROP_VEC2:
			p.value.vec2 = * (GF_PropVec2 *) ((char *)filter->filter_udta + arg->offset_in_private);
			break;
		case GF_PROP_VEC3I:
			p.value.vec3i = * (GF_PropVec3i *) ((char *)filter->filter_udta + arg->offset_in_private);
			break;
		case GF_PROP_VEC4I:
			p.value.vec4i = * (GF_PropVec4i *) ((char *)filter->filter_udta + arg->offset_in_private);
			break;
		case GF_PROP_FRACTION:
			p.value.frac = * (GF_Fraction *) ((char *)filter->filter_udta + arg->offset_in_private);
			break;
		case GF_PROP_FRACTION64:
			p.value.lfrac = * (GF_Fraction64 *) ((char *)filter->filter_udta + arg->offset_in_private);
			break;
		case GF_PROP_DATA:
		case GF_PROP_DATA_NO_COPY:
		case GF_PROP_CONST_DATA:
			p.value.data = * (GF_PropData *) ((char *)filter->filter_udta + arg->offset_in_private);
			break;
		case GF_PROP_POINTER:
			p.value.ptr = * (void **) ((char *)filter->filter_udta + arg->offset_in_private);
			break;
		case GF_PROP_STRING_NO_COPY:
		case GF_PROP_STRING:
		case GF_PROP_NAME:
			p.value.ptr = * (char **) ((char *)filter->filter_udta + arg->offset_in_private);
			break;
		//use uint_list as base type for lists
		case GF_PROP_STRING_LIST:
		case GF_PROP_UINT_LIST:
		case GF_PROP_4CC_LIST:
		case GF_PROP_SINT_LIST:
		case GF_PROP_VEC2I_LIST:
			p.value.uint_list = * (GF_PropUIntList *) ((char *)filter->filter_udta + arg->offset_in_private);
			break;
		default:
			if (gf_props_type_is_enum(arg->arg_type)) {
				p.value.uint = * (u32 *) ((char *)filter->filter_udta + arg->offset_in_private);
				break;
			}
			return GF_FALSE;
		}
		if (min_max_enum) *min_max_enum = arg->min_max_enum;
		*prop = p;
		return GF_TRUE;
	}
	return GF_FALSE;
}

GF_EXPORT
const char *gf_filter_get_arg_str(GF_Filter *filter, const char *arg_name, char dump[GF_PROP_DUMP_ARG_SIZE])
{
	GF_PropertyValue p;
	const char *arg_min_max = NULL;
	if (!dump || !gf_filter_get_arg_internal(filter, arg_name, &p, &arg_min_max))
		return NULL;
	return gf_props_dump_val(&p, dump, GF_PROP_DUMP_DATA_NONE, arg_min_max);
}

GF_EXPORT
Bool gf_filter_get_arg(GF_Filter *filter, const char *arg_name, GF_PropertyValue *prop)
{
	const char *arg_min_max = NULL;
	if (!prop) return GF_FALSE;
	return gf_filter_get_arg_internal(filter, arg_name, prop, &arg_min_max);
}

GF_EXPORT
Bool gf_filter_is_supported_mime(GF_Filter *filter, const char *mime)
{
	return gf_fs_is_supported_mime(filter->session, mime);
}

GF_EXPORT
Bool gf_filter_ui_event(GF_Filter *filter, GF_Event *uievt)
{
	return gf_fs_ui_event(filter->session, uievt);
}

GF_EXPORT
Bool gf_filter_all_sinks_done(GF_Filter *filter)
{
	u32 i, count;
	Bool res = GF_TRUE;
	if (!filter || filter->session->in_final_flush || (filter->session->run_status==GF_EOS)	)
		return GF_TRUE;

	gf_mx_p(filter->session->filters_mx);
	count = gf_list_count(filter->session->filters);
	for (i=0; i<count; i++) {
		u32 j;
		GF_Filter *f = gf_list_get(filter->session->filters, i);
		if (!f || f->num_output_pids) continue;
		gf_mx_v(filter->session->filters_mx);
		gf_mx_p(f->tasks_mx);
		for (j=0; j<f->num_input_pids; j++) {
			GF_FilterPidInst *pidi = gf_list_get(f->input_pids, j);
			if (pidi->pid->is_playing && !pidi->is_end_of_stream) {
				res = GF_FALSE;
				gf_mx_v(f->tasks_mx);
				return res;
			}
		}
		gf_mx_v(f->tasks_mx);
		gf_mx_p(filter->session->filters_mx);
	}

	gf_mx_v(filter->session->filters_mx);
	return res;
}


GF_EXPORT
void gf_filter_register_opengl_provider(GF_Filter *filter, Bool do_register)
{
#ifndef GPAC_DISABLE_3D
	GF_Err e;
	if (filter->removed || filter->finalized) return;
	if (filter->session->ext_gl_callback) return;

	if (do_register) {
		if (gf_list_find(filter->session->gl_providers, filter)<0)
			gf_list_add(filter->session->gl_providers, filter);
		return;
	}
	gf_list_del_item(filter->session->gl_providers, filter);
	e = gf_fs_check_gl_provider(filter->session);
	if (e && filter->session->nb_gl_filters) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Failed to reload an OpenGL provider and some filters require OpenGL, aborting\n"));
		gf_fs_abort(filter->session, GF_FS_FLUSH_NONE);
	}
#endif

}

GF_EXPORT
GF_Err gf_filter_request_opengl(GF_Filter *filter)
{
#ifndef GPAC_DISABLE_3D
	GF_Err e;
	if (filter->finalized || filter->removed) return GF_OK;

	filter->session->nb_gl_filters++;
	e = gf_fs_check_gl_provider(filter->session);
	if (e) {
		filter->session->nb_gl_filters--;
		return e;
	}
	if (!(filter->freg->flags & GF_FS_REG_CONFIGURE_MAIN_THREAD))
		safe_int_inc(&filter->nb_main_thread_forced);
	return GF_OK;
#else
	return GF_NOT_SUPPORTED;
#endif
}

GF_EXPORT
GF_Err gf_filter_set_active_opengl_context(GF_Filter *filter, Bool do_activate)
{
#ifndef GPAC_DISABLE_3D
	if (filter->finalized || filter->removed) return GF_OK;
	return gf_fs_set_gl(filter->session, do_activate);
#else
	return GF_NOT_SUPPORTED;
#endif
}


GF_EXPORT
u32 gf_filter_count_source_by_protocol(GF_Filter *filter, const char *protocol_scheme, Bool expand_proto, GF_FilterPid * (*enum_pids)(void *udta, u32 *idx), void *udta)
{
	u32 i, count, len, res=0;
	if (!filter || !protocol_scheme) return 0;
	gf_mx_p(filter->session->filters_mx);
	len = (u32) strlen(protocol_scheme);
	count = gf_list_count(filter->session->filters);
	for (i=0; i<count; i++) {
		const char *args;
		GF_Filter *src = gf_list_get(filter->session->filters, i);
		//check only sinks
		if (!src || src->freg->configure_pid) continue;
		args = src->src_args;
		if (!args) args = src->orig_args;
		if (!args || (strlen(args)<5) ) continue;

		if (strncmp(args + 4, protocol_scheme, len)) continue;
		if (!expand_proto && (args[len] != ':')) continue;

		//release session mutex as gf_filter_in_parent_chain may block on filter if it is waiting for the session mutex
		gf_mx_v(filter->session->filters_mx);
		if (! gf_filter_in_parent_chain(filter, src)) {
			u32 j=0;
			Bool found=GF_FALSE;
			if (!enum_pids) {
				gf_mx_p(filter->session->filters_mx);
				continue;
			}
			while (1) {
				GF_FilterPid *pid = enum_pids(udta, &j);
				if (!pid) break;
				j++;
				if ( gf_filter_in_parent_chain(pid->pid->filter, src)) {
					found=GF_TRUE;
					break;
				}
			}
			if (!found) {
				gf_mx_p(filter->session->filters_mx);
				continue;
			}
		}

		res++;
		gf_mx_p(filter->session->filters_mx);
	}

	gf_mx_v(filter->session->filters_mx);
	return res;
}

GF_EXPORT
void gf_filter_disable_probe(GF_Filter *filter)
{
	if (filter)
	 	filter->no_probe = GF_TRUE;
}

GF_EXPORT
void gf_filter_disable_inputs(GF_Filter *filter)
{
	if (filter)
	 	filter->no_inputs = GF_TRUE;
}

static Bool gf_filter_has_pid_connection_pending_internal(GF_Filter *filter, GF_Filter *stop_at_filter)
{
	u32 i, j;
	if (filter == stop_at_filter) return GF_FALSE;

	if (filter->has_pending_pids) return GF_TRUE;
	if (filter->in_pid_connection_pending) return GF_TRUE;
	if (filter->out_pid_connection_pending) return GF_TRUE;

	if (!filter->num_output_pids) {
		if (!filter->act_as_sink && !filter->multi_sink_target) {
			if (filter->forced_caps) {
				if (gf_filter_has_out_caps(filter->forced_caps, filter->nb_forced_caps))
					return GF_TRUE;
			} else {
				if (gf_filter_has_out_caps(filter->freg->caps, filter->freg->nb_caps))
					return GF_TRUE;
			}
		}
		return GF_FALSE;
	}

	for (i=0; i<filter->num_output_pids; i++) {
		GF_FilterPid *pid = gf_list_get(filter->output_pids, i);
		if (pid->init_task_pending) return GF_TRUE;
		for (j=0; j<pid->num_destinations; j++) {
			GF_FilterPidInst *pidi = gf_list_get(pid->destinations, j);
			Bool res = gf_filter_has_pid_connection_pending_internal(pidi->filter, stop_at_filter);
			if (res) return GF_TRUE;
		}
	}
	return GF_FALSE;
}

GF_EXPORT
Bool gf_filter_has_pid_connection_pending(GF_Filter *filter, GF_Filter *stop_at_filter)
{
	if (!filter) return GF_FALSE;
#ifndef GPAC_DISABLE_THREADS
	//lock session, this is an unsafe call
	GF_FilterSession *fsess = filter->session;
	gf_mx_p(fsess->filters_mx);
	Bool res = gf_filter_has_pid_connection_pending_internal(filter, stop_at_filter);
	gf_mx_v(fsess->filters_mx);
	return res;
#else
	return gf_filter_has_pid_connection_pending_internal(filter, stop_at_filter);
#endif
}

GF_EXPORT
Bool gf_filter_reporting_enabled(GF_Filter *filter)
{
	if (filter) return filter->session->reporting_on;
	return GF_FALSE;
}

GF_EXPORT
GF_Err gf_filter_update_status(GF_Filter *filter, u32 percent, char *szStatus)
{
	GF_Event evt;
	u32 len;
	if (!filter) return GF_BAD_PARAM;
	if (!filter->session->reporting_on)
		return GF_OK;

	if (!szStatus) {
		if (filter->status_str) filter->status_str[0] = 0;
		return GF_OK;
	}
	len = (u32) strlen(szStatus);
	if (len>=filter->status_str_alloc) {
		filter->status_str_alloc = len+1;
		filter->status_str = gf_realloc(filter->status_str, filter->status_str_alloc);
		if (!filter->status_str) {
			filter->status_str_alloc = 0;
			return GF_OUT_OF_MEM;
		}
	}
	memcpy(filter->status_str, szStatus, len+1);
	filter->status_percent = percent;
	filter->report_updated = GF_TRUE;

	memset(&evt, 0, sizeof(GF_Event));
	evt.type = GF_EVENT_PROGRESS;
	evt.progress.progress_type = 3;
	evt.progress.done = percent;
	evt.progress.total = ((s32)percent>0) ? 10000 : 0;
	evt.progress.filter_idx = gf_list_find(filter->session->filters, filter);
	gf_fs_ui_event(filter->session, &evt);
	return GF_OK;
}

void gf_filter_report_meta_option(GF_Filter *filter, const char *arg, Bool was_found, const char *sub_opt_name)
{
	if (!filter->session || filter->removed || filter->finalized) return;
	if (filter->orig_args) {
		char *opt_arg = strstr(filter->orig_args, "gfopt");
		if (opt_arg) {
			char *arg_pos = strstr(filter->orig_args, arg);
			if (arg_pos && (arg_pos>opt_arg))
				return;
		}
	}
	gf_mx_p(filter->session->filters_mx);
	//meta filters may report unused options set when setting up the filter, and not specified
	//at prompt, ignore them
	//the argtype will be ignored here
	gf_fs_push_arg(filter->session, arg, was_found, GF_ARGTYPE_META_REPORTING, filter, sub_opt_name);
	gf_mx_v(filter->session->filters_mx);
}

GF_Err gf_filter_set_description(GF_Filter *filter, const char *new_desc)
{
	if (!filter) return GF_BAD_PARAM;
	if (filter->instance_description)
		gf_free(filter->instance_description);

	filter->instance_description = new_desc ? gf_strdup(new_desc) : NULL;
	return GF_OK;
}
GF_Err gf_filter_set_class_hint(GF_Filter *filter, GF_ClassTypeHint class_hint)
{
	if (!filter) return GF_BAD_PARAM;
	filter->instance_class_hint = class_hint;
	return GF_OK;
}
GF_EXPORT
const char *gf_filter_get_description(GF_Filter *filter)
{
	return filter ? filter->instance_description : NULL;
}
GF_EXPORT
GF_ClassTypeHint gf_filter_get_class_hint(GF_Filter *filter)
{
	return filter ? filter->instance_class_hint : 0;
}

GF_Err gf_filter_set_version(GF_Filter *filter, const char *new_desc)
{
	if (!filter) return GF_BAD_PARAM;
	if (filter->instance_version)
		gf_free(filter->instance_version);

	filter->instance_version = new_desc ? gf_strdup(new_desc) : NULL;
	return GF_OK;
}
GF_EXPORT
const char *gf_filter_get_version(GF_Filter *filter)
{
	return filter ? filter->instance_version : NULL;
}

GF_Err gf_filter_set_author(GF_Filter *filter, const char *new_desc)
{
	if (!filter) return GF_BAD_PARAM;
	if (filter->instance_author)
		gf_free(filter->instance_author);

	filter->instance_author = new_desc ? gf_strdup(new_desc) : NULL;
	return GF_OK;
}
GF_EXPORT
const char *gf_filter_get_author(GF_Filter *filter)
{
	return filter ? filter->instance_author : NULL;
}

GF_Err gf_filter_set_help(GF_Filter *filter, const char *new_desc)
{
	if (!filter) return GF_BAD_PARAM;
	if (filter->instance_help)
		gf_free(filter->instance_help);

	filter->instance_help = new_desc ? gf_strdup(new_desc) : NULL;
	return GF_OK;
}
GF_EXPORT
const char *gf_filter_get_help(GF_Filter *filter)
{
	return filter ? filter->instance_help : NULL;
}

GF_Err gf_filter_define_args(GF_Filter *filter, GF_FilterArgs *args)
{
	if (!filter) return GF_BAD_PARAM;
	filter->instance_args = args;
	return GF_OK;
}
GF_EXPORT
GF_FilterArgs *gf_filter_get_args(GF_Filter *filter)
{
	return filter ? filter->instance_args : NULL;
}

GF_EXPORT
const GF_FilterCapability *gf_filter_get_caps(GF_Filter *filter, u32 *nb_caps)
{
	if (!filter || !filter->forced_caps || !nb_caps) return NULL;
	*nb_caps = filter->nb_forced_caps;
	return filter->forced_caps;
}

GF_EXPORT
GF_Filter *gf_filter_load_filter(GF_Filter *filter, const char *name, GF_Err *err_code)
{
	if (!filter) return NULL;
	GF_Filter *f = gf_fs_load_filter(filter->session, name, err_code);
	if (!f) return NULL;
	//do not allow implicit cloning when loading a filter from another filter
	if (!strstr(name, "clone"))
		f->clonable = GF_FILTER_NO_CLONE;
	return f;
}


GF_EXPORT
Bool gf_filter_end_of_session(GF_Filter *filter)
{
	if (!filter) return GF_TRUE;
	return filter->session->in_final_flush;
}

GF_EXPORT
Bool gf_filter_is_alias(GF_Filter *filter)
{
	if (filter && filter->multi_sink_target) return GF_TRUE;
	return GF_FALSE;
}

/*! checks if the some PID connection tasks are still pending at the session level
\param filter target filter
\return GF_TRUE if some connection tasks are pending, GF_FALSE otherwise
*/
GF_EXPORT
Bool gf_filter_connections_pending(GF_Filter *filter)
{
	u32 i, count;
	Bool res = GF_FALSE;
	if (!filter) return GF_FALSE;
	if (filter->session->pid_connect_tasks_pending) return GF_TRUE;
	if (filter->session->in_final_flush) return GF_FALSE;
	gf_mx_p(filter->session->filters_mx);
	count = gf_list_count(filter->session->filters);
	for (i=0; i<count; i++) {
		u32 j;
		GF_Filter *f = gf_list_get(filter->session->filters, i);
		if (!f || f->removed || f->finalized) continue;
		if (f->subsession_id != filter->subsession_id) continue;

		gf_mx_v(filter->session->filters_mx);
		gf_mx_p(f->tasks_mx);
		for (j=0; j<f->num_output_pids; j++) {
			GF_FilterPid *pid = gf_list_get(f->output_pids, j);
			if (pid->init_task_pending) {
				res = GF_TRUE;
				break;
			}
		}
		if (res) {
			gf_mx_v(f->tasks_mx);
			gf_mx_p(filter->session->filters_mx);
			break;
		}

		if (f==filter) {
			gf_mx_v(f->tasks_mx);
			gf_mx_p(filter->session->filters_mx);
			continue;
		}

		if (f->in_pid_connection_pending || f->out_pid_connection_pending) {
			res = GF_TRUE;
		}
		//filter has no output, check if it is expected or not
		else if (!f->removed && !f->finalized && !f->disabled && !f->num_output_pids && !f->act_as_sink && !f->multi_sink_target) {
			if (f->forced_caps) {
				res = gf_filter_has_out_caps(f->forced_caps, f->nb_forced_caps);
			} else {
				res = gf_filter_has_out_caps(f->freg->caps, f->freg->nb_caps);
			}
			if (res) {
				if (gf_filter_in_parent_chain(f, filter))
					res = GF_FALSE;
			}
		}
		gf_mx_v(f->tasks_mx);
		gf_mx_p(filter->session->filters_mx);
		if (res)
			break;
	}
	gf_mx_v(filter->session->filters_mx);

	return res;
}

GF_EXPORT
GF_Err gf_filter_prevent_blocking(GF_Filter *filter, Bool prevent_blocking_enabled)
{
	if (!filter) return GF_BAD_PARAM;
	filter->prevent_blocking = prevent_blocking_enabled;
	return GF_OK;
}

GF_EXPORT
Bool gf_filter_is_dynamic(GF_Filter *filter)
{
	return filter ? filter->dynamic_filter : GF_FALSE;
}

GF_EXPORT
void gf_filter_block_eos(GF_Filter *filter, Bool do_block)
{
	if (filter) filter->block_eos = do_block;
}

GF_EXPORT
GF_Err gf_filter_reconnect_output(GF_Filter *filter, GF_FilterPid *for_pid)
{
	u32 i;
	if (!filter) return GF_BAD_PARAM;
	if (for_pid) {
		if (PID_IS_INPUT(for_pid)) return GF_BAD_PARAM;
	}
	if (!filter->num_output_pids)
		return GF_EOS;
	//in case we had pending output pids
	if (filter->deferred_link) {
		filter->deferred_link = GF_FALSE;
		if (filter->has_pending_pids) {
			GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Applying defer linking of filter %s\n", filter->name));
			gf_filter_check_pending_pids(filter);
		}
		return GF_OK;
	}
	GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Relinking filter %s PID %s\n", filter->name, for_pid ? for_pid->name : "all"));

	for (i=0; i<filter->num_output_pids; i++) {
		GF_FilterPid *pid = gf_list_get(filter->output_pids, i);
		if (for_pid && (pid != for_pid)) continue;
		gf_filter_pid_post_init_task(filter, pid);
	}
	return GF_OK;
}

GF_EXPORT
GF_Err gf_filter_set_event_target(GF_Filter *filter, Bool enable_events)
{
	if (!filter) return GF_BAD_PARAM;
	filter->event_target = enable_events;
	return GF_OK;
}

GF_EXPORT
GF_Err gf_filter_push_caps(GF_Filter *filter, u32 code, GF_PropertyValue *value, const char *name, u32 flags, u8 priority)
{
	u32 nb_caps;
	GF_FilterCapability *caps;
	if (! (filter->freg->flags & GF_FS_REG_CUSTOM)) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Attempt to push cap on non custom filter %s\n", filter->freg->name));
		return GF_BAD_PARAM;
	}
	caps = (GF_FilterCapability *)filter->forced_caps;
	nb_caps = filter->nb_forced_caps;
	caps = gf_realloc(caps, sizeof(GF_FilterCapability)*(nb_caps+1) );
	if (!caps) return GF_OUT_OF_MEM;
	caps[nb_caps].code = code;
	caps[nb_caps].val = *value;
	caps[nb_caps].name = name ? gf_strdup(name) : NULL;
	caps[nb_caps].priority = priority;
	caps[nb_caps].flags = flags;
	filter->nb_forced_caps++;
	filter->forced_caps = caps;
	filter->nb_forced_bundles = filter->nb_forced_caps ? gf_filter_caps_bundle_count(filter->forced_caps, filter->nb_forced_caps) : 0;

	//reload graph for this updated registry!
	GF_FilterRegister *freg = (GF_FilterRegister *)filter->freg;
	freg->caps = filter->forced_caps;
	freg->nb_caps = filter->nb_forced_caps;
	gf_filter_sess_reset_graph(filter->session, filter->freg);
	gf_filter_sess_build_graph(filter->session, filter->freg);

	return GF_OK;
}

GF_EXPORT
GF_Err gf_filter_set_process_ckb(GF_Filter *filter, GF_Err (*process_cbk)(GF_Filter *filter) )
{
	if (! (filter->freg->flags & GF_FS_REG_CUSTOM)) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Attempt to assign filter callback on non custom filter %s\n", filter->freg->name));
		return GF_BAD_PARAM;
	}
	((GF_FilterRegister *) filter->freg)->process = process_cbk;
	return GF_OK;
}


GF_EXPORT
GF_Err gf_filter_set_configure_ckb(GF_Filter *filter, GF_Err (*configure_cbk)(GF_Filter *filter, GF_FilterPid *PID, Bool is_remove) )
{
	if (! (filter->freg->flags & GF_FS_REG_CUSTOM)) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Attempt to assign filter callback on non custom filter %s\n", filter->freg->name));
		return GF_BAD_PARAM;
	}
	((GF_FilterRegister *) filter->freg)->configure_pid = configure_cbk;
	return GF_OK;
}

GF_EXPORT
GF_Err gf_filter_set_process_event_ckb(GF_Filter *filter, Bool (*process_event_cbk)(GF_Filter *filter, const GF_FilterEvent *evt) )
{
	if (! (filter->freg->flags & GF_FS_REG_CUSTOM)) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Attempt to assign filter callback on non custom filter %s\n", filter->freg->name));
		return GF_BAD_PARAM;
	}
	((GF_FilterRegister *) filter->freg)->process_event = process_event_cbk;
	return GF_OK;
}

GF_EXPORT
GF_Err gf_filter_set_reconfigure_output_ckb(GF_Filter *filter, GF_Err (*reconfigure_output_cbk)(GF_Filter *filter, GF_FilterPid *PID) )
{
	if (! (filter->freg->flags & GF_FS_REG_CUSTOM)) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Attempt to assign filter callback on non custom filter %s\n", filter->freg->name));
		return GF_BAD_PARAM;
	}
	((GF_FilterRegister *) filter->freg)->reconfigure_output = reconfigure_output_cbk;
	return GF_OK;
}

GF_EXPORT
GF_Err gf_filter_set_probe_data_cbk(GF_Filter *filter, const char * (*probe_data_cbk)(const u8 *data, u32 size, GF_FilterProbeScore *score) )
{
	if (! (filter->freg->flags & GF_FS_REG_CUSTOM)) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Attempt to assign filter callback on non custom filter %s\n", filter->freg->name));
		return GF_BAD_PARAM;
	}
	((GF_FilterRegister *) filter->freg)->probe_data = probe_data_cbk;
	return GF_OK;
}


GF_EXPORT
const GF_FilterArgs *gf_filter_enumerate_args(GF_Filter *filter, u32 idx)
{
	u32 i;
	if (!filter) return NULL;
	if (!filter->freg->args) return NULL;

	for (i=0; i<=idx; i++) {
		if (! filter->freg->args[i].arg_name)
			return NULL;
	}
	return &filter->freg->args[idx];
}

GF_EXPORT
GF_Err gf_filter_set_rt_udta(GF_Filter *filter, void *udta)
{
	if (!filter) return GF_BAD_PARAM;
	filter->rt_udta = udta;
	return GF_OK;
}

GF_EXPORT
void *gf_filter_get_rt_udta(GF_Filter *filter)
{
	if (!filter) return NULL;
	return filter->rt_udta;
}


Bool gf_filter_is_instance_of(GF_Filter *filter, const GF_FilterRegister *freg)
{
	if (filter && freg && (filter->freg==freg))
		return GF_TRUE;
	return GF_FALSE;
}

GF_EXPORT
void gf_filter_abort(GF_Filter *filter)
{
	u32 i;
	GF_FilterEvent evt;
	if (!filter) return;
	gf_mx_p(filter->tasks_mx);
	GF_FEVT_INIT(evt, GF_FEVT_STOP, NULL);
	for (i=0; i<filter->num_input_pids; i++) {
		GF_FilterPid *pid = gf_list_get(filter->input_pids, i);
		gf_filter_pid_set_discard(pid, GF_TRUE);
		evt.base.on_pid = pid;
		gf_filter_pid_send_event(pid, &evt);
	}
	for (i=0; i<filter->num_output_pids; i++) {
		GF_FilterPid *pid = gf_list_get(filter->output_pids, i);
		gf_filter_pid_set_eos(pid);
	}
	filter->disabled = GF_FILTER_DISABLED;
	gf_mx_v(filter->tasks_mx);
}

GF_EXPORT
void gf_filter_lock(GF_Filter *filter, Bool do_lock)
{
	if (!filter) return;
	if (do_lock)
		gf_mx_p(filter->tasks_mx);
	else
		gf_mx_v(filter->tasks_mx);
}

GF_EXPORT
void gf_filter_lock_all(GF_Filter *filter, Bool do_lock)
{
	if (!filter) return;
	if (do_lock)
		gf_mx_p(filter->session->filters_mx);
	else
		gf_mx_v(filter->session->filters_mx);
}

void gf_filter_mirror_forced_caps(GF_Filter *filter, GF_Filter *dst_filter)
{
	if (filter && dst_filter) {
		filter->forced_caps = dst_filter->forced_caps;
		filter->nb_forced_caps = dst_filter->nb_forced_caps;
		filter->nb_forced_bundles = dst_filter->nb_forced_bundles;
	}
}

GF_EXPORT
void gf_filter_require_source_id(GF_Filter *filter)
{
	if (filter) filter->require_source_id = GF_TRUE;
}

void gf_filter_set_blocking(GF_Filter *filter, Bool is_blocking)
{
	if (filter) filter->is_blocking_source = is_blocking;
}

GF_EXPORT
const GF_FilterRegister *gf_filter_get_register(GF_Filter *filter)
{
	return filter ? filter->freg : NULL;
}

GF_EXPORT
void gf_filter_force_main_thread(GF_Filter *filter, Bool do_tag)
{
	if (!filter) return;
	if (do_tag)
		safe_int_inc(&filter->nb_main_thread_forced);
	else
		safe_int_dec(&filter->nb_main_thread_forced);
}

GF_EXPORT
Bool gf_filter_is_sink(GF_Filter *filter)
{
	if (!filter) return GF_FALSE;
	if (filter->forced_caps) {
		return !gf_filter_has_out_caps(filter->forced_caps, filter->nb_forced_caps);
	}
	return !gf_filter_has_out_caps(filter->freg->caps, filter->freg->nb_caps);
}

GF_EXPORT
Bool gf_filter_is_source(GF_Filter *filter)
{
	if (!filter) return GF_FALSE;
	if (filter->forced_caps) {
		return !gf_filter_has_in_caps(filter->forced_caps, filter->nb_forced_caps);
	}
	return !gf_filter_has_in_caps(filter->freg->caps, filter->freg->nb_caps);
}

GF_EXPORT
GF_Err gf_filter_tag_subsession(GF_Filter *filter, u32 subsession_id, u32 source_id)
{
	if (!filter) return GF_BAD_PARAM;
	//ignored in non implicit mode
	if (! (filter->session->flags & GF_FS_FLAG_IMPLICIT_MODE)) return GF_OK;
	//subsession explicitly assigned
	if (filter->subsession_id) return filter->subsession_id;
	filter->subsession_id = subsession_id;
	if (gf_filter_is_sink(filter))
		filter->subsource_id = 0;
	else
		filter->subsource_id = 1 + source_id;
	return GF_OK;
}

GF_EXPORT
Bool gf_filter_has_connect_errors(GF_Filter *filter)
{
	if (filter && filter->session->last_connect_error) return GF_TRUE;
	return GF_FALSE;
}

GF_EXPORT
Bool gf_filter_is_temporary(GF_Filter *filter)
{
	return filter ? filter->removed : GF_FALSE;
}

GF_EXPORT
void gf_filter_meta_set_instances(GF_Filter *filter, const char *instance_names_list)
{
	if (!filter) return;
	if (filter->meta_instances) gf_free(filter->meta_instances);
	filter->meta_instances = gf_strdup(instance_names_list);
}

GF_EXPORT
const char *gf_filter_meta_get_instances(GF_Filter *filter)
{
	return filter ? filter->meta_instances : NULL;
}

void gf_filter_skip_seg_size_events(GF_Filter *filter)
{
	if (filter) filter->no_segsize_evts=GF_TRUE;
}

	//filter netcap_id
GF_EXPORT
const char *gf_filter_get_netcap_id(GF_Filter *filter)
{
	return filter ? filter->netcap_id : NULL;
}


#if 0
void gf_filter_pid_dump_buffers(GF_FilterPid *pid)
{
	u32 i, j, count;
	if (!pid) return;
	if (PID_IS_OUTPUT(pid)) {
		for (i=0; i<pid->num_destinations; i++) {
			GF_FilterPidInst *pidi = gf_list_get(pid->destinations, i);
			GF_LOG(GF_LOG_WARNING, GF_LOG_APP, ("%s (%s->%s) %d packets\n", pid->name, pid->filter->name, pidi->filter->name, gf_fq_count(pidi->packets)));
			count = gf_list_count(pidi->filter->output_pids);
			for (j=0; j<count; j++) {
				gf_filter_pid_dump_buffers( gf_list_get(pidi->filter->output_pids, j) );
			}
		}
		return;
	}
	GF_FilterPidInst *pid_inst = (GF_FilterPidInst *) pid;
	GF_LOG(GF_LOG_WARNING, GF_LOG_APP, ("%s (%s->%s) %d packets\n", pid_inst->pid->name, pid_inst->pid->filter->name, pid_inst->filter->name, gf_fq_count(pid_inst->packets)));
	for (i=0; i<pid_inst->pid->filter->num_input_pids; i++) {
		pid = gf_list_get(pid_inst->pid->filter->input_pids, i);
		gf_filter_pid_dump_buffers(pid);
	}
}
GF_EXPORT
void gf_filter_dump_buffers(GF_Filter *f)
{
	u32 i;
	for (i=0; i<f->num_input_pids; i++) {
		gf_filter_pid_dump_buffers(gf_list_get(f->input_pids, i) );
	}
	for (i=0; i<f->num_output_pids; i++) {
		gf_filter_pid_dump_buffers(gf_list_get(f->output_pids, i) );
	}
}
#endif

static GF_Err gf_filter_probe_link_internal(GF_Filter *filter, u32 opid_idx, const char *fname, Bool all_links, char **res_chain)
{
	char *fdesc=NULL;
	char szFmt[20];
	GF_Filter *new_f;
	GF_Err e;
	GF_FilterPid *opid=NULL;
	GF_FilterSession *fs;
	GF_List *tmp_blacklist;
	if (!filter || !fname || !res_chain) return GF_BAD_PARAM;
	*res_chain = NULL;
	fs = filter->session;

	opid = gf_filter_get_opid(filter, opid_idx);
	if (!opid) return GF_BAD_PARAM;

	if (!strncmp(fname, "src=", 4) || !strlen(fname)) return GF_BAD_PARAM;

	fdesc = gf_strdup(fname);
	sprintf(szFmt, "%cgpac", fs->sep_args);
	if (strstr(fdesc, szFmt)) {
		sprintf(szFmt, "%c_GFTMP", fs->sep_args);
	} else {
		sprintf(szFmt, "%c%cgpac%c%c_GFTMP", fs->sep_args, fs->sep_args, fs->sep_args, fs->sep_args);
	}
	gf_dynstrcat(&fdesc, szFmt, NULL);

	gf_fs_lock_filters(fs, GF_TRUE);
	if (!strncmp(fname, "dst=", 4)) {
		new_f = gf_fs_load_destination(fs, fdesc+4, NULL, NULL, &e);
	} else {
		new_f = gf_fs_load_filter(fs, fdesc, &e);
	}
	gf_free(fdesc);

	if (!new_f) {
		gf_fs_lock_filters(fs, GF_FALSE);
		return e;
	}
	tmp_blacklist = gf_list_new();
	while (1) {
		GF_LinkInfo link_info;
		const GF_FilterRegister *last_freg = NULL;
		GF_List *fchain = gf_filter_pid_compute_link(opid, new_f, tmp_blacklist, &link_info);
		if (!fchain) break;
		if (*res_chain && (*res_chain)[0]) {
			gf_dynstrcat(res_chain, "|", NULL);
		}
		if (all_links) {
			char szTmp[20];
			sprintf(szTmp, "%u;%u,", link_info.distance, link_info.priority);
			gf_dynstrcat(res_chain, szTmp, NULL);
		}

		u32 i, count = gf_list_count(fchain);
		for (i=0; i<count; i+=2) {
			const GF_FilterRegister *freg = gf_list_get(fchain, i);
			if ((i+2==count) && (freg == new_f->freg))
				break;
			gf_dynstrcat(res_chain, freg->name, i ? "," : NULL);
			last_freg = freg;
		}
		gf_list_del(fchain);
		if (! *res_chain) *res_chain = gf_strdup("");
		if (!last_freg) break;
		gf_list_add(tmp_blacklist, (void*)last_freg);
		if (!all_links) break;
	}
	gf_list_del(tmp_blacklist);

	gf_list_del_item(fs->filters, new_f);
	if (!new_f->finalized && new_f->freg->finalize) {
		new_f->freg->finalize(new_f);
	}
	gf_filter_del(new_f);
	gf_fs_lock_filters(fs, GF_FALSE);
	if (*res_chain) return GF_OK;
	return GF_FILTER_NOT_FOUND;
}

GF_EXPORT
GF_Err gf_filter_probe_links(GF_Filter *filter, u32 opid_idx, const char *fname, char **res_chain)
{
	return gf_filter_probe_link_internal(filter, opid_idx, fname, GF_TRUE, res_chain);
}

GF_EXPORT
GF_Err gf_filter_probe_link(GF_Filter *filter, u32 opid_idx, const char *fname, char **res_chain)
{
	return gf_filter_probe_link_internal(filter, opid_idx, fname, GF_FALSE, res_chain);
}


GF_EXPORT
GF_Err gf_filter_get_possible_destinations(GF_Filter *filter, s32 opid_idx, char **res_list)
{
	u32 i, j, k, count;
	GF_FilterPid *opid=NULL;
	if (!filter || !res_list) return GF_BAD_PARAM;
	if (opid_idx>=0) {
		opid = gf_list_get(filter->output_pids, opid_idx);
		if (opid==NULL) return GF_BAD_PARAM;
	} else {
		if (!filter->num_output_pids) return GF_FILTER_NOT_FOUND;
	}
	*res_list = NULL;
	count = gf_list_count(filter->session->links);
	for (i=0; i<count; i++) {
		Bool is_match=GF_FALSE;
		const GF_FilterRegDesc *src = gf_list_get(filter->session->links, i);
		if (!src->nb_edges)
			continue;

		for (j=0; j<src->nb_edges; j++) {
			GF_FilterRegEdge *edge = &src->edges[j];
			if (edge->src_reg->freg != filter->freg) continue;
			//check pid caps match

			for (k=0; k<filter->num_output_pids; k++) {
				if ((opid_idx>=0) && ((u32)opid_idx==k)) continue;
				opid = gf_list_get(filter->output_pids, k);
				if (!opid) break;

				s16 priority=0;
				u32 dst_bundle_idx;
				//check path weight for the given dst cap - we MUST give the target cap otherwise we might get a default match to another cap
				u32 path_weight = gf_filter_pid_caps_match(opid, src->freg, NULL, &priority, &dst_bundle_idx, opid->filter->dst_filter, edge->dst_cap_idx);
				if (!path_weight) continue;
				is_match = GF_TRUE;
				break;
			}
		}
		if (is_match) {
			gf_dynstrcat(res_list, src->freg->name, ",");
		}
	}
	if (! *res_list) return GF_FILTER_NOT_FOUND;
	return GF_OK;
}
