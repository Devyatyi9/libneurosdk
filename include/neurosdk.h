#ifndef NEURO_SDK_H
#define NEURO_SDK_H

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

#include <stdbool.h>

/////////////////////////////////
// Export Definitions & Macros //
/////////////////////////////////

#if defined(__linux__) || defined(__APPLE__)
#define NEUROSDK_EXPORT
#else
#define NEUROSDK_EXPORT __declspec(dllexport)
#endif

#define OUT

//////////////////////////////
// Type Definitions & Enums //
//////////////////////////////

// Handles
typedef void *neurosdk_context_t;

// Error Codes
typedef enum neurosdk_error {
	NeuroSDK_None = 0,
	NeuroSDK_Internal,
	NeuroSDK_Uninitialized,
	NeuroSDK_NoGameName,
	NeuroSDK_OutOfMemory,
	NeuroSDK_NoURL,
	NeuroSDK_ConnectionError,
	NeuroSDK_MessageQueueFull,
	NeuroSDK_ReceivedBinary,
	NeuroSDK_InvalidJSON,
	NeuroSDK_UnknownCommand,
	NeuroSDK_InvalidMessage,
	NeuroSDK_CommandNotAvailable,
	NeuroSDK_SendFailed
} neurosdk_error_e;

// Severity Levels
typedef enum neurosdk_severity {
	NeuroSDK_Severity_Debug,
	NeuroSDK_Severity_Info,
	NeuroSDK_Severity_Warn,
	NeuroSDK_Severity_Error
} neurosdk_severity_e;

// Force Priority Levels
typedef enum neurosdk_priority {
	NeuroSDK_Priority_Low,
	NeuroSDK_Priority_Medium,
	NeuroSDK_Priority_High,
	NeuroSDK_Priority_Critical,
} neurosdk_priority_e;

// Context Creation Flags
typedef enum neurosdk_context_create_flags {
	NeuroSDK_ContextCreateFlags_None = 0,
	NeuroSDK_ContextCreateFlags_DebugPrints = (1 << 0),
	NeuroSDK_ContextCreateFlags_ValidationLayers = (1 << 1)
} neurosdk_context_create_flags_e;

#define NEUROSDK_CONTEXT_CREATE_FLAGS_DEBUG  \
	(NeuroSDK_ContextCreateFlags_DebugPrints | \
	 NeuroSDK_ContextCreateFlags_ValidationLayers)

// Message Types
typedef enum neurosdk_message_kind {
	NeuroSDK_MessageKind_Unknown = 0,
	// Client to Server (C2S)
	NeuroSDK_MessageKind_Startup,
	NeuroSDK_MessageKind_Context,
	NeuroSDK_MessageKind_ActionsRegister,
	NeuroSDK_MessageKind_ActionsUnregister,
	NeuroSDK_MessageKind_ActionsForce,
	NeuroSDK_MessageKind_ActionResult,
	// Server to Client (S2C)
	NeuroSDK_MessageKind_Action,
	NeuroSDK_MessageKind_StartupAcknowledgement,
	// Optional compatibility message proposed by the Neuro API.
	NeuroSDK_MessageKind_ActionsReregisterAll
} neurosdk_message_kind_e;

// Callbacks
typedef void (*neurosdk_callback_log_t)(neurosdk_severity_e severity,
                                        char *message,
                                        void *user_data);

/////////////////////
// Data Structures //
/////////////////////

// String encoding contract:
// - Every C string supplied by the application must be NUL-terminated UTF-8.
// - Convert UTF-16, wchar_t, and other application encodings to UTF-8 before
//   calling the SDK. Invalid UTF-8 in an outgoing message is rejected with
//   NeuroSDK_InvalidMessage.
// - String fields in messages returned by the SDK are NUL-terminated UTF-8 and
//   remain owned by that message until neurosdk_message_destroy() is called.

// Action Definition
typedef struct neurosdk_action {
	char *name;
	char *description;
	char *json_schema;
} neurosdk_action_t;

// Message Context
typedef struct neurosdk_message_context {
	char *message;
	bool silent;
} neurosdk_message_context_t;

// Action Registration
typedef struct neurosdk_message_actions_register {
	neurosdk_action_t *actions;
	int actions_len;
} neurosdk_message_actions_register_t;

// Action Unregistration
typedef struct neurosdk_message_actions_unregister {
	char **action_names;
	int action_names_len;
} neurosdk_message_actions_unregister_t;

// Forced Actions
typedef struct neurosdk_message_actions_force {
	char *state;
	char *query;
	bool ephemeral_context;
	char **action_names;
	int action_names_len;
	neurosdk_priority_e priority;
} neurosdk_message_actions_force_t;

// Action Result
typedef struct neurosdk_message_action_result {
	char *id;
	bool success;
	char *message;
} neurosdk_message_action_result_t;

// Action Message
typedef struct neurosdk_message_action {
	char *id;
	char *name;
	char *data;
} neurosdk_message_action_t;

// Startup Acknowledgement
typedef struct neurosdk_message_startup_acknowledgement {
	char *session_id;
	char *character_id;
	char *display_name;
} neurosdk_message_startup_acknowledgement_t;

// General Message Structure
typedef struct neurosdk_message {
	neurosdk_message_kind_e kind;
	union {
		neurosdk_message_context_t context;
		neurosdk_message_actions_register_t actions_register;
		neurosdk_message_actions_unregister_t actions_unregister;
		neurosdk_message_actions_force_t actions_force;
		neurosdk_message_action_result_t action_result;
		neurosdk_message_action_t action;
		neurosdk_message_startup_acknowledgement_t startup_acknowledgement;
	} value;
} neurosdk_message_t;

// Context Creation Descriptor
typedef struct neurosdk_context_create_desc {
	char const *url;
	char const *game_name;
	int poll_ms;
	void *user_data;
	neurosdk_context_create_flags_e flags;
	neurosdk_callback_log_t callback_log;
} neurosdk_context_create_desc_t;

//////////////////////
// Public Functions //
//////////////////////

// Library Information
NEUROSDK_EXPORT char const *neurosdk_version(void);
NEUROSDK_EXPORT char const *neurosdk_git_hash(void);

// Error Handling
NEUROSDK_EXPORT char const *neurosdk_error_string(neurosdk_error_e err);

// Message Management. Destroy every message returned by
// neurosdk_context_poll(). The returned array remains owned by its context and
// may be reused by later polls.
NEUROSDK_EXPORT neurosdk_error_e
neurosdk_message_destroy(neurosdk_message_t *msg);

// Context Management
NEUROSDK_EXPORT neurosdk_error_e
neurosdk_context_create(neurosdk_context_t *ctx,
                        neurosdk_context_create_desc_t *desc);
NEUROSDK_EXPORT neurosdk_error_e
neurosdk_context_destroy(neurosdk_context_t *ctx);
NEUROSDK_EXPORT bool neurosdk_context_connected(neurosdk_context_t *ctx);

// Communication Functions
NEUROSDK_EXPORT neurosdk_error_e
neurosdk_context_poll(neurosdk_context_t *ctx,
                      OUT neurosdk_message_t **messages,
                      OUT int *count);
NEUROSDK_EXPORT neurosdk_error_e neurosdk_context_send(neurosdk_context_t *ctx,
                                                       neurosdk_message_t *msg);

#ifdef __cplusplus
}
#endif  // __cplusplus

#endif  // NEURO_SDK_H
