/*
 * Copyright (c) 2004-2006 Maxim Sobolev <sobomax@FreeBSD.org>
 * Copyright (c) 2006-2007 Sippy Software, Inc., http://www.sippysoft.com
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#ifdef LINUX_XXX
/* Apparently needed for drand48(3) */
#define _SVID_SOURCE	1
/* Needed for the asprintf(3) */
#define _GNU_SOURCE	1
#endif

#include <sys/types.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <getopt.h>
#include <grp.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <pwd.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "config_pp.h"

#if !defined(NO_ERR_H)
#include <err.h>
#include "rtpp_util.h"
#else
#include "rtpp_util.h"
#endif

#ifdef HAVE_SYSTEMD_DAEMON
#include <systemd/sd-daemon.h>
#endif

#include <elperiodic.h>

#include "rtpp_types.h"
#include "rtpp_refcnt.h"
#include "rtpp_runcreds.h"
#include "rtpp_cfile.h"
#include "rtpp_log.h"
#include "rtpp_log_obj.h"
#include "rtpp_cfg_stable.h"
#include "rtpp_defines.h"
#include "rtpp_command.h"
#include "rtpp_controlfd.h"
#include "rtpp_genuid_singlet.h"
#include "rtpp_hash_table.h"
#include "rtpp_command_ver.h"
#include "rtpp_command_async.h"
#include "rtpp_command_ecodes.h"
#include "rtpp_port_table.h"
#include "rtpp_proc_async.h"
#include "rtpp_proc_ttl.h"
#include "rtpp_bindaddrs.h"
#include "rtpp_network.h"
#include "rtpp_notify.h"
#include "rtpp_math.h"
#include "rtpp_mallocs.h"
#if ENABLE_MODULE_IF
#include "rtpp_module_if.h"
#endif
#include "rtpp_stats.h"
#include "rtpp_sessinfo.h"
#include "rtpp_list.h"
#include "rtpp_time.h"
#include "rtpp_timed.h"
#include "rtpp_timed_task.h"
#include "rtpp_tnotify_set.h"
#include "rtpp_weakref.h"
#include "rtpp_debug.h"
#include "rtpp_locking.h"
#include "rtpp_nofile.h"
#include "advanced/po_manager.h"
#ifdef RTPP_CHECK_LEAKS
#include "libexecinfo/stacktraverse.h"
#include "libexecinfo/execinfo.h"
#include "rtpp_memdeb_internal.h"
#endif
#if RTPP_DEBUG_catchtrace
#include "rtpp_stacktrace.h"
#endif

#ifndef RTPP_DEBUG
# define RTPP_DEBUG	0
#else
# define RTPP_DEBUG	1
#endif

#define PRIO_UNSET (PRIO_MIN - 1)

static void usage(void);

#ifdef RTPP_CHECK_LEAKS
RTPP_MEMDEB_APP_STATIC;
#endif

static void
usage(void)
{

    fprintf(stderr, "usage:\trtpproxy [-2fvFiPaRbD] [-l addr1[/addr2]] "
      "[-6 addr1[/addr2]] [-s path]\n\t  [-t tos] [-r rdir [-S sdir]] [-T ttl] "
      "[-L nfiles] [-m port_min]\n\t  [-M port_max] [-u uname[:gname]] [-w sock_mode] "
      "[-n timeout_socket]\n\t  [-d log_level[:log_facility]] [-p pid_file]\n"
      "\t  [-c fifo|rr] [-A addr1[/addr2] [-N random/sched_offset] [-W setup_ttl]\n"
      "\trtpproxy -V\n");
    exit(1);
}

static struct rtpp_cfg_stable *_sig_cf;

static void rtpp_exit(int, int) __attribute__ ((noreturn));

static void
rtpp_exit(int memdeb, int rval)
{
    int ecode;

    ecode = 0;
#ifdef RTPP_CHECK_LEAKS
    if (memdeb) {
        ecode = rtpp_memdeb_dumpstats(MEMDEB_SYM, 0) == 0 ? 0 : 1;
    }
#endif
    exit(rval == 0 ? ecode : rval);
}

static void
rtpp_glog_fin(void)
{

    CALL_SMETHOD(_sig_cf->glog->rcnt, decref);
#ifdef RTPP_CHECK_LEAKS
    RTPP_MEMDEB_FIN(rtpproxy);
#ifdef RTPP_MEMDEB_STDOUT
    fclose(stdout);
#endif
#endif
}

static void
fatsignal(int sig)
{

    RTPP_LOG(_sig_cf->glog, RTPP_LOG_INFO, "got signal %d", sig);
    if (_sig_cf->fastshutdown == 0) {
        _sig_cf->fastshutdown = 1;
        return;
    }
    /*
     * Got second signal while already in the fastshutdown mode, something
     * probably jammed, do quick exit right from sighandler.
     */
    rtpp_exit(1, 0);
}

