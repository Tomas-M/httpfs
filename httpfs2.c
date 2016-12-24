/*
 * HTTPFS: import a file from a web server to local file system
 * the main use is, to mount an iso on a web server with loop device
 *
 * depends on:
 * FUSE: Filesystem in Userspace
 * Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>
 *
 * This program can be distributed under the terms of the GNU GPL.
 *
 */

/*
 * (c) 2006  hmb  marionraven at users.sourceforge.net
 *
 */

/*
 * Modified to work with fuse 2.7.
 * Added keepalive
 * The passthru functionality removed to simplify the code.
 * (c) 2008-2012,2016 Michal Suchanek <hramrach@gmail.com>
 *
 */

#define FUSE_USE_VERSION 26

#include "config.h"
#include <fuse/fuse_lowlevel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/dir.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <time.h>
#include <stddef.h>
#include <inttypes.h>

#ifdef USE_THREAD
#include <pthread.h>
static pthread_key_t url_key;
pthread_mutex_t cache_lock;
#define FUSE_LOOP fuse_session_loop_mt
#else
#define FUSE_LOOP fuse_session_loop
#endif

#ifdef USE_SSL
#include <gnutls/gnutls.h>
#include <gnutls/x509.h>
#endif

/*
 * ECONNRESET happens with some dodgy servers so may need to handle that.
 * Allow for building without ECONNRESET in case it is not defined.
 */
#ifdef ECONNRESET
#define RETRY_ON_RESET
#endif

enum sock_state {
    SOCK_CLOSED,
    SOCK_OPEN,
    SOCK_KEEPALIVE,
};

enum url_flags {
    URL_DUP,
    URL_SAVE,
    URL_DROP,
};

typedef struct url {
    int proto;
    long timeout;
    char * url;
    char * host; /*hostname*/
    int port;
    char * path; /*get path*/
    char * name; /*file name*/
#ifdef USE_AUTH
    char * auth; /*encoded auth data*/
#endif
#ifdef RETRY_ON_RESET
    long retry_reset; /*retry reset connections*/
    long resets;
#endif
    int sockfd;
    enum sock_state sock_type;
    int redirected;
    int redirect_followed;
    int redirect_depth;
#ifdef USE_SSL
    long ssl_log_level;
    unsigned md5;
    unsigned md2;
    int ssl_initialized;
    int ssl_connected;
    gnutls_certificate_credentials_t sc;
    gnutls_session_t ss;
    const char * cafile;
#endif
    char * req_buf;
    size_t req_buf_size;
    off_t file_size;
    time_t last_modified;
    char tname[TNAME_LEN + 1];
    char xmd5[33];
} struct_url;

// ========== CACHE  ============
#define CACHEMAXSIZE 2147483648LL
#define CRCLEN 32
typedef struct range struct_range;
typedef struct range {
    off_t start;
    size_t size;
    off_t cstart;
    char md5[33];
//    sizef_t csize; // actually, the same as size
    struct_range *next;
} struct_range;

struct_range *idxhead = 0, *lastidx = 0;
int fdcache = 0, fdidx = 0; // cache files descriptors are global for all theads
off_t cacheMaxSize = CACHEMAXSIZE; // default cache file size
//size_t cacheMaxSize = 327680; // debug

int init_cache(char *filename) {
    off_t s;
    struct_range *p = 0;
    int i, c, l;
    if ((fdcache = open(filename, O_RDWR|O_CREAT, S_IRUSR|S_IWUSR)) == -1) {
        fprintf(stderr, "Can't open cache file: %s\n", filename);
        return -1;
    }
    strcat(filename,".idx");
    if ((fdidx = open(filename, O_RDWR|O_CREAT, S_IRUSR|S_IWUSR)) == -1) {
        fprintf(stderr, "Can't open cache index file: %s\n", filename);
        close(fdcache);
        return -1;
    }
    s = lseek(fdidx, 0, SEEK_END);
    if ( s == 0 ) return 0; // nothing caches yet
    lseek(fdidx, 0, SEEK_SET);
    read(fdidx, &c, sizeof(c)); // number of entries
    read(fdidx, &l, sizeof(l)); // lask block index

    for (i = 0; i < c; i++) {
        if (idxhead == NULL){
            p = idxhead = malloc(sizeof(struct_range));
        } else {
            p->next = malloc(sizeof(struct_range));
            p = p->next;
        }
        if (i==l) lastidx = p;
        read(fdidx, &p->start, sizeof(p->start));
        read(fdidx, &p->size, sizeof(p->size));
        read(fdidx, &p->cstart, sizeof(p->cstart));
        read(fdidx, &p->md5, CRCLEN);
        p->md5[32] = 0;
        p->next = 0;
    }
    return 0;
}

ssize_t get_cached(struct_url *url, off_t start, size_t rsize) {

    ssize_t bytes = 0;
    struct_range *p, *p2;
    char md5[2][33];

#ifdef USE_THREAD
    pthread_mutex_lock(&cache_lock);
#endif
    p = idxhead;

    while (p) {
        if ( (p->start <= start) && ((p->start + (off_t)p->size-1) >= start+(off_t)rsize-1) ) {

            lseek(fdcache, p->cstart, SEEK_SET); // set to start of block to read header
            read(fdcache, md5[0], CRCLEN);
            md5[0][32] = 0;

            lseek(fdcache, p->cstart + (start - p->start)+CRCLEN ,SEEK_SET);
            bytes = (ssize_t)read(fdcache, url->req_buf, rsize);

            lseek(fdcache, p->cstart + (off_t)p->size + CRCLEN, SEEK_SET); // set to start of block to read header
            read(fdcache, md5[1], CRCLEN);
            md5[1][32] = 0;


            if (strcmp(p->md5, md5[0]) || strcmp(p->md5, md5[1])){ // Everything is bad. cache corrupted. reset cache
                bytes = 0;
                if (p == idxhead) { // some trick: make range zero; we should keep zero cstart for head;
                    p->start = 0;
                    p->size = 0;
                    memset(p->md5,0, 32);
                    if (lastidx == idxhead) { // need to revert lastidx to last element
                        while(lastidx->next) lastidx = lastidx->next;
                    }
                    if (p->next == NULL) {
                        idxhead = lastidx = 0; free(p); // there was only one cached block; can delete it
                    }
                    break;
                }
                p2=idxhead;
                while (p2->next) {
                    if (p2->next == p) {
                        if (p == lastidx) lastidx = p2; // newest block is 
                        p2->next = p->next;
                        free(p);
                        break;
                    }
                    p2 = p2->next;
                }
                break;
            }

            break;
        }

        p = p->next;
    }
#ifdef USE_THREAD
    pthread_mutex_unlock(&cache_lock);
#endif
    return bytes;
}

ssize_t update_cache(struct_url *url, off_t start, size_t rsize, char *md5) {
    struct_range *p, *t;
    int c, last;
#ifdef USE_THREAD
    pthread_mutex_lock(&cache_lock);
#endif
    if (idxhead == NULL) { // nothing is cached yet
        lastidx = idxhead = malloc(sizeof(struct_range));
        lastidx->next = 0;
        lastidx->cstart = 0;
    } else if (lastidx->cstart + (off_t)lastidx->size + CRCLEN*2 > cacheMaxSize) {
        lastidx = idxhead; // reached max file size. start from brginning
    } else if (lastidx->next == NULL) { // we may add one more block into cache
        lastidx->next = malloc(sizeof(struct_range));
        lastidx->next->cstart = lastidx->cstart + (off_t)lastidx->size + CRCLEN*2;
        lastidx = lastidx->next;
        lastidx->next = 0;
    } else { // we are in a middle of cache file.
        if (lastidx->next->cstart > lastidx->cstart + (off_t)lastidx->size + CRCLEN*2 + (off_t)rsize + CRCLEN*2) { // there is enough space till oldest block (large block was deleted earlier
            p = malloc(sizeof(struct_range));
            p->next = lastidx->next;
            p->cstart = lastidx->cstart + (off_t)lastidx->size + CRCLEN*2;
            lastidx->next = p;
            lastidx = p;
        } else {
            lastidx->next->cstart = lastidx->cstart + (off_t)lastidx->size + CRCLEN*2;
            lastidx = lastidx->next;
        }
    }
    lastidx->start = start;
    lastidx->size = rsize;
    strncpy(lastidx->md5, md5, 32);
    lastidx->md5[32]=0;

    // now we need remove indexes, which blocks will be overwritten
    p = lastidx->next;
    while (p) {
        if (p->cstart < lastidx->cstart + (off_t)lastidx->size + CRCLEN*2) {
            t = p;
            p = p->next;
            lastidx->next = p;
            free(t);
        } else p=0;
    }

    lseek(fdcache, lastidx->cstart, SEEK_SET);
    write(fdcache, md5, CRCLEN);
    write(fdcache, url->req_buf, rsize);
    write(fdcache, md5, CRCLEN);


    lseek(fdidx, sizeof(c)+sizeof(last), SEEK_SET);
    p = idxhead; c = 0, last = 0;;
    do {
        if (p == lastidx) last=c;
        write(fdidx, &p->start, sizeof(p->start));
        write(fdidx, &p->size, sizeof(p->size));
        write(fdidx, &p->cstart, sizeof(p->cstart));
        write(fdidx, &p->md5, CRCLEN);
        c++;
    } while ( (p = p->next) );
    lseek(fdidx, 0, SEEK_SET);
    write(fdidx, &c, sizeof(c));
    write(fdidx, &last, sizeof(last));

#ifdef USE_THREAD
    pthread_mutex_unlock(&cache_lock);
#endif
    return 0;
}

