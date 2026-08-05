# libneurosdk

C SDK for interacting with the Neuro API.

[![Test WebSocket Layer](../../actions/workflows/websocket.yml/badge.svg?branch=master)](../../actions/workflows/websocket.yml?query=branch%3Amaster)  
[![Test Neuro Protocol](../../actions/workflows/protocol.yml/badge.svg?branch=master)](../../actions/workflows/protocol.yml?query=branch%3Amaster)

## Documentation

Please check the header file.

### String Encoding

All C strings supplied to libneurosdk must be NUL-terminated UTF-8. Applications
using UTF-16, `wchar_t`, or another native string encoding must convert strings
to UTF-8 before calling the SDK. On Windows, for example, conversion from
UTF-16 should reject unpaired surrogates, such as by using
`WideCharToMultiByte` with `CP_UTF8` and `WC_ERR_INVALID_CHARS`.

Outgoing malformed UTF-8 is rejected with `NeuroSDK_InvalidMessage`. String
fields returned by the SDK are NUL-terminated UTF-8 and remain owned by the
message until `neurosdk_message_destroy()` is called.

## Contributing

Contributions are always welcome! Fork the repository and create pull requests
with your changes. Make sure you follow the formatting of the rest of the
codebase, which you can make sure you do by using `clang-format`.

## License

This project is licensed under the GPLv3 license. For more information, check
out the [LICENSE](LICENSE) file.
