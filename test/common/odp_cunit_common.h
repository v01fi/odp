/* Copyright (c) 2014-2018, Linaro Limited
 * All rights reserved.
 *
 * SPDX-License-Identifier:     BSD-3-Clause
 */

/**
 * @file
 *
 * ODP test application common headers
 */

#ifndef ODP_CUNICT_COMMON_H
#define ODP_CUNICT_COMMON_H

#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <CUnit/Basic.h>
#include <odp_api.h>

#define MAX_WORKERS 32 /**< Maximum number of work threads */

typedef int (*cunit_test_check_active)(void);

typedef struct {
	const char             *name;
	CU_TestFunc             test_func;
	cunit_test_check_active check_active;
} odp_testinfo_t;

typedef struct {
	const char       *name;
	CU_InitializeFunc init_func;
	CU_CleanupFunc    term_func;
	odp_testinfo_t   *testinfo_tbl;
} odp_suiteinfo_t;

static inline int odp_cunit_test_inactive(void) { return 0; }
static inline void odp_cunit_test_missing(void) { }

/* An active test case, with the test name matching the test function name */
#define ODP_TEST_INFO(test_func) \
	{#test_func, test_func, NULL}

/* A test case that is unconditionally inactive. Its name will be registered
 * with CUnit but it won't be executed and will be reported as inactive in
 * the result summary. */
#define ODP_TEST_INFO_INACTIVE(test_func, ...) \
	{#test_func, odp_cunit_test_missing, odp_cunit_test_inactive}

#define ODP_TEST_INACTIVE 0
#define ODP_TEST_ACTIVE   1

/* A test case that may be marked as inactive at runtime based on the
 * return value of the cond_func function. A return value of ODP_TEST_INACTIVE
 * means inactive, ODP_TEST_ACTIVE means active. */
#define ODP_TEST_INFO_CONDITIONAL(test_func, cond_func) \
	{#test_func, test_func, cond_func}

#define ODP_TEST_INFO_NULL {NULL, NULL, NULL}
#define ODP_SUITE_INFO_NULL {NULL, NULL, NULL, NULL}

typedef struct {
	uint32_t foo;
	uint32_t bar;
} test_shared_data_t;

/**
 * Thread argument
 */
typedef struct {
	int testcase; /**< specifies which set of API's to exercise */
	int numthrds; /**< no of pthreads to create */
} pthrd_arg;

/* parse parameters that affect the behaviour of odp_cunit_common */
int odp_cunit_parse_options(int argc, char *argv[]);
/* register suites to be run via odp_cunit_run() */
int odp_cunit_register(odp_suiteinfo_t testsuites[]);
/* update tests previously registered via odp_cunit_register() */
int odp_cunit_update(odp_suiteinfo_t testsuites[]);
/* the function, called by module main(), to run the testsuites: */
int odp_cunit_run(void);

/** create thread for start_routine function (which returns 0 on success) */
int odp_cunit_thread_create(int func_ptr(void *), pthrd_arg *arg);
int odp_cunit_thread_exit(pthrd_arg *);

/**
 * Global tests initialization/termination.
 *
 * Initialize global resources needed by the test executable. Default
 * definition does ODP init / term (both global and local).
 * Test executables can override it by calling one of the register function
 * below.
 * The functions are called at the very beginning and very end of the test
 * execution. Passing NULL to odp_cunit_register_global_init() and/or
 * odp_cunit_register_global_term() is legal and will simply prevent the
 * default (ODP init/term) to be done.
 */
void odp_cunit_register_global_init(int (*func_init_ptr)(odp_instance_t *inst));

void odp_cunit_register_global_term(int (*func_term_ptr)(odp_instance_t inst));

int odp_cunit_ret(int val);
int odp_cunit_print_inactive(void);
int odp_cunit_set_inactive(void);

/* Check from CI_SKIP environment variable if the test case should be skipped by CI */
int odp_cunit_ci_skip(const char *test_name);

/*
 * Wrapper for CU_ASSERT_FATAL implementation to show the compiler that
 * the function does not return if the assertion fails. This reduces bogus
 * warnings generated from the code after the fatal assert.
 */
static inline void odp_cu_assert_fatal(CU_BOOL value, unsigned int line,
				       const char *condition, const char *file)
{
	CU_assertImplementation(value, line, condition, file, "", CU_TRUE);

	if (!value) {
		/* not reached */
		abort();  /* this has noreturn function attribute */
		for (;;) /* this also shows that return is not possible */
			;
	}
}

#undef CU_ASSERT_FATAL
#define CU_ASSERT_FATAL(value) \
	{ odp_cu_assert_fatal((value), __LINE__, #value, __FILE__); }

#endif /* ODP_CUNICT_COMMON_H */
