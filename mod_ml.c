/* 
 *    mod_ml.c - apache interface for machine learning
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

#include <stdarg.h>
#ifdef linux
/* 
 * for the unix socket stuff 
 * probably means this won't work completely windows
 * or compile without compilation directives
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
#endif /* linux */

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
#include "apr_random.h"
#include "apr_sha1.h"
#include "util_script.h"
#include "ap_regex.h"
 
#if APR_HAS_THREADS
#include "apr_thread_mutex.h"
#endif
#include "util_mutex.h"

/* local files */
#include "mod_rewrite_funcs.h"
#include "mod_ml.h"

/* 
 * these functions make key strings for various hashes
 * have to use strings in the hash or keys that are the same get lost (?) 
 */
/* key for the field processor hash */
static const APR_INLINE char *_ml_fkey(
        apr_pool_t *p, 
        const ml_feature_t *fp
) {
    return apr_psprintf(p, "%d:%s", fp->fieldtype, fp->field);
}

/* key for processor hash - uses explicit proctype and proc */
static const APR_INLINE char *_ml_pkey2(
        apr_pool_t *p, 
        const ml_proctype pt, 
        const char *proc
) {
    return apr_psprintf(p, "%d:%s", pt, proc);
}

/* 
 * given such a complicated config need some way to view it 
 * display a list of features or labels and their associated cleanup helpers 
 */
static char * _ml_feature_str(
        const char *what,
        apr_array_header_t *feats,
        apr_hash_t *cleaners,
        apr_hash_t *fieldprocs,
        char **confv,
        apr_pool_t *p
) {
    char *conf = *confv;

    if (feats == NULL) return conf;
    if (cleaners == NULL) return conf;
    if (fieldprocs == NULL) return conf;

    conf = apr_pstrcat(p, conf, 
            apr_psprintf(p, 
                "%d %s\n", feats->nelts, what), NULL);

    ml_feature_t *features = (ml_feature_t *)feats->elts;
    for (int i=0; i<feats->nelts; i++) {
        conf = apr_pstrcat(p, conf, 
                apr_psprintf(p, "%d: key: %s feat: %s (%s) raw %s clean %s", 
                    i, _ml_fkey(p, &features[i]), features[i].field, 
                    ml_fieldtype_names[features[i].fieldtype],
                    features[i].raw, features[i].clean), NULL);
        if (fieldprocs != NULL) {
            ml_field_proc_t *field = 
                apr_hash_get(fieldprocs, 
                        _ml_fkey(p, &features[i]), 
                        ML_AHKS);
            if (field != NULL && field->cleaners != NULL) {
                ml_proc_key_t *fcs = (ml_proc_key_t *)field->cleaners->elts;
                for (int j=0; j<field->cleaners->nelts; j++) {
                    ml_proc_t *proc = NULL;
                    if (cleaners != NULL) {
                        proc = apr_hash_get(cleaners, fcs[j].pkey, ML_AHKS);
                    }
                    if (proc) {
                        conf = apr_pstrcat(p, conf, 
                                apr_psprintf(p, " cleaner: %s (%s)", 
                                    proc->proc, 
                                    ml_proctype_names[proc->proctype]), NULL);
                    }
                }
            }
        }
        conf = apr_pstrcat(p, conf, "\n", NULL);
    }
    return conf;
}

/* make class responses string */
static char *_ml_cr_str(
        const char *what, 
        apr_hash_t *classresponse, 
        char **confv, 
        apr_pool_t *p
) {
    char *conf = *confv;
    if (classresponse == NULL) return conf;

    conf = apr_pstrcat(p, conf, apr_psprintf(p, "%s\n", what), NULL);

    char *key;
    apr_array_header_t *clist;

    apr_hash_index_t *hi = apr_hash_first(p, classresponse);
    for (; hi; hi = apr_hash_next(hi)) 
    {
        key = (char *)apr_hash_this_key(hi);
        clist = (apr_array_header_t *)apr_hash_this_val(hi);
        ml_cr_t *crs = (ml_cr_t *)clist->elts;

        for (int i=0; i<clist->nelts; i++) {
            char *class = crs[i].class;
            char *action = crs[i].action;
            ml_classresponse crtype = crs[i].crtype;
            conf = apr_pstrcat(p, conf, 
                    apr_psprintf(p, 
                        "class %s (%s) response type %s action %s\n", 
                        class, key, ml_crtypes[crtype], action), NULL);
        }
    }
    return conf;
}

/* print details processors including fields and class responders */
static char *_ml_proc_str(
        const char *what,
        ml_directives_t *sc,
        apr_array_header_t *procs,
        char **confv,
        apr_pool_t *p
) {
    char *conf = *confv;
    conf = apr_pstrcat(p, apr_psprintf(p, conf, "%d %s\n", procs->nelts, what), NULL);
    ml_proc_t *proc = (ml_proc_t *)procs->elts;
    for (int i; i<procs->nelts; i++) {
        conf = apr_pstrcat(p, conf, apr_psprintf(p, "processor %s %s\n", 
                    ml_role_names[proc[i].role], proc[i].proc), NULL);

        conf = _ml_feature_str("features", 
                proc[i].features, sc->cleaners, sc->fieldprocs, &conf, p);
        conf = _ml_feature_str("labels", 
                proc[i].labels, sc->cleaners, sc->fieldprocs, &conf, p);
        if (proc[i].role == ML_CLASSIFIER) 
            conf = _ml_cr_str(apr_psprintf(p, "crs for %s", proc[i].proc), 
                                proc[i].classresponse, &conf, p);
        conf = apr_pstrcat(p, conf,
                apr_psprintf(p, "end %s conf\n", proc[i].proc), NULL);
    }
    return conf;
}

/* make a string with our entire config in it */
static char * _ml_conf_str(
        ml_directives_t *sc, 
        apr_pool_t *p 
) {
    if (sc == NULL) return NULL;
    char *conf = apr_psprintf(p, "\nML config\n");
    conf = apr_pstrcat(p, conf, 
            apr_psprintf(p, "enabled? %d\n", sc->enabled), NULL);

    if (sc->preprocessor != NULL) {
        conf = _ml_proc_str("preprocessors", sc, sc->preprocessor, &conf, p);
    }
    if (sc->classifier != NULL) {
        conf = _ml_proc_str("classifiers", sc, sc->classifier, &conf, p);
    }
    if (sc->features != NULL) {
        conf = _ml_feature_str("left over features", 
                sc->features, sc->cleaners, sc->fieldprocs, &conf, p);
    }
    if (sc->labels != NULL) {
        conf = _ml_feature_str("left over labels", 
                sc->labels, sc->cleaners, sc->fieldprocs, &conf, p);
    }
    if (sc->classresponse != NULL) {
        conf = _ml_cr_str("left over crs", sc->classresponse, &conf, p);
    }
    if (sc->cgi) {
        apr_hash_index_t *hi;
        conf = apr_pstrcat(p, conf, "CGI data\n", NULL);
        const apr_array_header_t *fields = apr_table_elts(sc->cgi);
        apr_table_entry_t *e = (apr_table_entry_t *) fields->elts;
        for(int i = 0; i < fields->nelts; i++) {
            conf = apr_pstrcat(p, conf, 
                    apr_psprintf(p, "%s = %s\n", e[i].key, e[i].val), NULL);
        }
    }
    if (sc->r && sc->r->subprocess_env) {
        conf = apr_pstrcat(p, conf, "Subprocess environment\n", NULL);
        const apr_array_header_t *envary = 
                                apr_table_elts(sc->r->subprocess_env);
        apr_table_entry_t *env = (apr_table_entry_t *)envary->elts;
        for (int i = 0; i<envary->nelts; i++) {
            conf = apr_pstrcat(p, conf,
                    apr_psprintf(p, "env var %s = %s\n", 
                        env[i].key, env[i].val),NULL);
        }
    }
    return conf;
}

/* log our configuration somewhere log or stdout */
static void _ml_log_conf_tok(
        server_rec *ctx, 
        apr_pool_t *p, 
        ml_directives_t *conf, 
        char *where
) {
    _ml_clog(ctx,"%s",where);
    char *confstr = _ml_conf_str(conf, p);
    char *last;
    char *line = apr_strtok(confstr, "\n", &last);
    while (line) {
        _ml_clog(ctx, "%s", line);
        line = apr_strtok(NULL, "\n", &last);
    }
}
/* for logging during startup */
static void _ml_log_conf(
        apr_pool_t *p, 
        ml_directives_t *conf, 
        char *where
) {
    _ml_log_conf_tok(NULL, p, conf, where);
}
/* for logging during a request */
static void _ml_clog_conf(request_rec *r, ml_directives_t *conf, char *where)
{
    _ml_log_conf_tok(r->server, r->pool, conf, where);
}

/*
 ==============================================================================
 Functions to handle configuration
 ==============================================================================
 */
/*
 * turns module on or off
 */
static const char *ml_set_enabled(cmd_parms *cmd, void *conf, const char *arg)
{
    ml_directives_t *config = conf;
    if(!strcasecmp(arg, "on")) config->enabled = ML_ENABLED;
    else config->enabled = ML_DISABLED;
    _ml_log("enabled %d", config->enabled);
    return NULL;
}

/* 
 * output format directive - how strings of fields are sent 
 * to classifiers and preprocessors
 */
