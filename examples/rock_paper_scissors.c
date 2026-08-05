#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif

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

#define WINNING_SCORE 3
#define RECONNECT_ATTEMPTS 5

typedef enum move {
	Move_Invalid,
	Move_Rock,
	Move_Paper,
	Move_Scissors,
} move_t;

static neurosdk_action_t play_action = {
    .name = "play",
    .description = "Choose rock, paper, or scissors for this round.",
    .json_schema =
        "{\"type\":\"object\",\"properties\":{\"choice\":{\"type\":\"string\","
        "\"enum\":[\"rock\",\"paper\",\"scissors\"]}},\"required\":[\"choice\"]"
        ","
        "\"additionalProperties\":false}",
};

static neurosdk_error_e register_play(neurosdk_context_t *context) {
	neurosdk_message_t message = {.kind = NeuroSDK_MessageKind_ActionsRegister};
	message.value.actions_register.actions = &play_action;
	message.value.actions_register.actions_len = 1;
	return neurosdk_context_send(context, &message);
}

static neurosdk_error_e unregister_play(neurosdk_context_t *context) {
	char *names[] = {play_action.name};
	neurosdk_message_t message = {.kind = NeuroSDK_MessageKind_ActionsUnregister};
	message.value.actions_unregister.action_names = names;
	message.value.actions_unregister.action_names_len = 1;
	return neurosdk_context_send(context, &message);
}

static void sleep_before_reconnect(unsigned int milliseconds) {
#ifdef _WIN32
	Sleep(milliseconds);
#else
	usleep(milliseconds * 1000U);
#endif
}

static void skip_spaces(char const **cursor) {
	while (**cursor == ' ' || **cursor == '\t' || **cursor == '\r' ||
	       **cursor == '\n')
		(*cursor)++;
}

static move_t parse_move(char const *data) {
	if (!data)
		return Move_Invalid;
	char const *cursor = data;
	skip_spaces(&cursor);
	if (*cursor++ != '{')
		return Move_Invalid;
	skip_spaces(&cursor);
	if (strncmp(cursor, "\"choice\"", 8) != 0)
		return Move_Invalid;
	cursor += 8;
	skip_spaces(&cursor);
	if (*cursor++ != ':')
		return Move_Invalid;
	skip_spaces(&cursor);

	move_t move = Move_Invalid;
	if (strncmp(cursor, "\"rock\"", 6) == 0) {
		move = Move_Rock;
		cursor += 6;
	} else if (strncmp(cursor, "\"paper\"", 7) == 0) {
		move = Move_Paper;
		cursor += 7;
	} else if (strncmp(cursor, "\"scissors\"", 10) == 0) {
		move = Move_Scissors;
		cursor += 10;
	}
	skip_spaces(&cursor);
	if (*cursor++ != '}')
		return Move_Invalid;
	skip_spaces(&cursor);
	return *cursor == '\0' ? move : Move_Invalid;
}

static char const *move_name(move_t move) {
	switch (move) {
		case Move_Rock:
			return "rock";
		case Move_Paper:
			return "paper";
		case Move_Scissors:
			return "scissors";
		default:
			return "invalid";
	}
}

static int round_result(move_t player, move_t neuro) {
	if (player == neuro)
		return 0;
	if ((player == Move_Rock && neuro == Move_Scissors) ||
	    (player == Move_Paper && neuro == Move_Rock) ||
	    (player == Move_Scissors && neuro == Move_Paper))
		return 1;
	return -1;
}

static neurosdk_error_e flush(neurosdk_context_t *context) {
	neurosdk_message_t *messages = NULL;
	int count = 0;
	neurosdk_error_e error = neurosdk_context_poll(context, &messages, &count);
	for (int i = 0; i < count; i++)
		neurosdk_message_destroy(&messages[i]);
	return error;
}

static neurosdk_error_e queue_round(neurosdk_context_t *context,
                                    move_t player_move,
                                    int player_score,
                                    int neuro_score) {
	neurosdk_error_e error = register_play(context);
	if (error != NeuroSDK_None)
		return error;

	char state[96];
	snprintf(state, sizeof(state),
	         "The player chose %s. Round wins: player %d, Neuro %d.",
	         move_name(player_move), player_score, neuro_score);
	char *action_names[] = {play_action.name};
	neurosdk_message_t force = {.kind = NeuroSDK_MessageKind_ActionsForce};
	force.value.actions_force.state = state;
	force.value.actions_force.query = "Choose your move for this round.";
	force.value.actions_force.action_names = action_names;
	force.value.actions_force.action_names_len = 1;
	return neurosdk_context_send(context, &force);
}

static neurosdk_error_e restore_round(
    neurosdk_context_t *context,
    neurosdk_context_create_desc_t *description,
    move_t player_move,
    int player_score,
    int neuro_score) {
	if (*context)
		neurosdk_context_destroy(context);

	neurosdk_error_e error = NeuroSDK_ConnectionError;
	unsigned int delay_ms = 500U;
	for (int attempt = 0; attempt < RECONNECT_ATTEMPTS; attempt++) {
		printf("Disconnected; restoring the active round in %u ms.\n", delay_ms);
		sleep_before_reconnect(delay_ms);
		error = neurosdk_context_create(context, description);
		if (error == NeuroSDK_None) {
			neurosdk_message_t startup = {.kind = NeuroSDK_MessageKind_Startup};
			error = neurosdk_context_send(context, &startup);
		}
		if (error == NeuroSDK_None)
			error = queue_round(context, player_move, player_score, neuro_score);
		if (error != NeuroSDK_None && *context)
			neurosdk_context_destroy(context);
		if (error == NeuroSDK_None)
			return NeuroSDK_None;
		delay_ms *= 2U;
	}
	return error;
}

