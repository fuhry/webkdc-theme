/*
 * service token related stuff
 */

#include "mod_webauth.h"

static MWA_SERVICE_TOKEN *
new_service_token(apr_pool_t *pool,
                  int key_type, 
                  const unsigned char *kdata,
                  int kd_len,
                  const unsigned char *tdata,
                  int td_len,
                  time_t expires,
                  time_t last_renewal_attempt)
{
    MWA_SERVICE_TOKEN *token;

    token = (MWA_SERVICE_TOKEN*) apr_pcalloc(pool, sizeof(MWA_SERVICE_TOKEN));
    token->expires = expires;

    token->token = apr_pstrmemdup(pool, tdata, td_len);

    token->last_renewal_attempt = last_renewal_attempt;
    token->key.type = key_type;

    token->key.data = apr_pstrmemdup(pool, kdata, kd_len);
    token->key.length = kd_len;

    /* FIXME: should validate key.length */
    return token;
}


static MWA_SERVICE_TOKEN *
read_service_token_cache(MWA_REQ_CTXT *rc)
{
    MWA_SERVICE_TOKEN *token;
    apr_file_t *cache;
    apr_finfo_t finfo;
    unsigned char *buffer;
    apr_status_t astatus;
    int status, num_read, tlen, klen;
    int s_expires, s_token, s_lra, s_kt, s_key;
    time_t expires, lra;
    uint32_t key_type;
    char *tok, *key;

    WEBAUTH_ATTR_LIST *alist;
    static char *mwa_func = "read_service_token_cache";

    /* check file */
    astatus = apr_file_open(&cache, rc->sconf->st_cache_path,
                            APR_READ|APR_FILE_NOCLEANUP,
                            APR_UREAD|APR_UWRITE,
                            rc->r->pool);

    if (astatus != APR_SUCCESS) {
        if (!APR_STATUS_IS_ENOENT(astatus)) {
            char errbuff[512];
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server, 
                         "mod_webauth: %s: apr_file_open (%s): %s (%d)",
                         mwa_func,
                         rc->sconf->st_cache_path, 
                         apr_strerror(astatus, errbuff, sizeof(errbuff)),
                         astatus);
        }
        return NULL;
    }

    astatus = apr_file_info_get(&finfo, APR_FINFO_NORM, cache);
    if (astatus != APR_SUCCESS) {
        char errbuff[512];
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server, 
                     "mod_webauth: %s: apr_file_info_get  (%s): %s (%d)", 
                     mwa_func,
                     rc->sconf->st_cache_path, 
                     apr_strerror(astatus, errbuff, sizeof(errbuff)),
                     astatus);
        apr_file_close(cache);
        return NULL;
    }
    buffer = (unsigned char*) apr_palloc(rc->r->pool, finfo.size);

    astatus = apr_file_read_full(cache, buffer, finfo.size, &num_read);
    apr_file_close(cache);

    if (astatus != APR_SUCCESS) {
        char errbuff[512];
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server, 
                     "mod_webauth: %s: pr_file_read_full (%s): %s (%d)",
                     mwa_func,
                     rc->sconf->st_cache_path, 
                     apr_strerror(astatus, errbuff, sizeof(errbuff)),
                     astatus);
    }

    status = webauth_attrs_decode(buffer, finfo.size, &alist);

    if (status != WA_ERR_NONE) {
        mwa_log_webauth_error(rc->r, status, NULL, "mwa_func", 
                              "webauth_attrs_decode");
        return NULL;
    }


    s_expires = webauth_attr_list_get_time(alist, "expires", &expires, 
                                           WA_F_FMT_STR);
    s_token = webauth_attr_list_get_str(alist, "token", &tok, &tlen,
                                        WA_F_NONE);
    s_lra = webauth_attr_list_get_time(alist, "last_renewal_attempt", 
                                       &lra, WA_F_FMT_STR);
    s_kt = webauth_attr_list_get_uint32(alist, "key_type", &key_type,
                                        WA_F_FMT_STR);
    s_key = webauth_attr_list_get(alist, "key", (void*)&key, 
                                  &klen, WA_F_FMT_HEX);

    if ((s_expires != WA_ERR_NONE) || (s_token != WA_ERR_NONE) ||
        (s_lra != WA_ERR_NONE) || (s_kt != WA_ERR_NONE) ||
        (s_key != WA_ERR_NONE)) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server, 
                     "mod_webauth: %s: attr_list_get failed for: %s%s%s%s%s",
                     mwa_func,
                     (s_expires != WA_ERR_NONE) ? "expires" : "",
                     (s_token != WA_ERR_NONE) ? "token" : "",
                     (s_lra != WA_ERR_NONE) ? "lasts_renewal_attempt" : "",
                     (s_kt != WA_ERR_NONE) ? "key_type" : "",
                     (s_key != WA_ERR_NONE) ? "key" : "");
        return NULL;
    }

    /* be careful to alloc all memory for token from process pool */
    token = new_service_token(rc->r->server->process->pool,
                              key_type, key, klen, tok, tlen, expires,
                              lra);
    webauth_attr_list_free(alist);
    return token;
}

