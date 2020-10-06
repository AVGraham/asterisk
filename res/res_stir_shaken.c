/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2020, Sangoma Technologies Corporation
 *
 * Kevin Harwell <kharwell@digium.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*** MODULEINFO
	<depend>crypto</depend>
	<depend>curl</depend>
	<depend>res_curl</depend>
	<support_level>core</support_level>
 ***/

#include "asterisk.h"

#include "asterisk/module.h"
#include "asterisk/sorcery.h"
#include "asterisk/time.h"
#include "asterisk/json.h"
#include "asterisk/astdb.h"
#include "asterisk/paths.h"
#include "asterisk/conversions.h"

#include "asterisk/res_stir_shaken.h"
#include "res_stir_shaken/stir_shaken.h"
#include "res_stir_shaken/general.h"
#include "res_stir_shaken/store.h"
#include "res_stir_shaken/certificate.h"
#include "res_stir_shaken/curl.h"

/*** DOCUMENTATION
	<configInfo name="res_stir_shaken" language="en_US">
		<synopsis>STIR/SHAKEN module for Asterisk</synopsis>
		<configFile name="stir_shaken.conf">
			<configObject name="general">
				<synopsis>STIR/SHAKEN general options</synopsis>
				<configOption name="type">
					<synopsis>Must be of type 'general'.</synopsis>
				</configOption>
				<configOption name="ca_file" default="">
					<synopsis>File path to the certificate authority certificate</synopsis>
				</configOption>
				<configOption name="ca_path" default="">
					<synopsis>File path to a chain of trust</synopsis>
				</configOption>
				<configOption name="cache_max_size" default="1000">
					<synopsis>Maximum size to use for caching public keys</synopsis>
				</configOption>
				<configOption name="curl_timeout" default="2">
					<synopsis>Maximum time to wait to CURL certificates</synopsis>
				</configOption>
			</configObject>
			<configObject name="store">
				<synopsis>STIR/SHAKEN certificate store options</synopsis>
				<configOption name="type">
					<synopsis>Must be of type 'store'.</synopsis>
				</configOption>
				<configOption name="path" default="">
					<synopsis>Path to a directory containing certificates</synopsis>
				</configOption>
				<configOption name="public_key_url" default="">
					<synopsis>URL to the public key(s)</synopsis>
					<description><para>
					 Must be a valid http, or https, URL. The URL must also contain the ${CERTIFICATE} variable, which is used for public key name substitution.
					 For example: http://mycompany.com/${CERTIFICATE}.pub
					</para></description>
				</configOption>
			</configObject>
			<configObject name="certificate">
				<synopsis>STIR/SHAKEN certificate options</synopsis>
				<configOption name="type">
					<synopsis>Must be of type 'certificate'.</synopsis>
				</configOption>
				<configOption name="path" default="">
					<synopsis>File path to a certificate</synopsis>
				</configOption>
				<configOption name="public_key_url" default="">
					<synopsis>URL to the public key</synopsis>
					<description><para>
					 Must be a valid http, or https, URL.
					</para></description>
				</configOption>
			</configObject>
		</configFile>
	</configInfo>
 ***/

#define STIR_SHAKEN_ENCRYPTION_ALGORITHM "ES256"
#define STIR_SHAKEN_PPT "shaken"
#define STIR_SHAKEN_TYPE "passport"

static struct ast_sorcery *stir_shaken_sorcery;

/* Used for AstDB entries */
#define AST_DB_FAMILY "STIR_SHAKEN"

/* The directory name to store keys in. Appended to ast_config_DATA_DIR */
#define STIR_SHAKEN_DIR_NAME "stir_shaken"

/* The maximum length for path storage */
#define MAX_PATH_LEN 256

