#include <net-snmp/net-snmp-config.h>

#include <net-snmp/library/snmpTLSBaseDomain.h>

#if HAVE_DMALLOC_H
#include <dmalloc.h>
#endif
#if HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#if HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#if HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#if HAVE_NETDB_H
#include <netdb.h>
#endif
#include <errno.h>

/* OpenSSL Includes */
#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#include <net-snmp/types.h>
#include <net-snmp/library/cert_util.h>
#include <net-snmp/library/snmp_openssl.h>
#include <net-snmp/library/default_store.h>
#include <net-snmp/library/callback.h>
#include <net-snmp/library/snmp_logging.h>
#include <net-snmp/library/snmp_api.h>
#include <net-snmp/library/tools.h>
#include <net-snmp/library/snmp_debug.h>
#include <net-snmp/library/snmp_assert.h>
#include <net-snmp/library/snmp_transport.h>
#include <net-snmp/library/snmp_secmod.h>

#define LOGANDDIE(msg) { snmp_log(LOG_ERR, "%s\n", msg); return 0; }

/* this is called during negotiationn */
int verify_callback(int ok, X509_STORE_CTX *ctx) {
    int err, depth;
    char buf[1024], *fingerprint;;
    X509 *thecert;
    netsnmp_cert *cert;

    thecert = X509_STORE_CTX_get_current_cert(ctx);
    err = X509_STORE_CTX_get_error(ctx);
    depth = X509_STORE_CTX_get_error_depth(ctx);
    
    /* things to do: */

    X509_NAME_oneline(X509_get_subject_name(thecert), buf, sizeof(buf));
    fingerprint = netsnmp_openssl_cert_get_fingerprint(thecert, -1);
    DEBUGMSGTL(("tls_x509:verify", "Cert: %s\n", buf));
    DEBUGMSGTL(("tls_x509:verify", "  fp: %s\n", fingerprint ?
                fingerprint : "unknown"));
    cert = netsnmp_cert_find(NS_CERT_REMOTE_PEER, NS_CERTKEY_FINGERPRINT,
                             (void*)fingerprint);
    DEBUGMSGTL(("tls_x509:verify",
                " value: %d, depth=%d, error code=%d, error string=%s\n",
                ok, depth, err, _x509_get_error(err, "verify callback")));

    if (cert) {
        /* XXXWJH: should really only accept here *IF* in the agent's
         * configuration to accept from the given table; this accepts
         * anything found here with no other configuration.
         */
        DEBUGMSGTL(("tls_x509:verify", "  matching fp found in %s; accepting\n",
                    cert->info.filename));
        return 1;
    } else
        DEBUGMSGTL(("tls_x509:verify", "  no matching fp found\n"));

    DEBUGMSGTL(("tls_x509:verify", "  returning the passed in value of %d\n",
                ok));
    return(ok);
}

static int
_netsnmp_tlsbase_verify_remote_fingerprint(X509 *remote_cert,
                                           _netsnmpTLSBaseData *tlsdata) {

    char            *fingerprint;

    fingerprint =
        netsnmp_openssl_cert_get_fingerprint(remote_cert, -1);
    if (!fingerprint) {
        /* no peer cert */
        snmp_log(LOG_ERR, "failed to get fingerprint of remote certificate\n");
        return SNMPERR_GENERR;
    }

    netsnmp_fp_lowercase_and_strip_colon(tlsdata->their_identity);
    if (tlsdata->their_identity &&
        0 != strcmp(tlsdata->their_identity, fingerprint)) {
        snmp_log(LOG_ERR, "The fingerprint from the remote side's certificate didn't match the expected\n");
        snmp_log(LOG_ERR, "  %s != %s\n",
                 fingerprint, tlsdata->their_identity);
        free(fingerprint);
        return SNMPERR_GENERR;
    }

    free(fingerprint);
    return SNMPERR_SUCCESS;
}

/* this is called after the connection on the client side by us to check
   other aspects about the connection */