static void
write_service_token_cache(MWA_REQ_CTXT *rc, MWA_SERVICE_TOKEN *token)
{
    apr_file_t *cache;
    unsigned char *buffer;
    apr_status_t astatus;
    int status, buff_len, ebuff_len, bytes_written;
    WEBAUTH_ATTR_LIST *alist;
    static char *prefix = "mod_webauth: write_service_token_cache";

    /* FIXME: need to do the whole "new.lock"/rename thingy */

    astatus = apr_file_open(&cache, rc->sconf->st_cache_path,
                            APR_WRITE|APR_CREATE|
                            APR_TRUNCATE |APR_FILE_NOCLEANUP,
                            APR_UREAD|APR_UWRITE,
                            rc->r->pool);

    if (astatus != APR_SUCCESS) {
            char errbuff[512];
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server, 
                         "%s: apr_file_open (%s): %s (%d)",
                         prefix,
                         rc->sconf->st_cache_path, 
                         apr_strerror(astatus, errbuff, sizeof(errbuff)),
                         astatus);
            return;
    }

    alist = webauth_attr_list_new(10);

    webauth_attr_list_add_str(alist, "token", token->token, 0, WA_F_NONE);

    webauth_attr_list_add_uint32(alist, "key_type", 
                                 token->key.type, WA_F_FMT_STR);

    webauth_attr_list_add_time(alist, "expires", 
                                 token->expires, WA_F_FMT_STR);

    webauth_attr_list_add_time(alist, "last_renewal_attempt", 
                                 token->last_renewal_attempt, WA_F_FMT_STR);

    webauth_attr_list_add(alist, "key", token->key.data,
                          token->key.length, WA_F_FMT_HEX);

    buff_len = webauth_attrs_encoded_length(alist);
    buffer = apr_palloc(rc->r->pool, buff_len);

    status = webauth_attrs_encode(alist, buffer, &ebuff_len, buff_len);
    webauth_attr_list_free(alist);

    if (status != WA_ERR_NONE) {
        apr_file_close(cache);
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server,
                     "%s: webauth_attrs_encode failed: %s (%d)",
                     prefix, webauth_error_message(status), status);
        return;
    }

    astatus = apr_file_write_full(cache, buffer, ebuff_len, &bytes_written);
    if (astatus != APR_SUCCESS) {
        char errbuff[512];
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server, 
                     "%s: pr_file_read_full (%s): %s (%d)",
                     prefix,
                     rc->sconf->st_cache_path, 
                     apr_strerror(astatus, errbuff, sizeof(errbuff)),
                     astatus);
    }

    apr_file_close(cache);
    return;
}

#define CHUNK_SIZE 4096

/*
 *
 */
static void init_string(MWA_STRING *string, apr_pool_t *pool)
{
    memset(string, 0, sizeof(MWA_STRING));
    string->pool = pool;
}

/*
 * given an MWA_STRING, append some new data to it.
 */
