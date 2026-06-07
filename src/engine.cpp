#ifndef ALLEGRO_STATICLINK
#define ALLEGRO_STATICLINK
#endif

#include "engine.hpp"

#include <allegro5/allegro_primitives.h>
#include <sqlite3.h>

#include <algorithm>
#include <ctime>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>

// Safe string utility to protect engine from SQLite NULL data mutations
static std::string safe_sqlite_string(sqlite3_stmt* stmt, int column_idx) {
  const unsigned char* text = sqlite3_column_text(stmt, column_idx);
  return text ? reinterpret_cast<const char*>(text) : "";
}

// ==============================================================================
// CORE ENGINE LIFECYCLE MANAGEMENT (RAII Enforced Structures)
// ==============================================================================
DarkCastleEngine::DarkCastleEngine() {
  load_game_database_from_sqlite();
  load_items_from_sqlite();
  load_chapters_from_sqlite();
  //generate_random_game_deck();

  // Automatically register visual textures from file paths before runtime loops
  // launch
  load_all_item_textures();
  p1_char_cursor = 0;  // Player 1 begins safely on index slot 0 (The Abbot)
  p2_char_cursor = 1;  // Player 2 begins safely on index slot 1 (The Tailor)
  current_scene = SCENE_MAIN_MENU;
  menu_cursor_index = 0;
  select_phase = 0;
  character_cursor_index = 0;
  hero_inv_cursor = 0;
  companion_inv_cursor = 0;
  resting_character = 0;
  game_over = false;
  player_won = false;
  is_awaiting_prompt_choice = false;
  is_awaiting_reroll_choice = false;
  is_awaiting_greatsword_choice = false;
}

DarkCastleEngine::~DarkCastleEngine() {
  if (owns_font_resource && local_font) {
    al_destroy_font(local_font);
    local_font = nullptr;
  }

  if (menu_title_font) {
    al_destroy_font(menu_title_font);
    menu_title_font = nullptr;
  }


  for (ALLEGRO_SAMPLE* sample : bone_dice_samples) {
    if (sample) al_destroy_sample(sample);
  }
  bone_dice_samples.clear();

  // Free all memory mapped item icon bitmap structures safely
  for (auto const& [name, bitmap] : item_icon_registry) {
    if (bitmap) al_destroy_bitmap(bitmap);
  }
  item_icon_registry.clear();
}

void DarkCastleEngine::assign_font(ALLEGRO_FONT* shared_font) {
  local_font = shared_font;
}

void DarkCastleEngine::assign_header_font(ALLEGRO_FONT* shared_font) {
  menu_title_font = shared_font;
}

void DarkCastleEngine::load_audio_sample(ALLEGRO_SAMPLE* sample) {
  if (sample) bone_dice_samples.push_back(sample);
}

void DarkCastleEngine::add_to_game_log(const std::string& message) {
  game_log_history.push_back(message);
  std::cout << "[LOG] " << message << std::endl;
  if (game_log_history.size() > 5) {
    game_log_history.erase(game_log_history.begin());
  }
}

// ==============================================================================
// INVENTORY & UTILITY ENGINE SUBROUTINES
// ==============================================================================
bool DarkCastleEngine::give_item_to_character(std::vector<ItemCard>& inventory,
                                              ItemCard new_item) {
  std::string recipient_name = "Someone";
  if (&inventory == &hero_inventory)
    recipient_name = hero.name;
  else if (&inventory == &companion_inventory)
    recipient_name = companion.name;

  if (inventory.empty()) {
    inventory.push_back(new_item);
    add_to_game_log(recipient_name + " obtained " + new_item.name + "!");
    return true;
  }
  if (inventory.size() >= 2) return false;
  if (inventory.size() == 1) {
    ItemCard held_item = inventory.at(0);
    if (held_item.is_2handed || new_item.is_2handed) return false;
    inventory.push_back(new_item);
    add_to_game_log(recipient_name + " obtained " + new_item.name + "!");
    return true;
  }
  return false;
}

bool DarkCastleEngine::consume_item_from_slot(std::vector<ItemCard>& inventory,
                                              Prisoner& target, size_t slot_idx) {
  if (slot_idx >= inventory.size()) return false;
  if (game_over) return false;

  const ItemCard& item = inventory.at(slot_idx);

  // ==============================================================================
  // PASS 1: FOOD PROVISIONS CONSUMPTION (Single-use healing item cards)
  // ==============================================================================
  if (item.is_consumable && item.heal_amount > 0) {
    int old_hp = target.current_hp;
    target.current_hp = std::min(target.current_hp + item.heal_amount, target.max_hp);
    
    // ==============================================================================
    // SEED FLOATING GREEN TEXT ANIMATION POOLS
    // ==============================================================================
    // Detect which character inventory slot triggered the consumption logic
    if (target.name == hero.name) {
      p1_heal_flash_timer = 45; // Start a 45-frame animation loop
      p1_heal_flash_amount = item.heal_amount;
    } else {
      p2_heal_flash_timer = 45;
      p2_heal_flash_amount = item.heal_amount;
    }

    add_to_game_log(target.name + " consumed " + item.name + ". Healed +" + 
                    std::to_string(item.heal_amount) + " HP (" + 
                    std::to_string(old_hp) + "->" + std::to_string(target.current_hp) + ")");
    
    inventory.erase(inventory.begin() + slot_idx);
    return true;
  }


  // ==============================================================================
  // PASS 2: TACTICAL POTION BYPASS BREWS (Skips rolling to break a shield card)
  // ==============================================================================
  if (item.is_consumable && !is_awaiting_door_open && 
     (active_card.type == CardType::STANDARD_COMBAT || active_card.type == CardType::BOSS_BATTLE)) {
    
    std::string target_shield = "";
    if (item.special_action_type == "POTION_BYPASS_MIGHT")   target_shield = "Might";
    if (item.special_action_type == "POTION_BYPASS_CUNNING") target_shield = "Cunning";
    if (item.special_action_type == "POTION_BYPASS_WISDOM")  target_shield = "Wisdom";

    if (!target_shield.empty()) {
      auto it = std::find(live_enemy_shields.begin(), live_enemy_shields.end(), target_shield);
      if (it != live_enemy_shields.end()) {
        live_enemy_shields.erase(it);
        
        add_to_game_log(target.name + " smashed " + item.name + "! Auto-cleared one " + target_shield + " shield.");
        inventory.erase(inventory.begin() + slot_idx);

        if (live_enemy_shields.empty()) {
          add_to_game_log("Enemies completely defeated! Room cleared.");
          room_is_completed = true;
        }
        return true;
      } else {
        add_to_game_log("Action Refused: No active " + target_shield + " shield exists to bypass!");
        return false;
      }
    }
  }

  return false;
}

bool DarkCastleEngine::discard_item_from_slot(std::vector<ItemCard>& inventory,
                                              const Prisoner& target,
                                              size_t slot_idx) {
  if (slot_idx >= inventory.size()) return false;
  add_to_game_log(target.name + " dropped " + inventory.at(slot_idx).name);
  inventory.erase(inventory.begin() + slot_idx);
  return true;
}

bool DarkCastleEngine::check_for_immunity_prompt(
    std::vector<ItemCard>& inventory, int raw_damage, int character_id) {
  if (raw_damage <= 0) return false;
  for (size_t i = 0; i < inventory.size(); ++i) {
    if (inventory.at(i).is_immunity_shield) {
      is_awaiting_prompt_choice = true;
      prompt_target_character = character_id;
      prompt_shield_vector_index = i;
      pending_monster_damage = raw_damage;
      return true;
    }
  }
  return false;
}

// ==============================================================================
// SQLITE FILE PARSING INTEGRATIONS
// ==============================================================================
bool DarkCastleEngine::load_game_database_from_sqlite() {
  sqlite3* db = nullptr;
  if (sqlite3_open("data/game.db", &db) != SQLITE_OK) return false;
  const char* schema =
      "CREATE TABLE IF NOT EXISTS prisoners (id INTEGER PRIMARY KEY "
      "AUTOINCREMENT, name TEXT NOT NULL, max_hp INTEGER, dice_faces TEXT);";
  sqlite3_exec(db, schema, nullptr, nullptr, nullptr);

  sqlite3_stmt* stmt = nullptr;
  int count = 0;
  if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM prisoners;", -1, &stmt,
                         nullptr) == SQLITE_OK) {
    if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
  }
  if (count == 0) {
    const char* seed =
        "INSERT INTO prisoners (name, max_hp, dice_faces) VALUES "
        "('The Abbot', 18, 'W W W W M M M C'),"
        "('The Tailor', 18, 'C C C C W W W M'),"
        "('The Smith', 18, 'M M M M W W W C'),"
        "('The Cook', 18, 'M M M M C C C W'),"
        "('The Tanner', 18, 'C C C C M M M W'),"
        "('The Miller', 18, 'W W W W C C C M');";
    sqlite3_exec(db, seed, nullptr, nullptr, nullptr);
  }

  const char* query = "SELECT name, max_hp, dice_faces FROM prisoners;";
  prisoner_db_pool.clear();
  if (sqlite3_prepare_v2(db, query, -1, &stmt, nullptr) == SQLITE_OK) {
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      Prisoner p;
      p.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
      p.max_hp = sqlite3_column_int(stmt, 1);
      p.current_hp = p.max_hp;
      std::string raw_dice = safe_sqlite_string(stmt, 2);
      std::stringstream ss(raw_dice);
      std::string t;
      while (ss >> t) {
        if (t == "M")
          p.dice_faces.push_back(DieFace::MIGHT);
        else if (t == "C")
          p.dice_faces.push_back(DieFace::CUNNING);
        else if (t == "W")
          p.dice_faces.push_back(DieFace::WISDOM);
        else if (t == "S")
          p.dice_faces.push_back(DieFace::SHIELD);
        else
          p.dice_faces.push_back(DieFace::BLANK);
      }
      prisoner_db_pool.push_back(p);
    }
    sqlite3_finalize(stmt);
  }
  sqlite3_close(db);
  return true;
}

bool DarkCastleEngine::load_items_from_sqlite() {
  sqlite3* db = nullptr;
  if (sqlite3_open("data/game.db", &db) != SQLITE_OK) {
    std::cerr
        << "Critical Error: Failed to open SQLite context for item loading."
        << std::endl;
    return false;
  }

  // 1. Build the items database schema matching your structural requirements
  const char* schema =
      "CREATE TABLE IF NOT EXISTS items ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT, "
      "name TEXT NOT NULL, "
      "description TEXT, "
      "is_2handed INT DEFAULT 0, "
      "is_consumable INT DEFAULT 0, "
      "is_weapon INT DEFAULT 0, "
      "is_immunity_shield INT DEFAULT 0, "
      "attack_amount INT DEFAULT 0, "
      "heal_amount INT DEFAULT 0, "
      "special_action_type TEXT DEFAULT 'NONE');";

  sqlite3_exec(db, schema, nullptr, nullptr, nullptr);

  // 2. Check if the treasure database has entries, seed it if empty
  sqlite3_stmt* stmt = nullptr;
  int count = 0;
  if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM items;", -1, &stmt,
                         nullptr) == SQLITE_OK) {
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
  }

  if (count == 0) {
    // Seed examples matching your item asset names and state logic routes
    const char* seed_query =
        "INSERT INTO items (name, description, is_2handed, is_consumable, "
        "is_weapon, is_immunity_shield, attack_amount, heal_amount, "
        "special_action_type) VALUES "
        "('Greatsword', 'Oversized heavy blade. Grants DOUBLE REROLL "
        "features.', 1, 0, 1, 0, 2, 0, 'DOUBLE_REROLL'),"
        "('Heavy Shield', 'Iron fortress barrier. Discard to block incoming "
        "round damage entirely.', 1, 0, 0, 1, 4, 0, 'HEAVY_SHIELD_ROUND'),"
        "('Brew of Might', 'Drinking this magical elixir smashes a Might block "
        "instantly.', 0, 1, 0, 0, 0, 0, 'POTION_BYPASS_MIGHT'),"
        "('Axe', 'Heavy iron hatchet. Deals double damage against Might "
        "targets.', 0, 0, 1, 0, 1, 0, 'DOUBLE_MIGHT'),"
        "('Cooked Meat', 'Salted rations. Eat to recover 4 health points.', 0, "
        "1, 0, 0, 0, 4, 'HEAL_FOOD'),"
        "('Plate Armor', 'Heavy passive metal plates.', 0, 0, 0, 0, 2, 0, "
        "'PERMANENT_PLATE_ARMOR'),"
        "('Helmet', 'Protective headgear. Shatters on heavy impacts.', 0, 0, "
        "0, 0, 3, 0, 'THRESHOLD_HELMET_BLOCK');";

    sqlite3_exec(db, seed_query, nullptr, nullptr, nullptr);
  }

  // 3. Prepare select parameters targeting your header's master_treasure_pool
  // vector
  master_treasure_pool.clear();
  const char* select_query =
      "SELECT id, name, description, is_2handed, is_consumable, is_weapon, "
      "is_immunity_shield, attack_amount, heal_amount, special_action_type "
      "FROM items;";

  if (sqlite3_prepare_v2(db, select_query, -1, &stmt, nullptr) == SQLITE_OK) {
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      ItemCard item;

      item.id = sqlite3_column_int(stmt, 0);
      item.name = safe_sqlite_string(stmt, 1);
      item.description = safe_sqlite_string(stmt, 2);

      // Map integer storage metrics safely back to C++ booleans
      item.is_2handed = (sqlite3_column_int(stmt, 3) != 0);
      item.is_consumable = (sqlite3_column_int(stmt, 4) != 0);
      item.is_weapon = (sqlite3_column_int(stmt, 5) != 0);
      item.is_immunity_shield = (sqlite3_column_int(stmt, 6) != 0);

      item.attack_amount = sqlite3_column_int(stmt, 7);
      item.heal_amount = sqlite3_column_int(stmt, 8);
      item.special_action_type = safe_sqlite_string(stmt, 9);

      // Stows item safely inside your randomized treasure generator array
      master_treasure_pool.push_back(item);
    }
    sqlite3_finalize(stmt);
  } else {
    std::cerr << "Warning: Item query failed: " << sqlite3_errmsg(db)
              << std::endl;
    sqlite3_close(db);
    return false;
  }

  sqlite3_close(db);
  return true;
}

// Helper lambda to cleanly map shorthand characters to DieFace enum tags
auto map_char_to_die_face = [](char c) -> DieFace {
  switch (c) {
    case 'M':
      return DieFace::MIGHT;
    case 'C':
      return DieFace::CUNNING;
    case 'W':
      return DieFace::WISDOM;
    case 'S':
      return DieFace::SHIELD;
    default:
      return DieFace::BLANK;
  }
};