int
netsnmp_tlsbase_verify_server_cert(SSL *ssl, _netsnmpTLSBaseData *tlsdata) {
    /* XXX */
    X509            *remote_cert;

    netsnmp_assert_or_return(ssl != NULL, SNMPERR_GENERR);
    netsnmp_assert_or_return(tlsdata != NULL, SNMPERR_GENERR);
    
    if (NULL == (remote_cert = SSL_get_peer_certificate(ssl))) {
        /* no peer cert */
        DEBUGMSGTL(("tls_x509:verify",
                    "remote connection provided no certificate (yet)\n"));
        return SNMPERR_TLS_NO_CERTIFICATE;
    }

    if (_netsnmp_tlsbase_verify_remote_fingerprint(remote_cert, tlsdata) !=
        SNMPERR_SUCCESS)
        return SNMPERR_GENERR;

    DEBUGMSGTL(("tls_x509:verify", "Verified server fingerprint\n"));
    tlsdata->flags |= NETSNMP_TLSBASE_CERT_FP_VERIFIED;
    return SNMPERR_SUCCESS;
}

/* this is called after the connection on the server side by us to check
   the validity of the client's certificate */
int
netsnmp_tlsbase_verify_client_cert(SSL *ssl, _netsnmpTLSBaseData *tlsdata) {
    /* XXX */
    X509            *remote_cert;

    if (NULL == (remote_cert = SSL_get_peer_certificate(ssl))) {
        /* no peer cert */
        DEBUGMSGTL(("tls_x509:verify",
                    "remote connection provided no certificate (yet)\n"));
        return SNMPERR_TLS_NO_CERTIFICATE;
    }
    
    if (_netsnmp_tlsbase_verify_remote_fingerprint(remote_cert, tlsdata) !=
        SNMPERR_SUCCESS)
        return SNMPERR_GENERR;

    DEBUGMSGTL(("tls_x509:verify", "Verified client fingerprint\n"));
    tlsdata->flags |= NETSNMP_TLSBASE_CERT_FP_VERIFIED;
    return SNMPERR_SUCCESS;
}

/* this is called after the connection on the server side by us to
   check other aspects about the connection and obtain the
   securityName from the remote certificate. */
int
netsnmp_tlsbase_extract_security_name(SSL *ssl, _netsnmpTLSBaseData *tlsdata) {
    netsnmp_container  *chain_maps;
    netsnmp_cert_map   *cert_map, *peer_cert;
    netsnmp_iterator  *itr;
    int                 rc;

    netsnmp_assert_or_return(ssl != NULL, SNMPERR_GENERR);
    netsnmp_assert_or_return(tlsdata != NULL, SNMPERR_GENERR);

    if (NULL == (chain_maps = netsnmp_openssl_get_cert_chain(ssl)))
        return SNMPERR_GENERR;
    /*
     * map fingerprints to mapping entries
     */
    rc = netsnmp_cert_get_secname_maps(chain_maps);
    if ((-1 == rc) || (CONTAINER_SIZE(chain_maps) == 0)) {
        netsnmp_cert_map_container_free(chain_maps);
        return SNMPERR_GENERR;
    }

    /*
     * change container to sorted (by clearing unsorted option), then
     * iterate over it until we find a map that returns a secname.
     */
    CONTAINER_SET_OPTIONS(chain_maps, 0, rc);
    itr = CONTAINER_ITERATOR(chain_maps);
    if (NULL == itr) {
        snmp_log(LOG_ERR, "could not get iterator for secname fingerprints\n");
        netsnmp_cert_map_container_free(chain_maps);
        return SNMPERR_GENERR;
    }
    peer_cert = cert_map = ITERATOR_FIRST(itr);
    for( ; !tlsdata->securityName && cert_map; cert_map = ITERATOR_NEXT(itr))
        tlsdata->securityName =
            netsnmp_openssl_extract_secname(cert_map, peer_cert);
    ITERATOR_RELEASE(itr);

    netsnmp_cert_map_container_free(chain_maps);
       
    return (tlsdata->securityName ? SNMPERR_SUCCESS : SNMPERR_GENERR);
}

