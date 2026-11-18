/*****************************************************************************
 * wlc_dl.c: WLC Control Center - downloads with yt-dlp
 *****************************************************************************
 * Copyright (C) 2026 Josh Wilkinson
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Runs yt-dlp (https://github.com/yt-dlp/yt-dlp) as a separate program, one
 * thread per download, never through a shell. yt-dlp writes into a private
 * folder; WLC then joins split video/audio and converts audio with its own
 * engine when ffmpeg is not installed, and moves the results into place
 * without ever overwriting a file. This computer only (see wlc_http.c).
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "wlc.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#ifndef _WIN32
# include <spawn.h>
# include <sys/wait.h>
#endif

#include <vlc_fs.h>
#include <vlc_url.h>
#include <vlc_configuration.h>

#ifndef _WIN32
extern char **environ;
#endif

enum { DL_QUEUED, DL_RUNNING, DL_PROCESSING, DL_DONE, DL_ERROR, DL_CANCELLED };
static const char *const dl_states[] = { "queued", "downloading", "processing",
                                         "done", "error", "cancelled" };
#define DL_PARALLEL 2
#define DL_MAX_FILES 1000

struct wlc_dl
{
    int     id;
    char   *url, *title, *mode, *quality, *fmt, *items, *outdir, *tmpdir;
    bool    subs, playlist;
    int     state;
    double  progress, speed, eta;
    int     item, nitems;
    char   *cur, *msg;
    char  **files;
    size_t  nfiles;
    pid_t   pid;
    atomic_bool cancel;
    float   sub_progress;           /* while WLC joins/converts */
    vlc_thread_t thread;
    bool    thread_ok;
    atomic_bool finished;
    intf_sys_t *sys;
};
typedef struct wlc_dl wlc_dl_t;

/*****************************************************************************
 * SHA-256 (to check the yt-dlp download against its published checksums)
 *****************************************************************************/
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t n; } sha256_t;

static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };

