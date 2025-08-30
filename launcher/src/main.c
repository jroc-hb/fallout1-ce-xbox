/*
 * Fallout CE Launcher for Xbox
 * =============================
 * This is a simple SDL2-based launcher menu for selecting various
 * configuration and startup options for Fallout CE on the original Xbox.
 */

 #include <stdio.h>
 #include <stdbool.h>
 #include <SDL.h>
 #include <SDL_image.h>
 #include <SDL_ttf.h>
 #include <hal/video.h>
 #include <xboxkrnl/xboxkrnl.h>
 #include <hal/debug.h>
 #include <nxdk/mount.h>
 #include <nxdk/path.h>
 #include <hal/xbox.h>
 #include <windows.h>
 
 // --- Menu Configuration ---
 #define MAIN_MENU_ITEM_COUNT 5
 #define VIDEO_MENU_ITEM_COUNT 5
 #define OTHER_MENU_ITEM_COUNT 4
 #define MAX_PATH 260
 
 // Function prototypes
 void configure_video();
 void return_to_main_menu();
 void other_options();
 void start_game();
 void delete_ini();
 void delete_cfg();
 void update_resolution_in_ini(int width, int height);
 void set_640x480();
 void set_720x480();
 void set_1280x720();
 void set_1920x1080();
 void toggle_FMV();
 void toggle_performance_overlay();
 void toggle_HDD_logging();
 void render_menu(SDL_Renderer* renderer, SDL_Texture* background, TTF_Font* font, char** menu_items, int item_count);
 
 // Menu Items
 const char* main_menu_items[MAIN_MENU_ITEM_COUNT] = {
     "Configure Video",
     "Other Options",
     "Reset f1_res.ini",
     "Reset fallout.cfg",
     "Start Game"
 };
 
 const char* video_menu_items[VIDEO_MENU_ITEM_COUNT] = {
     "640x480i/p",
     "720x480i/p",
     "1280x720p",
     "1920x1080i",
     "Back to Main Menu"
 };

 const char* other_menu_items[OTHER_MENU_ITEM_COUNT] = {
    "Disable FMVs",
    "Enable Performance Overlay",
    "Log Console to HDD (Debug Only)",
    "Back to Main Menu"
};
 
 // Menu Actions (Function pointers)
 void (*main_menu_actions[MAIN_MENU_ITEM_COUNT])() = {
     configure_video,
     other_options,
     delete_ini,
     delete_cfg,
     start_game
 };
 
 void (*video_menu_actions[VIDEO_MENU_ITEM_COUNT])() = {
     set_640x480,
     set_720x480,
     set_1280x720,
     set_1920x1080,
     return_to_main_menu
 };

 void (*other_menu_actions[OTHER_MENU_ITEM_COUNT])() = {
     toggle_FMV,
     toggle_performance_overlay,
     toggle_HDD_logging,
    return_to_main_menu
};
 
 // Active menu
 const char** current_menu_items = main_menu_items;
 int current_menu_count = MAIN_MENU_ITEM_COUNT;
 void (**current_menu_actions)() = main_menu_actions;
 int selected_index = 0;
 
 // Menu functions
 void configure_video() {
     DbgPrint("Video config placeholder\n");
     current_menu_items = video_menu_items;
     current_menu_count = VIDEO_MENU_ITEM_COUNT;
     current_menu_actions = video_menu_actions;
     selected_index = 0;
 }
 
 void return_to_main_menu() {
     current_menu_items = main_menu_items;
     current_menu_count = MAIN_MENU_ITEM_COUNT;
     current_menu_actions = main_menu_actions;
     selected_index = 0;
 }
 
 void other_options() {
    DbgPrint("Controls config placeholder\n");
    current_menu_items = other_menu_items;
    current_menu_count = OTHER_MENU_ITEM_COUNT;
    current_menu_actions = other_menu_actions;
    selected_index = 0;
 }
 
 void start_game() {
     // temp
     XLaunchXBE("\\Device\\CdRom0\\fallout1ce.xbe");
 
     if (GetFileAttributesA("\\Device\\CdRom0\\fallout1ce.xbe") != INVALID_FILE_ATTRIBUTES) {
         DbgPrint("Launching Fallout CE from disc\n");
         XLaunchXBE("\\Device\\CdRom0\\fallout1ce.xbe");
     } else if (GetFileAttributesA("D:\\fallout1ce.xbe") != INVALID_FILE_ATTRIBUTES) {
         DbgPrint("Launching Fallout CE from HDD\n");
         XLaunchXBE("D:\\fallout1ce.xbe");
     } else {
         DbgPrint("Error: fallout1ce.xbe not found!\n");
     };
 }
 
 void delete_ini() {
    // Remove the read-only attribute
    if (SetFileAttributesA("E:\\UDATA\\FALLOUT1\\f1_res.ini", FILE_ATTRIBUTE_READONLY)) {
        DbgPrint("Removed read only from f1_res.ini\n");
    }
     const char *ini_path = "E:\\UDATA\\FALLOUT1\\f1_res.ini";
     if (DeleteFileA(ini_path) == 0) {
         DbgPrint("Deleted INI file: %s\n", ini_path);
     } else {
         DbgPrint("Failed to delete INI file: %s\n", ini_path);
     }
 }
 
 void delete_cfg() {
     const char *cfg_path = "E:\\UDATA\\FALLOUT1\\fallout.cfg";
 
     // Check if the file exists
     if (GetFileAttributesA(cfg_path) == INVALID_FILE_ATTRIBUTES) {
         DbgPrint("E:\\UDATA\\FALLOUT1\\fallout.cfg does not exist, nothing to delete.\n");
         return;
     }
 
     // Try to delete the file
     if (DeleteFileA(cfg_path)) {
         DbgPrint("Deleted CFG file: %s\n", cfg_path);
     } else {
         // If it fails, get the error code
         DWORD error_code = GetLastError();
         if (error_code == ERROR_FILE_NOT_FOUND) {
             DbgPrint("CFG file not found: %s\n", cfg_path);
             return;
         } else if (error_code == ERROR_ACCESS_DENIED) {
             DbgPrint("Access denied when trying to delete CFG file: %s. Checking if file is read-only...\n", cfg_path);
 
             // Check if the file is read-only
             DWORD attrs = GetFileAttributesA(cfg_path);
             if (attrs != INVALID_FILE_ATTRIBUTES) {
                 // If the file has the read-only attribute, remove it
                 if (attrs & FILE_ATTRIBUTE_READONLY) {
                     DbgPrint("File is read-only. Removing read-only attribute...\n");
 
                     // Remove the read-only attribute
                     attrs &= ~FILE_ATTRIBUTE_READONLY;
                     if (SetFileAttributesA(cfg_path, attrs)) {
                         DbgPrint("Removed read-only attribute from file: %s\n", cfg_path);
 
                         // Try deleting again
                         if (DeleteFileA(cfg_path)) {
                             DbgPrint("Successfully deleted CFG file after removing read-only attribute: %s\n", cfg_path);
                         } else {
                             DbgPrint("Failed to delete CFG file even after removing read-only attribute: %s\n", cfg_path);
                         }
                     } else {
                         DbgPrint("Failed to remove read-only attribute from file: %s\n", cfg_path);
                     }
                 } else {
                     DbgPrint("File is not read-only, but access was denied. Error code: %lu\n", error_code);
                 }
             } else {
                 DbgPrint("Failed to get file attributes for: %s\n", cfg_path);
             }
             return;
         } else if (error_code == ERROR_SHARING_VIOLATION) {
             DbgPrint("Sharing violation when trying to delete CFG file: %s. Make sure no other process is using the file.\n", cfg_path);
             return;
         } else {
             DbgPrint("Failed to delete CFG file: %s. Error code: %lu\n", cfg_path, error_code);
         }
     }
 }
 
 void update_resolution_in_ini(int width, int height) {
    DbgPrint("Updating resolution in f1_res.ini to %dx%d\n", width, height);
    const char* path = "E:\\UDATA\\FALLOUT1\\f1_res.ini";
    // Remove the read-only attribute if set
    DWORD attrs = GetFileAttributesA(path);
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY)) {
        SetFileAttributesA(path, attrs & ~FILE_ATTRIBUTE_READONLY);
        DbgPrint("Removed read-only attribute from f1_res.ini\n");
    }

    FILE* file = fopen(path, "r");
    if (!file) {
        DbgPrint("Could not open f1_res.ini for reading.\n");
        return;
    }

    // Read file into memory
    static char lines[512][256];  // Supports up to 512 lines of 255 characters max
    int count = 0;
    int found_width = 0;
    int found_height = 0;

    while (fgets(lines[count], sizeof(lines[count]), file) && count < 512) {
        if (strncmp(lines[count], "SCR_WIDTH=", 10) == 0) {
            snprintf(lines[count], sizeof(lines[count]), "SCR_WIDTH=%d\n", width);
            found_width = 1;
        } else if (strncmp(lines[count], "SCR_HEIGHT=", 11) == 0) {
            snprintf(lines[count], sizeof(lines[count]), "SCR_HEIGHT=%d\n", height);
            found_height = 1;
        }
        count++;
    }

    fclose(file);

    // Append if not found
    if (!found_width && count < 512) {
        snprintf(lines[count++], sizeof(lines[0]), "SCR_WIDTH=%d\n", width);
    }
    if (!found_height && count < 512) {
        snprintf(lines[count++], sizeof(lines[0]), "SCR_HEIGHT=%d\n", height);
    }

    // Write updated contents
    file = fopen(path, "w");
    if (!file) {
        DbgPrint("Could not open f1_res.ini for writing.\n");
        return;
    }

    for (int i = 0; i < count; i++) {
        fputs(lines[i], file);
    }

    fclose(file);
    DbgPrint("Updated resolution in f1_res.ini to: %dx%d\n", width, height);
}
 
 void set_640x480() {
     update_resolution_in_ini(640, 480);
     return_to_main_menu();
 }
 
 void set_720x480() {
    DbgPrint("Setting resolution to 720x480\n");
     update_resolution_in_ini(720, 480);
     return_to_main_menu();
 }
 
 void set_1280x720() {
     update_resolution_in_ini(1280, 720);
     return_to_main_menu();
 }
 
 void set_1920x1080() {
     update_resolution_in_ini(1920, 1080);
     return_to_main_menu();
 }

 void toggle_FMV() {
     DbgPrint("FMVs \n");
 }

 void toggle_performance_overlay() {
     DbgPrint("Performance Overlay \n");
 }

 void toggle_HDD_logging() {
     DbgPrint("HDD Logging\n");
 }
 
 // Render the menu
 void render_menu(SDL_Renderer* renderer, SDL_Texture* background, TTF_Font* font, char** menu_items, int item_count) {
     SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
     SDL_RenderClear(renderer);
 
     // Draw background if available
     if (background) {
         SDL_RenderCopy(renderer, background, NULL, NULL);
     }
 
     int y_offset = 100;
     int spacing = 40;
 
     for (int i = 0; i < item_count; ++i) {
         SDL_Color color = (i == selected_index) ? (SDL_Color){255, 255, 0} : (SDL_Color){255, 255, 255};
         SDL_Surface* surface = TTF_RenderText_Solid(font, menu_items[i], color);
         if (!surface) continue;
 
         SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
 
         SDL_Rect dest = {
             .x = (640 - surface->w) / 2,
             .y = y_offset + i * spacing,
             .w = surface->w,
             .h = surface->h
         };
         SDL_RenderCopy(renderer, texture, NULL, &dest);
 
         SDL_FreeSurface(surface);
         SDL_DestroyTexture(texture);
     }
 
     SDL_RenderPresent(renderer);
 }
 
 int main(int argc, char* argv[]) {
     // NXDK: CMake doesn't automount the D: drive, so we need to do it manually
     if (!nxIsDriveMounted('D')) {
         CHAR targetPath[MAX_PATH];
         nxGetCurrentXbeNtPath(targetPath);
         char* slash = strrchr(targetPath, '\\');
         *(slash + 1) = '\0';
         BOOL ok = nxMountDrive('D', targetPath);
         DbgPrint("Mounted D: %s\n", ok ? "success" : "failed");
     }
 
     if (!nxIsDriveMounted('E')) {
         BOOL ok = nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\");
         DbgPrint("Mounted E: %s\n", ok ? "success" : "failed");
     }
 
     const int SCREEN_WIDTH = 640;
     const int SCREEN_HEIGHT = 480;
 
     XVideoSetMode(SCREEN_WIDTH, SCREEN_HEIGHT, 32, REFRESH_DEFAULT);
 
     if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
         DbgPrint("SDL Init Error: %s\n", SDL_GetError());
         return 1;
     }
 
     if (TTF_Init() != 0) {
         DbgPrint("TTF Init Error: %s\n", TTF_GetError());
         SDL_Quit();
         return 1;
     }
 
     SDL_Window* window = SDL_CreateWindow("Fallout CE Launcher",
         SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
         SCREEN_WIDTH, SCREEN_HEIGHT, 0);
     if (!window) {
         DbgPrint("Window creation failed: %s\n", SDL_GetError());
         return 1;
     }
 
     SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
     if (!renderer) {
         DbgPrint("Renderer creation failed: %s\n", SDL_GetError());
         SDL_DestroyWindow(window);
         return 1;
     }
 
     // Load background image
     SDL_Surface* bg_surface = SDL_LoadBMP("D:\\assets\\background.bmp");
     SDL_Texture* background = NULL;
     if (bg_surface) {
         background = SDL_CreateTextureFromSurface(renderer, bg_surface);
         SDL_FreeSurface(bg_surface);
     } else {
         DbgPrint("Failed to load background image: %s\n", SDL_GetError());
     }
 
     // Load font
     TTF_Font* font = TTF_OpenFont("D:\\assets\\r_fallouty.ttf", 24);
     if (!font) {
         DbgPrint("Failed to load font: %s\n", TTF_GetError());
         SDL_DestroyRenderer(renderer);
         SDL_DestroyWindow(window);
         TTF_Quit();
         SDL_Quit();
         return 1;
     }
 
     // Open game controller
     SDL_GameController* controller = NULL;
     for (int i = 0; i < SDL_NumJoysticks(); ++i) {
         if (SDL_IsGameController(i)) {
             controller = SDL_GameControllerOpen(i);
             break;
         }
     }
 
     // Input state
     bool running = true;
     bool up_pressed = false;
     bool down_pressed = false;
     bool a_pressed = false;
 
     while (running) {
         SDL_Event event;
         while (SDL_PollEvent(&event)) {
             if (event.type == SDL_QUIT) {
                 running = false;
             }
 
             if (event.type == SDL_CONTROLLERBUTTONDOWN) {
                 switch (event.cbutton.button) {
                     case SDL_CONTROLLER_BUTTON_DPAD_UP:
                         if (!up_pressed) {
                             selected_index = (selected_index - 1 + current_menu_count) % current_menu_count;
                             up_pressed = true;
                         }
                         break;
                     case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                         if (!down_pressed) {
                             selected_index = (selected_index + 1) % current_menu_count;
                             down_pressed = true;
                         }
                         break;
                     case SDL_CONTROLLER_BUTTON_A:
                         if (!a_pressed) {
                             current_menu_actions[selected_index]();
                             a_pressed = true;
                         }
                         break;
                 }
             }
 
             if (event.type == SDL_CONTROLLERBUTTONUP) {
                 switch (event.cbutton.button) {
                     case SDL_CONTROLLER_BUTTON_DPAD_UP:
                         up_pressed = false;
                         break;
                     case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                         down_pressed = false;
                         break;
                     case SDL_CONTROLLER_BUTTON_A:
                         a_pressed = false;
                         break;
                 }
             }
         }
 
         render_menu(renderer, background, font, current_menu_items, current_menu_count);
         SDL_Delay(16); // ~60 FPS
     }
 
     // Cleanup
     if (controller) SDL_GameControllerClose(controller);
     if (background) SDL_DestroyTexture(background);
     SDL_DestroyRenderer(renderer);
     SDL_DestroyWindow(window);
     TTF_CloseFont(font);
     TTF_Quit();
     SDL_Quit();
 
     return 0;
 }
 