SSL_CTX *
sslctx_client_setup(SSL_METHOD *method, _netsnmpTLSBaseData *tlsbase) {
    netsnmp_cert *id_cert, *peer_cert;
    SSL_CTX      *the_ctx;
    X509_STORE   *cert_store = NULL;

    /***********************************************************************
     * Set up the client context
     */
    the_ctx = SSL_CTX_new(method);
    if (!the_ctx) {
        snmp_log(LOG_ERR, "ack: %p\n", the_ctx);
        LOGANDDIE("can't create a new context");
    }
    SSL_CTX_set_read_ahead (the_ctx, 1); /* Required for DTLS */
        
    SSL_CTX_set_verify(the_ctx,
                       SSL_VERIFY_PEER|
                       SSL_VERIFY_FAIL_IF_NO_PEER_CERT|
                       SSL_VERIFY_CLIENT_ONCE,
                       &verify_callback);

    if (tlsbase->our_identity) {
        id_cert = netsnmp_cert_find(NS_CERT_IDENTITY, NS_CERTKEY_MULTIPLE,
                                    tlsbase->our_identity);
    } else {
        id_cert = netsnmp_cert_find(NS_CERT_IDENTITY, NS_CERTKEY_DEFAULT, NULL);
    }

    if (!id_cert)
        LOGANDDIE ("error finding client identity keys");

    if (!id_cert->key || !id_cert->key->okey)
        LOGANDDIE("failed to load private key");

    DEBUGMSGTL(("sslctx_client", "using public key: %s\n",
                id_cert->info.filename));
    DEBUGMSGTL(("sslctx_client", "using private key: %s\n",
                id_cert->key->info.filename));

    if (SSL_CTX_use_certificate(the_ctx, id_cert->ocert) <= 0)
        LOGANDDIE("failed to set the certificate to use");

    if (SSL_CTX_use_PrivateKey(the_ctx, id_cert->key->okey) <= 0)
        LOGANDDIE("failed to set the private key to use");

    if (!SSL_CTX_check_private_key(the_ctx))
        LOGANDDIE("public and private keys incompatible");

    if (tlsbase->their_identity)
        peer_cert = netsnmp_cert_find(NS_CERT_REMOTE_PEER,
                                      NS_CERTKEY_MULTIPLE,
                                      tlsbase->their_identity);
    else
        peer_cert = netsnmp_cert_find(NS_CERT_REMOTE_PEER, NS_CERTKEY_DEFAULT,
                                      NULL);
    if (!peer_cert)
        LOGANDDIE ("error finding client remote keys");

    cert_store = SSL_CTX_get_cert_store(the_ctx);
    if (!cert_store)
        LOGANDDIE("failed to find certificate store");

    if (0 == X509_STORE_add_cert(cert_store, peer_cert->ocert)) {
        netsnmp_openssl_err_log("adding peer to cert store");
        LOGANDDIE("failed to add peer to certificate store");
    }

    /** add all the certs in the clients chain */
    id_cert = id_cert->issuer_cert;
    for (; id_cert; id_cert = id_cert->issuer_cert) {
        if (NULL == id_cert->ocert) {
            DEBUGMSGT(("sslctx_client", "issuer missing x509 cert!\n"));
            continue;
        }
        if(! SSL_CTX_add_extra_chain_cert(the_ctx, id_cert->ocert)) {
            LOGANDDIE("failed to load truststore");
            /* Handle failed load here */
        }
    }

#if 0 /** XXX CAs? */
    if (!SSL_CTX_set_default_verify_paths(the_ctx)) {
        LOGANDDIE ("failed to set default verify path");
    }
#endif

    return the_ctx;
}