int main(void) {
	neurosdk_context_t context = NULL;
	neurosdk_context_create_desc_t description = {
	    .game_name = "Rock Paper Scissors",
	    .poll_ms = 100,
	};
	neurosdk_error_e error = neurosdk_context_create(&context, &description);
	if (error != NeuroSDK_None) {
		fprintf(stderr, "Connection failed: %s\n", neurosdk_error_string(error));
		return 1;
	}

	neurosdk_message_t startup = {.kind = NeuroSDK_MessageKind_Startup};
	error = neurosdk_context_send(&context, &startup);
	if (error == NeuroSDK_None)
		error = flush(&context);
	if (error != NeuroSDK_None) {
		fprintf(stderr, "Startup failed: %s\n", neurosdk_error_string(error));
		neurosdk_context_destroy(&context);
		return 1;
	}

	int player_score = 0;
	int neuro_score = 0;
	while (player_score < WINNING_SCORE && neuro_score < WINNING_SCORE) {
		int input = 0;
		printf("Round wins: you %d, Neuro %d\n", player_score, neuro_score);
		printf("Choose 1=rock, 2=paper, 3=scissors: ");
		if (scanf("%d", &input) != 1) {
			fprintf(stderr, "Invalid input.\n");
			break;
		}
		move_t player_move =
		    input >= 1 && input <= 3 ? (move_t)input : Move_Invalid;
		if (player_move == Move_Invalid) {
			printf("Invalid choice.\n");
			continue;
		}

		error = queue_round(&context, player_move, player_score, neuro_score);
		if (error != NeuroSDK_None || !neurosdk_context_connected(&context))
			error = restore_round(&context, &description, player_move, player_score,
			                      neuro_score);

		bool round_complete = false;
		bool action_window_active = true;
		int flush_polls = 2;
		while (neurosdk_context_connected(&context) &&
		       (!round_complete || flush_polls > 0)) {
			neurosdk_message_t *messages = NULL;
			int count = 0;
			error = neurosdk_context_poll(&context, &messages, &count);
			if (flush_polls > 0)
				flush_polls--;
			if (error != NeuroSDK_None || !neurosdk_context_connected(&context)) {
				if (action_window_active) {
					error = restore_round(&context, &description, player_move,
					                      player_score, neuro_score);
					flush_polls = 2;
					continue;
				}
				break;
			}
			for (int i = 0; i < count; i++) {
				if (messages[i].kind == NeuroSDK_MessageKind_ActionsReregisterAll &&
				    action_window_active) {
					error = register_play(&context);
					flush_polls = error == NeuroSDK_None ? 2 : 0;
				} else if (messages[i].kind == NeuroSDK_MessageKind_Action) {
					move_t neuro_move =
					    action_window_active &&
					            strcmp(messages[i].value.action.name, "play") == 0
					        ? parse_move(messages[i].value.action.data)
					        : Move_Invalid;
					neurosdk_message_t result = {.kind =
					                                 NeuroSDK_MessageKind_ActionResult};
					result.value.action_result.id = messages[i].value.action.id;
					if (neuro_move == Move_Invalid) {
						result.value.action_result.success = false;
						result.value.action_result.message =
						    "Expected {\"choice\":\"rock|paper|scissors\"}.";
						error = neurosdk_context_send(&context, &result);
						flush_polls = error == NeuroSDK_None ? 2 : 0;
					} else {
						int outcome = round_result(player_move, neuro_move);
						if (outcome > 0)
							player_score++;
						else if (outcome < 0)
							neuro_score++;
						printf("Neuro chose %s: %s\n", move_name(neuro_move),
						       outcome > 0   ? "you win the round"
						       : outcome < 0 ? "Neuro wins the round"
						                     : "draw");
						result.value.action_result.success = true;
						result.value.action_result.message = "Move accepted.";
						error = neurosdk_context_send(&context, &result);
						if (error == NeuroSDK_None) {
							action_window_active = false;
							error = unregister_play(&context);
						}
						char summary[128];
						snprintf(summary, sizeof(summary),
						         "Player chose %s, Neuro chose %s. Round wins: player %d, "
						         "Neuro %d.",
						         move_name(player_move), move_name(neuro_move),
						         player_score, neuro_score);
						neurosdk_message_t context_message = {
						    .kind = NeuroSDK_MessageKind_Context,
						};
						context_message.value.context.message = summary;
						context_message.value.context.silent = false;
						if (error == NeuroSDK_None)
							error = neurosdk_context_send(&context, &context_message);
						flush_polls = error == NeuroSDK_None ? 2 : 0;
						round_complete = error == NeuroSDK_None;
					}
				}
				neurosdk_message_destroy(&messages[i]);
			}
		}
		if (error != NeuroSDK_None || !round_complete)
			break;
	}

	if (error != NeuroSDK_None)
		fprintf(stderr, "Protocol error: %s\n", neurosdk_error_string(error));
	else if (player_score == WINNING_SCORE)
		printf("You win the match!\n");
	else if (neuro_score == WINNING_SCORE)
		printf("Neuro wins the match!\n");
	neurosdk_context_destroy(&context);
	return error == NeuroSDK_None ? 0 : 1;
}
