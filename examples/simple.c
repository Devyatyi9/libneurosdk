#include <stdio.h>

#ifdef _WIN32
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 5105)
#endif
#include <windows.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#define usleep(x) Sleep((x) / 1000)
#else
#include <unistd.h>
#endif

#include <neurosdk.h>

#define GAME_NAME "TestGame"

static neurosdk_error_e register_action(neurosdk_context_t *ctx,
                                        neurosdk_action_t *action) {
	neurosdk_message_t reg_msg = {.kind = NeuroSDK_MessageKind_ActionsRegister};
	reg_msg.value.actions_register.actions = action;
	reg_msg.value.actions_register.actions_len = 1;
	return neurosdk_context_send(ctx, &reg_msg);
}

int main(void) {
	neurosdk_context_t ctx;
	neurosdk_context_create_desc_t desc = {
	    .url = "ws://localhost:8000",
	    .game_name = GAME_NAME,
	    .poll_ms = 100,
	    .callback_log = NULL,
#ifdef DEBUG
	    .flags = NEUROSDK_CONTEXT_CREATE_FLAGS_DEBUG
#endif
	};

	neurosdk_error_e err = neurosdk_context_create(&ctx, &desc);
	if (err != NeuroSDK_None) {
		printf("Failed to create context: %s\n", neurosdk_error_string(err));
		return 1;
	}

	printf("Connected to Neuro!\n");
	neurosdk_message_t startup_msg = {.kind = NeuroSDK_MessageKind_Startup};
	err = neurosdk_context_send(&ctx, &startup_msg);
	if (err != NeuroSDK_None) {
		printf("Failed to send startup: %s\n", neurosdk_error_string(err));
		neurosdk_context_destroy(&ctx);
		return 1;
	}

	neurosdk_action_t action = {.name = "choose_name",
	                            .description = "Pick a username",
	                            .json_schema = "{}"};

	err = register_action(&ctx, &action);
	if (err != NeuroSDK_None) {
		printf("Failed to register action: %s\n", neurosdk_error_string(err));
		neurosdk_context_destroy(&ctx);
		return 1;
	}
	printf("Registered action.\n");

	neurosdk_message_t force_msg = {.kind = NeuroSDK_MessageKind_ActionsForce};
	force_msg.value.actions_force.state = "Simulation running";
	force_msg.value.actions_force.query = "Please execute choose_name";
	force_msg.value.actions_force.ephemeral_context = false;
	force_msg.value.actions_force.action_names = (char *[]){"choose_name"};
	force_msg.value.actions_force.action_names_len = 1;

	err = neurosdk_context_send(&ctx, &force_msg);
	if (err != NeuroSDK_None) {
		printf("Failed to force action: %s\n", neurosdk_error_string(err));
		neurosdk_context_destroy(&ctx);
		return 1;
	}
	printf("Requested action execution.\n");

	bool action_result_queued = false;
	bool action_result_flushed = false;
	while (neurosdk_context_connected(&ctx) && !action_result_flushed) {
		neurosdk_message_t *messages = NULL;
		int count = 0;

		err = neurosdk_context_poll(&ctx, &messages, &count);
		if (err != NeuroSDK_None) {
			printf("Polling failed: %s\n", neurosdk_error_string(err));
			break;
		}
		if (action_result_queued)
			action_result_flushed = true;
		if (count > 0) {
			for (int i = 0; i < count; i++) {
				if (messages[i].kind == NeuroSDK_MessageKind_StartupAcknowledgement) {
					printf("Startup acknowledged for %s.\n",
					       messages[i].value.startup_acknowledgement.display_name);
				} else if (messages[i].kind ==
				           NeuroSDK_MessageKind_ActionsReregisterAll) {
					err = register_action(&ctx, &action);
					if (err != NeuroSDK_None) {
						printf("Failed to re-register action: %s\n",
						       neurosdk_error_string(err));
						break;
					}
					printf("Re-registered action.\n");
				} else if (messages[i].kind == NeuroSDK_MessageKind_Action) {
					printf("- ID: %s\n", messages[i].value.action.id);
					printf("- Name: %s\n", messages[i].value.action.name);
					printf("- Data: %s\n", messages[i].value.action.data
					                           ? messages[i].value.action.data
					                           : "(null)");

					neurosdk_message_t res_msg = {.kind =
					                                  NeuroSDK_MessageKind_ActionResult};
					res_msg.value.action_result.id = messages[i].value.action.id;
					res_msg.value.action_result.success = true;
					res_msg.value.action_result.message = "Action executed successfully";

					err = neurosdk_context_send(&ctx, &res_msg);
					if (err != NeuroSDK_None) {
						printf("Failed to send preemptive action result: %s\n",
						       neurosdk_error_string(err));
						for (int j = 0; j < count; j++)
							neurosdk_message_destroy(&messages[j]);
						neurosdk_context_destroy(&ctx);
						return 1;
					}
					printf("Sent preemptive action result.\n");
					action_result_queued = true;
				}
			}

			for (int i = 0; i < count; i++) {
				neurosdk_message_destroy(&messages[i]);
			}
		}
		usleep(500000);
	}

	neurosdk_context_destroy(&ctx);
	printf("Disconnected from Neuro.\n");

	return 0;
}