SSL_CTX *
sslctx_server_setup(SSL_METHOD *method) {
    netsnmp_cert *id_cert;

    /***********************************************************************
     * Set up the server context
     */
    /* setting up for ssl */
    SSL_CTX *the_ctx = SSL_CTX_new(method);
    if (!the_ctx) {
        LOGANDDIE("can't create a new context");
    }

    id_cert = netsnmp_cert_find(NS_CERT_IDENTITY, NS_CERTKEY_DEFAULT,
                                (void*)1);
    if (!id_cert)
        LOGANDDIE ("error finding server identity keys");

    if (!id_cert->key || !id_cert->key->okey)
        LOGANDDIE("failed to load private key");

    DEBUGMSGTL(("sslctx_server", "using public key: %s\n",
                id_cert->info.filename));
    DEBUGMSGTL(("sslctx_server", "using private key: %s\n",
                id_cert->key->info.filename));

    if (SSL_CTX_use_certificate(the_ctx, id_cert->ocert) <= 0)
        LOGANDDIE("failed to set the certificate to use");

    if (SSL_CTX_use_PrivateKey(the_ctx, id_cert->key->okey) <= 0)
        LOGANDDIE("failed to set the private key to use");

    if (!SSL_CTX_check_private_key(the_ctx))
        LOGANDDIE("public and private keys incompatible");

    SSL_CTX_set_read_ahead(the_ctx, 1); /* XXX: DTLS only? */

#if 0
    certfile = netsnmp_ds_get_string(NETSNMP_DS_LIBRARY_ID,
                                     NETSNMP_DS_LIB_X509_CLIENT_CERTS);
    if(! SSL_CTX_load_verify_locations(the_ctx, certfile, NULL)) {
        LOGANDDIE("failed to load truststore");
        /* Handle failed load here */
    }
#endif

    SSL_CTX_set_verify(the_ctx,
                       SSL_VERIFY_PEER|
                       SSL_VERIFY_FAIL_IF_NO_PEER_CERT|
                       SSL_VERIFY_CLIENT_ONCE,
                       &verify_callback);

    return the_ctx;
}

int
netsnmp_tlsbase_config(struct netsnmp_transport_s *t, const char *token, const char *value) {
    _netsnmpTLSBaseData *tlsdata;

    netsnmp_assert_or_return(t != NULL, -1);
    netsnmp_assert_or_return(t->data != NULL, -1);

    tlsdata = t->data;

    if (strcmp(token, "our_identity") == 0 ||
        /* XXX: remove this option after a few weeks */
        strcmp(token, "my_fingerprint") == 0) {
        SNMP_FREE(tlsdata->our_identity);
        tlsdata->our_identity = strdup(value);
    }

    if (strcmp(token, "their_identity") == 0 ||
        /* XXX: remove this option after a few weeks */
        strcmp(token, "their_fingerprint") == 0) {
        SNMP_FREE(tlsdata->their_identity);
        tlsdata->their_identity = strdup(value);
    }
    return SNMPERR_SUCCESS;
}

int
netsnmp_tlsbase_session_init(struct netsnmp_transport_s *transport,
                             struct snmp_session *sess) {
    /* the default security model here should be TSM; most other
       things won't work with TLS because we'll throw out the packet
       if it doesn't have a proper tmStateRef (and onyl TSM generates
       this at the moment */
    if (!(transport->flags & NETSNMP_TRANSPORT_FLAG_LISTEN)) {
        if (sess->securityModel == SNMP_DEFAULT_SECMODEL) {
            sess->securityModel = NETSNMP_TSM_SECURITY_MODEL;
        } else {
            snmp_log(LOG_ERR, "A security model other than TSM is being used with (D)TLS; this likely won't work\n");
        }

        if (NULL == sess->securityName) {
            /* client side doesn't need a real securityName */
            /* XXX: call-home issues require them to set one for VACM; but
               we don't do callhome yet */
            sess->securityName = strdup("__BOGUS__");
            sess->securityNameLen = strlen(sess->securityName);
        }

        if (SNMP_DEFAULT_VERSION == sess->version) {
            sess->version = SNMP_VERSION_3;
        } else if (sess->version != SNMP_VERSION_3) {
            snmp_log(LOG_ERR, "A SNMP version other than 3 was requested with (D)TLS; this likely won't work\n");
        }

        if (sess->securityLevel <= 0) {
            sess->securityLevel = SNMP_SEC_LEVEL_AUTHPRIV;
        }
    }
    return SNMPERR_SUCCESS;
}

