#include "Server.hpp"

int main() {
    signal(SIGPIPE, SIG_IGN);
    Server server(10000);
    server.run();
}