struct ast_stir_shaken_payload {
	/*! The JWT header */
	struct ast_json *header;
	/*! The JWT payload */
	struct ast_json *payload;
	/*! Signature for the payload */
	unsigned char *signature;
	/*! The algorithm used */
	char *algorithm;
	/*! THe URL to the public key for the certificate */
	char *public_key_url;
};

struct ast_sorcery *ast_stir_shaken_sorcery(void)
{
	return stir_shaken_sorcery;
}

void ast_stir_shaken_payload_free(struct ast_stir_shaken_payload *payload)
{
	if (!payload) {
		return;
	}

	ast_json_unref(payload->header);
	ast_json_unref(payload->payload);
	ast_free(payload->algorithm);
	ast_free(payload->public_key_url);
	ast_free(payload->signature);

	ast_free(payload);
}

/*!
 * \brief Sets the expiration for the public key based on the provided fields.
 * If Cache-Control is present, use it. Otherwise, use Expires.
 *
 * \param hash The hash for the public key URL
 * \param data The CURL callback data containing expiration data
 */
static void set_public_key_expiration(const char *public_key_url, const struct curl_cb_data *data)
{
	char time_buf[32];
	char *value;
	struct timeval actual_expires = ast_tvnow();
	char hash[41];

	ast_sha1_hash(hash, public_key_url);

	value = curl_cb_data_get_cache_control(data);
	if (!ast_strlen_zero(value)) {
		char *str_max_age;

		str_max_age = strstr(value, "s-maxage");
		if (!str_max_age) {
			str_max_age = strstr(value, "max-age");
		}

		if (str_max_age) {
			unsigned int max_age;
			char *equal = strchr(str_max_age, '=');
			if (equal && !ast_str_to_uint(equal + 1, &max_age)) {
				actual_expires.tv_sec += max_age;
			}
		}
	} else {
		value = curl_cb_data_get_expires(data);
		if (!ast_strlen_zero(value)) {
			struct tm expires_time;

			strptime(value, "%a, %d %b %Y %T %z", &expires_time);
			expires_time.tm_isdst = -1;
			actual_expires.tv_sec = mktime(&expires_time);
		}
	}

	snprintf(time_buf, sizeof(time_buf), "%30lu", actual_expires.tv_sec);

	ast_db_put(hash, "expiration", time_buf);
}

/*!
 * \brief Check to see if the public key is expired
 *
 * \param public_key_url The public key URL
 *
 * \retval 1 if expired
 * \retval 0 if not expired
 */
static int public_key_is_expired(const char *public_key_url)
{
	struct timeval current_time = ast_tvnow();
	struct timeval expires = { .tv_sec = 0, .tv_usec = 0 };
	char expiration[32];
	char hash[41];

	ast_sha1_hash(hash, public_key_url);
	ast_db_get(hash, "expiration", expiration, sizeof(expiration));

	if (ast_strlen_zero(expiration)) {
		return 1;
	}

	if (ast_str_to_ulong(expiration, (unsigned long *)&expires.tv_sec)) {
		return 1;
	}

	return ast_tvcmp(current_time, expires) == -1 ? 0 : 1;
}

/*!
 * \brief Returns the path to the downloaded file for the provided URL
 *
 * \param public_key_url The public key URL
 *
 * \retval Empty string if not present in AstDB
 * \retval The file path if present in AstDB
 */
static char *get_path_to_public_key(const char *public_key_url)
{
	char hash[41];
	char file_path[MAX_PATH_LEN];

	ast_sha1_hash(hash, public_key_url);

	ast_db_get(hash, "path", file_path, sizeof(file_path));

	if (ast_strlen_zero(file_path)) {
		file_path[0] = '\0';
	}

	return ast_strdup(file_path);
}

/*!
 * \brief Add the public key details and file path to AstDB
 *
 * \param public_key_url The public key URL
 * \param filepath The path to the file
 */
static void add_public_key_to_astdb(const char *public_key_url, const char *filepath)
{
	char hash[41];

	ast_sha1_hash(hash, public_key_url);

	ast_db_put(AST_DB_FAMILY, public_key_url, hash);
	ast_db_put(hash, "path", filepath);
}

