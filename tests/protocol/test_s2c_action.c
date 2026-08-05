#include <stdio.h>
#include <string.h>

#include "../../src/neurosdk.c"

static int failures;

static void check(bool condition, int line, char const *expression) {
	if (!condition) {
		fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, line, expression);
		failures++;
	}
}

#define CHECK(condition) check((condition), __LINE__, #condition)

static context_t make_context(neurosdk_message_t *queue, int capacity) {
	context_t context = {0};
	context.message_queue = queue;
	context.message_queue_cap = capacity;
	return context;
}

static void receive_n(context_t *context, char const *json, size_t length) {
	ws_on_message(NULL, json, length, 0, context);
}

static void receive(context_t *context, char const *json) {
	receive_n(context, json, strlen(json));
}

static void test_valid_action(void) {
	neurosdk_message_t queue[1] = {0};
	context_t context = make_context(queue, 1);

	receive(&context,
	        "{\"command\":\"action\",\"data\":{\"id\":\"42\",\"name\":"
	        "\"jump\",\"data\":\"{\\\"height\\\":2}\"}}");

	CHECK(context.conn_err == NeuroSDK_None);
	CHECK(context.message_queue_size == 1);
	CHECK(queue[0].kind == NeuroSDK_MessageKind_Action);
	CHECK(strcmp(queue[0].value.action.id, "42") == 0);
	CHECK(strcmp(queue[0].value.action.name, "jump") == 0);
	CHECK(strcmp(queue[0].value.action.data, "{\"height\":2}") == 0);
	CHECK(neurosdk_message_destroy(&queue[0]) == NeuroSDK_None);
	CHECK(queue[0].kind == NeuroSDK_MessageKind_Unknown);
}

static void test_null_action_data(void) {
	neurosdk_message_t queue[1] = {0};
	context_t context = make_context(queue, 1);

	receive(&context,
	        "{\"command\":\"action\",\"data\":{\"id\":\"42\",\"name\":"
	        "\"jump\",\"data\":null}}");

	CHECK(context.conn_err == NeuroSDK_None);
	CHECK(context.message_queue_size == 1);
	CHECK(queue[0].value.action.data == NULL);
	CHECK(neurosdk_message_destroy(&queue[0]) == NeuroSDK_None);
}

static void test_utf8_action(void) {
	static char const utf8_name[] =
	    "\320\277\321\200\321\213\320\266\320\276\320\272";
	static char const json[] =
	    "{\"command\":\"action\",\"data\":{\"id\":\"42\",\"name\":\""
	    "\320\277\321\200\321\213\320\266\320\276\320\272"
	    "\",\"data\":null}}";
	neurosdk_message_t queue[1] = {0};
	context_t context = make_context(queue, 1);

	receive(&context, json);

	CHECK(context.conn_err == NeuroSDK_None);
	CHECK(context.message_queue_size == 1);
	CHECK(strcmp(queue[0].value.action.name, utf8_name) == 0);
	CHECK(neurosdk_message_destroy(&queue[0]) == NeuroSDK_None);
}

static void test_unicode_action(void) {
	static char const expected_name[] =
	    "\344\270\255\346\226\207 \360\237\224\245";
	neurosdk_message_t queue[1] = {0};
	context_t context = make_context(queue, 1);

	receive(&context,
	        "{\"command\":\"action\",\"data\":{\"id\":\"unicode\","
	        "\"name\":\"\\u4E2D\\u6587 \\uD83D\\uDD25\",\"data\":null}}");

	CHECK(context.conn_err == NeuroSDK_None);
	CHECK(context.message_queue_size == 1);
	CHECK(strcmp(queue[0].value.action.name, expected_name) == 0);
	CHECK(neurosdk_message_destroy(&queue[0]) == NeuroSDK_None);
}