static void append_string(MWA_STRING *string, const char *in_data, int in_size)
{
    int needed_size;

    if (in_size == 0)
        in_size = strlen(in_data);

    needed_size = string->size+in_size;

    if (string->data == NULL || needed_size > string->capacity) {
        char *new_data;
        while (string->capacity < needed_size+1)
            string->capacity += CHUNK_SIZE;

        new_data = apr_palloc(string->pool, string->capacity);

        if (string->data != NULL) {
            memcpy(new_data, string->data, string->size);
        } 
        /* don't have to free existing data since it from a pool */
        string->data = new_data;
    }
    memcpy(string->data+string->size, in_data, in_size);
    string->size = needed_size;
    /* always null-terminate, we have space becase of the +1 above */
    string->data[string->size] = '\0';
}

/*
 * gather up the POST data as it comes back from webkdc
 */
static size_t
post_gather(void *in_data, size_t size, size_t nmemb,
            MWA_STRING *string)
{
    size_t real_size = size*nmemb;
    append_string(string, (char*)in_data, (int)real_size);
    return real_size;
}

/*
 *post some xml to the webkdc and return response
 *
 * FIXME: need to think about retry/timeout policy
 */
static char *
post_to_webkdc(char *post_data, int post_data_len, MWA_REQ_CTXT *rc)
{
    CURL *curl;
    CURLcode code;
    char curl_error_buff[CURL_ERROR_SIZE+1];
    struct curl_slist *headers=NULL;
    MWA_STRING string;

    if (post_data_len == 0)
        post_data_len = strlen(post_data);

    curl = curl_easy_init();

    if (curl == NULL) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server,
                     "mod_webauth: post_to_webkdc: curl_easy_init failed");
        return NULL;
    }

    curl_easy_setopt(curl, CURLOPT_URL, rc->sconf->webkdc_url);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error_buff);

    /* FIXME: turning this off for testing! */
    ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server,
                 "mod_webauth: WARNING: USING CURLOPT_SSL_VERIFYPEER 0!");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, post_gather);

    /* don't pre-allocate in case our write function never gets called */
    init_string(&string, rc->r->pool);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &string);
    headers = curl_slist_append(headers, "Content-Type: text/xml");
 
    /* data to post */
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data);
 
    /* set the size of the postfields data */
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, post_data_len);
 
    /* pass our list of custom made headers */
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
 
    curl_error_buff[0] = '\0';
    code = curl_easy_perform(curl); /* post away! */
 
    curl_slist_free_all(headers); /* free the header list */

    if (code != CURLE_OK) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server,
                     "mod_webauth: curl_easy_perform: error(%d): %s",
                     code, curl_error_buff);
        return NULL;
    }
    /* null-terminate return data */
    if (string.data) {
        string.data[string.size] = '\0';
    }
    curl_easy_cleanup(curl);
    return string.data;
}

/*
 * concat all the text pieces together and return data
 */
static const char *
get_elem_text(MWA_REQ_CTXT *rc, apr_xml_elem *e, const char *def)
{
    if (e->first_cdata.first &&
        e->first_cdata.first->text) {
        apr_text *t;
        MWA_STRING string;
        init_string(&string, rc->r->pool);
        for (t = e->first_cdata.first; t != NULL; t = t->next) {
            append_string(&string, t->text, 0);
        }
        return string.data;
    } else {
        return def;
    }
}

/*
 * XXX
 */
static void
log_error_response(apr_xml_elem *e,
                   const char *mwa_func,
                   MWA_REQ_CTXT *rc)
{
    apr_xml_elem *sib;
    const char *error_code = "(no error_code)";
    const char *error_message = "(no error message)";

    for (sib = e->first_child; sib; sib = sib->next) {
        if (strcmp(sib->name, "errorCode") == 0) {
            error_code = get_elem_text(rc, sib, error_code);
        } else if (strcmp(sib->name, "errorMessage") == 0) {
            error_message = get_elem_text(rc, sib, error_message);
        } else {
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server, 
                         "mod_webauth: log_error_response: "
                         "ignoring unknown element in <errorResponse>: <%s>",
                         sib->name);
        }
    }
    ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server, 
                 "mod_webauth: %s: errorResponse from webkdc: errorCode(%s) "
                 "errorMessage(%s)",
                 mwa_func, error_code, error_message);

}

/*
 * pass in st_pool in case we want service token in process pool
 */