// ========== CACHE ============


static struct_url main_url;
static char* argv0;

static off_t get_stat(struct_url*, struct stat * stbuf);
static ssize_t get_data(struct_url*, off_t start, size_t rsize);
static int open_client_socket(struct_url *url);
static int close_client_socket(struct_url *url);
static int close_client_force(struct_url *url);
static struct_url * thread_setup(void);
static void destroy_url_copy(void *);

/* Protocol symbols. */
#define PROTO_HTTP 0
#ifdef USE_SSL
#define PROTO_HTTPS 1
#endif

#ifdef USE_AUTH
#include "md5.h"
#include "base64.h"
#endif

/*
 * The FUSE operations originally ripped from the hello_ll sample.
 */

static int httpfs_stat(fuse_ino_t ino, struct stat *stbuf)
{
    stbuf->st_ino = ino;
    switch (ino) {
        case 1:
            stbuf->st_mode = S_IFDIR | 0755;
            stbuf->st_nlink = 2;
            break;

        case 2: {
                    struct_url * url = thread_setup();
                    fprintf(stderr, "%s: %s: stat()\n", argv0, url->tname); /*DEBUG*/
                    stbuf->st_mode = S_IFREG | 0444;
                    stbuf->st_nlink = 1;
                    return (int) get_stat(url, stbuf);
                }
                break;

        default:
                errno = ENOENT;
                return -1;
    }
    return 0;
}

static void httpfs_getattr(fuse_req_t req, fuse_ino_t ino,
        struct fuse_file_info *fi)
{
    struct stat stbuf;

    (void) fi;

    memset(&stbuf, 0, sizeof(stbuf));
    if (httpfs_stat(ino, &stbuf) < 0)
        assert(errno),fuse_reply_err(req, errno);
    else
        fuse_reply_attr(req, &stbuf, 1.0);
}

static void httpfs_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    struct fuse_entry_param e;
    memset(&e, 0, sizeof(e));
    e.attr_timeout = 1.0;
    e.entry_timeout = 1.0;

    if (parent != 1 || strcmp(name, main_url.name) != 0){
        e.ino = 0;
    } else {
        e.ino = 2;
        if(httpfs_stat(e.ino, &e.attr) < 0){
            assert(errno);
            fuse_reply_err(req, errno);
            return;
        }

    }
    fuse_reply_entry(req, &e);
}

struct dirbuf {
    char *p;
    size_t size;
};

static void dirbuf_add(fuse_req_t req, struct dirbuf *b, const char *name,
        fuse_ino_t ino)
{
    struct stat stbuf;
    size_t oldsize = b->size;
    b->size += fuse_add_direntry(req, NULL, 0, name, NULL, 0);
    b->p = (char *) realloc(b->p, b->size);
    memset(&stbuf, 0, sizeof(stbuf));
    stbuf.st_ino = ino;
    fuse_add_direntry(req, b->p + oldsize, b->size - oldsize, name, &stbuf,
            (off_t) b->size);
}

#define min(x, y) ((x) < (y) ? (x) : (y))

static int reply_buf_limited(fuse_req_t req, const char *buf, size_t bufsize,
        off_t off, size_t maxsize)
{
    assert(off >= 0);

    if (off < bufsize)
        return fuse_reply_buf(req, buf + off,
                min(bufsize - (size_t)off, maxsize));
    else
        return fuse_reply_buf(req, NULL, 0);
}

static void httpfs_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
        off_t off, struct fuse_file_info *fi)
{
    (void) fi;

    if (ino != 1)
        fuse_reply_err(req, ENOTDIR);
    else {
        struct dirbuf b;

        memset(&b, 0, sizeof(b));
        dirbuf_add(req, &b, ".", 1);
        dirbuf_add(req, &b, "..", 1);
        dirbuf_add(req, &b, main_url.name, 2);
        reply_buf_limited(req, b.p, b.size, off, size);
        free(b.p);
    }
}

static void httpfs_open(fuse_req_t req, fuse_ino_t ino,
        struct fuse_file_info *fi)
{
    if (ino != 2)
        fuse_reply_err(req, EISDIR);
    else if ((fi->flags & 3) != O_RDONLY)
        fuse_reply_err(req, EACCES);
    else{
        /* direct_io is supposed to allow partial reads. However, setting
         * the flag causes read length max at 4096 bytes which leads to
         * *many* requests, poor performance, and errors. Some resources
         * like TCP ports are recycled too fast for Linux to cope.
         */
        //fi->direct_io = 1;
        fuse_reply_open(req, fi);
    }
}

static void httpfs_read(fuse_req_t req, fuse_ino_t ino, size_t size,
        off_t off, struct fuse_file_info *fi)
{
    (void) fi;

    struct_url * url = thread_setup();
    ssize_t res;

    assert(ino == 2);

    assert(url->file_size >= off);

    size=min(size, (size_t)(url->file_size - off));

    if(url->file_size == off) {
        /* Handling of EOF is not well documented, returning EOF as error
         * does not work but this does.  */
        fuse_reply_buf(req, NULL,  0);
        return;
    }
    /* since we have to return all stuff requested the buffer cannot be
     * allocated in advance */
    if(url->req_buf
            && ( (url->req_buf_size < size )
                || ( (url->req_buf_size > size )
                    && (url->req_buf_size > MAX_REQUEST) ) ) ){
        free(url->req_buf);
        url->req_buf = 0;
    }
    if(! url->req_buf){
        url->req_buf_size = size;
        url->req_buf = malloc(size);
    }

    if((res = get_data(url, off, size)) < 0){
        assert(errno);
        fuse_reply_err(req, errno);
    }else{
        fuse_reply_buf(req, url->req_buf, (size_t)res);
    }
}

static struct fuse_lowlevel_ops httpfs_oper = {
    .lookup             = httpfs_lookup,
    .getattr            = httpfs_getattr,
    .readdir            = httpfs_readdir,
    .open               = httpfs_open,
    .read               = httpfs_read,
};

/*
 * A few utility functions
 */
#ifdef NEED_STRNDUP
static char * strndup(const char * str, size_t n){
    if(n > strlen(str)) n = strlen(str);
    char * res = malloc(n + 1);
    memcpy(res, str, n);
    res[n] = 0;
    return res;
}
#endif

static int mempref(const char * mem, const char * pref, size_t size, int case_sensitive)
{
    /* return true if found */
    if (size < strlen(pref)) return 0;
    if (case_sensitive)
        return ! memcmp(mem, pref, strlen(pref));
    else {
        int i;
        for (i = 0; i < strlen(pref); i++)
            /* Unless somebody calling setlocale() behind our back locale should be C.  */
            /* It is important to not uppercase in languages like Turkish.  */
            if (tolower(mem[i]) != tolower(pref[i]))
                return 0;
        return 1;
    }
}

#ifdef USE_SSL

static void errno_report(const char * where);
static void ssl_error(ssize_t error, struct_url * url, const char * where);
static void ssl_error_p(ssize_t error, struct_url * url, const char * where, const char * extra);
/* Functions to deal with gnutls_datum_t stolen from gnutls docs.
 * The structure does not seem documented otherwise.
 */
