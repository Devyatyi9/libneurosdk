#include <stdio.h>
#include <stdlib.h>
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

static void check_json(context_t const *context,
                       neurosdk_message_t const *message,
                       char const *expected) {
	char *json = NULL;
	CHECK(build_c2s_json(context, message, &json) == NeuroSDK_None);
	CHECK(json != NULL);
	if (json) {
		if (strcmp(json, expected) != 0)
			fprintf(stderr, "actual:   %s\nexpected: %s\n", json, expected);
		CHECK(strcmp(json, expected) == 0);
		json_value_t *value = json_parse(json, strlen(json));
		CHECK(value != NULL);
		free(value);
	}
	free(json);
}

static void test_all_message_kinds(void) {
	context_t context = {.game_name = "game\"\\\n"};
	neurosdk_message_t message = {.kind = NeuroSDK_MessageKind_Startup};
	check_json(&context, &message,
	           "{\"command\":\"startup\",\"game\":\"game\\\"\\\\\\n\"}");

	message = (neurosdk_message_t){
	    .kind = NeuroSDK_MessageKind_Context,
	    .value.context = {.message = "\b\f\r\t\1", .silent = true}};
	check_json(&context, &message,
	           "{\"command\":\"context\",\"game\":\"game\\\"\\\\\\n\","
	           "\"data\":{\"message\":\"\\b\\f\\r\\t\\u0001\","
	           "\"silent\":true}}");

	neurosdk_action_t actions[] = {
	    {.name = "jump\"", .description = NULL, .json_schema = NULL},
	    {.name = "look",
	     .description = "left\\right",
	     .json_schema = "{ \"type\" : \"object\" }"},
	};
	message = (neurosdk_message_t){
	    .kind = NeuroSDK_MessageKind_ActionsRegister,
	    .value.actions_register = {.actions = actions, .actions_len = 2}};
	check_json(&context, &message,
	           "{\"command\":\"actions/register\",\"game\":\"game\\\"\\\\\\n\","
	           "\"data\":{\"actions\":[{\"name\":\"jump\\\"\","
	           "\"description\":\"\",\"schema\":{}},{\"name\":\"look\","
	           "\"description\":\"left\\\\right\",\"schema\":{ \"type\" : "
	           "\"object\" }}]}}");

	char *names[] = {"one\"", "two\\"};
	message =
	    (neurosdk_message_t){.kind = NeuroSDK_MessageKind_ActionsUnregister,
	                         .value.actions_unregister = {.action_names = names,
	                                                      .action_names_len = 2}};
	check_json(&context, &message,
	           "{\"command\":\"actions/unregister\",\"game\":\"game\\\"\\\\\\n\","
	           "\"data\":{\"action_names\":[\"one\\\"\",\"two\\\\\"]}}");

	message = (neurosdk_message_t){
	    .kind = NeuroSDK_MessageKind_ActionsForce,
	    .value.actions_force = {.state = NULL,
	                            .query = "why\"?",
	                            .ephemeral_context = true,
	                            .action_names = names,
	                            .action_names_len = 2,
	                            .priority = NeuroSDK_Priority_Critical}};
	check_json(&context, &message,
	           "{\"command\":\"actions/force\",\"game\":\"game\\\"\\\\\\n\","
	           "\"data\":{\"state\":null,\"query\":\"why\\\"?\","
	           "\"ephemeral_context\":true,\"action_names\":[\"one\\\"\","
	           "\"two\\\\\"],\"priority\":\"critical\"}}");
	message.value.actions_force.state = "ready\nnow";
	message.value.actions_force.ephemeral_context = false;
	message.value.actions_force.priority = NeuroSDK_Priority_Medium;
	check_json(&context, &message,
	           "{\"command\":\"actions/force\",\"game\":\"game\\\"\\\\\\n\","
	           "\"data\":{\"state\":\"ready\\nnow\",\"query\":\"why\\\"?\","
	           "\"ephemeral_context\":false,\"action_names\":[\"one\\\"\","
	           "\"two\\\\\"],\"priority\":\"medium\"}}");

	message = (neurosdk_message_t){
	    .kind = NeuroSDK_MessageKind_ActionResult,
	    .value.action_result = {.id = "id\"", .success = false, .message = NULL}};
	check_json(&context, &message,
	           "{\"command\":\"action/result\",\"game\":\"game\\\"\\\\\\n\","
	           "\"data\":{\"id\":\"id\\\"\",\"success\":false,"
	           "\"message\":null}}");
	message.value.action_result.message = "done\\\n";
	message.value.action_result.success = true;
	check_json(&context, &message,
	           "{\"command\":\"action/result\",\"game\":\"game\\\"\\\\\\n\","
	           "\"data\":{\"id\":\"id\\\"\",\"success\":true,"
	           "\"message\":\"done\\\\\\n\"}}");
}

