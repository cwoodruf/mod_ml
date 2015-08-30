
/* 
 *    mod_ml - apache interface for machine learning
 *
 *    Idea of this module is we can do feature selection and 
 *    some basic processing before sending off data to 
 *    an exteral process. This process can return a result 
 *    which we can then use to automatically modify the request. 
 *
 *    Presumably doing things this way is more efficient
 *    than logging then processing or doing classification
 *    at the application level.
 *
 *    Some use cases:
 *    - detecting user traits (e.g. are they bots)
 *      where there may be no content handling
 *    - machine learning outside of the scope of 
 *      a specific virtual host
 *    - applying analytics decisions before 
 *      content generation 
 */ 

#ifndef MOD_ML
#define MOD_ML

#include <stdarg.h>
/* 
 * for the unix socket stuff 
 * probably means this won't work completely windows
 */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
/// for printing env
#include <unistd.h>

#include "httpd.h"
#include "http_config.h"
#include "http_core.h"
#include "http_log.h"
#include "http_protocol.h"
#include "http_request.h"
#include "http_main.h"

#include "apr_strings.h"
#include "ap_config.h"
#include "apr_tables.h"
#include "util_script.h"
#include "ap_regex.h"
 
#if APR_HAS_THREADS
#include "apr_thread_mutex.h"
#endif

/* this is a local file */
#include "mod_rewrite_funcs.h"

/* max length of a time format string */
#define ML_TIME_LEN 128

#define ML_AHKS APR_HASH_KEY_STRING

/* ML_DEBUG only affects logging to stdout*/
#ifndef ML_DEBUG
/* #define ML_DEBUG 0 */
#define ML_DEBUG 1
#endif

#ifndef ML_LOGLV
#define ML_LOGLV APLOG_TRACE1
/* #define ML_LOGLV APLOG_ERR */
#endif

/* _ml_log logs to stdout on start */
#define _ml_log(...) if (ML_DEBUG >= 1) { ap_log_error(APLOG_MARK, ML_LOGLV, 0, NULL, __VA_ARGS__ ); }

/* context aware logging - should show up in apache error log */
#define _ml_clog(ctx,...) ap_log_error(APLOG_MARK, ML_LOGLV, 0, ctx, __VA_ARGS__ ); 
#define _ml_rclog(rc,ctx,...) ap_log_error(APLOG_MARK, ML_LOGLV, rc, ctx, __VA_ARGS__ );

/* get various things from the map hashes for directive parsing */
#define _ml_ft(fieldtype) (apr_hash_get(ml_fieldtype_map, fieldtype, APR_HASH_KEY_STRING))
#define _ml_pt(proctype) (apr_hash_get(ml_proctype_map, proctype, APR_HASH_KEY_STRING))
#define _ml_cr(crtype) (apr_hash_get(ml_cr_map, crtype, APR_HASH_KEY_STRING))


/* how to store the directives  */

/* on or off? */
typedef enum {
    ML_DISABLED = 0,
    ML_ENABLED
} ml_enabled;

/* when is the proc being run? */
typedef enum {
    ML_NOTRUN = 0,
    ML_CLEANER,
    ML_PREPROCESSOR,
    ML_CLASSIFIER,
    ML_CLASSRESPONSE
} ml_role;
static char *ml_role_names[] = {
    "notrun", "cleaner", "preprocessor", 
    "classifier", "class response"
};
/* 
 * enum relating to first arg of MLFeatures/MLLabels directive
 * where do we look for this field? 
 */
typedef enum {
    ML_FIELD_ERROR = 0,
    ML_CGI,
    ML_HEADER,
    ML_ENV,
    ML_COOKIE,
    ML_URI,
    ML_TIME,
    ML_LITERAL,
    ML_REQ,
    ML_AUTH
} ml_fieldtype;

static ml_fieldtype ml_ft[] = {
    ML_FIELD_ERROR,
    ML_CGI,
    ML_HEADER,
    ML_ENV,
    ML_COOKIE,
    ML_URI,
    ML_TIME,
    ML_LITERAL,
    ML_REQ,
    ML_AUTH
};
/* map of description of a type of process to the enum value */
static apr_hash_t *ml_fieldtype_map = NULL;
static char *ml_fieldtype_names[] = { 
    "_error!_", "cgi", "header", "env", "cookie", 
    "uri", "time", "literal", "request", "auth" 
};

/* what type of {field|preprocessor|classifier} processor is this? 
 * want the values for ml_proctype and ml_classifier to match up
 * for the IP, PROC and SOCK categories of external processes
 */
typedef enum {
    ML_PROC_MISSING = 0,
    /* these need to match the ML_CLASS_ versions */
    ML_IP = 1,       /* expects IP:PORT string */
    ML_PROC = 2,
    ML_SOCK = 3,
    ML_REGEX
} ml_proctype;

/* map of description of a type of process to the enum value */
static apr_hash_t *ml_proctype_map = NULL;
static char *ml_proctype_names[] = { "_missing!_", "ip", "script", "sock", "regex" };

/* for the hash which expects a pointer as a value */
static ml_proctype ml_pt[] = {
    ML_PROC_MISSING,
    ML_IP,
    ML_PROC,
    ML_SOCK,
    ML_REGEX
};

