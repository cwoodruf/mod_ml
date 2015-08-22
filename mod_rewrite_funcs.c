/*
 * code from the mapping feature of mod_rewrite that can be reused in mod_ml
 * in mod_rewrite.c most of these functions were static/private
 * what is of interest here is the code for running external programs
 * that can act as an intermediary to a machine learning program
 * in some cases we care about the output (field cleanup and classification)
 * in some cases we don't care about the output (preprocessing/model building)
 */

/* note to self: which of these can we safely leave out? */
#include "apr.h"
#include "apr_strings.h"
#include "apr_hash.h"
#include "apr_user.h"
#include "apr_lib.h"
#include "apr_signal.h"
#include "apr_global_mutex.h"

#if APR_HAS_THREADS
#include "apr_thread_mutex.h"
#endif

#define APR_WANT_MEMFUNC
#define APR_WANT_STRFUNC
#define APR_WANT_IOVEC
#include "apr_want.h"

#include "ap_config.h"
#include "httpd.h"
#include "http_config.h"
#include "http_request.h"
#include "http_core.h"
#include "http_log.h"
#include "http_protocol.h"
#include "http_vhost.h"
#include "util_mutex.h"

/// #include "mod_rewrite_funcs.h"

/*
 * code from the mapping feature of mod_rewrite that can be reused in mod_ml
 * in mod_rewrite.c most of these functions were static/private
 * what is of interest here is the code for running external programs
 * that can act as an intermediary to a machine learning program
 * in some cases we care about the output (field cleanup and classification)
 * in some cases we don't care about the output (preprocessing/model building)
 */
#include "apr.h"
#include "apr_strings.h"
#include "apr_hash.h"
#include "apr_user.h"
#include "apr_lib.h"
#include "apr_signal.h"
#include "apr_global_mutex.h"

#if APR_HAS_THREADS
#include "apr_thread_mutex.h"
#endif

#include "ap_config.h"
#include "httpd.h"
#include "http_config.h"
#include "http_request.h"
#include "http_core.h"
#include "http_log.h"
#include "http_protocol.h"
#include "http_vhost.h"
#include "util_mutex.h"

#include "ap_expr.h"

#include "mod_rewrite_funcs.h"
#include "mod_ml.h"

#define ML_MR_PRG_BUF 1024

/* Locks/Mutexes */
static apr_global_mutex_t *ml_mr_lock_acquire = NULL;
static const char *ml_mr_mutex_type = "mod-ml-lock";

/* child process code */
static void ml_mr_child_errfn(apr_pool_t *p, apr_status_t err,
                                const char *desc)
{
    ap_log_error(APLOG_MARK, APLOG_ERR, err, NULL, APLOGNO(00653) "%s", desc);
}

apr_status_t ml_mr_program_child(apr_pool_t *p, const char *progname, char **argv, apr_file_t **fpout, apr_file_t **fpin)
{
    apr_status_t rc;
    apr_procattr_t *procattr;
    apr_proc_t *procnew;

    if (   APR_SUCCESS == (rc=apr_procattr_create(&procattr, p))
        && APR_SUCCESS == (rc=apr_procattr_io_set(procattr, APR_FULL_BLOCK,
                                                  APR_FULL_BLOCK, APR_NO_PIPE))
        && APR_SUCCESS == (rc=apr_procattr_dir_set(procattr,
                                             ap_make_dirstr_parent(p, progname)))
        && APR_SUCCESS == (rc=apr_procattr_cmdtype_set(procattr, APR_PROGRAM))
        && APR_SUCCESS == (rc=apr_procattr_child_errfn_set(procattr,
                                                           ml_mr_child_errfn))
        && APR_SUCCESS == (rc=apr_procattr_error_check_set(procattr, 1))) {

        procnew = apr_pcalloc(p, sizeof(*procnew));
        rc = apr_proc_create(procnew, progname, (const char **)argv, NULL,
                             procattr, p);

        if (rc == APR_SUCCESS) {
            apr_pool_note_subprocess(p, procnew, APR_KILL_AFTER_TIMEOUT);

            if (fpin) {
                (*fpin) = procnew->in;
            }

            if (fpout) {
                (*fpout) = procnew->out;
            }
        }
    }
    return (rc);
}

