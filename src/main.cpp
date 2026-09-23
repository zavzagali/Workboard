#include "app.h"
#include "storage.h"

int main()
{
    auto app = App::create();
    workboard::load_state(*app);
    app->on_persist([app] { workboard::save_state(*app); });
    app->run();
    // Final safety net: the state is already saved on every change,
    // but make sure the very latest state hits the disk on exit.
    workboard::save_state(*app);
}