/* what type of response do we make based on a classification? */
typedef enum {
    ML_CLASS_NONE = 0,  /* by default don't do anything */
    /* these need to match the ML_ versions */
    ML_CLASS_IP = 1,    /* send output to external process */
    ML_CLASS_PROC = 2,  /* send output to internal process */
    ML_CLASS_SOCK = 3,  /* send output to unix socket */
    ML_CLASS_CGI,       /* create cgi field with response */
    ML_CLASS_HEADER,    /* create this header with response */
    ML_CLASS_ENV,       /* set this environment variable with response */
    ML_CLASS_COOKIE,    /* set cookie with this response */
    ML_CLASS_HTTP,      /* send an http response code */
    ML_CLASS_REDIRECT   /* modify uri and make a new request */

} ml_classresponse;

/* for setting ml_cr_map hash values */
static ml_classresponse ml_cr[] = {
    ML_CLASS_NONE,
    ML_CLASS_IP,     
    ML_CLASS_PROC,  
    ML_CLASS_SOCK,
    ML_CLASS_CGI,
    ML_CLASS_HEADER,
    ML_CLASS_ENV,
    ML_CLASS_COOKIE,
    ML_CLASS_HTTP,
    ML_CLASS_REDIRECT
};
/* use to translate cr strings into ml_classresponses */
static apr_hash_t *ml_cr_map = NULL;
/* for translating back into strings in log msgs */
static char *ml_crtypes[] = { 
    "_no_response!_", 
    "ip", "proc", "sock",
    "cgi", "header", "env", "cookie", "http", "redirect",
};

/* how to format feature strings */
typedef enum {
    ML_OUT_RAW = 0,
    ML_OUT_QUOTED,
    ML_OUT_JSON,
    ML_OUT_JSONFLDS,
    ML_OUT_JSONARY,
    ML_OUT_CSV,
    ML_OUT_NONE
} ml_outformat;

/* features|vars to send to preprocessor or classifier */
typedef struct ml_feature {
    ml_fieldtype fieldtype;
    char *name;             /* what to display */
    char *field;            /* what to actually get */
    char *raw;              /* data from the request */
    char *clean;            /* data after being processed with a cleaner */
} ml_feature_t;

/* helper for regular expressions */
typedef struct ml_regex {
    ap_rxplus_t *rx;
    char *pat;
    int cflags;
} ml_regex_t;

/* helper for external executables */
typedef struct ml_exec {
    apr_file_t *fpin;
    apr_file_t *fpout;
    char *argv[2];          /* first arg is always program name and second is always null */
} ml_exec_t;

/* helper for ip requests */
typedef struct ml_ip {
    char *host;
    char *scope;
    apr_port_t port;
    int family;
} ml_ip_t;

/* helper for unix sockets */
typedef struct ml_sock {
    char *path;
} ml_sock_t;

/* store processor information */
typedef struct ml_proc {
    ml_role role;                 /* clean, preprocess, classify, class response */
    ml_proctype proctype;         /* where are we sending data? */
    char *proc;                   /* name of helper process|ip|socket|regex */
    void *helper;                 /* what this is depends on proctype */
    /* specific features for processors, classifiers and class responses */
    ml_outformat outformat;       /* how to make output strings */
    apr_array_header_t *features; /* features associated with this proc */
    apr_array_header_t *vars;   /* vars associated with this proc */
    apr_hash_t *classresponse;    /* actions associated with class responses */
    struct ml_proc *def_fp;       /* how to clean fields w/o a specific cleaner */
    struct ml_proc *out_fp;       /* how to clean output */
} ml_proc_t;

/* for keeping track of which cleaner to use for which field */
typedef struct ml_proc_key {
    const char *pkey;
} ml_proc_key_t;
    
/* map a field to a cleaner */
typedef struct ml_field_proc {
    ml_fieldtype fieldtype; /* used for key into features list */
    char *field;            /* used for key into features list */
    apr_array_header_t *cleaners; 
                            /* list of ml_proc_keys for this field */
} ml_field_proc_t;

/* list of class vs response */
typedef struct ml_cr {
    char *class;            /* what to look for from classifier */
    char *action;           /* what to do with it */
    char *args;             /* any extra info for an action */
    ml_classresponse crtype;/* how to interpret action */
    ml_proc_t *proc;        /* for the ip, proc and sock responses  */
} ml_cr_t;

/* our directives */
typedef struct {
    ml_enabled enabled; 
    unsigned int save_features;         /* flag to indicate feature list started */
    unsigned int reset_features;        /* flag creation of new feature list */
    char * dir;
    server_rec *s;
    apr_hash_t *fieldprocs;             /* how to change raw input into features */
    ml_proc_t *def_fp;                  /* how to clean fields w/o a specific cleaner */
    ml_proc_t *out_fp;                  /* how to clean output */
    apr_hash_t *cleaners;               /* used by fieldprocs to fix up input fields */
    apr_array_header_t *features;       /* feature input into preprocessor and classifier */
    apr_table_t *cgi;                   /* place to hold cgi data if needed */
    apr_array_header_t *vars;           /* vars for internal use - e.g modifying ip procs  */
    ml_outformat outformat;             /* how to make feature strings */
    apr_array_header_t *preprocessor;   /* how to preprocess input - doesn't send a response */
    apr_array_header_t *classifier;     /* how to classify input - sends a response */
    apr_hash_t *classresponse;          /* what to change or add based on classification */
    request_rec *r;                     /* if we have request_rec save it here */
    int is_handler;                     /* only true if we are being run as a handler */

} ml_directives_t;

#endif /* MOD_ML */