// Helper subroutine to parse space-separated shorthand strings (e.g., "M M C
// S") into enum vectors
auto parse_shield_string = [](const std::string& raw_str,
                              std::vector<DieFace>& target_vector) {
  target_vector.clear();
  std::stringstream ss(raw_str);
  char token;
  while (ss >> token) {
    target_vector.push_back(map_char_to_die_face(token));
  }
};

bool DarkCastleEngine::load_chapters_from_sqlite() {
  sqlite3* db = nullptr;
  if (sqlite3_open("data/game.db", &db) != SQLITE_OK) {
    std::cerr << "Critical Error: Failed to open SQLite context for chapter loading." << std::endl;
    return false;
  }

  // Ensure table framework structurally matches your explicit schema definition script exactly
  const char* schema =
      "CREATE TABLE IF NOT EXISTS chapters ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT, title TEXT NOT NULL, flavor_text TEXT, "
      "card_type INT NOT NULL DEFAULT 0, attack_damage INT DEFAULT 2, enemy_shields TEXT, "
      "choice_1_text TEXT, choice_1_damage INT DEFAULT 0, choice_1_shields TEXT, "
      "choice_2_text TEXT, choice_2_damage INT DEFAULT 0, choice_2_shields TEXT, "
      "target_attribute TEXT, required_successes INT DEFAULT 0, max_attempts INT DEFAULT 0, "
      "damage_per_roll INT DEFAULT 0, failure_damage INT DEFAULT 0, trap_damage INT DEFAULT 0, "
      "has_reward INT DEFAULT 0, reward_type TEXT DEFAULT 'NONE', reward_amount INT DEFAULT 0);";
  sqlite3_exec(db, schema, nullptr, nullptr, nullptr);

  // Seed fallback examples if the user table is completely empty at initialization
  sqlite3_stmt* stmt = nullptr;
  int count = 0;
  if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM chapters;", -1, &stmt, nullptr) == SQLITE_OK) {
    if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
  }

  if (count == 0) {
    std::cout << "[DB] Chapters table empty. Attempting file-driven seed sequence..." << std::endl;
    
    // Read the explicit external backup script from your localized data path
    std::ifstream seed_file("data/data.seed");
    if (!seed_file.is_open()) {
      std::cerr << "Warning: 'data/data.seed' could not be read. Generating minimum fallback rows..." << std::endl;
      // Fallback fallback if your disk deployment configuration is corrupted
      const char* default_seed =
          "INSERT INTO chapters (title, flavor_text, card_type, attack_damage, enemy_shields) VALUES "
          "('Dungeon Cell Guards', 'Two armored orcs block the dim corridor.', 1, 2, 'M C W S');";
      sqlite3_exec(db, default_seed, nullptr, nullptr, nullptr);
    } else {
      std::stringstream buffer;
      buffer << seed_file.rdbuf();
      seed_file.close();

      std::string full_sql = buffer.str();
      std::stringstream sql_stream(full_sql);
      std::string single_statement;
      
      // Parse individual semicolon-terminated SQL commands to avoid bulk transaction parsing glitches
      while (std::getline(sql_stream, single_statement, ';')) {
        // Trim trailing/leading whitespace or empty carriage lines
        if (single_statement.find_first_not_of(" \t\n\r") == std::string::npos) {
          continue; 
        }
        
        char* errmsg = nullptr;
        if (sqlite3_exec(db, single_statement.c_str(), nullptr, nullptr, &errmsg) != SQLITE_OK) {
          std::cerr << "Database Seed Execution Error: " << (errmsg ? errmsg : "Unknown Fault") << std::endl;
          if (errmsg) sqlite3_free(errmsg);
        }
      }
      std::cout << "[DB] External data.seed parsed and applied successfully." << std::endl;
    }
  }

  // Query specifically pulls "card_type" instead of "type" to align with your schema script
  const char* query = 
      "SELECT id, title, flavor_text, card_type, attack_damage, enemy_shields, "
      "choice_1_text, choice_1_damage, choice_1_shields, choice_2_text, choice_2_damage, choice_2_shields, "
      "target_attribute, required_successes, max_attempts, damage_per_roll, failure_damage, trap_damage, "
      "has_reward, reward_type, reward_amount FROM chapters;";

  master_chapter_pool.clear();

  if (sqlite3_prepare_v2(db, query, -1, &stmt, nullptr) == SQLITE_OK) {
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      ChapterCard card;
      
      card.id                = sqlite3_column_int(stmt, 0);
      card.title             = safe_sqlite_string(stmt, 1);
      card.flavor_text       = safe_sqlite_string(stmt, 2);
      card.type              = static_cast<CardType>(sqlite3_column_int(stmt, 3));
      card.attack_damage     = sqlite3_column_int(stmt, 4);

      // Parse shorthand shield string collections
      std::stringstream ss(safe_sqlite_string(stmt, 5));
      char token;
      while (ss >> token) {
        if (token == 'M') card.enemy_shields.push_back(DieFace::MIGHT);
        else if (token == 'C') card.enemy_shields.push_back(DieFace::CUNNING);
        else if (token == 'W') card.enemy_shields.push_back(DieFace::WISDOM);
        else if (token == 'S') {
            // do nothing for now.
        }
      }

      card.choice_1_text     = safe_sqlite_string(stmt, 6);
      card.choice_1_damage   = sqlite3_column_int(stmt, 7);
      card.choice_2_text     = safe_sqlite_string(stmt, 9);
      card.choice_2_damage   = sqlite3_column_int(stmt, 10);

      // Reads target_attribute securely as a string token type
      std::string attr_str = safe_sqlite_string(stmt, 12);
      if (!attr_str.empty()) {
        char ac = attr_str.at(0);
        card.target_attribute = (ac == 'M') ? DieFace::MIGHT : (ac == 'C') ? DieFace::CUNNING : (ac == 'W') ? DieFace::WISDOM : DieFace::BLANK;
      }

      card.required_successes = sqlite3_column_int(stmt, 13);
      card.max_attempts       = sqlite3_column_int(stmt, 14);
      card.damage_per_roll    = sqlite3_column_int(stmt, 15);
      card.failure_damage     = sqlite3_column_int(stmt, 16);
      card.trap_damage        = sqlite3_column_int(stmt, 17);

      card.has_reward        = (sqlite3_column_int(stmt, 18) != 0);
      card.reward_type       = safe_sqlite_string(stmt, 19);
      card.reward_amount     = sqlite3_column_int(stmt, 20);

      if (card.type == CardType::BOSS_BATTLE) {
        final_boss_card = card;
      } else {
        master_chapter_pool.push_back(card);
      }
    }
    sqlite3_finalize(stmt);
  } else {
    std::cerr << "Critical Error: Chapter query statement compilation failed: " << sqlite3_errmsg(db) << std::endl;
    sqlite3_close(db);
    return false;
  }

  sqlite3_close(db);
  return true;
}

void DarkCastleEngine::generate_random_game_deck() {
  current_game_deck.clear();
  std::vector<ChapterCard> pool = master_chapter_pool;
  if (pool.empty()) return;
  
  std::mt19937 rand_eng(static_cast<unsigned int>(time(nullptr)));
  std::shuffle(pool.begin(), pool.end(), rand_eng);
  
  size_t limit = std::min(static_cast<size_t>(15), pool.size());
  for (size_t i = 0; i < limit; ++i) {
    current_game_deck.push_back(pool.at(i));
  }
  
  // 1. Pull the top room card row off the vector stack list to seed active_card
  advance_to_next_chapter_card();

  // Reset all state machine switches cleanly for a pristine Room 1 setup
  is_awaiting_door_open         = true;
  room_is_completed             = false;
  is_awaiting_prompt_choice     = false;
  is_awaiting_reroll_choice     = false;
  is_awaiting_greatsword_choice = false;
  is_awaiting_loot_choice       = false;
  
  challenge_successes_gained    = 0;
  challenge_attempts_left       = 0;

  // 2. Classify and route active input triggers depending on the drawn card type
  if (active_card.type == CardType::SKILL_CHALLENGE) {
    setup_active_challenge(active_card);
  }
  else if (active_card.type == CardType::BRANCHING_CHOICE) {
    // Force the input prompt choice flag to true so the choice columns open up
    is_awaiting_prompt_choice   = true;
  }
}

void DarkCastleEngine::advance_to_next_chapter_card() {
  if (current_game_deck.empty()) {
    if (active_card.type != CardType::BOSS_BATTLE) {
      active_card = final_boss_card;
      live_enemy_shields.clear();
      for (const auto& face : active_card.enemy_shields) {
        if (face == DieFace::MIGHT)
          live_enemy_shields.push_back("Might");
        else if (face == DieFace::CUNNING)
          live_enemy_shields.push_back("Cunning");
        else if (face == DieFace::WISDOM)
          live_enemy_shields.push_back("Wisdom");
        else if (face == DieFace::SHIELD)
          live_enemy_shields.push_back("Shield");
        else
          live_enemy_shields.push_back("Blank");
      }
      add_to_game_log("CRITICAL: The exploration deck is exhausted. " +
                      active_card.title + " emerges!");
      if (active_card.type == CardType::BRANCHING_CHOICE)
        is_awaiting_prompt_choice = true;
      return;
    } else {
      player_won = true;
      game_over = true;
      add_to_game_log("The Boss has been overthrown! You escape the castle!");
      return;
    }
  }

  active_card = current_game_deck.at(0);
  current_game_deck.erase(current_game_deck.begin());

  live_enemy_shields.clear();
  for (const auto& face : active_card.enemy_shields) {
    if (face == DieFace::MIGHT)
      live_enemy_shields.push_back("Might");
    else if (face == DieFace::CUNNING)
      live_enemy_shields.push_back("Cunning");
    else if (face == DieFace::WISDOM)
      live_enemy_shields.push_back("Wisdom");
    else if (face == DieFace::SHIELD)
      live_enemy_shields.push_back("Shield");
    else
      live_enemy_shields.push_back("Blank");
  }

  if (active_card.type == CardType::SKILL_CHALLENGE)
    setup_active_challenge(active_card);
  if (active_card.type == CardType::BRANCHING_CHOICE)
    is_awaiting_prompt_choice = true;
}

void DarkCastleEngine::setup_active_challenge(ChapterCard card) {
  is_awaiting_prompt_choice = false;
  challenge_attempts_left = card.max_attempts;
  challenge_successes_gained = 0;
}

void DarkCastleEngine::execute_challenge_roll_step() {
  if (challenge_attempts_left <= 0 || game_over) return;

  hero.current_hp -= active_card.damage_per_roll;
  challenge_attempts_left--;
  if (hero.current_hp <= 0) {
    game_over = true;
    return;
  }

  DieFace roll = hero.dice_faces.at(rand() % hero.dice_faces.size());

  std::string roll_name = "Blank";
  if (roll == DieFace::MIGHT)
    roll_name = "M (Might)";
  else if (roll == DieFace::CUNNING)
    roll_name = "C (Cunning)";
  else if (roll == DieFace::WISDOM)
    roll_name = "W (Wisdom)";
  else if (roll == DieFace::SHIELD)
    roll_name = "S (Shield)";

  bool scored = (roll == active_card.target_attribute);

  for (const auto& item : hero_inventory) {
    if (item.is_weapon &&
        item.description.find("Shield") != std::string::npos) {
      if (roll == DieFace::SHIELD) scored = true;
    }
  }

  if (scored) {
    add_to_game_log(hero.name + " rolled " + roll_name + " (SUCCESS!)");
  } else {
    add_to_game_log(hero.name + " rolled " + roll_name + " (Miss)");
  }

  if (scored) challenge_successes_gained++;
  if (challenge_successes_gained >= active_card.required_successes) {
    add_to_game_log("Challenge Complete! Passing gate...");
    distribute_challenge_reward(active_card);
    advance_to_next_chapter_card();
    return;
  }
  if (challenge_attempts_left <= 0) {
    hero.current_hp -= active_card.failure_damage;
    add_to_game_log("Challenge Failed! Party took damage.");
    if (hero.current_hp <= 0) {
      game_over = true;
    } else {
      advance_to_next_chapter_card();
    }
  }
}