static const char *ml_set_outformat(
        cmd_parms *cmd, 
        void *conf, 
        const char *arg
) {
    ml_directives_t *config = conf;
    if(!strcasecmp(arg, "raw")) 
        config->outformat = ML_OUT_RAW;

    else if(!strcasecmp(arg, "quoted")) 
        config->outformat = ML_OUT_QUOTED;

    else if(!strcasecmp(arg, "json")) 
        config->outformat = ML_OUT_JSON;

    else if(!strcasecmp(arg, "jsonarray")) 
        config->outformat = ML_OUT_JSONARY;

    else if(!strcasecmp(arg, "jsonfields")) 
        config->outformat = ML_OUT_JSONFLDS;

    else if(!strcasecmp(arg, "csv")) 
        config->outformat = ML_OUT_CSV;

    return NULL;
}  

/*
 * helpers are processors of input
 * they can take on 3 different roles
 * ML_CLEANER - modify individual fields
 * ML_PREPROCESS - feed a set of fields to some external process
 * ML_CLASSIFY - feed a set of fields to some external process
 *               and do something with a returned result
 * the processor can be a regex or an
 * ip address to an external service, 
 * a unix socket or an external process run by this module
 */
static void _ml_proc_set_helper(apr_pool_t *p, ml_proc_t *fp) 
{
    if (fp->proc == NULL || strlen(fp->proc) == 0) {
        _ml_log("no proc found in _ml_proc_set_helper!");
        return;
    }
    switch (fp->proctype) {
        case ML_REGEX:
            {
                fp->helper = apr_pcalloc(p, sizeof(ml_regex_t));
                ml_regex_t *helper = (ml_regex_t *)fp->helper;
                helper->rx = ap_rxplus_compile(p, fp->proc);
                helper->pat = fp->proc;
                if (helper->rx == NULL) 
                    _ml_log("failed to make %s into regex", fp->proc);
            }
            break;
        case ML_PROC: 
            {
                fp->helper = apr_pcalloc(p, sizeof(ml_exec_t));
                ml_exec_t *helper = fp->helper;
                helper->argv[0] = apr_pstrdup(p, fp->proc);
                /* exec used by apr will fail w/o this */
                helper->argv[1] = NULL; 

                /* this only seems to work when it is started here 
                 * but what if it fails? 
                 */
                int rc = ml_mr_program_child(p, 
                        fp->proc, helper->argv, &helper->fpout, &helper->fpin);
                _ml_log("got return code %d at %d", rc, __LINE__);
                if (rc != APR_SUCCESS || !helper->fpin || !helper->fpout) {
                    ap_log_error(APLOG_MARK, APLOG_ERR, rc, NULL, 
                                 "mod_ml: could not start process "
                                 "program %s", fp->proc);
                }
            }
            break;
        case ML_IP:
            {
                char *host;
                char *scope;
                apr_port_t port;
                int rc = apr_parse_addr_port(
                        &host, &scope, &port, fp->proc, p);
                if (rc != APR_SUCCESS || host == NULL || port == 0) {
                    ap_log_error(APLOG_MARK, APLOG_ERR, rc, NULL, 
                                 "mod_ml: no host:port in %s", fp->proc);
                    return;
                }
                fp->helper = apr_pcalloc(p, sizeof(ml_ip_t));
                ml_ip_t *helper = fp->helper;
                helper->host = host;
                helper->scope = scope;
                helper->port = port;
                helper->family = (strchr(host,':') ? APR_INET6 : APR_INET);
            }
            break;
        case ML_SOCK:
            {
                if (fp->proc == NULL || !(*fp->proc)) {
                    ap_log_error(APLOG_MARK, APLOG_ERR, 0, NULL, APLOGNO(00666)
                                 "mod_ml: empty socket path");
                    return;
                }
                fp->helper = apr_pcalloc(p, sizeof(ml_sock_t));
                ml_sock_t *helper = (ml_sock_t *)fp->helper;
                helper->path = apr_pstrdup(p, fp->proc);
            }
            break;
        case ML_PROC_MISSING:
        default: return;
    }
}

/* use copy of string to make name value pair */
static char *_ml_split_at(
        char at, 
        const char *fieldv, 
        char **name, 
        apr_pool_t *p
) {
    char *field = apr_pstrdup(p, fieldv);
    char *eq = strchr(field, at);
    *name = field;
    if (eq) {
        *eq = '\0';
        field = eq+1;
    } 
    return field;
}

/* sets the feature's name and field, handles names with = signs in them */
void _ml_set_feat_name(
        apr_pool_t *p, 
        ml_feature_t *feat, 
        const char *field
) {
    char *name;
    field = _ml_split_at('=', field, &name, p);
    feat->field = apr_pstrdup(p, field);
    if (name != field) {
        feat->name = apr_pstrdup(p, name);
    } else {
        /* auth's field "name" is a password */
        if (feat->fieldtype == ML_AUTH) {
            feat->name = apr_pstrdup(p, "auth");
        } else {
            feat->name = feat->field;
        }
    }
}

/* 
 * create field -> cleaner mapping 
 * automatically adds field to list of fields 
 * unless one already exists from the MLFeatures directive
 */
static int _ml_set_field(
        cmd_parms *cmd, 
        void *config, 
        const char *field, 
        const char *p, 
        const char *fieldtype, 
        const char *proctype
) {
    ml_directives_t *c = config;
    
    ml_fieldtype *ft = _ml_ft(fieldtype);
    if (ft == NULL) {
        _ml_log("bad fieldtype %s ", fieldtype);
        return 0;
    }

    ml_field_proc_t *fp; 
    ml_proctype *pt = _ml_pt(proctype);
    if (pt == NULL) {
        _ml_log("bad proctype %s ", proctype);
        return 0;
    }

    ml_feature_t *feat;
    if (c->save_features) {
        feat = (ml_feature_t *)apr_array_push(c->features);
    } else {
        feat = (ml_feature_t *)apr_pcalloc(cmd->pool, sizeof(ml_feature_t));
    }
    feat->fieldtype = *ft;
    _ml_set_feat_name(cmd->pool, feat, field);

    const char *fkey = _ml_fkey(cmd->pool, feat);
    fp = apr_hash_get(c->fieldprocs, fkey, ML_AHKS);
    if (fp == NULL) {
        fp = apr_pcalloc(cmd->pool, sizeof(ml_field_proc_t));
    }
    fp->fieldtype = *ft;
    fp->field = apr_pstrdup(cmd->pool, field); /* could use feat's ptr? */

    /* see if we've seen this cleanup proc before */
    ml_proc_t *proc;
    const char *pkey = _ml_pkey2(cmd->pool, *pt, p);
    proc = apr_hash_get(c->cleaners, pkey, ML_AHKS);
    if (proc == NULL) {
        proc = (ml_proc_t *)apr_pcalloc(cmd->pool, sizeof(ml_proc_t));
        proc->role = ML_CLEANER;
        proc->proctype = *pt;
        proc->proc = apr_pstrdup(cmd->pool, p);
        _ml_proc_set_helper(cmd->pool, proc);
        _ml_log("setting cleaners hash %s at %d", pkey, __LINE__);
        apr_hash_set(c->cleaners, pkey, ML_AHKS, proc);
    }
    /* fields can have more than one cleaner */
    if (fp->cleaners == NULL) {
        fp->cleaners = apr_array_make(cmd->pool, 1, sizeof(ml_proc_key_t));
        _ml_log("made new cleaners array");
    }
    ml_proc_key_t *cleaner = apr_array_push(fp->cleaners);
    cleaner->pkey = pkey;

    _ml_log("adding cleaner: %s", cleaner->pkey);
    _ml_log("saved feature %d:%s", feat->fieldtype, feat->field );

    apr_hash_set(c->fieldprocs, fkey, ML_AHKS, fp);
    return 1;
}

/* default cleaner for field data */
static const char *ml_set_def_field_proc(
        cmd_parms *cmd, 
        void *config, 
        const char *proctype, 
        const char *proc
) {
    ml_directives_t *c = config;
    ml_proctype *pt = _ml_pt(proctype);
    if (pt == NULL) {
        _ml_log("bad proctype %s for def_fp", proctype);
        return 0;
    }
    _ml_log("setting default proctype %s (%d)", proctype, *pt);
    c->def_fp = apr_pcalloc(cmd->pool, sizeof(ml_proc_t));
    c->def_fp->proctype = *pt;
    c->def_fp->role = ML_CLEANER;
    c->def_fp->proc = apr_pstrdup(cmd->pool, proc);
    _ml_log("set default proctype %s (%d) proc: %s (role %d)", 
            proctype, c->def_fp->proctype, c->def_fp->proc, c->def_fp->role);
    _ml_proc_set_helper(cmd->pool, c->def_fp);
    return NULL;
}

/* default cleaner for output data */
static const char *ml_set_out_proc(
        cmd_parms *cmd, 
        void *config, 
        const char *proctype, 
        const char *proc
) {
    ml_directives_t *c = config;
    ml_proctype *pt = _ml_pt(proctype);
    if (pt == NULL) {
        _ml_log("bad proctype %s for out_fp", proctype);
        return 0;
    }
    _ml_log("setting default proctype %s (%d)", proctype, *pt);
    c->out_fp = apr_pcalloc(cmd->pool, sizeof(ml_proc_t));
    c->out_fp->proctype = *pt;
    c->out_fp->role = ML_CLEANER;
    c->out_fp->proc = apr_pstrdup(cmd->pool, proc);
    _ml_log("set output string proctype %s (%d) proc: %s (role %d)", 
            proctype, c->out_fp->proctype, c->out_fp->proc, c->out_fp->role);
    _ml_proc_set_helper(cmd->pool, c->out_fp);
    return NULL;
}