/*!
 * \brief Remove the public key details and associated information from AstDB
 *
 * \param public_key_url The public key URL
 */
static void remove_public_key_from_astdb(const char *public_key_url)
{
	char hash[41];
	char filepath[MAX_PATH_LEN];

	ast_sha1_hash(hash, public_key_url);

	/* Remove this public key from storage */
	ast_db_get(hash, "path", filepath, sizeof(filepath));

	/* Remove the actual file from the system */
	remove(filepath);

	ast_db_del(AST_DB_FAMILY, public_key_url);
	ast_db_deltree(hash, NULL);
}

/*!
 * \brief Verifies the signature using a public key
 *
 * \param msg The payload
 * \param signature The signature to verify
 * \param public_key The public key used for verification
 *
 * \retval -1 on failure
 * \retval 0 on success
 */
static int stir_shaken_verify_signature(const char *msg, const char *signature, EVP_PKEY *public_key)
{
	EVP_MD_CTX *mdctx = NULL;
	int ret = 0;
	unsigned char *decoded_signature;
	size_t signature_length, decoded_signature_length, padding = 0;

	mdctx = EVP_MD_CTX_create();
	if (!mdctx) {
		ast_log(LOG_ERROR, "Failed to create Message Digest Context\n");
		return -1;
	}

	ret = EVP_DigestVerifyInit(mdctx, NULL, EVP_sha256(), NULL, public_key);
	if (ret != 1) {
		ast_log(LOG_ERROR, "Failed to initialize Message Digest Context\n");
		EVP_MD_CTX_destroy(mdctx);
		return -1;
	}

	ret = EVP_DigestVerifyUpdate(mdctx, (unsigned char *)msg, strlen(msg));
	if (ret != 1) {
		ast_log(LOG_ERROR, "Failed to update Message Digest Context\n");
		EVP_MD_CTX_destroy(mdctx);
		return -1;
	}

	/* We need to decode the signature from base64 to bytes. Make sure we have
	 * at least enough characters for this check */
	signature_length = strlen(signature);
	if (signature_length > 2 && signature[signature_length - 1] == '=') {
		padding++;
		if (signature[signature_length - 2] == '=') {
			padding++;
		}
	}

	decoded_signature_length = (signature_length / 4 * 3) - padding;
	decoded_signature = ast_calloc(1, decoded_signature_length);
	ast_base64decode(decoded_signature, signature, decoded_signature_length);

	ret = EVP_DigestVerifyFinal(mdctx, decoded_signature, decoded_signature_length);
	if (ret != 1) {
		ast_log(LOG_ERROR, "Failed final phase of signature verification\n");
		EVP_MD_CTX_destroy(mdctx);
		ast_free(decoded_signature);
		return -1;
	}

	EVP_MD_CTX_destroy(mdctx);
	ast_free(decoded_signature);

	return 0;
}

/*!
 * \brief CURL the file located at public_key_url to the specified path
 *
 * \param public_key_url The public key URL
 * \param path The path to download the file to
 *
 * \retval -1 on failure
 * \retval 0 on success
 */
static int run_curl(const char *public_key_url, const char *path)
{
	struct curl_cb_data *data;

	data = curl_cb_data_create();
	if (!data) {
		ast_log(LOG_ERROR, "Failed to create CURL callback data\n");
		return -1;
	}

	if (curl_public_key(public_key_url, path, data)) {
		ast_log(LOG_ERROR, "Could not retrieve public key for '%s'\n", public_key_url);
		curl_cb_data_free(data);
		return -1;
	}

	set_public_key_expiration(public_key_url, data);
	curl_cb_data_free(data);

	return 0;
}