static void test_utf8_and_validation(void) {
	static char const utf8[] = "\320\277\321\200\320\270\320\262\320\265\321\202";
	context_t context = {.game_name = utf8};
	neurosdk_message_t message = {.kind = NeuroSDK_MessageKind_Context,
	                              .value.context = {.message = (char *)utf8}};
	char expected[256];
	snprintf(expected, sizeof(expected),
	         "{\"command\":\"context\",\"game\":\"%s\",\"data\":{"
	         "\"message\":\"%s\",\"silent\":false}}",
	         utf8, utf8);
	check_json(&context, &message, expected);

	unsigned char invalid_utf8[] = {0xC0, 0xAF, '\0'};
	message.value.context.message = (char *)invalid_utf8;
	char *json = NULL;
	CHECK(build_c2s_json(&context, &message, &json) == NeuroSDK_InvalidMessage);
	CHECK(json == NULL);

	neurosdk_action_t action = {.name = "test", .json_schema = "[]"};
	message = (neurosdk_message_t){
	    .kind = NeuroSDK_MessageKind_ActionsRegister,
	    .value.actions_register = {.actions = &action, .actions_len = 1}};
	CHECK(build_c2s_json(&context, &message, &json) == NeuroSDK_InvalidMessage);
	action.json_schema = "{\"broken\":}";
	CHECK(build_c2s_json(&context, &message, &json) == NeuroSDK_InvalidMessage);
	action.json_schema = "{\"type\":\"object\"} trailing";
	CHECK(build_c2s_json(&context, &message, &json) == NeuroSDK_InvalidMessage);

	char *names[] = {NULL};
	message =
	    (neurosdk_message_t){.kind = NeuroSDK_MessageKind_ActionsUnregister,
	                         .value.actions_unregister = {.action_names = names,
	                                                      .action_names_len = 1}};
	CHECK(build_c2s_json(&context, &message, &json) == NeuroSDK_InvalidMessage);
}

static void test_size_limit(void) {
	context_t context = {.game_name = "game"};
	char *large = malloc(PROTOCOL_MESSAGE_MAX_SIZE);
	CHECK(large != NULL);
	if (!large)
		return;
	memset(large, 'a', PROTOCOL_MESSAGE_MAX_SIZE - 1);
	large[PROTOCOL_MESSAGE_MAX_SIZE - 1] = '\0';
	neurosdk_message_t message = {.kind = NeuroSDK_MessageKind_Context,
	                              .value.context = {.message = large}};
	char *json = NULL;
	CHECK(build_c2s_json(&context, &message, &json) == NeuroSDK_InvalidMessage);
	CHECK(json == NULL);
	free(large);
}

int main(void) {
	test_all_message_kinds();
	test_utf8_and_validation();
	test_size_limit();
	if (failures) {
		fprintf(stderr, "%d C2S JSON test(s) failed\n", failures);
		return 1;
	}
	puts("C2S JSON tests passed");
	return 0;
}
