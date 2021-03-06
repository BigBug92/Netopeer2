/**
 * @file ietf_system.c
 * @author Michal Vasko <mvasko@cesnet.cz>
 * @brief netopeer2-server ietf-system model subtree subscription
 *
 * Copyright (c) 2016 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include <nc_server.h>
#include <sysrepo.h>

#include "common.h"

static int
subtree_change_resolve(sr_session_ctx_t *session, sr_change_oper_t sr_oper, sr_val_t *sr_old_val,
                       sr_val_t *sr_new_val, NC_SSH_KEY_TYPE *prev_keytype)
{
    int rc = -2, ret;
    const char *xpath, *key_end, *oper_str = NULL;
    char *path = NULL, quot;
    char *list1_key = NULL, *list2_key = NULL;
    sr_val_t *keydata_val = NULL;
    NC_SSH_KEY_TYPE keytype;

    xpath = (sr_old_val ? sr_old_val->xpath : sr_new_val->xpath);

    if (strncmp(xpath, "/ietf-system:system/authentication/user[name=", 45)) {
        EINT;
        return SR_ERR_INTERNAL;
    }

    switch (sr_oper) {
    case SR_OP_CREATED:
        oper_str = "created";
        break;
    case SR_OP_DELETED:
        oper_str = "deleted";
        break;
    case SR_OP_MODIFIED:
        oper_str = "modified";
        break;
    default:
        EINT;
        return SR_ERR_INTERNAL;
    }
    VRB("Path \"%s\" %s.", xpath, oper_str);

    xpath += 45;

    quot = xpath[0];
    ++xpath;

    key_end = strchr(xpath, quot);
    if (!key_end) {
        EINT;
        return SR_ERR_INTERNAL;
    }
    list1_key = strndup(xpath, key_end - xpath);
    xpath = key_end + 1;

    if (!strcmp(xpath, "]/name")) {
        /* don't care */
        rc = 0;
        goto cleanup;
    }
    if (strncmp(xpath, "]/authorized-key[name=", 22)) {
        EINT;
        rc = SR_ERR_INTERNAL;
        goto cleanup;
    }
    xpath += 22;

    quot = xpath[0];
    ++xpath;

    key_end = strchr(xpath, quot);
    if (!key_end) {
        EINT;
        rc = SR_ERR_INTERNAL;
        goto cleanup;
    }
    list2_key = strndup(xpath, key_end - xpath);
    xpath = key_end + 1;

    if (strncmp(xpath, "]/", 2)) {
        EINT;
        rc = SR_ERR_INTERNAL;
        goto cleanup;
    }
    xpath += 2;

    if (!strcmp(xpath, "name")) {
        /* we actually don't care */
        rc = 0;
    } else if (!strcmp(xpath, "algorithm")) {
        if (sr_oper == SR_OP_DELETED) {
            /* it will all get deleted when removing "key-data" */
            rc = 0;
            goto cleanup;
        }

        if (strcmp(sr_new_val->data.string_val, "ssh-dss") && strcmp(sr_new_val->data.string_val, "ssh-rsa")
                && strncmp(sr_new_val->data.string_val, "ecdsa-sha2-", 11)) {
            ERR("Unsupported SSH key algorithm \"%s\".", sr_new_val->data.string_val);
            rc = SR_ERR_INVAL_ARG;
            goto cleanup;
        }
        if (!strcmp(sr_new_val->data.string_val, "ssh-dss")) {
            keytype = NC_SSH_KEY_DSA;
        } else if (!strcmp(sr_new_val->data.string_val, "ssh-rsa")) {
            keytype = NC_SSH_KEY_RSA;
        } else {
            keytype = NC_SSH_KEY_ECDSA;
        }

        if (sr_oper == SR_OP_CREATED) {
            /* just store it */
            *prev_keytype = keytype;
        } else {
            /* we must remove the key first, then re-add it */
            asprintf(&path, "/ietf-system:system/authentication/user[name='%s']/authorized-key[name='%s']/key-data",
                     list1_key, list2_key);
            ret = sr_get_item(session, path, &keydata_val);
            if (ret != SR_ERR_OK) {
                ERR("Failed to get \"%s\" from sysrepo.", path);
                rc = ret;
                goto cleanup;
            }

            if (nc_server_ssh_del_authkey(NULL, keydata_val->data.binary_val, 0, list1_key)) {
                EINT;
                rc = SR_ERR_INTERNAL;
                goto cleanup;
            }

            if (nc_server_ssh_add_authkey(keydata_val->data.binary_val, keytype, list1_key)) {
                EINT;
                rc = SR_ERR_INTERNAL;
                goto cleanup;
            }
        }

        rc = 0;
    } else if (!strcmp(xpath, "key-data")) {
        if (sr_oper != SR_OP_CREATED) {
            /* key-data should be unique */
            if (nc_server_ssh_del_authkey(NULL, sr_old_val->data.binary_val, 0, list1_key)) {
                EINT;
                rc = SR_ERR_INTERNAL;
                goto cleanup;
            }
        }

        if (sr_oper != SR_OP_DELETED) {
            if (!prev_keytype || nc_server_ssh_add_authkey(sr_new_val->data.binary_val, *prev_keytype, list1_key)) {
                EINT;
                rc = SR_ERR_INTERNAL;
                goto cleanup;
            }
        }

        rc = 0;
    }