/*!
 * \brief Downloads the public key from public_key_url. If curl is non-zero, that signals
 * CURL has already been run, and we should bail here. The entry is added to AstDB as well.
 *
 * \param public_key_url The public key URL
 * \param path The path to download the file to
 * \param curl Flag signaling if we have run CURL or not
 *
 * \retval -1 on failure
 * \retval 0 on success
 */
static int curl_and_check_expiration(const char *public_key_url, const char *path, int *curl)
{
	if (curl) {
		ast_log(LOG_ERROR, "Already downloaded public key '%s'\n", path);
		return -1;
	}

	if (run_curl(public_key_url, path)) {
		return -1;
	}

	if (public_key_is_expired(public_key_url)) {
		ast_log(LOG_ERROR, "Newly downloaded public key '%s' is expired\n", path);
		return -1;
	}

	*curl = 1;
	add_public_key_to_astdb(public_key_url, path);

	return 0;
}

struct ast_stir_shaken_payload *ast_stir_shaken_verify(const char *header, const char *payload, const char *signature,
	const char *algorithm, const char *public_key_url)
{
	struct ast_stir_shaken_payload *ret_payload;
	EVP_PKEY *public_key;
	char *filename;
	int curl = 0;
	struct ast_json_error err;
	RAII_VAR(char *, file_path, NULL, ast_free);

	if (ast_strlen_zero(header)) {
		ast_log(LOG_ERROR, "'header' is required for STIR/SHAKEN verification\n");
		return NULL;
	}

	if (ast_strlen_zero(payload)) {
		ast_log(LOG_ERROR, "'payload' is required for STIR/SHAKEN verification\n");
		return NULL;
	}

	if (ast_strlen_zero(signature)) {
		ast_log(LOG_ERROR, "'signature' is required for STIR/SHAKEN verification\n");
		return NULL;
	}

	if (ast_strlen_zero(algorithm)) {
		ast_log(LOG_ERROR, "'algorithm' is required for STIR/SHAKEN verification\n");
		return NULL;
	}

	if (ast_strlen_zero(public_key_url)) {
		ast_log(LOG_ERROR, "'public_key_url' is required for STIR/SHAKEN verification\n");
		return NULL;
	}

	/* Check to see if we have already downloaded this public key. The reason we
	 * store the file path is because:
	 *
	 * 1. If, for some reason, the default directory changes, we still know where
	 * to look for the files we already have.
	 *
	 * 2. In the future, if we want to add a way to store the keys in multiple
	 * {configurable) directories, we already have the storage mechanism in place.
	 * The only thing that would be left to do is pull from the configuration.
	 */
	file_path = get_path_to_public_key(public_key_url);

	/* If we don't have an entry in AstDB, CURL from the provided URL */
	if (ast_strlen_zero(file_path)) {

		size_t file_path_size;

		/* Remove this entry from the database, since we will be
		 * downloading a new file anyways.
		 */
		remove_public_key_from_astdb(public_key_url);

		/* Go ahead and free file_path, in case anything was allocated above */
		ast_free(file_path);

		/* Set up the default path */
		filename = basename(public_key_url);
		file_path_size = strlen(ast_config_AST_DATA_DIR) + 3 + strlen(STIR_SHAKEN_DIR_NAME) + strlen(filename) + 1;
		file_path = ast_calloc(1, file_path_size);
		snprintf(file_path, sizeof(*file_path), "%s/keys/%s/%s", ast_config_AST_DATA_DIR, STIR_SHAKEN_DIR_NAME, filename);

		/* Download to the default path */
		if (run_curl(public_key_url, file_path)) {
			return NULL;
		}

		/* Signal that we have already downloaded a new file, no reason to do it again */
		curl = 1;

		/* We should have a successful download at this point, so
		 * add an entry to the database.
		 */
		add_public_key_to_astdb(public_key_url, file_path);
	}

	/* Check to see if the key we downloaded (or already had) is expired */
	if (public_key_is_expired(public_key_url)) {

		ast_debug(3, "Public key '%s' is expired\n", public_key_url);

		remove_public_key_from_astdb(public_key_url);

		/* If this fails, then there's nothing we can do */
		if (curl_and_check_expiration(public_key_url, file_path, &curl)) {
			return NULL;
		}
	}

	/* First attempt to read the key. If it fails, try downloading the file,
	 * unless we already did. Check for expiration again */
	public_key = stir_shaken_read_key(file_path, 0);
	if (!public_key) {

		ast_debug(3, "Failed first read of public key file '%s'\n", file_path);

		remove_public_key_from_astdb(public_key_url);

		if (curl_and_check_expiration(public_key_url, file_path, &curl)) {
			return NULL;
		}

		public_key = stir_shaken_read_key(file_path, 0);
		if (!public_key) {
			ast_log(LOG_ERROR, "Failed to read public key from '%s'\n", file_path);
			remove_public_key_from_astdb(public_key_url);
			return NULL;
		}
	}

	if (stir_shaken_verify_signature(payload, signature, public_key)) {
		ast_log(LOG_ERROR, "Failed to verify signature\n");
		EVP_PKEY_free(public_key);
		return NULL;
	}

	/* We don't need the public key anymore */
	EVP_PKEY_free(public_key);

	ret_payload = ast_calloc(1, sizeof(*ret_payload));
	if (!ret_payload) {
		ast_log(LOG_ERROR, "Failed to allocate STIR/SHAKEN payload\n");
		return NULL;
	}

	ret_payload->header = ast_json_load_string(header, &err);
	if (!ret_payload->header) {
		ast_log(LOG_ERROR, "Failed to create JSON from header\n");
		ast_stir_shaken_payload_free(ret_payload);
		return NULL;
	}

	ret_payload->payload = ast_json_load_string(payload, &err);
	if (!ret_payload->payload) {
		ast_log(LOG_ERROR, "Failed to create JSON from payload\n");
		ast_stir_shaken_payload_free(ret_payload);
		return NULL;
	}

	ret_payload->signature = (unsigned char *)ast_strdup(signature);
	ret_payload->algorithm = ast_strdup(algorithm);
	ret_payload->public_key_url = ast_strdup(public_key_url);

	return ret_payload;
}