static MWA_SERVICE_TOKEN *
parse_service_token_response(apr_xml_doc *xd,
                             MWA_REQ_CTXT *rc)
{
    MWA_SERVICE_TOKEN *st;
    apr_xml_elem *e, *sib;
    int bskey_len;
    char *bskey;
    static const char *mwa_func = "parse_service_token_response";
    const char *expires, *session_key, *token_data;
    
    e = xd->root;

    if (strcmp(e->name, "errorResponse") == 0) {
        log_error_response(e, mwa_func, rc);
        return NULL;
    } else if (strcmp(e->name, "getTokensResponse") != 0) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server, 
                     "mod_webauth: %s: unknown response(%s)", 
                     mwa_func, e->name);
        return NULL;
    }

    /* parse it already */
    e = e->first_child;
    if (!e || strcmp(e->name, "tokens") != 0) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server, 
                     "mod_webauth: %s: can't find <tokens>", 
                     mwa_func);
        return NULL;
    }

    e = e->first_child;
    if (!e || strcmp(e->name, "token") != 0) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server, 
                     "mod_webauth: %s: can't find <token>", 
                     mwa_func);
        return NULL;
    }

    session_key = expires = token_data = NULL;

    for (sib = e->first_child; sib; sib = sib->next) {
        if (strcmp(sib->name, "sessionKey") == 0) {
            session_key = get_elem_text(rc, sib, NULL);
        } else if (strcmp(sib->name, "expires") == 0) {
            expires = get_elem_text(rc, sib, NULL);
        } else if (strcmp(sib->name, "tokenData") == 0) {
            token_data = get_elem_text(rc, sib, NULL);
        } else {
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server, 
                         "mod_webauth: %s: "
                         "ignoring unknown element in <token>: <%s>",
                         mwa_func, sib->name);
        }
    }

    if ((session_key == NULL) || (expires == NULL) || (token_data == NULL)) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server, 
                     "mod_webauth: %s: "
                     "missing %s%s%s",
                     mwa_func, 
                     session_key == NULL ? "<sessionKey> " : "",
                     expires == NULL ? "<expires> " : "",
                     token_data == NULL ? "<tokenData> " : "");
        return NULL;
    }

    bskey = (char*)apr_palloc(rc->r->pool, apr_base64_decode_len(session_key));
    bskey_len = apr_base64_decode(bskey, session_key);

    st = new_service_token(rc->r->server->process->pool,
                           WA_AES_KEY, /* FIXME: hardcoded for now */
                           bskey,
                           bskey_len,
                           token_data,
                           strlen(token_data),
                           atoi(expires),
                           0);
    return st;
}

/*
 * request a service token from the WebKDC
 */
static MWA_SERVICE_TOKEN *
request_service_token(MWA_REQ_CTXT *rc)
{
    WEBAUTH_KRB5_CTXT *ctxt;
    apr_xml_parser *xp;
    apr_xml_doc *xd;
    char *xml_request, *xml_response;
    unsigned char *k5_req, *bk5_req;
    int status, k5_req_len, bk5_req_len;
    static const char *mwa_func = "request_service_token";
    apr_status_t astatus;

    ctxt = mwa_get_webauth_krb5_ctxt(rc->r, mwa_func);
    if (ctxt == NULL)
        return 0;

    status = webauth_krb5_init_via_keytab(ctxt, rc->sconf->keytab_path, NULL);
    if (status != WA_ERR_NONE) {
        mwa_log_webauth_error(rc->r, status, ctxt, mwa_func,
                              "webauth_krb5_init_via_keytab");
        webauth_krb5_free(ctxt);
        return 0;
    }

    status = webauth_krb5_mk_req(ctxt, rc->sconf->webkdc_principal, 
                                 &k5_req, &k5_req_len);
    if (status != WA_ERR_NONE) {
        mwa_log_webauth_error(rc->r, status, ctxt, mwa_func,
                              "webauth_krb5_mk_req");
        webauth_krb5_free(ctxt);
        return 0;
    }
    webauth_krb5_free(ctxt);

    bk5_req_len = apr_base64_encode_len(k5_req_len);
    bk5_req = (char*) apr_palloc(rc->r->pool, bk5_req_len);
    apr_base64_encode(bk5_req, k5_req, k5_req_len);
    free(k5_req);
    
    xml_request = apr_pstrcat(rc->r->pool, 
                              "<getTokensRequest>"
                              "<requesterCredential type='krb5'>",
                              bk5_req,
                              "</requesterCredential>"
                              "<tokens><token type='service'/></tokens>"
                              "</getTokensRequest>",
                              NULL);

    ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server, 
                 "mod_webauth: xml_request(%s)", xml_request);


    xml_response = post_to_webkdc(xml_request, 0, rc);

    if (xml_response == NULL)
        return 0;

    ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server, 
                 "mod_webauth: xml_response(%s)", xml_response);

    
    xp = apr_xml_parser_create(rc->r->pool);
    if (xp == NULL) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server, 
                     "mod_webauth: %s: apr_xml_parser_create failed", 
                     mwa_func);
        return 0;
    }

    astatus = apr_xml_parser_feed(xp, xml_response, strlen(xml_response));
    if (astatus == APR_SUCCESS) {
        astatus = apr_xml_parser_done(xp, &xd);
    }

    if (astatus != APR_SUCCESS) {
        char errbuff[1024];
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server, 
                     "mod_webauth: %s: "
                     "apr_xml_parser_{feed,done} failed: %s (%d)", 
                     mwa_func,
                     apr_xml_parser_geterror(xp, errbuff, sizeof(errbuff)),
                     astatus);
        return 0;
    }

    ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server, 
                 "mod_webauth: xml doc root(%s)", xd->root->name);

    return parse_service_token_response(xd, rc);
}

