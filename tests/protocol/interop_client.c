#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 5105)
#endif
#include <windows.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#else
#include <unistd.h>
#endif

#include <neurosdk.h>

static void sleep_ms(unsigned int milliseconds) {
#ifdef _WIN32
	Sleep(milliseconds);
#else
	usleep(milliseconds * 1000U);
#endif
}

static neurosdk_error_e register_actions(neurosdk_context_t *context) {
	static neurosdk_action_t actions[] = {
	    {.name = "echo_text",
	     .description = "Echo text supplied by Neuro.",
	     .json_schema = "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":"
	                    "\"string\"}},\"required\":[\"text\"]}"},
	};
	neurosdk_message_t message = {
	    .kind = NeuroSDK_MessageKind_ActionsRegister,
	    .value.actions_register = {.actions = actions, .actions_len = 1},
	};
	return neurosdk_context_send(context, &message);
}

int main(int argc, char **argv) {
	if (argc != 2) {
		fprintf(stderr, "usage: %s ws://host:port/path\n", argv[0]);
		return 2;
	}
	neurosdk_context_create_desc_t description = {
	    .url = argv[1],
	    .game_name = "Protocol Interop \"UTF-8\"",
	    .poll_ms = 20,
	};
	neurosdk_context_t context = NULL;
	neurosdk_error_e error = neurosdk_context_create(&context, &description);
	if (error != NeuroSDK_None) {
		fprintf(stderr, "context create failed: %s\n",
		        neurosdk_error_string(error));
		return 1;
	}

	neurosdk_message_t startup = {.kind = NeuroSDK_MessageKind_Startup};
	error = neurosdk_context_send(&context, &startup);
	if (error == NeuroSDK_None)
		error = register_actions(&context);

	bool acknowledgement_seen = false;
	bool action_seen = false;
	for (int attempt = 0; attempt < 500 && error == NeuroSDK_None && !action_seen;
	     attempt++) {
		neurosdk_message_t *messages = NULL;
		int count = 0;
		error = neurosdk_context_poll(&context, &messages, &count);
		for (int i = 0; i < count && error == NeuroSDK_None; i++) {
			if (messages[i].kind == NeuroSDK_MessageKind_StartupAcknowledgement) {
				neurosdk_message_startup_acknowledgement_t *ack =
				    &messages[i].value.startup_acknowledgement;
				if (strcmp(ack->session_id, "interop-session") != 0 ||
				    strcmp(ack->character_id, "neuro") != 0 ||
				    strcmp(ack->display_name, "Neuro-sama") != 0) {
					error = NeuroSDK_InvalidMessage;
				} else {
					acknowledgement_seen = true;
				}
			} else if (messages[i].kind ==
			           NeuroSDK_MessageKind_ActionsReregisterAll) {
				error = register_actions(&context);
			} else if (messages[i].kind == NeuroSDK_MessageKind_Action) {
				neurosdk_message_action_t *action = &messages[i].value.action;
				if (strcmp(action->name, "echo_text") != 0 || !action->data ||
				    strcmp(action->data, "{\"text\":\"hello\"}") != 0) {
					error = NeuroSDK_InvalidMessage;
				} else {
					neurosdk_message_t result = {
					    .kind = NeuroSDK_MessageKind_ActionResult,
					    .value.action_result = {.id = action->id,
					                            .success = true,
					                            .message = "Echoed: hello"},
					};
					error = neurosdk_context_send(&context, &result);
					action_seen = error == NeuroSDK_None;
				}
			}
		}
		for (int i = 0; i < count; i++)
			neurosdk_message_destroy(&messages[i]);
		if (!action_seen)
			sleep_ms(10);
	}

	if (error == NeuroSDK_None && (!acknowledgement_seen || !action_seen))
		error = NeuroSDK_InvalidMessage;
	for (int attempt = 0; attempt < 100 && error == NeuroSDK_None &&
	                      neurosdk_context_connected(&context);
	     attempt++) {
		neurosdk_message_t *messages = NULL;
		int count = 0;
		error = neurosdk_context_poll(&context, &messages, &count);
		for (int i = 0; i < count; i++)
			neurosdk_message_destroy(&messages[i]);
		sleep_ms(10);
	}
	neurosdk_context_destroy(&context);
	if (error != NeuroSDK_None) {
		fprintf(stderr, "protocol profile failed: %s\n",
		        neurosdk_error_string(error));
		return 1;
	}
	return 0;
}