void DarkCastleEngine::execute_combat_round() {
  if (live_enemy_shields.empty() || is_awaiting_prompt_choice || is_awaiting_reroll_choice || game_over) {
    return;
  }

  // Map out pointers to easily switch targets based on the current acting index
  Prisoner* actor = (current_acting_player_id == 1) ? &hero : &companion;
  std::vector<ItemCard>& inv = (current_acting_player_id == 1) ? hero_inventory : companion_inventory;

  // Determine who the next player will be after this roll settles
  int next_player_id = (current_acting_player_id == 1) ? 2 : 1;

  // Reset block flags at the start of a fresh round cycle (when the door opener steps up)
  if (current_acting_player_id == current_door_opener_id) {
    hero_blocked_this_round = false;
    comp_blocked_this_round = false;
  }

  // ==============================================================================
  // RE-ENFORCE COMBAT REST PASS BYPASS MACHINE GATES
  // ==============================================================================
  if (resting_character == current_acting_player_id) {
    int old_hp = actor->current_hp;
    actor->current_hp = std::min(actor->current_hp + 1, actor->max_hp);
    add_to_game_log(actor->name + " rested, recovered +1 HP (" + 
                    std::to_string(old_hp) + "->" + std::to_string(actor->current_hp) + ")");

    hero_last_roll_str = "";
    companion_last_roll_str = "";

    // Safely check if this step completes the full round loop
    bool round_is_complete = (current_acting_player_id != current_door_opener_id);

    if (!round_is_complete) {
      // Pass the dice context over to the active teammate without rolling any attack traits
      current_acting_player_id = next_player_id;
      add_to_game_log("It is now " + ((current_acting_player_id == 1) ? hero.name : companion.name) + "'s turn to strike.");
    } 
    else {
      // Both characters finished their phase steps (one fought, one rested). Execute enemy retaliation!
      current_acting_player_id = current_door_opener_id;
      add_to_game_log("The enemy counter-attacks!");

      int incoming_base_damage = active_card.attack_damage;

      auto apply_enemy_damage = [&](Prisoner& p_actor, std::vector<ItemCard>& p_inv, int char_id, bool has_rolled_block) {
        if (resting_character == char_id || has_rolled_block) return; // Resting characters are safe from counter-attacks

        int armor_mod = 0;
        bool heavy_shield_spent = false;

        auto item_it = p_inv.begin();
        while (item_it != p_inv.end()) {
          if (item_it->special_action_type == "PERMANENT_PLATE_ARMOR") {
            armor_mod += item_it->attack_amount;
            ++item_it;
          } else if (item_it->special_action_type == "HEAVY_SHIELD_ROUND") {
            heavy_shield_spent = true;
            item_it = p_inv.erase(item_it);
          } else {
            ++item_it;
          }
        }

        if (heavy_shield_spent) return;

        int final_damage = std::max(0, incoming_base_damage - armor_mod);
        if (final_damage > 0) {
          p_actor.current_hp = std::max(0, p_actor.current_hp - final_damage);
          add_to_game_log(p_actor.name + " sustained -" + std::to_string(final_damage) + " enemy combat damage!");
        }
      };

      apply_enemy_damage(hero, hero_inventory, 1, hero_blocked_this_round);
      apply_enemy_damage(companion, companion_inventory, 2, comp_blocked_this_round);

      if (hero.current_hp <= 0 || companion.current_hp <= 0) {
        game_over = true;
        add_to_game_log("A companion has fallen in battle! The run has failed.");
      }
    }
    return; // Exit out immediately to bypass the lower dice rolling code execution paths!
  }

  // ==============================================================================
  // STEP A-2: STANDARD ROLL PASS (Executed ONLY if NOT resting)
  // ==============================================================================
  else if (!actor->dice_faces.empty()) {
    // FIX A: Lock down the player index on entry frame frame capture
    int original_roller_id = current_acting_player_id;

    // Roll from the active player's unique dice array template
    DieFace rolled_face = actor->dice_faces.at(rand() % actor->dice_faces.size());

    std::string base_trait = "Blank";
    if (rolled_face == DieFace::MIGHT)   base_trait = "Might";
    if (rolled_face == DieFace::CUNNING) base_trait = "Cunning";
    if (rolled_face == DieFace::WISDOM)  base_trait = "Wisdom";
    if (rolled_face == DieFace::SHIELD)  base_trait = "Shield";

    // FIX B: Use original_roller_id to guarantee the strings route to the right slots!
    if (original_roller_id == 1) {
      hero_last_roll_str = base_trait;
    } else {
      companion_last_roll_str = base_trait;
    }

    if (rolled_face == DieFace::SHIELD) {
      if (current_acting_player_id == 1) hero_blocked_this_round = true;
      else                               comp_blocked_this_round = true;
      add_to_game_log(actor->name + " raised a Shield!");
    }

    // Process weapon modifiers
    std::string weapon_mod = "NONE";
    for (const auto& item : inv) {
      if (item.is_weapon) weapon_mod = item.special_action_type;
    }

    // PASS 3: INTERACTIVE RE-ROLL WEAPON AND SHIELD TRAITS
    bool has_reroll_gear = false;
    for (const auto& item : inv) {
      if (item.special_action_type == "RE_ROLL_MIGHT" && rolled_face != DieFace::MIGHT) has_reroll_gear = true;
      if (item.special_action_type == "RE_ROLL_CUNNING" && rolled_face != DieFace::CUNNING) has_reroll_gear = true;
      if (item.special_action_type == "RE_ROLL_ANY" && rolled_face != DieFace::SHIELD) has_reroll_gear = true;
    }

    // Verify if the active rolled face actually matches an existing enemy shield card
    auto match_it = std::find(live_enemy_shields.begin(), live_enemy_shields.end(), base_trait);

    if (has_reroll_gear && match_it == live_enemy_shields.end() && rolled_face != DieFace::SHIELD) {
      is_awaiting_reroll_choice = true;
      rerolls_remaining_this_turn = 1;
      
      add_to_game_log(actor->name + " missed, but their gear allows a TACTICAL RE-ROLL!");
      add_to_game_log("Press [Y] to roll again | Press [N] to keep original result.");
      return; 
    }

    // STEP A-3: STANDARD SHIELD MATCHING & ERASURE LOOPS
    int shields_to_erase = 0;
    auto it = std::find(live_enemy_shields.begin(), live_enemy_shields.end(), base_trait);

    if (it != live_enemy_shields.end()) {
      shields_to_erase = 1;
      if (weapon_mod == "DOUBLE_MIGHT" && rolled_face == DieFace::MIGHT) {
        shields_to_erase = 2;
        add_to_game_log(actor->name + " smashes with an Axe! Crushed TWO Might barriers!");
      } else if (weapon_mod == "RANGED_DOUBLE") {
        shields_to_erase = 2;
        add_to_game_log(actor->name + " fires a Crossbow volley! Erased 2 shields.");
      } else {
        add_to_game_log(actor->name + " rolled " + base_trait + " and matched an enemy shield!");
      }

      for (int m = 0; m < shields_to_erase; ++m) {
        auto erase_it = std::find(live_enemy_shields.begin(), live_enemy_shields.end(), base_trait);
        if (erase_it != live_enemy_shields.end()) {
          live_enemy_shields.erase(erase_it);
        }
      }
    } else if (rolled_face != DieFace::SHIELD) {
      add_to_game_log(actor->name + " rolled " + base_trait + " (No shield match)");
    }
  }

  // --- IMMEDIATE VICTORY VERIFICATION GATES ---
  if (live_enemy_shields.empty()) {
    add_to_game_log("Enemies completely defeated! Room cleared.");
    room_is_completed = true;
    if (active_card.has_reward && active_card.reward_type != "NONE") {
      distribute_challenge_reward(active_card);
    }
    return;
  }

  // ==============================================================================
  // STEP B: INTELLIGENT INITIALIZATION AND TURN ROUTING
  // ==============================================================================
  bool party_is_split = (resting_character > 0);

  if (party_is_split) {
    // --- SCENARIO A: SOLO ENGAGEMENT ROUTING MATRIX ---
    add_to_game_log("The enemy counter-attacks the active combatant!");

    int incoming_base_damage = active_card.attack_damage;

    auto apply_solo_enemy_damage = [&](Prisoner& p_actor, std::vector<ItemCard>& p_inv, int char_id, bool has_rolled_block) {
      if (resting_character == char_id || has_rolled_block) return;

      int armor_mod = 0;
      bool heavy_shield_spent = false;

      auto item_it = p_inv.begin();
      while (item_it != p_inv.end()) {
        if (item_it->special_action_type == "PERMANENT_PLATE_ARMOR") {
          armor_mod += item_it->attack_amount;
          ++item_it;
        } else if (item_it->special_action_type == "HEAVY_SHIELD_ROUND") {
          heavy_shield_spent = true;
          item_it = p_inv.erase(item_it);
        } else {
          ++item_it;
        }
      }

      if (heavy_shield_spent) return;

      int final_damage = std::max(0, incoming_base_damage - armor_mod);
      if (final_damage > 0) {
        p_actor.current_hp = std::max(0, p_actor.current_hp - final_damage);
        add_to_game_log(p_actor.name + " sustained -" + std::to_string(final_damage) + " enemy combat damage!");
      }
    };

    apply_solo_enemy_damage(hero, hero_inventory, 1, hero_blocked_this_round);
    apply_solo_enemy_damage(companion, companion_inventory, 2, comp_blocked_this_round);

    // --- FIX: ALLOW TURN TO PASS TO THE RESTING PLAYER SO THEY HEAL NEXT ROUND ---
    // Instead of locking onto the fighter, pass turn order to the next player inline!
    current_acting_player_id = (current_acting_player_id == 1) ? 2 : 1;
    
    // Clear out the round block flags immediately to prepare for the next step pass
    hero_blocked_this_round = false;
    comp_blocked_this_round = false;

    if (hero.current_hp <= 0 || companion.current_hp <= 0) {
      game_over = true;
      add_to_game_log("A prisoner has fallen! The run has failed.");
    }
  }
  else {
    // --- SCENARIO B: STANDARD COOPERATIVE 2-PLAYER ROUTING MATRIX ---
    // This is your original code logic flow that executes ONLY if no one is resting
    bool round_is_complete = (current_acting_player_id != current_door_opener_id);

    if (!round_is_complete) {
      current_acting_player_id = next_player_id;
      add_to_game_log("It is now " + ((current_acting_player_id == 1) ? hero.name : companion.name) + "'s turn to strike.");
    } 
    else {
      current_acting_player_id = current_door_opener_id;
      add_to_game_log("The enemy counter-attacks!");

      int incoming_base_damage = active_card.attack_damage;

      auto apply_coop_enemy_damage = [&](Prisoner& p_actor, std::vector<ItemCard>& p_inv, int char_id, bool has_rolled_block) {
        if (has_rolled_block) return;

        int armor_mod = 0;
        bool heavy_shield_spent = false;

        auto item_it = p_inv.begin();
        while (item_it != p_inv.end()) {
          if (item_it->special_action_type == "PERMANENT_PLATE_ARMOR") {
            armor_mod += item_it->attack_amount;
            ++item_it;
          } else if (item_it->special_action_type == "HEAVY_SHIELD_ROUND") {
            heavy_shield_spent = true;
            item_it = p_inv.erase(item_it);
          } else {
            ++item_it;
          }
        }

        if (heavy_shield_spent) return;

        int final_damage = std::max(0, incoming_base_damage - armor_mod);
        if (final_damage > 0) {
          p_actor.current_hp = std::max(0, p_actor.current_hp - final_damage);
          add_to_game_log(p_actor.name + " sustained -" + std::to_string(final_damage) + " enemy combat damage!");
        }
      };

      apply_coop_enemy_damage(hero, hero_inventory, 1, hero_blocked_this_round);
      apply_coop_enemy_damage(companion, companion_inventory, 2, comp_blocked_this_round);

      if (hero.current_hp <= 0 || companion.current_hp <= 0) {
        game_over = true;
        add_to_game_log("A companion has fallen in battle! The run has failed.");
      }
    }
  }
}