/*!
 * \brief Verifies the necessary contents are in the JSON and returns a
 * ast_stir_shaken_payload with the extracted values.
 *
 * \param json The JSON to verify
 *
 * \return ast_stir_shaken_payload on success
 * \return NULL on failure
 */
static struct ast_stir_shaken_payload *stir_shaken_verify_json(struct ast_json *json)
{
	struct ast_stir_shaken_payload *payload;
	struct ast_json *obj;
	const char *val;

	payload = ast_calloc(1, sizeof(*payload));
	if (!payload) {
		ast_log(LOG_ERROR, "Failed to allocate STIR/SHAKEN payload\n");
		goto cleanup;
	}

	/* Look through the header first */
	obj = ast_json_object_get(json, "header");
	if (!obj) {
		ast_log(LOG_ERROR, "STIR/SHAKEN JWT did not have the required field 'header'\n");
		goto cleanup;
	}

	payload->header = ast_json_deep_copy(obj);
	if (!payload->header) {
		ast_log(LOG_ERROR, "STIR_SHAKEN payload failed to copy 'header'\n");
		goto cleanup;
	}

	/* Check the ppt value for "shaken" */
	val = ast_json_string_get(ast_json_object_get(obj, "ppt"));
	if (ast_strlen_zero(val)) {
		ast_log(LOG_ERROR, "STIR/SHAKEN JWT did not have the required field 'ppt'\n");
		goto cleanup;
	}
	if (strcmp(val, STIR_SHAKEN_PPT)) {
		ast_log(LOG_ERROR, "STIR/SHAKEN JWT field 'ppt' did not have "
			"required value '%s' (was '%s')\n", STIR_SHAKEN_PPT, val);
		goto cleanup;
	}