static void test_startup_acknowledgement(void) {
	neurosdk_message_t queue[1] = {0};
	context_t context = make_context(queue, 1);

	receive(&context,
	        "{\"command\":\"startup\",\"data\":{\"session\":{"
	        "\"sessionId\":\"session-42\",\"characterId\":\"neuro\","
	        "\"displayName\":\"Neuro-sama\"}}}");

	CHECK(context.conn_err == NeuroSDK_None);
	CHECK(context.message_queue_size == 1);
	CHECK(queue[0].kind == NeuroSDK_MessageKind_StartupAcknowledgement);
	CHECK(strcmp(queue[0].value.startup_acknowledgement.session_id,
	             "session-42") == 0);
	CHECK(strcmp(queue[0].value.startup_acknowledgement.character_id, "neuro") ==
	      0);
	CHECK(strcmp(queue[0].value.startup_acknowledgement.display_name,
	             "Neuro-sama") == 0);
	CHECK(neurosdk_message_destroy(&queue[0]) == NeuroSDK_None);
	CHECK(queue[0].kind == NeuroSDK_MessageKind_Unknown);
}

static void test_reregister_all(void) {
	neurosdk_message_t queue[1] = {0};
	context_t context = make_context(queue, 1);

	receive(&context, "{\"command\":\"actions/reregister_all\"}");

	CHECK(context.conn_err == NeuroSDK_None);
	CHECK(context.message_queue_size == 1);
	CHECK(queue[0].kind == NeuroSDK_MessageKind_ActionsReregisterAll);
	CHECK(neurosdk_message_destroy(&queue[0]) == NeuroSDK_None);
	CHECK(queue[0].kind == NeuroSDK_MessageKind_Unknown);
}

static void test_invalid_action(char const *json,
                                neurosdk_error_e expected_error) {
	neurosdk_message_t queue[1] = {0};
	context_t context = make_context(queue, 1);

	receive(&context, json);

	if (context.conn_err != expected_error || context.message_queue_size != 0) {
		fprintf(
		    stderr,
		    "Invalid action mismatch: expected error %d, got %d, queue %d: %s\n",
		    expected_error, context.conn_err, context.message_queue_size, json);
		failures++;
	}
	if (context.message_queue_size != 0)
		neurosdk_message_destroy(&queue[0]);
}

static void test_invalid_actions(void) {
	test_invalid_action("{\"command\":\"action\"}", NeuroSDK_InvalidMessage);
	test_invalid_action("{\"command\":\"action\",\"data\":null}",
	                    NeuroSDK_InvalidMessage);
	test_invalid_action("{\"command\":\"action\",\"data\":{\"name\":\"jump\"}}",
	                    NeuroSDK_InvalidMessage);
	test_invalid_action("{\"command\":\"action\",\"data\":{\"id\":\"42\"}}",
	                    NeuroSDK_InvalidMessage);
	test_invalid_action(
	    "{\"command\":\"action\",\"data\":{\"id\":\"allocated\",\"name\":42}}",
	    NeuroSDK_InvalidMessage);
	test_invalid_action(
	    "{\"command\\u0000ignored\":\"action\",\"data\":{\"id\":\"42\",\"name\":"
	    "\"jump\"}}",
	    NeuroSDK_InvalidMessage);
	test_invalid_action(
	    "{\"command\":\"action\",\"data\\u0000ignored\":{\"id\":\"42\",\"name\":"
	    "\"jump\"}}",
	    NeuroSDK_InvalidMessage);
	test_invalid_action(
	    "{\"command\":\"action\",\"data\":{\"id\\u0000ignored\":\"42\",\"name\":"
	    "\"jump\"}}",
	    NeuroSDK_InvalidMessage);
	test_invalid_action(
	    "{\"command\":\"action\\u0000ignored\",\"data\":{\"id\":\"42\",\"name\":"
	    "\"jump\"}}",
	    NeuroSDK_InvalidMessage);
	test_invalid_action(
	    "{\"command\":\"action\",\"data\":{\"id\":\"42\\u0000ignored\",\"name\":"
	    "\"jump\"}}",
	    NeuroSDK_InvalidMessage);
	test_invalid_action(
	    "{\"command\":\"action\",\"data\":{\"id\":\"42\",\"name\":"
	    "\"jump\\u0000ignored\"}}",
	    NeuroSDK_InvalidMessage);
	test_invalid_action(
	    "{\"command\":\"action\",\"data\":{\"id\":\"42\",\"name\":\"jump\","
	    "\"data\":\"{}\\u0000ignored\"}}",
	    NeuroSDK_InvalidMessage);
}