static gnutls_datum_t
load_file (const char *file)
{
    FILE *f;
    gnutls_datum_t loaded_file = { NULL, 0 };
    long filelen;
    void *ptr;
    f = fopen (file, "r");
    if (!f)
        errno_report(file);
    else if (fseek (f, 0, SEEK_END) != 0)
        errno_report(file);
    else if ((filelen = ftell (f)) < 0)
        errno_report(file);
    else if (fseek (f, 0, SEEK_SET) != 0)
        errno_report(file);
    else if (!(ptr = malloc ((size_t) filelen)))
        errno_report(file);
    else if (fread (ptr, 1, (size_t) filelen, f) < (size_t) filelen)
        errno_report(file);
    else {
        loaded_file.data = ptr;
        loaded_file.size = (unsigned int) filelen;
        fprintf(stderr, "Loaded '%s' %ld bytes\n", file, filelen);
        /* fwrite(ptr, filelen, 1, stderr); */
    }
    return loaded_file;
}

static void
unload_file (gnutls_datum_t data)
{
    free (data.data);
}

/* This function will print some details of the
 * given session.
 *
 * Stolen from the GNUTLS docs.
 */
int
print_ssl_info (gnutls_session_t session)
{
    const char *tmp;
    gnutls_credentials_type_t cred;
    gnutls_kx_algorithm_t kx;
    int dhe, ecdh;
    dhe = ecdh = 0;
    if (!session) {
        fprintf(stderr, "No SSL session data.\n");
        return 0;
    }
    /* print the key exchange’s algorithm name
    */
    kx = gnutls_kx_get (session);
    tmp = gnutls_kx_get_name (kx);
    fprintf(stderr, "- Key Exchange: %s\n", tmp);
    /* Check the authentication type used and switch
     * to the appropriate.
     */
    cred = gnutls_auth_get_type (session);
    switch (cred)
    {
        case GNUTLS_CRD_CERTIFICATE:
            /* certificate authentication */
            /* Check if we have been using ephemeral Diffie-Hellman.
            */
            if (kx == GNUTLS_KX_DHE_RSA || kx == GNUTLS_KX_DHE_DSS)
                dhe = 1;
#if (GNUTLS_VERSION_MAJOR > 3 )
            else if (kx == GNUTLS_KX_ECDHE_RSA || kx == GNUTLS_KX_ECDHE_ECDSA)
                ecdh = 1;
#endif
            /* cert should have been printed when it was verified */
            break;
        default:
            fprintf(stderr, "Not a x509 sesssion !?!\n");

    }
#if (GNUTLS_VERSION_MAJOR > 3 )
    /* switch */
    if (ecdh != 0)
        fprintf(stderr, "- Ephemeral ECDH using curve %s\n",
                gnutls_ecc_curve_get_name (gnutls_ecc_curve_get (session)));
    else
#endif
        if (dhe != 0)
            fprintf(stderr, "- Ephemeral DH using prime of %d bits\n",
                    gnutls_dh_get_prime_bits (session));
    /* print the protocol’s name (ie TLS 1.0)
    */
    tmp = gnutls_protocol_get_name (gnutls_protocol_get_version (session));
    fprintf(stderr, "- Protocol: %s\n", tmp);
    /* print the certificate type of the peer.
     * ie X.509
     */
    tmp =
        gnutls_certificate_type_get_name (gnutls_certificate_type_get (session));
    fprintf(stderr, "- Certificate Type: %s\n", tmp);
    /* print the compression algorithm (if any)
    */
    tmp = gnutls_compression_get_name (gnutls_compression_get (session));
    fprintf(stderr, "- Compression: %s\n", tmp);
    /* print the name of the cipher used.
     * ie 3DES.
     */
    tmp = gnutls_cipher_get_name (gnutls_cipher_get (session));
    fprintf(stderr, "- Cipher: %s\n", tmp);
    /* Print the MAC algorithms name.
     * ie SHA1
     */
    tmp = gnutls_mac_get_name (gnutls_mac_get (session));
    fprintf(stderr, "- MAC: %s\n", tmp);
    fprintf(stderr, "Note: SSL paramaters may change as new connections are established to the server.\n");
    return 0;
}



/* This function will try to verify the peer’s certificate, and
 * also check if the hostname matches, and the activation, expiration dates.
 *
 * Stolen from the gnutls manual.
 */
static int
verify_certificate_callback (gnutls_session_t session)
{
    unsigned int status;
    const gnutls_datum_t *cert_list;
    unsigned int cert_list_size;
    int ret;
    gnutls_x509_crt_t cert;
    gnutls_datum_t data = {0};
    struct_url * url = gnutls_session_get_ptr (session);
    const char *hostname = url->host;

    /* This verification function uses the trusted CAs in the credentials
     * structure. So you must have installed one or more CA certificates.
     */
    ret = gnutls_certificate_verify_peers2 (session, &status);
    if (ret < 0)
    {
        ssl_error(ret, url, "verify certificate");
        return GNUTLS_E_CERTIFICATE_ERROR;
    }
    if (status & GNUTLS_CERT_INVALID)
        fprintf(stderr, "The server certificate is NOT trusted.\n");
    if (status & GNUTLS_CERT_INSECURE_ALGORITHM)
        fprintf(stderr, "The server certificate uses an insecure algorithm.\n");
    if (status & GNUTLS_CERT_SIGNER_NOT_FOUND)
        fprintf(stderr, "The server certificate hasn’t got a known issuer.\n");
    if (status & GNUTLS_CERT_REVOKED)
        fprintf(stderr, "The server certificate has been revoked.\n");
    if (status & GNUTLS_CERT_EXPIRED)
        fprintf(stderr, "The server certificate has expired\n");
    if (status & GNUTLS_CERT_NOT_ACTIVATED)
        fprintf(stderr, "The server certificate is not yet activated\n");
    /* Up to here the process is the same for X.509 certificates and
     * OpenPGP keys. From now on X.509 certificates are assumed. This can
     * be easily extended to work with openpgp keys as well.
     */
    if (gnutls_certificate_type_get (session) != GNUTLS_CRT_X509)
        return GNUTLS_E_CERTIFICATE_ERROR;
    if (gnutls_x509_crt_init (&cert) < 0)
    {
        ssl_error(ret, url, "verify certificate");
        return GNUTLS_E_CERTIFICATE_ERROR;
    }
    cert_list = gnutls_certificate_get_peers (session, &cert_list_size);
    if (cert_list == NULL)
    {
        fprintf(stderr, "No server certificate was found!\n");
        return GNUTLS_E_CERTIFICATE_ERROR;
    }
    /* Check the hostname matches the certificate. */
    ret = gnutls_x509_crt_import (cert, &cert_list[0], GNUTLS_X509_FMT_DER);
    if (ret < 0)
    {
        ssl_error(ret, url, "parsing certificate");
        return GNUTLS_E_CERTIFICATE_ERROR;
    }
    if (!(url->ssl_connected)) if (!gnutls_x509_crt_print (cert, GNUTLS_CRT_PRINT_FULL, &data)) {
        fprintf(stderr, "%s", data.data);
        gnutls_free(data.data);
    }
    if (!hostname || !gnutls_x509_crt_check_hostname (cert, hostname))
    {
        int found = 0;
        if (hostname) {
            int i;
            size_t len = strlen(hostname);
            if (*(hostname+len-1) == '.') len--;
            if (!(url->ssl_connected)) fprintf(stderr, "Server hostname verification failed. Trying to peek into the cert.\n");
            for (i=0;;i++) {
                char * dn = NULL;
                size_t dn_size = 0;
                int dn_ret = 0;
                int match=0;
                gnutls_x509_crt_get_dn_by_oid(cert, GNUTLS_OID_X520_COMMON_NAME, i, 0, dn, &dn_size);
                if (dn_size) dn = malloc(dn_size + 1); /* nul not counted */
                if (dn)
                    dn_ret = gnutls_x509_crt_get_dn_by_oid(cert, GNUTLS_OID_X520_COMMON_NAME, i, 0, dn, &dn_size);
                if (!dn_ret){
                    if (dn) {
                        if (*(dn+dn_size-1) == '.') dn_size--;
                        if (len == dn_size)
                            match = ! strncmp(dn, hostname, len);
                        if (match) found = 1;
                        if (!(url->ssl_connected)) fprintf(stderr, "Cert CN(%i): %s: %c\n", i, dn, match?'*':'X');
                    }}
                else
                    ssl_error(dn_ret, url, "getting cert subject data");
                if (dn) free(dn);
                if (dn_ret || !dn)
                    break;
            }
        }
        if(!found){
            fprintf(stderr, "The server certificate’s owner does not match hostname ’%s’\n",
                    hostname);
            return GNUTLS_E_CERTIFICATE_ERROR;
        }
    }
    gnutls_x509_crt_deinit (cert);
    /*
     * It the status includes GNUTLS_CERT_INVALID whenever
     * there is a problem and the other flags are just informative.
     */
    if (status & GNUTLS_CERT_INVALID)
        return GNUTLS_E_CERTIFICATE_ERROR;
    /* notify gnutls to continue handshake normally */
    return 0;
}