	/* Check the typ value for "passport" */
	val = ast_json_string_get(ast_json_object_get(obj, "typ"));
	if (ast_strlen_zero(val)) {
		ast_log(LOG_ERROR, "STIR/SHAKEN JWT did not have the required field 'typ'\n");
		goto cleanup;
	}
	if (strcmp(val, STIR_SHAKEN_TYPE)) {
		ast_log(LOG_ERROR, "STIR/SHAKEN JWT field 'typ' did not have "
			"required value '%s' (was '%s')\n", STIR_SHAKEN_TYPE, val);
		goto cleanup;
	}

	/* Check the alg value for "ES256" */
	val = ast_json_string_get(ast_json_object_get(obj, "alg"));
	if (ast_strlen_zero(val)) {
		ast_log(LOG_ERROR, "STIR/SHAKEN JWT did not have required field 'alg'\n");
		goto cleanup;
	}
	if (strcmp(val, STIR_SHAKEN_ENCRYPTION_ALGORITHM)) {
		ast_log(LOG_ERROR, "STIR/SHAKEN JWT field 'alg' did not have "
			"required value '%s' (was '%s')\n", STIR_SHAKEN_ENCRYPTION_ALGORITHM, val);
		goto cleanup;
	}

	payload->algorithm = ast_strdup(val);
	if (!payload->algorithm) {
		ast_log(LOG_ERROR, "STIR/SHAKEN payload failed to copy 'algorithm'\n");
		goto cleanup;
	}

	/* Now let's check the payload section */
	obj = ast_json_object_get(json, "payload");
	if (!obj) {
		ast_log(LOG_ERROR, "STIR/SHAKEN payload JWT did not have required field 'payload'\n");
		goto cleanup;
	}

	/* Check the orig tn value for not NULL */
	val = ast_json_string_get(ast_json_object_get(ast_json_object_get(obj, "orig"), "tn"));
	if (ast_strlen_zero(val)) {
		ast_log(LOG_ERROR, "STIR/SHAKEN JWT did not have required field 'orig->tn'\n");
		goto cleanup;
	}

	/* Payload seems sane. Copy it and return on success */
	payload->payload = ast_json_deep_copy(obj);
	if (!payload->payload) {
		ast_log(LOG_ERROR, "STIR/SHAKEN payload failed to copy 'payload'\n");
		goto cleanup;
	}

	return payload;

cleanup:
	ast_stir_shaken_payload_free(payload);
	return NULL;
}

/*!
 * \brief Signs the payload and returns the signature.
 *
 * \param json_str The string representation of the JSON
 * \param private_key The private key used to sign the payload
 *
 * \retval signature on success
 * \retval NULL on failure
 */