/*
 * generate our app-state blob once and re-use it
 */
static void
get_app_state(MWA_REQ_CTXT *rc, MWA_SERVICE_TOKEN *token, time_t curr)
{
   WEBAUTH_ATTR_LIST *alist;
   int tlen, olen, status;
   void *as;

   token->app_state = NULL;
   token->app_state_len = 0;   

   alist = webauth_attr_list_new(10);
   if (alist == NULL) {
       ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server,
                    "mod_webauth: get_app_state: "
                    "webauth_attr_list_new failed");
       return;
   }
   webauth_attr_list_add_str(alist, WA_TK_TOKEN_TYPE, WA_TT_APP, 0, 
                             WA_F_NONE);
   webauth_attr_list_add(alist, WA_TK_SESSION_KEY, 
                         token->key.data, token->key.length, WA_F_NONE);
   webauth_attr_list_add_time(alist, WA_TK_EXPIRATION_TIME,
                              token->expires, WA_F_NONE);

   tlen = webauth_token_encoded_length(alist);

   as = (char*)apr_palloc(rc->r->server->process->pool, tlen);

   status = webauth_token_create(alist, curr,
                                 (unsigned char*)as, &olen, tlen, mwa_g_ring);
    webauth_attr_list_free(alist);

    if (status != WA_ERR_NONE) {
        mwa_log_webauth_error(rc->r, status, NULL,
                              "get_app_state",
                              "webauth_token_create");
    } else {
        token->app_state = as;
        token->app_state_len = tlen;
    }
    return;
}

/*
 * this function returns a service-token to ues.
 * 
 * it looks in memory first, then the service token cache, then makes
 * a request if all else fails.
 *
 * it also does housekeeping on the service token, such as attempting
 * to request a new one while the current one is still active but nearing
 * expiration.
 *
 */
MWA_SERVICE_TOKEN *
mwa_get_service_token(MWA_REQ_CTXT *rc)
{
    MWA_SERVICE_TOKEN *token;
    time_t curr = time(NULL); /* rc->r->request_time didn't seem reliable */
    /* FIXME: LOCKING OF GLOBAL VARIABLE */

    /* FIXME: 3600 MAGIC! */
    if (mwa_g_service_token != NULL && 
        (mwa_g_service_token->expires-3600 > curr)) {
        return mwa_g_service_token;
    }

    /* check file to see if there is a newer token */
    token = read_service_token_cache(rc);

    if (token != NULL) {
        mwa_g_service_token = token;
        get_app_state(rc, token, curr);
        return token;
    }

    token = request_service_token(rc);

    if (token != NULL) {
        write_service_token_cache(rc, token);
        get_app_state(rc, token, curr);
        mwa_g_service_token = token;
    } else {
       ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server,
                    "mod_webauth: mwa_get_service_token failed!");
    }
    return token;
}