static void
sighup(int sig)
{

    if (_sig_cf->slowshutdown == 0) {
        RTPP_LOG(_sig_cf->glog, RTPP_LOG_INFO,
          "got SIGHUP, initiating deorbiting-burn sequence");
    }
    _sig_cf->slowshutdown = 1;
}

static void
ehandler(void)
{

#if RTPP_DEBUG_catchtrace 
    rtpp_stacktrace_print("Exiting from: ehandler()");
#endif
    rtpp_controlfd_cleanup(_sig_cf);
    unlink(_sig_cf->pid_file);
    RTPP_LOG(_sig_cf->glog, RTPP_LOG_INFO, "rtpproxy ended");
}

long long
rtpp_rlim_max(struct rtpp_cfg_stable *cfsp)
{

    return (long long)(cfsp->nofile->limit->rlim_max);
}

#define LOPT_DSO      256
#define LOPT_BRSYM    257
#define LOPT_NICE     258
#define LOPT_OVL_PROT 259
#define LOPT_CONFIG   260

const static struct option longopts[] = {
    { "dso", required_argument, NULL, LOPT_DSO },
    { "bridge_symmetric", no_argument, NULL, LOPT_BRSYM },
    { "nice", required_argument, NULL, LOPT_NICE },
    { "overload_prot", optional_argument, NULL, LOPT_OVL_PROT },
    { "config", required_argument, NULL, LOPT_CONFIG },
    { NULL,  0,                 NULL, 0 }
};

static void
init_config_bail(struct rtpp_cfg_stable *cfsp, int rval, const char *msg, int memdeb)
{
    struct rtpp_module_if *mif, *tmp;
    struct rtpp_ctrl_sock *ctrl_sock, *ctrl_sock_next;

    if (msg != NULL) {
        RTPP_LOG(cfsp->glog, RTPP_LOG_ERR, "%s", msg);
    }
    free(cfsp->locks);
    CALL_METHOD(cfsp->rtpp_tnset_cf, dtor);
    CALL_METHOD(cfsp->nofile, dtor);

    for (ctrl_sock = RTPP_LIST_HEAD(cfsp->ctrl_socks);
      ctrl_sock != NULL; ctrl_sock = ctrl_sock_next) {
        ctrl_sock_next = RTPP_ITER_NEXT(ctrl_sock);
        free(ctrl_sock);
    }
    free(cfsp->ctrl_socks);
    free(cfsp->runcreds);
#if ENABLE_MODULE_IF
    for (mif = RTPP_LIST_HEAD(cfsp->modules_cf); mif != NULL; mif = tmp) {
        tmp = RTPP_ITER_NEXT(mif);
        CALL_SMETHOD(mif->rcnt, decref);
    }
#endif
    free(cfsp->modules_cf);
    rtpp_gen_uid_free();
    free(cfsp);
    rtpp_exit(memdeb, rval);
}