void DarkCastleEngine::process_input_event(int keycode) {
  // Clear any game over states if a keystroke occurs during full splash frames
  if (game_over) {
    if (keycode == ALLEGRO_KEY_ENTER || keycode == ALLEGRO_KEY_SPACE) {
      game_over = false;
      player_won = false;
      resting_character = 0;
      hero_last_roll_str = "";
      companion_last_roll_str = "";
      current_scene = SCENE_MAIN_MENU;
      add_to_game_log("Returned to title. Prepare for another escape attempt!");
    }
    return;
  }

  // ==============================================================================
  // SCENE CONTEXT LOOP A: MAIN TITLE SELECTION PROCESSING SCREEN
  // ==============================================================================
  if (current_scene == SCENE_MAIN_MENU) {
    // Up and Down keys are mapped to toggle your menu_cursor_index highlight positions
    if (keycode == ALLEGRO_KEY_UP || keycode == ALLEGRO_KEY_W) {
      menu_cursor_index = 0; // Highlight [ ENTER THE CASTLE ]
    }
    if (keycode == ALLEGRO_KEY_DOWN || keycode == ALLEGRO_KEY_S) {
      menu_cursor_index = 1; // Highlight [ ABANDON RUN ]
    }

    if (keycode == ALLEGRO_KEY_ENTER || keycode == ALLEGRO_KEY_SPACE) {
      if (menu_cursor_index == 1) {
        add_to_game_log("Exiting game run...");
        exit(0);
      }
      else {
        // Shift cleanly into character selection configurations
        current_scene = SCENE_CHARACTER_SELECT;
        add_to_game_log("Flipped to Character Select cells.");
      }
    }
    return;
  }


  // ==============================================================================
  // SCENE CONTEXT LOOP B: COOPERATIVE PRISONER ROW SELECTOR MENUS
  // ==============================================================================
  if (current_scene == SCENE_CHARACTER_SELECT) {
    
    // --- STEP 1: ISOLATED DIRECTIONAL SELECTION CONTROLS ---
    if (select_phase == 0) {
      if (keycode == ALLEGRO_KEY_LEFT || keycode == ALLEGRO_KEY_A) {
        p1_char_cursor = (p1_char_cursor - 1 + 6) % 6;
      }
      if (keycode == ALLEGRO_KEY_RIGHT || keycode == ALLEGRO_KEY_D) {
        p1_char_cursor = (p1_char_cursor + 1) % 6;
      }
      character_cursor_index = p1_char_cursor;
    } 
    else if (select_phase == 1) {
      if (keycode == ALLEGRO_KEY_LEFT || keycode == ALLEGRO_KEY_A) {
        p2_char_cursor = (p2_char_cursor - 1 + 6) % 6;
      }
      if (keycode == ALLEGRO_KEY_RIGHT || keycode == ALLEGRO_KEY_D) {
        p2_char_cursor = (p2_char_cursor + 1) % 6;
      }
      character_cursor_index = p2_char_cursor;
    }

    // --- STEP 2: PROTECTED TURN-BASED CONFIRMATION PASS ---
    if (keycode == ALLEGRO_KEY_ENTER || keycode == ALLEGRO_KEY_SPACE) {
      
      // Enforce a hard time-gate interlock to filter out rapid multi-frame duplicate keystrokes
      static double last_selection_click_time = 0.0;
      double current_time = al_get_time();
      if ((current_time - last_selection_click_time) < 0.25) {
        return; // Discard duplicate frame stutters completely!
      }
      last_selection_click_time = current_time;

      // Phase 0 Confirmation: Player 1 Locks In
      if (select_phase == 0) {
        select_phase = 1;
        
        // Prevent Player 2 from starting on the exact same card row
        if (p2_char_cursor == p1_char_cursor) {
          p2_char_cursor = (p2_char_cursor + 1) % 6;
        }
        
        character_cursor_index = p2_char_cursor;
        add_to_game_log("Player 1 locked choice. Player 2 choose your prisoner.");
        return; 
      }
      // Phase 1 Confirmation: Player 2 Final Submission Pass
      else if (select_phase == 1) {
        if (p1_char_cursor == p2_char_cursor) {
          add_to_game_log("Selection Blocked: Both players cannot choose the exact same prisoner!");
          return;
        }

        // Assign Player 1 attributes dynamically using your base variables
        if (p1_char_cursor == 0) { hero.name = "The Abbot"; hero.max_hp = 12; hero.dice_faces = {DieFace::MIGHT, DieFace::WISDOM, DieFace::WISDOM, DieFace::WISDOM, DieFace::SHIELD, DieFace::SHIELD}; }
        else if (p1_char_cursor == 1) { hero.name = "The Tailor"; hero.max_hp = 18; hero.dice_faces = {DieFace::CUNNING, DieFace::CUNNING, DieFace::WISDOM, DieFace::WISDOM, DieFace::MIGHT, DieFace::SHIELD}; }
        else if (p1_char_cursor == 2) { hero.name = "The Smith"; hero.max_hp = 16; hero.dice_faces = {DieFace::MIGHT, DieFace::MIGHT, DieFace::MIGHT, DieFace::CUNNING, DieFace::WISDOM, DieFace::SHIELD}; }
        else if (p1_char_cursor == 3) { hero.name = "The Cook"; hero.max_hp = 14; hero.dice_faces = {DieFace::CUNNING, DieFace::CUNNING, DieFace::CUNNING, DieFace::MIGHT, DieFace::WISDOM, DieFace::SHIELD}; }
        else if (p1_char_cursor == 4) { hero.name = "The Tanner"; hero.max_hp = 15; hero.dice_faces = {DieFace::MIGHT, DieFace::MIGHT, DieFace::CUNNING, DieFace::CUNNING, DieFace::WISDOM, DieFace::SHIELD}; }
        else if (p1_char_cursor == 5) { hero.name = "The Miller"; hero.max_hp = 17; hero.dice_faces = {DieFace::MIGHT, DieFace::MIGHT, DieFace::WISDOM, DieFace::WISDOM, DieFace::CUNNING, DieFace::SHIELD}; }
        hero.current_hp = hero.max_hp;

        // Assign Player 2 attributes dynamically using your base variables
        if (p2_char_cursor == 0) { companion.name = "The Abbot"; companion.max_hp = 12; companion.dice_faces = {DieFace::MIGHT, DieFace::WISDOM, DieFace::WISDOM, DieFace::WISDOM, DieFace::SHIELD, DieFace::SHIELD}; }
        else if (p2_char_cursor == 1) { companion.name = "The Tailor"; companion.max_hp = 18; companion.dice_faces = {DieFace::CUNNING, DieFace::CUNNING, DieFace::WISDOM, DieFace::WISDOM, DieFace::MIGHT, DieFace::SHIELD}; }
        else if (p2_char_cursor == 2) { companion.name = "The Smith"; companion.max_hp = 16; companion.dice_faces = {DieFace::MIGHT, DieFace::MIGHT,DieFace::MIGHT, DieFace::CUNNING, DieFace::WISDOM, DieFace::SHIELD}; }
        else if (p2_char_cursor == 3) { companion.name = "The Cook"; companion.max_hp = 14; companion.dice_faces = {DieFace::CUNNING, DieFace::CUNNING, DieFace::CUNNING, DieFace::MIGHT, DieFace::WISDOM, DieFace::SHIELD}; }
        else if (p2_char_cursor == 4) { companion.name = "The Tanner"; companion.max_hp = 15; companion.dice_faces = {DieFace::MIGHT, DieFace::MIGHT, DieFace::CUNNING, DieFace::CUNNING, DieFace::WISDOM, DieFace::SHIELD}; }
        else if (p2_char_cursor == 5) { companion.name = "The Miller"; companion.max_hp = 17; companion.dice_faces = {DieFace::MIGHT, DieFace::MIGHT, DieFace::WISDOM, DieFace::WISDOM, DieFace::CUNNING, DieFace::SHIELD}; }
        companion.current_hp = companion.max_hp;

        select_phase = 0;

        hero_inventory.clear();
        companion_inventory.clear();
        hero_inv_cursor = 0;
        companion_inv_cursor = 0;
        hero_last_roll_str = "";
        companion_last_roll_str = "";

        generate_random_game_deck();
        current_scene = SCENE_GAMEPLAY;
      }
    }
    return;
  }


  // ==============================================================================
  // SCENE CONTEXT LOOP C: ACTIVE GAMEPLAY MODE
  // ==============================================================================
  if (current_scene == SCENE_GAMEPLAY) {
    
    // --------------------------------------------------------------------------
    // ACTION GATE 1: DOOR FLIP HOOK
    // --------------------------------------------------------------------------
    if (is_awaiting_door_open) {
      if (keycode == ALLEGRO_KEY_1) {
        current_door_opener_id = 1;
        current_acting_player_id = 1;
        is_awaiting_door_open = false;
        room_is_completed = false;
        play_door_open_sfx();
        live_enemy_shields.clear();

        if (active_card.type == CardType::SKILL_CHALLENGE) {
          setup_active_challenge(active_card);
        } else if (active_card.type == CardType::STANDARD_COMBAT || active_card.type == CardType::BOSS_BATTLE) {
          for (const auto& face : active_card.enemy_shields) {
            if (face == DieFace::MIGHT)   live_enemy_shields.push_back("Might");
            if (face == DieFace::CUNNING) live_enemy_shields.push_back("Cunning");
            if (face == DieFace::WISDOM)  live_enemy_shields.push_back("Wisdom");
            if (face == DieFace::SHIELD)  live_enemy_shields.push_back("Shield");
          }
        } else if (active_card.type == CardType::BRANCHING_CHOICE) {
          is_awaiting_prompt_choice = true;
        }

        if (active_card.type == CardType::NARRATIVE_EVENT) {
          room_is_completed = true;
          if (active_card.trap_damage > 0) {
            int old_hp = hero.current_hp;
            hero.current_hp = std::max(0, hero.current_hp - active_card.trap_damage);
            add_to_game_log("SNAP! " + hero.name + " triggered a trap! Sustained -" + 
                            std::to_string(active_card.trap_damage) + " damage (" + 
                            std::to_string(old_hp) + " -> " + std::to_string(hero.current_hp) + ")");
          }
        }

        if (active_card.has_reward && (active_card.type == CardType::NARRATIVE_EVENT || active_card.reward_type == "DOUBLE_ITEM")) {
          distribute_challenge_reward(active_card);
        }
        add_to_game_log(hero.name + " turned the card and leads the room!");
      }

      if (keycode == ALLEGRO_KEY_2) {
        current_door_opener_id = 2;
        current_acting_player_id = 2;
        is_awaiting_door_open = false;
        room_is_completed = false;
        play_door_open_sfx();
        live_enemy_shields.clear();

        if (active_card.type == CardType::SKILL_CHALLENGE) {
          setup_active_challenge(active_card);
        } else if (active_card.type == CardType::STANDARD_COMBAT || active_card.type == CardType::BOSS_BATTLE) {
          for (const auto& face : active_card.enemy_shields) {
            if (face == DieFace::MIGHT)   live_enemy_shields.push_back("Might");
            if (face == DieFace::CUNNING) live_enemy_shields.push_back("Cunning");
            if (face == DieFace::WISDOM)  live_enemy_shields.push_back("Wisdom");
            if (face == DieFace::SHIELD)  live_enemy_shields.push_back("Shield");
          }
        } else if (active_card.type == CardType::BRANCHING_CHOICE) {
          is_awaiting_prompt_choice = true;
        }

        if (active_card.type == CardType::NARRATIVE_EVENT) {
          room_is_completed = true;
          if (active_card.trap_damage > 0) {
            int old_hp = companion.current_hp;
            companion.current_hp = std::max(0, companion.current_hp - active_card.trap_damage);
            add_to_game_log("SNAP! " + companion.name + " triggered a trap! Sustained -" + 
                            std::to_string(active_card.trap_damage) + " damage (" + 
                            std::to_string(old_hp) + " -> " + std::to_string(companion.current_hp) + ")");
          }
        }

        if (active_card.has_reward && (active_card.type == CardType::NARRATIVE_EVENT || active_card.reward_type == "DOUBLE_ITEM")) {
          distribute_challenge_reward(active_card);
        }
        add_to_game_log(companion.name + " turned the card and leads the room!");
      }

      if (hero.current_hp <= 0 || companion.current_hp <= 0) game_over = true;
      return;
    }

    // --------------------------------------------------------------------------
    // ACTION GATE 2: WEAPON RE-ROLL CHOICE MODAL FILTER
    // --------------------------------------------------------------------------
    if (is_awaiting_reroll_choice) {
      Prisoner* current_actor = (current_acting_player_id == 1) ? &hero : &companion;

      if (keycode == ALLEGRO_KEY_Y) {
        is_awaiting_reroll_choice = false;
        rerolls_remaining_this_turn = 0;
        add_to_game_log(current_actor->name + " spent a charge to re-roll! Rolling again...");
        execute_combat_round();
      }

      if (keycode == ALLEGRO_KEY_N) {
        is_awaiting_reroll_choice = false;
        rerolls_remaining_this_turn = 0;
        add_to_game_log(current_actor->name + " accepted their current die roll outcome.");

        bool round_is_complete = (current_acting_player_id != current_door_opener_id);
        int next_player_id = (current_acting_player_id == 1) ? 2 : 1;

        if (!round_is_complete) {
          current_acting_player_id = next_player_id;
          add_to_game_log("It is now " + ((current_acting_player_id == 1) ? hero.name : companion.name) + "'s turn to strike.");
        } else {
          execute_combat_round();
        }
      }
      return;
    }


    // --------------------------------------------------------------------------
    // ACTION GATE 3: GREATSWORD COMBAT TARGET INDICES SELECTOR
    // --------------------------------------------------------------------------
    if (is_awaiting_greatsword_choice) {
      int max_shields = static_cast<int>(live_enemy_shields.size());
      if (max_shields == 0) {
        is_awaiting_greatsword_choice = false;
        return;
      }

      if (keycode == ALLEGRO_KEY_LEFT || keycode == ALLEGRO_KEY_A) {
        greatsword_shield_cursor = (greatsword_shield_cursor - 1 + max_shields) % max_shields;
      }
      if (keycode == ALLEGRO_KEY_RIGHT || keycode == ALLEGRO_KEY_D) {
        greatsword_shield_cursor = (greatsword_shield_cursor + 1) % max_shields;
      }
      if (keycode == ALLEGRO_KEY_ENTER || keycode == ALLEGRO_KEY_SPACE) {
        add_to_game_log("Greatsword slashing strike cleaves monster shield index: " + std::to_string(greatsword_shield_cursor));
        live_enemy_shields.erase(live_enemy_shields.begin() + greatsword_shield_cursor);
        greatsword_removals_left--;

        if (greatsword_removals_left <= 0 || live_enemy_shields.empty()) {
          is_awaiting_greatsword_choice = false;
        } else {
          greatsword_shield_cursor = 0;
        }
      }
      return;
    }

    // --------------------------------------------------------------------------
    // ACTION GATE 4: ROOM REWARD LOOT DROP DISTRIBUTION PANEL SELECTOR
    // --------------------------------------------------------------------------
    if (is_awaiting_loot_choice) {
      if (keycode == ALLEGRO_KEY_LEFT || keycode == ALLEGRO_KEY_A) {
        loot_target_player_cursor = 0;
      }
      if (keycode == ALLEGRO_KEY_RIGHT || keycode == ALLEGRO_KEY_D) {
        loot_target_player_cursor = 1;
      }
      if (keycode == ALLEGRO_KEY_X) {
        is_awaiting_loot_choice = false;
        add_to_game_log("The party left the " + pending_looted_item.name + " behind in the dust.");

        if (active_card.reward_type == "DOUBLE_ITEM" && !room_is_completed) {
          room_is_completed = true;
          pending_looted_item = master_treasure_pool.at(rand() % master_treasure_pool.size());
          is_awaiting_loot_choice = true;
          add_to_game_log("Second Item: " + pending_looted_item.name + "! Allocate or discard.");
        } else {
          room_is_completed = true;
        }
        return;
      }
      if (keycode == ALLEGRO_KEY_ENTER || keycode == ALLEGRO_KEY_SPACE) {
        std::vector<ItemCard>& target_inv = (loot_target_player_cursor == 0) ? hero_inventory : companion_inventory;
        Prisoner& target_prisoner = (loot_target_player_cursor == 0) ? hero : companion;

        if (give_item_to_character(target_inv, pending_looted_item)) {
          is_awaiting_loot_choice = false;

          if (active_card.reward_type == "DOUBLE_ITEM" && !room_is_completed) {
            room_is_completed = true;
            pending_looted_item = master_treasure_pool.at(rand() % master_treasure_pool.size());
            is_awaiting_loot_choice = true;
            add_to_game_log("Second Item: " + pending_looted_item.name + "! Assign to your characters.");
          } else {
            room_is_completed = true;
          }
        } else {
          add_to_game_log("Inventory Full! " + target_prisoner.name + " cannot carry this. Press [X] to discard.");
        }
      }
      return;
    }

    // --------------------------------------------------------------------------
    // ACTION GATE 5: BRANCHING CHOICE ROOM PASS PATH SELECTORS
    // --------------------------------------------------------------------------
    if (active_card.type == CardType::BRANCHING_CHOICE && is_awaiting_prompt_choice) {
      if (keycode == ALLEGRO_KEY_Y) {
        is_awaiting_prompt_choice = false;
        add_to_game_log("The party selects: Option [Y] -> " + active_card.choice_1_text);

        if (active_card.choice_1_damage > 0) {
          Prisoner* leader = (current_door_opener_id == 1) ? &hero : &companion;
          leader->current_hp = std::max(0, leader->current_hp - active_card.choice_1_damage);
          add_to_game_log(leader->name + " sustained -" + std::to_string(active_card.choice_1_damage) + " trap damage resolving the choices!");
        }

        for (const auto& face : active_card.choice_1_shields) {
          if (face == DieFace::MIGHT)   live_enemy_shields.push_back("Might");
          if (face == DieFace::CUNNING) live_enemy_shields.push_back("Cunning");
          if (face == DieFace::WISDOM)  live_enemy_shields.push_back("Wisdom");
          if (face == DieFace::SHIELD)  live_enemy_shields.push_back("Shield");
        }

        if (live_enemy_shields.empty()) room_is_completed = true;
        if (hero.current_hp <= 0 || companion.current_hp <= 0) game_over = true;
      }

      if (keycode == ALLEGRO_KEY_N) {
        is_awaiting_prompt_choice = false;
        add_to_game_log("The party selects: Option [N] -> " + active_card.choice_2_text);

        if (active_card.choice_2_damage > 0) {
          Prisoner* leader = (current_door_opener_id == 1) ? &hero : &companion;
          leader->current_hp = std::max(0, leader->current_hp - active_card.choice_2_damage);
          add_to_game_log(leader->name + " sustained -" + std::to_string(active_card.choice_2_damage) + " trap damage resolving the choices!");
        }

        for (const auto& face : active_card.choice_2_shields) {
          if (face == DieFace::MIGHT)   live_enemy_shields.push_back("Might");
          if (face == DieFace::CUNNING) live_enemy_shields.push_back("Cunning");
          if (face == DieFace::WISDOM)  live_enemy_shields.push_back("Wisdom");
          if (face == DieFace::SHIELD)  live_enemy_shields.push_back("Shield");
        }

        if (live_enemy_shields.empty()) room_is_completed = true;
        if (hero.current_hp <= 0 || companion.current_hp <= 0) game_over = true;
      }
      return;
    }

    // --------------------------------------------------------------------------
    // ACTION INTERACTION C: ACTIVE GEAR DISCARDING/DROPPING PROCEDURES
    // --------------------------------------------------------------------------
    if (keycode == ALLEGRO_KEY_BACKSPACE) {
      if (!hero_inventory.empty() && hero_inv_cursor < static_cast<int>(hero_inventory.size())) {
        std::string dropped_item_name = hero_inventory.at(hero_inv_cursor).name;
        hero_inventory.erase(hero_inventory.begin() + hero_inv_cursor);
        add_to_game_log(hero.name + " discarded " + dropped_item_name + " to free a slot.");
        if (hero_inv_cursor > 0) hero_inv_cursor--;
      } else {
        add_to_game_log("Action Refused: Chosen slot is already empty!");
      }
    }

    if (keycode == ALLEGRO_KEY_DELETE) {
      if (!companion_inventory.empty() && companion_inv_cursor < static_cast<int>(companion_inventory.size())) {
        std::string dropped_item_name = companion_inventory.at(companion_inv_cursor).name;
        companion_inventory.erase(companion_inventory.begin() + companion_inv_cursor);
        add_to_game_log(companion.name + " discarded " + dropped_item_name + " to free a slot.");
        if (companion_inv_cursor > 0) companion_inv_cursor--;
      } else {
        add_to_game_log("Action Refused: Chosen slot is already empty!");
      }
    }

    // --------------------------------------------------------------------------
    // ACTION INTERACTION E: CORE TURN-BASED RECOVERY CONTROLLER ('R' KEY)
    // --------------------------------------------------------------------------
    if (keycode == ALLEGRO_KEY_R && !game_over && !room_is_completed) {
      if (active_card.type == CardType::SKILL_CHALLENGE) {
        add_to_game_log("You cannot rest!");
        return;
      }

      resting_character = (resting_character + 1) % 3;
      if (resting_character == 1)      add_to_game_log(hero.name + " is resting.");
      else if (resting_character == 2) add_to_game_log(companion.name + " is resting.");
      else                             add_to_game_log("Both players active.");
      return;
    }
    // REVISED NAVIGATION METHOD: TURN-AWARE DYNAMIC KEYBOARD ARROW CONTROLLERS
    if (!is_awaiting_greatsword_choice && !is_awaiting_loot_choice && !is_awaiting_door_open && !room_is_completed) {
      if (keycode == ALLEGRO_KEY_LEFT || keycode == ALLEGRO_KEY_RIGHT) {
        if (current_acting_player_id == 1) {
          hero_inv_cursor = (hero_inv_cursor == 0) ? 1 : 0;
          add_to_game_log(hero.name + " focused Item Slot " + std::to_string(hero_inv_cursor + 1));
        } 
        else if (current_acting_player_id == 2) {
          companion_inv_cursor = (companion_inv_cursor == 0) ? 1 : 0;
          add_to_game_log(companion.name + " focused Item Slot " + std::to_string(companion_inv_cursor + 1));
        }
      }
    }

    // --------------------------------------------------------------------------
    // ACTION INTERACTION E: ITEM CONSUMPTION MODIFIERS ACTIVATION ENGINE
    // --------------------------------------------------------------------------
    if (keycode == ALLEGRO_KEY_LSHIFT) {
      consume_item_from_slot(hero_inventory, hero, hero_inv_cursor);
    }
    if (keycode == ALLEGRO_KEY_RSHIFT) {
      consume_item_from_slot(companion_inventory, companion, companion_inv_cursor);
    }

    // --------------------------------------------------------------------------
    // ACTION INTERACTION D: CORE INTERACTIVE THREAT RESOLVER ACTIONS (SPACEBAR)
    // --------------------------------------------------------------------------
    if (keycode == ALLEGRO_KEY_SPACE && !game_over && !room_is_completed) {
      if (!bone_dice_samples.empty()) {
        al_play_sample(bone_dice_samples.at(0), 1.0, 0.0, 1.0, ALLEGRO_PLAYMODE_ONCE, nullptr);
      }

      if (active_card.type == CardType::SKILL_CHALLENGE) {
        if (challenge_attempts_left <= 0) return;

        Prisoner* actor = (current_door_opener_id == 1) ? &hero : &companion;
        challenge_attempts_left--;

        DieFace roll = actor->dice_faces.at(rand() % actor->dice_faces.size());
        bool scored = (roll == active_card.target_attribute);

        std::string roll_name = (roll == DieFace::MIGHT) ? "Might" :
                                (roll == DieFace::CUNNING) ? "Cunning" :
                                (roll == DieFace::WISDOM) ? "Wisdom" : "Shield";

        if (current_door_opener_id == 1) {
          hero_last_roll_str = roll_name;
        } else {
          companion_last_roll_str = roll_name;
        }

        if (scored) {
          challenge_successes_gained++;
          add_to_game_log(actor->name + " rolled " + roll_name + " (SUCCESS!)");
        } else {
          actor->current_hp -= active_card.damage_per_roll;
          add_to_game_log(actor->name + " rolled " + roll_name + " (Miss) -" + std::to_string(active_card.damage_per_roll) + " HP");
        }

        if (challenge_successes_gained >= active_card.required_successes) {
          add_to_game_log("Challenge Complete! Room threats resolved safely.");
          room_is_completed = true;
          distribute_challenge_reward(active_card);
        }
        else if (challenge_attempts_left <= 0) {
          actor->current_hp -= active_card.failure_damage;
          add_to_game_log(actor->name + " ran out of attempts! Triggered trap: -" + std::to_string(active_card.failure_damage) + " HP.");
          room_is_completed = true;
        }

        if (hero.current_hp <= 0 || companion.current_hp <= 0) game_over = true;
        return;
      }
      else {
        int original_acting_id = current_acting_player_id;

        if (resting_character == original_acting_id) {
          execute_combat_round();

          if (resting_character != current_acting_player_id && !is_dice_animating && !room_is_completed && !game_over) {
            is_dice_animating = true;
            dice_anim_frame_counter = 0;

            if (current_acting_player_id == 1) hero_last_roll_str = "";
            else                               companion_last_roll_str = "";

            if (resting_character == 1) hero_last_roll_str = "";
            if (resting_character == 2) companion_last_roll_str = "";
          }
        } 
        else {
          if (!is_dice_animating) {
            is_dice_animating = true;
            dice_anim_frame_counter = 0;

            if (current_acting_player_id == 1) hero_last_roll_str = "";
            else                               companion_last_roll_str = "";

            if (resting_character == 1) hero_last_roll_str = "";
            if (resting_character == 2) companion_last_roll_str = "";
          }
        }
        return;
      }
    }

    // --------------------------------------------------------------------------
    // ACTION INTERACTION F: ROOM COMPLETION ADVANCEMENT STEP PROGRESSOR ('N' KEY)
    // --------------------------------------------------------------------------
    if (keycode == ALLEGRO_KEY_N && !game_over) {
      if (active_card.type == CardType::NARRATIVE_EVENT) {
        is_awaiting_door_open = true;
        resting_character = 0;
        advance_to_next_chapter_card();
        return;
      }

      bool combat_won = ((active_card.type == CardType::STANDARD_COMBAT || 
                          active_card.type == CardType::BOSS_BATTLE) && 
                         live_enemy_shields.empty());

      if (room_is_completed || combat_won) {
        is_awaiting_door_open = true;
        resting_character = 0;
        advance_to_next_chapter_card();
      } else {
        add_to_game_log("Action Refused: You must resolve the current room threat first!");
      }
      return;
    }
  }
}