apr_status_t run_ml_mr_program(server_rec *s, apr_pool_t *p, ml_proc_t *map)
{
    apr_file_t *fpin = NULL;
    apr_file_t *fpout = NULL;
    if (map->proctype != ML_PROC) {
        return APR_SUCCESS;
    }
    if (map->helper == NULL) return APR_SUCCESS;

    ml_exec_t *helper = (ml_exec_t *)map->helper;
    if (!(map->proc) || !*(map->proc)) {
        return APR_SUCCESS;
    }

    if (map->proc == NULL || strlen(map->proc) == 0) return APR_SUCCESS;

    int rc = ml_mr_program_child(p, map->proc, helper->argv, &fpout, &fpin);
    if (rc != APR_SUCCESS || fpin == NULL || fpout == NULL) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rc, s, APLOGNO(00654)
                     "mod_ml: could not start process "
                     "program %s", map->proc);
        return rc;
    }
    helper->fpin  = fpin;
    helper->fpout = fpout;
    return APR_SUCCESS;
}
    

apr_status_t run_ml_mr_programs(server_rec *s, ml_directives_t *conf, apr_pool_t *p)
{
    apr_hash_index_t *hi;
    apr_status_t rc;

    /*  If the engine isn't turned on,
     *  don't even try to do anything.
     */
    if (conf->enabled == ML_DISABLED) {
        return APR_SUCCESS;
    }

    /* do the cleaner programs first */
    for (hi = apr_hash_first(p, conf->cleaners); hi; hi = apr_hash_next(hi)){
        void *val;
        int rc;
        apr_hash_this(hi, NULL, NULL, &val);
        if ((rc = run_ml_mr_program(s,p,val)) != APR_SUCCESS) return rc;
    }

    /* for the following most likely there will only be one each for a given conf */

    /* next the preprocessor(s) */
    ml_proc_t *procs;
    procs = (ml_proc_t *)conf->preprocessor->elts;
    for (int i=0; i<conf->preprocessor->nelts; i++) {
        if ((rc = run_ml_mr_program(s,p,&procs[i])) != APR_SUCCESS) return rc;
    }

    /* finally the classifier(s) */
    procs = (ml_proc_t *)conf->classifier->elts;
    for (int i=0; i<conf->classifier->nelts; i++) {
        if ((rc = run_ml_mr_program(s,p,&procs[i])) != APR_SUCCESS) return rc;
    }

    return APR_SUCCESS;
}


