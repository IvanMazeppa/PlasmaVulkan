#include "core/Application.h"
#include <iostream>
#include <exception>
#include <string>
#include <sstream>
#include <glm/glm.hpp>

struct SimulationPreset {
    std::string name;
    std::string description;
    uint32_t particleCount;
    float gravity;
    float turbulence;
    float damping;
    float angularMomentum;
    float timeScale;
    bool accretionDisk;
    float blackHoleMass;
};

void printMenu() {
    std::cout << "\n========================================\n";
    std::cout << "     PLASMAVULKAN SIMULATION MENU\n";
    std::cout << "========================================\n\n";
    std::cout << "PRESETS:\n";
    std::cout << "  1. Stellar Formation (Default)\n";
    std::cout << "  2. Galaxy Spiral Arms\n";
    std::cout << "  3. Black Hole Accretion Disk\n";
    std::cout << "  4. Globular Cluster\n";
    std::cout << "  5. Protoplanetary Disk\n";
    std::cout << "  6. Galaxy Collision (Dual Gravity)\n";
    std::cout << "  7. Quasar Jets (High Energy)\n";
    std::cout << "  8. Custom Settings\n";
    std::cout << "  9. Start with Current Settings\n\n";
    std::cout << "Enter choice (1-9): ";
}

SimulationPreset getPreset(int choice) {
    switch(choice) {
        case 1: // Stellar Formation
            return {"Stellar Formation", "Star-forming nebula with gravitational collapse",
                    1000000, 0.6f, 0.0f, 0.999f, 1.0f, 1.0f, false, 1.0f};
        case 2: // Galaxy Spiral Arms
            return {"Galaxy Spiral", "Rotating galaxy with spiral arm formation",
                    1500000, 0.8f, 0.0f, 0.999f, 2.5f, 1.0f, true, 3.0f};
        case 3: // Black Hole Accretion
            return {"Black Hole Accretion", "Supermassive black hole with accretion disk",
                    1000000, 1.2f, 0.0f, 0.998f, 3.0f, 1.0f, true, 10.0f};
        case 4: // Globular Cluster
            return {"Globular Cluster", "Dense stellar cluster with complex dynamics",
                    2000000, 0.5f, 0.0f, 0.9995f, 0.5f, 1.0f, false, 0.5f};
        case 5: // Protoplanetary Disk
            return {"Protoplanetary Disk", "Young star system with planet formation",
                    750000, 0.7f, 0.02f, 0.997f, 2.0f, 1.0f, true, 1.0f};
        case 6: // Galaxy Collision
            return {"Galaxy Collision", "Two galaxies on collision course",
                    2000000, 0.8f, 0.0f, 0.999f, 2.0f, 1.5f, false, 5.0f};
        case 7: // Quasar Jets
            return {"Quasar Jets", "Active galactic nucleus with relativistic jets",
                    1500000, 1.5f, 0.05f, 0.996f, 4.0f, 1.2f, true, 20.0f};
        default: // Custom
            return {"Custom", "User-defined parameters",
                    1000000, 0.6f, 0.0f, 0.999f, 1.0f, 1.0f, false, 1.0f};
    }
}

void getCustomSettings(SimulationPreset& preset) {
    std::string input;
    
    std::cout << "\n--- CUSTOM SETTINGS ---\n";
    
    std::cout << "Particle count (100000-2000000) [1000000]: ";
    std::getline(std::cin, input);
    if (!input.empty()) {
        preset.particleCount = std::stoi(input);
        preset.particleCount = std::max(100000u, std::min(2000000u, preset.particleCount));
    }
    
    std::cout << "Gravity strength (0.0-2.0) [0.6]: ";
    std::getline(std::cin, input);
    if (!input.empty()) preset.gravity = std::stof(input);
    
    std::cout << "Turbulence (0.0-1.0) [0.0]: ";
    std::getline(std::cin, input);
    if (!input.empty()) preset.turbulence = std::stof(input);
    
    std::cout << "Damping (0.99-1.0) [0.999]: ";
    std::getline(std::cin, input);
    if (!input.empty()) preset.damping = std::stof(input);
    
    std::cout << "Angular momentum boost (0.0-5.0) [1.0]: ";
    std::getline(std::cin, input);
    if (!input.empty()) preset.angularMomentum = std::stof(input);
    
    std::cout << "Time scale (0.1-3.0) [1.0]: ";
    std::getline(std::cin, input);
    if (!input.empty()) preset.timeScale = std::stof(input);
    
    std::cout << "Enable accretion disk mode? (y/n) [n]: ";
    std::getline(std::cin, input);
    preset.accretionDisk = (!input.empty() && (input[0] == 'y' || input[0] == 'Y'));
    
    if (preset.accretionDisk) {
        std::cout << "Black hole mass (solar masses, 1-50) [1.0]: ";
        std::getline(std::cin, input);
        if (!input.empty()) preset.blackHoleMass = std::stof(input);
    }
}

int main(int argc, char* argv[]) {
    try {
        printMenu();
        
        std::string input;
        std::getline(std::cin, input);
        
        int choice = 9; // Default to current settings
        if (!input.empty()) {
            choice = std::stoi(input);
        }
        
        SimulationPreset preset = getPreset(choice);
        
        if (choice == 8) { // Custom settings
            getCustomSettings(preset);
        }
        
        if (choice != 9) {
            std::cout << "\n=== STARTING SIMULATION ===\n";
            std::cout << "Preset: " << preset.name << "\n";
            std::cout << preset.description << "\n";
            std::cout << "Particles: " << preset.particleCount << "\n";
            std::cout << "===========================\n\n";
        }
        
        std::cout << "Starting PlasmaVulkan application..." << std::endl;
        plasma::Application app("Plasma VK - Volumetric Particle Renderer", 1920, 1080);
        
        // Apply preset settings if not using defaults
        if (choice != 9) {
            app.setActiveParticleCount(preset.particleCount);
            app.setGravityStrength(preset.gravity);
            app.setTurbulenceStrength(preset.turbulence);
            app.setDampingFactor(preset.damping);
            app.setAngularMomentumBoost(preset.angularMomentum);
            app.setTimeScale(preset.timeScale);
            
            if (choice == 6) { // Galaxy Collision preset
                app.setDualGalaxyMode(true);
                app.setGravityCenter(glm::vec3(-6.0f, 0.0f, 1.0f));  // Galaxy A (much closer, slight offset)
                app.setGravityCenter2(glm::vec3(6.0f, 0.0f, -1.0f)); // Galaxy B (much closer, slight offset collision course)
                app.setBlackHoleMass(3.0f);   // Milky Way-sized black hole
                app.setBlackHoleMass2(4.0f);  // Slightly larger Andromeda-sized black hole
                
                // Position camera at collision center (midpoint between galaxies)
                glm::vec3 collisionCenter = glm::vec3(0.0f, 0.0f, 0.0f); // Midpoint between galaxies
                app.setCameraPosition(25.0f, collisionCenter); // Much closer to see both galaxies clearly
                
                std::cout << "Galaxy collision mode enabled - Milky Way vs Andromeda!" << std::endl;
                std::cout << "Initial separation: 12 units | Camera at 25 units distance" << std::endl;
            } else if (preset.accretionDisk) {
                app.setConstraintShape(4); // ACCRETION_DISK
                app.setBlackHoleMass(preset.blackHoleMass);
            }
        }
        
        std::cout << "Application created, starting main loop..." << std::endl;
        app.run();
        std::cout << "Application finished normally." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    
    return EXIT_SUCCESS;
}