static unsigned char *stir_shaken_sign(char *json_str, EVP_PKEY *private_key)
{
	EVP_MD_CTX *mdctx = NULL;
	int ret = 0;
	unsigned char *encoded_signature = NULL;
	unsigned char *signature = NULL;
	size_t encoded_length = 0;
	size_t signature_length = 0;

	mdctx = EVP_MD_CTX_create();
	if (!mdctx) {
		ast_log(LOG_ERROR, "Failed to create Message Digest Context\n");
		goto cleanup;
	}

	ret = EVP_DigestSignInit(mdctx, NULL, EVP_sha256(), NULL, private_key);
	if (ret != 1) {
		ast_log(LOG_ERROR, "Failed to initialize Message Digest Context\n");
		goto cleanup;
	}

	ret = EVP_DigestSignUpdate(mdctx, json_str, strlen(json_str));
	if (ret != 1) {
		ast_log(LOG_ERROR, "Failed to update Message Digest Context\n");
		goto cleanup;
	}

	ret = EVP_DigestSignFinal(mdctx, NULL, &signature_length);
	if (ret != 1) {
		ast_log(LOG_ERROR, "Failed initial phase of Message Digest Context signing\n");
		goto cleanup;
	}

	signature = ast_calloc(1, sizeof(unsigned char) * signature_length);
	if (!signature) {
		ast_log(LOG_ERROR, "Failed to allocate space for signature\n");
		goto cleanup;
	}

	ret = EVP_DigestSignFinal(mdctx, signature, &signature_length);
	if (ret != 1) {
		ast_log(LOG_ERROR, "Failed final phase of Message Digest Context signing\n");
		goto cleanup;
	}

	/* There are 6 bits to 1 base64 digit, so in order to get the size of the base64 encoded
	 * signature, we need to multiply by the number of bits in a byte and divide by 6. Since
	 * there's rounding when doing base64 conversions, add 3 bytes, just in case, and account
	 * for padding. Add another byte for the NULL-terminator.
	 */
	encoded_length = ((signature_length * 4 / 3 + 3) & ~3) + 1;
	encoded_signature = ast_calloc(1, encoded_length);
	if (!encoded_signature) {
		ast_log(LOG_ERROR, "Failed to allocate space for encoded signature\n");
		goto cleanup;
	}

	ast_base64encode((char *)encoded_signature, signature, signature_length, encoded_length);

cleanup:
	if (mdctx) {
		EVP_MD_CTX_destroy(mdctx);
	}
	ast_free(signature);

	return encoded_signature;
}

/*!
 * \brief Adds the 'x5u' (public key URL) field to the JWT.
 *
 * \param json The JWT
 * \param x5u The public key URL
 *
 * \retval 0 on success
 * \retval -1 on failure
 */
static int stir_shaken_add_x5u(struct ast_json *json, const char *x5u)
{
	struct ast_json *value;

	value = ast_json_string_create(x5u);
	if (!value) {
		return -1;
	}

	return ast_json_object_set(ast_json_object_get(json, "header"), "x5u", value);
}

/*!
 * \brief Adds the 'attest' field to the JWT.
 *
 * \param json The JWT
 * \param attest The value to set attest to
 *
 * \retval 0 on success
 * \retval -1 on failure
 */
static int stir_shaken_add_attest(struct ast_json *json, const char *attest)
{
	struct ast_json *value;

	value = ast_json_string_create(attest);
	if (!value) {
		return -1;
	}

	return ast_json_object_set(ast_json_object_get(json, "payload"), "attest", value);
}

/*!
 * \brief Adds the 'origid' field to the JWT.
 *
 * \param json The JWT
 * \param origid The value to set origid to
 *
 * \retval 0 on success
 * \retval -1 on failure
 */
static int stir_shaken_add_origid(struct ast_json *json, const char *origid)
{
	struct ast_json *value;

	value = ast_json_string_create(origid);
	if (!origid) {
		return -1;
	}

	return ast_json_object_set(ast_json_object_get(json, "payload"), "origid", value);
}

/*!
 * \brief Adds the 'iat' field to the JWT.
 *
 * \param json The JWT
 *
 * \retval 0 on success
 * \retval -1 on failure
 */
static int stir_shaken_add_iat(struct ast_json *json)
{
	struct ast_json *value;
	struct timeval tv;
	int timestamp;

	tv = ast_tvnow();
	timestamp = tv.tv_sec + tv.tv_usec / 1000;
	value = ast_json_integer_create(timestamp);

	return ast_json_object_set(ast_json_object_get(json, "payload"), "iat", value);
}

struct ast_stir_shaken_payload *ast_stir_shaken_sign(struct ast_json *json)
{
	struct ast_stir_shaken_payload *payload;
	unsigned char *signature;
	const char *caller_id_num;
	char *json_str = NULL;
	struct stir_shaken_certificate *cert = NULL;

	payload = stir_shaken_verify_json(json);
	if (!payload) {
		return NULL;
	}