static void
init_config(struct rtpp_cfg_stable *cfsp, int argc, char **argv)
{
    int ch, i, umode, stdio_mode;
    char *bh[2], *bh6[2], *cp, *tp[2];
    const char *errmsg;
    struct passwd *pp;
    struct group *gp;
    double x, y;
    struct rtpp_ctrl_sock *ctrl_sock;
    int option_index, brsym;
    struct proto_cap *pcp;
    struct rtpp_module_if *mif;
    char mpath[PATH_MAX + 1];

    bh[0] = bh[1] = bh6[0] = bh6[1] = NULL;

    umode = stdio_mode = 0;

    cfsp->pid_file = PID_FILE;

    cfsp->port_min = PORT_MIN;
    cfsp->port_max = PORT_MAX;
    cfsp->port_ctl = 0;

    cfsp->advaddr[0] = NULL;
    cfsp->advaddr[1] = NULL;

    cfsp->max_ttl = SESSION_TIMEOUT;
    cfsp->tos = TOS;
    cfsp->rrtcp = 1;
    cfsp->runcreds->sock_mode = 0;
    cfsp->ttl_mode = TTL_UNIFIED;
    cfsp->log_level = -1;
    cfsp->log_facility = -1;
    cfsp->sched_offset = 0.0;
    cfsp->sched_hz = rtpp_get_sched_hz();
    cfsp->sched_policy = SCHED_OTHER;
    cfsp->sched_nice = PRIO_UNSET;
    cfsp->target_pfreq = MIN(POLL_RATE, cfsp->sched_hz);
#if RTPP_DEBUG
    fprintf(stderr, "target_pfreq = %f\n", cfsp->target_pfreq);
#endif
    cfsp->slowshutdown = 0;
    cfsp->fastshutdown = 0;

    cfsp->rtpp_tnset_cf = rtpp_tnotify_set_ctor();
    if (cfsp->rtpp_tnset_cf == NULL) {
        err(1, "rtpp_tnotify_set_ctor");
    }

    cfsp->locks = malloc(sizeof(*cfsp->locks));
    if (cfsp->locks == NULL) {
        err(1, "malloc(rtpp_cfg->locks)");
    }
    pthread_mutex_init(&(cfsp->locks->glob), NULL);
    cfsp->bindaddrs_cf = rtpp_bindaddrs_ctor();
    if (cfsp->bindaddrs_cf == NULL) {
        err(1, "malloc(rtpp_cfg->bindaddrs_cf)");
    }

    cfsp->nofile = rtpp_nofile_ctor();
    if (cfsp->nofile == NULL)
        err(1, "malloc(rtpp_cfg->nofile)");

    option_index = -1;
    brsym = 0;
    while ((ch = getopt_long(argc, argv, "vf2Rl:6:s:S:t:r:p:T:L:m:M:u:Fin:Pad:"
      "VN:c:A:w:bW:DC", longopts, &option_index)) != -1) {
	switch (ch) {
        case LOPT_DSO:
            if (!RTPP_LIST_IS_EMPTY(cfsp->modules_cf)) {
                 errx(1, "this version of the rtpproxy only supports loading a "
                   "single module");
            }
            cp = realpath(optarg, mpath);
            if (cp == NULL) {
                 err(1, "realpath");
            }
            mif = rtpp_module_if_ctor(cp);
            if (mif == NULL) {
                err(1, "%s: dymanic module constructor has failed", cp);
            }
            if (CALL_METHOD(mif, load, cfsp, cfsp->glog) != 0) {
                RTPP_LOG(cfsp->glog, RTPP_LOG_ERR,
                  "%p: dymanic module load has failed", mif);
                exit(1);
            }
            rtpp_list_append(cfsp->modules_cf, mif);
            break;

        case LOPT_BRSYM:
            brsym = 1;
            break;

        case LOPT_NICE:
            if (atoi_safe(optarg, &cfsp->sched_nice))
                errx(1, "%s: nice level argument is invalid", optarg);
            if (cfsp->sched_nice > PRIO_MAX || cfsp->sched_nice < PRIO_MIN) {
                errx(1, "%d: nice level is out of range %d..%d",
                  cfsp->sched_nice, PRIO_MIN, PRIO_MAX);
            }
            break;

        case LOPT_OVL_PROT:
            cfsp->overload_prot.low_trs = 0.85;
            cfsp->overload_prot.high_trs = 0.90;
            cfsp->overload_prot.ecode = ECODE_OVERLOAD;
            break;

        case LOPT_CONFIG:
            cfsp->cfile = optarg;
            break;

        case 'c':
            if (strcmp(optarg, "fifo") == 0) {
                 cfsp->sched_policy = SCHED_FIFO;
                 break;
            }
            if (strcmp(optarg, "rr") == 0) {
                 cfsp->sched_policy = SCHED_RR;
                 break;
            }
            errx(1, "%s: unknown scheduling policy", optarg);
            break;

        case 'N':
	    if (strcmp(optarg, "random") == 0) {
                x = getdtime() * 1000000.0;
                srand48((long)x);
                cfsp->sched_offset = drand48();
            } else {
                tp[0] = optarg;
                tp[1] = strchr(tp[0], '/');
       	        if (tp[1] == NULL) {
                    errx(1, "%s: -N should be in the format X/Y", optarg);
                }
                *tp[1] = '\0';
                tp[1]++;
                x = (double)strtol(tp[0], &tp[0], 10);
                y = (double)strtol(tp[1], &tp[1], 10);
                cfsp->sched_offset = x / y;
            }
            x = (double)cfsp->sched_hz / cfsp->target_pfreq;
            cfsp->sched_offset = trunc(x * cfsp->sched_offset) / x;
            cfsp->sched_offset /= cfsp->target_pfreq;
            warnx("sched_offset = %f",  cfsp->sched_offset);
            break;

	case 'f':
	    cfsp->nodaemon = 1;
	    break;

	case 'l':
	    bh[0] = optarg;
	    bh[1] = strchr(bh[0], '/');
	    if (bh[1] != NULL) {
		*bh[1] = '\0';
		bh[1]++;
		cfsp->bmode = 1;
		/*
		 * Historically, in bridge mode all clients are assumed to
		 * be asymmetric
		 */
		cfsp->aforce = 1;
	    }
	    break;

	case '6':
	    bh6[0] = optarg;
	    bh6[1] = strchr(bh6[0], '/');
	    if (bh6[1] != NULL) {
		*bh6[1] = '\0';
		bh6[1]++;
		cfsp->bmode = 1;
		cfsp->aforce = 1;
	    }
	    break;

    case 'A':
        if (*optarg == '\0') {
            errx(1, "first advertised address is invalid");
        }
        cfsp->advaddr[0] = optarg;
        cp = strchr(optarg, '/');
        if (cp != NULL) {
            *cp = '\0';
            cp++;
            if (*cp == '\0') {
                errx(1, "second advertised address is invalid");
            }
        }
        cfsp->advaddr[1] = cp;
        break;

	case 's':
            ctrl_sock = rtpp_ctrl_sock_parse(optarg);
            if (ctrl_sock == NULL) {
                errx(1, "can't parse control socket argument");
            }
            rtpp_list_append(cfsp->ctrl_socks, ctrl_sock);
            if (RTPP_CTRL_ISDG(ctrl_sock)) {
                umode = 1;
            } else if (ctrl_sock->type == RTPC_STDIO) {
                stdio_mode = 1;
            }
	    break;

	case 't':
            if (atoi_safe(optarg, &cfsp->tos))
                errx(1, "%s: TOS argument is invalid", optarg);
	    if (cfsp->tos > 255)
		errx(1, "%d: TOS is too large", cfsp->tos);
	    break;

	case '2':
	    cfsp->dmode = 1;
	    break;

	case 'v':
	    printf("Basic version: %d\n", CPROTOVER);
	    for (pcp = iterate_proto_caps(NULL); pcp != NULL; pcp = iterate_proto_caps(pcp)) {
		printf("Extension %s: %s\n", pcp->pc_id, pcp->pc_description);
	    }
	    init_config_bail(cfsp, 0, NULL, 0);
	    break;

	case 'r':
	    cfsp->rdir = optarg;
	    break;

	case 'S':
	    cfsp->sdir = optarg;
	    break;

	case 'R':
	    cfsp->rrtcp = 0;
	    break;

	case 'p':
	    cfsp->pid_file = optarg;
	    break;

	case 'T':
	    if (atoi_safe(optarg, &cfsp->max_ttl))
                errx(1, "%s: max TTL argument is invalid", optarg);
	    break;

	case 'L': {
            int rlim_max_opt;

            if (atoi_safe(optarg, &rlim_max_opt))
                errx(1, "%s: max file rlimit argument is invalid", optarg);
	    cfsp->nofile->limit->rlim_cur = rlim_max_opt;
	    cfsp->nofile->limit->rlim_max = rlim_max_opt;
	    if (setrlimit(RLIMIT_NOFILE, cfsp->nofile->limit) != 0)
		err(1, "setrlimit");
	    if (getrlimit(RLIMIT_NOFILE, cfsp->nofile->limit) != 0)
		err(1, "getrlimit");
	    if (cfsp->nofile->limit->rlim_max < rlim_max_opt)
		warnx("limit allocated by setrlimit (%d) is less than "
		  "requested (%d)", (int) cfsp->nofile->limit->rlim_max,
		  rlim_max_opt);
	    break;
            }

	case 'm':
	    if (atoi_safe(optarg, &cfsp->port_min))
                errx(1, "%s: min port argument is invalid", optarg);
	    break;

	case 'M':
	    if (atoi_safe(optarg, &cfsp->port_max))
                errx(1, "%s: max port argument is invalid", optarg);
	    break;

	case 'u':
	    cfsp->runcreds->uname = optarg;
	    cp = strchr(optarg, ':');
	    if (cp != NULL) {
		if (cp == optarg)
		    cfsp->runcreds->uname = NULL;
		cp[0] = '\0';
		cp++;
	    }
	    cfsp->runcreds->gname = cp;
	    cfsp->runcreds->uid = -1;
	    cfsp->runcreds->gid = -1;
	    if (cfsp->runcreds->uname != NULL) {
		pp = getpwnam(cfsp->runcreds->uname);
		if (pp == NULL)
		    errx(1, "can't find ID for the user: %s", cfsp->runcreds->uname);
		cfsp->runcreds->uid = pp->pw_uid;
		if (cfsp->runcreds->gname == NULL)
		    cfsp->runcreds->gid = pp->pw_gid;
	    }
	    if (cfsp->runcreds->gname != NULL) {
		gp = getgrnam(cfsp->runcreds->gname);
		if (gp == NULL)
		    errx(1, "can't find ID for the group: %s", cfsp->runcreds->gname);
		cfsp->runcreds->gid = gp->gr_gid;
                if (cfsp->runcreds->sock_mode == 0) {
                    cfsp->runcreds->sock_mode = 0755;
                }
	    }
	    break;

	case 'w': {
            int sock_mode;

	    if (atoi_safe(optarg, &sock_mode))
                errx(1, "%s: socket mode argument is invalid", optarg);
            cfsp->runcreds->sock_mode = sock_mode;
	    break;
            }

	case 'F':
	    cfsp->no_check = 1;
	    break;

	case 'i':
	    cfsp->ttl_mode = TTL_INDEPENDENT;
	    break;

	case 'n':
	    if(strlen(optarg) == 0)
		errx(1, "timeout notification socket name too short");
            if (CALL_METHOD(cfsp->rtpp_tnset_cf, append, optarg,
              &errmsg) != 0) {
                errx(1, "error adding timeout notification: %s", errmsg);
            }
	    break;

	case 'P':
	    cfsp->record_pcap = 1;
	    break;

	case 'a':
	    cfsp->record_all = 1;
	    break;

	case 'd':
	    cp = strchr(optarg, ':');
	    if (cp != NULL) {
		cfsp->log_facility = rtpp_log_str2fac(cp + 1);
		if (cfsp->log_facility == -1)
		    errx(1, "%s: invalid log facility", cp + 1);
		*cp = '\0';
	    }
	    cfsp->log_level = rtpp_log_str2lvl(optarg);
	    if (cfsp->log_level == -1)
		errx(1, "%s: invalid log level", optarg);
            CALL_METHOD(cfsp->glog, setlevel, cfsp->log_level);
	    break;

	case 'V':
	    printf("%s\n", RTPP_SW_VERSION);
	    init_config_bail(cfsp, 0, NULL, 0);
	    break;

        case 'W':
            if (atoi_safe(optarg, &cfsp->max_setup_ttl))
                errx(1, "%s: max setup TTL argument is invalid", optarg);
            break;

        case 'b':
            cfsp->seq_ports = 1;
            break;

        case 'D':
	    cfsp->no_chdir = 1;
	    break;

        case 'C':
	    printf("%s\n", get_mclock_name());
	    init_config_bail(cfsp, 0, NULL, 0);
	    break;

	case '?':
	default:
	    usage();
	}
    }

    if (optind != argc) {
       warnx("%d extra unhandled argument%s at the end of the command line",
         argc - optind, (argc - optind) > 1 ? "s" : "");
       usage();
    }

    if (cfsp->cfile != NULL) {
        if (rtpp_cfile_process(cfsp) < 0) {
            init_config_bail(cfsp, 1, "rtpp_cfile_process() failed", 1);
        }
    }

    if (cfsp->bmode != 0 && brsym != 0) {
        cfsp->aforce = 0;
    }

    if (cfsp->max_setup_ttl == 0) {
        cfsp->max_setup_ttl = cfsp->max_ttl;
    }

    /* No control socket has been specified, add a default one */
    if (RTPP_LIST_IS_EMPTY(cfsp->ctrl_socks)) {
        ctrl_sock = rtpp_ctrl_sock_parse(CMD_SOCK);
        if (ctrl_sock == NULL) {
            errx(1, "can't parse control socket: \"%s\"", CMD_SOCK);
        }
        rtpp_list_append(cfsp->ctrl_socks, ctrl_sock);
    }

    if (cfsp->rdir == NULL && cfsp->sdir != NULL)
	errx(1, "-S switch requires -r switch");

    if (cfsp->nodaemon == 0 && stdio_mode != 0)
        errx(1, "stdio command mode requires -f switch");

    if (cfsp->no_check == 0 && getuid() == 0 && cfsp->runcreds->uname == NULL) {
	if (umode != 0) {
	    errx(1, "running this program as superuser in a remote control "
	      "mode is strongly not recommended, as it poses serious security "
	      "threat to your system. Use -u option to run as an unprivileged "
	      "user or -F is you want to run as a superuser anyway.");
	} else {
	    warnx("WARNING!!! Running this program as superuser is strongly "
	      "not recommended, as it may pose serious security threat to "
	      "your system. Use -u option to run as an unprivileged user "
	      "or -F to surpress this warning.");
	}
    }

    /* make sure that port_min and port_max are even */
    if ((cfsp->port_min % 2) != 0)
	cfsp->port_min++;
    if ((cfsp->port_max % 2) != 0) {
	cfsp->port_max--;
    } else {
	/*
	 * If port_max is already even then there is no
	 * "room" for the RTCP port, go back by two ports.
	 */
	cfsp->port_max -= 2;
    }

    if (!IS_VALID_PORT(cfsp->port_min))
	errx(1, "invalid value of the port_min argument, "
	  "not in the range 1-65535");
    if (!IS_VALID_PORT(cfsp->port_max))
	errx(1, "invalid value of the port_max argument, "
	  "not in the range 1-65535");
    if (cfsp->port_min > cfsp->port_max)
	errx(1, "port_min should be less than port_max");

    if (bh[0] == NULL && bh[1] == NULL && bh6[0] == NULL && bh6[1] == NULL) {
	bh[0] = "*";
    }

    for (i = 0; i < 2; i++) {
	if (bh[i] != NULL && *bh[i] == '\0')
	    bh[i] = NULL;
	if (bh6[i] != NULL && *bh6[i] == '\0')
	    bh6[i] = NULL;
    }

    i = ((bh[0] == NULL) ? 0 : 1) + ((bh[1] == NULL) ? 0 : 1) +
      ((bh6[0] == NULL) ? 0 : 1) + ((bh6[1] == NULL) ? 0 : 1);
    if (cfsp->bmode != 0) {
	if (bh[0] != NULL && bh6[0] != NULL)
	    errx(1, "either IPv4 or IPv6 should be configured for external "
	      "interface in bridging mode, not both");
	if (bh[1] != NULL && bh6[1] != NULL)
	    errx(1, "either IPv4 or IPv6 should be configured for internal "
	      "interface in bridging mode, not both");
    if (cfsp->advaddr[0] != NULL && cfsp->advaddr[1] == NULL)
        errx(1, "two advertised addresses are required for internal "
          "and external interfaces in bridging mode");
	if (i != 2)
	    errx(1, "incomplete configuration of the bridging mode - exactly "
	      "2 listen addresses required, %d provided", i);
    } else if (i != 1) {
	errx(1, "exactly 1 listen addresses required, %d provided", i);
    }

    for (i = 0; i < 2; i++) {
	cfsp->bindaddr[i] = NULL;
	if (bh[i] != NULL) {
	    cfsp->bindaddr[i] = CALL_METHOD(cfsp->bindaddrs_cf,
              host2, bh[i], AF_INET, &errmsg);
	    if (cfsp->bindaddr[i] == NULL)
		errx(1, "host2bindaddr: %s", errmsg);
	    continue;
	}
	if (bh6[i] != NULL) {
	    cfsp->bindaddr[i] = CALL_METHOD(cfsp->bindaddrs_cf,
              host2, bh6[i], AF_INET6, &errmsg);
	    if (cfsp->bindaddr[i] == NULL)
		errx(1, "host2bindaddr: %s", errmsg);
	    continue;
	}
    }
    if (cfsp->bindaddr[0] == NULL) {
	cfsp->bindaddr[0] = cfsp->bindaddr[1];
	cfsp->bindaddr[1] = NULL;
    }
}

