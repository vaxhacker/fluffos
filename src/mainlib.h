#include <string>

// Init everything driver needs.
struct event_base* init_main(std::string_view config_file);
void setup_signal_handlers();
// Hand a pending SIGTERM/SIGINT to master::signal_shutdown(), once; called
// from the game tick. Without that apply, or when it errors, the driver takes
// the crash path (master::crash(), then abort) as it did from the handler.
void dispatch_shutdown_signal();
std::string get_argument(unsigned int pos, int argc, char** argv);
