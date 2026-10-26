/**
 * @file test_validation_incr.c
 * @brief end to end test of edits over augmented modules, meant to be run on a build with and on a build without
 * -DENABLE_VALIDATE_INCREMENTAL so that both validators must give the same answers
 *
 * Module incr-b augments incr-a and carries a rule that reads incr-a, incr-c reads both, so an edit of one module
 * can be rejected by a rule of another one.
 *
 *  * Copyright (c) 2026 Vecima Networks Inc.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#define _GNU_SOURCE

#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cmocka.h>
#include <libyang/libyang.h>

#include "sysrepo.h"
#include "tests/tcommon.h"

struct state {
    sr_conn_ctx_t *conn;
    sr_session_ctx_t *sess;
};

static int
setup_f(void **state)
{
    struct state *st;
    const char *schema_paths[] = {
        TESTS_SRC_DIR "/files/incr-a.yang",
        TESTS_SRC_DIR "/files/incr-b.yang",
        TESTS_SRC_DIR "/files/incr-c.yang",
        NULL
    };

    st = calloc(1, sizeof *st);
    *state = st;

    if (sr_connect(0, &st->conn) != SR_ERR_OK) {
        return 1;
    }
    if (sr_install_modules(st->conn, schema_paths, TESTS_SRC_DIR "/files", NULL) != SR_ERR_OK) {
        return 1;
    }
    if (sr_session_start(st->conn, SR_DS_RUNNING, &st->sess) != SR_ERR_OK) {
        return 1;
    }

    return 0;
}

static int
teardown_f(void **state)
{
    struct state *st = (struct state *)*state;
    const char *module_names[] = {"incr-c", "incr-b", "incr-a", NULL};

    sr_remove_modules(st->conn, module_names, 0);
    sr_disconnect(st->conn);
    free(st);
    return 0;
}

static int
clear_data(void **state)
{
    struct state *st = (struct state *)*state;

    sr_discard_changes(st->sess);
    sr_delete_item(st->sess, "/incr-c:c-refs", 0);
    sr_delete_item(st->sess, "/incr-b:b-top", 0);
    sr_delete_item(st->sess, "/incr-a:ifs", 0);
    sr_apply_changes(st->sess, 0);

    return 0;
}

static void
set_item(struct state *st, const char *path, const char *value)
{
    assert_int_equal(SR_ERR_OK, sr_set_item_str(st->sess, path, value, NULL, 0));
}

/**
 * @brief Whether @p path exists in the running datastore, the pending edit is not looked at.
 */
static int
exists(struct state *st, const char *path)
{
    sr_session_ctx_t *sess;
    sr_val_t *val = NULL;
    int ret;

    assert_int_equal(SR_ERR_OK, sr_session_start(st->conn, SR_DS_RUNNING, &sess));
    ret = sr_get_item(sess, path, 0, &val);
    if (ret == SR_ERR_OK) {
        sr_free_val(val);
    } else {
        assert_int_equal(SR_ERR_NOT_FOUND, ret);
    }
    sr_session_stop(sess);

    return ret == SR_ERR_OK;
}

/**
 * @brief Valid starting data: e1 is an 'eth' with an explicit b-cfg and one address, x1 is a plain 'eth' read by the
 * rule of b-chk, and c-ref r1 reads the speed and the address of e1.
 */
static void
load_base(struct state *st)
{
    set_item(st, "/incr-a:ifs/if[name='e1']/type", "incr-a:eth");
    set_item(st, "/incr-a:ifs/if[name='e1']/incr-b:b-cfg/speed", "1000");
    set_item(st, "/incr-a:ifs/if[name='e1']/incr-b:b-cfg/mtu", "1500");
    set_item(st, "/incr-a:ifs/if[name='e1']/incr-b:b-addr[id='1']/ip", "10.0.0.1");
    set_item(st, "/incr-a:ifs/if[name='x1']/type", "incr-a:eth");
    set_item(st, "/incr-b:b-top/b-chk[name='k2']/ifname", "x1");
    set_item(st, "/incr-b:b-top/b-chk[name='k2']/need", "y");
    set_item(st, "/incr-c:c-refs/c-ref[name='r1']/ifname", "e1");
    set_item(st, "/incr-c:c-refs/c-ref[name='r1']/addr-id", "1");
    set_item(st, "/incr-c:c-refs/c-ref[name='r1']/fast", "y");
    assert_int_equal(SR_ERR_OK, sr_apply_changes(st->sess, 0));
}

/**
 * @brief Apply the pending edit and require it to be rejected without touching the datastore.
 */
static void
apply_rejected(struct state *st)
{
    assert_int_equal(SR_ERR_VALIDATION_FAILED, sr_apply_changes(st->sess, 0));
    assert_int_equal(SR_ERR_OK, sr_discard_changes(st->sess));
}