// ==============================================================================
// VISUAL LAYER INITIALIZATION SUITE
// ==============================================================================
void DarkCastleEngine::render_main_title_screen() {
  // Use menu_title_font if successfully loaded, otherwise fall back to local_font safely
  ALLEGRO_FONT* header_font = menu_title_font ? menu_title_font : local_font;

  // --- RE-CENTERED AT 512 HORIZONTALLY FOR 1024 WIDTH VIEWS ---
  // Large Title Banner drawn at 36pt size
  al_draw_text(header_font, al_map_rgb(255, 255, 255), 512, 220,
               ALLEGRO_ALIGN_CENTER, "ESCAPE THE DARK CASTLE");

  // Subtitle text drawn below the header
  al_draw_text(local_font, al_map_rgb(130, 130, 130), 512, 280,
               ALLEGRO_ALIGN_CENTER, "A Digital Tabletop Board Game");

  // Highlight selection block states
  ALLEGRO_COLOR p_col = (menu_cursor_index == 0) ? al_map_rgb(255, 215, 0) : al_map_rgb(70, 70, 70);
  ALLEGRO_COLOR q_col = (menu_cursor_index == 1) ? al_map_rgb(255, 50, 50) : al_map_rgb(70, 70, 70);

  // Menu Options pushed down to clear the larger header space
  al_draw_text(local_font, p_col, 512, 420, ALLEGRO_ALIGN_CENTER, "[ ENTER THE CASTLE ]");
  al_draw_text(local_font, q_col, 512, 470, ALLEGRO_ALIGN_CENTER, "[ ABANDON RUN ]");
}

// ==============================================================================
// 6-CHARACTER SELECTION DYNAMIC DRAFT GRID
// ==============================================================================
void DarkCastleEngine::render_character_selection_menu() {
  al_draw_text(local_font, al_map_rgb(255, 255, 255), 512, 35, // Centered at 512 for 1024 width
               ALLEGRO_ALIGN_CENTER, "CHOOSE YOUR PRISONERS");

  if (select_phase == 0) {
    al_draw_text(local_font, al_map_rgb(200, 50, 50), 512, 65,
                 ALLEGRO_ALIGN_CENTER, "SELECT PRIMARY HERO (PLAYER 1)");
  } else {
    al_draw_text(local_font, al_map_rgb(50, 150, 250), 512, 65,
                 ALLEGRO_ALIGN_CENTER, "SELECT COMPANION (PLAYER 2)");
  }

  // FORCE CLAMP to 6 to prevent duplicate SQL rows from drawing off-screen
  int max_c = std::min(6, static_cast<int>(prisoner_db_pool.size()));
  if (max_c == 0) {
    al_draw_text(local_font, al_map_rgb(255, 0, 0), 512, 300,
                 ALLEGRO_ALIGN_CENTER, "CRITICAL ERROR: Prisoner database pool is empty!");
    return;
  }

  // Perfect horizontal layout dimensions for a centered 6-card row grid at 1024x768
  int card_width = 125; // Widened slightly from 115 to look excellent at 1024
  int card_gap = 20;    // Increased gap to breathe better
  int total_width = (max_c * card_width) + ((max_c - 1) * card_gap); // (6 * 125) + (5 * 20) = 850
  int start_x = (1024 - total_width) / 2; // (1024 - 850) / 2 = 87px starting margin
  int card_y = 120;

  for (int i = 0; i < max_c; ++i) {
    const Prisoner& p = prisoner_db_pool.at(i);
    int current_x = start_x + (i * (card_width + card_gap));

    bool is_hovered = (character_cursor_index == i);
    bool is_already_picked = (select_phase == 1 && hero.name == p.name);


    ALLEGRO_COLOR border_color = al_map_rgb(55, 55, 55);
    ALLEGRO_COLOR text_color = al_map_rgb(130, 130, 130);

    if (is_already_picked) {
      border_color = al_map_rgb(200, 50, 50);
      text_color = al_map_rgb(90, 40, 40);
    } else if (is_hovered) {
      border_color = al_map_rgb(255, 215, 0); // Golden border for active selection
      text_color = al_map_rgb(255, 255, 255);
    }

    // Draw the 125x380 card boundary container box
    al_draw_rectangle(current_x, card_y, current_x + card_width, card_y + 380,
                      border_color, is_hovered ? 2 : 1);

    // Profile photo placeholder
    al_draw_filled_rectangle(current_x + 10, card_y + 15, current_x + card_width - 10, card_y + 95, al_map_rgb(25, 25, 25));
    al_draw_rectangle(current_x + 10, card_y + 15, current_x + card_width - 10, card_y + 95, border_color, 1);
    al_draw_text(local_font, text_color, current_x + (card_width / 2), card_y + 50, ALLEGRO_ALIGN_CENTER, "FACE");

    // Metadata textual names
    al_draw_text(local_font, text_color, current_x + (card_width / 2), card_y + 115, ALLEGRO_ALIGN_CENTER, p.name.c_str());

    std::string hp_str = "HP: " + std::to_string(p.max_hp);
    al_draw_text(local_font, text_color, current_x + (card_width / 2), card_y + 140, ALLEGRO_ALIGN_CENTER, hp_str.c_str());

    // Clean 2-column x 4-row grid fallback boxes for the dice faces
    al_draw_text(local_font, text_color, current_x + (card_width / 2), card_y + 165, ALLEGRO_ALIGN_CENTER, "DICE:");

    int grid_start_x = current_x + 36;
    int grid_start_y = card_y + 190;

    for (size_t f = 0; f < p.dice_faces.size(); ++f) {
      int col = f % 2;
      int row = f / 2;
      int icon_x = grid_start_x + (col * 28);
      int icon_y = grid_start_y + (row * 26);

      al_draw_rectangle(icon_x, icon_y, icon_x + 24, icon_y + 24, text_color, 1);

      char letter = '?';
      if (p.dice_faces.at(f) == DieFace::MIGHT) letter = 'M';
      else if (p.dice_faces.at(f) == DieFace::CUNNING) letter = 'C';
      else if (p.dice_faces.at(f) == DieFace::WISDOM) letter = 'W';
      else if (p.dice_faces.at(f) == DieFace::SHIELD) letter = 'S';

      al_draw_textf(local_font, text_color, icon_x + 12, icon_y + 4, ALLEGRO_ALIGN_CENTER, "%c", letter);
    }

    // --- ENFORCE DIRECT CURSOR STATE VALUE COMPARISONS HERE ---
    if (select_phase == 1 && p1_char_cursor == i) {
      al_draw_filled_rectangle(current_x + 1, card_y + 1, current_x + card_width - 1, card_y + 379, al_map_rgba(5, 5, 5, 210));
      al_draw_text(local_font, al_map_rgb(200, 50, 50), current_x + (card_width / 2), card_y + 180, ALLEGRO_ALIGN_CENTER, "[ PICKED ]");
    }
  }

  // Bottom footer strings centered at 512
  al_draw_text(local_font, al_map_rgb(90, 90, 90), 512, 535, ALLEGRO_ALIGN_CENTER, "Use LEFT / RIGHT arrow keys to navigate rows.");
  al_draw_text(local_font, al_map_rgb(90, 90, 90), 512, 560, ALLEGRO_ALIGN_CENTER, "Press ENTER or SPACE to confirm profile.");
}