/* 
 * add to field - cleaner map 
 * these modify fields in some way: e.g. remove newlines 
 * quoting might be another job is ML_OUT_RAW is the output format 
 */
static const char *ml_set_field_proc(
        cmd_parms *cmd, 
        void *config, 
        const char *args
) {
    const char *proctype = ap_getword_conf(cmd->pool, &args);
    const char *proc = ap_getword_conf(cmd->pool, &args);
    const char *fieldtype = ap_getword_conf(cmd->pool, &args);
    char *field;
    ml_directives_t *c = config;
    c->save_features = 1;
    _ml_log("proctype %s", proctype );
    _ml_log("proc %s", proc );
    _ml_log("fieldtype %s", fieldtype );
    _ml_log("full string %s", args );
    field = ap_getword_conf(cmd->pool, &args);
    while (field && (strlen(field) > 0)) {
        if (!_ml_set_field(cmd, config, field, proc, fieldtype, proctype)) {
            return NULL;
        }
        field = ap_getword_conf(cmd->pool, &args);
    }
    _ml_log_conf(cmd->pool, config,"....................................... ml_set_field_proc");
    return NULL;
}

/* 
 * add a process/script etc to do preprocessing of features - 
 * does not expect response
 */
static const char *_ml_set_proc(
        cmd_parms *cmd, 
        ml_directives_t *c, 
        const char *proctype, 
        const char *proc, ml_role role
) {
    ml_proctype *pt = _ml_pt(proctype);
    if (pt == NULL || *pt == ML_REGEX) {
        return "bad proc type in _ml_set_proc!";
    }
    ml_proc_t *p = NULL;
    switch (role) {
        case ML_PREPROCESSOR:
            p = (ml_proc_t *)apr_array_push(c->preprocessor);
            break;
        case ML_CLASSIFIER:
            p = (ml_proc_t *)apr_array_push(c->classifier);
            p->classresponse = c->classresponse;
            break;
        default: return 
                 "role should be ML_PREPROCESSOR "
                 "or ML_CLASSIFIER in _ml_set_proc!";
    }
    p->proctype = *pt;
    p->role = role;
    p->proc = apr_pstrdup(cmd->pool, proc);
    p->features = c->features;
    p->labels = c->labels;
    p->outformat = c->outformat;
    p->out_fp = c->out_fp;
    p->def_fp = c->def_fp;
    c->reset_features = TRUE;
    _ml_proc_set_helper(cmd->pool, p);
    _ml_log_conf(cmd->pool, c, ".......................................... ml_set_proc");
    return NULL;
}

/* to our list of preprocessors */
static const char *ml_set_preprocessor(
        cmd_parms *cmd, 
        void *config, 
        const char *proctype, 
        const char *proc
) {
    return _ml_set_proc(cmd, 
            (ml_directives_t *)config, proctype, proc, ML_PREPROCESSOR);
}

/* 
 * add a process/script etc to do classification of features  
 * response expected 
 */
static const char *ml_set_classifier(
        cmd_parms *cmd, 
        void *config, 
        const char *proctype, 
        const char *proc
) {
    return _ml_set_proc(cmd, 
            (ml_directives_t *)config, proctype, proc, ML_CLASSIFIER);
}

/* things to feed in to the preprocessor or classifier */
static const char *_ml_add_feat(
        apr_pool_t *p, 
        ml_directives_t *c,
        apr_array_header_t *list, 
        const char *fieldtype, 
        const char *field
) {
    ml_fieldtype *ft = _ml_ft(fieldtype);
    if (ft == NULL) {
        return "bad field type!";
    }
    ml_feature_t *feat = (ml_feature_t *)apr_array_push(list);
    feat->fieldtype = *ft;
    _ml_set_feat_name(p, feat, field);
    _ml_log_conf(p, c, "....................................... ml_add_feat");
    return NULL;
}

/* restart various lists
 * features may have been saved via MLFieldProc 
 * directives so need to replace those 
 * also may be the case that a processing directive
 * such as preprocess, classify or class response happened
 * befor this directive in which case these fields
 * are assumed to be for the next processor
 */
static void _ml_restart_lists(
        cmd_parms *cmd,
        ml_directives_t *c,
        unsigned int isfeat
) {
    unsigned int featreset = FALSE;
    if (isfeat && c->save_features) {
        c->features = apr_array_make(cmd->pool, 1, sizeof(ml_feature_t));
        featreset = TRUE;
        c->save_features = FALSE;
    }
    if (c->reset_features) {
        c->labels = apr_array_make(cmd->pool, 1, sizeof(ml_feature_t));
        if (!featreset)
            c->features = apr_array_make(cmd->pool, 1, sizeof(ml_feature_t));
        c->reset_features = FALSE;
    }
}

/* add to our list of features */
static const char *ml_set_features(
        cmd_parms *cmd, 
        void *config, 
        const char *fieldtype, 
        const char *field
) {
    ml_directives_t *c = (ml_directives_t *)config;
    _ml_restart_lists(cmd, c, TRUE);
    return _ml_add_feat(cmd->pool, c, c->features, fieldtype, field);
}

/* 
 * add to our list of labels 
 * apart from being sent to front of list labels not treated differently 
 */
static const char *ml_set_labels(
        cmd_parms *cmd, 
        void *config, 
        const char *fieldtype, 
        const char *field
) {
    ml_directives_t *c = (ml_directives_t *)config;
    _ml_restart_lists(cmd, c, FALSE);
    return _ml_add_feat(cmd->pool, c, c->labels, fieldtype, field);
}

/* 
 * what to do with the output from the classifier(s) 
 * since we are only going to get one class save 
 * lists of actions based on class
 */
static const char *ml_set_class_response(
        cmd_parms *cmd, 
        void *config, 
        const char *args
) {
    apr_pool_t *p = cmd->pool;

    ml_directives_t *c = (ml_directives_t *)config;

    if (c->reset_features) {
        c->classresponse = apr_hash_make(cmd->pool);
        /* can't change reset_features here because of MLFeatures/Labels */
    }

    char *class = ap_getword_conf(p, &args);
    if (class == NULL) {
        return "error: missing class to match!";
    }
    char *crt = ap_getword_conf(p, &args);
    if (crt == NULL) {
        return apr_psprintf(p, 
                "error: missing class response type for class %s!", class);
    }
    ml_classresponse *crtype = _ml_cr(crt);
    if (crtype == NULL) {
        return apr_psprintf(p, 
                "error: don't know class response type for %s for class %s!", 
                crt, class);
    }
    /* ap_getword_conf will remove quotes etc from arguments */
    const char *actword = ap_getword_conf(p, &args);
    char *action = NULL;
    if (actword) action = apr_pstrdup(p, actword);
    while (actword && strlen(actword) > 0) {
        actword = ap_getword_conf(p, &args);
        if (actword) action = apr_pstrcat(p, action, " ", actword, NULL);
    }
    if (action == NULL || strlen(action) == 0) 
        return "missing action in ml_set_class_response!";

    /* for each class we are watching have a list of responses */
    /* these will be applied in order if the class is seen */
    apr_array_header_t *crlist = 
        (apr_array_header_t *)
            apr_hash_get(c->classresponse, class, ML_AHKS);

    if (crlist == NULL) {
        crlist = apr_array_make(p, 1, sizeof(ml_cr_t));
        apr_hash_set(c->classresponse, class, ML_AHKS, crlist);
    }

    ml_cr_t *cr = (ml_cr_t *)apr_array_push(crlist);
    cr->crtype = *crtype;
    cr->class = apr_pstrdup(p, class);

    switch (cr->crtype) {
        case ML_CLASS_IP:
        case ML_CLASS_SOCK:
        case ML_CLASS_PROC:
            {
                /* the arguments help us decide what to send */
                cr->args = _ml_split_at(' ', action, &cr->action, p);
                cr->proc = (ml_proc_t *)apr_pcalloc(p, sizeof(ml_proc_t));
                cr->proc->role = ML_CLASSRESPONSE;
                /* proctype and crtype can both be used for the proctype */
                cr->proc->proctype = cr->crtype; 
                cr->proc->proc = apr_pstrdup(p, cr->action);
                _ml_proc_set_helper(p, cr->proc); 
                cr->proc->outformat = c->outformat;
                cr->proc->features = c->features;
                cr->proc->labels = c->labels;
                c->reset_features = TRUE;
            }
            break;
        default:
            {
                cr->action = apr_pstrdup(p, action);
                cr->args = NULL;
                cr->proc = NULL;
            }
    }

    _ml_log_conf(cmd->pool, c, ".......................... ml_set_class_response");
    return NULL;
}

/*
 ==============================================================================
 The directive structure for our name tag:
 ==============================================================================
 */