	/* From the payload section of the JSON, get the orig section, and then get
	 * the value of tn. This will be the caller ID number */
	caller_id_num = ast_json_string_get(ast_json_object_get(ast_json_object_get(
			ast_json_object_get(json, "payload"), "orig"), "tn"));
	if (!caller_id_num) {
		ast_log(LOG_ERROR, "Failed to get caller ID number from JWT\n");
		goto cleanup;
	}

	cert = stir_shaken_certificate_get_by_caller_id_number(caller_id_num);
	if (!cert) {
		ast_log(LOG_ERROR, "Failed to retrieve certificate for caller ID "
			"'%s'\n", caller_id_num);
		goto cleanup;
	}

	if (stir_shaken_add_x5u(json, stir_shaken_certificate_get_public_key_url(cert))) {
		ast_log(LOG_ERROR, "Failed to add 'x5u' (public key URL) to payload\n");
		goto cleanup;
	}

	/* TODO: This is just a placeholder for adding 'attest', 'iat', and
	 * 'origid' to the payload. Later, additional logic will need to be
	 * added to determine what these values actually are, but the functions
	 * themselves are ready to go.
	 */
	if (stir_shaken_add_attest(json, "B")) {
		ast_log(LOG_ERROR, "Failed to add 'attest' to payload\n");
		goto cleanup;
	}

	if (stir_shaken_add_origid(json, "asterisk")) {
		ast_log(LOG_ERROR, "Failed to add 'origid' to payload\n");
		goto cleanup;
	}

	if (stir_shaken_add_iat(json)) {
		ast_log(LOG_ERROR, "Failed to add 'iat' to payload\n");
		goto cleanup;
	}

	json_str = ast_json_dump_string(json);
	if (!json_str) {
		ast_log(LOG_ERROR, "Failed to convert JSON to string\n");
		goto cleanup;
	}

	signature = stir_shaken_sign(json_str, stir_shaken_certificate_get_private_key(cert));
	if (!signature) {
		goto cleanup;
	}

	payload->signature = signature;
	ao2_cleanup(cert);
	ast_json_free(json_str);

	return payload;

cleanup:
	ao2_cleanup(cert);
	ast_stir_shaken_payload_free(payload);
	ast_json_free(json_str);
	return NULL;
}

static int reload_module(void)
{
	if (stir_shaken_sorcery) {
		ast_sorcery_reload(stir_shaken_sorcery);
	}

	return 0;
}

static int unload_module(void)
{
	stir_shaken_certificate_unload();
	stir_shaken_store_unload();
	stir_shaken_general_unload();

	ast_sorcery_unref(stir_shaken_sorcery);
	stir_shaken_sorcery = NULL;

	return 0;
}

static int load_module(void)
{
	if (!(stir_shaken_sorcery = ast_sorcery_open())) {
		ast_log(LOG_ERROR, "stir/shaken - failed to open sorcery\n");
		return AST_MODULE_LOAD_DECLINE;
	}

	if (stir_shaken_general_load()) {
		unload_module();
		return AST_MODULE_LOAD_DECLINE;
	}

	if (stir_shaken_store_load()) {
		unload_module();
		return AST_MODULE_LOAD_DECLINE;
	}

	if (stir_shaken_certificate_load()) {
		unload_module();
		return AST_MODULE_LOAD_DECLINE;
	}

	ast_sorcery_load(ast_stir_shaken_sorcery());

	return AST_MODULE_LOAD_SUCCESS;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_GLOBAL_SYMBOLS | AST_MODFLAG_LOAD_ORDER,
				"STIR/SHAKEN Module for Asterisk",
	.support_level = AST_MODULE_SUPPORT_CORE,
	.load = load_module,
	.unload = unload_module,
	.reload = reload_module,
	.load_pri = AST_MODPRI_CHANNEL_DEPEND - 1,
	.requires = "res_curl",
);