static void logfunc(int level, const char * str)
{
    fputs(str, stderr);
}

static void ssl_error_p(ssize_t error, struct_url * url, const char * where, const char * extra)
{
    const char * err_desc;
    if((error == GNUTLS_E_FATAL_ALERT_RECEIVED) || (error == GNUTLS_E_WARNING_ALERT_RECEIVED))
        err_desc = gnutls_alert_get_name(gnutls_alert_get(url->ss));
    else
        err_desc = gnutls_strerror((int)error);

    fprintf(stderr, "%s: %s: %s: %s%zd %s.\n", argv0, url->tname, where, extra, error, err_desc);
}

static void ssl_error(ssize_t error, struct_url * url, const char * where)
{
    ssl_error_p(error, url, where, "");
    /* FIXME try to decode errors more meaningfully */
    errno = EIO;
}
#endif

static void errno_report(const char * where)
{
    struct_url * url = thread_setup();
    int e = errno;
    fprintf(stderr, "%s: %s: %s: %d %s.\n", argv0, url->tname, where, errno, strerror(errno));
    errno = e;
}

static char * url_encode(char * path) {
    return strdup(path); /*FIXME encode*/
}

/*
 * functions for handling struct_url
 */

static int init_url(struct_url* url)
{
    memset(url, 0, sizeof(*url));
    url->sock_type = SOCK_CLOSED;
    url->timeout = TIMEOUT;
#ifdef RETRY_ON_RESET
    url->retry_reset = RESET_RETRIES;
#endif
#ifdef USE_SSL
    url->cafile = CERT_STORE;
#endif
    return 0;
}

static int free_url(struct_url* url)
{
    if(url->sock_type != SOCK_CLOSED)
        close_client_force(url);
    if(url->host) free(url->host);
    url->host = 0;
    if(url->path) free(url->path);
    url->path = 0;
    if(url->name) free(url->name);
    url->name = 0;
#ifdef USE_AUTH
    if(url->auth) free(url->auth);
    url->auth = 0;
#endif
    url->port = 0;
    url->proto = 0; /* only after socket closed */
    url->file_size=0;
    url->last_modified=0;
    return 0;
}

static void print_url(FILE *f, const struct_url * url)
{
    char * protocol = "?!?";
    switch(url->proto){
        case PROTO_HTTP:
            protocol = "http";
            break;;
#ifdef USE_SSL
        case PROTO_HTTPS:
            protocol = "https";
            break;;
#endif
    }
    fprintf(f, "file name: \t%s\n", url->name);
    fprintf(f, "host name: \t%s\n", url->host);
    fprintf(f, "port number: \t%d\n", url->port);
    fprintf(f, "protocol: \t%s\n", protocol);
    fprintf(f, "request path: \t%s\n", url->path);
#ifdef USE_AUTH
    fprintf(f, "auth data: \t%s\n", url->auth ? "(present)" : "(null)");
#endif
}

static int parse_url(char * _url, struct_url* res, enum url_flags flag)
{
    const char * url_orig;
    const char * url;
    const char * http = "http://";
#ifdef USE_SSL
    const char * https = "https://";
#endif /* USE_SSL */
    int path_start = '/';

    if (!_url)
        _url = res->url;
    assert(_url);
    switch(flag) {
        case URL_DUP:
            _url = strdup(_url);
        case URL_SAVE:
            assert (_url != res->url);
            if (res->url)
                free(res->url);
            res->url = _url;
            break;
        case URL_DROP:
            assert (res->url);
            break;
    }
    /* constify so compiler warns about modification */
    url_orig = url = _url;

    close_client_force(res);
#ifdef USE_SSL
    res->ssl_connected = 0;
#endif

    if (strncmp(http, url, strlen(http)) == 0) {
        url += strlen(http);
        res->proto = PROTO_HTTP;
        res->port = 80;
#ifdef USE_SSL
    } else if (strncmp(https, url, strlen(https)) == 0) {
        url += strlen(https);
        res->proto = PROTO_HTTPS;
        res->port = 443;
#endif /* USE_SSL */
    } else {
        fprintf(stderr, "Invalid protocol in url: %s\n", url_orig);
        return -1;
    }

    /* determine if path was given */
    if(res->path)
        free(res->path);
    if(strchr(url, path_start))
        res->path = url_encode(strchr(url, path_start));
    else{
        path_start = 0;
        res->path = strdup("/");
    }


#ifdef USE_AUTH
    /* Get user and password */
    if(res->auth)
        free(res->auth);
    if(strchr(url, '@') && (strchr(url, '@') < strchr(url, path_start))){
        res->auth = b64_encode((unsigned char *)url, strchr(url, '@') - url);
        url = strchr(url, '@') + 1;
    }else{
        res->auth = 0;
    }
#endif /* USE_AUTH */

    /* Get port number. */
    int host_end = path_start;
    if(strchr(url, ':') && (strchr(url, ':') < strchr(url, path_start))){
        /* FIXME check that port is a valid numeric value */
        res->port = atoi(strchr(url, ':') + 1);
        if (! res->port) {
            fprintf(stderr, "Invalid port in url: %s\n", url_orig);
            return -1;
        }
        host_end = ':';
    }
    /* Get the host name. */
    if (url == strchr(url, host_end)){ /*no hastname in the url */
        fprintf(stderr, "No hostname in url: %s\n", url_orig);
        return -1;
    }
    if(res->host)
        free(res->host);
    res->host = strndup(url, (size_t)(strchr(url, host_end) - url));

    if(flag != URL_DROP) {
        /* Get the file name. */
        url = strchr(url, path_start);
        const char * end = url + strlen(url);
        end--;

        /* Handle broken urls with multiple slashes. */
        while((end > url) && (*end == '/')) end--;
        end++;
        if(res->name)
            free(res->name);
        if((path_start == 0) || (end == url)
                || (strncmp(url, "/", (size_t)(end - url)) ==  0)){
            res->name = strdup(res->host);
        }else{
            while(strchr(url, '/') && (strchr(url, '/') < end))
                url = strchr(url, '/') + 1;
            res->name = strndup(url, (size_t)(end - url));
        }
    } else
        assert(res->name);

    return res->proto;
}

static void usage(void)
{
    fprintf(stderr, "%s >>> Version: %s <<<\n", __FILE__, VERSION);
    fprintf(stderr, "usage:  %s [-c [console]] "
#ifdef USE_SSL
            "[-a file] [-d n] [-5] [-2] "
#endif
            "[-f] [-t timeout] [-r n] [-C filename] [-S n] url mount-parameters\n\n", argv0);
#ifdef USE_SSL
    fprintf(stderr, "\t -2 \tAllow RSA-MD2 server certificate\n");
    fprintf(stderr, "\t -5 \tAllow RSA-MD5 server certificate\n");
    fprintf(stderr, "\t -a \tCA file used to verify server certificate\n\t\t(default: %s)\n", CERT_STORE);
#endif
    fprintf(stderr, "\t -c \tuse console for standard input/output/error\n\t\t(default: %s)\n", CONSOLE);
#ifdef USE_SSL
    fprintf(stderr, "\t -d \tGNUTLS debug level (default 0)\n");
#endif
    fprintf(stderr, "\t -f \tstay in foreground - do not fork\n");
#ifdef RETRY_ON_RESET
    fprintf(stderr, "\t -r \tnumber of times to retry connection on reset\n\t\t(default: %i)\n", RESET_RETRIES);
#endif
    fprintf(stderr, "\t -t \tset socket timeout in seconds (default: %i)\n", TIMEOUT);
    fprintf(stderr, "\t -C \tset cache filename. also creates .idx file near to cache file\n");
    fprintf(stderr, "\t -S \tset max size of cache file (default: %lld)\n", CACHEMAXSIZE);
    fprintf(stderr, "\tmount-parameters should include the mount point\n");
}

#define shift { if(!argv[1] || !argv[2]) { usage(); return 4; };\
    argc--; argv[1] = argv[0]; argv = argv + 1;}

static int convert_num(long * num, char ** argv)
{
    char * end = " ";
    if( isdigit(*(argv[1]))) {
        *num = strtol(argv[1], &end, 0);
        /* now end should point to '\0' */
    }
    if(*end){
        usage();
        fprintf(stderr, "'%s' is not a number.\n",
                argv[1]);
        return -1;
    }
    return 0;
}