static enum rtpp_timed_cb_rvals
update_derived_stats(double dtime, void *argp)
{
    struct rtpp_stats *rtpp_stats;

    rtpp_stats = (struct rtpp_stats *)argp;
    CALL_SMETHOD(rtpp_stats, update_derived, dtime);
    return (CB_MORE);
}

int
main(int argc, char **argv)
{
    int i, len;
    long long ncycles_ref, counter;
    struct rtpp_cfg_stable cfs;
    char buf[256];
    struct sched_param sparam;
    void *elp;
    struct rtpp_timed_task *tp;
#if RTPP_DEBUG_timers
    double sleep_time, filter_lastval;
#endif
    struct rtpp_module_if *mif, *tmp;

#ifdef RTPP_CHECK_LEAKS
    RTPP_MEMDEB_APP_INIT();
#endif
    if (getdtime() == -1) {
        err(1, "timer self-test has failed: please check your build configuration");
        /* NOTREACHED */
    }

#ifdef RTPP_CHECK_LEAKS
    if (rtpp_memdeb_selftest(MEMDEB_SYM) != 0) {
        errx(1, "MEMDEB self-test has failed");
        /* NOTREACHED */
    }
    rtpp_memdeb_approve(MEMDEB_SYM, "addr2bindaddr", 100, "Too busy to fix now");
#endif

    memset(&cfs, 0, sizeof(cfs));

    cfs.ctrl_socks = rtpp_zmalloc(sizeof(struct rtpp_list));
    if (cfs.ctrl_socks == NULL) {
         err(1, "can't allocate memory for the struct ctrl_socks");
         /* NOTREACHED */
    }

    cfs.modules_cf = rtpp_zmalloc(sizeof(struct rtpp_list));
    if (cfs.modules_cf == NULL) {
         err(1, "can't allocate memory for the struct modules_cf");
         /* NOTREACHED */
    }

    cfs.runcreds = rtpp_zmalloc(sizeof(struct rtpp_runcreds));
    if (cfs.runcreds == NULL) {
         err(1, "can't allocate memory for the struct runcreds");
         /* NOTREACHED */
    }

    seedrandom();
    if (rtpp_gen_uid_init() != 0) {
        err(1, "rtpp_gen_uid_init() failed");
        /* NOTREACHED */
    }

    cfs.glog = rtpp_log_ctor("rtpproxy", NULL, LF_REOPEN);
    if (cfs.glog == NULL) {
        err(1, "can't initialize logging subsystem");
        /* NOTREACHED */
    }
 #ifdef RTPP_CHECK_LEAKS
    rtpp_memdeb_setlog(MEMDEB_SYM, cfs.glog);
 #endif

    _sig_cf = &cfs;
    atexit(rtpp_glog_fin);

    init_config(&cfs, argc, argv);

    cfs.sessions_ht = rtpp_hash_table_ctor(rtpp_ht_key_str_t, 0);
    if (cfs.sessions_ht == NULL) {
        err(1, "can't allocate memory for the hash table");
         /* NOTREACHED */
    }
    cfs.sessions_wrt = rtpp_weakref_ctor();
    if (cfs.sessions_wrt == NULL) {
        err(1, "can't allocate memory for the sessions weakref table");
         /* NOTREACHED */
    }
    cfs.rtp_streams_wrt = rtpp_weakref_ctor();
    if (cfs.rtp_streams_wrt == NULL) {
        err(1, "can't allocate memory for the RTP streams weakref table");
         /* NOTREACHED */
    }
    cfs.rtcp_streams_wrt = rtpp_weakref_ctor();
    if (cfs.rtcp_streams_wrt == NULL) {
        err(1, "can't allocate memory for the RTCP streams weakref table");
         /* NOTREACHED */
    }
    cfs.servers_wrt = rtpp_weakref_ctor();
    if (cfs.servers_wrt == NULL) {
        err(1, "can't allocate memory for the servers weakref table");
         /* NOTREACHED */
    }
    cfs.sessinfo = rtpp_sessinfo_ctor(&cfs);
    if (cfs.sessinfo == NULL) {
        errx(1, "cannot construct rtpp_sessinfo structure");
    }

    cfs.rtpp_stats = rtpp_stats_ctor();
    if (cfs.rtpp_stats == NULL) {
        err(1, "can't allocate memory for the stats data");
         /* NOTREACHED */
    }

    for (i = 0; i <= RTPP_PT_MAX; i++) {
        cfs.port_table[i] = rtpp_port_table_ctor(cfs.port_min,
          cfs.port_max, cfs.seq_ports, cfs.port_ctl);
        if (cfs.port_table[i] == NULL) {
            err(1, "can't allocate memory for the ports data");
            /* NOTREACHED */
        }
    }

    if (rtpp_controlfd_init(&cfs) != 0) {
        err(1, "can't initialize control socket%s",
          cfs.ctrl_socks->len > 1 ? "s" : "");
    }

    if (cfs.nodaemon == 0) {
        if (cfs.no_chdir == 0) {
            cfs.cwd_orig = getcwd(NULL, 0);
            if (cfs.cwd_orig == NULL) {
                err(1, "getcwd");
            }
        }
	if (rtpp_daemon(cfs.no_chdir, 0) == -1)
	    err(1, "can't switch into daemon mode");
	    /* NOTREACHED */
    }

    if (CALL_METHOD(cfs.glog, start, &cfs) != 0) {
        /* We cannot possibly function with broken logs, bail out */
        syslog(LOG_CRIT, "rtpproxy pid %d has failed to initialize logging"
            " facilities: crash", getpid());
        err(1, "rtpproxy has failed to initialize logging facilities");
    }

    atexit(ehandler);
    RTPP_LOG(cfs.glog, RTPP_LOG_INFO, "rtpproxy started, pid %d", getpid());

#ifdef RTPP_CHECK_LEAKS
    rtpp_memdeb_setbaseln(MEMDEB_SYM);
#endif

    i = open(cfs.pid_file, O_WRONLY | O_CREAT | O_TRUNC, DEFFILEMODE);
    if (i >= 0) {
	len = sprintf(buf, "%u\n", (unsigned int)getpid());
	write(i, buf, len);
	close(i);
    } else {
	RTPP_ELOG(cfs.glog, RTPP_LOG_ERR, "can't open pidfile for writing");
    }

    if (cfs.sched_policy != SCHED_OTHER) {
        sparam.sched_priority = sched_get_priority_max(cfs.sched_policy);
        if (sched_setscheduler(0, cfs.sched_policy, &sparam) == -1) {
            RTPP_ELOG(cfs.glog, RTPP_LOG_ERR, "sched_setscheduler(SCHED_%s, %d)",
              (cfs.sched_policy == SCHED_FIFO) ? "FIFO" : "RR", sparam.sched_priority);
        }
    }
    if (cfs.sched_nice != PRIO_UNSET) {
        if (setpriority(PRIO_PROCESS, 0, cfs.sched_nice) == -1) {
            RTPP_ELOG(cfs.glog, RTPP_LOG_ERR, "can't set scheduling "
              "priority to %d", cfs.sched_nice);
            exit(1);
        }
    }

    if (cfs.runcreds->uname != NULL || cfs.runcreds->gname != NULL) {
	if (drop_privileges(&cfs) != 0) {
	    RTPP_ELOG(cfs.glog, RTPP_LOG_ERR,
	      "can't switch to requested user/group");
	    exit(1);
	}
    }
    set_rlimits(&cfs);

    cfs.rtpp_proc_cf = rtpp_proc_async_ctor(&cfs);
    if (cfs.rtpp_proc_cf == NULL) {
        RTPP_LOG(cfs.glog, RTPP_LOG_ERR,
          "can't init RTP processing subsystem");
        exit(1);
    }

    cfs.rtpp_proc_ttl_cf = rtpp_proc_ttl_ctor(&cfs);
    if (cfs.rtpp_proc_ttl_cf == NULL) {
        RTPP_LOG(cfs.glog, RTPP_LOG_ERR,
          "can't init TTL processing subsystem");
        exit(1);
    }

    counter = 0;

    cfs.rtpp_timed_cf = rtpp_timed_ctor(0.1);
    if (cfs.rtpp_timed_cf == NULL) {
        RTPP_ELOG(cfs.glog, RTPP_LOG_ERR,
          "can't init scheduling subsystem");
        exit(1);
    }

    tp = CALL_SMETHOD(cfs.rtpp_timed_cf, schedule_rc, 1.0,
      cfs.rtpp_stats->rcnt, update_derived_stats, NULL,
      cfs.rtpp_stats);
    if (tp == NULL) {
        RTPP_ELOG(cfs.glog, RTPP_LOG_ERR,
          "can't schedule notification to derive stats");
        exit(1);
    }
    CALL_SMETHOD(tp->rcnt, decref);

    cfs.rtpp_notify_cf = rtpp_notify_ctor(cfs.glog);
    if (cfs.rtpp_notify_cf == NULL) {
        RTPP_ELOG(cfs.glog, RTPP_LOG_ERR,
          "can't init timeout notification subsystem");
        exit(1);
    }

    cfs.observers = rtpp_po_mgr_ctor();
    if (cfs.observers == NULL) {
        RTPP_LOG(cfs.glog, RTPP_LOG_ERR,
          "can't init packet inspection subsystem");
        exit(1);
    }

#if ENABLE_MODULE_IF
    if (!RTPP_LIST_IS_EMPTY(cfs.modules_cf)) {
        mif = RTPP_LIST_HEAD(cfs.modules_cf);
        if (CALL_METHOD(mif, start, &cfs) != 0) {
            RTPP_ELOG(cfs.glog, RTPP_LOG_ERR,
              "%p: dymanic module start has failed", mif);
            exit(1);
        }
    }
#endif

    cfs.rtpp_cmd_cf = rtpp_command_async_ctor(&cfs);
    if (cfs.rtpp_cmd_cf == NULL) {
        RTPP_ELOG(cfs.glog, RTPP_LOG_ERR,
          "can't init command processing subsystem");
        exit(1);
    }

    signal(SIGHUP, sighup);
    signal(SIGINT, fatsignal);
    signal(SIGKILL, fatsignal);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, fatsignal);
    signal(SIGXCPU, fatsignal);
    signal(SIGXFSZ, fatsignal);
    signal(SIGVTALRM, fatsignal);
    signal(SIGPROF, fatsignal);
    signal(SIGUSR1, fatsignal);
    signal(SIGUSR2, fatsignal);
