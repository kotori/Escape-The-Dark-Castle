#ifndef ALLEGRO_STATICLINK
#define ALLEGRO_STATICLINK
#endif

#include <allegro5/allegro.h>
#include <allegro5/allegro_acodec.h>
#include <allegro5/allegro_audio.h>
#include <allegro5/allegro_image.h>
#include <allegro5/allegro_font.h>
#include <allegro5/allegro_primitives.h>
#include <allegro5/allegro_ttf.h>

#include <iostream>

#include "engine.hpp"

// Enforce standard modern video frame metrics
constexpr float FPS = 60.0f;
constexpr int SCREEN_WIDTH = 1024;
constexpr int SCREEN_HEIGHT = 768;

int main(int argc, char** argv) {
  // ==============================================================================
  // 1. MULTIMEDIA HARDWARE SUBSYSTEM INITIALIZATION
  // ==============================================================================
  if (!al_init()) {
    std::cerr << "Critical Error: Failed to initialize Allegro 5 core thread "
                 "framework."
              << std::endl;
    return -1;
  }

  if (!al_install_keyboard()) {
    std::cerr
        << "Critical Error: Peripheral IO fault. Keyboard installation failed."
        << std::endl;
    return -1;
  }

  if (!al_init_primitives_addon()) {
    std::cerr << "Critical Error: Primitive pipeline failed to initialize."
              << std::endl;
    return -1;
  }

  if (!al_init_image_addon()) {
    std::cerr << "Critical Error: Image subsystem driver failed to bind."
              << std::endl;
    return -1;
  }

  // Initialize font subroutines
  al_init_font_addon();
  if (!al_init_ttf_addon()) {
    std::cerr
        << "Critical Error: TrueType Font (TTF) layout engine failed to bind."
        << std::endl;
    return -1;
  }

  // Initialize audio mixer channels
  if (!al_install_audio()) {
    std::cerr
        << "Critical Error: Audio driver hardware layer missing or failed."
        << std::endl;
    return -1;
  }
  if (!al_init_acodec_addon()) {
    std::cerr << "Critical Error: Multiformat audio codec extensions missing."
              << std::endl;
    return -1;
  }
  if (!al_reserve_samples(6)) {
    std::cerr << "Critical Error: Mixer volume tracks failed to reserve "
                 "streaming allocations."
              << std::endl;
    return -1;
  }

  // ==============================================================================
  // 2. WINDOW CREATION & ASSET RESOLUTION
  // ==============================================================================
  ALLEGRO_DISPLAY* display = al_create_display(SCREEN_WIDTH, SCREEN_HEIGHT);
  if (!display) {
    std::cerr << "Critical Error: OpenGL/Direct3D context creation failed at "
                 "800x600 resolution."
              << std::endl;
    return -1;
  }
  al_set_window_title(display,
                      "Escape the Dark Castle - (Unofficial) Allegro5 Edition");

  ALLEGRO_PATH* path = al_get_standard_path(ALLEGRO_RESOURCES_PATH);
  if (path) {
    // Forces the operational framework to read assets relative to the binary's
    // true folder location
    al_change_directory(al_path_cstr(path, ALLEGRO_NATIVE_PATH_SEP));
    al_destroy_path(path);
  }

  // Load a fallback vector font from local development asset pathways
  ALLEGRO_FONT* game_font = al_load_ttf_font("data/fonts/DejaVuSans.ttf", 14, 0);
  ALLEGRO_FONT* header_font = al_load_ttf_font("data/fonts/Moria_DF.TTF", 36, 0);

  if (!game_font || !header_font) {
    std::cerr << "Warning: fonts missing. Falling back to "
                 "built-in system text styling."
              << std::endl;
    game_font = al_create_builtin_font();
    header_font = al_create_builtin_font();
  }

  // Load dice impact raw sample file
  ALLEGRO_SAMPLE* dice_sfx = al_load_sample("data/sfx/dice_roll.wav");
  if (!dice_sfx) {
    std::cerr << "Warning: Audio asset 'data/sfx/dice_roll.wav' missing. Game "
                 "will remain muted."
              << std::endl;
  }

  // Load dice impact raw sample file
  ALLEGRO_SAMPLE* treasure_sfx = al_load_sample("data/sfx/treasure.wav");
  if (!treasure_sfx) {
    std::cerr << "Warning: Audio asset 'data/sfx/treasure.wav' missing. Game "
                 "will remain muted."
              << std::endl;
  }

  ALLEGRO_SAMPLE* door_sfx = al_load_sample("data/sfx/door.wav");
  if (!door_sfx) {
    std::cerr << "Warning: Audio asset 'data/sfx/door.wav' missing. Game "
                 "will remain muted."
              << std::endl;
  }


  // Load a dedicated shield block impact audio file
  ALLEGRO_SAMPLE* shield_sfx = al_load_sample("data/sfx/shield_block.wav");
  if (!shield_sfx)
    std::cerr << "Warning: 'data/sfx/shield_block.wav' missing." << std::endl;


  ALLEGRO_BITMAP* icon = al_load_bitmap("data/gfx/icon.ico");
  if (!icon)
    std::cerr << "Warning: 'data/gfx/icon.ico' missing." << std::endl;


  // ==============================================================================
  // 3. EVENT ROUTER REGISTRATION
  // ==============================================================================
  ALLEGRO_TIMER* fps_timer = al_create_timer(1.0 / FPS);
  ALLEGRO_EVENT_QUEUE* event_queue = al_create_event_queue();

  if (!fps_timer || !event_queue) {
    std::cerr << "Critical Error: System clock allocation out of operational "
                 "memory frames."
              << std::endl;
    // Clean up immediately to prevent platform memory leakage
    if (game_font) al_destroy_font(game_font);
    if (dice_sfx) al_destroy_sample(dice_sfx);
    if (door_sfx) al_destroy_sample(door_sfx);
    if (shield_sfx) al_destroy_sample(shield_sfx);
    al_destroy_display(display);
    return -1;
  }

  // Connect device pipelines into a singular consolidated wait loop target
  al_register_event_source(event_queue, al_get_display_event_source(display));
  al_register_event_source(event_queue, al_get_timer_event_source(fps_timer));
  al_register_event_source(event_queue, al_get_keyboard_event_source());

  // ==============================================================================
  // 4. ENGINE INVOCATION & RUNTIME LOOP
  // ==============================================================================
  // Constructing the engine triggers SQLite setups automatically via
  // constructor logic hooks
  DarkCastleEngine engine;
  engine.assign_font(game_font);
  engine.assign_header_font(header_font);

  if (dice_sfx) engine.load_audio_sample(dice_sfx);      // Index 0
  if (shield_sfx) engine.load_audio_sample(shield_sfx);  // Index 1
  if (treasure_sfx) engine.load_audio_sample(treasure_sfx); // Index 2
  if (door_sfx) engine.load_audio_sample(door_sfx); // Index 3

  bool running = true;
  bool redraw_needed = true;

  // Fire the hardware clock pipeline
  al_start_timer(fps_timer);

  while (running) {
    ALLEGRO_EVENT event;
    al_wait_for_event(event_queue, &event);

    // Process frame ticking updates
    if (event.type == ALLEGRO_EVENT_TIMER) {
      redraw_needed = true;
    }
    // Handle physical user OS window kill target button adjustments
    else if (event.type == ALLEGRO_EVENT_DISPLAY_CLOSE) {
      running = false;
    }
    // Redirect physical peripheral key triggers into your engine's state
    // controller
    else if (event.type == ALLEGRO_EVENT_KEY_DOWN) {
      if (event.keyboard.keycode == ALLEGRO_KEY_ESCAPE) {
        running = false;
      } else {
        engine.process_input_event(event.keyboard.keycode);
      }
    }

    // Execution of the rendering stream passes once system frames step forward
    // safely
    if (redraw_needed && al_is_event_queue_empty(event_queue)) {
      redraw_needed = false;

      // Paint canvas back buffer field solid dark charcoal tone matching theme
      // properties
      al_clear_to_color(al_map_rgb(20, 20, 20));

      // Execute engine scene frame routers to display standard user interface
      // view modules
      engine.draw_scene_frame();

      // Swap the back buffer to the active screen view port area natively
      al_flip_display();
    }
  }

  // ==============================================================================
  // 5. SECURE DESTRUCTION LAYERS (RAII Enforcement Targets)
  // ==============================================================================
  al_stop_timer(fps_timer);
  al_destroy_timer(fps_timer);
  al_destroy_event_queue(event_queue);

  // --- FIX: DEALLOCATE CORE SOUND EFFECT SAMPLES SYSTEM BUFFERS ---
  // If your engine vector manages references, we explicitly free the original loaders
  if (dice_sfx) {
    al_destroy_sample(dice_sfx);
    dice_sfx = nullptr;
  }
  if (shield_sfx) {
    al_destroy_sample(shield_sfx);
    shield_sfx = nullptr;
  }
  if (treasure_sfx) {
    al_destroy_sample(treasure_sfx);
    treasure_sfx = nullptr;
  }
  if (door_sfx) {
    al_destroy_sample(door_sfx);
    door_sfx = nullptr;
  }

  // Free primary font resources if they weren't assigned to sub-destructors
  if (game_font) {
    al_destroy_font(game_font);
    game_font = nullptr;
  }

  // Note: The 'engine' destructor cleans up the internal variables naturally
  // on context destruction when main() completes its scope pass.
  al_destroy_display(display);

  std::cout << "Escape the Dark Castle cleanly exited." << std::endl;
  return 0;
}