static void
test_add(void **state)
{
    struct state *st = (struct state *)*state;

    load_base(st);

    /* a valid augmented list entry and a valid new target instance */
    set_item(st, "/incr-a:ifs/if[name='e1']/incr-b:b-addr[id='2']/ip", "10.0.0.2");
    set_item(st, "/incr-a:ifs/if[name='e2']/type", "incr-a:eth");
    assert_int_equal(SR_ERR_OK, sr_apply_changes(st->sess, 0));
    assert_true(exists(st, "/incr-a:ifs/if[name='e1']/incr-b:b-addr[id='2']/ip"));
    assert_true(exists(st, "/incr-a:ifs/if[name='e2']/type"));

    /* a duplicate ip in the same list breaks the unique */
    set_item(st, "/incr-a:ifs/if[name='e1']/incr-b:b-addr[id='3']/ip", "10.0.0.1");
    apply_rejected(st);
    assert_false(exists(st, "/incr-a:ifs/if[name='e1']/incr-b:b-addr[id='3']/ip"));

    /* the same ip under another interface does not */
    set_item(st, "/incr-a:ifs/if[name='x1']/incr-b:b-addr[id='1']/ip", "10.0.0.1");
    assert_int_equal(SR_ERR_OK, sr_apply_changes(st->sess, 0));

    /* the must of the augment reads the sibling speed, from both sides, and the must of c-ref reads it as well */
    set_item(st, "/incr-a:ifs/if[name='e1']/incr-b:b-cfg/mtu", "500");
    apply_rejected(st);
    set_item(st, "/incr-a:ifs/if[name='e1']/incr-b:b-cfg/speed", "2000");
    apply_rejected(st);
    set_item(st, "/incr-a:ifs/if[name='e1']/incr-b:b-cfg/speed", "900");
    apply_rejected(st);
    set_item(st, "/incr-a:ifs/if[name='e1']/incr-b:b-cfg/speed", "1200");
    assert_int_equal(SR_ERR_OK, sr_apply_changes(st->sess, 0));
    assert_true(exists(st, "/incr-a:ifs/if[name='e1']/incr-b:b-cfg/speed"));
}

static void
test_delete(void **state)
{
    struct state *st = (struct state *)*state;

    load_base(st);

    /* the referenced address and the referenced interface cannot go */
    assert_int_equal(SR_ERR_OK, sr_delete_item(st->sess, "/incr-a:ifs/if[name='e1']/incr-b:b-addr[id='1']", 0));
    apply_rejected(st);
    assert_true(exists(st, "/incr-a:ifs/if[name='e1']/incr-b:b-addr[id='1']/ip"));

    assert_int_equal(SR_ERR_OK, sr_delete_item(st->sess, "/incr-a:ifs/if[name='e1']", 0));
    apply_rejected(st);
    assert_true(exists(st, "/incr-a:ifs/if[name='e1']/type"));

    /* the rule of b-chk only needs x1 to exist and to be an 'eth' */
    assert_int_equal(SR_ERR_OK, sr_delete_item(st->sess, "/incr-a:ifs/if[name='x1']", 0));
    apply_rejected(st);
    assert_true(exists(st, "/incr-a:ifs/if[name='x1']/type"));

    /* the explicit container goes and a default one comes back, which is too slow for c-ref/fast */
    assert_int_equal(SR_ERR_OK, sr_delete_item(st->sess, "/incr-a:ifs/if[name='e1']/incr-b:b-cfg", 0));
    apply_rejected(st);
    assert_true(exists(st, "/incr-a:ifs/if[name='e1']/incr-b:b-cfg/speed"));

    /* with the referrer deleted in the same edit nothing dangles any more */
    assert_int_equal(SR_ERR_OK, sr_delete_item(st->sess, "/incr-a:ifs/if[name='x1']", 0));
    assert_int_equal(SR_ERR_OK, sr_delete_item(st->sess, "/incr-b:b-top/b-chk[name='k2']", 0));
    assert_int_equal(SR_ERR_OK, sr_apply_changes(st->sess, 0));
    assert_false(exists(st, "/incr-a:ifs/if[name='x1']/type"));
    assert_false(exists(st, "/incr-b:b-top/b-chk[name='k2']/ifname"));
    assert_true(exists(st, "/incr-a:ifs/if[name='e1']/type"));
    assert_true(exists(st, "/incr-a:ifs/if[name='e1']/incr-b:b-addr[id='1']/ip"));
    assert_true(exists(st, "/incr-c:c-refs/c-ref[name='r1']/fast"));
}