static int have_done_bootstrap = 0;

static int
tls_bootstrap(int majorid, int minorid, void *serverarg, void *clientarg) {
    /* don't do this more than once */
    if (have_done_bootstrap)
        return 0;
    have_done_bootstrap = 1;

    netsnmp_certs_load();

    return 0;
}

void
netsnmp_tlsbase_ctor(void) {

    /* bootstrap ssl since we'll need it */
    netsnmp_init_openssl();

    /* the private client cert to authenticate with */
    netsnmp_ds_register_config(ASN_OCTET_STR, "snmp", "extraX509SubDir",
                               NETSNMP_DS_LIBRARY_ID,
                               NETSNMP_DS_LIB_CERT_EXTRA_SUBDIR);

    /*
     * for the client
     */

    /* pem file of valid server CERT CAs */
    netsnmp_ds_register_config(ASN_OCTET_STR, "snmp", "defX509ServerCerts",
                               NETSNMP_DS_LIBRARY_ID,
                               NETSNMP_DS_LIB_X509_SERVER_CERTS);

    /* the public client cert to authenticate with */
    netsnmp_ds_register_config(ASN_OCTET_STR, "snmp", "defX509ClientPub",
                               NETSNMP_DS_LIBRARY_ID,
                               NETSNMP_DS_LIB_X509_CLIENT_PUB);

    /* the private client cert to authenticate with */
    netsnmp_ds_register_config(ASN_OCTET_STR, "snmp", "defX509ClientPriv",
                               NETSNMP_DS_LIBRARY_ID,
                               NETSNMP_DS_LIB_X509_CLIENT_PRIV);

    /*
     * for the server
     */

    /* The list of valid client keys to accept (or CAs I think) */
    netsnmp_ds_register_config(ASN_OCTET_STR, "snmp", "defX509ClientCerts",
                               NETSNMP_DS_LIBRARY_ID,
                               NETSNMP_DS_LIB_X509_CLIENT_CERTS);

    /* The X509 server key to use */
    netsnmp_ds_register_config(ASN_OCTET_STR, "snmp", "defX509ServerPub",
                               NETSNMP_DS_LIBRARY_ID,
                               NETSNMP_DS_LIB_X509_SERVER_PUB);

    netsnmp_ds_register_config(ASN_OCTET_STR, "snmp", "defX509ServerPriv",
                               NETSNMP_DS_LIBRARY_ID,
                               NETSNMP_DS_LIB_X509_SERVER_PRIV);

    /*
     * register our boot-strapping needs
     */
    snmp_register_callback(SNMP_CALLBACK_LIBRARY,
			   SNMP_CALLBACK_POST_READ_CONFIG,
			   tls_bootstrap, NULL);

}

_netsnmpTLSBaseData *
netsnmp_tlsbase_allocate_tlsdata(netsnmp_transport *t, int isserver) {

    _netsnmpTLSBaseData *tlsdata;

    if (NULL == t)
        return NULL;

    /* allocate our TLS specific data */
    tlsdata = SNMP_MALLOC_TYPEDEF(_netsnmpTLSBaseData);
    if (NULL == tlsdata) {
        SNMP_FREE(t);
        return NULL;
    }

    if (!isserver)
        tlsdata->flags |= NETSNMP_TLSBASE_IS_CLIENT;

    return tlsdata;
}