static void test_duplicate_fields(void) {
	test_invalid_action(
	    "{\"command\":\"action\",\"command\":\"action\",\"data\":{\"id\":\"42\","
	    "\"name\":\"jump\"}}",
	    NeuroSDK_InvalidMessage);
	test_invalid_action(
	    "{\"command\":\"action\",\"data\":{\"id\":\"42\",\"name\":\"jump\"},"
	    "\"data\":{\"id\":\"43\",\"name\":\"run\"}}",
	    NeuroSDK_InvalidMessage);
	test_invalid_action(
	    "{\"command\":\"action\",\"data\":{\"id\":\"42\",\"id\":\"43\","
	    "\"name\":\"jump\"}}",
	    NeuroSDK_InvalidMessage);
	test_invalid_action(
	    "{\"command\":\"action\",\"data\":{\"id\":\"42\",\"name\":\"jump\","
	    "\"name\":\"run\"}}",
	    NeuroSDK_InvalidMessage);
	test_invalid_action(
	    "{\"command\":\"action\",\"data\":{\"id\":\"42\",\"name\":\"jump\","
	    "\"data\":null,\"data\":\"{}\"}}",
	    NeuroSDK_InvalidMessage);
}

static void test_error_classification(void) {
	test_invalid_action("{", NeuroSDK_InvalidJSON);
	test_invalid_action(
	    "{\"command\":\"action\",\"data\":{\"id\":\"42\",\"name\":"
	    "\"jump\"}} trailing",
	    NeuroSDK_InvalidJSON);
	test_invalid_action("[]", NeuroSDK_InvalidMessage);
	test_invalid_action("{\"command\":\"future/command\"}",
	                    NeuroSDK_UnknownCommand);
}

static char *make_nested_action(size_t depth) {
	static char const prefix[] = "{\"command\":\"action\",\"extra\":";
	static char const suffix[] = ",\"data\":{\"id\":\"42\",\"name\":\"jump\"}}";
	if (depth > (SIZE_MAX - sizeof(prefix) - sizeof(suffix)) / 2)
		return NULL;
	size_t size = sizeof(prefix) - 1 + depth * 2 + sizeof(suffix);
	char *json = malloc(size);
	if (!json)
		return NULL;
	char *cursor = json;
	memcpy(cursor, prefix, sizeof(prefix) - 1);
	cursor += sizeof(prefix) - 1;
	memset(cursor, '[', depth);
	cursor += depth;
	memset(cursor, ']', depth);
	cursor += depth;
	memcpy(cursor, suffix, sizeof(suffix));
	return json;
}

static void test_depth_limit(void) {
	char *json = make_nested_action(16);
	CHECK(json != NULL);
	if (!json)
		return;
	neurosdk_message_t queue[1] = {0};
	context_t context = make_context(queue, 1);
	receive(&context, json);
	CHECK(context.conn_err == NeuroSDK_None);
	CHECK(context.message_queue_size == 1);
	CHECK(neurosdk_message_destroy(&queue[0]) == NeuroSDK_None);
	free(json);

	json = make_nested_action(JSON_MAX_RECURSION + 1);
	CHECK(json != NULL);
	if (!json)
		return;
	context = make_context(queue, 1);
	receive(&context, json);
	CHECK(context.conn_err == NeuroSDK_InvalidJSON);
	CHECK(context.message_queue_size == 0);
	free(json);
}