static void
test_cross_module(void **state)
{
    struct state *st = (struct state *)*state;

    load_base(st);

    /* only incr-a is edited, the rule that breaks is in incr-b */
    set_item(st, "/incr-a:ifs/if[name='x1']/type", "incr-a:ani");
    apply_rejected(st);
    assert_true(exists(st, "/incr-b:b-top/b-chk[name='k2']/need"));

    /* same flip once the rule of b-chk is gone: the b-cfg that is only a default simply disappears */
    assert_int_equal(SR_ERR_OK, sr_delete_item(st->sess, "/incr-b:b-top/b-chk[name='k2']", 0));
    set_item(st, "/incr-a:ifs/if[name='x1']/type", "incr-a:ani");
    assert_int_equal(SR_ERR_OK, sr_apply_changes(st->sess, 0));

    /* only incr-b and incr-c are edited, they read incr-a */
    set_item(st, "/incr-c:c-refs/c-ref[name='r2']/ifname", "x1");
    assert_int_equal(SR_ERR_OK, sr_apply_changes(st->sess, 0));
    set_item(st, "/incr-c:c-refs/c-ref[name='r1']/addr-id", "9");
    apply_rejected(st);
}

/**
 * @brief Flipping the type removes the explicit b-cfg through its when, the edit does not mention it, and
 * c-ref/fast reads its speed. Full validation rejects the edit.
 *
 * The same scenario at libyang level (test_aug_when_cascade in test_validation_incr.c there) is accepted by the
 * incremental validator, here it is rejected in both modes, so this stays as a guard for the sysrepo flow.
 */
static void
test_when_cascade(void **state)
{
    struct state *st = (struct state *)*state;

    load_base(st);
    assert_int_equal(SR_ERR_OK, sr_delete_item(st->sess, "/incr-b:b-top/b-chk[name='k2']", 0));
    assert_int_equal(SR_ERR_OK, sr_apply_changes(st->sess, 0));

    set_item(st, "/incr-a:ifs/if[name='e1']/type", "incr-a:ani");
    apply_rejected(st);
    assert_true(exists(st, "/incr-a:ifs/if[name='e1']/incr-b:b-cfg/speed"));
}

/**
 * @brief The same cascade caught by a leafref: x1 points at the speed of e1, the flip of e1 removes it.
 *
 * Leafrefs are resolved in the first validation pass, before sysrepo merges the validation diff, so only the
 * fallback of the incremental validator on deletions made by the validation itself catches this.
 */
static void
test_when_cascade_leafref(void **state)
{
    struct state *st = (struct state *)*state;

    load_base(st);
    assert_int_equal(SR_ERR_OK, sr_delete_item(st->sess, "/incr-c:c-refs/c-ref[name='r1']", 0));
    set_item(st, "/incr-a:ifs/if[name='x1']/incr-b:peer-speed", "1000");
    assert_int_equal(SR_ERR_OK, sr_apply_changes(st->sess, 0));

    set_item(st, "/incr-a:ifs/if[name='e1']/type", "incr-a:ani");
    apply_rejected(st);
    assert_true(exists(st, "/incr-a:ifs/if[name='e1']/incr-b:b-cfg/speed"));
    assert_true(exists(st, "/incr-a:ifs/if[name='x1']/incr-b:peer-speed"));
}

/**
 * @brief Creating the m2 case replaces the default m1 case, and c-ref/m1need reads m1-val.
 *
 * On x1 the whole b-cfg only exists as a default. The libyang-level test (test_aug_dflt_case_in_dflt) shows the
 * incremental validator accepting this when the edit diff calls that b-cfg created; the sysrepo edit diff matches the
 * existing default container instead, so here both modes reject. On e1 b-cfg is explicit.
 */
static void
test_dflt_case_replace(void **state)
{
    struct state *st = (struct state *)*state;

    load_base(st);
    set_item(st, "/incr-c:c-refs/c-ref[name='r4']/ifname", "x1");
    set_item(st, "/incr-c:c-refs/c-ref[name='r4']/m1need", "y");
    set_item(st, "/incr-c:c-refs/c-ref[name='r5']/ifname", "e1");
    set_item(st, "/incr-c:c-refs/c-ref[name='r5']/m1need", "y");
    assert_int_equal(SR_ERR_OK, sr_apply_changes(st->sess, 0));

    set_item(st, "/incr-a:ifs/if[name='e1']/incr-b:b-cfg/m2-val", "5");
    apply_rejected(st);
    assert_false(exists(st, "/incr-a:ifs/if[name='e1']/incr-b:b-cfg/m2-val"));

    set_item(st, "/incr-a:ifs/if[name='x1']/incr-b:b-cfg/m2-val", "5");
    apply_rejected(st);
    assert_false(exists(st, "/incr-a:ifs/if[name='x1']/incr-b:b-cfg/m2-val"));
}

int
main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_teardown(test_add, clear_data),
        cmocka_unit_test_teardown(test_delete, clear_data),
        cmocka_unit_test_teardown(test_cross_module, clear_data),
        cmocka_unit_test_teardown(test_when_cascade, clear_data),
        cmocka_unit_test_teardown(test_when_cascade_leafref, clear_data),
        cmocka_unit_test_teardown(test_dflt_case_replace, clear_data),
    };

    setenv("CMOCKA_TEST_ABORT", "1", 1);
    test_init();
    return cmocka_run_group_tests(tests, setup_f, teardown_f);
}
