#ifndef MOD_REWRITE_FUNCS
#define MOD_REWRITE_FUNCS
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

/*
#define APR_WANT_MEMFUNC
#define APR_WANT_STRFUNC
#define APR_WANT_IOVEC
#include "apr_want.h"
*/

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

/* local file */
#include "mod_ml.h"

#define ML_MR_PRG_BUF 1024


/* single linked list used for
 * variable expansion
 */
typedef struct result_list {
    struct result_list *next;
    apr_size_t len;
    const char *string;
} result_list;

/* apache hook function callbacks */
int pre_config(apr_pool_t *pconf, apr_pool_t *plog, apr_pool_t *ptemp);
int post_config(apr_pool_t *p, apr_pool_t *plog, apr_pool_t *ptemp, server_rec *s);
void init_child(apr_pool_t *p, server_rec *s);

apr_status_t ml_mr_program_child(apr_pool_t *p,
                                             const char *progname, char **argv,
                                             apr_file_t **fpout,
                                             apr_file_t **fpin);
apr_status_t run_ml_mr_programs(server_rec *s, ml_directives_t *conf, apr_pool_t *p);
char *ml_mr_send_to_program(
        request_rec *r, 
        apr_file_t *fpin, 
        apr_file_t *fpout, 
        char *input
);
apr_status_t ml_mr_lock_create(server_rec *s, apr_pool_t *p);
apr_status_t ml_mr_lock_remove(void *data);

#endif /* MOD_REWRITE_FUNCS */