static int convert_num64(unsigned long long * num, char ** argv)
{
    char * end = " ";
    if( isdigit(*(argv[1]))) {
        *num = strtoull(argv[1], &end, 0);
        /* now end should point to '\0' */
    }
    if(*end){
        usage();
        fprintf(stderr, "'%s' is not a number.\n",
                argv[1]);
        return -1;
    }
    return 0;
}


int main(int argc, char *argv[])
{
    char * fork_terminal = CONSOLE;
    char * cachename = NULL;
    int do_fork = 1;
    putenv("TZ=");/*UTC*/
    argv0 = argv[0];
    init_url(&main_url);
    strncpy(main_url.tname, "main", TNAME_LEN);

    while( argv[1] && (*(argv[1]) == '-') )
    {
        char * arg = argv[1]; shift;
        while (*++arg){
            switch (*arg){
                case 'C': cachename = malloc(strlen(argv[1])+5); // 4 (".idx") + 1 '\0'
                          strcpy(cachename, argv[1]);
                          shift;
                          break;
                case 'S': if (convert_num64((unsigned long long*)(&cacheMaxSize), argv))
                              return 5;
                          shift;
                          break;
                case 'c': if( *(argv[1]) != '-' ) {
                              fork_terminal = argv[1]; shift;
                          }else{
                              fork_terminal = 0;
                          }
                          break;
#ifdef USE_SSL
                case '2': main_url.md2 = GNUTLS_VERIFY_ALLOW_SIGN_RSA_MD2;
                          break;
                case '5': main_url.md5 = GNUTLS_VERIFY_ALLOW_SIGN_RSA_MD5;
                          break;
                case 'a': main_url.cafile = argv[1];
                          shift;
                          break;
                case 'd': if (convert_num(&main_url.ssl_log_level, argv))
                              return 4;
                          shift;
                          break;
#endif
#ifdef RETRY_ON_RESET
                case 'r': if (convert_num(&main_url.retry_reset, argv))
                              return 4;
                          shift;
                          break;
#endif
                case 't': if (convert_num(&main_url.timeout, argv))
                              return 4;
                          shift;
                          break;
                case 'f': do_fork = 0;
                          break;
                default:
                          usage();
                          fprintf(stderr, "Unknown option '%c'.\n", *arg);
                          return 4;
            }
        }
    }

    if (argc < 3) {
        usage();
        return 1;
    }
    if (cachename) {
        if (init_cache(cachename) != 0){
            fprintf(stderr, "err cache init\n");
             return 5;
        }
        free(cachename);
    }
    if(parse_url(argv[1], &main_url, URL_DUP) == -1){
        fprintf(stderr, "invalid url: %s\n", argv[1]);
        return 2;
    }
    print_url(stderr, &main_url);
    int sockfd = open_client_socket(&main_url);
    if(sockfd < 0) {
        fprintf(stderr, "Connection failed.\n");
        return 3;
    }
#ifdef USE_SSL
    else {
        print_ssl_info(main_url.ss);
    }
#endif
    close_client_socket(&main_url);
    struct stat st;
    off_t size = get_stat(&main_url, &st);
    if(size >= 0) {
        fprintf(stderr, "file size: \t%" PRIdMAX "\n", (intmax_t)size);
    }else{
        return 3;
    }

    shift;
    if(fork_terminal && access(fork_terminal, O_RDWR)){
        errno_report(fork_terminal);
        fork_terminal=0;
    }

#ifdef USE_THREAD
    close_client_force(&main_url); /* each thread should open its own socket */
    pthread_key_create(&url_key, &destroy_url_copy);
    pthread_mutex_init(&cache_lock, NULL);
#endif
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    struct fuse_chan *ch;
    char *mountpoint;
    int err = -1;
    int fork_res = 0;

    if (fuse_parse_cmdline(&args, &mountpoint, NULL, NULL) != -1 &&
            (ch = fuse_mount(mountpoint, &args)) != NULL) {
        /* try to fork at some point where the setup is mostly done */
        /* FIXME try to close std* and the like ? */
        if(do_fork) fork_res = fork();

        switch (fork_res) {
            case 0:

                {
                    if(fork_terminal){
                        /* if we can access the console use it */
                        int fd = open(fork_terminal, O_RDONLY);
                        dup2(fd, 0);
                        close (fd);
                        fd = open(fork_terminal, O_WRONLY);
                        dup2(fd, 1);
                        close (fd);
                        fd = open(fork_terminal, O_WRONLY|O_SYNC);
                        dup2(fd, 2);
                        close (fd);
                    }

                    struct fuse_session *se;
                    se = fuse_lowlevel_new(&args, &httpfs_oper,
                            sizeof(httpfs_oper), NULL);
                    if (se != NULL) {
                        if (fuse_set_signal_handlers(se) != -1) {
                            fuse_session_add_chan(se, ch);
                            err = FUSE_LOOP(se);
                            fuse_remove_signal_handlers(se);
                            fuse_session_remove_chan(ch);
                        }
                        fuse_session_destroy(se);
                    }
                    fuse_unmount(mountpoint, ch);
                }
                break;;
            case -1:
                errno_report("fork");
                break;;
            default:
                err = 0;
                break;;
        }
    }
    fuse_opt_free_args(&args);

#ifdef USE_THREAD
    pthread_mutex_destroy(&cache_lock);
#endif
    if (fdcache > 0) {
        close(fdcache);
        close(fdidx);
    }

    return err ? err : 0;
}

#ifdef USE_SSL
/* handle non-fatal SSL errors */
int handle_ssl_error(struct_url *url, ssize_t * res, const char *where)
{
    /* do not handle success */
    if (!res)
        return 0;
    /*
     * It is suggested to retry GNUTLS_E_INTERRUPTED and GNUTLS_E_AGAIN
     * However, retrying only causes delay in practice. FIXME
     */
    if ((*res == GNUTLS_E_INTERRUPTED) || (*res == GNUTLS_E_AGAIN))
        return 0;

    if (*res == GNUTLS_E_REHANDSHAKE) {
        fprintf(stderr, "%s: %s: %s: %zd %s.\n", argv0, url->tname, where, *res,
                "SSL rehanshake requested by server");
        if (gnutls_safe_renegotiation_status(url->ss)) {
            *res = gnutls_handshake (url->ss);
            if (*res) {
                return 0;
            }
            return 1;
        } else {
            fprintf(stderr, "%s: %s: %s: %zd %s.\n", argv0, url->tname, where, *res,
                    "safe rehandshake not supported on this connection");
            return 0;
        }
    }

    if (!gnutls_error_is_fatal((int)*res)) {
        ssl_error_p(*res, url, where, "non-fatal SSL error ");
        *res = 0;
        return 1;
    }

    return 0;
}
#endif

/*
 * Socket operations that abstract ssl and keepalive as much as possible.
 * Keepalive is set when parsing the headers.
 *
 */

static int close_client_socket(struct_url *url) {
    if (url->sock_type == SOCK_KEEPALIVE) {
        fprintf(stderr, "%s: %s: keeping socket open.\n", argv0, url->tname); /*DEBUG*/
        return SOCK_KEEPALIVE;
    }
    return close_client_force(url);
}

static int close_client_force(struct_url *url) {
    int sock_closed = 0;

    if(url->sock_type != SOCK_CLOSED){
        fprintf(stderr, "%s: %s: closing socket.\n", argv0, url->tname); /*DEBUG*/
#ifdef USE_SSL
        if (url->proto == PROTO_HTTPS) {
            fprintf(stderr, "%s: %s: closing SSL socket.\n", argv0, url->tname);
            gnutls_bye(url->ss, GNUTLS_SHUT_RDWR);
            gnutls_deinit(url->ss);
        }
#endif
        close(url->sockfd);
        sock_closed = 1;
    }
    url->sock_type = SOCK_CLOSED;

    if(url->redirected && url->redirect_followed) {
        fprintf(stderr, "%s: %s: returning from redirect to master %s\n", argv0, url->tname, url->url);
        if (sock_closed) url->redirect_depth = 0;
        url->redirect_followed = 0;
        url->redirected = 0;
        parse_url(NULL, url, URL_DROP);
        print_url(stderr, url);
        return -EAGAIN;
    }
    return url->sock_type;
}

#ifdef USE_THREAD

static void destroy_url_copy(void * urlptr)
{
    if(urlptr){
        fprintf(stderr, "%s: Thread %08lX ended.\n", argv0, pthread_self()); /*DEBUG*/
        free_url(urlptr);
        free(urlptr);
    }
}

