# PlasmaVulkan Key Bindings Reference

## Core Controls
- **ESC** - Exit application
- **SPACE** - Toggle SPH (Smoothed Particle Hydrodynamics) fluid mode
- **H** - Display help and current physics parameters

## Camera Controls
- **Mouse Drag** - Rotate camera (orbit around center)
- **Mouse Scroll** - Zoom in/out
- **Mouse Right Click + Drag** - Pan camera target

## Physics Parameters
- **G / Shift+G** - Increase/Decrease gravity strength
- **T / Shift+T** - Increase/Decrease turbulence strength  
- **D / Shift+D** - Increase/Decrease damping factor
- **P / Shift+P** - Increase/Decrease active particle count

## Advanced Physics
- **I / Shift+I** - Increase/Decrease energy injection
- **U / Shift+U** - Increase/Decrease time scale
- **J / Shift+J** - Increase/Decrease angular momentum boost
- **K / Shift+K** - Increase/Decrease disk radius

## Black Hole Controls
- **M** - Increase black hole mass (affects jet strength)
- **Shift+M** - Decrease black hole mass
- **N** - Toggle repulsive gravity mode
- **B** - Toggle bloom post-processing effect

## Viscosity Controls
- **V / Shift+V** - Increase/Decrease alpha viscosity (for accretion disks)
- **C / Shift+C** - Increase/Decrease critical density for accretion
- **X / Shift+X** - Increase/Decrease density scale

## Shape Constraints
- **F1** - No constraint (open space)
- **F2** - Sphere constraint
- **F3** - Disc constraint
- **F4** - Torus constraint
- **F5** - Accretion disk constraint
- **W** - Toggle wireframe visualization for constraints
- **[ / ]** - Adjust constraint radius
- **- / =** - Adjust constraint thickness

## Camera Presets
- **Arrow Keys** - Fine camera position adjustment
- **Page Up/Down** - Adjust camera distance

## SPH Mode Controls (when active)
- **1** - Display performance tip for SPH
- **2** - Reduce pressure for less explosive behavior
- **3** - Increase viscosity for more fluid-like behavior
- **Q / E** - SPH-specific parameter adjustments

## Display Options
- **O** - Toggle on-screen display (OSD) status updates
- **V** - Toggle standard volumetric rendering mode (3D plasma glow, 128³ voxels, real-time performance)
- **Ctrl+V** - START CINEMATIC RECORDING MODE (400³ voxels, 512 ray steps, auto-records)
- **R** - Reset simulation
- **Shift+R** - Initialize particles in disk formation

## Recording System
- **F** - Toggle recording (start/stop 5-second loop at 60fps)
- **Shift+F** - High quality recording (2x particles, volumetrics, enhanced bloom)
- **L** - Toggle loop mode (resets simulation when recording starts)

## Relativistic Jets
- **A** - Toggle relativistic jets on/off

## Available Keys for New Features
The following keys are currently unbound and available:
- **S** - Available
- **Y** - Available
- **Z** - Available
- **F6-F12** - Available for additional presets

## Notes
- Most physics parameters can be adjusted with key repeat (hold key down)
- Shift modifier typically reverses the operation (increase vs decrease)
- Visual indicators appear in window title for active modes
- Console output provides feedback for parameter changes