int netsnmp_tlsbase_wrapup_recv(netsnmp_tmStateReference *tmStateRef,
                                _netsnmpTLSBaseData *tlsdata,
                                void **opaque, int *olength) {
    /* RFCXXXX Section 5.1.2 step 2: tmSecurityLevel */
    /* XXX: disallow NULL auth/encr algs in our implementations */
    tmStateRef->transportSecurityLevel = SNMP_SEC_LEVEL_AUTHPRIV;

    /* use x509 cert to do lookup to secname if DNE in cachep yet */
    if (!tlsdata->securityName) {
        netsnmp_tlsbase_extract_security_name(tlsdata->ssl, tlsdata);
        if (NULL != tlsdata->securityName) {
            DEBUGMSGTL(("tlstcp", "set SecName to: %s\n",
                        tlsdata->securityName));
        } else {
            SNMP_FREE(tmStateRef);
            return SNMPERR_GENERR;
        }
    }

    /* RFCXXXX Section 5.1.2 step 2: tmSecurityName */
    /* XXX: detect and throw out overflow secname sizes rather
       than truncating. */
    strncpy(tmStateRef->securityName, tlsdata->securityName,
            sizeof(tmStateRef->securityName)-1);
    tmStateRef->securityName[sizeof(tmStateRef->securityName)-1] = '\0';
    tmStateRef->securityNameLen = strlen(tmStateRef->securityName);

    /* RFCXXXX Section 5.1.2 step 2: tmSessionID */
    /* use our TLSData pointer as the session ID */
    memcpy(tmStateRef->sessionID, &tlsdata, sizeof(netsnmp_tmStateReference *));

    /* save the tmStateRef in our special pointer */
    *opaque = tmStateRef;
    *olength = sizeof(netsnmp_tmStateReference);

    return SNMPERR_SUCCESS;
}

