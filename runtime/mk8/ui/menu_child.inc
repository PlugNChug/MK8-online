// A guest page without a prepared layout can still open the host Menu.
static std::atomic<bool> mk8_open_overlay{false};
extern "C" void mk8_menu_request_overlay(void) { mk8_open_overlay=true; }
