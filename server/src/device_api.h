#pragma once

#include <webapp/uvcpp_web_app.h>

namespace molink {

class server_runtime;

void register_device_routes(uvcpp::uvcpp_web_app& app, server_runtime& runtime);

}  // namespace molink