static const command_rec ml_directives[] =
{
    AP_INIT_TAKE1("MLEnabled", ml_set_enabled, NULL, OR_ALL,
					"Enable or disable mod_ml"),
    AP_INIT_TAKE1("MLOutFormat", ml_set_outformat, NULL, OR_ALL,
					"How to format feature string: "
                    "csv, json, jsonarray, jsonfields, raw, quoted"),
    AP_INIT_TAKE2("MLDefFieldProc", ml_set_def_field_proc, NULL, OR_ALL,
					"Default processor for cleaning field data. "
                    "regexes must use m// with () captures or s/// to work. "
                    "Arguments: {sock|ip|proc|regex} {string}"),
    AP_INIT_RAW_ARGS("MLFieldProc", ml_set_field_proc, NULL, OR_ALL,
					"Field/processor map. "
                    "Arguments: {regex|proc|ip|sock} {processor} "
                    "{cgi|uri|header|env|cookie} {field1 field2 ...}"),
    AP_INIT_TAKE2("MLPreProcess", ml_set_preprocessor, NULL, OR_ALL,
					"Process that creates model or does basic data intake. "
                    "Arguments: {sock|ip|proc} {string}"),
    AP_INIT_TAKE2("MLClassifier", ml_set_classifier, NULL, OR_ALL,
					"Process that makes decisions based on field input. "
                    "Arguments: {sock|ip|proc} {string}"),
    AP_INIT_ITERATE2("MLFeatures", ml_set_features, NULL, OR_ALL,
					"Field names of features in order. "
                    "Arguments: "
                    "{cgi|uri|header|env|cookie|time|literal|request|auth} "
                    "{field1, field2 ...}"),
    AP_INIT_ITERATE2("MLLabels", ml_set_labels, NULL, OR_ALL,
					"Label name(s) in order (used for supervised learning). "
                    "Arguments: "
                    "{cgi|uri|header|env|cookie|time|literal|request|auth} "
                    "{field1, field2 ...}"),
    AP_INIT_RAW_ARGS("MLClassResponse", ml_set_class_response, NULL, OR_ALL,
                    "What to do based on classification (applied in order). "
                    "Arguments: "
                    "{class} {cgi|uri|header|cookie|env|http|ip|proc|sock} "
                    "{action}"),
    AP_INIT_TAKE2("MLOutProc", ml_set_out_proc, NULL, OR_ALL,
					"Processor for cleaning a full feature string. "
                    "regexes must use m// with () captures or s/// to work. "
                    "Arguments: {sock|ip|proc|regex} {string}"),
    
    { NULL }
};

/* build a blank directory conf - also build maps to read directives */
static void* ml_create_dir_conf(apr_pool_t *pool, char *x)
{
    ml_directives_t *ml = apr_pcalloc(pool, sizeof(ml_directives_t));
    ml->enabled = ML_DISABLED;
    ml->dir = apr_pstrdup(pool, x);
    ml->s = NULL;
    ml->r = NULL;
    ml->is_handler = FALSE;
    ml->save_features = FALSE;
    ml->reset_features = FALSE;
    ml->fieldprocs = apr_hash_make(pool);
    ml->def_fp = NULL;
    ml->out_fp = NULL;
    ml->cleaners = apr_hash_make(pool);
    ml->features = apr_array_make(pool, 1, sizeof(ml_feature_t));
    ml->outformat = ML_OUT_NONE;
    ml->cgi = NULL; /* only create this if needed */
    ml->labels = apr_array_make(pool, 1, sizeof(ml_feature_t)); 
    ml->preprocessor = apr_array_make(pool, 1, sizeof(ml_proc_t)); 
    ml->classifier = apr_array_make(pool, 1, sizeof(ml_proc_t)); 
    ml->classresponse = apr_hash_make(pool);

    /* many fields require us to figure out what a processor is */
    /* using an array so we can save a pointer to the actual value easily */
    if (ml_proctype_map == NULL) {
        ml_proctype_map = apr_hash_make(pool);
        apr_hash_set(ml_proctype_map, "ip", ML_AHKS, ml_pt+ML_IP);
        apr_hash_set(ml_proctype_map, "proc", ML_AHKS, ml_pt+ML_PROC);
        apr_hash_set(ml_proctype_map, "script", ML_AHKS, ml_pt+ML_PROC);
        apr_hash_set(ml_proctype_map, "sock", ML_AHKS, ml_pt+ML_SOCK);
        apr_hash_set(ml_proctype_map, "socket", ML_AHKS, ml_pt+ML_SOCK);
        apr_hash_set(ml_proctype_map, "regex", ML_AHKS, ml_pt+ML_REGEX);
        apr_hash_set(ml_proctype_map, "pat", ML_AHKS, ml_pt+ML_REGEX);
    }

    /* we also have to figure out where to find a given field */
    if (ml_fieldtype_map == NULL) {
        ml_fieldtype_map = apr_hash_make(pool);
        apr_hash_set(ml_fieldtype_map, "cgi", ML_AHKS, ml_ft+ML_CGI);
        apr_hash_set(ml_fieldtype_map, "header", ML_AHKS, ml_ft+ML_HEADER);
        apr_hash_set(ml_fieldtype_map, "env", ML_AHKS, ml_ft+ML_ENV);
        apr_hash_set(ml_fieldtype_map, "cookie", ML_AHKS, ml_ft+ML_COOKIE);
        apr_hash_set(ml_fieldtype_map, "uri", ML_AHKS, ml_ft+ML_URI);
        apr_hash_set(ml_fieldtype_map, "url", ML_AHKS, ml_ft+ML_URI);
        apr_hash_set(ml_fieldtype_map, "time", ML_AHKS, ml_ft+ML_TIME);
        apr_hash_set(ml_fieldtype_map, "literal", ML_AHKS, ml_ft+ML_LITERAL);
        apr_hash_set(ml_fieldtype_map, "request", ML_AHKS, ml_ft+ML_REQ);
        apr_hash_set(ml_fieldtype_map, "auth", ML_AHKS, ml_ft+ML_AUTH);
    }

    /* class response types */
    if (ml_cr_map == NULL) {
        ml_cr_map = apr_hash_make(pool);
        apr_hash_set(ml_cr_map, "ip", ML_AHKS, ml_cr+ML_CLASS_IP);
        apr_hash_set(ml_cr_map, "proc", ML_AHKS, ml_cr+ML_CLASS_PROC);
        apr_hash_set(ml_cr_map, "sock", ML_AHKS, ml_cr+ML_CLASS_SOCK);
        apr_hash_set(ml_cr_map, "cgi", ML_AHKS, ml_cr+ML_CLASS_CGI);
        apr_hash_set(ml_cr_map, "header", ML_AHKS, ml_cr+ML_CLASS_HEADER);
        apr_hash_set(ml_cr_map, "env", ML_AHKS, ml_cr+ML_CLASS_ENV);
        apr_hash_set(ml_cr_map, "cookie", ML_AHKS, ml_cr+ML_CLASS_COOKIE);
        apr_hash_set(ml_cr_map, "http", ML_AHKS, ml_cr+ML_CLASS_HTTP);
        apr_hash_set(ml_cr_map, "redirect", ML_AHKS, ml_cr+ML_CLASS_REDIRECT);
    }
    return ml;
}

/* create per-server config structures */
static void* ml_create_svr_conf(apr_pool_t *pool, server_rec *s)
{
    ml_directives_t *ml = ml_create_dir_conf(pool,NULL);
    ml->s = s;
    return ml;
}

/* 
 * "merge"  per-dir config structures 
 * just treating all configuration as independent right now 
 * each instance must completely define its behaviour 
 * similar to the default behaviour of mod_rewrite which is my model 
 */
static void* ml_merge_dir_conf(apr_pool_t *pool, void *BASE, void *ADD)
{
    _ml_log("merging dir");
    return ADD;
}

/* merge  per-server config structures */
static void* ml_merge_svr_conf(apr_pool_t *pool, void *BASE, void *ADD)
{
    _ml_log("merging server using dir function");
    return ml_merge_dir_conf(pool,BASE,ADD);
}

/*
 * directive handler function prototypes
 */
/* mod_ml handlers */
static int ml_classifier_hook(request_rec *r);
static int ml_handler(request_rec *r); 
static int ml_log_after(request_rec *r);

