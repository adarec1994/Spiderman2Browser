#include "App.h"
#include <iostream>

int main(int argc, char* argv[]) {
    App app;
    if (!app.init(1280, 800, "XBX Model Viewer"))
        return 1;
    app.run();
    app.shutdown();
    return 0;
}
