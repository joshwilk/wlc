/*****************************************************************************
 * wlc.h: WLC Control Center - shared declarations
 *****************************************************************************
 * Copyright (C) 2026 Josh Wilk
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/
#ifndef WLC_CONTROL_H
#define WLC_CONTROL_H

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdatomic.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <vlc_common.h>
#include <vlc_interface.h>
#include <vlc_playlist.h>
#include <vlc_input.h>
#include <vlc_memstream.h>
#include <vlc_network.h>

#include "../../misc/webservices/json.h"

#define WLC_VERSION      "0.2.0"
#define WLC_DEFAULT_PORT 48620
#define WLC_CAST_PORT    48621
#define WLC_CAST_GROUP   "239.87.76.67"   /* 'W' 'L' 'C' */
#define WLC_MAX_ROOTS    64
#define WLC_MAX_PEERS    32
#define WLC_MAX_SESS     32
#define WLC_MAX_GRANTS   1024
#define WLC_MAX_SCREENS  8
#define WLC_MAX_QUEUE    500
#define WLC_SCREEN_CMDS  16
#define WLC_WORKERS      8
#define WLC_MAX_BODY     (8 * 1024 * 1024)
#define WLC_MAX_LIB      50000

typedef struct
{
    char    id[40];
    char    name[64];
    char    ip[64];
    char    auth[8];
    int     port;
    mtime_t seen;
    bool    manual;
    bool    own;        /* a browser screen hosted by this WLC */
    char    base[64];   /* path prefix of its WilkCast API, "/cast/v1/" */
    char    kind[12];   /* "wlc", "browser", or what the device says */
} wlc_peer_t;

/* A web browser (a TV, a phone, an old laptop) that opened /screen on this
 * WLC and signed in with the PIN. It is announced and reachable as its own
 * WilkCast receiver; it fetches its commands from us twice a second. */
typedef struct
{
    char    id[40];
    char    name[64];
    char    host[64];       /* the address the browser reaches us by */
    mtime_t seen;
    char   *cmds[WLC_SCREEN_CMDS];
    unsigned ncmds;
    /* last status the screen reported */
    char    state[12];
    double  time, length, volume;
    char    title[256];
    char    error[160];
} wlc_screen_t;

enum { WLC_JOB_QUEUED, WLC_JOB_RUNNING, WLC_JOB_DONE, WLC_JOB_ERROR,
       WLC_JOB_CANCELLED };

typedef struct
{
    int             id;
    int             state;
    float           progress;
    char           *src;
    char           *dst;    /* final name, decided when the job finishes */
    char           *part;   /* temporary output */
    char           *label;
    char           *chain;  /* sout chain without the output path */
    char           *mux;
    char           *msg;
    bool            audio_only;
    bool            cancel;
    input_thread_t *input;
} wlc_job_t;

typedef struct
{
    char   *path;
    int64_t size;
    int64_t mtime;
    char    kind;   /* 'v'ideo 'a'udio 'i'mage */
} wlc_entry_t;

typedef struct
{
    int     id;
    int64_t when;
    bool    undone;
    size_t  n;
    char  **from;
    char  **to;
} wlc_batch_t;

struct intf_sys_t
{
    intf_thread_t *intf;
    playlist_t    *pl;
    vlc_mutex_t    lock;
    atomic_bool    stop;
    char          *cfg_dir;
    bool           first_run;

    /* http server */
    int          lfd;
    int          port;
    vlc_thread_t workers[WLC_WORKERS];
    unsigned     nworkers;

    /* identity and access */
    char     id[40];
    char     name[64];
    char     pin[8];
    char     media_key[33];
    bool     lan;
    char     cast_accept[8];   /* "pin", "open" or "off" */
    int      fail_count;
    mtime_t  fail_until;
    char     sessions[WLC_MAX_SESS][33];
    unsigned sess_next;
    char    *roots[WLC_MAX_ROOTS];
    unsigned nroots;
    char    *grants[WLC_MAX_GRANTS];
    unsigned grant_next;

    /* equalizer */
    bool  eq_on, eq_limiter;
    float eq_preamp, eq_balance, eq_width;
    char *eq_graphic, *eq_param;

    /* library */
    wlc_entry_t *lib;
    size_t       nlib;
    bool         lib_valid;
    bool         lib_truncated;

    /* converter */
    wlc_job_t  **jobs;
    size_t       njobs;
    int          job_seq;
    int          parallel;
    vlc_thread_t job_thread;
    bool         job_thread_ok;

    /* renamer */
    wlc_batch_t *batches;
    size_t       nbatches;
    int          batch_seq;

