#pragma once

#include <string>

#include <webapp/uvcpp_web_app.h>

namespace molink {

class server_runtime;

void register_admin_routes(uvcpp::uvcpp_web_app& app, server_runtime& runtime,
                           const std::string& ui_root);

}  // namespace molink