void DarkCastleEngine::render_active_tabletop_loop() {
  // ==============================================================================
  // 1. TOP BAR: DECK COUNTER & DECK BACK IMAGE
  // ==============================================================================
  int rooms_left = static_cast<int>(current_game_deck.size());
  std::string deck_count_str = "ROOMS UNTIL BOSS: " + std::to_string(rooms_left);

  int deck_x1 = 924;
  int deck_x2 = 984;

  if (deck_back_texture) {
    al_draw_bitmap(deck_back_texture, deck_x1, 20, 0);
  } else {
    al_draw_filled_rectangle(deck_x1, 20, deck_x2, 90, al_map_rgb(40, 40, 40));
    al_draw_rectangle(deck_x1, 20, deck_x2, 90, al_map_rgb(100, 100, 100), 2);
    al_draw_text(local_font, al_map_rgb(150, 150, 150), deck_x1 + 30, 45,
                 ALLEGRO_ALIGN_CENTER, "DECK");
  }
  al_draw_text(local_font, al_map_rgb(200, 200, 200), deck_x1 - 15, 45,
               ALLEGRO_ALIGN_RIGHT, deck_count_str.c_str());

  // ==============================================================================
  // 2. CENTER PIECE: CARD CONTAINER & MYSTERY OVERLAYS
  // ==============================================================================
  int center_x = 512;
  int center_y = 340;

  if (room_placeholder_image && !is_awaiting_door_open) {
    al_draw_bitmap(room_placeholder_image, center_x - 120, center_y - 140, 0);
  } else {
    al_draw_filled_rectangle(center_x - 120, center_y - 140, center_x + 120,
                             center_y + 20, al_map_rgb(20, 20, 20));
    al_draw_rectangle(center_x - 120, center_y - 140, center_x + 120,
                      center_y + 20, al_map_rgb(60, 60, 60), 1);
    
    if (is_awaiting_door_open) {
      al_draw_text(local_font, al_map_rgb(100, 100, 100), center_x, center_y - 70,
                   ALLEGRO_ALIGN_CENTER, "[ UNEXPLORED ROOM ]");
    } else {
      al_draw_text(local_font, al_map_rgb(80, 80, 80), center_x, center_y - 70,
                   ALLEGRO_ALIGN_CENTER, "[ CASTLE ARTWORK ]");
    }
  }

  // Phase separation layout metrics
  int line_count = 1;

  if (is_awaiting_door_open) {
    al_draw_text(local_font, al_map_rgb(140, 140, 140), center_x, center_y + 40,
                 ALLEGRO_ALIGN_CENTER, "??? An Unknown Threat Awaits ???");
    
    al_draw_multiline_text(local_font, al_map_rgb(90, 90, 90), center_x, center_y + 75,
                           500, 22, ALLEGRO_ALIGN_CENTER, 
                           "The corridor turns sharply into the pitch black. Someone must step forward, turn the handle, and face whatever nightmare lies beyond...");
    
    line_count = 3;
  } 
  else {
    al_draw_text(local_font, al_map_rgb(255, 255, 255), center_x, center_y + 40,
                 ALLEGRO_ALIGN_CENTER, active_card.title.c_str());

    al_do_multiline_text(local_font, 500, active_card.flavor_text.c_str(),
      [](int line_num, const char *line, int size, void *extra) -> bool {
        *reinterpret_cast<int*>(extra) = line_num + 1;
        return true;
      }, &line_count);

    if (line_count <= 0) line_count = 1;

    al_draw_multiline_text(local_font, al_map_rgb(160, 160, 160), center_x, center_y + 65,
                           500, 22, ALLEGRO_ALIGN_CENTER, active_card.flavor_text.c_str());
  }

  // This baseline variable now manages your vertical stack offsets beautifully
  int challenge_y_baseline = center_y + 65 + (line_count * 22);

  // ==============================================================================
  // 3. INTERACTION SAFETY GATE (Hides threat properties until card is turned)
  // ==============================================================================
  if (!is_awaiting_door_open && !game_over) 
  {
    if (active_card.type == CardType::SKILL_CHALLENGE) {
      std::string trait_name = "Blank";
      if (active_card.target_attribute == DieFace::MIGHT)   trait_name = "Might";
      if (active_card.target_attribute == DieFace::CUNNING) trait_name = "Cunning";
      if (active_card.target_attribute == DieFace::WISDOM)  trait_name = "Wisdom";

      std::string chal_str = "CHALLENGE: " + trait_name + " NEEDED: " +
                             std::to_string(challenge_successes_gained) + "/" +
                             std::to_string(active_card.required_successes);

      al_draw_text(local_font, al_map_rgb(200, 180, 50), center_x, challenge_y_baseline,
                   ALLEGRO_ALIGN_CENTER, chal_str.c_str());

      std::string att_str = "Attempts Remaining: " + std::to_string(challenge_attempts_left);
      al_draw_text(local_font, al_map_rgb(140, 140, 140), center_x,
                   challenge_y_baseline + 22, ALLEGRO_ALIGN_CENTER, att_str.c_str());

    } 
    else if (active_card.type == CardType::STANDARD_COMBAT ||
             active_card.type == CardType::BOSS_BATTLE) {

      al_draw_text(local_font, al_map_rgb(200, 50, 50), center_x, challenge_y_baseline,
                   ALLEGRO_ALIGN_CENTER, "LIVE ENEMY SHIELDS TO SMASH:");

      int shield_count = static_cast<int>(live_enemy_shields.size());
      int shield_box_w = 70;
      int shield_gap = 10;
      int shield_start_x = center_x - ((shield_count * shield_box_w) + ((shield_count - 1) * shield_gap)) / 2;

      for (int s = 0; s < shield_count; ++s) {
        int sx = shield_start_x + (s * (shield_box_w + shield_gap));

        bool is_targeted_by_greatsword = (is_awaiting_greatsword_choice && greatsword_shield_cursor == s);
        ALLEGRO_COLOR bg_color = is_targeted_by_greatsword ? al_map_rgb(120, 90, 10) : al_map_rgb(50, 20, 20);
        ALLEGRO_COLOR border_color = is_targeted_by_greatsword ? al_map_rgb(255, 215, 0) : al_map_rgb(180, 60, 60);
        int thickness = is_targeted_by_greatsword ? 2 : 1;

        int shield_rect_y = challenge_y_baseline + 20;

        al_draw_filled_rectangle(sx, shield_rect_y, sx + shield_box_w, shield_rect_y + 25, bg_color);
        al_draw_rectangle(sx, shield_rect_y, sx + shield_box_w, shield_rect_y + 25, border_color, thickness);
        al_draw_text(local_font, al_map_rgb(240, 240, 240), sx + (shield_box_w / 2), shield_rect_y + 5,
                     ALLEGRO_ALIGN_CENTER, live_enemy_shields.at(s).c_str());

        if (is_targeted_by_greatsword) {
          std::string prompt_count = "STRIKES LEFT: " + std::to_string(greatsword_removals_left);
          al_draw_text(local_font, al_map_rgb(255, 215, 0), center_x, shield_rect_y + 35, ALLEGRO_ALIGN_CENTER, prompt_count.c_str());
        }
      }
    }
    else if (active_card.type == CardType::BRANCHING_CHOICE && is_awaiting_prompt_choice) {
      int card_y_top = challenge_y_baseline + 10;
      int box_w = 230;
      int box_h = 135;
      int gap = 20;

      int x1_y = center_x - box_w - (gap / 2);
      int x2_y = center_x - (gap / 2);
      int x1_n = center_x + (gap / 2);
      int x2_n = center_x + box_w + (gap / 2);

      al_draw_filled_rectangle(x1_y, card_y_top, x2_y, card_y_top + box_h, al_map_rgb(20, 25, 35));
      al_draw_rectangle(x1_y, card_y_top, x2_y, card_y_top + box_h, al_map_rgb(50, 150, 250), 1);

      std::string label_y = "[Y] " + active_card.choice_1_text;
      std::string risk_y = "Risk: " + std::to_string(active_card.choice_1_damage) + " Damage";
      al_draw_text(local_font, al_map_rgb(255, 255, 255), x1_y + 15, card_y_top + 15, ALLEGRO_ALIGN_LEFT, label_y.c_str());
      al_draw_text(local_font, al_map_rgb(200, 200, 200), x1_y + 15, card_y_top + 45, ALLEGRO_ALIGN_LEFT, risk_y.c_str());
      al_draw_text(local_font, al_map_rgb(130, 130, 130), x1_y + 15, card_y_top + 75, ALLEGRO_ALIGN_LEFT, "Shields to Smash:");

      int shield_idx = 0;
      for (const auto& face : active_card.choice_1_shields) {
        int sx = x1_y + 15 + (shield_idx * 40);
        int sy = card_y_top + 95;
        al_draw_filled_rectangle(sx, sy, sx + 32, sy + 25, al_map_rgb(50, 20, 20));
        al_draw_rectangle(sx, sy, sx + 32, sy + 25, al_map_rgb(180, 60, 60), 1);

        std::string f_char = "M";
        if (face == DieFace::CUNNING) f_char = "C";
        if (face == DieFace::WISDOM)  f_char = "W";
        if (face == DieFace::SHIELD)  f_char = "S";
        al_draw_text(local_font, al_map_rgb(240, 240, 240), sx + 16, sy + 5, ALLEGRO_ALIGN_CENTER, f_char.c_str());
        shield_idx++;
      }

      al_draw_filled_rectangle(x1_n, card_y_top, x2_n, card_y_top + box_h, al_map_rgb(20, 25, 35));
      al_draw_rectangle(x1_n, card_y_top, x2_n, card_y_top + box_h, al_map_rgb(50, 150, 250), 1);

      std::string label_n = "[N] " + active_card.choice_2_text;
      std::string risk_n = "Risk: " + std::to_string(active_card.choice_2_damage) + " Damage";
      al_draw_text(local_font, al_map_rgb(255, 255, 255), x1_n + 15, card_y_top + 15, ALLEGRO_ALIGN_LEFT, label_n.c_str());
      al_draw_text(local_font, al_map_rgb(200, 200, 200), x1_n + 15, card_y_top + 45, ALLEGRO_ALIGN_LEFT, risk_n.c_str());
      al_draw_text(local_font, al_map_rgb(130, 130, 130), x1_n + 15, card_y_top + 75, ALLEGRO_ALIGN_LEFT, "Shields to Smash:");

      shield_idx = 0;
      for (const auto& face : active_card.choice_2_shields) {
        int sx = x1_n + 15 + (shield_idx * 40);
        int sy = card_y_top + 95;
        al_draw_filled_rectangle(sx, sy, sx + 32, sy + 25, al_map_rgb(50, 20, 20));
        al_draw_rectangle(sx, sy, sx + 32, sy + 25, al_map_rgb(180, 60, 60), 1);

        std::string f_char = "M";
        if (face == DieFace::CUNNING) f_char = "C";
        if (face == DieFace::WISDOM)  f_char = "W";
        if (face == DieFace::SHIELD)  f_char = "S";
        al_draw_text(local_font, al_map_rgb(240, 240, 240), sx + 16, sy + 5, ALLEGRO_ALIGN_CENTER, f_char.c_str());
        shield_idx++;
      }
    }
  } // Closes the safety gate
  // Animated Dice indicator text boxes

  // ==============================================================================
  // CORE PHASE PROMPT BANNER DRIFTED ABOVE SAFETY INTERCEPTS
  // ==============================================================================
  // Set to 572 to position it right in the empty black layout bar above HUD panels safely
  int base_prompt_y = 572; 

  if (is_awaiting_door_open) {
    al_draw_text(local_font, al_map_rgb(255, 215, 0), center_x, base_prompt_y,
                 ALLEGRO_ALIGN_CENTER, "DOOR LOCKED: Press for [1] Hero or [2] for Companion to open door.");
  } else if (room_is_completed) {
    al_draw_text(local_font, al_map_rgb(50, 255, 50), center_x, base_prompt_y,
                 ALLEGRO_ALIGN_CENTER, "ROOM CLEARED: Press [N] to move on.");
  } else {
    al_draw_text(local_font, al_map_rgb(150, 150, 150), center_x, base_prompt_y,
                 ALLEGRO_ALIGN_CENTER, "ROOM ACTIVE: Press [SPACE] to Roll/Combat | Press [R] to alternate rest actions before rolling.");
  }

  // ==============================================================================
  // 4. BOTTOM BAR: CHARACTER PANELS & INVENTORIES
  // ==============================================================================
  int player_y = 600;
  int layout_xs[] = {40, 644};

  for (int p_idx = 0; p_idx < 2; ++p_idx) {
    int px = layout_xs[p_idx];
    bool is_hero = (p_idx == 0);
    const Prisoner& p = is_hero ? hero : companion;
    const std::vector<ItemCard>& inv = is_hero ? hero_inventory : companion_inventory;

    bool is_resting = (resting_character == (p_idx + 1));
    bool is_selected = !is_resting;

    bool is_active_attacker = (current_acting_player_id == (p_idx + 1));
    bool is_puzzle_challenge = (active_card.type == CardType::SKILL_CHALLENGE);
    bool is_door_opener = (current_door_opener_id == (p_idx + 1));

    ALLEGRO_COLOR frame_color = al_map_rgb(60, 60, 60);      
    ALLEGRO_COLOR text_color  = al_map_rgb(130, 130, 130);   
    int frame_line_thickness  = 1;
    bool draw_fully_opaque    = true;

    if (is_puzzle_challenge) {
      if (is_door_opener) {
        frame_color = al_map_rgb(212, 175, 55);              
        text_color  = al_map_rgb(255, 255, 255);
        frame_line_thickness = 3;
        draw_fully_opaque = true;
      } else {
        frame_color = al_map_rgb(45, 45, 50);                
        text_color  = al_map_rgb(90, 90, 95);
        frame_line_thickness = 1;
        draw_fully_opaque = false;
      }
    } 
    else {
      if (is_resting) {
        frame_color = al_map_rgb(40, 45, 40);                
        text_color  = al_map_rgb(80, 100, 80);
        frame_line_thickness = 1;
        draw_fully_opaque = false;
      } 
      else if (is_active_attacker) {
        frame_color = al_map_rgb(212, 175, 55); 
        text_color  = al_map_rgb(255, 255, 255);
        frame_line_thickness = 3;
        draw_fully_opaque = true;
      } 
      else {
        frame_color = al_map_rgb(140, 140, 140);
        text_color  = al_map_rgb(190, 190, 190);
        frame_line_thickness = 1;
        draw_fully_opaque = true;
      }
    }

    // ==============================================================================
    // EMERGENCY LOW-HP FLASHING INDICATOR HIGHLIGHT MATRIX
    // ==============================================================================
    if (p.current_hp > 0 && p.current_hp < 4) {
      double pulse_wave = (sin(al_get_time() * 7.5) + 1.0) / 2.0;
      int red_component = 110 + static_cast<int>(pulse_wave * 145);

      if (draw_fully_opaque) {
        // --- STANCE A: ACTIVE PLAYER LOW-HP EFFECT ---
        frame_color = al_map_rgb(red_component, 20, 20); 
        frame_line_thickness = 3;                       
        text_color = al_map_rgb(255, 180, 180);          
      } 
      else {
        // --- STANCE B: INACTIVE PLAYER LOW-HP EFFECT (FADED RED) ---
        int faded_red = 40 + static_cast<int>(pulse_wave * 60); 
        
        frame_color = al_map_rgb(faded_red, 15, 15);
        frame_line_thickness = 1; 
        text_color = al_map_rgb(180, 120, 120); 
      }
    }

    // Draw outer boundary container rectangle frame utilizing our calculated thickness rules
    al_draw_rectangle(px, player_y, px + 340, player_y + 130, frame_color, frame_line_thickness);

    int img_id = 0;
    if (p.name == "The Abbot") img_id = 0;
    else if (p.name == "The Tailor") img_id = 1;
    else if (p.name == "The Smith") img_id = 2;
    else if (p.name == "The Cook") img_id = 3;
    else if (p.name == "The Tanner") img_id = 4;
    else if (p.name == "The Miller") img_id = 5;

    if (prisoner_textures[img_id]) {
      bool is_low_hp = (p.current_hp > 0 && p.current_hp < 4);
      if (draw_fully_opaque) {
        al_draw_bitmap(prisoner_textures[img_id], px + 10, player_y + 15, 0);
      } else if (is_resting) {
        al_draw_tinted_bitmap(prisoner_textures[img_id], al_map_rgba(40, 80, 40, 90), px + 10, player_y + 15, 0);
      } else if (is_low_hp) {
        // TINT THE INACTIVE DANGER PROFILE AVATAR WITH FADED TRANSPARENT CRIMSON ALPHAS ---
        al_draw_tinted_bitmap(prisoner_textures[img_id], al_map_rgba(140, 30, 30, 110), px + 10, player_y + 15, 0);
      } else {
        al_draw_tinted_bitmap(prisoner_textures[img_id], al_map_rgba(255, 255, 255, 110), px + 10, player_y + 15, 0);
      }
    } else {
      ALLEGRO_COLOR bg_tint = draw_fully_opaque ? al_map_rgb(25, 25, 25) : al_map_rgb(18, 18, 18);
      al_draw_filled_rectangle(px + 10, player_y + 15, px + 75, player_y + 115, bg_tint);
      al_draw_rectangle(px + 10, player_y + 15, px + 75, player_y + 115, frame_color, 1);
      al_draw_text(local_font, text_color, px + 42, player_y + 55, ALLEGRO_ALIGN_CENTER, "FACE");
    }

    std::string label = p.name + (is_resting ? " [REST]" : " [ACT]");
    al_draw_text(local_font, is_resting ? al_map_rgb(50, 180, 50) : text_color, px + 85, player_y + 20, ALLEGRO_ALIGN_LEFT, label.c_str());

    // HUD CHARACTER CARD DICE OVERLAY AND PULSING SPINNING CONTAINERS
    std::string character_roll = is_hero ? hero_last_roll_str : companion_last_roll_str;
    bool this_player_is_spinning = (is_dice_animating && current_acting_player_id == (p_idx + 1));

    if (!is_awaiting_door_open && !game_over && (this_player_is_spinning || !character_roll.empty())) {
      int die_box_x = px + 285;
      int die_box_y = player_y + 12;
      int die_size = 40;

      // Assign matching layout properties using your active status monitors
      ALLEGRO_COLOR die_bg = is_active_attacker ? al_map_rgb(22, 22, 26) : al_map_rgb(15, 15, 15);
      ALLEGRO_COLOR die_ln = this_player_is_spinning ? al_map_rgb(255, 140, 0) : (is_active_attacker ? al_map_rgb(212, 175, 55) : al_map_rgb(70, 70, 75));
      int die_thickness = (is_active_attacker || this_player_is_spinning) ? 2 : 1;

      // Draw the structured bounding outline box
      al_draw_filled_rectangle(die_box_x, die_box_y, die_box_x + die_size, die_box_y + die_size, die_bg);
      al_draw_rectangle(die_box_x, die_box_y, die_box_x + die_size, die_box_y + die_size, die_ln, die_thickness);

      if (this_player_is_spinning) {
        // SPINNING STAGE: Draw your global random flickering chars right inside the active slot
        char flick_char = is_hero ? p1_rolling_flicker_char : p2_rolling_flicker_char;
        char print_buf[] = {flick_char, '\0'};
        al_draw_text(local_font, al_map_rgb(255, 140, 0), die_box_x + (die_size / 2), die_box_y + (die_size / 2) - 6,
                     ALLEGRO_ALIGN_CENTER, print_buf);
      } 
      else {
        // SETTLED STAGE: Draw the permanent shorthand uppercase token letter result representation
        char shorthand_letter = 'M';
        ALLEGRO_COLOR shorthand_color = al_map_rgb(200, 200, 200);

        if (character_roll == "Cunning") { shorthand_letter = 'C'; shorthand_color = al_map_rgb(130, 180, 240); }
        if (character_roll == "Wisdom")  { shorthand_letter = 'W'; shorthand_color = al_map_rgb(190, 140, 240); }
        if (character_roll == "Shield")  { shorthand_letter = 'S'; shorthand_color = al_map_rgb(150, 240, 150); }

        char print_buf[] = {shorthand_letter, '\0'};
        al_draw_text(local_font, shorthand_color, die_box_x + (die_size / 2), die_box_y + (die_size / 2) - 6,
                     ALLEGRO_ALIGN_CENTER, print_buf);
      }
    }


    std::string hp_metrics = "HP: " + std::to_string(p.current_hp) + " / " + std::to_string(p.max_hp);
    al_draw_text(local_font, text_color, px + 85, player_y + 45, ALLEGRO_ALIGN_LEFT, hp_metrics.c_str());

    al_draw_text(local_font, text_color, px + 85, player_y + 70, ALLEGRO_ALIGN_LEFT, "ITEMS:");

    for (int i = 0; i < 2; ++i) {
      int icon_x = px + 185 + (i * 68);
      int icon_y = player_y + 92;

      int active_cursor = is_hero ? hero_inv_cursor : companion_inv_cursor;
      bool is_slot_hovered = (active_cursor == i && is_selected);

      ALLEGRO_COLOR item_border_color = is_slot_hovered ? al_map_rgb(255, 215, 0) : al_map_rgb(45, 45, 45);
      int border_thickness = is_slot_hovered ? 2 : 1;

      if (i < static_cast<int>(inv.size())) {
        const ItemCard& item = inv.at(i);
        auto it = item_icon_registry.find(item.name);
        bool has_custom_icon = (it != item_icon_registry.end() && it->second != nullptr);

        if (has_custom_icon) {
          al_draw_bitmap(it->second, icon_x, icon_y, 0);
          if (is_slot_hovered) {
            al_draw_rectangle(icon_x, icon_y, icon_x + 60, icon_y + 32, item_border_color, border_thickness);
          }
        } else {
          al_draw_filled_rectangle(icon_x, icon_y, icon_x + 60, icon_y + 32, al_map_rgb(35, 35, 35));
          al_draw_rectangle(icon_x, icon_y, icon_x + 60, icon_y + 32, item_border_color, border_thickness);

          std::string short_name = item.name.substr(0, std::min<size_t>(4, item.name.length()));
          al_draw_text(local_font, al_map_rgb(200, 200, 200), icon_x + 30, icon_y + 8, ALLEGRO_ALIGN_CENTER, short_name.c_str());
        }
      } else {
        al_draw_rectangle(icon_x, icon_y, icon_x + 60, icon_y + 32, item_border_color, border_thickness);
        al_draw_text(local_font, al_map_rgb(55, 55, 55), icon_x + 30, icon_y + 8, ALLEGRO_ALIGN_CENTER, "X");
      }
    }
  }

  // ==============================================================================
  // 5. CENTER LOWER CONTENT: SCROLLING COMBAT LOG TEXT TICKER
  // ==============================================================================
  if (active_card.type == CardType::BRANCHING_CHOICE && is_awaiting_prompt_choice) {
    return; 
  }

  int log_box_x1 = 412;
  int log_box_x2 = 612;
  int log_box_y1 = player_y;

  al_draw_filled_rectangle(log_box_x1, log_box_y1, log_box_x2, log_box_y1 + 130, al_map_rgb(15, 15, 15));
  al_draw_rectangle(log_box_x1, log_box_y1, log_box_x2, log_box_y1 + 130, al_map_rgb(45, 45, 45), 1);

  al_set_clipping_rectangle(log_box_x1 + 4, log_box_y1 + 4, 192, 122);

  int log_text_y_start = log_box_y1 + 12;
  int current_line_y = log_text_y_start;

  for (int l_idx = static_cast<int>(game_log_history.size()) - 1; l_idx >= 0; --l_idx) {
    ALLEGRO_COLOR log_color = al_map_rgb(110, 110, 110);
    if (l_idx == static_cast<int>(game_log_history.size()) - 1) {
      log_color = al_map_rgb(245, 245, 240);
    } else if (l_idx == static_cast<int>(game_log_history.size()) - 2) {
      log_color = al_map_rgb(175, 175, 175);
    }

    int statement_sub_lines = 0;
    al_do_multiline_text(local_font, 180, game_log_history.at(l_idx).c_str(),
      [](int line_num, const char *line, int size, void *extra) -> bool {
        *reinterpret_cast<int*>(extra) = line_num + 1;
        return true;
      }, &statement_sub_lines);
    if (statement_sub_lines <= 0) statement_sub_lines = 1;

    if (current_line_y + (statement_sub_lines * 14) > log_box_y1 + 120) {
      break; 
    }

    al_draw_multiline_text(local_font, log_color, 512, current_line_y, 180, 14, ALLEGRO_ALIGN_CENTER, game_log_history.at(l_idx).c_str());
    current_line_y += (statement_sub_lines * 14) + 6; 
  }

  if (game_log_history.empty()) {
    al_draw_text(local_font, al_map_rgb(60, 60, 60), 512, log_box_y1 + 55, ALLEGRO_ALIGN_CENTER, "LOG SILENT");
  }

  al_set_clipping_rectangle(0, 0, 1024, 768);
}