static void test_invalid_startup_messages(void) {
	test_invalid_action("{\"command\":\"startup\"}", NeuroSDK_InvalidMessage);
	test_invalid_action("{\"command\":\"startup\",\"data\":null}",
	                    NeuroSDK_InvalidMessage);
	test_invalid_action("{\"command\":\"startup\",\"data\":{}}",
	                    NeuroSDK_InvalidMessage);
	test_invalid_action("{\"command\":\"startup\",\"data\":{\"session\":null}}",
	                    NeuroSDK_InvalidMessage);
	test_invalid_action(
	    "{\"command\":\"startup\",\"data\":{\"session\":{},\"session\":{}}}",
	    NeuroSDK_InvalidMessage);
	test_invalid_action(
	    "{\"command\":\"startup\",\"data\":{\"session\":{\"sessionId\":\"s\","
	    "\"characterId\":\"neuro\"}}}",
	    NeuroSDK_InvalidMessage);
	test_invalid_action(
	    "{\"command\":\"startup\",\"data\":{\"session\":{\"sessionId\":\"s\","
	    "\"sessionId\":\"again\",\"characterId\":\"neuro\","
	    "\"displayName\":\"Neuro-sama\"}}}",
	    NeuroSDK_InvalidMessage);
	test_invalid_action(
	    "{\"command\":\"startup\",\"data\":{\"session\":{\"sessionId\":\"s\","
	    "\"characterId\":\"neuro\\u0000hidden\","
	    "\"displayName\":\"Neuro-sama\"}}}",
	    NeuroSDK_InvalidMessage);
	test_invalid_action(
	    "{\"command\":\"startup\",\"data\":{\"session\":{\"sessionId\":\"s\","
	    "\"characterId\":\"neuro\",\"displayName\":42}}}",
	    NeuroSDK_InvalidMessage);
}

static void test_invalid_reregister_all(void) {
	test_invalid_action("{\"command\":\"actions/reregister_all\",\"data\":null}",
	                    NeuroSDK_InvalidMessage);
}

static void test_raw_nul(void) {
	static char const json[] =
	    "{\"command\":\"action\",\0\"data\":{\"id\":\"42\",\"name\":\"jump\"}}";
	neurosdk_message_t queue[1] = {0};
	context_t context = make_context(queue, 1);

	receive_n(&context, json, sizeof(json) - 1);

	CHECK(context.conn_err == NeuroSDK_InvalidJSON);
	CHECK(context.message_queue_size == 0);
}

static void test_invalid_unicode(void) {
	test_invalid_action(
	    "{\"command\":\"action\",\"data\":{\"id\":\"42\",\"name\":"
	    "\"\\uD800\"}}",
	    NeuroSDK_InvalidJSON);
	test_invalid_action(
	    "{\"command\":\"action\",\"data\":{\"id\":\"42\",\"name\":"
	    "\"\\uDC00\"}}",
	    NeuroSDK_InvalidJSON);
	test_invalid_action(
	    "{\"command\":\"action\",\"data\":{\"id\":\"42\",\"name\":"
	    "\"\\uD800\\u0041\"}}",
	    NeuroSDK_InvalidJSON);

	static char const malformed_utf8[] =
	    "{\"command\":\"action\",\"data\":{\"id\":\"42\",\"name\":\""
	    "\300\257\"}}";
	neurosdk_message_t queue[1] = {0};
	context_t context = make_context(queue, 1);
	receive_n(&context, malformed_utf8, sizeof(malformed_utf8) - 1);
	CHECK(context.conn_err == NeuroSDK_InvalidJSON);
	CHECK(context.message_queue_size == 0);

	static char const raw_control[] =
	    "{\"command\":\"action\",\"data\":{\"id\":\"42\",\"name\":\"bad"
	    "\1value\"}}";
	context = make_context(queue, 1);
	receive_n(&context, raw_control, sizeof(raw_control) - 1);
	CHECK(context.conn_err == NeuroSDK_InvalidJSON);
	CHECK(context.message_queue_size == 0);
}

static void check_parse_is_transactional(char const *json,
                                         neurosdk_error_e expected_error) {
	context_t context = {0};
	neurosdk_message_t expected = {
	    .kind = NeuroSDK_MessageKind_ActionResult,
	    .value.action_result =
	        {
	            .id = (char *)"sentinel-id",
	            .success = true,
	            .message = (char *)"sentinel-message",
	        },
	};
	neurosdk_message_t actual = expected;

	CHECK(parse_s2c_json(&context, &actual, json, strlen(json)) ==
	      expected_error);
	CHECK(memcmp(&actual, &expected, sizeof(actual)) == 0);
}

