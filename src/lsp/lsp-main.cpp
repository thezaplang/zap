#include "lsp.hpp"
#include <cstdio>

using namespace zap::lsp;

int main(){
  /// Disable buffering, can cut messages.
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  /// Server itself.
  Server server;


  server.logMessage(Server::MessageType::Info, "Server starting...");
  
  server.send();

  std::string line;

  while(true){
    /// Process the client's message.
    std::string strMessage = server.processMessage(line);
  }

}