char *ml_mr_send_to_program(
        request_rec *r, 
        apr_file_t *fpin, 
        apr_file_t *fpout, 
        char *input
) {
    char *buf;
    char c;
    apr_size_t i, nbytes, combined_len = 0;
    apr_status_t rv;
    const char *eol = APR_EOL_STR;
    apr_size_t eolc = 0;
    int found_nl = 0;
    result_list *buflist = NULL, *curbuf = NULL;
    if (fpin == NULL || fpout == NULL || ap_strchr(input, '\n')) {
        return NULL;
    }

    /* take the lock */
    if (ml_mr_lock_acquire) {
        rv = apr_global_mutex_lock(ml_mr_lock_acquire);
        if (rv != APR_SUCCESS) {
            ap_log_rerror(APLOG_MARK, APLOG_ERR, rv, r, APLOGNO(00659)
                          "apr_global_mutex_lock(ml_mr_lock_acquire) "
                          "failed");
            return NULL; /* Maybe this should be fatal? */
        }
    }
    nbytes = strlen(input);
    /* XXX: error handling */
    apr_file_write_full(fpin, input, nbytes, NULL);
    nbytes = 1;
    apr_file_write_full(fpin, "\n", nbytes, NULL);

    buf = apr_pcalloc(r->pool, ML_MR_PRG_BUF + 1);

    /* read in the response value */
    nbytes = 1;
    apr_file_read(fpout, &c, &nbytes);
    do {
        i = 0;
        while (nbytes == 1 && (i < ML_MR_PRG_BUF)) {
            if (c == eol[eolc]) {
                if (!eol[++eolc]) {
                    /* remove eol from the buffer */
                    --eolc;
                    if (i < eolc) {
                        curbuf->len -= eolc-i;
                        i = 0;
                    }
                    else {
                        i -= eolc;
                    }
                    ++found_nl;
                    break;
                }
            }

            /* only partial (invalid) eol sequence -> reset the counter */
            else if (eolc) {
                eolc = 0;
            }

            /* catch binary mode, e.g. on Win32 */
            else if (c == '\n') {
                ++found_nl;
                break;
            }

            buf[i++] = c;
            apr_file_read(fpout, &c, &nbytes);
        }

        /* well, if there wasn't a newline yet, we need to read further */
        if (buflist || (nbytes == 1 && !found_nl)) {
            if (!buflist) {
                curbuf = buflist = apr_palloc(r->pool, sizeof(*buflist));
            }
            else if (i) {
                curbuf->next = apr_palloc(r->pool, sizeof(*buflist));
                curbuf = curbuf->next;

            }
            curbuf->next = NULL;

            if (i) {
                curbuf->string = buf;
                curbuf->len = i;
                combined_len += i;
                buf = apr_palloc(r->pool, ML_MR_PRG_BUF + 1);
            }

            if (nbytes == 1 && !found_nl) {
                continue;
            }
        }

        break;
    } while (1);

    /* concat the stuff */
    if (buflist) {
        char *p;

        p = buf = apr_palloc(r->pool, combined_len + 1); /* \0 */
        while (buflist) {
            if (buflist->len) {
                memcpy(p, buflist->string, buflist->len);
                p += buflist->len;
            }
            buflist = buflist->next;
        }
        *p = '\0';
        i = combined_len;
    }
    else {
        buf[i] = '\0';
    }
    /* give the lock back */
    if (ml_mr_lock_acquire) {
        rv = apr_global_mutex_unlock(ml_mr_lock_acquire);
        if (rv != APR_SUCCESS) {
            ap_log_rerror(APLOG_MARK, APLOG_ERR, rv, r, APLOGNO(00660)
                          "apr_global_mutex_unlock(ml_mr_lock_acquire) "
                          "failed");
            return NULL; /* Maybe this should be fatal? */
        }
    }
    return buf;
}

apr_status_t ml_mr_lock_create(server_rec *s, apr_pool_t *p)
{
    apr_status_t rc;

    /* create the lockfile */
    rc = ap_global_mutex_create(&ml_mr_lock_acquire, NULL,
                                ml_mr_mutex_type, NULL, s, p, 0);
    if (rc != APR_SUCCESS) {
        return rc;
    }

    return APR_SUCCESS;
}

apr_status_t ml_mr_lock_remove(void *data)
{
    /* destroy the ml_mr_lock */
    if (ml_mr_lock_acquire) {
        apr_global_mutex_destroy(ml_mr_lock_acquire);
        ml_mr_lock_acquire = NULL;
    }
    return APR_SUCCESS;
}

/* 
 * some initialization stuff based on mod_rewrite.c 
 * need this for starting processes using files in mod_rewrite_funcs.c
 */

int pre_config(apr_pool_t *pconf, apr_pool_t *plog, apr_pool_t *ptemp)
{
   ap_mutex_register(pconf, ml_mr_mutex_type, NULL, APR_LOCK_DEFAULT, 0);
   return OK;
}

int post_config(apr_pool_t *p, apr_pool_t *plog, apr_pool_t *ptemp, server_rec *s)
{
    apr_status_t rv;
    rv = ml_mr_lock_create(s, p);
    if (rv != APR_SUCCESS) { return HTTP_INTERNAL_SERVER_ERROR; }
    apr_pool_cleanup_register(p, (void *)s, ml_mr_lock_remove, apr_pool_cleanup_null);
    return OK;
}

void init_child(apr_pool_t *p, server_rec *s)
{
    apr_status_t rv = 0; 
    if (ml_mr_lock_acquire) {
        rv = apr_global_mutex_child_init(&ml_mr_lock_acquire,
                         apr_global_mutex_lockfile(ml_mr_lock_acquire), p);
        if (rv != APR_SUCCESS) {
            ap_log_error(APLOG_MARK, APLOG_CRIT, rv, s, APLOGNO(00666)
                         "mod_ml: could not init ml_mr_lock_acquire"
                          " in child");
        }
    }
}