#if RTPP_DEBUG_catchtrace
    signal(SIGQUIT, rtpp_stacktrace);
    signal(SIGILL, rtpp_stacktrace);
    signal(SIGTRAP, rtpp_stacktrace);
    signal(SIGABRT, rtpp_stacktrace);
#if defined(SIGEMT)
    signal(SIGEMT, rtpp_stacktrace);
#endif
    signal(SIGFPE, rtpp_stacktrace);
    signal(SIGBUS, rtpp_stacktrace);
    signal(SIGSEGV, rtpp_stacktrace);
    signal(SIGSYS, rtpp_stacktrace);
#endif

#ifdef HAVE_SYSTEMD_DAEMON
    sd_notify(0, "READY=1");
#endif

    elp = prdic_init(cfs.target_pfreq / 10.0, cfs.sched_offset);
    for (;;) {
        ncycles_ref = (long long)prdic_getncycles_ref(elp);

        CALL_METHOD(cfs.rtpp_cmd_cf, wakeup);
        if (cfs.fastshutdown != 0) {
            break;
        }
        if (cfs.slowshutdown != 0 &&
          CALL_METHOD(cfs.sessions_wrt, get_length) == 0) {
            RTPP_LOG(cfs.glog, RTPP_LOG_INFO,
              "deorbiting-burn sequence completed, exiting");
            break;
        }
        prdic_procrastinate(elp);
        counter++;
    }
    prdic_free(elp);

    CALL_METHOD(cfs.rtpp_cmd_cf, dtor);