/* map handler to server event ... */
static void ml_register_hooks(apr_pool_t *p)
{
    /* defined in mod_rewrite_funcs.c */
    ap_hook_pre_config(pre_config, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_post_config(post_config, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_child_init(init_child, NULL, NULL, APR_HOOK_MIDDLE);

    /* in reality header_parser and fixups 
     * don't have access to enough info to work
     * well with external processes but 
     * fixups and header_parser appear to be
     * the only places where setting an
     * environment variable will be detected by mod_rewrite (?)
     * 2015-08-26: turns out the order of the LoadModule directives 
     *             affects when hooks get run - putting mod_ml before 
     *             mod_rewrite allows mod_rewrite to see mod_ml's changes
     */
    ap_hook_fixups(ml_classifier_hook, NULL, NULL, APR_HOOK_REALLY_FIRST);

    /* display configuration using SetHandler ml */
    ap_hook_handler(ml_handler, NULL, NULL, APR_HOOK_REALLY_FIRST);

    /* this appears to be the best place to do the preprocessing step */
    ap_hook_log_transaction(ml_log_after, NULL, NULL, APR_HOOK_REALLY_LAST);
}

/* Dispatch list for API hooks */
module AP_MODULE_DECLARE_DATA ml_module = {
    STANDARD20_MODULE_STUFF, 
    ml_create_dir_conf,             /* create per-dir    config structures */
    ml_merge_dir_conf,              /* merge  per-dir    config structures */
    ml_create_svr_conf,             /* create per-server config structures */
    ml_merge_svr_conf,              /* merge  per-server config structures */
    ml_directives,                  /* table of config file commands       */
    ml_register_hooks               /* register hooks                      */
};

/*
 ==============================================================================
 Directive handlers 
 ==============================================================================
 */

/* communications stuff */

static int logError(int status, const char *msg, request_rec *r) 
{
    ap_log_rerror(APLOG_MARK, APLOG_ERR, status, r, "mod_ml: %s", msg);
    return status;
}

/*
 * socket communication example adapted from client example in
 * http://beej.us/guide/bgipc/output/html/multipage/unixsock.html
 * conflict between the apache user and socket perms can be a problem
 */
static int _ml_send_to_sock(
        request_rec *r, 
        ml_proc_t *pr, 
        const char *msg, 
        char response[]
) {
#ifndef linux
    return APR_SUCCESS;
#else 
    if (pr->helper == NULL) return APR_SUCCESS;

    ml_sock_t *helper = (ml_sock_t *)pr->helper;

    if (helper->path == NULL || strlen(helper->path) == 0) return APR_SUCCESS;

    int s, t, len;
    struct sockaddr_un remote;

    if ((s = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        return logError(661, "failed to create unix socket", r);
    }

    remote.sun_family = AF_UNIX;
    strcpy(remote.sun_path, helper->path);
    len = strlen(remote.sun_path) + sizeof(remote.sun_family);

    if (connect(s, (struct sockaddr *)&remote, len) == -1) {
        close(s);
        return logError(errno, "failed to connect to unix socket", r);
    }

    if (send(s, msg, strlen(msg), 0) == -1) {
        close(s);
        return logError(errno, "error sending to unix socket", r);
    }
    if (send(s, "\n", strlen("\n"), 0) == -1) {
        close(s);
        return logError(errno, "error sending to unix socket", r);
    }

    if ((t=recv(s, response, HUGE_STRING_LEN, 0)) > 0) {
        response[t] = '\0';
    } else {
        if (t < 0) {
            close(s);
            return logError(errno, "error receiving from unix socket", r);
        } else {
            close(s);
            return logError(errno, "unix socket closed", r);
        }
    }
    close(s);
    return APR_SUCCESS;
#endif /* linux */
}

/* 
 * adapted from http://www.hoboes.com/Mimsy/hacks/calling-external-server/ 
 * only thing different is the ip/port coming from a helper 
 */
static int _ml_send_to_ip(
        request_rec *request, 
        ml_proc_t *pr, 
        const char *msg, 
        char response[],
        apr_size_t item_size
) {
    if (pr->helper == NULL) return APR_SUCCESS;

    apr_socket_t *sock;
    apr_sockaddr_t *sockaddr;
    apr_status_t status;
    ml_ip_t *helper = (ml_ip_t *)pr->helper;

    if (helper->host == NULL || strlen(helper->host) == 0) return APR_SUCCESS;
    if (helper->port <= 0) return APR_SUCCESS;

    //timeout in microseconds, so time out after 5 seconds
    apr_interval_time_t timeout = 5000000;

    if ((status = apr_sockaddr_info_get(
                    &sockaddr, 
                    helper->host, 
                    helper->family, 
                    helper->port, 
                    0, 
                    request->pool)) != APR_SUCCESS
       ) {
        return logError(status, "creating socket address", request);
    }
    if ((status = apr_socket_create(
                    &sock, 
                    sockaddr->family, 
                    SOCK_STREAM, 
                    APR_PROTO_TCP, 
                    request->pool)) != APR_SUCCESS
       ) {
        return logError(status, "creating socket", request);
    }
    if ((status = apr_socket_timeout_set(sock, timeout)) != APR_SUCCESS) {
        return logError(status, "setting socket timeout", request);
    }
    if ((status = apr_socket_connect(sock, sockaddr)) != APR_SUCCESS) {
        return logError(status, "connecting", request);
    }
    _ml_clog(request->server, "socket: sending %s", msg);
    msg = apr_pstrcat(request->pool, msg, "\n\n", NULL);
    item_size = strlen(msg);
    if ((status = apr_socket_send(sock, msg, &item_size)) != APR_SUCCESS) {
        return logError(status, "sending query", request);
    }
    status = apr_socket_recv(sock, response, &item_size);
    if (status != APR_SUCCESS) {
        return logError(
                status, 
                apr_psprintf(request->pool, "received response %s", response), 
                request);
    }
    _ml_clog(request->server, "socket: item_size %ld received %s", (long)item_size, response);
    apr_socket_close(sock);
    /* make sure it ends, and chop off carriage return */
    char *nl;
    response[item_size] = '\0';
    /* lop off newlines at end of string */
    if ((nl = strchr(response,'\n')) != NULL) {
        while (nl = strrchr(response, '\n')) {
            *nl = '\0';
        }
    }
    return APR_SUCCESS;
}

/*
 * start mod_ml related stuff 
 */

/* are we the handler for this request? */
static int _ml_handling(request_rec *r)
{
    if (strcmp(r->handler, "ml")) return 0;
    return 1;
}

/* data processing stuff */

/* turn a 20 element sha1 digest array to a 40 char hex string */
static char *_ml_hex(apr_pool_t *p, unsigned char d[])
{
    return apr_psprintf(p,
            "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x"
            "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
            d[0], d[1], d[2], d[3], d[4], d[5], d[6], 
            d[7], d[8], d[9], d[10], d[11], d[12], d[13],
            d[14], d[15], d[16], d[17], d[18], d[19]);
}

/* get data from various sources */
static char * _ml_get(
        request_rec *r, 
        ml_fieldtype ft, 
        const char *field, 
        ml_directives_t *c
) {
    apr_pool_t *p = r->pool;
    const char *cookiep = NULL;

    const char *s = NULL;

    switch (ft) {
        case ML_CGI: 
        /* see 
         * http://people.apache.org/~humbedooh/mods/examples/mod_example_2.c 
         * http://httpd.apache.org/docs/2.4/developer/modguide.html#snippets 
         */
            if (c->cgi == NULL) {
                if (!strcmp(r->method, "GET")) {
                    ap_args_to_table(r, &c->cgi);

                } else if (!strcmp(r->method, "POST")) {
                    char *val = NULL;
                    apr_array_header_t *POST = NULL;
                    ap_form_pair_t *pairs = NULL;
                    apr_off_t len;
                    apr_size_t size;
                    c->cgi = apr_table_make(r->pool, 1);

                    int rc = ap_parse_form_data(r, NULL, &POST, -1, 
                                                HUGE_STRING_LEN);
                    if (rc || !POST) return NULL;

                    pairs = (ap_form_pair_t *)POST->elts;
                    for (int i=0; i<POST->nelts; i++) {
                        apr_brigade_length(pairs[i].value, 1, &len);
                        size = (apr_size_t) len;
                        val = apr_pcalloc(p, size + 1);
                        apr_brigade_flatten(pairs[i].value, val, &size);
                        val[len] = 0;
                        apr_table_set(c->cgi, 
                                apr_pstrdup(r->pool, pairs[i].name), 
                                val);
                    }
                }
            }
            s = apr_table_get(c->cgi, field);
            if (s) {
                return apr_pstrdup(p, s);
            }
            return NULL;

        case ML_HEADER: 
            s = apr_table_get(r->headers_in, field);
            if (s) {
                return apr_pstrdup(p, s);
            }
            return NULL;

        case ML_TIME:
            {
                if (!strcmp(field,"millis")) {
                    return apr_ltoa(p, r->request_time);
                }
                if (!strcmp(field,"epoch")) {
                    return apr_ltoa(p, r->request_time/1000);
                }
                if (!strcmp(field,"ctime")) {
                    char *t = apr_pcalloc(p, ML_TIME_LEN);
                    apr_ctime(t, r->request_time);
                    return t;
                }
                if (!strcmp(field,"rfc822")) {
                    char *t = apr_pcalloc(p, ML_TIME_LEN);
                    apr_rfc822_date(t, r->request_time);
                    return t;
                }
                /* assume what we are being given is a time format */
                apr_time_exp_t tm;
                apr_time_exp_gmt(&tm, r->request_time);
                char t[ML_TIME_LEN] = "";
                apr_size_t len = ML_TIME_LEN;
                /* the ML_TIME_LEN is critical */
                char *name;
                char *value = _ml_split_at('=', field, &name, p);
                apr_strftime(t, &len, ML_TIME_LEN, value, &tm);
                return apr_pstrdup(p, t);
            } 
        case ML_LITERAL:
            return apr_psprintf(p, "%s", field);

        case ML_ENV: 
            s = apr_table_get(r->subprocess_env, field);
            if (s) {
                return apr_pstrdup(p, s);
            }
            s = getenv(field);
            if (s) {
                return apr_pstrdup(p, s);
            }
            return NULL;

        case ML_REQ:
            /* 
             * explanation of what some of these are here:
             * http://docstore.mik.ua/orelly/apache_mod/128.htm 
             * "writing apache modules with perl and c" 
             * by Lincoln Stein and Doug McEachern
             */
            switch (*field) {
                case 'a':
                    if (!strcasecmp(field,"args")) 
                        return apr_pstrdup(p, r->args);  
                    break;
                case 'c':
                    if (!strcasecmp(field,"content_type")) 
                        return apr_pstrdup(p, r->content_type);  
                    if (!strcasecmp(field,"content_encoding")) 
                        return apr_pstrdup(p, r->content_encoding);  
                    break;
                case 'f':
                    if (!strcasecmp(field,"filename")) 
                        return apr_pstrdup(p, r->filename);  
                    break;
                case 'h':
                    if (!strcasecmp(field,"handler")) 
                        return apr_pstrdup(p, r->handler);  
                    if (!strcasecmp(field,"hostname")) 
                        return apr_pstrdup(p, r->hostname);  
                    break;
                case 'm':
                    if (!strcasecmp(field,"method")) 
                        return apr_pstrdup(p, r->method);  
                    break;
                case 'p':
                    if (!strcasecmp(field,"protocol")) 
                        return apr_pstrdup(p, r->protocol);  
                    if (!strcasecmp(field,"path_info")) 
                        return apr_pstrdup(p, r->path_info);  
                    break;
                case 'r':
                    if (!strcasecmp(field,"range")) 
                        return apr_pstrdup(p, r->range);  
                    break;
                case 's':
                    // this holds the actual http response (e.g. 200 OK)
                    if (!strcasecmp(field,"status_line")) 
                        return apr_pstrdup(p, r->status_line);  
                    break;
                case 't':
                    if (!strcasecmp(field,"the_request")) 
                        return apr_pstrdup(p, r->the_request);  
                    break;
                case 'u':
                    if (!strcasecmp(field,"unparsed_uri")) 
                        return apr_pstrdup(p, r->unparsed_uri);  
                    if (!strcasecmp(field,"uri")) 
                        return apr_pstrdup(p, r->uri);  
                    break;
                case 'v':
                    if (!strcasecmp(field,"vlist_validator")) 
                        return apr_pstrdup(p, r->vlist_validator);  
                default: return NULL;
            }
            return NULL;
            
        case ML_COOKIE: 
            cookiep = apr_table_get(r->headers_in, "Cookie");
            if (cookiep != NULL) {
                /* max cookie length is 4096 */
                char cookies[5000] = "";
                strcpy(cookies, cookiep);
                if (strlen(cookiep) >= 5000) cookies[5000] = '\0';
                char *last;
                char *cookie = apr_strtok(cookies, ";", &last);
                while (cookie) {
                    if (strlen(cookie) > strlen(field)) {
                        if (!strncmp(cookie, field, strlen(field))) {
                            char *val = strchr(cookie,'=');
                            if (val) {
                                return apr_pstrdup(p, val+1);
                            }
                        }
                    }
                    cookie = apr_strtok(NULL, ";", &last);
                }
            }
            return NULL;

        case ML_URI: 
            switch (*field) {
                case 'f':
                    if (!strcmp(field,"fragment")) 
                        return apr_pstrdup(p, r->parsed_uri.fragment); 
                    break;
                case 'h':
                    if (!strcmp(field,"hostinfo")) 
                        return apr_pstrdup(p, r->parsed_uri.hostinfo); 
                    if (!strcmp(field,"hostname")) 
                        return apr_pstrdup(p, r->parsed_uri.hostname); 
                    break;
                case 'p':
                    if (!strcmp(field,"port_str")) 
                        return apr_pstrdup(p, r->parsed_uri.port_str); 
                    if (!strcmp(field,"port")) 
                        return apr_pstrdup(p, r->parsed_uri.port_str); 
                    if (!strcmp(field,"path")) 
                        return apr_pstrdup(p, r->parsed_uri.path); 
                    if (!strcmp(field,"password")) 
                        return apr_pstrdup(p, r->parsed_uri.password); 
                    break;
                case 'q':
                    if (!strcmp(field,"query")) 
                        return apr_pstrdup(p, r->parsed_uri.query); 
                    break;
                case 's':
                    if (!strcmp(field,"scheme")) 
                        return apr_pstrdup(p, r->parsed_uri.scheme); 
                    break;
                case 'u':
                    if (!strcmp(field,"user")) 
                        return apr_pstrdup(p, r->parsed_uri.user); 
                    break;
                default: 
                    return NULL;
            }
            break;
        case ML_AUTH:
            {
                /* make a nonce that works with the test programs
                 * the form is nonce={sha1 hex digest}:{salt}
                 * this is added to the feature string as a feature
                 */
                unsigned char d[APR_SHA1_DIGESTSIZE];
                apr_sha1_ctx_t ctx;
                apr_sha1_init(&ctx);
                apr_sha1_update(&ctx, field, strlen(field));
                apr_sha1_final(d, &ctx);
                char * hex = _ml_hex(p, d);
#if APR_HAS_RANDOM
                unsigned char salt[APR_SHA1_DIGESTSIZE];
                apr_generate_random_bytes(salt, APR_SHA1_DIGESTSIZE);
                char *hexsalt = _ml_hex(p, salt);
#else
                /* see https://en.wikipedia.org/wiki/Lehmer_random_number_generator */
                char *hexsalt = apr_ltoa(p, 
                        ((r->request_time * 48271) % 2147483647));
#endif
                apr_sha1_init(&ctx);
                char * both = apr_pstrcat(p, hex, hexsalt, NULL);
                apr_sha1_update(&ctx, both, strlen(both));
                apr_sha1_final(d, &ctx);
                char *hexboth = _ml_hex(p, d);
                return apr_pstrcat(p, "nonce=",hexboth,":",hexsalt,NULL);
            }
            break;
        default: 
            return NULL;
    }
}

/* apply a regex to change some raw data substitute and match both work*/
static char *_ml_apply_regex(request_rec *r, char *raw, ml_regex_t *helper) 
{
    /* for this to work the pattern MUST have form of m/(.*)/ or s/(.*)/$1/ */
    if (raw && helper && helper->rx) {
        char *newpattern;
        int matched = ap_rxplus_exec(r->pool, helper->rx, raw, &newpattern);
        if (matched) {
            if (helper->rx->subs) {
                return newpattern;
            }
            newpattern = ap_rxplus_pmatch(r->pool, helper->rx, matched);
            return newpattern;
        }
    }
    return raw;
}

/* use a helper to transform some raw data */
static char *_ml_apply_helper(request_rec *r, ml_proc_t *fp, char *raw)
{
    if (fp == NULL) return raw;
    if (raw == NULL) return raw;
    if (fp->helper == NULL) return raw;

    switch (fp->proctype) {
        case ML_REGEX: 
            { 
                return _ml_apply_regex(r, raw, fp->helper); 
            }
        case ML_PROC: 
            { 
                ml_exec_t *help = (ml_exec_t *)fp->helper;
                return ml_mr_send_to_program(r, help->fpin, help->fpout, raw);
            }
        case ML_IP:
            {
                char response[HUGE_STRING_LEN] = "";
                int rc = _ml_send_to_ip(r, fp, raw, response, HUGE_STRING_LEN);
                return apr_pstrdup(r->pool, response);
            }
        case ML_SOCK:
            {
                char response[HUGE_STRING_LEN] = "";
                int rc = _ml_send_to_sock(r, fp, raw, response);
                return apr_pstrdup(r->pool, response);
            }
        default:
            return raw;
    }
}

/* given a field proc get the field data and clean it */
static void _ml_clean(
        request_rec *r, 
        ml_directives_t *c,
        ml_field_proc_t *fp, 
        ml_feature_t *feature
) {
    feature->raw = _ml_get(r, fp->fieldtype, fp->field, c);
    if (feature->raw == NULL) {
        feature->clean = NULL;
        return;
    }
    if (fp->cleaners == NULL || fp->cleaners->nelts == 0) {
        feature->clean = apr_psprintf(r->pool, "%s", feature->raw);
        return;
    }

    char *clean = feature->raw;
    ml_proc_t *helper = NULL;

    ml_proc_key_t *clnr = (ml_proc_key_t *)fp->cleaners->elts;

    for (int i=0; i<fp->cleaners->nelts; i++) {

        _ml_clog(r->server,"at %d pkey is %s", __LINE__, clnr[i].pkey);
        helper = apr_hash_get(c->cleaners, clnr[i].pkey, ML_AHKS);

        if (helper) {
            _ml_clog(r->server,"got helper at %d raw %s", __LINE__,clean);
            clean = _ml_apply_helper(r, helper, clean);
            _ml_clog(r->server,"at %d clean %s", __LINE__,clean);
        }
    }

    if (helper == NULL) {
        _ml_clog(r->server,"not using clean at %d clean %s", __LINE__,clean);
        feature->clean = apr_psprintf(r->pool, "%s", feature->raw);
    } else {
        _ml_clog(r->server,"using clean at %d clean %s", __LINE__,clean);
        feature->clean = clean;
    }
}

/* grab and clean field data on a best efforts basis */
static void _ml_process_fields(
        request_rec *r, 
        ml_directives_t *c, 
        ml_proc_t *p,
        apr_array_header_t *featurelist
) {
    ml_feature_t *feats = (ml_feature_t *)featurelist->elts;
    for (int i=0; i<featurelist->nelts; i++) {

        /* see if we've seen this field before */
        /// if (feats[i].clean) continue;

        /* see if we have a cleaner for this field */
        ml_field_proc_t *fp = 
            apr_hash_get(c->fieldprocs, 
                    _ml_fkey(r->pool, &feats[i]), ML_AHKS);

        if (fp) {
            _ml_clog(r->server, "starting to clean %s", feats[i].field);
            _ml_clean(r, c, fp, &feats[i]);
        } else {
            feats[i].raw = _ml_get(r, feats[i].fieldtype, feats[i].field, c);
            _ml_clog(r->server, "not cleaning %s %s", feats[i].field, feats[i].raw);
            feats[i].clean = _ml_apply_helper(r, p->def_fp, feats[i].raw);
        }
    }
}

/* dump our internal configuration data */
static void _ml_print_conf(request_rec *r, ml_directives_t *sc, char *msg) {
    if (!_ml_handling(r)) return;
    ap_rprintf(r, "\n%s\n", msg);
    ap_rprintf(r, "%s", _ml_conf_str(sc, r->pool));
}

/* add \s to " characters in a string */
static char *_ml_esc_quotes(apr_pool_t *p, char *str)
{
    if (strchr(str, '"') != NULL) {
        char *clean = apr_pstrdup(p, str);
        char *last;
        char *quot = apr_strtok(clean, "\"", &last);
        str = apr_pstrdup(p, "");
        clean = quot;
        while (quot) {
            quot = apr_strtok(NULL, "\"", &last);
            str = apr_pstrcat(p, str, clean, NULL);
            if (quot) str = apr_pstrcat(p, str, "\\\"", NULL);
            clean = quot;
        }
        str = apr_pstrcat(p, str, clean, NULL);
    }
    return str;
}

/* concatenate features into a string - uses MLOutFormat */
static char *_ml_feature_cat(
        request_rec *r, 
        int *fcount, 
        char *fstr, 
        apr_array_header_t *features, 
        ml_outformat outformat
) {
    int i;
    apr_pool_t *p = r->pool;
    if (outformat == ML_OUT_NONE) outformat = ML_OUT_RAW;

    ml_feature_t *fs = (ml_feature_t *)features->elts;
    for (i=0; i<features->nelts; i++) {
        if (fs[i].clean == NULL) fs[i].clean = apr_pstrdup(p, "");
        /* escape quotes */
        if (outformat != ML_OUT_RAW) {
            fs[i].clean = _ml_esc_quotes(p, fs[i].clean);
        }
        switch (outformat) {
            case ML_OUT_CSV:
                {
                    fstr = apr_pstrcat(p, fstr, "\"", fs[i].clean,"\",", NULL);
                }
                break;
            case ML_OUT_JSONARY:
                {
                    fstr = apr_pstrcat(p, fstr, 
                            apr_psprintf(p, "\"%s\",", fs[i].clean), NULL);
                }
                break;
            case ML_OUT_JSON:
                {
                    fstr = apr_pstrcat(p, fstr, 
                            apr_psprintf(p, "\"%d\":\"%s\",", 
                                *fcount, fs[i].clean), NULL);
                }
                break;
            case ML_OUT_JSONFLDS:
                {
                    char *name = _ml_esc_quotes(p, fs[i].name);
                    fstr = apr_pstrcat(p, fstr, apr_psprintf(p, 
                                "\"%s\":\"%s\",", name, fs[i].clean), NULL);
                }
                break;
            case ML_OUT_QUOTED:
                {
                    fstr = apr_pstrcat(p, fstr, 
                            apr_psprintf(p, "\"%s\" ", fs[i].clean),NULL);
                }
                break;
            default:
                {
                    fstr = apr_pstrcat(p, fstr, " ", fs[i].clean,NULL);
                }
        }
        ++(*fcount);
    }
    return fstr;
}

/* build a string of features to send to an external process */
static char *_ml_feature_string(
        request_rec *r, 
        ml_directives_t *c,
        ml_proc_t *p
) {
    apr_array_header_t *features = p->features;
    if (features == NULL) features = c->features;

    apr_array_header_t *labels = p->labels;
    if (labels == NULL) labels = c->labels;

    ml_outformat outformat = p->outformat;
    if (outformat == ML_OUT_NONE) outformat = c->outformat;

    _ml_process_fields(r, c, p, features);
    _ml_process_fields(r, c, p, labels);

    char *featstr = NULL;

    switch (outformat) {
        case ML_OUT_JSON: 
        case ML_OUT_JSONFLDS: 
            featstr = apr_pstrdup(r->pool, "{");
            break;
        case ML_OUT_JSONARY: 
            featstr = apr_pstrdup(r->pool, "[");
            break;
        default:
            featstr = apr_pstrdup(r->pool, "");
    }

    int fcount = 1;
    featstr = _ml_feature_cat(r, &fcount, featstr, labels, outformat);
    featstr = _ml_feature_cat(r, &fcount, featstr, features, outformat);

    if (outformat != ML_OUT_RAW && featstr[strlen(featstr)-1] == ',') 
        featstr[strlen(featstr)-1] = '\0';

    switch (outformat) {
        case ML_OUT_JSON: 
        case ML_OUT_JSONFLDS: 
            featstr = apr_pstrcat(r->pool, featstr, "}",NULL); 
            break;
        case ML_OUT_JSONARY: 
            featstr = apr_pstrcat(r->pool, featstr, "]",NULL);
            break;
        default: break;
    }
    _ml_clog(r->server,"at %d feature string for %s %s: %s", 
            __LINE__, ml_role_names[p->role], p->proc, featstr);
    featstr = _ml_apply_helper(r, p->out_fp, featstr);
    if (c->is_handler) 
        ap_rprintf(r, "feature string for %s %s:\n%s\n", 
            ml_role_names[p->role], p->proc, featstr);
    return featstr;
}

/* make copy of cookie string with one cookie removed */
static char *_ml_rm_cookie(request_rec *r,
        char *cookiep, const char *name)
{
    apr_pool_t *p = r->pool;
    if (cookiep == NULL) return apr_pstrdup(p, "");

    char *editcookies = apr_pstrdup(p, cookiep);
    char *found = strcasestr(editcookies, name);
    if (found == NULL) return editcookies;

    if (*(found+strlen(name)) == '=' && 
            (found == editcookies || *(found-1) == ' ' || *(found-1) == ';') 
    ) {
        char *end = strchr(found, ';');
        *found = '\0';
        if (end) {
            end++;
            editcookies = apr_pstrcat(p, editcookies, end, NULL);
        }
    } 
    return editcookies;
}

/* 
 * insert arguments based on a format string 
 */
static char *_ml_ins_args(
        apr_pool_t *p, 
        const char *argsv,
        const char *features,
        const char *class
) {
    char empty[2] = "";
    char *last;
    const char *ins;

    if (argsv == NULL) return apr_pstrdup(p, "");

    char *argsin = apr_pstrdup(p, argsv);
    char *argprev = NULL;
    char *arg = apr_strtok(argsin, "%", &last);

    /* initialize to first bit of argsin before % (if % found) */
    char *argsout = apr_pstrdup(p, "");

    while (arg) {
        /* add any nonformat piece of the string after insert 
         * don't want to do this until the string is properly 
         * terminated - ie after apr_strtok happens
         */
        if (argprev) 
            argsout = apr_pstrcat(p, argsout, argprev, NULL);

        switch (*arg) {
            case 'F':
            case 'f':
                arg++;
                ins = features;
                break;
            case 'C':
            case 'c':
                arg++;
                ins = class;
                break;
            default: 
                ins = empty;
        }
        /* this replaces the format char with something meaningful */
        argsout = apr_pstrcat(p, argsout, ins, NULL);

        /* remember where we were */
        argprev = arg;

        /* see if there is anything else to do */
        arg = apr_strtok(NULL, "%", &last);
    }

    /* don't forget the last bit of the string */
    if (argprev) 
        argsout = apr_pstrcat(p, argsout, argprev, NULL);

    return argsout;
}

/* apply one class response for a specific class */
static int _ml_apply_one_cr(request_rec *r, ml_directives_t *c, ml_cr_t *cr) 
{
    if (cr == NULL || cr->action == NULL) return OK;

    const char *class = cr->class;
    apr_pool_t *p = r->pool;
    _ml_clog(r->server, "--------------------------------------- start CR %s %s", cr->class, cr->action);

    switch (cr->crtype) {
        case ML_CLASS_CGI: 
            {
                /* 
                 * tack them on to the end of r->args and 
                 * r->subprocess_env->query_string 
                 * only works with GET variables
                 */
                char conj[2] = "&";
                if (r->args && strlen(r->args)) {
                    if (!strchr(r->args, '&') && 
                            !strchr(r->args, '?')) strcpy(conj,"?");
                    if (!strstr(r->args, cr->action)) {
                        r->args = apr_pstrcat(p, r->args, conj, cr->action, NULL);
                    }
                } else {
                    r->args = apr_psprintf(p, "%s", cr->action);
                }
                if (r->parsed_uri.query && strlen(r->parsed_uri.query)) { 
                    if (!strchr(r->parsed_uri.query, '&') && 
                            !strchr(r->parsed_uri.query, '?')) strcpy(conj,"?");
                    else strcpy(conj,"&");
                    if (!strstr(r->parsed_uri.query, cr->action)) {
                        r->parsed_uri.query = 
                            apr_pstrcat(p, 
                                r->parsed_uri.query, conj, cr->action, NULL);
                    }
                } else {
                    r->parsed_uri.query = apr_psprintf(p, "%s", cr->action);
                }
                if (r->subprocess_env) {
                    /* other vars to change ? */
                    char *query_string;
                    const char *qs = 
                        apr_table_get(r->subprocess_env, "QUERY_STRING");
                    if (qs && strlen(qs)) {
                        if (!strchr(qs, '&') && !strchr(qs, '?')) strcpy(conj,"?");
                        else strcpy(conj,"&");
                        if (!strstr(qs, cr->action)) {
                            query_string = 
                                apr_pstrcat(p, qs, conj, cr->action, NULL);
                        }
                    } else {
                        query_string = apr_psprintf(p, "%s", cr->action);
                    }
                    apr_table_set(r->subprocess_env, 
                            "QUERY_STRING", query_string);

                    qs = apr_table_get(r->subprocess_env, "REQUEST_URI");
                    if (qs && strlen(qs)) {
                        if (!strchr(qs, '&') && !strchr(qs, '?')) strcpy(conj, "?");
                        else strcpy(conj,"&");
                        if (!strstr(qs, cr->action)) {
                            query_string = 
                                apr_pstrcat(p, qs, conj, cr->action, NULL);
                        }
                    } else {
                        query_string = apr_psprintf(p, "%s", cr->action);
                    }
                    apr_table_set(r->subprocess_env, 
                            "REQUEST_URI", query_string);
                }
            }
            break;

        case ML_CLASS_HEADER:
            {
                /* would it make sense to be changing only the 
                 * incoming/outgoing headers under some circumstances? 
                 */
                char *name = NULL;
                char *value = _ml_split_at('=', cr->action, &name, p);
                if (name != value) {
                    apr_table_set(r->headers_in, 
                            apr_pstrdup(p, name), apr_pstrdup(p, value));
                    apr_table_set(r->headers_out, 
                            apr_pstrdup(p, name), apr_pstrdup(p, value));
                } else {
                    apr_table_set(r->headers_in, 
                            apr_pstrdup(p, name), apr_pstrdup(p, ""));
                    apr_table_set(r->headers_out, 
                            apr_pstrdup(p, name), apr_pstrdup(p, ""));
                }
            }
            break;

        case ML_CLASS_ENV:
            {
                if (r->subprocess_env == NULL) return OK;
                char *name = NULL;
                char *value = _ml_split_at('=', cr->action, &name, p);
                if (name != value) {
                    apr_table_set(r->subprocess_env, 
                            apr_pstrdup(p, name), apr_pstrdup(p, value));
                } else {
                    apr_table_set(r->subprocess_env, 
                            apr_pstrdup(p, name), apr_pstrdup(p, ""));
                }
            }
            break;

        case ML_CLASS_COOKIE: 
            {
                char *action = apr_pstrdup(p, cr->action);
                /*
                char *sp = strrchr(action, ' ');
                while (strlen(action) && sp) {
                    *sp = '\0';
                    if (strlen(action)) sp = strrchr(action, ' ');
                    else sp = NULL;
                }
                */

                /* basic sanity checks */
                char *eq = strchr(action, '=');
                char *semi = strchr(action, ';');
                if (eq == NULL || semi == NULL || eq > semi) break;

                char *name = NULL;
                char *value = _ml_split_at('=', action, &name, p);
                char *outcookiep = 
                    (char *)apr_table_get(r->headers_out, "Set-Cookie");
                _ml_clog(r->server, "outcookie before: %s", outcookiep);
                outcookiep = _ml_rm_cookie(r, outcookiep, name);
                _ml_clog(r->server, "outcookie after: %s", outcookiep);
                char *outcookies =
                    apr_pstrcat(p, outcookiep, " ", action, NULL);
                apr_table_set(r->headers_out, "Set-Cookie", outcookies);

                char *incookiep = 
                    (char *)apr_table_get(r->headers_in, "Cookie");
                _ml_clog(r->server, "incookie before: %s", incookiep);
                incookiep = _ml_rm_cookie(r, incookiep, name);
                _ml_clog(r->server, "incookie after: %s", incookiep);
                char *incookies = 
                    apr_pstrcat(p, incookiep, " ", action, NULL);
                apr_table_set(r->headers_in, "Cookie", incookies);
            }
            break;
        case ML_CLASS_HTTP:
            {
                return apr_atoi64(cr->action);
            }
            break;
        case ML_CLASS_IP:
        case ML_CLASS_PROC:
        case ML_CLASS_SOCK:
            {
                char *featurestr = _ml_feature_string(r, c, cr->proc);
                char *args = _ml_ins_args(r->pool, cr->args, featurestr, cr->class);

                _ml_clog(r->server, "interpolated args:\n%s\n", args);
                if (c->is_handler) ap_rprintf(r, "interpolated args:\n%s\n", args);

                char *response = _ml_apply_helper(r, cr->proc, args);
            }
            break;
        case ML_CLASS_REDIRECT:
            /* test mod_rewrite for this with an environment variable */
        default: return OK;
    }
    _ml_clog(r->server, "--------------------------- finish CR %s %s", cr->class, cr->action);
    return OK;
}

/* apply class responses to a class */
static int _ml_apply_crs(
        request_rec *r, 
        ml_directives_t *c, 
        ml_proc_t *p,
        const char *class
) {
    if (class == NULL) return OK;
    if (c == NULL) return OK;
    if (p == NULL) return OK;

    apr_hash_t *cr = p->classresponse;
    if (cr == NULL) cr = c->classresponse;
    if (cr == NULL) return OK;

    apr_array_header_t *crarray = apr_hash_get(cr, class, ML_AHKS);
    if (crarray == NULL) return OK;

    ml_cr_t *crs = (ml_cr_t *)crarray->elts;
    int rc = OK;
    for (int i; i<crarray->nelts; i++) {
        rc = _ml_apply_one_cr(r, c, &crs[i]);
    }
    return rc;
}


/* preprocessing - where we don't use the response */
static int _ml_preprocess(request_rec *r, ml_directives_t *c)
{
    int i;
    char *class;
    if (c->enabled != ML_ENABLED) return OK;

    ml_proc_t *procs = (ml_proc_t *)c->preprocessor->elts;
    for (i = 0; i<c->preprocessor->nelts; i++) {
        _ml_clog(r->server, "0000000000000000000000000000000000000000000000000000 start preprocessor %d: %s\n", i, class);
        char *features = _ml_feature_string(r, c, &procs[i]);
        class = _ml_apply_helper(r, &procs[i], features);
        _ml_clog(r->server, "0000000000000000000000000000000000000000000000000000 finish preprocessor %d: %s\n", i, class);
    }
    return OK;
}

/* classification - where we keep the response */
static int _ml_classify(request_rec *r, ml_directives_t *c)
{
    int i, rc;
    char *class;
    if (c->enabled != ML_ENABLED) return OK;

    /* if there are conflicting responses this keeps the last one */
    ml_proc_t *procs = (ml_proc_t *)c->classifier->elts;
    for (i = 0; i<c->classifier->nelts; i++) {
        _ml_clog(r->server, "================================================ start classifier %d: %s", i, procs[i].proc);
        char *features = _ml_feature_string(r, c, &procs[i]);
        class = _ml_apply_helper(r, &procs[i], features);
        /* then finally apply the class responses to the classifier output */
        rc = _ml_apply_crs(r, c, &procs[i], class);
        _ml_clog(r->server, "================================================ finish classifier %d: %s", i, class);
    }
    return rc;
}

/* combine preprocess and classification */
static int _ml_process(request_rec *r, ml_directives_t *c)
{
    int rc;
    rc = _ml_preprocess(r, c);
    rc = _ml_classify(r, c);
    return rc;
}

/* 
 * the top level hook functions 
 */
/* set up the environment for running a hook function */
static ml_directives_t *_ml_setup_conf(request_rec *r)
{
    ap_add_common_vars(r);
    ap_add_cgi_vars(r);
    
    ml_directives_t *dc = ap_get_module_config(r->per_dir_config, &ml_module);
    if (dc == NULL) {
        /* we should never get here */
        dc = ap_get_module_config(r->server->module_config, &ml_module);
    }
    if (dc) {
        dc->r = r;
        /* forces reparse of cgi data */
        dc->cgi = NULL;
    }
    return dc;
}

/* do classification as part of http processing */
static int ml_classifier_hook(request_rec *r) 
{
    ml_directives_t *dc = _ml_setup_conf(r);
    _ml_clog(r->server, "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ start ml_classifier");

    if (dc == NULL || !dc->enabled) return DECLINED;
    dc->is_handler = FALSE;

    /* if there is nothing to do decline */
    if (!dc->classifier || dc->classifier->nelts <= 0) 
        return DECLINED;

    _ml_clog_conf(r, dc,"ml_classifier_hook actually doing something");
    return _ml_classify(r, dc);
}

/* this handler is only place where features can be parsed */
static int ml_handler(request_rec *r) 
{
    ml_directives_t *dc = _ml_setup_conf(r);
    if (!dc->enabled) return DECLINED;

    if (!_ml_handling(r)) return DECLINED;

    r->content_type = "text/plain";      
    _ml_clog(r->server, "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ start ml_handler");

    dc->is_handler = TRUE;
    int http_status = _ml_classify(r, dc);

    /* with SetHandler ml set show what we did */
    if (!r->header_only) {
        ap_rprintf(r, "got http_status %d from classifier", http_status);
        _ml_print_conf(r, dc, "directory conf");
    }
    return OK;
}

/* this is the main hook for the preprocess step */
static int ml_log_after(request_rec *r)
{
    ml_directives_t *dc = _ml_setup_conf(r);

    if (dc == NULL || !dc->enabled) return DECLINED;
    dc->is_handler = FALSE;

    /* if there is nothing to do decline */
    if (!dc->preprocessor || dc->preprocessor->nelts <= 0) 
        return DECLINED;

    const char * s = apr_table_get(r->headers_in, "User-Agent");
    if (s && strstr(s,"(internal dummy connection)")) 
        return DECLINED;

    _ml_clog_conf(r, dc,"ml_log_after doing preprocess");
    _ml_preprocess(r, dc);
    return OK;
}