const char * _x509_get_error(int x509failvalue, const char *location) {
    static const char *reason = NULL;
    
    /* XXX: use this instead: X509_verify_cert_error_string(err) */

    switch (x509failvalue) {
    case X509_V_OK:
        reason = "X509_V_OK";
        break;
    case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT:
        reason = "X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT";
        break;
    case X509_V_ERR_UNABLE_TO_GET_CRL:
        reason = "X509_V_ERR_UNABLE_TO_GET_CRL";
        break;
    case X509_V_ERR_UNABLE_TO_DECRYPT_CERT_SIGNATURE:
        reason = "X509_V_ERR_UNABLE_TO_DECRYPT_CERT_SIGNATURE";
        break;
    case X509_V_ERR_UNABLE_TO_DECRYPT_CRL_SIGNATURE:
        reason = "X509_V_ERR_UNABLE_TO_DECRYPT_CRL_SIGNATURE";
        break;
    case X509_V_ERR_UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY:
        reason = "X509_V_ERR_UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY";
        break;
    case X509_V_ERR_CERT_SIGNATURE_FAILURE:
        reason = "X509_V_ERR_CERT_SIGNATURE_FAILURE";
        break;
    case X509_V_ERR_CRL_SIGNATURE_FAILURE:
        reason = "X509_V_ERR_CRL_SIGNATURE_FAILURE";
        break;
    case X509_V_ERR_CERT_NOT_YET_VALID:
        reason = "X509_V_ERR_CERT_NOT_YET_VALID";
        break;
    case X509_V_ERR_CERT_HAS_EXPIRED:
        reason = "X509_V_ERR_CERT_HAS_EXPIRED";
        break;
    case X509_V_ERR_CRL_NOT_YET_VALID:
        reason = "X509_V_ERR_CRL_NOT_YET_VALID";
        break;
    case X509_V_ERR_CRL_HAS_EXPIRED:
        reason = "X509_V_ERR_CRL_HAS_EXPIRED";
        break;
    case X509_V_ERR_ERROR_IN_CERT_NOT_BEFORE_FIELD:
        reason = "X509_V_ERR_ERROR_IN_CERT_NOT_BEFORE_FIELD";
        break;
    case X509_V_ERR_ERROR_IN_CERT_NOT_AFTER_FIELD:
        reason = "X509_V_ERR_ERROR_IN_CERT_NOT_AFTER_FIELD";
        break;
    case X509_V_ERR_ERROR_IN_CRL_LAST_UPDATE_FIELD:
        reason = "X509_V_ERR_ERROR_IN_CRL_LAST_UPDATE_FIELD";
        break;
    case X509_V_ERR_ERROR_IN_CRL_NEXT_UPDATE_FIELD:
        reason = "X509_V_ERR_ERROR_IN_CRL_NEXT_UPDATE_FIELD";
        break;
    case X509_V_ERR_OUT_OF_MEM:
        reason = "X509_V_ERR_OUT_OF_MEM";
        break;
    case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:
        reason = "X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT";
        break;
    case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN:
        reason = "X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN";
        break;
    case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY:
        reason = "X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY";
        break;
    case X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE:
        reason = "X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE";
        break;
    case X509_V_ERR_CERT_CHAIN_TOO_LONG:
        reason = "X509_V_ERR_CERT_CHAIN_TOO_LONG";
        break;
    case X509_V_ERR_CERT_REVOKED:
        reason = "X509_V_ERR_CERT_REVOKED";
        break;
    case X509_V_ERR_INVALID_CA:
        reason = "X509_V_ERR_INVALID_CA";
        break;
    case X509_V_ERR_PATH_LENGTH_EXCEEDED:
        reason = "X509_V_ERR_PATH_LENGTH_EXCEEDED";
        break;
    case X509_V_ERR_INVALID_PURPOSE:
        reason = "X509_V_ERR_INVALID_PURPOSE";
        break;
    case X509_V_ERR_CERT_UNTRUSTED:
        reason = "X509_V_ERR_CERT_UNTRUSTED";
        break;
    case X509_V_ERR_CERT_REJECTED:
        reason = "X509_V_ERR_CERT_REJECTED";
        break;
    case X509_V_ERR_SUBJECT_ISSUER_MISMATCH:
        reason = "X509_V_ERR_SUBJECT_ISSUER_MISMATCH";
        break;
    case X509_V_ERR_AKID_SKID_MISMATCH:
        reason = "X509_V_ERR_AKID_SKID_MISMATCH";
        break;
    case X509_V_ERR_AKID_ISSUER_SERIAL_MISMATCH:
        reason = "X509_V_ERR_AKID_ISSUER_SERIAL_MISMATCH";
        break;
    case X509_V_ERR_KEYUSAGE_NO_CERTSIGN:
        reason = "X509_V_ERR_KEYUSAGE_NO_CERTSIGN";
        break;
    case X509_V_ERR_UNABLE_TO_GET_CRL_ISSUER:
        reason = "X509_V_ERR_UNABLE_TO_GET_CRL_ISSUER";
        break;
    case X509_V_ERR_UNHANDLED_CRITICAL_EXTENSION:
        reason = "X509_V_ERR_UNHANDLED_CRITICAL_EXTENSION";
        break;
    case X509_V_ERR_KEYUSAGE_NO_CRL_SIGN:
        reason = "X509_V_ERR_KEYUSAGE_NO_CRL_SIGN";
        break;
    case X509_V_ERR_UNHANDLED_CRITICAL_CRL_EXTENSION:
        reason = "X509_V_ERR_UNHANDLED_CRITICAL_CRL_EXTENSION";
        break;
    case X509_V_ERR_INVALID_NON_CA:
        reason = "X509_V_ERR_INVALID_NON_CA";
        break;
    case X509_V_ERR_PROXY_PATH_LENGTH_EXCEEDED:
        reason = "X509_V_ERR_PROXY_PATH_LENGTH_EXCEEDED";
        break;
    case X509_V_ERR_KEYUSAGE_NO_DIGITAL_SIGNATURE:
        reason = "X509_V_ERR_KEYUSAGE_NO_DIGITAL_SIGNATURE";
        break;
    case X509_V_ERR_PROXY_CERTIFICATES_NOT_ALLOWED:
        reason = "X509_V_ERR_PROXY_CERTIFICATES_NOT_ALLOWED";
        break;
    case X509_V_ERR_INVALID_EXTENSION:
        reason = "X509_V_ERR_INVALID_EXTENSION";
        break;
    case X509_V_ERR_INVALID_POLICY_EXTENSION:
        reason = "X509_V_ERR_INVALID_POLICY_EXTENSION";
        break;
    case X509_V_ERR_NO_EXPLICIT_POLICY:
        reason = "X509_V_ERR_NO_EXPLICIT_POLICY";
        break;
    case X509_V_ERR_UNNESTED_RESOURCE:
        reason = "X509_V_ERR_UNNESTED_RESOURCE";
        break;
    case X509_V_ERR_APPLICATION_VERIFICATION:
        reason = "X509_V_ERR_APPLICATION_VERIFICATION";
    default:
        reason = "unknown failure code";
    }

    return reason;
}