static struct_url * create_url_copy(const struct_url * url)
{
    struct_url * res = malloc(sizeof(struct_url));
    memcpy(res, url, sizeof(struct_url));
    if(url->name)
        res->name = strdup(url->name);
    if(url->host)
        res->host = strdup(url->host);
    if(url->path)
        res->path = strdup(url->path);
#ifdef USE_AUTH
    if(url->auth)
        res->auth = strdup(url->auth);
#endif
    memset(res->tname, 0, TNAME_LEN + 1);
    snprintf(res->tname, TNAME_LEN, "%0*lX", TNAME_LEN, pthread_self());
    return res;
}

static struct_url * thread_setup(void)
{
    struct_url * res = pthread_getspecific(url_key);
    if(!res) {
        fprintf(stderr, "%s: Thread %08lX started.\n", argv0, pthread_self()); /*DEBUG*/
        res = create_url_copy(&main_url);
        pthread_setspecific(url_key, res);
    }
    return res;
}

#else /*USE_THREAD*/
static struct_url * thread_setup(void) { return &main_url; }
#endif


static ssize_t read_client_socket(struct_url *url, void * buf, size_t len) {
    ssize_t res;
    struct timeval timeout;
    timeout.tv_sec = url->timeout;
    timeout.tv_usec = 0;
    setsockopt(url->sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#ifdef USE_SSL
    if (url->proto == PROTO_HTTPS) {
        do {
            res = gnutls_record_recv(url->ss, buf, len);
        } while ((res < 0) && handle_ssl_error(url, &res, "read"));
        if (res <= 0) ssl_error(res, url, "read");
    } else
#endif
    {
        res = read(url->sockfd, buf, len);
        if (res <= 0) errno_report("read");
    }
    return res;
}

static ssize_t
write_client_socket(struct_url *url, const void * buf, size_t len)
{
    do {
        int fd = open_client_socket(url);
        ssize_t res;

        if (fd < 0) return -1; /*error hopefully reported by open*/
#ifdef USE_SSL
        if (url->proto == PROTO_HTTPS) {
            do {
                res = gnutls_record_send(url->ss, buf, len);
            } while ((res < 0) && handle_ssl_error(url, &res, "write"));
            if (res <= 0) ssl_error(res, url, "write");
        } else
#endif
        {
            res = write(url->sockfd, buf, len);
            if (res <= 0) errno_report("write");
        }
        if ( !(res <= 0) || (url->sock_type != SOCK_KEEPALIVE )) return res;

        /* retry a failed keepalive socket */
        close_client_force(url);
    } while (url->sock_type == SOCK_KEEPALIVE);
    return -1; /*should not reach*/
}

/*
 * Function yields either a positive int after connecting to
 * host 'hostname' on port 'port'  or < 0 in case of error
 *
 * It handles keepalive by not touching keepalive sockets.
 * The SSL context is created so that read/write can use it.
 *
 * hostname is something like 'www.tmtd.de' or 192.168.0.86
 * port is expected in machine order (not net order)
 *
 * ((Flonix  defines USE_IPV6))
 *
 */
#if defined(AF_INET6) && defined(IN6_IS_ADDR_V4MAPPED)
#define USE_IPV6
#endif

static int open_client_socket(struct_url *url) {
#ifdef USE_IPV6
    struct addrinfo hints;
    char portstr[10];
    int gaierr;
    struct addrinfo* ai;
    struct addrinfo* aiv4;
    struct addrinfo* aiv6 = 0;
    struct sockaddr_in6 sa;
#else /* USE_IPV6 */
    struct hostent *he;
    struct sockaddr_in sa;
#endif /* USE_IPV6 */
    socklen_t sa_len;
    int sock_family, sock_type, sock_protocol;

    if(url->sock_type == SOCK_KEEPALIVE) {
        fprintf(stderr, "%s: %s: reusing keepalive socket.\n", argv0, url->tname); /*DEBUG*/
        return url->sock_type;
    }

    if(url->sock_type != SOCK_CLOSED) close_client_socket(url);

    if (url->redirected)
        url->redirect_followed = 1;

    fprintf(stderr, "%s: %s: connecting to %s port %i.\n", argv0, url->tname, url->host, url->port);

    (void) memset((void*) &sa, 0, sizeof(sa));

#ifdef USE_IPV6
    (void) memset(&hints, 0, sizeof(hints));
    hints.ai_family = PF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    (void) snprintf(portstr, sizeof(portstr), "%d", (int) url->port);
    if ((gaierr = getaddrinfo(url->host, portstr, &hints, &ai)) != 0) {
        (void) fprintf(stderr, "%s: %s: getaddrinfo %s - %s\n",
                argv0, url->tname, url->host, gai_strerror(gaierr));
        errno = EIO;
        return -1;
    }

    /* Find the first IPv4 and IPv6 entries. */
    for (aiv4 = ai; aiv4 != NULL; aiv4 = aiv4->ai_next) {
        if (aiv4->ai_family == AF_INET)
            break;
        if ((aiv4->ai_family == AF_INET6) && (aiv6 == NULL))
            aiv6 = aiv4;
    }

    /* If there's an IPv4 address, use that, otherwise try IPv6. */
    if (aiv4 == NULL)
        aiv4 = aiv6;
    if (aiv4 == NULL) {
        (void) fprintf(stderr, "%s: %s: no valid address found for host %s\n",
                argv0, url->tname, url->host);
        errno = EIO;
        return -1;
    }
    if (sizeof(sa) < aiv4->ai_addrlen) {
        (void) fprintf(stderr, "%s: %s: %s - sockaddr too small (%lu < %lu)\n",
                argv0, url->tname, url->host, (unsigned long) sizeof(sa),
                (unsigned long) aiv4->ai_addrlen);
        errno = EIO;
        return -1;
    }
    sock_family = aiv4->ai_family;
    sock_type = aiv4->ai_socktype;
    sock_protocol = aiv4->ai_protocol;
    sa_len = aiv4->ai_addrlen;
    (void) memmove(&sa, aiv4->ai_addr, sa_len);
    freeaddrinfo(ai);

#else /* USE_IPV6 */

    he = gethostbyname(url->host);
    if (he == NULL) {
        (void) fprintf(stderr, "%s: %s: unknown host - %s\n", argv0, url->tname, url->host);
        errno = EIO;
        return -1;
    }
    sock_family = sa.sin_family = he->h_addrtype;
    sock_type = SOCK_STREAM;
    sock_protocol = 0;
    sa_len = sizeof(sa);
    (void) memmove(&sa.sin_addr, he->h_addr, he->h_length);
    sa.sin_port = htons(url->port);

#endif /* USE_IPV6 */

    url->sockfd = socket(sock_family, sock_type, sock_protocol);
    if (url->sockfd < 0) {
        errno_report("couldn't get socket");
        return -1;
    }
    if (connect(url->sockfd, (struct sockaddr*) &sa, sa_len) < 0) {
        errno_report("couldn't connect socket");
        return -1;
    }

#ifdef USE_SSL
    if ((url->proto) == PROTO_HTTPS) {
        /* Make SSL connection. */
        ssize_t r = 0;
        const char * ps = "NORMAL"; /* FIXME allow user setting */
        const char * errp = NULL;
        if (!url->ssl_initialized) {
            r = gnutls_global_init();
            if (!r)
                r = gnutls_certificate_allocate_credentials (&url->sc); /* docs suggest to share creds */
            if (url->cafile) {
                if (!r)
                    r = gnutls_certificate_set_x509_trust_file (url->sc, url->cafile, GNUTLS_X509_FMT_PEM);
                if (r>0)
                    fprintf(stderr, "%s: SSL init: loaded %zi CA certificate(s).\n", argv0, r);
                if (r>0) r = 0;
            }
            if (!r)
                gnutls_certificate_set_verify_function (url->sc, verify_certificate_callback);
            gnutls_certificate_set_verify_flags (url->sc, GNUTLS_VERIFY_ALLOW_X509_V1_CA_CRT /* suggested */
                    | url->md5 | url->md2 ); /* oprional for old cert compat */
            if (!r) url->ssl_initialized = 1;
            gnutls_global_set_log_level((int)url->ssl_log_level);
            gnutls_global_set_log_function(&logfunc);
        }
        if (r) {
            ssl_error(r, url, "SSL init");
            return -1;
        }

        fprintf(stderr, "%s: %s: initializing SSL socket.\n", argv0, url->tname);
        r = gnutls_init(&url->ss, GNUTLS_CLIENT);
        if (!r) gnutls_session_set_ptr(url->ss, url); /* used in cert verifier */
        if (!r) r = gnutls_priority_set_direct(url->ss, ps, &errp);
        if (!r) errp = NULL;
        /* alternative to gnutls_priority_set_direct: if (!r) gnutls_set_default_priority(url->ss); */
        if (!r) r = gnutls_credentials_set(url->ss, GNUTLS_CRD_CERTIFICATE, url->sc);
        if (!r) gnutls_transport_set_ptr(url->ss, (gnutls_transport_ptr_t) (intptr_t) url->sockfd);
        if (!r) r = gnutls_handshake (url->ss);
        do ; while ((r) && handle_ssl_error(url, &r, "opening SSL socket"));
        if (r) {
            close(url->sockfd);
            if (errp) fprintf(stderr, "%s: invalid SSL priority\n %s\n %*s\n", argv0, ps, (int)(errp - ps), "^");
            fprintf(stderr, "%s: %s:%d - ", argv0, url->host, url->port);
            ssl_error(r, url, "SSL connection failed");
            fprintf(stderr, "%s: %s: closing SSL socket.\n", argv0, url->tname);
            gnutls_deinit(url->ss);
            errno = EIO;
            return -1;
        }
        url->ssl_connected = 1; /* Prevent printing cert data over and over again */
    }
#endif
    return url->sock_type = SOCK_OPEN;
}

static void
http_report(const char * reason, const char * method,
        const char * buf, size_t len)
{
    struct_url * url = thread_setup();

    fprintf(stderr, "%s: %s: %s: %s\n", argv0, url->tname, method, reason);
    fwrite(buf, len, 1, stderr);
    if(len && ( *(buf+len-1) != '\n')) fputc('\n', stderr);
}

/*
 * Scan the received header for interesting fields. Since C does not have
 * tools for working with potentially unterminated strings this is quite
 * long and ugly.
 *
 * Return the length of the header in case part of the data was
 * read with the header.
 * Content-Length means different thing whith GET and HEAD.
 */

static ssize_t
parse_header(struct_url *url, const char * buf, size_t bytes,
        const char * method, off_t * content_length, int expect)
{
    /* FIXME check the header parser */
    int status;
    const char * ptr = buf;
    const char * end;
    int seen_accept = 0, seen_length = 0, seen_close = 0, seen_md5 = 0;

    if (bytes <= 0) {
        errno = EINVAL;
        return -1;
    }

    end = memchr(ptr, '\n', bytes);
    if(!end) {
        http_report ( "reply does not contain newline!", method, buf, 0);
        errno = EIO;
        return -1;
    }
    end = ptr;
    while(1){
        end = memchr(end + 1, '\n', bytes - (size_t)(end - ptr));
        if(!end || ((end + 1) >= (ptr + bytes)) ) {
            http_report ("reply does not contain end of header!",
                    method, buf, bytes);
            errno = EIO;
            return -1;
        }
        if(mempref(end, "\n\r\n", bytes - (size_t)(end - ptr), 1)) break;
    }
    ssize_t header_len = (end + 3) - ptr;

    end = memchr(ptr, '\n', bytes);
    char * http = "HTTP/1.1 ";
    if(!mempref(ptr, http, (size_t)(end - ptr), 1) || !isdigit( *(ptr + strlen(http))) ) {
        http_report ("reply does not contain status!",
                method, buf, (size_t)header_len);
        errno = EIO;
        return -1;
    }
    status = (int)strtol( ptr + strlen(http), (char **)&ptr, 10);
    if (status == 301 || status == 302 || status == 307 || status == 303) {
        char * location = "Location: ";
        char * xmd5 = "X-MD5: ";
        int seen_location = 0, seen_md5 = 0;
        char * tmp = 0;
        int res;
        ptrdiff_t llen = (ptrdiff_t) strlen(location);

        while(1) {
            ptr = end+1;
            if( !(ptr < buf + (header_len - 4))){
                if ( !seen_md5 && !url->redirected ) url->xmd5[0] = 0; // response from main server has no X-MD5
                if ( !seen_location) {
                    close_client_force(url);
                    http_report("redirect did not contain a Location header!",
                            method, buf, 0);
                    errno = ENOENT;
                    return -1;
                }
                url->redirect_depth ++;
                if (url->redirect_depth > MAX_REDIRECTS) {
                    fprintf(stderr, "%s: %s: server redirected %i times already. Giving up.", argv0, url->tname, MAX_REDIRECTS);
                    errno = EIO;
                    if (tmp) free(tmp);
                    return -1;
                }

                if (status == 301 && url->redirect_depth == 1) { // change url permanently only if main server asked for it
                    fprintf(stderr, "%s: %s: permanent redirect to %s\n", argv0, url->tname, tmp);

                    res = parse_url(tmp, url, URL_SAVE);
                } else {
                    fprintf(stderr, "%s: %s: temporary redirect to %s\n", argv0, url->tname, tmp);

                    url->redirected = 1;
                    res = parse_url(tmp, url, URL_DROP);
                    //free(tmp);
                }
                if (tmp) free(tmp);

                if(res < 0) {
                    errno = EIO;
                    return res;
                }

                print_url(stderr, url);
                return -EAGAIN;
            }

            end = memchr(ptr, '\n', bytes - (size_t)(ptr - buf));
            if( mempref(ptr, xmd5, (size_t)(end - ptr), 0) ){
                if ( ! url->redirected ){
                    strncpy(url->xmd5,(ptr + strlen(xmd5)), (size_t)(end - ptr) - strlen(xmd5)-1);
                    url->xmd5[32] = 0;
                    seen_md5 = 1;
                }
                fprintf(stderr,"Is in redirect?: %s\n", url->redirected?"yes":"no");
                fprintf(stderr,"X-MD5: %s\n", url->xmd5);
                continue;
            }
            if (mempref(ptr, location, (size_t)(end - ptr), 0) ){
                size_t len = (size_t) (end - ptr - llen);
                if (*(end-1) == '\r') len--; // check for trailing '\r' and remove it
                tmp = malloc(len + 1);

                tmp[len] = 0;
                strncpy(tmp, ptr + llen, len);
                seen_location = 1;
                continue;
/*
                url->redirect_depth ++;
                if (url->redirect_depth > MAX_REDIRECTS) {
                    fprintf(stderr, "%s: %s: server redirected %i times already. Giving up.", argv0, url->tname, MAX_REDIRECTS);
                    errno = EIO;
                    return -1;
                }

                if (status == 301 && url->redirect_depth == 1) { // change url permanently only if main server asked for it
                    fprintf(stderr, "%s: %s: permanent redirect to %s\n", argv0, url->tname, tmp);

                    res = parse_url(tmp, url, URL_SAVE);
                } else {
                    fprintf(stderr, "%s: %s: temporary redirect to %s\n", argv0, url->tname, tmp);

                    url->redirected = 1;
                    res = parse_url(tmp, url, URL_DROP);
                    free(tmp);
                }

                if(res < 0) {
                    errno = EIO;
                    return res;
                }

                print_url(stderr, url);
                return -EAGAIN;
*/
            }
        }
    }
    if (status != expect) {
        fprintf(stderr, "%s: %s: failed with status: %d%.*s.\n",
                argv0, method, status, (int)((end - ptr) - 1), ptr);
        if (!strcmp("HEAD", method)) fwrite(buf, bytes, 1, stderr); /*DEBUG*/
        if (status == 404)
            errno = ENOENT;
        else
            errno = EIO;
        return -1;
    }

    char * content_length_str = "Content-Length: ";
    char * accept = "Accept-Ranges: bytes";
    char * range = "Content-Range: bytes";
    char * date = "Last-Modified: ";
    char * close = "Connection: close";
    char * xmd5 = "X-MD5: ";
    struct tm tm;
    while(1)
    {
        ptr = end+1;
        if( !(ptr < buf + (header_len - 4))){
            if(!seen_md5 && !url->redirected) url->xmd5[0]=0;
            if(seen_accept && seen_length){
                if ( url->redirected ) url->sock_type = SOCK_OPEN; // don't continue with a mirror - need to get md5 from main server
                else {
                    if(url->sock_type == SOCK_OPEN && !seen_close)
                        url->sock_type = SOCK_KEEPALIVE;
                    if(url->sock_type == SOCK_KEEPALIVE && seen_close)
                        url->sock_type = SOCK_OPEN;
                }
                return header_len;
            }
            close_client_force(url);
            errno = EIO;
            if(! seen_accept){
                http_report("server must Accept-Range: bytes",
                        method, buf, 0);
                return -1;
            }
            if(! seen_length){
                http_report("reply didn't contain Content-Length!",
                        method, buf, 0);
                return -1;
            }
            /* fallback - should not reach */
            http_report("error parsing header.",
                    method, buf, 0);
            return -1;

        }
        end = memchr(ptr, '\n', bytes - (size_t)(ptr - buf));

        if( mempref(ptr, xmd5, (size_t)(end - ptr), 0) ){
            if ( !  url->redirected ){
                strncpy(url->xmd5,(ptr + strlen(xmd5)), (size_t)(end - ptr) - strlen(xmd5)-1);
                url->xmd5[32] = 0;
            }
            fprintf(stderr,"Is in redirect?: %s\n", url->redirected?"yes":"no");
            fprintf(stderr,"X-MD5: %s\n", url->xmd5);
            continue;
        }
        if( mempref(ptr, content_length_str, (size_t)(end - ptr), 0)
                && isdigit( *(ptr + strlen(content_length_str))) ){
            *content_length = atoll(ptr + strlen(content_length_str));
            seen_length = 1;
            continue;
        }
        if( mempref(ptr, range, (size_t)(end - ptr), 0) ){
            seen_accept = 1;
            continue;
        }
        if( mempref(ptr, accept, (size_t)(end - ptr), 0) ){
            seen_accept = 1;
            continue;
        }
        if( mempref(ptr, date, (size_t)(end - ptr), 0) ){
            memset(&tm, 0, sizeof(tm));
            if(!strptime(ptr + strlen(date),
                        "%n%a, %d %b %Y %T %Z", &tm)){
                http_report("invalid time",
                        method, ptr + strlen(date),
                        (size_t)(end - ptr) - strlen(date)) ;
                continue;
            }
            url->last_modified = mktime(&tm);
            continue;
        }
        if( mempref(ptr, close, (size_t)(end - ptr), 0) ){
            seen_close = 1;
        }
    }
}

/*
 * Send the header, and get a reply.
 * This relies on 1k reads and writes being generally atomic -
 * - they fit into a single frame. The header should fit into that
 * and we do not need partial read handling so the exchange is simple.
 * However, broken sockets have to be handled here.
 */

static ssize_t
exchange(struct_url *url, char * buf, const char * method,
        off_t * content_length, off_t start, off_t end, size_t * header_length)
{
    ssize_t res;
    size_t bytes;
    int range = (end > 0);

req:
    /* Build request buffer, starting with the request method. */

    bytes = (size_t)snprintf(buf, HEADER_SIZE, "%s %s HTTP/1.1\r\nHost: %s\r\n",
            method, url->path, url->host);
    bytes += (size_t)snprintf(buf + bytes, HEADER_SIZE - bytes,
            "User-Agent: %s %s\r\n", __FILE__, VERSION);
    if (range) bytes += (size_t)snprintf(buf + bytes, HEADER_SIZE - bytes,
            "Range: bytes=%" PRIdMAX "-%" PRIdMAX "\r\n", (intmax_t)start, (intmax_t)end);
#ifdef USE_AUTH
    if ( url->auth )
        bytes += (size_t)snprintf(buf + bytes, HEADER_SIZE - bytes,
                "Authorization: Basic %s\r\n", url->auth);
#endif
    bytes += (size_t)snprintf(buf + bytes, HEADER_SIZE - bytes, "\r\n");

    /* Now actually send it. */
    while(1){
        /*
         * It looks like the sockets abandoned by the server do not go away.
         * Instead of returning EPIPE they allow zero writes and zero reads. So
         * this is the place where a stale socket would be detected.
         *
         * Socket that return EAGAIN cause long delays. Reopen.
         *
         * Reset errno because reads/writes of 0 bytes are a success and are not
         * required to touch it but are handled as error below.
         *
         */
#define CONNFAIL ((res <= 0) && ! errno) || (errno == EAGAIN) || (errno == EPIPE)

        errno = 0;
        res = write_client_socket(url, buf, bytes);

#ifdef RETRY_ON_RESET
        if ((errno == ECONNRESET) && (url->resets < url->retry_reset)) {
            errno_report("exchange: sleeping");
            sleep(1U << url->resets);
            url->resets ++;
            if (close_client_force(url) == -EAGAIN)
                goto req;
            continue;
        }
        url->resets = 0;
#endif
        if (CONNFAIL) {
            errno_report("exchange: failed to send request, retrying"); /* DEBUG */
            if (close_client_force(url) == -EAGAIN)
                goto req;
            continue;
        }
        if (res <= 0){
            errno_report("exchange: failed to send request"); /* DEBUG */
            if (close_client_force(url) == -EAGAIN)
                goto req;
            if (!errno)
                errno = EIO;
            return res;
        }
        res = read_client_socket(url, buf, HEADER_SIZE);
#ifdef RETRY_ON_RESET
        if ((errno == ECONNRESET) && (url->resets < url->retry_reset)) {
            errno_report("exchange: sleeping");
            sleep(1U << url->resets);
            url->resets ++;
            if (close_client_force(url) == -EAGAIN)
                goto req;
            continue;
        }
        url->resets = 0;
#endif
        if (CONNFAIL) {
            errno_report("exchange: did not receive a reply, retrying"); /* DEBUG */
            if (close_client_force(url) == -EAGAIN)
                goto req;
            continue;
        } else if (res <= 0) {
            errno_report("exchange: failed receving reply from server"); /* DEBUG */
            if (close_client_force(url) == -EAGAIN)
                goto req;
            if (!errno)
                errno = EIO;
            return res;
        } else {
            bytes = (size_t)res;
            res = parse_header(url, buf, bytes, method, content_length,
                    range ? 206 : 200);
            if (res == -EAGAIN) /* redirect */
                goto req;

            if (res <= 0){
                http_report("exchange: server error", method, buf, bytes);
                return res;
            }

            if (header_length) *header_length = (size_t)res;

            return (ssize_t)bytes;
        }
    }
}

/*
 * Function uses HEAD-HTTP-Request
 * to determine the file size
 */

static off_t get_stat(struct_url *url, struct stat * stbuf) {
    char buf[HEADER_SIZE];

    if( exchange(url, buf, "HEAD", &(url->file_size), 0, 0, 0) < 0 )
        return -1;

    close_client_socket(url);

    stbuf->st_mtime = url->last_modified;
    return stbuf->st_size = url->file_size;
}


/*
 * get_data does all the magic
 * a GET-Request with Range-Header
 * allows to read arbitrary bytes
 */

static ssize_t get_data(struct_url *url, off_t start, size_t rsize)
{
    char buf[HEADER_SIZE];
    char md5[33];
    const char * b;
    ssize_t bytes;
    off_t end = start + (off_t)rsize - 1;
    char * destination; //  = url->req_buf;
    off_t content_length;
    size_t header_length;
    #ifdef USE_AUTH
    MD5_CTX ctx;
    #endif
    unsigned char xmd5[33]; // 32 digits + null terminator
    size_t size;

    if (fdcache>0)
        if ( (bytes = (ssize_t)get_cached(url, start, rsize)) == (ssize_t)rsize ) return (ssize_t)rsize;

retry:
    destination = url->req_buf;
    size = rsize;

    bytes = exchange(url, buf, "GET", &content_length,
            start, end, &header_length);
    if(bytes <= 0) return -1;

    if (content_length != size) {
        http_report("didn't yield the whole piece.", "GET", 0, 0);
        size = min((size_t)content_length, size);
    }


    b = buf + header_length;

    bytes -= (b - buf);
    memcpy(destination, b, (size_t)bytes);

    #ifdef USE_AUTH
    MD5_Init(&ctx);
    MD5_Update(&ctx, destination, (size_t)bytes);
    #endif

    size -= (size_t)bytes;
    destination +=bytes;
    for (; size > 0; size -= (size_t)bytes, destination += bytes) {

        bytes = read_client_socket(url, destination, size);
        if (bytes < 0) {
            errno_report("GET (read)");
            return -1;
        }
        if (bytes == 0) {
            break;
        }
        MD5_Update(&ctx, destination, (size_t)bytes);
    }

    MD5_Final(xmd5,&ctx);
#if 1
{
    int i;
    for(i = 0; i < 16; i++) sprintf((char*)(md5+(i<<1)), "%02x", xmd5[i]);
    md5[32]=0;
    fprintf(stderr, "XMD5 : %s\n",(char*)url->xmd5);
    fprintf(stderr, "MD5  : %s\n",md5);
    if (strncmp((char*)url->xmd5, (char*)md5, 32) && url->xmd5[0]) {
        close_client_force(url);
        goto retry;
    }
}
#endif
    close_client_socket(url);
    if (fdcache>0) {
        update_cache(url, start, rsize, md5);
    }
    return (ssize_t)(end - start) + 1 - (ssize_t)size;
}
