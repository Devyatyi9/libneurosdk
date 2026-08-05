#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

#define BACKOFF_INITIAL_MS 500U
#define BACKOFF_MAX_MS 30000U

static sig_atomic_t volatile stop_requested;

static void request_stop(int signal_number) {
	(void)signal_number;
	stop_requested = 1;
}

static void sleep_once_ms(unsigned int milliseconds) {
#ifdef _WIN32
	Sleep(milliseconds);
#else
	usleep(milliseconds * 1000U);
#endif
}

static void sleep_until_retry(unsigned int milliseconds) {
	while (!stop_requested && milliseconds > 0) {
		unsigned int interval = milliseconds > 100U ? 100U : milliseconds;
		sleep_once_ms(interval);
		milliseconds -= interval;
	}
}

static neurosdk_error_e send_registration(neurosdk_context_t *context) {
	static neurosdk_action_t actions[] = {
	    {.name = "ping",
	     .description = "Reply to the game with a short acknowledgement.",
	     .json_schema = "{}"},
	};
	neurosdk_message_t message = {
	    .kind = NeuroSDK_MessageKind_ActionsRegister,
	    .value.actions_register = {.actions = actions, .actions_len = 1},
	};
	return neurosdk_context_send(context, &message);
}

static neurosdk_error_e initialize_session(neurosdk_context_t *context) {
	neurosdk_message_t startup = {.kind = NeuroSDK_MessageKind_Startup};
	neurosdk_error_e error = neurosdk_context_send(context, &startup);
	if (error != NeuroSDK_None)
		return error;
	return send_registration(context);
}

static neurosdk_error_e handle_action(neurosdk_context_t *context,
                                      neurosdk_message_action_t const *action) {
	bool is_ping = strcmp(action->name, "ping") == 0;
	neurosdk_message_t result = {
	    .kind = NeuroSDK_MessageKind_ActionResult,
	    .value.action_result =
	        {
	            .id = action->id,
	            .success = is_ping,
	            .message = is_ping ? "Pong." : "Unknown action.",
	        },
	};
	return neurosdk_context_send(context, &result);
}

static neurosdk_error_e process_messages(neurosdk_context_t *context,
                                         neurosdk_message_t *messages,
                                         int count) {
	neurosdk_error_e error = NeuroSDK_None;
	for (int i = 0; i < count && error == NeuroSDK_None; i++) {
		switch (messages[i].kind) {
			case NeuroSDK_MessageKind_Action:
				error = handle_action(context, &messages[i].value.action);
				break;
			case NeuroSDK_MessageKind_ActionsReregisterAll:
				error = send_registration(context);
				break;
			case NeuroSDK_MessageKind_StartupAcknowledgement:
				printf("Connected character: %s (opaque session ID: %s)\n",
				       messages[i].value.startup_acknowledgement.display_name,
				       messages[i].value.startup_acknowledgement.session_id);
				break;
			default:
				break;
		}
	}
	for (int i = 0; i < count; i++)
		neurosdk_message_destroy(&messages[i]);
	return error;
}

static unsigned int next_backoff(unsigned int current) {
	return current >= BACKOFF_MAX_MS / 2U ? BACKOFF_MAX_MS : current * 2U;
}

static unsigned int add_jitter(unsigned int delay) {
	unsigned int range = delay / 4U;
	return delay - range + (unsigned int)rand() % (range * 2U + 1U);
}

int main(void) {
	signal(SIGINT, request_stop);
#ifdef SIGTERM
	signal(SIGTERM, request_stop);
#endif
	srand((unsigned int)time(NULL));

	neurosdk_context_create_desc_t description = {
	    .url = NULL,
	    .game_name = "ReconnectExample",
	    .poll_ms = 250,
	};
	unsigned int backoff_ms = BACKOFF_INITIAL_MS;

	while (!stop_requested) {
		neurosdk_context_t context = NULL;
		neurosdk_error_e error = neurosdk_context_create(&context, &description);
		if (error == NeuroSDK_None)
			error = initialize_session(&context);
		if (error == NeuroSDK_None) {
			puts("Connected; startup and current actions queued.");
			backoff_ms = BACKOFF_INITIAL_MS;
		}

		while (!stop_requested && error == NeuroSDK_None &&
		       neurosdk_context_connected(&context)) {
			neurosdk_message_t *messages = NULL;
			int count = 0;
			error = neurosdk_context_poll(&context, &messages, &count);
			if (error == NeuroSDK_None)
				error = process_messages(&context, messages, count);
		}

		if (context)
			neurosdk_context_destroy(&context);
		if (stop_requested)
			break;

		unsigned int delay = add_jitter(backoff_ms);
		printf("Disconnected (%s); retrying in %u ms.\n",
		       neurosdk_error_string(error), delay);
		sleep_until_retry(delay);
		backoff_ms = next_backoff(backoff_ms);
	}

	puts("Shutdown complete.");
	return 0;
}
