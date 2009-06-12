/*
 * $Id$
 *
 * Copyright (c) 2009 Nominet UK.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

#include <libhsm.h>
#include <libhsmdns.h>

#define PTHREAD_THREADS_MAX 2048

extern char *optarg;
char *progname = NULL;

typedef struct {
	unsigned int id;
	hsm_ctx_t *ctx;
	hsm_key_t *key;
	unsigned int iterations;
} sign_arg_t;

void
usage ()
{
	fprintf(stderr,
		"usage: %s "
		"[-f config] [-i iterations] [-s keysize] [-t threads]\n",
		progname);
}

void *
sign (void *arg)
{
	hsm_ctx_t *ctx = NULL;
	hsm_key_t *key = NULL;
	
	size_t i;
	unsigned int iterations = 0;

	ldns_rr_list *rrset;
	ldns_rr *rr, *sig, *dnskey_rr;
	ldns_status status;
	hsm_sign_params_t *sign_params;

	sign_arg_t *sign_arg = arg;

	ctx = sign_arg->ctx;
	key = sign_arg->key;
	iterations = sign_arg->iterations;

	fprintf(stderr, "Signer thread #%d started...\n", sign_arg->id);

	/* Prepare dummy RRset for signing */
	rrset = ldns_rr_list_new();
	status = ldns_rr_new_frm_str(&rr, "regress.opendnssec.se. IN A 123.123.123.123", 0, NULL, NULL);
	if (status == LDNS_STATUS_OK) ldns_rr_list_push_rr(rrset, rr);
	status = ldns_rr_new_frm_str(&rr, "regress.opendnssec.se. IN A 124.124.124.124", 0, NULL, NULL);
	if (status == LDNS_STATUS_OK) ldns_rr_list_push_rr(rrset, rr);
	sign_params = hsm_sign_params_new();
	sign_params->algorithm = LDNS_RSASHA1;
	sign_params->owner = ldns_rdf_new_frm_str(LDNS_RDF_TYPE_DNAME, "opendnssec.se.");
	dnskey_rr = hsm_get_dnskey(ctx, key, sign_params);
	sign_params->keytag = ldns_calc_keytag(dnskey_rr);

	/* Do some signing */
	for (i=0; i<iterations; i++) {
		sig = hsm_sign_rrset(ctx, rrset, key, sign_params);	
		if (! sig) {
			fprintf(stderr, "hsm_sign_rrset() returned error\n");
			break;
		}				
		ldns_rr_free(sig);
	}
	
	/* Clean up */
	ldns_rr_list_deep_free(rrset);
	hsm_sign_params_free(sign_params);
	ldns_rr_free(dnskey_rr);	
	hsm_destroy_context(ctx);
	
	fprintf(stderr, "Signer thread #%d done.\n", sign_arg->id);
	
	pthread_exit(NULL);
}


int
main (int argc, char *argv[])
{
	int result;

	hsm_ctx_t *ctx = NULL;
	hsm_key_t *key = NULL;
	unsigned int keysize = 1024;
	unsigned int iterations = 1;
	unsigned int threads = 1;

	static struct timeval start,end;

	char *config = NULL;
	const char *repository = "regress";
	
	sign_arg_t sign_arg_array[PTHREAD_THREADS_MAX];
	
	pthread_t      thread_array[PTHREAD_THREADS_MAX];
    pthread_attr_t thread_attr;
    void          *thread_status;

	int ch;
	unsigned int n;

	progname = argv[0];

	while ((ch = getopt(argc, argv, "f:i:s:t:")) != -1) {
		switch (ch) {
		case 'f':
			config = strdup(optarg);
			break;
		case 'i':
			iterations = atoi(optarg);
			break;
		case 's':
			keysize = atoi(optarg);
			break;
		case 't':
			threads = atoi(optarg);
			break;
		default:
			usage();
			exit(1);
		}
	}

	if (!config) {
		usage();
		exit(1);
	}

	/* Open HSM library */
	fprintf(stderr, "Opening HSM Library...\n");
	result = hsm_open(config, hsm_prompt_pin, NULL);	
	if (result) {
		fprintf(stderr, "hsm_open() returned %d\n", result);
		exit(-1);
	}

	/* Create HSM context */
	ctx = hsm_create_context();
	if (! ctx) {
		fprintf(stderr, "hsm_create_context() returned error\n");
		exit(-1);
	}

	/* Generate a temporary key */
	fprintf(stderr, "Generating temporary key...\n");
	key = hsm_generate_rsa_key(ctx, repository, keysize);
	if (key) {
		char *id = hsm_get_key_id(ctx, key);
		printf("Temporary key created: %s\n", id);
		free(id);			
	} else {
		fprintf(stderr, "hsm_generate_rsa_key() returned %d\n", result);
		exit(-1);
	}
	
	/* Prepare threads */
	pthread_attr_init(&thread_attr);
	pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_JOINABLE);
	
	for (n=0; n<threads; n++) {
		sign_arg_array[n].id = n;
		sign_arg_array[n].ctx = hsm_create_context();
		if (! sign_arg_array[n].ctx) {
			fprintf(stderr, "hsm_create_context() returned error\n");
			exit(-1);
		}
		sign_arg_array[n].key = key;
		sign_arg_array[n].iterations = iterations;
	}

	fprintf(stderr, "Signing %d RRsets using %d %s...\n",
		iterations, threads, (threads > 1 ? "threads" : "thread"));
	gettimeofday(&start, NULL);
	
	/* Create threads for signing */
	for (n=0; n<threads; n++) {
		result = pthread_create(&thread_array[n], &thread_attr,
			sign, (void *) &sign_arg_array[n]);
		if (result) {
			fprintf(stderr, "pthread_create() returned %d\n", result);
			exit(EXIT_FAILURE);
		}
	}

	/* Wait for threads to finish */
	for (n=0; n<threads; n++) {
		result = pthread_join(thread_array[n], &thread_status);
		if (result) {
			fprintf(stderr, "pthread_join() returned %d\n", result);
			exit(EXIT_FAILURE);			
		}
	}

	gettimeofday(&end, NULL);
	fprintf(stderr, "Signing done.\n");

	/* Report results */
	end.tv_sec -= start.tv_sec;
	end.tv_usec-= start.tv_usec;
	double elapsed =(double)(end.tv_sec)+(double)(end.tv_usec)*.000001;
	double speed = iterations / elapsed * threads;
	printf("%d signatures using %d threads, %.2f sig/s (RSA %d bits)\n",
		iterations, threads, speed, keysize);
	
	/* Delete temporary key */
	fprintf(stderr, "Deleting temporary key...\n");
	result = hsm_remove_key(ctx, key);
	if (result) {
		fprintf(stderr, "hsm_remove_key() returned %d\n", result);
		exit(-1);
	}

	/* Clean up */
	hsm_destroy_context(ctx);
	(void) hsm_close();
	if (config) free(config);
	
	return 0;
}