#if ENABLE_MODULE_IF
    for (mif = RTPP_LIST_HEAD(cfs.modules_cf); mif != NULL; mif = tmp) {
        tmp = RTPP_ITER_NEXT(mif);
        CALL_SMETHOD(mif->rcnt, decref);
    }
#endif
    CALL_SMETHOD(cfs.observers->rcnt, decref);
    free(cfs.modules_cf);
    free(cfs.runcreds);
    CALL_METHOD(cfs.rtpp_notify_cf, dtor);
    CALL_METHOD(cfs.bindaddrs_cf, dtor);
    free(cfs.locks);
    CALL_METHOD(cfs.rtpp_tnset_cf, dtor);
    CALL_SMETHOD(cfs.rtpp_timed_cf, shutdown);
    CALL_SMETHOD(cfs.rtpp_timed_cf->rcnt, decref);
    CALL_METHOD(cfs.rtpp_proc_cf, dtor);
    CALL_METHOD(cfs.rtpp_proc_ttl_cf, dtor);
    CALL_SMETHOD(cfs.sessinfo->rcnt, decref);
    CALL_SMETHOD(cfs.rtpp_stats->rcnt, decref);
    for (i = 0; i <= RTPP_PT_MAX; i++) {
        CALL_SMETHOD(cfs.port_table[i]->rcnt, decref);
    }
#ifdef HAVE_SYSTEMD_DAEMON
    sd_notify(0, "STATUS=Exited");
#endif

    rtpp_exit(1, 0);
}
