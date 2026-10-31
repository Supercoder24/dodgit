# Development Reference

## Build and Flash Code
First [Setup the Development Environment](#setup-the-development-environment)
1. Open the nRF Connect Extension
2. Click the Build button in the Actions section
3. Connect the board via the right/bottom micro-USB port
4. Click the Flash button in the Actions section

## Setup the Development Environment

On Windows:

1. Install Git
2. Install cmake and ninja using winget: `winget install Kitware.CMake Ninja-build.Ninja`
3. Install nRF Util from nordicsemi.com
* Create a new folder `C:\Program Files\Nordic Semiconductor`
* Move nrftools.exe there
* Add to path:
  * Right click This PC->Properties
  * Click "Advanced system settings"
  * Click "Environment Variables"
  * Click "Edit" on one of the "Path" variables (User variable if just installing for you, system variable if installing for all users)
  * Add `C:\Program Files\Nordic Semiconductor` via the "New" dialog
4. Install J-Link driver package from segger.com
5. Install VSCode (if not installed)
6. Install the nRF Connect for VS Code Extension Pack in VSCode
7. Install SDK via the button in the extension
  * Select Global
  * Select nRF Connect SDK (NOT Bare Metal)
9. Clone this git repo and open in VSCode
10. Run `git submodule update --init` to load the DW3xxx Driver Submodule
11. The example projects should all appear in the nRF Connect Extension
12. Click "Add Build Configuration"
13. Select `decawave_dwm3001cdk/nrf52833` from the "Board Target" selector, then "Generate and Build"

On Mac:

NOTE: Need at least 15 GB free or installations will fail

1. Install Homebrew (if not installed; run `homebrew --version` to check)
2. Install Git, cmake, and ninja (if not installed; run `[tool] --version` to check): `brew install git cmake ninja`
3. Install nRF Util from nordicsemi.com
  * Update permissions with `chmod +x nrfutil`
  * Move to `/usr/local/bin`
4. Install J-Link driver package from segger.com
5. Install VSCode (if not installed)
6. Install the nRF Connect for VS Code Extension Pack in VSCode
7. Install SDK via the button in the extension
  * Select Global
  * Select nRF Connect SDK (NOT Bare Metal)
9. Clone this git repo and open in VSCode
10. Run `git submodule update --init` to load the DW3xxx Driver Submodule
11. The example projects should all appear in the nRF Connect Extension
12. Click "Add Build Configuration"
13. Select `decawave_dwm3001cdk/nrf52833` from the "Board Target" selector, then "Generate and Build"


## First Time Project Initialization
1. In the Extension, click Create New Application
2. Load from sample
3. Search for blinky
4. Remove .git from the example project: `rm -rf examples/blinky/.git`