cleanup:
    free(list1_key);
    free(list2_key);
    free(path);
    sr_free_val(keydata_val);
    if (rc == -2) {
        ERR("Unknown value \"%s\" change.", (sr_old_val ? sr_old_val->xpath : sr_new_val->xpath));
        rc = SR_ERR_INVAL_ARG;
    }
    return rc;
}

static int
subtree_change_cb(sr_session_ctx_t *session, const char *UNUSED(xpath), sr_notif_event_t event, void *UNUSED(private_ctx))
{
    int rc, rc2, sr_rc = SR_ERR_OK;
    sr_change_iter_t *sr_iter = NULL;
    sr_change_oper_t sr_oper;
    sr_val_t *sr_old_val = NULL, *sr_new_val = NULL;
    NC_SSH_KEY_TYPE prev_keytype = 0;

    if (event != SR_EV_APPLY) {
        EINT;
        return SR_ERR_INVAL_ARG;
    }

    rc = sr_get_changes_iter(session, "/ietf-system:system/authentication/user/authorized-key//*", &sr_iter);
    if (rc != SR_ERR_OK) {
        EINT;
        return rc;
    }
    while ((rc = sr_get_change_next(session, sr_iter, &sr_oper, &sr_old_val, &sr_new_val)) == SR_ERR_OK) {
        if ((sr_old_val && ((sr_old_val->type == SR_LIST_T) && (sr_oper != SR_OP_MOVED)))
                || (sr_new_val && ((sr_new_val->type == SR_LIST_T) && (sr_oper != SR_OP_MOVED)))
                || (sr_old_val && (sr_old_val->type == SR_CONTAINER_T)) || (sr_new_val && (sr_new_val->type == SR_CONTAINER_T))) {
            /* no semantic meaning */
            continue;
        }

        rc2 = subtree_change_resolve(session, sr_oper, sr_old_val, sr_new_val, &prev_keytype);

        sr_free_val(sr_old_val);
        sr_free_val(sr_new_val);

        if (rc2) {
            sr_rc = SR_ERR_OPERATION_FAILED;
            break;
        }
    }
    sr_free_change_iter(sr_iter);
    if ((rc != SR_ERR_OK) && (rc != SR_ERR_NOT_FOUND)) {
        EINT;
        return rc;
    }

    return sr_rc;
}

int
feature_change_ietf_system(sr_session_ctx_t *session, const char *feature_name, bool enabled)
{
    int rc, rc2 = 0;
    sr_val_iter_t *sr_iter;
    sr_val_t *sr_val;
    NC_SSH_KEY_TYPE prev_keytype = 0;

    assert(feature_name);
    if (strcmp(feature_name, "local-users")) {
        VRB("Unknown or unsupported feature \"%s\" %s, ignoring.", feature_name, (enabled ? "enabled" : "disabled"));
        return EXIT_SUCCESS;
    }

    if (enabled) {
        rc = sr_get_items_iter(np2srv.sr_sess.srs, "/ietf-system:system/authentication/user/authorized-key//*", &sr_iter);
        if (rc != SR_ERR_OK) {
            ERR("Failed to get \"%s\" values iterator from sysrepo (%s).", sr_strerror(rc));
            return EXIT_FAILURE;
        }

        while ((rc = sr_get_item_next(np2srv.sr_sess.srs, sr_iter, &sr_val)) == SR_ERR_OK) {
            if (sr_val->type == SR_LIST_T) {
                /* no semantic meaning */
                continue;
            }

            rc2 = subtree_change_resolve(session, SR_OP_CREATED, NULL, sr_val, &prev_keytype);
            sr_free_val(sr_val);
            if (rc2) {
                ERR("Failed to enable nodes depending on the \"%s\" ietf-system feature.", feature_name);
                break;
            }
        }
        sr_free_val_iter(sr_iter);
        if (rc2) {
            return EXIT_FAILURE;
        } else if ((rc != SR_ERR_OK) && (rc != SR_ERR_NOT_FOUND)) {
            ERR("Failed to get the next value from sysrepo iterator (%s).", sr_strerror(rc));
            return EXIT_FAILURE;
        }
    } else {
        nc_server_ssh_del_authkey(NULL, NULL, 0, NULL);
    }

    return EXIT_SUCCESS;
}

int
ietf_system_init(const struct lys_module *module)
{
    int rc;

    rc = sr_subtree_change_subscribe(np2srv.sr_sess.srs, "/ietf-system:system/authentication/user",
                                     subtree_change_cb, NULL, 0, SR_SUBSCR_APPLY_ONLY | SR_SUBSCR_CTX_REUSE, &np2srv.sr_subscr);
    if (SR_ERR_OK != rc) {
        ERR("Failed to subscribe to \"ietf-system\" module subtree changes (%s).", sr_strerror(rc));
        return EXIT_FAILURE;
    }

    /* applies the whole current configuration */
    if (lys_features_state(module, "local-users") == 1) {
        if (feature_change_ietf_system(np2srv.sr_sess.srs, "local-users", 1)) {
            return EXIT_FAILURE;
        }
    }

    return EXIT_SUCCESS;
}