static void test_parse_is_transactional(void) {
	check_parse_is_transactional("{\"command\":\"action\"}",
	                             NeuroSDK_InvalidMessage);
	check_parse_is_transactional(
	    "{\"command\":\"action\",\"data\":{\"id\":\"allocated\",\"name\":42}}",
	    NeuroSDK_InvalidMessage);
	check_parse_is_transactional(
	    "{\"command\":\"action\",\"data\":{\"id\":\"allocated\",\"id\":\"again\","
	    "\"name\":\"jump\"}}",
	    NeuroSDK_InvalidMessage);
}

static void test_queue_full(void) {
	context_t context = make_context(NULL, 0);

	receive(&context,
	        "{\"command\":\"action\",\"data\":{\"id\":\"42\",\"name\":"
	        "\"jump\",\"data\":\"{}\"}}");

	CHECK(context.conn_err == NeuroSDK_MessageQueueFull);
	CHECK(context.message_queue_size == 0);
}

static void test_message_size_limit(void) {
	static char const prefix[] =
	    "{\"command\":\"action\",\"data\":{\"id\":\"42\",\"name\":\"jump\"}}";
	char *json = malloc((size_t)PROTOCOL_MESSAGE_MAX_SIZE + 1);
	CHECK(json != NULL);
	if (!json)
		return;
	memcpy(json, prefix, sizeof(prefix) - 1);
	memset(json + sizeof(prefix) - 1, ' ',
	       (size_t)PROTOCOL_MESSAGE_MAX_SIZE + 1 - (sizeof(prefix) - 1));

	neurosdk_message_t queue[1] = {0};
	context_t context = make_context(queue, 1);
	receive_n(&context, json, PROTOCOL_MESSAGE_MAX_SIZE);
	CHECK(context.conn_err == NeuroSDK_None);
	CHECK(context.message_queue_size == 1);
	CHECK(neurosdk_message_destroy(&queue[0]) == NeuroSDK_None);

	context = make_context(queue, 1);
	receive_n(&context, json, (size_t)PROTOCOL_MESSAGE_MAX_SIZE + 1);
	CHECK(context.conn_err == NeuroSDK_InvalidJSON);
	CHECK(context.message_queue_size == 0);
	free(json);
}

static void test_destroy_with_queued_action(void) {
	context_t *context = calloc(1, sizeof(*context));
	CHECK(context != NULL);
	if (!context)
		return;
	context->message_queue = calloc(1, sizeof(*context->message_queue));
	context->pending_messages = calloc(1, sizeof(*context->pending_messages));
	context->game_name = duplicate_string("test");
	context->message_queue_cap = 1;
	CHECK(context->message_queue != NULL);
	CHECK(context->pending_messages != NULL);
	CHECK(context->game_name != NULL);
	int mutex_result = mtx_init(&context->out_mtx, mtx_plain);
	CHECK(mutex_result == thrd_success);
	if (!context->message_queue || !context->pending_messages ||
	    !context->game_name || mutex_result != thrd_success) {
		if (mutex_result == thrd_success)
			mtx_destroy(&context->out_mtx);
		free(context->message_queue);
		free(context->pending_messages);
		free((void *)context->game_name);
		free(context);
		return;
	}

	receive(context,
	        "{\"command\":\"action\",\"data\":{\"id\":\"42\",\"name\":"
	        "\"jump\",\"data\":\"{}\"}}");
	CHECK(context->message_queue_size == 1);

	neurosdk_context_t public_context = (neurosdk_context_t)context;
	CHECK(neurosdk_context_destroy(&public_context) == NeuroSDK_None);
	CHECK(public_context == NULL);
}

int main(void) {
	test_valid_action();
	test_null_action_data();
	test_utf8_action();
	test_unicode_action();
	test_startup_acknowledgement();
	test_reregister_all();
	test_invalid_actions();
	test_duplicate_fields();
	test_error_classification();
	test_depth_limit();
	test_invalid_startup_messages();
	test_invalid_reregister_all();
	test_raw_nul();
	test_invalid_unicode();
	test_parse_is_transactional();
	test_queue_full();
	test_message_size_limit();
	test_destroy_with_queued_action();

	if (failures) {
		fprintf(stderr, "%d S2C action test(s) failed\n", failures);
		return 1;
	}
	puts("S2C action tests passed");
	return 0;
}
