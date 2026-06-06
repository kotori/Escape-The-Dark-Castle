#ifndef ENGINE_HPP
#define ENGINE_HPP

#include <allegro5/allegro.h>
#include <allegro5/allegro_audio.h>
#include <allegro5/allegro_font.h>

#include <map>
#include <string>
#include <vector>

#include "types.hpp"

class DarkCastleEngine {
 private:
  enum SceneState { SCENE_MAIN_MENU, SCENE_CHARACTER_SELECT, SCENE_GAMEPLAY };
  SceneState current_scene = SCENE_MAIN_MENU;

  Prisoner hero;
  Prisoner companion;
  std::vector<ItemCard> hero_inventory;
  std::vector<ItemCard> companion_inventory;

  ChapterCard active_card;
  std::vector<std::string> live_enemy_shields;

  std::vector<Prisoner> prisoner_db_pool;
  std::vector<ChapterCard> master_chapter_pool;
  std::vector<ChapterCard> current_game_deck;
  ChapterCard final_boss_card;
  std::vector<ItemCard> master_treasure_pool;
  std::vector<ALLEGRO_SAMPLE*> bone_dice_samples;

  // Stores ongoing combat, choice, or loot text notifications
  std::vector<std::string> game_log_history;
  void add_to_game_log(const std::string& message);

  // Loot Distribution Overlay Variables
  bool is_awaiting_loot_choice = false;
  ItemCard pending_looted_item;
  int loot_target_player_cursor = 0;  // 0 = Hero, 1 = Companion

  void render_loot_distribution_overlay();

  int challenge_attempts_left = 0;
  int challenge_successes_gained = 0;
  int resting_character = 0;
  bool game_over = false;
  bool player_won = false;

  bool is_awaiting_prompt_choice = false;
  int prompt_target_character = 0;
  size_t prompt_shield_vector_index = 0;
  int pending_monster_damage = 0;

  bool owns_font_resource = false;
  void load_all_item_textures();

  // UI & Cursor Navigation Trackers
  int menu_cursor_index = 0;
  int select_phase = 0;
  int character_cursor_index = 0;

  int hero_inv_cursor = 0;
  int companion_inv_cursor = 0;

  int current_acting_player_id = 1; // 1 = Hero (P1), 2 = Companion (P2)

  bool is_awaiting_door_open = true;
  int current_door_opener_id = 1; // 1 = Hero, 2 = Companion
  bool room_is_completed = false; // Prevents moving on until the room requirements

  // Interactive Reroll State Machine
  bool is_awaiting_reroll_choice = false;
  int rerolls_remaining_this_turn = 0;
  DieFace active_rolled_face = DieFace::BLANK;

  bool is_awaiting_greatsword_choice = false;
  int greatsword_removals_left = 0;
  int greatsword_shield_cursor = 0;

  ALLEGRO_FONT* local_font = nullptr; // default font
  ALLEGRO_FONT* menu_title_font = nullptr; // Dedicated large font asset for headers

  // ==============================================================================
  // CRITICAL ARRAYS AND REPOSITORIES CORRECTIONS
  // ==============================================================================
  // FIX: Sized explicitly to 6 to safely house all SQLite prisoner profiles
  ALLEGRO_BITMAP* prisoner_textures[6] = {nullptr, nullptr, nullptr,
                                          nullptr, nullptr, nullptr};

  // FIX: Added tracking sheets for your custom 24x24 and 96x96 dice assets
  ALLEGRO_BITMAP* mini_die_textures[5] = {nullptr, nullptr, nullptr, nullptr,
                                          nullptr};
  ALLEGRO_BITMAP* large_die_textures[5] = {nullptr, nullptr, nullptr, nullptr,
                                           nullptr};

  std::map<std::string, ALLEGRO_BITMAP*> item_icon_registry;
  ALLEGRO_BITMAP* room_placeholder_image = nullptr;
  ALLEGRO_BITMAP* deck_back_texture = nullptr;

  bool hero_blocked_this_round = false;
  bool comp_blocked_this_round = false;

  // Private Engine Subroutines
  bool give_item_to_character(std::vector<ItemCard>& inventory,
                              ItemCard new_item);
  bool check_for_immunity_prompt(std::vector<ItemCard>& inventory,
                                 int raw_damage, int character_id);
  void render_main_title_screen();
  void render_character_selection_menu();
  void render_active_tabletop_loop();
  void draw_ui_overlay_prompts();
  void setup_active_challenge(ChapterCard card);

  void render_game_over_lose_screen();
  void render_game_victory_screen();

  bool consume_item_from_slot(std::vector<ItemCard>& inventory,
                              Prisoner& target, size_t slot_idx);
  bool discard_item_from_slot(std::vector<ItemCard>& inventory,
                              const Prisoner& target, size_t slot_idx);

  // Visual Dice Animation Tickers
  bool is_dice_animating = false;
  int dice_anim_frame_counter = 0;

  void play_context_dice_sfx(DieFace finalized_face);
  void play_treasure_award_sfx();

  char p1_rolling_flicker_char = 'M';
  char p2_rolling_flicker_char = 'W';

  std::string hero_last_roll_str = "";      // P1 last roll string container
  std::string companion_last_roll_str = ""; // P2 last roll string container

 public:
  DarkCastleEngine();
  ~DarkCastleEngine();

  void render_victory_splash_overlay();
  void render_loss_splash_overlay();
  bool load_game_database_from_sqlite();
  bool load_items_from_sqlite();
  bool load_chapters_from_sqlite();
  void generate_random_game_deck();
  void advance_to_next_chapter_card();
  void distribute_challenge_reward(const ChapterCard& card);
  void execute_challenge_roll_step();
  void execute_combat_round();
  void process_input_event(int keycode);
  void assign_font(ALLEGRO_FONT* shared_font);
  void assign_header_font(ALLEGRO_FONT* shared_font);
  void load_audio_sample(ALLEGRO_SAMPLE* sample);
  void play_door_open_sfx();
  void draw_scene_frame();
};

#endif
