#include "core/Application.h"
#include <iostream>
#include <exception>

int main(int argc, char* argv[]) {
    try {
        std::cout << "Starting PlasmaVulkan application..." << std::endl;
        plasma::Application app("Plasma VK - Volumetric Particle Renderer", 1920, 1080);
        std::cout << "Application created, starting main loop..." << std::endl;
        app.run();
        std::cout << "Application finished normally." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    
    return EXIT_SUCCESS;
}