// ==============================================================================
// POPUP PROMPT OVERLAYS (FIXED HORIZONTAL & VERTICAL CENTERING)
// ==============================================================================
void DarkCastleEngine::draw_ui_overlay_prompts() {
  if (!is_awaiting_prompt_choice) return;
  if (active_card.type == CardType::BRANCHING_CHOICE) return;

  // FIXED MIDPOINTS: Center anchors mapped directly to 1024x768 screen coordinate system
  int center_x = 512, center_y = 384, box_w = 460, box_h = 180;
  int x1 = center_x - (box_w / 2), y1 = center_y - (box_h / 2);
  int x2 = center_x + (box_w / 2), y2 = center_y + (box_h / 2);

  // Dim the entire 1024x768 window interface canvas screen area
  al_draw_filled_rectangle(0, 0, 1024, 768, al_map_rgba(10, 10, 10, 180));
  al_draw_filled_rectangle(x1, y1, x2, y2, al_map_rgb(20, 20, 20));
  al_draw_rectangle(x1, y1, x2, y2, al_map_rgb(218, 165, 32), 2);

  std::string target_name =
      (prompt_target_character == 1) ? hero.name : companion.name;
  const std::vector<ItemCard>& inv =
      (prompt_target_character == 1) ? hero_inventory : companion_inventory;

  std::string shield_name = "Shield Component";
  if (prompt_shield_vector_index < inv.size()) {
    shield_name = inv.at(prompt_shield_vector_index).name;
  }

  std::string title_line = "TACTICAL DEFENSE: OPTIONAL BLOCK";
  std::string query_line = target_name + " is taking " +
                           std::to_string(pending_monster_damage) +
                           " incoming damage!";
  std::string action_line =
      "Sacrifice " + shield_name + " to intercept this hit completely?";

  al_draw_text(local_font, al_map_rgb(218, 165, 32), center_x, y1 + 25,
               ALLEGRO_ALIGN_CENTER, title_line.c_str());
  al_draw_text(local_font, al_map_rgb(230, 230, 230), center_x, y1 + 60,
               ALLEGRO_ALIGN_CENTER, query_line.c_str());
  al_draw_text(local_font, al_map_rgb(160, 160, 160), center_x, y1 + 85,
               ALLEGRO_ALIGN_CENTER, action_line.c_str());

  al_draw_text(local_font, al_map_rgb(50, 200, 50), center_x - 100, y1 + 135,
               ALLEGRO_ALIGN_CENTER, "[Y] - ACTIVATE SHIELD");
  al_draw_text(local_font, al_map_rgb(220, 50, 50), center_x + 100, y1 + 135,
               ALLEGRO_ALIGN_CENTER, "[N] - TAKE HIT DAMAGE");

  // ==============================================================================
  // WEAPON REROLL CHOICE OVERLAY
  // ==============================================================================
  if (is_awaiting_reroll_choice) {
    // FIXED RESOLUTION RESET: Renders weapon reroll popups inside true 1024x768 center lines
    al_draw_filled_rectangle(0, 0, 1024, 768, al_map_rgba(10, 10, 10, 180));
    al_draw_filled_rectangle(x1, y1, x2, y2, al_map_rgb(20, 20, 20));
    al_draw_rectangle(x1, y1, x2, y2, al_map_rgb(180, 60, 60), 2);

    std::string p_name =
        (current_acting_player_id == 1) ? hero.name : companion.name;

    al_draw_text(local_font, al_map_rgb(200, 50, 50), center_x, y1 + 25,
                 ALLEGRO_ALIGN_CENTER, "STRATEGIC ADAPTATION: WEAPON TRAIT");
    std::string prompt_query = p_name + " has " +
                               std::to_string(rerolls_remaining_this_turn) +
                               " re-roll options remaining.";
    al_draw_text(local_font, al_map_rgb(230, 230, 230), center_x, y1 + 60,
                 ALLEGRO_ALIGN_CENTER, prompt_query.c_str());
    al_draw_text(local_font, al_map_rgb(150, 150, 150), center_x, y1 + 85,
                 ALLEGRO_ALIGN_CENTER,
                 "Spend a weapon charge to roll your character die again?");

    al_draw_text(local_font, al_map_rgb(50, 200, 50), center_x - 100, y1 + 135,
                 ALLEGRO_ALIGN_CENTER, "[Y] - RE-ROLL DIE");
    al_draw_text(local_font, al_map_rgb(220, 50, 50), center_x + 100, y1 + 135,
                 ALLEGRO_ALIGN_CENTER, "[N] - ACCEPT RESULT");
  }
}


void DarkCastleEngine::render_game_victory_screen() {
  al_draw_filled_rectangle(0, 0, 800, 600, al_map_rgba(0, 15, 0, 240));
  al_draw_rectangle(50, 50, 750, 550, al_map_rgb(218, 165, 32), 3);
  al_draw_text(local_font, al_map_rgb(218, 165, 32), 400, 160,
               ALLEGRO_ALIGN_CENTER, "ESCAPE THE DARK CASTLE COMPLETE!");
  al_draw_text(local_font, al_map_rgb(180, 230, 180), 400, 220,
               ALLEGRO_ALIGN_CENTER,
               "The heavy iron gates smash open. Blinding sunlight greets your "
               "eyes at last!");
  al_draw_text(local_font, al_map_rgb(255, 255, 255), 400, 480,
               ALLEGRO_ALIGN_CENTER,
               "Press ESCAPE to return to the surface safely.");
}