    /* casting */
    int          ufd;
    vlc_thread_t cast_thread;
    bool         cast_thread_ok;
    wlc_peer_t   peers[WLC_MAX_PEERS];
    unsigned     npeers;
    wlc_screen_t screens[WLC_MAX_SCREENS];
    unsigned     nscreens;
};

typedef struct
{
    intf_sys_t *sys;
    int         fd;
    bool        local;    /* request comes from this computer */
    bool        authed;   /* local, or remote with a valid session */
    char        peer_ip[64];
    char        my_ip[64];
    char        method[8];
    char       *path;
    char       *query;
    char        host[160], origin[200], cookie[512], pin[32], range[80];
    char       *body;
    size_t      body_len;
    json_value *json;
} wlc_req_t;

/* wlc.c */
void  wlc_save( intf_sys_t * );                 /* lock must be held */
char *wlc_cfg_path( intf_sys_t *, const char *name );
char *wlc_read_file( const char *path, size_t max, size_t *len );
int   wlc_write_file( const char *path, const void *data, size_t len );

/* wlc_http.c */
int   wlc_http_start( intf_sys_t * );
void  wlc_http_stop( intf_sys_t * );
void  wlc_send( wlc_req_t *, int code, const char *ctype, const char *extra,
                const void *body, size_t len );
void  wlc_send_json( wlc_req_t *, int code, struct vlc_memstream * );
void  wlc_error( wlc_req_t *, int code, const char *msg );
void  wlc_ok( wlc_req_t * );
void  wlc_json_str( struct vlc_memstream *, const char * );
char *wlc_qparam( wlc_req_t *, const char *name );
const json_value *wlc_jget( const json_value *, const char *key );
const char *wlc_jstr( const json_value *, const char *key );
double wlc_jnum( const json_value *, const char *key, double def );
bool  wlc_jbool( const json_value *, const char *key, bool def );
char *wlc_realpath( const char *path );
bool  wlc_path_shared( intf_sys_t *, const char *real );
bool  wlc_path_ok( wlc_req_t *, const char *real );
char  wlc_kind( const char *name );
const char *wlc_mime( const char *name );
void  wlc_serve_file( wlc_req_t *, const char *real );
void  wlc_new_token( char out[33] );
void  wlc_local_addrs( struct vlc_memstream * );  /* JSON array of IPv4 */
int   wlc_http_request( intf_sys_t *, const char *host, int port,
                        const char *method, const char *path,
                        const char *pin, const char *body,
                        char **resp, char my_ip[64] );

/* wlc_api.c */
void wlc_status_json( intf_sys_t *, struct vlc_memstream * );
int  wlc_player_cmd( wlc_req_t *, const char *cmd, const json_value *args,
                     const char **err );
void wlc_h_status( wlc_req_t * );
void wlc_h_cmd( wlc_req_t * );
void wlc_h_playlist( wlc_req_t * );
void wlc_h_browse( wlc_req_t * );
void wlc_h_library( wlc_req_t * );
void wlc_h_meta( wlc_req_t * );
void wlc_h_art( wlc_req_t * );
void wlc_h_roots( wlc_req_t * );
void wlc_h_rename( wlc_req_t * );
void wlc_h_rename_undo( wlc_req_t * );
void wlc_h_rename_history( wlc_req_t * );
void wlc_h_eq_get( wlc_req_t * );
void wlc_h_eq_set( wlc_req_t * );
void wlc_eq_apply( intf_sys_t * );             /* lock must be held */
void wlc_rename_load( intf_sys_t * );
void wlc_api_cleanup( intf_sys_t * );

/* wlc_jobs.c */
int  wlc_jobs_start( intf_sys_t * );
void wlc_jobs_stop( intf_sys_t * );
void wlc_h_convert( wlc_req_t * );
void wlc_h_jobs( wlc_req_t * );
void wlc_h_jobs_cancel( wlc_req_t * );
void wlc_h_profiles( wlc_req_t * );

/* wlc_cast.c */
int  wlc_cast_start( intf_sys_t * );
void wlc_cast_stop( intf_sys_t * );
void wlc_h_peers( wlc_req_t * );
void wlc_h_peer_add( wlc_req_t * );
void wlc_h_cast_send( wlc_req_t * );
void wlc_h_cast_control( wlc_req_t * );
void wlc_h_cast_peer_status( wlc_req_t * );
void wlc_h_castv1( wlc_req_t *, const char *what );
void wlc_h_screen_register( wlc_req_t * );
void wlc_h_screen_sync( wlc_req_t * );
void wlc_screens_free( intf_sys_t * );

#endif