#define ROR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block( sha256_t *s, const uint8_t *p )
{
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for( int i = 0; i < 16; i++ )
        w[i] = (uint32_t)p[4*i] << 24 | (uint32_t)p[4*i+1] << 16 | (uint32_t)p[4*i+2] << 8 | p[4*i+3];
    for( int i = 16; i < 64; i++ )
    {
        uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
    e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for( int i = 0; i < 64; i++ )
    {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static bool sha256_file( const char *path, char hex[65] )
{
    sha256_t s = { { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                     0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 }, 0, {0}, 0 };
    int fd = vlc_open( path, O_RDONLY );
    if( fd == -1 )
        return false;
    uint8_t buf[65536];
    ssize_t r;
    while( (r = read( fd, buf, sizeof(buf) )) > 0 )
        for( ssize_t i = 0; i < r; i++ )
        {
            s.buf[s.n++] = buf[i];
            if( s.n == 64 ) { sha256_block( &s, s.buf ); s.n = 0; }
            s.len += 8;
        }
    close( fd );
    if( r < 0 )
        return false;
    uint64_t bits = s.len;
    s.buf[s.n++] = 0x80;
    if( s.n > 56 ) { while( s.n < 64 ) s.buf[s.n++] = 0; sha256_block( &s, s.buf ); s.n = 0; }
    while( s.n < 56 ) s.buf[s.n++] = 0;
    for( int i = 7; i >= 0; i-- ) s.buf[s.n++] = (uint8_t)(bits >> (8 * i));
    sha256_block( &s, s.buf );
    for( int i = 0; i < 8; i++ )
        snprintf( hex + 8 * i, 9, "%08x", s.h[i] );
    return true;
}

/*****************************************************************************
 * Finding the tools
 *****************************************************************************/
static bool is_exe( const char *p )
{
    struct stat st;
    return p != NULL && !vlc_stat( p, &st ) && S_ISREG( st.st_mode ) && !access( p, X_OK );
}

static char *bin_dir( intf_sys_t *sys )
{
    char *d;
    return asprintf( &d, "%s/bin", sys->cfg_dir ) < 0 ? NULL : d;
}

/* yt-dlp or ffmpeg: WLC's own copy first, then the usual places.
 * WLC_YTDLP / WLC_FFMPEG override ("none" = pretend it is not there). */
static char *find_tool( intf_sys_t *sys, const char *name )
{
    const char *env = getenv( !strcmp( name, "ffmpeg" ) ? "WLC_FFMPEG" : "WLC_YTDLP" );
    if( env != NULL && env[0] != '\0' )
        return strcmp( env, "none" ) && is_exe( env ) ? strdup( env ) : NULL;
    char *own = bin_dir( sys ), *p = NULL;
    if( own != NULL && asprintf( &p, "%s/%s", own, name ) >= 0 && is_exe( p ) )
    {
        free( own );
        return p;
    }
    free( p );
    free( own );
    static const char *const dirs[] = { "/opt/homebrew/bin", "/usr/local/bin", "/usr/bin" };
    for( size_t i = 0; i < ARRAY_SIZE(dirs); i++ )
    {
        if( asprintf( &p, "%s/%s", dirs[i], name ) >= 0 && is_exe( p ) )
            return p;
        free( p );
    }
    const char *home = getenv( "HOME" );
    if( home != NULL && asprintf( &p, "%s/.local/bin/%s", home, name ) >= 0 && is_exe( p ) )
        return p;
    free( p );
    return NULL;
}

/*****************************************************************************
 * Running programs (no shell; stdout+stderr into one pipe)
 *****************************************************************************/
#ifndef _WIN32
static char **child_env( intf_sys_t *sys )
{
    size_t n = 0;
    while( environ[n] != NULL ) n++;
    char **env = calloc( n + 3, sizeof(char *) );
    if( env == NULL )
        return NULL;
    char *own = bin_dir( sys );
    const char *path = getenv( "PATH" );
    size_t k = 0;
    /* apps started from the Dock get a bare PATH; add the usual places */
    if( asprintf( &env[k], "PATH=%s:/opt/homebrew/bin:/usr/local/bin:%s",
                  own ? own : "/nonexistent", path ? path : "/usr/bin:/bin" ) < 0 )
        env[k] = NULL;
    else
        k++;
    env[k++] = strdup( "PYTHONUNBUFFERED=1" );
    for( size_t i = 0; i < n; i++ )
        if( strncmp( environ[i], "PATH=", 5 ) && strncmp( environ[i], "PYTHONUNBUFFERED=", 17 ) )
            env[k++] = strdup( environ[i] );
    free( own );
    return env;
}

static void free_env( char **env )
{
    for( size_t i = 0; env != NULL && env[i] != NULL; i++ )
        free( env[i] );
    free( env );
}

/* Starts argv[0] in its own process group; *fd reads its output. */
static pid_t spawn( intf_sys_t *sys, char *const argv[], int *fd )
{
    int p[2];
    if( vlc_pipe( p ) )
        return -1;
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    posix_spawn_file_actions_init( &fa );
    posix_spawn_file_actions_addopen( &fa, 0, "/dev/null", O_RDONLY, 0 );
    posix_spawn_file_actions_adddup2( &fa, p[1], 1 );
    posix_spawn_file_actions_adddup2( &fa, p[1], 2 );
    posix_spawnattr_init( &at );
    /* VLC blocks and ignores signals for its own threads; the child must not
     * inherit that, or it could never be stopped */
    sigset_t none, dflt;
    sigemptyset( &none );
    sigemptyset( &dflt );
    static const int sigs[] = { SIGTERM, SIGINT, SIGHUP, SIGQUIT, SIGPIPE, SIGCHLD, SIGALRM };
    for( size_t i = 0; i < ARRAY_SIZE(sigs); i++ )
        sigaddset( &dflt, sigs[i] );
    posix_spawnattr_setsigmask( &at, &none );
    posix_spawnattr_setsigdefault( &at, &dflt );
    short flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;   /* macOS: pass on only 0, 1, 2 */
#endif
    posix_spawnattr_setflags( &at, flags );
    posix_spawnattr_setpgroup( &at, 0 );
    char **env = child_env( sys );
    pid_t pid = -1;
    if( env == NULL || posix_spawn( &pid, argv[0], &fa, &at, argv, env ) )
        pid = -1;
    free_env( env );
    posix_spawn_file_actions_destroy( &fa );
    posix_spawnattr_destroy( &at );
    close( p[1] );
    if( pid == -1 )
    {
        close( p[0] );
        return -1;
    }
    *fd = p[0];
    return pid;
}

static void kill_group( pid_t pid )
{
    if( pid > 0 )
        kill( -pid, SIGTERM );
}

static int reap( pid_t pid )
{
    int status = 0;
    while( waitpid( pid, &status, 0 ) == -1 && errno == EINTR )
        ;
    return WIFEXITED( status ) ? WEXITSTATUS( status ) : 128 + WTERMSIG( status );
}

/* Runs a program to the end (or the timeout) and returns its output
 * (NUL-terminated, at most max bytes kept). */
static int run_capture( intf_sys_t *sys, char *const argv[], char **out,
                        size_t max, int timeout_s )
{
    int fd;
    *out = NULL;
    pid_t pid = spawn( sys, argv, &fd );
    if( pid == -1 )
        return -1;
    size_t len = 0, cap = 16384;
    char *data = malloc( cap );
    mtime_t deadline = mdate() + (mtime_t)timeout_s * CLOCK_FREQ;
    bool timed_out = false;
    while( data != NULL )
    {
        int left = (int)((deadline - mdate()) / 1000);
        if( left <= 0 || atomic_load( &sys->stop ) )
        {
            timed_out = true;
            break;
        }
        struct pollfd pf = { .fd = fd, .events = POLLIN };
        int r = poll( &pf, 1, left > 500 ? 500 : left );
        if( r < 0 && errno != EINTR )
            break;
        if( r <= 0 )
            continue;
        if( len + 1 >= cap && len < max )
        {
            char *n = realloc( data, cap * 2 );
            if( n == NULL )
                break;
            data = n;
            cap *= 2;
        }
        char sink[4096];
        ssize_t n = len < max ? read( fd, data + len, cap - 1 - len )
                              : read( fd, sink, sizeof(sink) );   /* over the limit: drain */
        if( n < 0 && errno == EINTR )
            continue;
        if( n <= 0 )
            break;
        if( len < max )
            len += n;
    }
    if( timed_out )
        kill_group( pid );
    close( fd );
    int code = reap( pid );
    if( data != NULL )
    {
        data[len] = '\0';
        *out = data;
    }
    return timed_out ? -2 : code;
}
#endif

/*****************************************************************************
 * Small file helpers
 *****************************************************************************/
static bool has_suffix( const char *s, const char *suf )
{
    size_t a = strlen( s ), b = strlen( suf );
    return a >= b && !strcasecmp( s + a - b, suf );
}

/* removes a folder WLC made, without following links out of it */
static void rm_tree( const char *dir )
{
    DIR *d = vlc_opendir( dir );
    if( d != NULL )
    {
        const char *name;
        while( (name = vlc_readdir( d )) != NULL )
        {
            if( !strcmp( name, "." ) || !strcmp( name, ".." ) )
                continue;
            char *p;
            if( asprintf( &p, "%s/%s", dir, name ) < 0 )
                continue;
            struct stat st;
            if( !vlc_lstat( p, &st ) && S_ISDIR( st.st_mode ) )
                rm_tree( p );
            else
                vlc_unlink( p );
            free( p );
        }
        closedir( d );
    }
    rmdir( dir );
}

/* every regular file under dir, as paths relative to it */
static void list_files( const char *dir, const char *rel, char ***out, size_t *n )
{
    char *full;
    if( asprintf( &full, "%s%s%s", dir, rel[0] ? "/" : "", rel ) < 0 )
        return;
    DIR *d = vlc_opendir( full );
    if( d != NULL )
    {
        const char *name;
        while( (name = vlc_readdir( d )) != NULL && *n < DL_MAX_FILES )
        {
            if( name[0] == '.' )
                continue;
            char *r, *p;
            if( asprintf( &r, "%s%s%s", rel, rel[0] ? "/" : "", name ) < 0 )
                continue;
            if( asprintf( &p, "%s/%s", full, name ) < 0 ) { free( r ); continue; }
            struct stat st;
            if( !vlc_lstat( p, &st ) && S_ISDIR( st.st_mode ) )
            {
                list_files( dir, r, out, n );
                free( r );
            }
            else if( !vlc_lstat( p, &st ) && S_ISREG( st.st_mode ) )
            {
                char **o = realloc( *out, (*n + 1) * sizeof(char *) );
                if( o != NULL ) { *out = o; o[(*n)++] = r; } else free( r );
            }
            else
                free( r );
            free( p );
        }
        closedir( d );
    }
    free( full );
}

static int cmp_str( const void *a, const void *b )
{
    return strcmp( *(char *const *)a, *(char *const *)b );
}

static void mkdirs( const char *path )
{
    char *p = strdup( path );
    for( char *s = p ? p + 1 : NULL; s != NULL && *s; s++ )
        if( *s == '/' )
        {
            *s = '\0';
            vlc_mkdir( p, 0755 );
            *s = '/';
        }
    if( p != NULL )
        vlc_mkdir( p, 0755 );
    free( p );
}

/* "Song.f251.webm" -> stem "Song", true; "Song.mp3" -> false */
static bool format_part( const char *rel, char **stem )
{
    const char *dot = strrchr( rel, '.' );
    if( dot == NULL || dot == rel )
        return false;
    const char *f = dot - 1;
    while( f > rel && *f != '.' && *f != '/' )
        f--;
    if( *f != '.' || f[1] != 'f' || f + 2 >= dot )
        return false;
    for( const char *c = f + 2; c < dot; c++ )
        if( !((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z')
           || (*c >= 'A' && *c <= 'Z') || *c == '-' || *c == '_') )
            return false;
    *stem = strndup( rel, f - rel );
    return *stem != NULL;
}

static const char *ext_of( const char *p )
{
    const char *slash = strrchr( p, '/' ), *dot = strrchr( p, '.' );
    return dot != NULL && (slash == NULL || dot > slash) ? dot + 1 : "";
}

static bool is_media( const char *rel )
{
    static const char *const skip[] = { ".part", ".ytdl", ".temp", ".json", ".description",
                                        ".jpg", ".jpeg", ".png", ".webp", ".vtt", ".srt",
                                        ".ass", ".lrc", ".txt" };
    for( size_t i = 0; i < ARRAY_SIZE(skip); i++ )
        if( has_suffix( rel, skip[i] ) )
            return false;
    return true;
}

/*****************************************************************************
 * One download
 *****************************************************************************/
static void dl_set( wlc_dl_t *dl, char **field, const char *v )
{
    vlc_mutex_lock( &dl->sys->lock );
    free( *field );
    *field = v ? strdup( v ) : NULL;
    vlc_mutex_unlock( &dl->sys->lock );
}

static void dl_add_file( wlc_dl_t *dl, const char *path )
{
    vlc_mutex_lock( &dl->sys->lock );
    char **f = realloc( dl->files, (dl->nfiles + 1) * sizeof(char *) );
    if( f != NULL )
    {
        dl->files = f;
        f[dl->nfiles] = strdup( path );
        if( f[dl->nfiles] != NULL )
            dl->nfiles++;
    }
    vlc_mutex_unlock( &dl->sys->lock );
}

static double num_or( const char *s, double def )
{
    if( s == NULL || !strcmp( s, "NA" ) || !strcmp( s, "None" ) )
        return def;
    char *end;
    double v = strtod( s, &end );
    return end == s || !isfinite( v ) ? def : v;
}

/* one line of yt-dlp's output */
static void dl_line( wlc_dl_t *dl, char *line )
{
    intf_sys_t *sys = dl->sys;
    if( !strncmp( line, "WLCP ", 5 ) )
    {   /* WLCP downloaded total estimate speed eta */
        char *f[5] = { 0 }, *save = NULL, *t = strtok_r( line + 5, " ", &save );
        for( int i = 0; i < 5 && t != NULL; i++, t = strtok_r( NULL, " ", &save ) )
            f[i] = t;
        double got = num_or( f[0], 0 ), tot = num_or( f[1], -1 );
        if( tot <= 0 )
            tot = num_or( f[2], -1 );
        vlc_mutex_lock( &sys->lock );
        double part = tot > 0 ? fmin( 1., got / tot ) : 0.;
        int n = dl->nitems > 0 ? dl->nitems : 1;
        int idx = dl->item > 0 ? dl->item - 1 : 0;
        dl->progress = fmin( 0.999, (idx + part) / n );
        dl->speed = num_or( f[3], 0 );
        dl->eta = num_or( f[4], -1 );
        vlc_mutex_unlock( &sys->lock );
    }
    else if( !strncmp( line, "WLCT ", 5 ) )
    {   /* WLCT index count title */
        char *save = NULL;
        char *i = strtok_r( line + 5, " ", &save );
        char *n = strtok_r( NULL, " ", &save );
        char *title = strtok_r( NULL, "", &save );
        vlc_mutex_lock( &sys->lock );
        dl->item = (int)num_or( i, 0 );
        dl->nitems = (int)num_or( n, 0 );
        free( dl->cur );
        dl->cur = title ? strdup( title ) : NULL;
        if( dl->title == NULL && title != NULL && dl->nitems <= 1 )
            dl->title = strdup( title );
        vlc_mutex_unlock( &sys->lock );
    }
    else if( !strncmp( line, "ERROR: ", 7 ) )
    {
        msg_Warn( sys->intf, "yt-dlp: %s", line + 7 );
        dl_set( dl, &dl->msg, line + 7 );
    }
}

static const char *audio_profile( const char *fmt )
{
    if( !strcmp( fmt, "mp3" ) ) return "mp3";
    if( !strcmp( fmt, "m4a" ) ) return "m4a";
    if( !strcmp( fmt, "opus" ) ) return "opus";
    if( !strcmp( fmt, "flac" ) ) return "flac";
    if( !strcmp( fmt, "wav" ) ) return "wav";
    return NULL;
}

/* After yt-dlp: join/convert as needed and move into dl->outdir. */
static void dl_finish_files( wlc_dl_t *dl, bool have_ffmpeg )
{
    intf_sys_t *sys = dl->sys;
    char **rel = NULL;
    size_t n = 0;
    list_files( dl->tmpdir, "", &rel, &n );
    qsort( rel, n, sizeof(char *), cmp_str );
    const bool audio = !strcmp( dl->mode, "audio" );
    size_t done = 0, media = 0;
    for( size_t i = 0; i < n; i++ )
        if( is_media( rel[i] ) )
            media++;

    for( size_t i = 0; i < n && !atomic_load( &dl->cancel ); i++ )
    {
        if( rel[i] == NULL )
            continue;
        char *src = NULL, *slave = NULL, *stem = NULL, *ext_out = NULL;
        const char *chain = "", *mux = NULL;
        char *own_chain = NULL;
        bool convert = false, audio_only = false;
        if( asprintf( &src, "%s/%s", dl->tmpdir, rel[i] ) < 0 )
            break;

        if( !is_media( rel[i] ) )
        {   /* subtitles go next to the video so WLC finds them; the rest is dropped */
            if( has_suffix( rel[i], ".vtt" ) || has_suffix( rel[i], ".srt" ) )
            {
                char *d = NULL;
                if( asprintf( &d, "%s/%s", dl->outdir, rel[i] ) >= 0 )
                {
                    char *slash = strrchr( d, '/' );
                    *slash = '\0';
                    mkdirs( d );
                    *slash = '/';
                    struct stat st;
                    if( vlc_lstat( d, &st ) )       /* never over an existing file */
                        vlc_rename( src, d );
                }
                free( d );
            }
            free( src );
            continue;
        }

        /* a video downloaded as two parts (no ffmpeg to join them) */
        if( format_part( rel[i], &stem ) )
        {
            for( size_t k = i + 1; k < n; k++ )
            {
                char *s2 = NULL;
                if( rel[k] != NULL && format_part( rel[k], &s2 ) && !strcmp( s2, stem ) )
                {
                    char *other;
                    if( asprintf( &other, "%s/%s", dl->tmpdir, rel[k] ) >= 0 )
                    {   /* the bigger one is the video */
                        struct stat a, b;
                        if( !vlc_stat( src, &a ) && !vlc_stat( other, &b ) && b.st_size > a.st_size )
                        {
                            slave = src;
                            src = other;
                        }
                        else
                            slave = other;
                    }
                    free( rel[k] );
                    rel[k] = NULL;
                    done++;
                }
                free( s2 );
                if( slave != NULL )
                    break;
            }
        }
        else
        {
            stem = strdup( rel[i] );
            char *dot = stem ? strrchr( stem, '.' ) : NULL;
            if( dot != NULL && strchr( dot, '/' ) == NULL )
                *dot = '\0';
        }

        const char *in_ext = ext_of( src );
        if( slave != NULL )
        {   /* join with WLC's engine */
            ext_out = strdup( strcmp( dl->fmt, "mp4" ) ? "mkv" : "mp4" );
            mux = strcmp( dl->fmt, "mp4" ) ? "ffmpeg{mux=matroska}" : "mp4";
            convert = true;
        }
        else if( audio && !have_ffmpeg && audio_profile( dl->fmt ) != NULL
              && strcasecmp( in_ext, dl->fmt ) )
        {   /* convert with WLC's engine */
            const char *pid = audio_profile( dl->fmt ), *e = NULL;
            int ab = !strcmp( dl->quality, "best" ) ? 320 : atoi( dl->quality );
            if( !strcmp( pid, "opus" ) && ab > 256 ) ab = 192;
            if( !strcmp( pid, "m4a" ) && ab > 256 ) ab = 256;
            if( wlc_profile_recipe( pid, ab, &own_chain, &mux, &e, &audio_only ) )
            {
                chain = own_chain;
                ext_out = strdup( e );
                convert = true;
            }
        }
        if( ext_out == NULL )
            ext_out = strdup( in_ext );

        /* where it goes: outdir/<same relative folder>/<name>.<ext>, never overwriting */
        char *dir = NULL, *base = NULL, *final = NULL;
        const char *slash = stem ? strrchr( stem, '/' ) : NULL;
        if( stem != NULL
         && asprintf( &dir, "%s%s%.*s", dl->outdir, slash ? "/" : "",
                      slash ? (int)(slash - stem) : 0, stem ) >= 0 )
        {
            base = strdup( slash ? slash + 1 : stem );
            mkdirs( dir );
            final = base && ext_out ? wlc_free_name( dir, base, ext_out ) : NULL;
        }

        bool ok = false;
        if( final != NULL && convert )
        {
            char *part;
            if( asprintf( &part, "%s/.wlc-dl-%d.part", dir, dl->id ) >= 0 )
            {
                dl_set( dl, &dl->cur, base );
                ok = !wlc_run_sout( sys, src, slave, chain, mux, part, audio_only,
                                    &dl->cancel, &dl->sub_progress )
                     && !vlc_rename( part, final );
                if( !ok )
                    vlc_unlink( part );
                free( part );
            }
        }
        else if( final != NULL )
            ok = !vlc_rename( src, final );

        if( ok )
            dl_add_file( dl, final );
        else if( !atomic_load( &dl->cancel ) )
            dl_set( dl, &dl->msg, convert ? "WLC could not join or convert one of the files"
                                          : "could not move a finished file into place" );
        done++;
        vlc_mutex_lock( &sys->lock );
        dl->progress = media ? fmin( 1., (double)done / media ) : 1.;
        vlc_mutex_unlock( &sys->lock );
        free( final ); free( base ); free( dir ); free( own_chain );
        free( ext_out ); free( stem ); free( slave ); free( src );
    }
    for( size_t i = 0; i < n; i++ )
        free( rel[i] );
    free( rel );
}

#ifndef _WIN32
static void *dl_thread_run( void *data )
{
    wlc_dl_t *dl = data;
    intf_sys_t *sys = dl->sys;
    char *ytdlp = find_tool( sys, "yt-dlp" );
    char *ffmpeg = find_tool( sys, "ffmpeg" );
    const bool audio = !strcmp( dl->mode, "audio" );
    char fsel[256] = "", qual[32] = "";
    const char *args[64];
    int a = 0;

    if( ytdlp == NULL )
    {
        dl_set( dl, &dl->msg, "yt-dlp is not set up; use \"Set up yt-dlp\" first" );
        vlc_mutex_lock( &sys->lock );
        dl->state = DL_ERROR;
        vlc_mutex_unlock( &sys->lock );
        goto out;
    }
    mkdirs( dl->tmpdir );

    args[a++] = ytdlp;
    args[a++] = "--ignore-config";
    args[a++] = "--no-color";
    args[a++] = "--newline";
    args[a++] = "--progress";
    args[a++] = "--no-mtime";
    args[a++] = "--no-simulate";
    args[a++] = "--no-part";
    args[a++] = "--trim-filenames"; args[a++] = "160";
    args[a++] = "--progress-template";
    args[a++] = "download:WLCP %(progress.downloaded_bytes)s %(progress.total_bytes)s "
                "%(progress.total_bytes_estimate)s %(progress.speed)s %(progress.eta)s";
    args[a++] = "--print";
    args[a++] = "before_dl:WLCT %(playlist_index|1)s %(n_entries|1)s %(title)s";
    args[a++] = "-P"; args[a++] = dl->tmpdir;
    args[a++] = "-o";
    /* a playlist goes into a folder named after it, numbered in order */
    args[a++] = dl->playlist
        ? "%(playlist_title,playlist_id|Playlist).120B/%(playlist_index)s - %(title).150B.%(ext)s"
        : "%(title).150B.%(ext)s";
    args[a++] = dl->playlist ? "--yes-playlist" : "--no-playlist";
    args[a++] = "-I";       /* "just this one" on a link that is only a list: its first item */
    args[a++] = dl->playlist ? (dl->items[0] != '\0' ? dl->items : "::") : "1";
    args[a++] = "--ffmpeg-location";
    args[a++] = ffmpeg ? ffmpeg : "/nonexistent/ffmpeg";

    if( audio )
    {
        if( ffmpeg != NULL )
        {
            args[a++] = "-f"; args[a++] = "ba/b";
            args[a++] = "-x";
            args[a++] = "--audio-format"; args[a++] = dl->fmt;
            snprintf( qual, sizeof(qual), "%s", !strcmp( dl->quality, "best" ) ? "0"
                      : dl->quality );
            if( strcmp( qual, "0" ) )
                strncat( qual, "K", sizeof(qual) - strlen( qual ) - 1 );
            args[a++] = "--audio-quality"; args[a++] = qual;
            args[a++] = "--embed-metadata";
            if( strcmp( dl->fmt, "wav" ) )
                args[a++] = "--embed-thumbnail";
        }
        else
        {   /* WLC converts afterwards; AAC (m4a) first, it is the easiest to read */
            args[a++] = "-f"; args[a++] = "ba[ext=m4a]/ba/b";
        }
    }
    else
    {
        const bool mp4 = !strcmp( dl->fmt, "mp4" );
        char h[24] = "";
        if( strcmp( dl->quality, "best" ) )
            snprintf( h, sizeof(h), "[height<=%d]", atoi( dl->quality ) );
        if( mp4 )
            snprintf( fsel, sizeof(fsel),
                      "bv*%s[ext=mp4]+ba[ext=m4a]/b%s[ext=mp4]/bv*%s+ba/b%s/b", h, h, h, h );
        else
            snprintf( fsel, sizeof(fsel), "bv*%s+ba/b%s/b", h, h );
        args[a++] = "-f"; args[a++] = fsel;
        if( ffmpeg != NULL )
        {
            args[a++] = "--merge-output-format"; args[a++] = mp4 ? "mp4" : "mkv";
            args[a++] = "--embed-metadata";
            args[a++] = "--embed-thumbnail";
        }
        if( dl->subs )
        {
            args[a++] = "--write-subs";
            args[a++] = "--sub-langs"; args[a++] = "en.*,en,-live_chat";
            if( ffmpeg != NULL )
                args[a++] = "--embed-subs";
        }
    }
    args[a++] = "--";
    args[a++] = dl->url;
    args[a] = NULL;

    int fd = -1;
    pid_t pid = spawn( sys, (char *const *)args, &fd );
    vlc_mutex_lock( &sys->lock );
    dl->pid = pid;
    if( pid == -1 )
        dl->state = DL_ERROR;
    vlc_mutex_unlock( &sys->lock );
    if( pid == -1 )
    {
        dl_set( dl, &dl->msg, "could not start yt-dlp" );
        goto out;
    }
    msg_Info( sys->intf, "WLC download #%d: %s", dl->id, dl->url );

    /* read its output line by line */
    char buf[8192];
    size_t have = 0;
    mtime_t killed_at = 0;
    for( ;; )
    {
        if( atomic_load( &dl->cancel ) || atomic_load( &sys->stop ) )
        {
            if( killed_at == 0 )
            {
                kill_group( pid );
                killed_at = mdate();
            }
            else if( mdate() - killed_at > 4 * CLOCK_FREQ )
                kill( -pid, SIGKILL );      /* it would not stop */
        }
        struct pollfd pf = { .fd = fd, .events = POLLIN };
        int r = poll( &pf, 1, 300 );
        if( r < 0 && errno != EINTR )
            break;
        if( r <= 0 )
            continue;
        ssize_t got = read( fd, buf + have, sizeof(buf) - 1 - have );
        if( got <= 0 )
            break;
        have += got;
        buf[have] = '\0';
        char *start = buf, *nl;
        while( (nl = strpbrk( start, "\r\n" )) != NULL )
        {
            *nl = '\0';
            if( *start )
                dl_line( dl, start );
            start = nl + 1;
        }
        have = strlen( start );
        memmove( buf, start, have + 1 );
        if( have >= sizeof(buf) - 1 )
            have = 0;   /* an absurdly long line: drop it */
    }
    close( fd );
    int code = reap( pid );
    vlc_mutex_lock( &sys->lock );
    dl->pid = 0;
    vlc_mutex_unlock( &sys->lock );

    if( atomic_load( &dl->cancel ) || atomic_load( &sys->stop ) )
    {
        vlc_mutex_lock( &sys->lock );
        dl->state = DL_CANCELLED;
        vlc_mutex_unlock( &sys->lock );
        goto out;
    }

    /* whatever arrived gets finished, even if some playlist items failed */
    vlc_mutex_lock( &sys->lock );
    dl->state = DL_PROCESSING;
    vlc_mutex_unlock( &sys->lock );
    mkdirs( dl->outdir );
    dl_finish_files( dl, ffmpeg != NULL );

    vlc_mutex_lock( &sys->lock );
    if( atomic_load( &dl->cancel ) )
        dl->state = DL_CANCELLED;
    else if( dl->nfiles == 0 )
    {
        dl->state = DL_ERROR;
        if( dl->msg == NULL )
            dl->msg = code ? strdup( "yt-dlp could not download that link" )
                           : strdup( "nothing was downloaded" );
    }
    else
    {
        dl->state = DL_DONE;
        dl->progress = 1.;
        if( code != 0 && dl->msg == NULL )
            dl->msg = strdup( "some items could not be downloaded" );
        sys->lib_valid = false;
    }
    vlc_mutex_unlock( &sys->lock );
    msg_Info( sys->intf, "WLC download #%d: %s, %zu file(s)", dl->id,
              dl_states[dl->state], dl->nfiles );
out:
    if( dl->tmpdir != NULL )
        rm_tree( dl->tmpdir );
    free( ytdlp );
    free( ffmpeg );
    atomic_store( &dl->finished, true );
    return NULL;
}
#endif

static void dl_free( wlc_dl_t *dl )
{
    free( dl->url ); free( dl->title ); free( dl->mode ); free( dl->quality );
    free( dl->fmt ); free( dl->items ); free( dl->outdir ); free( dl->tmpdir );
    free( dl->cur ); free( dl->msg );
    for( size_t i = 0; i < dl->nfiles; i++ )
        free( dl->files[i] );
    free( dl->files );
    free( dl );
}

/* starts queued downloads (two at a time) and joins finished threads */
static void *dl_manager( void *data )
{
    intf_sys_t *sys = data;
    while( !atomic_load( &sys->stop ) )
    {
        vlc_mutex_lock( &sys->lock );
        int running = 0;
        for( size_t i = 0; i < sys->ndls; i++ )
        {
            wlc_dl_t *dl = sys->dls[i];
            if( dl->thread_ok && atomic_load( &dl->finished ) )
            {
                vlc_mutex_unlock( &sys->lock );
                vlc_join( dl->thread, NULL );
                vlc_mutex_lock( &sys->lock );
                dl->thread_ok = false;
            }
            if( dl->state == DL_QUEUED && atomic_load( &dl->cancel ) )
                dl->state = DL_CANCELLED;
            if( dl->state == DL_RUNNING || dl->state == DL_PROCESSING )
                running++;
        }
#ifndef _WIN32
        for( size_t i = 0; i < sys->ndls && running < DL_PARALLEL; i++ )
        {
            wlc_dl_t *dl = sys->dls[i];
            if( dl->state != DL_QUEUED || dl->thread_ok )
                continue;
            dl->state = DL_RUNNING;
            atomic_store( &dl->finished, false );
            if( vlc_clone( &dl->thread, dl_thread_run, dl, VLC_THREAD_PRIORITY_LOW ) )
            {
                dl->state = DL_ERROR;
                dl->msg = strdup( "could not start the download" );
            }
            else
            {
                dl->thread_ok = true;
                running++;
            }
        }
#endif
        vlc_mutex_unlock( &sys->lock );
        msleep( 300000 );
    }
    return NULL;
}

int wlc_dl_start( intf_sys_t *sys )
{
    sys->dl_thread_ok = !vlc_clone( &sys->dl_thread, dl_manager, sys,
                                    VLC_THREAD_PRIORITY_LOW );
    return sys->dl_thread_ok ? VLC_SUCCESS : VLC_EGENERIC;
}

void wlc_dl_stop( intf_sys_t *sys )
{
    /* sys->stop is already set: running downloads kill yt-dlp and return */
    if( sys->dl_thread_ok )
        vlc_join( sys->dl_thread, NULL );
    for( size_t i = 0; i < sys->ndls; i++ )
    {
        wlc_dl_t *dl = sys->dls[i];
        atomic_store( &dl->cancel, true );
        if( dl->pid > 0 )
            kill_group( dl->pid );
        if( dl->thread_ok )
            vlc_join( dl->thread, NULL );
        dl_free( dl );
    }
    free( sys->dls );
    sys->dls = NULL;
    sys->ndls = 0;
    free( sys->dl_installing );
}

/*****************************************************************************
 * HTTP handlers (all local-only)
 *****************************************************************************/
static char *default_dir( bool audio )
{
    char *base = config_GetUserDir( audio ? VLC_MUSIC_DIR : VLC_VIDEOS_DIR );
    char *home = config_GetUserDir( VLC_HOME_DIR );
    char *d = NULL;
    struct stat st;
    /* VLC answers $HOME when the folder is not set up; use Movies / Music then */
    if( base == NULL || (home != NULL && !strcmp( base, home )) || vlc_stat( base, &st ) )
    {
        free( base );
        base = NULL;
        if( home != NULL && asprintf( &base, "%s/%s", home, audio ? "Music" : "Movies" ) < 0 )
            base = NULL;
    }
    if( base != NULL && asprintf( &d, "%s/WLC Downloads", base ) < 0 )
        d = NULL;
    free( base );
    free( home );
    return d;
}

#ifndef _WIN32
static char *tool_version( intf_sys_t *sys, const char *path )
{
    char *argv[] = { (char *)path, (char *)"--version", NULL }, *out = NULL;
    if( path == NULL || run_capture( sys, argv, &out, 4096, 20 ) != 0 || out == NULL )
    {
        free( out );
        return NULL;
    }
    char *nl = strpbrk( out, "\r\n" );
    if( nl ) *nl = '\0';
    if( strlen( out ) > 60 ) out[60] = '\0';
    return out;
}
#endif

static void tool_json( intf_sys_t *sys, struct vlc_memstream *ms )
{
    char *yt = find_tool( sys, "yt-dlp" ), *ff = find_tool( sys, "ffmpeg" );
    char *own = bin_dir( sys ), *ver = NULL;
#ifndef _WIN32
    ver = tool_version( sys, yt );
#endif
    char *vdir = default_dir( false ), *adir = default_dir( true );
    vlc_memstream_puts( ms, "{\"ytdlp\":" );
    if( yt ) wlc_json_str( ms, yt ); else vlc_memstream_puts( ms, "null" );
    vlc_memstream_puts( ms, ",\"version\":" );
    if( ver ) wlc_json_str( ms, ver ); else vlc_memstream_puts( ms, "null" );
    vlc_memstream_printf( ms, ",\"own\":%s,\"ffmpeg\":",
                          yt && own && !strncmp( yt, own, strlen( own ) ) ? "true" : "false" );
    if( ff ) wlc_json_str( ms, ff ); else vlc_memstream_puts( ms, "null" );
    vlc_memstream_puts( ms, ",\"videoDir\":" );
    wlc_json_str( ms, vdir ? vdir : "" );
    vlc_memstream_puts( ms, ",\"audioDir\":" );
    wlc_json_str( ms, adir ? adir : "" );
    vlc_mutex_lock( &sys->lock );
    vlc_memstream_printf( ms, ",\"installing\":%s", sys->dl_installing ? "true" : "false" );
    vlc_mutex_unlock( &sys->lock );
#if defined(__APPLE__) || defined(__linux__)
    vlc_memstream_puts( ms, ",\"installable\":true}" );
#else
    vlc_memstream_puts( ms, ",\"installable\":false}" );
#endif
    free( yt ); free( ff ); free( own ); free( ver ); free( vdir ); free( adir );
}

void wlc_h_dl_tool( wlc_req_t *req )
{
    struct vlc_memstream ms;
    vlc_memstream_open( &ms );
    tool_json( req->sys, &ms );
    wlc_send_json( req, 200, &ms );
}

/* Downloads the official yt-dlp build for this computer into WLC's own bin
 * folder, checked against the release's SHA2-256SUMS. Also used to update. */
void wlc_h_dl_install( wlc_req_t *req )
{
#if !defined(_WIN32) && (defined(__APPLE__) || defined(__linux__))
    intf_sys_t *sys = req->sys;
# if defined(__APPLE__)
    const char *asset = "yt-dlp_macos";
# elif defined(__aarch64__)
    const char *asset = "yt-dlp_linux_aarch64";
# else
    const char *asset = "yt-dlp_linux";
# endif
    const char *base = getenv( "WLC_YTDLP_RELEASE" );
    if( base == NULL || base[0] == '\0' )
        base = "https://github.com/yt-dlp/yt-dlp/releases/latest/download";
    char *curl = is_exe( "/usr/bin/curl" ) ? strdup( "/usr/bin/curl" ) : find_tool( sys, "curl" );
    char *dir = bin_dir( sys ), *tmp = NULL, *sums = NULL, *dst = NULL;
    char *url_bin = NULL, *url_sums = NULL, *out = NULL;
    const char *err = NULL;

    vlc_mutex_lock( &sys->lock );
    if( sys->dl_installing != NULL )
        err = "yt-dlp is already being set up";
    else
        sys->dl_installing = strdup( asset );
    vlc_mutex_unlock( &sys->lock );
    if( err != NULL )
    {
        free( curl ); free( dir );
        wlc_error( req, 409, err );
        return;
    }

    if( curl == NULL )
        err = "curl is missing, so WLC cannot fetch yt-dlp";
    else if( dir == NULL
          || asprintf( &tmp, "%s/.yt-dlp.download", dir ) < 0
          || asprintf( &sums, "%s/.yt-dlp.sums", dir ) < 0
          || asprintf( &dst, "%s/yt-dlp", dir ) < 0
          || asprintf( &url_bin, "%s/%s", base, asset ) < 0
          || asprintf( &url_sums, "%s/SHA2-256SUMS", base ) < 0 )
        err = "out of memory";
    else
    {
        mkdirs( dir );
        char *a1[] = { curl, (char *)"-fsSL", (char *)"--retry", (char *)"2", (char *)"-o", tmp, url_bin, NULL };
        char *a2[] = { curl, (char *)"-fsSL", (char *)"--retry", (char *)"2", (char *)"-o", sums, url_sums, NULL };
        char hex[65];
        if( run_capture( sys, a1, &out, 8192, 600 ) != 0 )
            err = "could not download yt-dlp (check the internet connection)";
        else if( free( out ), out = NULL, run_capture( sys, a2, &out, 8192, 120 ) != 0 )
            err = "could not download yt-dlp's checksums";
        else if( !sha256_file( tmp, hex ) )
            err = "could not read the download";
        else
        {
            /* find "<hash>  <asset>" in the checksum list */
            FILE *f = vlc_fopen( sums, "r" );
            char line[512];
            bool match = false, listed = false;
            while( f != NULL && fgets( line, sizeof(line), f ) )
            {
                char *sp = strchr( line, ' ' );
                if( sp == NULL ) continue;
                char *name = sp;
                while( *name == ' ' || *name == '*' ) name++;
                name[strcspn( name, "\r\n" )] = '\0';
                if( strcmp( name, asset ) ) continue;
                listed = true;
                match = (size_t)(sp - line) == 64 && !strncasecmp( line, hex, 64 );
            }
            if( f != NULL ) fclose( f );
            if( !listed )
                err = "yt-dlp's checksum list does not include this computer's build";
            else if( !match )
                err = "the yt-dlp download did not match its published checksum; nothing was installed";
            else if( chmod( tmp, 0755 ) || vlc_rename( tmp, dst ) )
                err = "could not put yt-dlp in place";
        }
        free( out );
        vlc_unlink( tmp );
        vlc_unlink( sums );
    }
    if( err == NULL )
        msg_Info( sys->intf, "WLC: yt-dlp set up in %s", dir );
    vlc_mutex_lock( &sys->lock );
    free( sys->dl_installing );
    sys->dl_installing = NULL;
    vlc_mutex_unlock( &sys->lock );
    free( curl ); free( dir ); free( tmp ); free( sums ); free( dst );
    free( url_bin ); free( url_sums );
    if( err != NULL )
    {
        wlc_error( req, 502, err );
        return;
    }
    wlc_h_dl_tool( req );
#else
    wlc_error( req, 501, "setting up yt-dlp is not supported on this system yet" );
#endif
}

static bool url_ok( const char *u )
{
    return u != NULL && strlen( u ) < 4096 && strspn( u, " \t" ) == 0
        && (!strncasecmp( u, "http://", 7 ) || !strncasecmp( u, "https://", 8 ));
}

/* GET-like: what is at this link? (title, playlist entries, heights) */
void wlc_h_dl_probe( wlc_req_t *req )
{
#ifndef _WIN32
    intf_sys_t *sys = req->sys;
    const char *url = wlc_jstr( req->json, "url" );
    if( !url_ok( url ) )
    {
        wlc_error( req, 400, "paste a link that starts with http:// or https://" );
        return;
    }
    char *yt = find_tool( sys, "yt-dlp" );
    if( yt == NULL )
    {
        wlc_error( req, 409, "set up yt-dlp first" );
        return;
    }
    char *argv[] = { yt, (char *)"--ignore-config", (char *)"--no-warnings",
                     (char *)"-J", (char *)"--flat-playlist", (char *)"--", (char *)url, NULL };
    char *out = NULL;
    int code = run_capture( sys, argv, &out, 64 << 20, 120 );
    free( yt );
    char *brace = out ? strchr( out, '{' ) : NULL;
    json_value *j = code == 0 && brace ? json_parse( brace ) : NULL;
    if( j == NULL || j->type != json_object )
    {
        msg_Warn( sys->intf, "yt-dlp probe failed (code %d, %zu bytes): %.300s", code,
                  out ? strlen( out ) : 0, out ? out : "(no output)" );
        /* yt-dlp's "ERROR: ..." line says why */
        const char *e = out ? strstr( out, "ERROR: " ) : NULL;
        char msg[300];
        snprintf( msg, sizeof(msg), "%s", e ? e + 7 : code == -2
                  ? "yt-dlp took too long to answer" : "yt-dlp could not read that link" );
        msg[strcspn( msg, "\r\n" )] = '\0';
        free( out );
        if( j ) json_value_free( j );
        wlc_error( req, 422, msg );
        return;
    }
    struct vlc_memstream ms;
    vlc_memstream_open( &ms );
    const char *type = wlc_jstr( j, "_type" );
    const json_value *entries = wlc_jget( j, "entries" );
    const bool pl = (type && !strcmp( type, "playlist" )) || (entries && entries->type == json_array);
    vlc_memstream_printf( &ms, "{\"playlist\":%s", pl ? "true" : "false" );
    static const char *const strs[] = { "title", "uploader", "channel", "thumbnail",
                                        "webpage_url", "extractor_key" };
    for( size_t i = 0; i < ARRAY_SIZE(strs); i++ )
    {
        const char *v = wlc_jstr( j, strs[i] );
        vlc_memstream_printf( &ms, ",\"%s\":", strs[i] );
        if( v ) wlc_json_str( &ms, v ); else vlc_memstream_puts( &ms, "null" );
    }
    vlc_memstream_printf( &ms, ",\"duration\":%.0f", wlc_jnum( j, "duration", 0 ) );
    /* the video heights on offer, and whether there is sound */
    bool heights[8] = { 0 };
    static const int hs[8] = { 2160, 1440, 1080, 720, 480, 360, 240, 144 };
    const json_value *fm = wlc_jget( j, "formats" );
    for( unsigned i = 0; fm && fm->type == json_array && i < fm->u.array.length; i++ )
    {
        double h = wlc_jnum( fm->u.array.values[i], "height", 0 );
        for( int k = 0; k < 8; k++ )
            if( h >= hs[k] * 0.9 && h <= hs[k] * 1.1 )
                heights[k] = true;
    }
    vlc_memstream_puts( &ms, ",\"heights\":[" );
    bool first = true;
    for( int k = 0; k < 8; k++ )
        if( heights[k] )
        {
            vlc_memstream_printf( &ms, "%s%d", first ? "" : ",", hs[k] );
            first = false;
        }
    vlc_memstream_puts( &ms, "],\"entries\":[" );
    unsigned count = 0;
    for( unsigned i = 0; entries && entries->type == json_array && i < entries->u.array.length; i++ )
    {
        if( count == 1000 ) break;
        const json_value *e = entries->u.array.values[i];
        const char *t = wlc_jstr( e, "title" );
        vlc_memstream_printf( &ms, "%s{\"i\":%u,\"title\":", count ? "," : "", i + 1 );
        wlc_json_str( &ms, t ? t : wlc_jstr( e, "url" ) ? wlc_jstr( e, "url" ) : "" );
        vlc_memstream_printf( &ms, ",\"duration\":%.0f}", wlc_jnum( e, "duration", 0 ) );
        count++;
    }
    vlc_memstream_printf( &ms, "],\"count\":%u}",
                          entries && entries->type == json_array ? entries->u.array.length : 0 );
    json_value_free( j );
    free( out );
    wlc_send_json( req, 200, &ms );
#else
    wlc_error( req, 501, "downloads are not supported on this system yet" );
#endif
}

static bool pick( const char *v, const char *const *ok, size_t n )
{
    for( size_t i = 0; v != NULL && i < n; i++ )
        if( !strcmp( v, ok[i] ) )
            return true;
    return false;
}

void wlc_h_dl_add( wlc_req_t *req )
{
    intf_sys_t *sys = req->sys;
    const json_value *j = req->json;
    const char *url = wlc_jstr( j, "url" );
    const char *mode = wlc_jstr( j, "mode" );
    const char *quality = wlc_jstr( j, "quality" );
    const char *fmt = wlc_jstr( j, "format" );
    const char *items = wlc_jstr( j, "items" );
    const char *dir = wlc_jstr( j, "dir" );
    const char *title = wlc_jstr( j, "title" );
    static const char *const modes[] = { "video", "audio" };
    static const char *const vq[] = { "best", "2160", "1440", "1080", "720", "480", "360" };
    static const char *const aq[] = { "best", "320", "256", "192", "128" };
    static const char *const vf[] = { "mp4", "mkv" };
    static const char *const af[] = { "mp3", "m4a", "opus", "flac", "wav" };

    if( !url_ok( url ) )
        { wlc_error( req, 400, "paste a link that starts with http:// or https://" ); return; }
    if( !pick( mode, modes, 2 ) )
        { wlc_error( req, 400, "choose video or audio" ); return; }
    const bool audio = !strcmp( mode, "audio" );
    if( !(audio ? pick( quality, aq, ARRAY_SIZE(aq) ) : pick( quality, vq, ARRAY_SIZE(vq) ))
     || !(audio ? pick( fmt, af, ARRAY_SIZE(af) ) : pick( fmt, vf, ARRAY_SIZE(vf) )) )
        { wlc_error( req, 400, "that quality or format is not offered" ); return; }
    if( items != NULL && strspn( items, "0123456789,-: " ) != strlen( items ) )
        { wlc_error( req, 400, "items look like 1-5,8,10" ); return; }

    /* where the files go: a folder you name, or Movies|Music/WLC Downloads */
    char *out = NULL;
    if( dir != NULL && dir[0] != '\0' )
    {
        if( dir[0] != '/' || strstr( dir, "/../" ) || has_suffix( dir, "/.." ) )
            { wlc_error( req, 400, "the folder must be a full path like /Users/you/Movies" ); return; }
        out = strdup( dir );
    }
    else
        out = default_dir( audio );
    if( out == NULL )
        { wlc_error( req, 500, "no folder to save into" ); return; }
    while( strlen( out ) > 1 && out[strlen( out ) - 1] == '/' )
        out[strlen( out ) - 1] = '\0';
    mkdirs( out );
    char *real = wlc_realpath( out );
    struct stat st;
    if( real == NULL || vlc_stat( real, &st ) || !S_ISDIR( st.st_mode ) || !strcmp( real, "/" ) )
    {
        free( out ); free( real );
        wlc_error( req, 400, "WLC cannot save into that folder" );
        return;
    }
    free( out );

    wlc_dl_t *dl = calloc( 1, sizeof(*dl) );
    if( dl == NULL ) { free( real ); wlc_error( req, 500, "out of memory" ); return; }
    dl->sys = sys;
    dl->url = strdup( url );
    dl->title = title && title[0] ? strdup( title ) : NULL;
    dl->mode = strdup( mode );
    dl->quality = strdup( quality );
    dl->fmt = strdup( fmt );
    dl->items = strdup( items ? items : "" );
    dl->subs = wlc_jbool( j, "subs", false );
    dl->playlist = wlc_jbool( j, "playlist", false );
    dl->outdir = real;
    dl->eta = -1;
    atomic_init( &dl->cancel, false );
    atomic_init( &dl->finished, false );

    bool add_root = wlc_jbool( j, "addToLibrary", true );
    vlc_mutex_lock( &sys->lock );
    dl->id = ++sys->dl_seq;
    if( asprintf( &dl->tmpdir, "%s/.wlc-dl-%d-%d", dl->outdir, (int)getpid(), dl->id ) < 0 )
        dl->tmpdir = NULL;
    wlc_dl_t **n = realloc( sys->dls, (sys->ndls + 1) * sizeof(*n) );
    bool ok = n != NULL && dl->url && dl->mode && dl->quality && dl->fmt && dl->items && dl->tmpdir;
    if( n != NULL )
        sys->dls = n;
    if( ok )
    {
        sys->dls[sys->ndls++] = dl;
        /* so downloads show up in the Media Center */
        bool covered = false;
        for( unsigned i = 0; i < sys->nroots; i++ )
        {
            size_t l = strlen( sys->roots[i] );
            covered |= !strncmp( dl->outdir, sys->roots[i], l )
                    && (dl->outdir[l] == '\0' || dl->outdir[l] == '/');
        }
        if( add_root && !covered && sys->nroots < WLC_MAX_ROOTS )
        {
            sys->roots[sys->nroots] = strdup( dl->outdir );
            if( sys->roots[sys->nroots] != NULL )
            {
                sys->nroots++;
                sys->lib_valid = false;
                wlc_save( sys );
            }
        }
    }
    const int id = dl->id;
    vlc_mutex_unlock( &sys->lock );
    if( !ok )
    {
        dl_free( dl );
        wlc_error( req, 500, "out of memory" );
        return;
    }
    char buf[48];
    int len = snprintf( buf, sizeof(buf), "{\"ok\":true,\"id\":%d}", id );
    wlc_send( req, 200, "application/json", NULL, buf, len );
}

void wlc_h_dl_jobs( wlc_req_t *req )
{
    intf_sys_t *sys = req->sys;
    struct vlc_memstream ms;
    vlc_memstream_open( &ms );
    vlc_memstream_puts( &ms, "{\"jobs\":[" );
    vlc_mutex_lock( &sys->lock );
    for( size_t i = sys->ndls; i-- > 0; )
    {
        const wlc_dl_t *dl = sys->dls[i];
        double prog = dl->progress;
        if( dl->state == DL_PROCESSING && dl->sub_progress > 0 && dl->nfiles == 0 )
            prog = dl->sub_progress;
        vlc_memstream_printf( &ms, "%s{\"id\":%d,\"state\":\"%s\",\"progress\":%.3f,"
                              "\"speed\":%.0f,\"eta\":%.0f,\"item\":%d,\"items\":%d,\"url\":",
                              i + 1 < sys->ndls ? "," : "", dl->id, dl_states[dl->state],
                              prog, dl->speed, dl->eta, dl->item, dl->nitems );
        wlc_json_str( &ms, dl->url );
        vlc_memstream_puts( &ms, ",\"title\":" );
        wlc_json_str( &ms, dl->title ? dl->title : "" );
        vlc_memstream_puts( &ms, ",\"current\":" );
        wlc_json_str( &ms, dl->cur ? dl->cur : "" );
        vlc_memstream_puts( &ms, ",\"mode\":" );
        wlc_json_str( &ms, dl->mode );
        vlc_memstream_puts( &ms, ",\"format\":" );
        wlc_json_str( &ms, dl->fmt );
        vlc_memstream_puts( &ms, ",\"quality\":" );
        wlc_json_str( &ms, dl->quality );
        vlc_memstream_puts( &ms, ",\"dir\":" );
        wlc_json_str( &ms, dl->outdir );
        vlc_memstream_puts( &ms, ",\"msg\":" );
        wlc_json_str( &ms, dl->msg ? dl->msg : "" );
        vlc_memstream_puts( &ms, ",\"files\":[" );
        for( size_t k = 0; k < dl->nfiles; k++ )
        {
            if( k ) vlc_memstream_putc( &ms, ',' );
            wlc_json_str( &ms, dl->files[k] );
        }
        vlc_memstream_puts( &ms, "]}" );
    }
    vlc_mutex_unlock( &sys->lock );
    vlc_memstream_puts( &ms, "]}" );
    wlc_send_json( req, 200, &ms );
}

void wlc_h_dl_cancel( wlc_req_t *req )
{
    intf_sys_t *sys = req->sys;
    const int id = (int)wlc_jnum( req->json, "id", -1 );
    bool found = false;
    vlc_mutex_lock( &sys->lock );
    for( size_t i = 0; i < sys->ndls; i++ )
        if( sys->dls[i]->id == id )
        {
            wlc_dl_t *dl = sys->dls[i];
            atomic_store( &dl->cancel, true );
            if( dl->pid > 0 )
                kill_group( dl->pid );
            if( dl->state == DL_QUEUED )
                dl->state = DL_CANCELLED;
            found = true;
        }
    vlc_mutex_unlock( &sys->lock );
    if( found ) wlc_ok( req ); else wlc_error( req, 404, "no such download" );
}

/* forget finished downloads (the files stay) */
void wlc_h_dl_clear( wlc_req_t *req )
{
    intf_sys_t *sys = req->sys;
    vlc_mutex_lock( &sys->lock );
    size_t k = 0;
    for( size_t i = 0; i < sys->ndls; i++ )
    {
        wlc_dl_t *dl = sys->dls[i];
        if( dl->state >= DL_DONE && dl->thread_ok && atomic_load( &dl->finished ) )
        {   /* its thread is past its last use of shared state */
            vlc_join( dl->thread, NULL );
            dl->thread_ok = false;
        }
        if( dl->state >= DL_DONE && !dl->thread_ok )
            dl_free( dl );
        else
            sys->dls[k++] = dl;
    }
    sys->ndls = k;
    vlc_mutex_unlock( &sys->lock );
    wlc_ok( req );
}

/* Play a link right away: yt-dlp finds the stream(s), WLC plays them. */
void wlc_h_dl_play( wlc_req_t *req )
{
#ifndef _WIN32
    intf_sys_t *sys = req->sys;
    const char *url = wlc_jstr( req->json, "url" );
    const char *audio = wlc_jbool( req->json, "audio", false ) ? "audio" : NULL;
    if( !url_ok( url ) )
    {
        wlc_error( req, 400, "paste a link that starts with http:// or https://" );
        return;
    }
    char *yt = find_tool( sys, "yt-dlp" );
    if( yt == NULL )
    {
        wlc_error( req, 409, "set up yt-dlp first" );
        return;
    }
    char *argv[] = { yt, (char *)"--ignore-config", (char *)"--no-warnings", (char *)"--no-playlist",
                     (char *)"-f", (char *)(audio ? "ba/b" : "b[height<=1080]/bv*[height<=1080]+ba/b"),
                     (char *)"--print", (char *)"WLCT %(title)s", (char *)"--print", (char *)"urls",
                     (char *)"--", (char *)url, NULL };
    char *out = NULL;
    int code = run_capture( sys, argv, &out, 1 << 20, 90 );
    free( yt );
    char *title = NULL, *u1 = NULL, *u2 = NULL;
    for( char *save = NULL, *l = out ? strtok_r( out, "\r\n", &save ) : NULL; l != NULL;
         l = strtok_r( NULL, "\r\n", &save ) )
    {
        if( !strncmp( l, "WLCT ", 5 ) && title == NULL ) title = l + 5;
        else if( url_ok( l ) && u1 == NULL ) u1 = l;
        else if( url_ok( l ) && u2 == NULL ) u2 = l;
    }
    if( code != 0 || u1 == NULL )
    {
        const char *e = out ? strstr( out, "ERROR: " ) : NULL;
        char msg[300];
        snprintf( msg, sizeof(msg), "%s", e ? e + 7 : "yt-dlp found nothing to play there" );
        msg[strcspn( msg, "\r\n" )] = '\0';
        free( out );
        wlc_error( req, 422, msg );
        return;
    }
    char *slave = NULL;
    const char *opts[1];
    int nopts = 0;
    if( u2 != NULL && asprintf( &slave, "input-slave=%s", u2 ) >= 0 )
        opts[nopts++] = slave;
    int r = playlist_AddExt( sys->pl, u1, title, true, nopts, nopts ? opts : NULL,
                             VLC_INPUT_OPTION_TRUSTED, true );
    free( slave );
    free( out );
    if( r )
        wlc_error( req, 500, "WLC could not start that stream" );
    else
        wlc_ok( req );
#else
    wlc_error( req, 501, "not supported on this system yet" );
#endif
}