void DarkCastleEngine::draw_scene_frame() {
  // Clear the backplate buffer to standard deep black before any scene content generates
  al_clear_to_color(al_map_rgb(10, 10, 10));

  if (is_dice_animating && !game_over) {
    dice_anim_frame_counter++;

    // Character literal array avoids string allocation overhead on every single tick pass
    char options[] = {'M', 'C', 'W', 'S'};
    p1_rolling_flicker_char = options[rand() % 4];
    p2_rolling_flicker_char = options[rand() % 4];

    if (dice_anim_frame_counter >= 30) {
      is_dice_animating = false;
      dice_anim_frame_counter = 0;
      // EXECUTE THE COMBAT STEP CALCULATIONS NATIVELY ON ANIMATION SETTLE ---
      execute_combat_round();
      // Fire contextual audio hooks matching the newly settled active face card
      play_context_dice_sfx(active_rolled_face);
    }
  }

  switch (current_scene) {
    case SCENE_MAIN_MENU:
      render_main_title_screen();
      break;
    case SCENE_CHARACTER_SELECT:
      render_character_selection_menu();
      break;
    case SCENE_GAMEPLAY:
      render_active_tabletop_loop();
      draw_ui_overlay_prompts();

      // Layer the golden loot overlay modal on top of your board game views
      if (is_awaiting_loot_choice) {
        render_loot_distribution_overlay();
      }

      // ==============================================================================
      // LAYER THE END-GAME OVERLAYS ON TOP OF THE ACTIVE GAMEPLAY ELEMENTS
      // ==============================================================================
      if (game_over) {
        if (player_won) {
          render_victory_splash_overlay();
        } else {
          render_loss_splash_overlay();
        }
      }
      break;
  }

  // Swap graphics buffers to commit all active render passes to your monitor display window
  al_flip_display();
}

void DarkCastleEngine::load_all_item_textures() {
  // Array containing all 21 completely unique item types found in the base game
  std::vector<std::string> unique_item_names = {"Club",
                                                "Dagger",
                                                "Mace",
                                                "Sword",
                                                "Axe",
                                                "Halberd",
                                                "Greatsword",
                                                "Sling",
                                                "Crossbow",
                                                "Buckler",
                                                "Shield",
                                                "Heavy Shield",
                                                "Helmet",
                                                "Plate Armor",
                                                "Torch",
                                                "Rope",
                                                "Partially Rotten Apple",
                                                "Stale Loaf of Bread",
                                                "Moldy Cheese",
                                                "Cooked Meat",
                                                "Brew of Might",
                                                "Brew of Cunning",
                                                "Brew of Wisdom"};

  for (const auto& name : unique_item_names) {
    // Convert item string names into lowercase snake_case file signatures
    // safely
    std::string filename = name;
    std::transform(
        filename.begin(), filename.end(), filename.begin(),
        [](unsigned char c) { return (c == ' ') ? '_' : ::tolower(c); });

    std::string full_path = "data/gfx/items/" + filename + ".png";

    // Attempt to load the structural image file asset via Allegro's pipeline
    ALLEGRO_BITMAP* bitmap = al_load_bitmap(full_path.c_str());
    if (bitmap) {
      item_icon_registry[name] = bitmap;
    } else {
      // Map missing indicators explicitly to prevent pointer crashes when
      // queried later
      std::cerr << "Warning: Missing item visual graphic asset path: "
                << full_path << std::endl;
      item_icon_registry[name] = nullptr;
    }
  }
}

void DarkCastleEngine::play_door_open_sfx() {
  // If no audio vector was initialized or index 3 doesn't exist, drop out safely
  if (bone_dice_samples.size() <= 3) return;

  // Extract the heavy door opening audio clip from vector slot 3
  ALLEGRO_SAMPLE* selected_sample = bone_dice_samples.at(3);
  if (selected_sample) {
    al_play_sample(selected_sample, 1.0, 0.0, 1.0, ALLEGRO_PLAYMODE_ONCE, nullptr);
  }
}

void DarkCastleEngine::play_treasure_award_sfx() {
  // If no audio vector was initialized or index 2 doesn't exist, drop out safely
  if (bone_dice_samples.size() <= 2) return;

  // Extract the reward chime from vector slot 2
  ALLEGRO_SAMPLE* selected_sample = bone_dice_samples.at(2);
  if (selected_sample) {
    al_play_sample(selected_sample, 1.0, 0.0, 1.0, ALLEGRO_PLAYMODE_ONCE, nullptr);
  }
}

void DarkCastleEngine::play_context_dice_sfx(DieFace finalized_face) {
  // If no samples were successfully loaded into memory arrays, drop out safely
  if (bone_dice_samples.empty()) return;

  size_t target_sample_idx =
      0;  // Default index 0 is our standard rattle impact

  // Dynamically route audio assets depending on the internal enum values
  if (finalized_face == DieFace::SHIELD) {
    // Switch to the shield bounce/clash sound if index 1 exists
    if (bone_dice_samples.size() > 1) {
      target_sample_idx = 1;
    }
  }

  ALLEGRO_SAMPLE* selected_sample = bone_dice_samples.at(target_sample_idx);
  if (selected_sample) {
    al_play_sample(selected_sample, 1.0, 0.0, 1.0, ALLEGRO_PLAYMODE_ONCE,
                   nullptr);
  }
}

void DarkCastleEngine::distribute_challenge_reward(const ChapterCard& card) {
  if (!card.has_reward || card.reward_type == "NONE") {
    room_is_completed = true;
    return;
  }

  if (card.reward_type == "HEAL") {
    int heal_amt = card.reward_amount;
    hero.current_hp = std::min(hero.current_hp + heal_amt, hero.max_hp);
    companion.current_hp = std::min(companion.current_hp + heal_amt, companion.max_hp);
    add_to_game_log("The party rests in the cleared room. Recovered +" + std::to_string(heal_amt) + " HP.");
    room_is_completed = true; 
  } 
  else if (card.reward_type == "ITEM" && !master_treasure_pool.empty()) {
    is_awaiting_loot_choice = true;
    pending_looted_item = master_treasure_pool.at(rand() % master_treasure_pool.size());
    loot_target_player_cursor = 0;
    
    play_treasure_award_sfx();
    add_to_game_log("Found item: " + pending_looted_item.name + "! Choose who claims it.");
  }
  // --- INTEGRATE DOUBLE ITEM REWARD MODAL ALLOCATIONS ---
  else if (card.reward_type == "DOUBLE_ITEM" && !master_treasure_pool.empty()) {
    is_awaiting_loot_choice = true;
    pending_looted_item = master_treasure_pool.at(rand() % master_treasure_pool.size());
    loot_target_player_cursor = 0;
    
    play_treasure_award_sfx();
    add_to_game_log("JACKPOT! " + card.title + " yields 2 pieces of treasure!");
    add_to_game_log("First Item: " + pending_looted_item.name + ". Allocate now.");
  }
}


void DarkCastleEngine::render_loot_distribution_overlay() {
  // Perfect horizontal layout centering positions for a 1024 width window canvas
  int panel_x1 = 242;
  int panel_y1 = 330;
  int panel_x2 = 782; // 540 pixels wide
  
  // --- FIX: HEIGHT EXTENSION TO ENFORCE COMPLETELY CLOSED CONTAINER BOUNDS ---
  // Increased from its original clipping height down by 30px to clear text!
  int panel_y2 = 680; 

  // Draw the main background box container
  al_draw_filled_rectangle(panel_x1, panel_y1, panel_x2, panel_y2, al_map_rgb(15, 15, 18));
  al_draw_rectangle(panel_x1, panel_y1, panel_x2, panel_y2, al_map_rgb(212, 175, 55), 2); // Gold border

  int center_x = 512;

  // Header Title
  al_draw_text(local_font, al_map_rgb(212, 175, 55), center_x, panel_y1 + 25,
               ALLEGRO_ALIGN_CENTER, "--- TREASURE DISCOVERED ---");

  // Item Title (Preserves your original format: Name [SPECIAL_ACTION_TYPE])
  std::string name_line = pending_looted_item.name;
  if (!pending_looted_item.special_action_type.empty()) {
    name_line += " [" + pending_looted_item.special_action_type + "]";
  }
  al_draw_text(local_font, al_map_rgb(255, 255, 255), center_x, panel_y1 + 65,
               ALLEGRO_ALIGN_CENTER, name_line.c_str());

  // Item Effect Description Details Pass (Uses your correct .description field!)
  al_draw_text(local_font, al_map_rgb(170, 170, 175), center_x, panel_y1 + 105,
               ALLEGRO_ALIGN_CENTER, pending_looted_item.description.c_str());

  // --------------------------------------------------------------------------
  // ALLOCATOR PLAYER BUTTON TARGET CELLS
  // --------------------------------------------------------------------------
  int btn_y = panel_y1 + 165;
  int btn_w = 160;
  int btn_h = 45;

  // Left Button: Hero (Player 1)
  int btn1_x1 = center_x - 190;
  ALLEGRO_COLOR b1_col = (loot_target_player_cursor == 0) ? al_map_rgb(212, 175, 55) : al_map_rgb(45, 45, 50);
  al_draw_rectangle(btn1_x1, btn_y, btn1_x1 + btn_w, btn_y + btn_h, b1_col, (loot_target_player_cursor == 0) ? 2 : 1);
  al_draw_text(local_font, (loot_target_player_cursor == 0) ? al_map_rgb(255, 255, 255) : al_map_rgb(110, 110, 115),
               btn1_x1 + (btn_w / 2), btn_y + 12, ALLEGRO_ALIGN_CENTER, hero.name.c_str());

  // Right Button: Companion (Player 2)
  int btn2_x1 = center_x + 30;
  ALLEGRO_COLOR b2_col = (loot_target_player_cursor == 1) ? al_map_rgb(212, 175, 55) : al_map_rgb(45, 45, 50);
  al_draw_rectangle(btn2_x1, btn_y, btn2_x1 + btn_w, btn_y + btn_h, b2_col, (loot_target_player_cursor == 1) ? 2 : 1);
  al_draw_text(local_font, (loot_target_player_cursor == 1) ? al_map_rgb(255, 255, 255) : al_map_rgb(110, 110, 115),
               btn2_x1 + (btn_w / 2), btn_y + 12, ALLEGRO_ALIGN_CENTER, companion.name.c_str());

  // --------------------------------------------------------------------------
  // FOOTER TEXT COORDINATES DOWNWARD TO MATCH NEW BOX HEIGHT
  // --------------------------------------------------------------------------
  al_draw_text(local_font, al_map_rgb(140, 140, 145), center_x, panel_y2 - 35,
               ALLEGRO_ALIGN_CENTER, "Navigate: [LEFT/RIGHT Arrows] | Confirm: [ENTER/SPACEBAR] | Discard: [X]");
}



void DarkCastleEngine::render_victory_splash_overlay() {
  // Use menu_title_font if loaded, otherwise fall back to local_font safely
  ALLEGRO_FONT* header_font = menu_title_font ? menu_title_font : local_font;
  int center_x = 512, center_y = 384;

  // 1. Draw backdrop dim filter and golden modal boundaries
  al_draw_filled_rectangle(0, 0, 1024, 768, al_map_rgba(5, 15, 5, 220)); // Subtle emerald tint
  
  al_draw_filled_rectangle(center_x - 300, center_y - 120, center_x + 300, center_y + 100, al_map_rgb(20, 30, 20));
  al_draw_rectangle(center_x - 300, center_y - 120, center_x + 300, center_y + 100, al_map_rgb(50, 220, 50), 3); // Thick Green Frame

  // 2. Large 36pt Title Banner Text
  al_draw_text(header_font, al_map_rgb(255, 215, 0), center_x, center_y - 85,
               ALLEGRO_ALIGN_CENTER, "THE CASTLE RUN ESCAPED!");

  // 3. Narrative Description Text
  al_draw_text(local_font, al_map_rgb(230, 245, 230), center_x, center_y - 20,
               ALLEGRO_ALIGN_CENTER, "Against all odds, your prisoners have shattered the dark gates.");
  al_draw_text(local_font, al_map_rgb(180, 200, 180), center_x, center_y + 10,
               ALLEGRO_ALIGN_CENTER, "The light of dawn breaks across the valley. You are free!");

  // 4. Interaction Guidelines Footnote Indicator
  al_draw_text(local_font, al_map_rgb(120, 150, 120), center_x, center_y + 60,
               ALLEGRO_ALIGN_CENTER, "Press [ENTER] or [SPACEBAR] for MAIN MENU");
}

void DarkCastleEngine::render_loss_splash_overlay() {
  // ADJUST VERTICAL COORDINATES FOR PERFECT VISUAL SYMMETRY
  int panel_x1 = 132;
  int panel_y1 = 224; // <-- SHIFTED UP FROM 330 FOR MARGIN ALIGNMENT
  int panel_x2 = 892;
  int panel_y2 = 544; // <-- SHIFTED UP FROM 650 TO CLOSE THE 320px CONTAINER BOUNDS

  // Draw the main background box container overlay
  al_draw_filled_rectangle(panel_x1, panel_y1, panel_x2, panel_y2, al_map_rgb(15, 15, 18));
  al_draw_rectangle(panel_x1, panel_y1, panel_x2, panel_y2, al_map_rgb(200, 50, 50), 2); // Grim red border

  int center_x = 512;

  // Header Title - Dedicated large font asset for headers
  ALLEGRO_FONT* header_font = menu_title_font ? menu_title_font : local_font;
  al_draw_text(header_font, al_map_rgb(200, 50, 50), center_x, panel_y1 + 45,
               ALLEGRO_ALIGN_CENTER, "YOU PERISHED IN THE DARK");

  // Biographical Ticker Lines
  al_draw_text(local_font, al_map_rgb(180, 180, 185), center_x, panel_y1 + 125,
               ALLEGRO_ALIGN_CENTER, "A fatal blow strikes down your party. Your names are forgotten.");

  al_draw_text(local_font, al_map_rgb(140, 140, 145), center_x, panel_y1 + 165,
               ALLEGRO_ALIGN_CENTER, "The castle corridors claim another group of eternal souls...");

  // Bottom Interactive Reset Guidance Line
  al_draw_text(local_font, al_map_rgb(110, 110, 115), center_x, panel_y2 - 45,
               ALLEGRO_ALIGN_CENTER, "Press [ENTER] or [SPACEBAR] to return to the Main Menu");
}
