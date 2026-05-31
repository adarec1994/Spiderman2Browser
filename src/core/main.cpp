#include "App.h"

int main(int argc, char* argv[]) {
    App app;
    if (!app.init(1280, 800, "Spiderman 2 Asset Browser"))
        return 1;
    app.run();
    app.shutdown();
    return 0;
}