void _openssl_log_error(int rc, SSL *con, const char *location) {
    const char     *reason, *file, *data;
    unsigned long   numerical_reason;
    int             flags, line;

    snmp_log(LOG_ERR, "---- OpenSSL Related Erorrs: ----\n");

    /* SSL specific errors */
    if (con) {

        int sslnum = SSL_get_error(con, rc);

        switch(sslnum) {
        case SSL_ERROR_NONE:
            reason = "SSL_ERROR_NONE";
            break;

        case SSL_ERROR_SSL:
            reason = "SSL_ERROR_SSL";
            break;

        case SSL_ERROR_WANT_READ:
            reason = "SSL_ERROR_WANT_READ";
            break;

        case SSL_ERROR_WANT_WRITE:
            reason = "SSL_ERROR_WANT_WRITE";
            break;

        case SSL_ERROR_WANT_X509_LOOKUP:
            reason = "SSL_ERROR_WANT_X509_LOOKUP";
            break;

        case SSL_ERROR_SYSCALL:
            reason = "SSL_ERROR_SYSCALL";
            snmp_log(LOG_ERR, "TLS error: %s: rc=%d, sslerror = %d (%s): system_error=%d (%s)\n",
                     location, rc, sslnum, reason, errno, strerror(errno));
            snmp_log(LOG_ERR, "TLS Error: %s\n",
                     ERR_reason_error_string(ERR_get_error()));
            return;

        case SSL_ERROR_ZERO_RETURN:
            reason = "SSL_ERROR_ZERO_RETURN";
            break;

        case SSL_ERROR_WANT_CONNECT:
            reason = "SSL_ERROR_WANT_CONNECT";
            break;

        case SSL_ERROR_WANT_ACCEPT:
            reason = "SSL_ERROR_WANT_ACCEPT";
            break;
            
        default:
            reason = "unknown";
        }

        snmp_log(LOG_ERR, " TLS error: %s: rc=%d, sslerror = %d (%s)\n",
                 location, rc, sslnum, reason);

        snmp_log(LOG_ERR, " TLS Error: %s\n",
                 ERR_reason_error_string(ERR_get_error()));

    }

    /* other errors */
    while ((numerical_reason =
            ERR_get_error_line_data(&file, &line, &data, &flags)) != 0) {
        snmp_log(LOG_ERR, " error: #%lu (file %s, line %d)\n",
                 numerical_reason, file, line);

        /* if we have a text translation: */
        if (data && (flags & ERR_TXT_STRING)) {
            snmp_log(LOG_ERR, "  Textual Error: %s\n", data);
            /*
             * per openssl man page: If it has been allocated by
             * OPENSSL_malloc(), *flags&ERR_TXT_MALLOCED is true.
             *
             * arggh... stupid openssl prototype for ERR_get_error_line_data
             * wants a const char **, but returns something that we might
             * need to free??
             */
            if (flags & ERR_TXT_MALLOCED)
                OPENSSL_free(NETSNMP_REMOVE_CONST(void *, data));        }
    }
    
    snmp_log(LOG_ERR, "---- End of OpenSSL Errors ----\n");
}
